#include <iostream>
#include <filesystem>
#include <stdexcept>

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include "include/protocol.hpp"
#include "include/common.hpp"

namespace fs = std::filesystem;

void send_error(NET_StreamSocket *client, int task_id, const std::string &message) {
	try {
		auto response = serialize_tcp_message("ERROR", task_id, {}, message);

		if (!write_exact(client, response.data(), response.size())) {
			std::cerr << "Failed to send ERROR: " << SDL_GetError() << std::endl;
		}
	} catch (const std::exception &e) {
		std::cerr << "Failed to create ERROR response: " << e.what() << std::endl;
	}
}

void process_job(NET_StreamSocket *client) {
	StreamPtr connection(client);

	Message request;
	std::string error;

	if (!read_tcp_message(client, request, error)) {
		std::cerr << "Invalid request: " << error << std::endl;
		return;
	}

	int task_id = request.header["task_id"].get<int>();

	if (request.header["type"] != "JOB" || request.payload.empty()) {
		send_error(client, task_id, "Expected nonempty JOB");
		return;
	}

	try {
		TempDir temp;

		fs::path in_file = temp.path() / "input.ii";
		fs::path out_file = temp.path() / "output.o";

		if (!save_file(in_file, request.payload)) {
			send_error(client, task_id, "Failed to save source file");
			return;
		}

		std::cout << "Compiling task " << task_id << "..." << std::endl;

		int ret = run_process({
			"g++", "-c", "-x", "c++-cpp-output",
			in_file.string(), "-o", out_file.string()
		});

		if (ret != 0) {
			send_error(client, task_id, "Compilation failed");
			return;
		}

		std::vector<uint8_t> obj_data;

		if (!load_file(out_file, obj_data, MAX_PAYLOAD_SIZE) || obj_data.empty()) {
			send_error(client, task_id, "Object file not found or exceeds limit");
			return;
		}

		auto response = serialize_tcp_message("RESULT", task_id, obj_data);

		if (!write_exact(client, response.data(), response.size())) {
			std::cerr << "Failed to send RESULT: " << SDL_GetError() << std::endl;
			return;
		}

		std::cout << "Completed task " << task_id << std::endl;
	} catch (const std::exception &e) {
		send_error(client, task_id, e.what());
	}
}

int main() {
	try {
		NetworkContext net;

		AddressPtr local_addr = resolve_address("127.0.0.1");

		DatagramPtr udp_sock(
			NET_CreateDatagramSocket(local_addr.get(), UDP_PORT, 0)
		);

		if (!udp_sock) {
			throw std::runtime_error(std::string("UDP socket: ") + SDL_GetError());
		}

		ServerPtr tcp_server(
			NET_CreateServer(local_addr.get(), TCP_PORT, 0)
		);

		if (!tcp_server) {
			throw std::runtime_error(std::string("TCP server: ") + SDL_GetError());
		}

		std::cout << "Worker started. UDP: " << UDP_PORT
			<< ", TCP: " << TCP_PORT << std::endl;

		while (true) {
			NET_Datagram *dgram = nullptr;

			if (!NET_ReceiveDatagram(udp_sock.get(), &dgram)) {
				throw std::runtime_error(std::string("UDP receive: ") + SDL_GetError());
			}

			PacketPtr packet(dgram);

			if (packet) {
				std::string raw_json(
					reinterpret_cast<const char *>(packet->buf),
					packet->buflen
				);

				json req = json::parse(raw_json, nullptr, false);

				if (req.is_object() &&
					req.contains("type") &&
					req["type"].is_string() &&
					req["type"] == "PING") {

					json res = {
						{"type", "PONG"},
						{"tcp_port", TCP_PORT}
					};

					std::string res_str = res.dump();

					if (!NET_SendDatagram(
						udp_sock.get(), packet->addr, packet->port,
						res_str.data(), static_cast<int>(res_str.size())
					)) {
						std::cerr << "Failed to send PONG: "
							<< SDL_GetError() << std::endl;
					}
				}
			}

			NET_StreamSocket *client = nullptr;

			if (!NET_AcceptClient(tcp_server.get(), &client)) {
				throw std::runtime_error(std::string("TCP accept: ") + SDL_GetError());
			}

			if (client != nullptr) {
				process_job(client);
			}

			SDL_Delay(10);
		}
	} catch (const std::exception &e) {
		std::cerr << "Worker error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
