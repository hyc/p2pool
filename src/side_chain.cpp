/*
 * This file is part of the Monero P2Pool <https://github.com/SChernykh/p2pool>
 * Copyright (c) 2021 SChernykh <https://github.com/SChernykh>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "p2pool.h"
#include "side_chain.h"
#include "pool_block.h"
#include "wallet.h"
#include "block_template.h"
#include "randomx.h"
#include "dataset.hpp"
#include "configuration.h"
#include "intrin_portable.h"
#include "keccak.h"
#include "p2p_server.h"
#include "params.h"
#include "json_parsers.h"
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <fstream>
#include <iterator>
#include <numeric>

// Only uncomment it to debug issues with uncle/orphan blocks
//#define DEBUG_BROADCAST_DELAY_MS 100

#ifdef DEBUG_BROADCAST_DELAY_MS
#include <thread>
#endif

static constexpr char log_category_prefix[] = "SideChain ";

constexpr uint64_t MIN_DIFFICULTY = 1000;
constexpr size_t UNCLE_BLOCK_DEPTH = 3;

static_assert(1 <= UNCLE_BLOCK_DEPTH && UNCLE_BLOCK_DEPTH <= 10, "Invalid UNCLE_BLOCK_DEPTH");

namespace p2pool {

SideChain::SideChain(p2pool* pool)
	: m_pool(pool)
	, m_chainTip(nullptr)
	, m_poolName("default")
	, m_targetBlockTime(1)
	, m_minDifficulty(MIN_DIFFICULTY, 0)
	, m_chainWindowSize(2160)
	, m_unclePenalty(20)
	, m_curDifficulty(m_minDifficulty)
{
	if (!load_config(m_pool->params().m_config)) {
		panic();
	}

	if (!check_config()) {
		panic();
	}

	uv_mutex_init_checked(&m_sidechainLock);

	m_difficultyData.reserve(m_chainWindowSize);

	LOGINFO(1, "generating consensus ID");

	const randomx_flags flags = randomx_get_flags();
	randomx_cache* cache = randomx_alloc_cache(flags | RANDOMX_FLAG_LARGE_PAGES);
	if (!cache) {
		LOGWARN(1, "couldn't allocate RandomX cache using large pages");
		cache = randomx_alloc_cache(flags);
		if (!cache) {
			LOGERR(1, "couldn't allocate RandomX cache, aborting");
			panic();
		}
	}

	{
		char consensus_str[log::Stream::BUF_SIZE + 1];
		log::Stream s(consensus_str);

		s	<< m_poolName			<< '\0'
			<< m_poolPassword		<< '\0'
			<< m_targetBlockTime	<< '\0'
			<< m_minDifficulty		<< '\0'
			<< m_chainWindowSize	<< '\0'
			<< m_unclePenalty		<< '\0';

		randomx_init_cache(cache, consensus_str, s.m_pos);
	}

	// Intentionally not a power of 2
	constexpr size_t scratchpad_size = 1009;

	rx_vec_i128* scratchpad = reinterpret_cast<rx_vec_i128*>(cache->memory);
	rx_vec_i128* scratchpad_end = scratchpad + scratchpad_size;
	rx_vec_i128* scratchpad_ptr = scratchpad;
	rx_vec_i128* cache_ptr = scratchpad_end;

	for (uint64_t i = scratchpad_size, n = RANDOMX_ARGON_MEMORY * 1024 / sizeof(rx_vec_i128); i < n; ++i) {
		*scratchpad_ptr = rx_xor_vec_i128(*scratchpad_ptr, *cache_ptr);
		++cache_ptr;
		++scratchpad_ptr;
		if (scratchpad_ptr == scratchpad_end) {
			scratchpad_ptr = scratchpad;
		}
	}

	hash id;
	keccak(reinterpret_cast<uint8_t*>(scratchpad), static_cast<int>(scratchpad_size * sizeof(rx_vec_i128)), id.h, HASH_SIZE);

	randomx_release_cache(cache);

	m_consensusId.assign(id.h, id.h + HASH_SIZE);

	char buf[HASH_SIZE * 2 + 1];
	log::Stream s(buf);
	s << id << '\0';

	// Hide most consensus ID bytes, we only want it on screen to show that we're on the right sidechain
	memset(buf + 8, '*', HASH_SIZE * 2 - 16);
	LOGINFO(1, "consensus ID = " << log::LightCyan() << buf);
}

SideChain::~SideChain()
{
	uv_mutex_destroy(&m_sidechainLock);
	for (auto& it : m_blocksById) {
		delete it.second;
	}
}

void SideChain::fill_sidechain_data(PoolBlock& block, Wallet* w, const hash& txkeySec, std::vector<MinerShare>& shares)
{
	MutexLock lock(m_sidechainLock);

	block.m_minerWallet = *w;
	block.m_txkeySec = txkeySec;
	block.m_uncles.clear();

	if (!m_chainTip) {
		block.m_parent = {};
		block.m_sidechainHeight = 0;
		block.m_difficulty = m_minDifficulty;
		block.m_cumulativeDifficulty = m_minDifficulty;

		get_shares(&block, shares);
		return;
	}

	block.m_parent = m_chainTip->m_sidechainId;
	block.m_sidechainHeight = m_chainTip->m_sidechainHeight + 1;

	// Collect uncles from 3 previous block heights
	hash parent_blocks[UNCLE_BLOCK_DEPTH];

	PoolBlock* tmp = get_parent(m_chainTip);
	for (size_t i = 0; tmp && (i < UNCLE_BLOCK_DEPTH); ++i) {
		parent_blocks[i] = tmp->m_sidechainId;
		tmp = get_parent(tmp);
	}

	// First get a list of already mined blocks at these heights
	std::vector<hash> mined_blocks;
	mined_blocks.reserve(UNCLE_BLOCK_DEPTH * 2 + 1);

	tmp = m_chainTip;
	for (uint64_t i = 0, n = std::min<uint64_t>(UNCLE_BLOCK_DEPTH, m_chainTip->m_sidechainHeight + 1); tmp && (i < n); ++i) {
		mined_blocks.push_back(tmp->m_sidechainId);
		mined_blocks.insert(mined_blocks.end(), tmp->m_uncles.begin(), tmp->m_uncles.end());
		tmp = get_parent(tmp);
	}

	for (uint64_t i = 0, n = std::min<uint64_t>(UNCLE_BLOCK_DEPTH, m_chainTip->m_sidechainHeight + 1); i < n; ++i) {
		for (PoolBlock* uncle : m_blocksByHeight[m_chainTip->m_sidechainHeight - i]) {
			// Only add verified and valid blocks
			if (!uncle || !uncle->m_verified || uncle->m_invalid) {
				continue;
			}

			// Only add it if it hasn't been mined already
			if (std::find(mined_blocks.begin(), mined_blocks.end(), uncle->m_sidechainId) != mined_blocks.end()) {
				continue;
			}

			// Only add it if it's on the same chain
			bool same_chain = false;
			do {
				tmp = m_chainTip;
				while (tmp->m_sidechainHeight > uncle->m_sidechainHeight) {
					tmp = get_parent(tmp);
					if (!tmp) {
						break;
					}
				}
				if (!tmp || (tmp->m_sidechainHeight < uncle->m_sidechainHeight)) {
					break;
				}
				PoolBlock* tmp2 = uncle;
				for (size_t j = 0; (j < UNCLE_BLOCK_DEPTH) && tmp && tmp2 && (tmp->m_sidechainHeight + UNCLE_BLOCK_DEPTH >= block.m_sidechainHeight); ++j) {
					if (tmp->m_parent == tmp2->m_parent) {
						same_chain = true;
						break;
					}
					tmp = get_parent(tmp);
					tmp2 = get_parent(tmp2);
				}
			} while (0);

			if (same_chain) {
				block.m_uncles.emplace_back(uncle->m_sidechainId);
				LOGINFO(4, "block template at height " << block.m_sidechainHeight <<
					": added " << uncle->m_sidechainId <<
					" (height " << uncle->m_sidechainHeight <<
					") as an uncle block, depth " << block.m_sidechainHeight - uncle->m_sidechainHeight);
			}
			else {
				LOGINFO(4, "block template at height " << block.m_sidechainHeight <<
					": uncle block " << uncle->m_sidechainId <<
					" (height " << uncle->m_sidechainHeight <<
					") is not on the same chain, depth " << block.m_sidechainHeight - uncle->m_sidechainHeight);
			}
		}
	}

	// Sort uncles and remove duplicates
	if (block.m_uncles.size() > 1) {
		std::sort(block.m_uncles.begin(), block.m_uncles.end());
		block.m_uncles.erase(std::unique(block.m_uncles.begin(), block.m_uncles.end()), block.m_uncles.end());
	}

	block.m_difficulty = m_curDifficulty;
	block.m_cumulativeDifficulty = m_chainTip->m_cumulativeDifficulty + block.m_difficulty;

	for (const hash& uncle_id : block.m_uncles) {
		auto it = m_blocksById.find(uncle_id);
		if (it == m_blocksById.end()) {
			LOGERR(1, "block template has an unknown uncle block " << uncle_id << ". Fix the code!");
			continue;
		}
		block.m_cumulativeDifficulty += it->second->m_difficulty;
	}

	get_shares(&block, shares);
}

bool SideChain::get_shares(PoolBlock* tip, std::vector<MinerShare>& shares) const
{
	shares.clear();
	shares.reserve(m_chainWindowSize * 2);

	// Collect shares from each block in the PPLNS window, starting from the "tip"

	uint64_t block_depth = 0;
	PoolBlock* cur = tip;
	do {
		MinerShare cur_share{ cur->m_difficulty.lo, &cur->m_minerWallet };

		for (const hash& uncle_id : cur->m_uncles) {
			auto it = m_blocksById.find(uncle_id);
			if (it == m_blocksById.end()) {
				LOGWARN(4, "get_shares: can't find uncle block at height = " << cur->m_sidechainHeight << ", id = " << uncle_id);
				LOGWARN(4, "get_shares: can't calculate shares for block at height = " << tip->m_sidechainHeight << ", id = " << tip->m_sidechainId << ", mainchain height = " << tip->m_txinGenHeight);
				return false;
			}

			PoolBlock* uncle = it->second;

			// Skip uncles which are already out of PPLNS window
			if (tip->m_sidechainHeight - uncle->m_sidechainHeight >= m_chainWindowSize) {
				continue;
			}

			// Take some % of uncle's weight into this share
			uint64_t product[2];
			product[0] = umul128(uncle->m_difficulty.lo, m_unclePenalty, &product[1]);

			uint64_t rem;
			const uint64_t uncle_penalty = udiv128(product[1], product[0], 100, &rem);

			cur_share.m_weight += uncle_penalty;
			shares.emplace_back(uncle->m_difficulty.lo - uncle_penalty, &uncle->m_minerWallet);
		}

		shares.push_back(cur_share);

		++block_depth;
		if (block_depth >= m_chainWindowSize) {
			break;
		}

		// Reached the genesis block so we're done
		if (cur->m_sidechainHeight == 0) {
			break;
		}

		auto it = m_blocksById.find(cur->m_parent);
		if (it == m_blocksById.end()) {
			LOGWARN(4, "get_shares: can't find parent block at height = " << cur->m_sidechainHeight - 1 << ", id = " << cur->m_parent);
			LOGWARN(4, "get_shares: can't calculate shares for block at height = " << tip->m_sidechainHeight << ", id = " << tip->m_sidechainId << ", mainchain height = " << tip->m_txinGenHeight);
			return false;
		}

		cur = it->second;
	} while (block_depth < m_chainWindowSize);

	// Combine shares with the same wallet addresses
	std::sort(shares.begin(), shares.end(), [](const auto& a, const auto& b) { return *a.m_wallet < *b.m_wallet; });

	size_t k = 0;
	for (size_t i = 1, n = shares.size(); i < n; ++i)
	{
		if (*shares[i].m_wallet == *shares[k].m_wallet) {
			shares[k].m_weight += shares[i].m_weight;
		}
		else {
			++k;
			shares[k].m_weight = shares[i].m_weight;
			shares[k].m_wallet = shares[i].m_wallet;
		}
	}

	shares.resize(k + 1);

	LOGINFO(5, "get_shares: " << k + 1 << " unique wallets in PPLNS window");
	return true;
}

bool SideChain::block_seen(const PoolBlock& block)
{
	MutexLock lock(m_sidechainLock);
	return !m_seenBlocks.insert(block.m_sidechainId).second;
}

bool SideChain::add_external_block(PoolBlock& block, std::vector<hash>& missing_blocks)
{
	if (block.m_difficulty < m_minDifficulty) {
		LOGWARN(3, "add_external_block: block has invalid difficulty " << block.m_difficulty << ", expected >= " << m_minDifficulty);
		return false;
	}

	difficulty_type min_accepted_diff;
	{
		MutexLock lock(m_sidechainLock);
		if (m_blocksById.find(block.m_sidechainId) != m_blocksById.end()) {
			LOGINFO(4, "add_external_block: block " << block.m_sidechainId << " is already added");
			return true;
		}

		// Find the minimum difficulty in the current PPLNS window
		min_accepted_diff = m_curDifficulty;
		for (PoolBlock* tmp = m_chainTip; tmp && (tmp->m_sidechainHeight + m_chainWindowSize > m_chainTip->m_sidechainHeight); tmp = get_parent(tmp)) {
			if (tmp->m_difficulty < min_accepted_diff) {
				min_accepted_diff = tmp->m_difficulty;
			}
		}
	}

	LOGINFO(4, "add_external_block: height = " << block.m_sidechainHeight << ", id = " << block.m_sidechainId << ", mainchain height = " << block.m_txinGenHeight);

	// Reduce it by 50% to account for alternative chains. This is mainly an anti-spam measure, not an actual verification step
	min_accepted_diff.lo = (min_accepted_diff.lo >> 1) | (min_accepted_diff.hi << 63);
	min_accepted_diff.hi >>= 1;

	if (block.m_difficulty < min_accepted_diff) {
		LOGWARN(4, "add_external_block: block has too low difficulty " << block.m_difficulty << ", expected >= " << min_accepted_diff << ". Ignoring it.");
		return true;
	}

	// This check is not always possible to perform because of mainchain reorgs
	ChainMain data;
	if (m_pool->chainmain_get_by_hash(block.m_prevId, data)) {
		if (data.height + 1 != block.m_txinGenHeight) {
			LOGWARN(3, "add_external_block: wrong mainchain height " << block.m_txinGenHeight << ", expected " << data.height + 1);
			return false;
		}
	}
	else {
		LOGWARN(3, "add_external_block: block is built on top of an unknown mainchain block " << block.m_prevId << ", mainchain reorg might've happened");
	}

	hash seed;
	if (!m_pool->get_seed(block.m_txinGenHeight, seed)) {
		LOGWARN(3, "add_external_block: couldn't get seed hash for mainchain height " << block.m_txinGenHeight);
		return false;
	}

	hash pow_hash;
	if (!block.get_pow_hash(m_pool->hasher(), seed, pow_hash)) {
		LOGWARN(3, "add_external_block: couldn't get PoW hash for height = " << block.m_sidechainHeight << ", mainchain height " << block.m_txinGenHeight);
		return false;
	}

	if (!block.m_difficulty.check_pow(pow_hash)) {
		LOGWARN(3, "add_external_block: not enougn PoW for height = " << block.m_sidechainHeight << ", mainchain height " << block.m_txinGenHeight);
		return false;
	}

	missing_blocks.clear();
	{
		MutexLock lock(m_sidechainLock);
		if (!block.m_parent.empty() && (m_blocksById.find(block.m_parent) == m_blocksById.end())) {
			missing_blocks.push_back(block.m_parent);
		}

		for (const hash& h : block.m_uncles) {
			if (!h.empty() && (m_blocksById.find(h) == m_blocksById.end())) {
				missing_blocks.push_back(h);
			}
		}
	}

	add_block(block);
	return true;
}

void SideChain::add_block(const PoolBlock& block)
{
	LOGINFO(3, "add_block: height = " << block.m_sidechainHeight <<
		", id = " << block.m_sidechainId <<
		", mainchain height = " << block.m_txinGenHeight <<
		", verified = " << (block.m_verified ? 1 : 0)
	);

	PoolBlock* new_block = new PoolBlock(block);

	MutexLock lock(m_sidechainLock);

	auto result = m_blocksById.insert({ new_block->m_sidechainId, new_block });
	if (!result.second) {
		LOGWARN(3, "add_block: trying to add the same block twice, id = "
			<< new_block->m_sidechainId << ", sidechain height = "
			<< new_block->m_sidechainHeight << ", height = "
			<< new_block->m_txinGenHeight);

		delete new_block;
		return;
	}

	m_blocksByHeight[new_block->m_sidechainHeight].push_back(new_block);

	update_depths(new_block);

	if (new_block->m_verified) {
		if (!new_block->m_invalid) {
			update_chain_tip(new_block);
		}
	}
	else {
		verify_loop(new_block);
	}
}

bool SideChain::has_block(const hash& id)
{
	MutexLock lock(m_sidechainLock);
	return m_blocksById.find(id) != m_blocksById.end();
}

bool SideChain::get_block_blob(const hash& id, std::vector<uint8_t>& blob)
{
	MutexLock lock(m_sidechainLock);

	PoolBlock* block = nullptr;

	// Empty hash means we return current sidechain tip
	if (id == hash()) {
		block = m_chainTip;
	}
	else {
		auto it = m_blocksById.find(id);
		if (it != m_blocksById.end()) {
			block = it->second;
		}
	}

	if (!block) {
		return false;
	}

	blob.reserve(block->m_mainChainData.size() + block->m_sideChainData.size());

	blob = block->m_mainChainData;
	blob.insert(blob.end(), block->m_sideChainData.begin(), block->m_sideChainData.end());
	return true;
}

bool SideChain::get_outputs_blob(PoolBlock* block, uint64_t total_reward, std::vector<uint8_t>& blob)
{
	std::vector<MinerShare> shares;
	std::vector<uint64_t> rewards;

	shares.reserve(m_chainWindowSize * 2);
	rewards.reserve(m_chainWindowSize * 2);

	MutexLock lock(m_sidechainLock);

	if (!get_shares(block, shares) || !split_reward(total_reward, shares, rewards) || (rewards.size() != shares.size())) {
		return false;
	}

	const size_t n = shares.size();

	blob.clear();
	blob.reserve(n * 38 + 64);

	writeVarint(n, blob);

	block->m_outputs.clear();
	block->m_outputs.reserve(n);

	hash eph_public_key;
	for (size_t i = 0; i < n; ++i) {
		writeVarint(rewards[i], blob);

		blob.emplace_back(TXOUT_TO_KEY);

		shares[i].m_wallet->get_eph_public_key(block->m_txkeySec, i, eph_public_key);
		blob.insert(blob.end(), eph_public_key.h, eph_public_key.h + HASH_SIZE);

		block->m_outputs.emplace_back(rewards[i], eph_public_key);
	}

	return true;
}

void SideChain::print_status()
{
	uint64_t rem;
	uint64_t pool_hashrate = udiv128(m_curDifficulty.hi, m_curDifficulty.lo, m_targetBlockTime, &rem);

	const difficulty_type& network_diff = m_pool->miner_data().difficulty;
	uint64_t network_hashrate = udiv128(network_diff.hi, network_diff.lo, 120, &rem);

	uint64_t block_depth = 0;
	PoolBlock* cur = m_chainTip;
	const uint64_t tip_height = m_chainTip ? m_chainTip->m_sidechainHeight : 0;

	uint32_t total_blocks_in_window = 0;
	uint32_t total_uncles_in_window = 0;

	uint32_t our_blocks_in_window = 0;
	uint32_t our_uncles_in_window = 0;

	std::vector<hash> blocks_in_window;
	blocks_in_window.reserve(m_chainWindowSize * 9 / 8);

	while (cur) {
		blocks_in_window.emplace_back(cur->m_sidechainId);
		++total_blocks_in_window;

		if (cur->m_minerWallet == m_pool->params().m_wallet) {
			++our_blocks_in_window;
		}

		++block_depth;
		if (block_depth >= m_chainWindowSize) {
			break;
		}

		for (const hash& uncle_id : cur->m_uncles) {
			blocks_in_window.emplace_back(uncle_id);
			auto it = m_blocksById.find(uncle_id);
			if (it != m_blocksById.end()) {
				PoolBlock* uncle = it->second;
				if (tip_height - uncle->m_sidechainHeight < m_chainWindowSize) {
					++total_uncles_in_window;
					if (uncle->m_minerWallet == m_pool->params().m_wallet) {
						++our_uncles_in_window;
					}
				}
			}
		}

		cur = get_parent(cur);
	}

	uint64_t total_orphans = 0;
	uint64_t our_orphans = 0;

	if (m_chainTip) {
		std::sort(blocks_in_window.begin(), blocks_in_window.end());
		for (uint64_t i = 0; (i < m_chainWindowSize) && (i <= tip_height); ++i) {
			for (PoolBlock* block : m_blocksByHeight[tip_height - i]) {
				if (!std::binary_search(blocks_in_window.begin(), blocks_in_window.end(), block->m_sidechainId)) {
					LOGINFO(4, "orphan block at height " << log::Gray() << block->m_sidechainHeight << log::NoColor() << ": " << log::Gray() << block->m_sidechainId);
					++total_orphans;
					if (block->m_minerWallet == m_pool->params().m_wallet) {
						++our_orphans;
					}
				}
			}
		}
	}

	LOGINFO(0, "status" <<
		"\nMain chain height   = " << m_pool->block_template().height() <<
		"\nMain chain hashrate = " << log::Hashrate(network_hashrate) <<
		"\nSide chain height   = " << tip_height + 1 <<
		"\nSide chain hashrate = " << log::Hashrate(pool_hashrate) <<
		"\nPPLNS window        = " << total_blocks_in_window << " blocks (+" << total_uncles_in_window << " uncles, " << total_orphans << " orphans)"
		"\nYour shares         = " << our_blocks_in_window << " blocks (+" << our_uncles_in_window << " uncles, " << our_orphans << " orphans)"
		"\nNext payout         = " << log::XMRAmount(m_pool->block_template().next_payout())
	);

	LOGINFO(0, "background jobs running: " << num_running_jobs.load());
}

bool SideChain::split_reward(uint64_t reward, const std::vector<MinerShare>& shares, std::vector<uint64_t>& rewards)
{
	const size_t num_shares = shares.size();

	const uint64_t total_weight = std::accumulate(shares.begin(), shares.end(), 0ULL, [](uint64_t a, const MinerShare& b) { return a + b.m_weight; });

	rewards.clear();
	rewards.reserve(num_shares);

	// Each miner gets a proportional fraction of the block reward
	uint64_t w = 0;
	uint64_t reward_given = 0;
	for (uint64_t i = 0; i < num_shares; ++i) {
		w += shares[i].m_weight;

		uint64_t hi;
		const uint64_t lo = umul128(w, reward, &hi);

		uint64_t rem;
		const uint64_t next_value = udiv128(hi, lo, total_weight, &rem);
		rewards.emplace_back(next_value - reward_given);
		reward_given = next_value;
	}

	// Double check that we gave out the exact amount
	if (std::accumulate(rewards.begin(), rewards.end(), 0ULL) != reward) {
		LOGERR(1, "miners got incorrect reward. This should never happen because math says so. Check the code!");
		return false;
	}

	return true;
}

bool SideChain::get_difficulty(PoolBlock* tip, std::vector<DifficultyData>& difficultyData, difficulty_type& curDifficulty) const
{
	difficultyData.clear();

	PoolBlock* cur = tip;
	uint64_t oldest_timestamp = std::numeric_limits<uint64_t>::max();

	uint64_t block_depth = 0;
	do {
		oldest_timestamp = std::min(oldest_timestamp, cur->m_timestamp);
		difficultyData.emplace_back(cur->m_timestamp, cur->m_cumulativeDifficulty);

		for (const hash& uncle_id : cur->m_uncles) {
			auto it = m_blocksById.find(uncle_id);
			if (it == m_blocksById.end()) {
				LOGWARN(4, "get_difficulty: can't find uncle block at height = " << cur->m_sidechainHeight << ", id = " << uncle_id);
				LOGWARN(4, "get_difficulty: can't calculate diff for block at height = " << tip->m_sidechainHeight << ", id = " << tip->m_sidechainId << ", mainchain height = " << tip->m_txinGenHeight);
				return false;
			}

			const PoolBlock* uncle = it->second;
			if (tip->m_sidechainHeight - uncle->m_sidechainHeight < m_chainWindowSize) {
				oldest_timestamp = std::min(oldest_timestamp, uncle->m_timestamp);
				difficultyData.emplace_back(uncle->m_timestamp, uncle->m_cumulativeDifficulty);
			}
		}

		++block_depth;
		if (block_depth >= m_chainWindowSize) {
			break;
		}

		// Reached the genesis block so we're done
		if (cur->m_sidechainHeight == 0) {
			break;
		}

		auto it = m_blocksById.find(cur->m_parent);
		if (it == m_blocksById.end()) {
			LOGWARN(4, "get_difficulty: can't find parent block at height = " << cur->m_sidechainHeight - 1 << ", id = " << cur->m_parent);
			LOGWARN(4, "get_difficulty: can't calculate diff for block at height = " << tip->m_sidechainHeight << ", id = " << tip->m_sidechainId << ", mainchain height = " << tip->m_txinGenHeight);
			return false;
		}

		cur = it->second;
	} while (true);

	// Discard 10% oldest and 10% newest (by timestamp) blocks
	std::vector<uint32_t> tmpTimestamps;
	tmpTimestamps.reserve(difficultyData.size());

	std::transform(difficultyData.begin(), difficultyData.end(), std::back_inserter(tmpTimestamps),
		[oldest_timestamp](const DifficultyData& d)
		{
			return static_cast<uint32_t>(d.m_timestamp - oldest_timestamp);
		});

	const uint64_t cut_size = (difficultyData.size() + 9) / 10;
	const uint64_t index1 = cut_size - 1;
	const uint64_t index2 = difficultyData.size() - cut_size;

	std::nth_element(tmpTimestamps.begin(), tmpTimestamps.begin() + index1, tmpTimestamps.end());
	const uint64_t timestamp1 = oldest_timestamp + tmpTimestamps[index1];

	std::nth_element(tmpTimestamps.begin(), tmpTimestamps.begin() + index2, tmpTimestamps.end());
	const uint64_t timestamp2 = oldest_timestamp + tmpTimestamps[index2];

	const uint64_t delta_t = (timestamp2 > timestamp1) ? (timestamp2 - timestamp1) : 1;

	difficulty_type diff1{ std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max() };
	difficulty_type diff2{ 0, 0 };

	for (const DifficultyData& d : difficultyData) {
		if (timestamp1 <= d.m_timestamp && d.m_timestamp <= timestamp2) {
			if (d.m_cumulativeDifficulty < diff1) {
				diff1 = d.m_cumulativeDifficulty;
			}
			if (diff2 < d.m_cumulativeDifficulty) {
				diff2 = d.m_cumulativeDifficulty;
			}
		}
	}

	// This is correct as long as the difference between two 128-bit difficulties is less than 2^64, even if it wraps
	const uint64_t delta_diff = diff2.lo - diff1.lo;

	uint64_t product[2];
	product[0] = umul128(delta_diff, m_targetBlockTime, &product[1]);

	if (product[1] >= delta_t) {
		LOGERR(1, "calculated difficulty is too high for block at height = " << tip->m_sidechainHeight << ", id = " << tip->m_sidechainId << ", mainchain height = " << tip->m_txinGenHeight);
		return false;
	}

	uint64_t rem;
	curDifficulty.lo = udiv128(product[1], product[0], delta_t, &rem);
	curDifficulty.hi = 0;

	if (curDifficulty < m_minDifficulty) {
		curDifficulty = m_minDifficulty;
	}

	return true;
}

void SideChain::verify_loop(PoolBlock* block)
{
	// PoW is already checked at this point

	std::vector<PoolBlock*> blocks_to_verify(1, block);
	PoolBlock* highest_block = nullptr;

	while (!blocks_to_verify.empty()) {
		block = blocks_to_verify.back();
		blocks_to_verify.pop_back();

		if (block->m_verified) {
			continue;
		}

		verify(block);

		if (!block->m_verified) {
			LOGINFO(5, "not enough data to verify block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight);
			continue;
		}

		if (block->m_invalid) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " is invalid");
		}
		else {
			LOGINFO(3, "verified block at height = " << block->m_sidechainHeight <<
				", depth = " << block->m_depth <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight);

			// This block is now verified

			if (is_longer_chain(highest_block, block)) {
				highest_block = block;
			}
			else if (highest_block && (highest_block->m_sidechainHeight > block->m_sidechainHeight)) {
				LOGINFO(4, "block " << highest_block->m_sidechainId <<
					", height = " << highest_block->m_sidechainHeight <<
					" is not a longer chain than " << block->m_sidechainId <<
					", height " << block->m_sidechainHeight);
			}

			// If it came through a broadcast, send it to our peers
			if (block->m_wantBroadcast && !block->m_broadcasted) {
				block->m_broadcasted = true;
				m_pool->p2p_server()->broadcast(*block);
			}

			// Try to verify blocks on top of this one
			for (size_t i = 1; i <= UNCLE_BLOCK_DEPTH; ++i) {
				auto it = m_blocksByHeight.find(block->m_sidechainHeight + i);
				if (it == m_blocksByHeight.end()) {
					continue;
				}

				const std::vector<PoolBlock*>& next_blocks = it->second;
				if (!next_blocks.empty()) {
					blocks_to_verify.insert(blocks_to_verify.end(), next_blocks.begin(), next_blocks.end());
				}
			}
		}
	}

	if (highest_block) {
		update_chain_tip(highest_block);
	}

	return;
}

void SideChain::verify(PoolBlock* block)
{
	// Genesis block
	if (block->m_sidechainHeight == 0) {
		if (!block->m_parent.empty() ||
			!block->m_uncles.empty() ||
			(block->m_difficulty != m_minDifficulty) ||
			(block->m_cumulativeDifficulty != m_minDifficulty))
		{
			block->m_invalid = true;
		}

		block->m_verified = true;
		return;
	}

	// Deep block
	//
	// Blocks in PPLNS window (m_chainWindowSize) require up to m_chainWindowSize earlier blocks to verify
	// If a block is deeper than m_chainWindowSize * 2 - 1 it can't influence blocks in PPLNS window
	// Also, having so many blocks on top of this one means it was verified by the network at some point
	// We skip checks in this case to make pruning possible
	if (block->m_depth >= m_chainWindowSize * 2) {
		LOGINFO(4, "block " << block->m_sidechainId << " skipped verification");
		block->m_verified = true;
		block->m_invalid = false;
		return;
	}

	// Regular block

	// Must have a parent
	if (block->m_parent.empty()) {
		block->m_verified = true;
		block->m_invalid = true;
		return;
	}

	// Check parent
	auto it = m_blocksById.find(block->m_parent);
	if ((it == m_blocksById.end()) || !it->second->m_verified) {
		block->m_verified = false;
		return;
	}

	// If it's invalid then this block is also invalid
	PoolBlock* parent = it->second;
	if (parent->m_invalid) {
		block->m_verified = true;
		block->m_invalid = true;
		return;
	}

	const uint64_t expectedHeight = parent->m_sidechainHeight + 1;
	if (block->m_sidechainHeight != expectedHeight) {
		LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
			", id = " << block->m_sidechainId <<
			", mainchain height = " << block->m_txinGenHeight <<
			" has wrong height: expected " << expectedHeight);
		block->m_invalid = true;
		return;
	}

	// Uncle hashes must be sorted in the ascending order to prevent cheating when the same hash is repeated multiple times
	for (size_t i = 1, n = block->m_uncles.size(); i < n; ++i) {
		if (!(block->m_uncles[i - 1] < block->m_uncles[i])) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has invalid uncle order");
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}
	}

	difficulty_type expectedCumulativeDifficulty = parent->m_cumulativeDifficulty + block->m_difficulty;

	// Check uncles

	// First get a list of already mined blocks at possible uncle heights
	std::vector<hash> mined_blocks;

	if (!block->m_uncles.empty()) {
		mined_blocks.reserve(UNCLE_BLOCK_DEPTH * 2 + 1);

		PoolBlock* tmp = parent;
		for (uint64_t i = 0, n = std::min<uint64_t>(UNCLE_BLOCK_DEPTH, block->m_sidechainHeight + 1); tmp && (i < n); ++i) {
			mined_blocks.push_back(tmp->m_sidechainId);
			mined_blocks.insert(mined_blocks.end(), tmp->m_uncles.begin(), tmp->m_uncles.end());
			tmp = get_parent(tmp);
		}
	}

	for (const hash& uncle_id : block->m_uncles) {
		// Empty hash is only used in the genesis block and only for its parent
		// Uncles can't be empty
		if (uncle_id.empty()) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has empty uncle hash");
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		// Can't mine the same uncle block twice
		if (std::find(mined_blocks.begin(), mined_blocks.end(), uncle_id) != mined_blocks.end()) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has an uncle (" << uncle_id << ") that's already been mined");
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		it = m_blocksById.find(uncle_id);
		if ((it == m_blocksById.end()) || !it->second->m_verified) {
			block->m_verified = false;
			return;
		}

		PoolBlock* uncle = it->second;

		// If it's invalid then this block is also invalid
		if (uncle->m_invalid) {
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		// Check that it has correct height
		if ((uncle->m_sidechainHeight >= block->m_sidechainHeight) || (uncle->m_sidechainHeight + UNCLE_BLOCK_DEPTH < block->m_sidechainHeight)) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has an uncle at the wrong height (" << uncle->m_sidechainHeight << ')');
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		// Check that uncle and parent have the same ancestor (they must be on the same chain)
		PoolBlock* tmp = parent;
		while (tmp->m_sidechainHeight > uncle->m_sidechainHeight) {
			tmp = get_parent(tmp);
			if (!tmp) {
				LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
					", id = " << block->m_sidechainId <<
					", mainchain height = " << block->m_txinGenHeight << " has an uncle from a different chain (check 1 failed)");
				block->m_verified = true;
				block->m_invalid = true;
				return;
			}
		}

		if (tmp->m_sidechainHeight < uncle->m_sidechainHeight) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has an uncle from a different chain (check 2 failed)");
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		bool same_chain = false;
		PoolBlock* tmp2 = uncle;
		for (size_t j = 0; (j < UNCLE_BLOCK_DEPTH) && tmp && tmp2 && (tmp->m_sidechainHeight + UNCLE_BLOCK_DEPTH >= block->m_sidechainHeight); ++j) {
			if (tmp->m_parent == tmp2->m_parent) {
				same_chain = true;
				break;
			}
			tmp = get_parent(tmp);
			tmp2 = get_parent(tmp2);
		}

		if (!same_chain) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight << " has an uncle from a different chain (check 3 failed)");
			block->m_verified = true;
			block->m_invalid = true;
			return;
		}

		expectedCumulativeDifficulty += uncle->m_difficulty;
	}

	// We can verify this block now (all previous blocks in the window are verified and valid)
	// It can still turn out to be invalid
	block->m_verified = true;

	if (block->m_cumulativeDifficulty != expectedCumulativeDifficulty) {
		LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
			", id = " << block->m_sidechainId <<
			", mainchain height = " << block->m_txinGenHeight <<
			" has wrong cumulative difficulty: got " << block->m_cumulativeDifficulty << ", expected " << expectedCumulativeDifficulty);
		block->m_invalid = true;
		return;
	}

	// Verify difficulty and miner rewards only for blocks in PPLNS window
	if (block->m_depth >= m_chainWindowSize) {
		LOGINFO(4, "block " << block->m_sidechainId << " skipped diff/reward verification");
		block->m_invalid = false;
		return;
	}

	difficulty_type diff;
	if (!get_difficulty(parent, m_difficultyData, diff)) {
		block->m_invalid = true;
		return;
	}

	if (diff != block->m_difficulty) {
		LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
			", id = " << block->m_sidechainId <<
			", mainchain height = " << block->m_txinGenHeight <<
			" has wrong difficulty: got " << block->m_difficulty << ", expected " << diff);
		block->m_invalid = true;
		return;
	}

	std::vector<MinerShare> shares;
	if (!get_shares(block, shares)) {
		block->m_invalid = true;
		return;
	}

	if (shares.size() != block->m_outputs.size()) {
		LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
			", id = " << block->m_sidechainId <<
			", mainchain height = " << block->m_txinGenHeight
			<< " has invalid number of outputs: got " << block->m_outputs.size() << ", expected " << shares.size());
		block->m_invalid = true;
		return;
	}

	uint64_t total_reward = std::accumulate(block->m_outputs.begin(), block->m_outputs.end(), 0ULL,
		[](uint64_t a, const PoolBlock::TxOutput& b)
		{
			return a + b.m_reward;
		});

	std::vector<uint64_t> rewards;
	split_reward(total_reward, shares, rewards);

	if (rewards.size() != block->m_outputs.size()) {
		LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
			", id = " << block->m_sidechainId <<
			", mainchain height = " << block->m_txinGenHeight
			<< " has invalid number of outputs: got " << block->m_outputs.size() << ", expected " << rewards.size());
		block->m_invalid = true;
		return;
	}

	for (size_t i = 0, n = rewards.size(); i < n; ++i) {
		if (rewards[i] != block->m_outputs[i].m_reward) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight <<
				" has invalid reward at index " << i << ": got " << block->m_outputs[i].m_reward << ", expected " << rewards[i]);
			block->m_invalid = true;
			return;
		}

		hash eph_public_key;
		shares[i].m_wallet->get_eph_public_key(block->m_txkeySec, i, eph_public_key);

		if (eph_public_key != block->m_outputs[i].m_ephPublicKey) {
			LOGWARN(3, "block at height = " << block->m_sidechainHeight <<
				", id = " << block->m_sidechainId <<
				", mainchain height = " << block->m_txinGenHeight <<
				" pays out to a wrong wallet at index " << i);
			block->m_invalid = true;
			return;
		}
	}

	// All checks passed
	block->m_invalid = false;
}

void SideChain::update_chain_tip(PoolBlock* block)
{
	if (!block->m_verified || block->m_invalid) {
		LOGERR(1, "trying to update chain tip to an unverified or invalid block, fix the code!");
		return;
	}

	if (block->m_depth >= m_chainWindowSize) {
		LOGINFO(5, "Trying to update chain tip to a block with depth " << block->m_depth << ". Ignoring it.");
		return;
	}

	if (is_longer_chain(m_chainTip, block)) {
		difficulty_type diff;
		if (get_difficulty(block, m_difficultyData, diff)) {
			m_chainTip = block;
			m_curDifficulty = diff;

			LOGINFO(2, "new chain tip: next height = " << log::Gray() << block->m_sidechainHeight + 1 << log::NoColor() <<
				", next difficulty = " << log::Gray() << m_curDifficulty << log::NoColor() <<
				", main chain height = " << log::Gray() << m_chainTip->m_txinGenHeight);

			block->m_wantBroadcast = true;
			m_pool->update_block_template_async();
			prune_old_blocks();
		}
	}
	else if (block->m_sidechainHeight > m_chainTip->m_sidechainHeight) {
		LOGINFO(4, "block " << block->m_sidechainId <<
			", height = " << block->m_sidechainHeight <<
			" is not a longer chain than " << m_chainTip->m_sidechainId <<
			", height " << m_chainTip->m_sidechainHeight);
	}
	else if (block->m_sidechainHeight + UNCLE_BLOCK_DEPTH > m_chainTip->m_sidechainHeight) {
		LOGINFO(4, "possible uncle block: id = " << log::Gray() << block->m_sidechainId << log::NoColor() <<
			", height = " << log::Gray() << block->m_sidechainHeight);
		m_pool->update_block_template_async();
	}

	if (block->m_wantBroadcast && !block->m_broadcasted) {
		block->m_broadcasted = true;
#ifdef DEBUG_BROADCAST_DELAY_MS
		struct Work
		{
			uv_work_t req;
			P2PServer* server;
			PoolBlock* block;
		};
		Work* work = new Work{};
		work->req.data = work;
		work->server = m_pool->p2p_server();
		work->block = block;
		const int err = uv_queue_work(uv_default_loop(), &work->req,
			[](uv_work_t*)
			{
				num_running_jobs.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(DEBUG_BROADCAST_DELAY_MS));
			},
			[](uv_work_t* req, int)
			{
				Work* work = reinterpret_cast<Work*>(req->data);
				work->server->broadcast(*work->block);
				delete reinterpret_cast<Work*>(req->data);
				num_running_jobs.fetch_sub(1);
			});
		if (err) {
			LOGERR(1, "update_chain_tip: uv_queue_work failed, error " << uv_err_name(err));
		}
#else
		m_pool->p2p_server()->broadcast(*block);
#endif
	}
}

PoolBlock* SideChain::get_parent(const PoolBlock* block)
{
	if (block) {
		auto it = m_blocksById.find(block->m_parent);
		if (it != m_blocksById.end()) {
			return it->second;
		}
	}
	return nullptr;
}

bool SideChain::is_longer_chain(const PoolBlock* block, const PoolBlock* candidate)
{
	if (!candidate || !candidate->m_verified || candidate->m_invalid) {
		return false;
	}

	if (!block) {
		return true;
	}

	// If these two blocks are on the same chain, they must have a common ancestor

	const PoolBlock* block_ancestor = block;
	while (block_ancestor->m_sidechainHeight > candidate->m_sidechainHeight) {
		const hash& id = block_ancestor->m_parent;
		block_ancestor = get_parent(block_ancestor);
		if (!block_ancestor) {
			LOGINFO(4, "couldn't find ancestor " << id << " of block " << block->m_sidechainId << " at height " << block->m_sidechainHeight);
			break;
		}
	}

	if (block_ancestor) {
		const PoolBlock* candidate_ancestor = candidate;
		while (candidate_ancestor->m_sidechainHeight > block_ancestor->m_sidechainHeight) {
			const hash& id = candidate_ancestor->m_parent;
			candidate_ancestor = get_parent(candidate_ancestor);
			if (!candidate_ancestor) {
				LOGINFO(4, "couldn't find ancestor " << id << " of block " << candidate->m_sidechainId << " at height " << candidate->m_sidechainHeight);
				break;
			}
		}

		while (block_ancestor && candidate_ancestor) {
			if (block_ancestor->m_parent == candidate_ancestor->m_parent) {
				// If they are really on the same chain, we can just compare cumulative difficulties
				return block->m_cumulativeDifficulty < candidate->m_cumulativeDifficulty;
			}
			block_ancestor = get_parent(block_ancestor);
			candidate_ancestor = get_parent(candidate_ancestor);
		}
	}

	// They're on totally different chains. Compare total difficulties over the last m_chainWindowSize blocks
	difficulty_type block_total_diff;
	difficulty_type candidate_total_diff;

	const PoolBlock* old_chain = block;
	const PoolBlock* new_chain = candidate;

	uint64_t candidate_mainchain_height = 0;
	hash mainchain_prev_id;

	for (uint64_t i = 0; (i < m_chainWindowSize) && (old_chain || new_chain); ++i) {
		if (old_chain) {
			block_total_diff += old_chain->m_difficulty;
			old_chain = get_parent(old_chain);
		}

		if (new_chain) {
			candidate_total_diff += new_chain->m_difficulty;

			ChainMain data;
			if ((new_chain->m_prevId != mainchain_prev_id) && m_pool->chainmain_get_by_hash(new_chain->m_prevId, data)) {
				mainchain_prev_id = new_chain->m_prevId;
				candidate_mainchain_height = std::max(candidate_mainchain_height, data.height);
			}

			new_chain = get_parent(new_chain);
		}
	}

	if (block_total_diff >= candidate_total_diff) {
		return false;
	}

	// Final check: candidate chain must be built on top of recent mainchain blocks
	if (candidate_mainchain_height + 10 < m_pool->miner_data().height) {
		LOGWARN(3, "received a longer alternative chain but it's stale: height " << candidate_mainchain_height << ", current height " << m_pool->miner_data().height);
		return false;
	}

	LOGINFO(3, "received a longer alternative chain: height " <<
		log::Gray() << block->m_sidechainHeight << log::NoColor() << " -> " <<
		log::Gray() << candidate->m_sidechainHeight << log::NoColor() << ", cumulative difficulty " <<
		log::Gray() << block->m_cumulativeDifficulty << log::NoColor() << " -> " <<
		log::Gray() << candidate->m_cumulativeDifficulty);

	return true;
}

void SideChain::update_depths(PoolBlock* block)
{
	for (size_t i = 1; i <= UNCLE_BLOCK_DEPTH; ++i) {
		for (PoolBlock* child : m_blocksByHeight[block->m_sidechainHeight + i]) {
			if (child->m_parent == block->m_sidechainId) {
				if (i != 1) {
					LOGERR(1, "m_blocksByHeight is inconsistent with child->m_parent. Fix the code!");
				}
				else {
					block->m_depth = std::max(block->m_depth, child->m_depth + 1);
				}
			}

			auto it = std::find(child->m_uncles.begin(), child->m_uncles.end(), block->m_sidechainId);
			if (it != child->m_uncles.end()) {
				block->m_depth = std::max(block->m_depth, child->m_depth + i);
			}
		}
	}

	std::vector<PoolBlock*> blocks_to_update(1, block);

	do {
		block = blocks_to_update.back();
		blocks_to_update.pop_back();

		auto it = m_blocksById.find(block->m_parent);
		if (it != m_blocksById.end()) {
			if (it->second->m_sidechainHeight + 1 != block->m_sidechainHeight) {
				LOGERR(1, "m_sidechainHeight is inconsistent with block->m_parent. Fix the code!");
			}

			if (it->second->m_depth < block->m_depth + 1) {
				it->second->m_depth = block->m_depth + 1;
				blocks_to_update.push_back(it->second);
			}
		}

		for (const hash& uncle_id : block->m_uncles) {
			it = m_blocksById.find(uncle_id);
			if (it == m_blocksById.end()) {
				continue;
			}

			if ((it->second->m_sidechainHeight >= block->m_sidechainHeight) || (it->second->m_sidechainHeight + UNCLE_BLOCK_DEPTH < block->m_sidechainHeight)) {
				LOGERR(1, "m_sidechainHeight is inconsistent with block->m_uncles. Fix the code!");
			}

			const uint64_t d = block->m_sidechainHeight - it->second->m_sidechainHeight;
			if (it->second->m_depth < block->m_depth + d) {
				it->second->m_depth = block->m_depth + d;
				blocks_to_update.push_back(it->second);
			}
		}
	} while (!blocks_to_update.empty());
}

void SideChain::prune_old_blocks()
{
	// Leave 2 minutes worth of spare blocks in addition to 2xPPLNS window for lagging nodes which need to sync
	const uint64_t prune_distance = m_chainWindowSize * 2 + 120 / m_targetBlockTime;

	if (m_chainTip->m_sidechainHeight < prune_distance) {
		return;
	}

	const uint64_t h = m_chainTip->m_sidechainHeight - prune_distance;

	uint64_t num_blocks_pruned = 0;

	for (auto it = m_blocksByHeight.begin(); (it != m_blocksByHeight.end()) && (it->first <= h);) {
		for (PoolBlock* block : it->second) {
			if (block->m_depth < prune_distance) {
				continue;
			}
			auto it2 = m_blocksById.find(block->m_sidechainId);
			if (it2 != m_blocksById.end()) {
				m_blocksById.erase(it2);
				delete block;
				++num_blocks_pruned;
			}
			else {
				LOGERR(1, "m_blocksByHeight and m_blocksById are inconsistent at height " << it->first << ". Fix the code!");
			}
		}

		auto old_it = it;
		++it;
		m_blocksByHeight.erase(old_it);
	}

	if (num_blocks_pruned) {
		LOGINFO(3, "pruned " << num_blocks_pruned << " old blocks at heights <= " << h);
	}
}

void SideChain::get_missing_blocks(std::vector<hash>& missing_blocks)
{
	missing_blocks.clear();

	MutexLock lock(m_sidechainLock);

	for (auto& b : m_blocksById) {
		if (b.second->m_verified) {
			continue;
		}

		if (!b.second->m_parent.empty() && (m_blocksById.find(b.second->m_parent) == m_blocksById.end())) {
			missing_blocks.push_back(b.second->m_parent);
		}

		for (const hash& h : b.second->m_uncles) {
			if (!h.empty() && (m_blocksById.find(h) == m_blocksById.end())) {
				missing_blocks.push_back(h);
			}
		}
	}
}

bool SideChain::load_config(const std::string& filename)
{
	if (filename.empty()) {
		LOGINFO(1, "using default config");
		return true;
	}

	LOGINFO(1, "loading config from " << log::Gray() << filename);

	std::ifstream f(filename);
	if (!f.is_open()) {
		LOGERR(1, "can't open " << filename);
		return false;
	}

	rapidjson::Document doc;
	rapidjson::IStreamWrapper s(f);
	if (doc.ParseStream(s).HasParseError()) {
		LOGERR(1, "failed to parse JSON data in " << filename);
		return false;
	}

	if (!doc.IsObject()) {
		LOGERR(1, "invalid JSON data in " << filename << ": top level is not an object");
		return false;
	}

	parseValue(doc, "name", m_poolName);
	parseValue(doc, "password", m_poolPassword);
	parseValue(doc, "block_time", m_targetBlockTime);

	uint64_t min_diff;
	if (parseValue(doc, "min_diff", min_diff)) {
		m_minDifficulty = { min_diff, 0 };
	}

	parseValue(doc, "pplns_window", m_chainWindowSize);
	parseValue(doc, "uncle_penalty", m_unclePenalty);

	return true;
}

bool SideChain::check_config()
{
	if (m_poolName.empty()) {
		LOGERR(1, "name can't be empty");
		return false;
	}

	if (m_poolName.length() > 128) {
		LOGERR(1, "name is too long (must be 128 characters max)");
		return false;
	}

	if (m_poolPassword.length() > 128) {
		LOGERR(1, "password is too long (must be 128 characters max)");
		return false;
	}

	if ((m_targetBlockTime < 1) || (m_targetBlockTime > 120)) {
		LOGERR(1, "block_time is invalid (must be between 1 and 120)");
		return false;
	}

	const difficulty_type min_diff{ MIN_DIFFICULTY, 0 };
	const difficulty_type max_diff{ 1000000000, 0 };

	if ((m_minDifficulty < min_diff) || (max_diff < m_minDifficulty)) {
		LOGERR(1, "min_diff is invalid (must be between " << min_diff << " and " << max_diff << ')');
		return false;
	}

	if ((m_chainWindowSize < 60) || (m_chainWindowSize > 2160)) {
		LOGERR(1, "pplns_window is invalid (must be between 60 and 2160)");
		return false;
	}

	if ((m_unclePenalty < 1) || (m_unclePenalty > 99)) {
		LOGERR(1, "uncle_penalty is invalid (must be between 1 and 99)");
		return false;
	}

	LOGINFO(1, log::LightCyan() << "pool name     = " << m_poolName);
	LOGINFO(1, log::LightCyan() << "block time    = " << m_targetBlockTime << " seconds");
	LOGINFO(1, log::LightCyan() << "min diff      = " << m_minDifficulty);
	LOGINFO(1, log::LightCyan() << "PPLNS window  = " << m_chainWindowSize << " blocks");
	LOGINFO(1, log::LightCyan() << "uncle penalty = " << m_unclePenalty << '%');

	return true;
}

} // namespace p2pool
