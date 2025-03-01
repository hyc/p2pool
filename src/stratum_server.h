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

#pragma once

#include "tcp_server.h"
#include <rapidjson/document.h>
#include <random>

namespace p2pool {

class p2pool;
class BlockTemplate;

static constexpr size_t STRATUM_BUF_SIZE = log::Stream::BUF_SIZE + 1;

class StratumServer : public TCPServer<STRATUM_BUF_SIZE, STRATUM_BUF_SIZE>
{
public:
	explicit StratumServer(p2pool *pool);
	~StratumServer();

	void on_block(const BlockTemplate& block);

	struct StratumClient : public Client
	{
		StratumClient();
		~StratumClient();

		static Client* allocate() { return new StratumClient(); }

		void reset() override;
		bool on_connect() override { return true; }
		bool on_read(char* data, uint32_t size) override;

		bool process_request(char* data, uint32_t size);
		bool process_login(rapidjson::Document& doc, uint32_t id);
		bool process_submit(rapidjson::Document& doc, uint32_t id);

		uint32_t m_rpcId;

		uv_mutex_t m_jobsLock;

		struct SavedJob {
			uint32_t job_id;
			uint32_t extra_nonce;
			uint32_t template_id;
			uint64_t target;
		} m_jobs[4];

		uint32_t m_perConnectionJobId;
	};

	bool on_login(StratumClient* client, uint32_t id);
	bool on_submit(StratumClient* client, uint32_t id, const char* job_id_str, const char* nonce_str);
	uint64_t get_random64();

private:
	static void on_share_found(uv_work_t* req);
	static void on_after_share_found(uv_work_t* req, int status);

	p2pool* m_pool;

	struct BlobsData
	{
		std::vector<uint8_t> m_blobs;
		size_t m_blobSize;
		uint64_t m_target;
		uint32_t m_numClientsExpected;
		uint32_t m_templateId;
		uint64_t m_height;
		hash m_seedHash;
	};

	uv_mutex_t m_blobsQueueLock;
	uv_async_t m_blobsAsync;
	std::vector<BlobsData*> m_blobsQueue;

	static void on_blobs_ready(uv_async_t* handle) { reinterpret_cast<StratumServer*>(handle->data)->on_blobs_ready(); }
	void on_blobs_ready();

	std::atomic<uint32_t> m_extraNonce;

	uv_mutex_t m_rngLock;
	std::random_device m_rd;
	std::mt19937_64 m_rng;

	struct SubmittedShare
	{
		uv_work_t m_req;
		StratumServer* m_server;
		StratumClient* m_client;
		uint32_t m_clientResetCounter;
		uint32_t m_rpcId;
		uint32_t m_id;
		uint32_t m_templateId;
		uint32_t m_nonce;
		uint32_t m_extraNonce;

		enum class Result {
			STALE,
			COULDNT_CHECK_POW,
			LOW_DIFF,
			OK
		} m_result;
	};

	uv_mutex_t m_submittedSharesPoolLock;
	std::vector<SubmittedShare*> m_submittedSharesPool;
};

} // namespace p2pool
