#include <iostream>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>
#include "include/protocol.hpp"

std::vector<uint8_t> preprocess_file(const std::string &filename) {
	std::string out_file = filename + ".ii";
	std::string cmd = "g++ -E " + filename + " -o " + out_file;
	if (std::system(cmd.c_str()) != 0) return {};

	std::ifstream ifs(out_file, std::ios::binary | std::ios::ate);
	if (!ifs.is_open()) return {};
	size_t size = ifs.tellg();
	ifs.seekg(0);
	std::vector<uint8_t> data(size);
	ifs.read(reinterpret_cast<char *>(data.data()), size);
	return data;
}

int main() {
	SDL_Init(0);
	NET_Init();

	std::string worker_ip = "127.0.0.1";
	uint16_t worker_udp_port = 41000;
	uint16_t worker_tcp_port = 0;

	NET_Address *worker_addr = NET_ResolveHostname(worker_ip.c_str());
	if (NET_WaitUntilResolved(worker_addr, 5000) < 0) {
		std::cerr << "Failed to resolve hostname" << std::endl;
		return 1;
	}

	NET_DatagramSocket *udp_sock = NET_CreateDatagramSocket(nullptr, 0, 0);

	json ping_req = {{"type", "PING"}};
	std::string ping_str = ping_req.dump();
	NET_SendDatagram(udp_sock, worker_addr, worker_udp_port, ping_str.c_str(), static_cast<int>(ping_str.size()));

	for (int i = 0; i < 50; ++i) {
		NET_Datagram *dgram = nullptr;
		if (NET_ReceiveDatagram(udp_sock, &dgram) && dgram != nullptr) {
			std::string raw_json(reinterpret_cast<char *>(dgram->buf), dgram->buflen);
			json pong_res = json::parse(raw_json, nullptr, false);
			if (!pong_res.is_discarded() && pong_res.value("type", "") == "PONG") {
				worker_tcp_port = pong_res.value("tcp_port", 0);
			}
			NET_DestroyDatagram(dgram);
			if (worker_tcp_port != 0) break;
		}
		SDL_Delay(10);
	}
	NET_DestroyDatagramSocket(udp_sock);

	if (worker_tcp_port == 0) {
		std::cerr << "No available worker" << std::endl;
		NET_UnrefAddress(worker_addr);
		return 1;
	}

	std::vector<uint8_t> ii_data = preprocess_file("main.cpp");
	if (ii_data.empty()) {
		std::cerr << "Preprocessing failed" << std::endl;
		NET_UnrefAddress(worker_addr);
		return 1;
	}

	NET_StreamSocket *tcp_client = NET_CreateClient(worker_addr, worker_tcp_port, 0);
	if (NET_WaitUntilConnected(tcp_client, 5000) < 0) {
		std::cerr << "TCP connection failed" << std::endl;
		NET_DestroyStreamSocket(tcp_client);
		NET_UnrefAddress(worker_addr);
		return 1;
	}

	auto job_msg = serialize_tcp_message("JOB", 1, ii_data);
	write_exact(tcp_client, job_msg.data(), job_msg.size());

	uint32_t header_size_be = 0;
	if (read_exact(tcp_client, &header_size_be, sizeof(uint32_t))) {
		uint32_t header_size = SDL_Swap32BE(header_size_be);

		std::vector<char> json_data(header_size + 1, '\0');
		if (read_exact(tcp_client, json_data.data(), header_size)) {
			json header = json::parse(json_data.data(), nullptr, false);

			if (!header.is_discarded() && header.value("type", "") == "RESULT") {
				size_t payload_size = header.value("payload_size", 0);
				std::vector<uint8_t> obj_data(payload_size);
				if (read_exact(tcp_client, obj_data.data(), payload_size)) {
					std::ofstream ofs("main.o", std::ios::binary);
					ofs.write(reinterpret_cast<const char *>(obj_data.data()), obj_data.size());
					ofs.close();

					std::cout << "Linking..." << std::endl;
					std::system("g++ main.o -o result_app");
				}
			} else if (!header.is_discarded() && header.value("type", "") == "ERROR") {
				std::cerr << "Worker error: " << header.value("message", "unknown") << std::endl;
			}
		}
	}

	NET_DestroyStreamSocket(tcp_client);
	NET_UnrefAddress(worker_addr);
	NET_Quit();
	SDL_Quit();

	return 0;
}
