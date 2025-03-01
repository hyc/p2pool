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

#include "uv_util.h"
#include <map>
#include <set>

namespace p2pool {

template<size_t READ_BUF_SIZE, size_t WRITE_BUF_SIZE>
class TCPServer : public nocopy_nomove
{
public:
	struct Client;
	typedef Client* (*allocate_client_callback)();

	TCPServer(allocate_client_callback allocate_new_client, const std::string& listen_addresses);
	virtual ~TCPServer();

	template<typename T>
	void parse_address_list(const std::string& address_list, T callback);

	bool connect_to_peer(bool is_v6, const char* ip, int port);

	void drop_connections();
	void shutdown_tcp();
	virtual void print_status();

	uv_loop_t* get_loop() { return &m_loop; }

	int listen_port() const { return m_listenPort; }

	struct raw_ip
	{
		alignas(8) uint8_t data[16];

		FORCEINLINE bool operator<(const raw_ip& other) const
		{
			const uint64_t* a = reinterpret_cast<const uint64_t*>(data);
			const uint64_t* b = reinterpret_cast<const uint64_t*>(other.data);

			if (a[1] < b[1]) return true;
			if (a[1] > b[1]) return false;

			return a[0] < b[0];
		}

		FORCEINLINE bool operator==(const raw_ip& other) const
		{
			const uint64_t* a = reinterpret_cast<const uint64_t*>(data);
			const uint64_t* b = reinterpret_cast<const uint64_t*>(other.data);

			return (a[0] == b[0]) && (a[1] == b[1]);
		}

		FORCEINLINE bool operator!=(const raw_ip& other) const { return !operator==(other); }
	};

	static_assert(sizeof(raw_ip) == 16, "struct raw_ip has invalid size");
	static_assert(sizeof(in6_addr) == 16, "struct in6_addr has invalid size");
	static_assert(sizeof(in_addr) == 4, "struct in_addr has invalid size");

	bool connect_to_peer(bool is_v6, const raw_ip& ip, int port);
	virtual void on_connect_failed(bool is_v6, const raw_ip& ip, int port);

	void ban(const raw_ip& ip, uint64_t seconds);

	struct Client
	{
		Client();
		virtual ~Client();

		virtual void reset();
		virtual bool on_connect() = 0;
		virtual bool on_read(char* data, uint32_t size) = 0;

		static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
		static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
		static void on_write(uv_write_t* req, int status);

		void close();
		void ban(uint64_t seconds);

		void init_addr_string(bool is_v6, const sockaddr_storage* peer_addr);

		TCPServer* m_owner;

		// Used to maintain connected clients list
		Client* m_prev;
		Client* m_next;

		uv_tcp_t m_socket;
		uv_write_t m_write;
		uv_connect_t m_connectRequest;

		bool m_isV6;
		bool m_isIncoming;
		raw_ip m_addr;
		int m_port;
		char m_addrString[64];

		bool m_readBufInUse;
		char m_readBuf[READ_BUF_SIZE];
		uint32_t m_numRead;

		struct WriteBuf
		{
			Client* m_client;
			uv_write_t m_write;
			char m_data[WRITE_BUF_SIZE];
		};

		uv_mutex_t m_writeBuffersLock;
		std::vector<WriteBuf*> m_writeBuffers;

		std::atomic<uint32_t> m_resetCounter{ 0 };

		uv_mutex_t m_sendLock;
	};

	struct SendCallbackBase
	{
		virtual ~SendCallbackBase() {}
		virtual size_t operator()(void*) = 0;
	};

	template<typename T>
	struct SendCallback : public SendCallbackBase
	{
		explicit FORCEINLINE SendCallback(T&& callback) : m_callback(std::move(callback)) {}
		size_t operator()(void* buf) override { return m_callback(buf); }

	private:
		SendCallback& operator=(SendCallback&&) = delete;

		T m_callback;
	};

	template<typename T>
	FORCEINLINE bool send(Client* client, T&& callback) { return send_internal(client, SendCallback<T>(std::move(callback))); }

private:
	static void loop(void* data);
	static void on_new_connection(uv_stream_t* server, int status);
	static void on_connection_close(uv_handle_t* handle);
	static void on_connect(uv_connect_t* req, int status);
	void on_new_client(uv_stream_t* server);
	void on_new_client_nolock(uv_stream_t* server, Client* client);

	bool connect_to_peer_nolock(Client* client, bool is_v6, const sockaddr* addr);

	bool send_internal(Client* client, SendCallbackBase&& callback);

	allocate_client_callback m_allocateNewClient;

	void start_listening(const std::string& listen_addresses);

	std::vector<uv_tcp_t*> m_listenSockets6;
	std::vector<uv_tcp_t*> m_listenSockets;
	uv_thread_t m_loopThread;

protected:
	std::atomic<int> m_finished{ 0 };
	int m_listenPort;

	uv_loop_t m_loop;

	uv_mutex_t m_clientsListLock;
	std::vector<Client*> m_preallocatedClients;
	Client* m_connectedClientsList;
	uint32_t m_numConnections;
	uint32_t m_numIncomingConnections;

	uv_mutex_t m_bansLock;
	std::map<raw_ip, time_t> m_bans;

	uv_mutex_t m_pendingConnectionsLock;
	std::set<raw_ip> m_pendingConnections;
};

} // namespace p2pool
