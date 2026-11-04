#include <iostream>
#include <fstream>
#include <cstdlib>
#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>
#include "include/protocol.hpp"

const uint16_t udp_port = 41000;
const uint16_t tcp_port = 41001;

void process_job(NET_StreamSocket *client) {
	uint32_t header_size_be = 0;
	if (!read_exact(client, &header_size_be, sizeof(uint32_t))) {
		NET_DestroyStreamSocket(client);
		return;
	}
	uint32_t header_size = SDL_Swap32BE(header_size_be);

	std::vector<char> json_data(header_size + 1, '\0');
	if (!read_exact(client, json_data.data(), header_size)) {
		NET_DestroyStreamSocket(client);
		return;
	}

	json header = json::parse(json_data.data(), nullptr, false);
	if (header.is_discarded() || header.value("type", "") != "JOB") {
		NET_DestroyStreamSocket(client);
		return;
	}

	int task_id = header.value("task_id", -1);
	size_t payload_size = header.value("payload_size", 0);

	std::vector<uint8_t> source_code(payload_size);
	if (payload_size > 0) {
		if (!read_exact(client, source_code.data(), payload_size)) {
			NET_DestroyStreamSocket(client);
			return;
		}
	}

	std::string in_file = "task_" + std::to_string(task_id) + ".ii";
	std::string out_file = "task_" + std::to_string(task_id) + ".o";

	std::ofstream ofs(in_file, std::ios::binary);
	ofs.write(reinterpret_cast<const char *>(source_code.data()), source_code.size());
	ofs.close();

	std::string cmd = "g++ -c " + in_file + " -o " + out_file;
	int ret = std::system(cmd.c_str());

	if (ret == 0) {
		std::ifstream ifs(out_file, std::ios::binary | std::ios::ate);
		if (ifs.is_open()) {
			size_t obj_size = ifs.tellg();
			ifs.seekg(0);
			std::vector<uint8_t> obj_data(obj_size);
			ifs.read(reinterpret_cast<char *>(obj_data.data()), obj_size);

			auto response = serialize_tcp_message("RESULT", task_id, obj_data);
			write_exact(client, response.data(), response.size());
		} else {
			auto response = serialize_tcp_message("ERROR", task_id, {}, "Object file not found");
			write_exact(client, response.data(), response.size());
		}
	} else {
		auto response = serialize_tcp_message("ERROR", task_id, {}, "Compilation failed");
		write_exact(client, response.data(), response.size());
	}

	NET_DestroyStreamSocket(client);
}

int main() {
	SDL_Init(0);
	NET_Init();

	NET_DatagramSocket *udp_sock = NET_CreateDatagramSocket(nullptr, udp_port, 0);
	NET_Server *tcp_server = NET_CreateServer(nullptr, tcp_port, 0);

	std::cout << "Worker started. UDP: " << udp_port << ", TCP: " << tcp_port << std::endl;

	while (true) {
		NET_Datagram *dgram = nullptr;
		if (NET_ReceiveDatagram(udp_sock, &dgram) && dgram != nullptr) {
			std::string raw_json(reinterpret_cast<char *>(dgram->buf), dgram->buflen);
			json req = json::parse(raw_json, nullptr, false);
			if (!req.is_discarded() && req.value("type", "") == "PING") {
				json res = {{"type", "PONG"}, {"tcp_port", tcp_port}};
				std::string res_str = res.dump();
				NET_SendDatagram(udp_sock, dgram->addr, dgram->port, res_str.c_str(), static_cast<int>(res_str.size()));
			}
			NET_DestroyDatagram(dgram);
		}

		NET_StreamSocket *client = nullptr;
		if (NET_AcceptClient(tcp_server, &client) && client != nullptr) {
			process_job(client);
		}

		SDL_Delay(10);
	}

	NET_DestroyServer(tcp_server);
	NET_DestroyDatagramSocket(udp_sock);
	NET_Quit();
	SDL_Quit();
	return 0;
}
