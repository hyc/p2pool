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

static void usage()
{
	printf(
		"\nUsage:\n\n" \
		"--wallet             Wallet address to mine to. Subaddresses and integrated addresses are not supported!\n"
		"--host               IP address of your Monero node, default is 127.0.0.1\n"
		"--rpc-port           monerod RPC API port number, default is 18081\n"
		"--zmq-port           monerod ZMQ pub port number, default is 18083 (same port as in monerod's \"--zmq-pub\" command line parameter)\n"
		"--stratum            Comma-separated list of IP:port for stratum server to listen on\n"
		"--p2p                Comma-separated list of IP:port for p2p server to listen on\n"
		"--addpeers           Comma-separated list of IP:port of other p2pool nodes to connect to\n"
		"--light-mode         Don't allocate RandomX dataset, saves 2GB of RAM\n"
		"--loglevel           Verbosity of the log, integer number between 0 and 5\n"
		"--config             Name of the p2pool config file\n"
		"--help               Show this help message\n\n"
		"Example command line:\n\n"
		"%s --host 127.0.0.1 --rpc-port 18081 --zmq-port 18083 --wallet YOUR_WALLET_ADDRESS --stratum [::]:3333,0.0.0.0:3333 --p2p [::]:37890,0.0.0.0:37890\n\n",
#ifdef _WIN32
		"p2pool.exe"
#else
		"./p2pool"
#endif
	);
}

int main(int argc, char* argv[])
{
	if (argc == 1) {
		usage();
		return 0;
	}

	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "/help") || !strcmp(argv[i], "-h") || !strcmp(argv[i], "/h")) {
			usage();
			return 0;
		}
	}

	p2pool::p2pool pool(argc, argv);
	return pool.run();
}
