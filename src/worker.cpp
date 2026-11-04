#include <iostream>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include "include/protocol.hpp"
#include "include/common.hpp"

namespace fs = std::filesystem;

constexpr size_t MAX_CLIENTS = 4;
std::mutex output_mutex;

struct ClientThread {
	std::thread thread;
	std::shared_ptr<std::atomic<bool>> finished;
};

void log_message(const std::string &message) {
	std::lock_guard<std::mutex> lock(output_mutex);
	std::cout << message << std::endl;
}

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

		log_message("Compiling task " + std::to_string(task_id) + "...");

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

		log_message("Completed task " + std::to_string(task_id));
	} catch (const std::exception &e) {
		send_error(client, task_id, e.what());
	}
}

uint16_t parse_port(const char *value) {
	size_t parsed = 0;
	long port = std::stol(value, &parsed);

	if (value[parsed] != '\0' || port <= 0 || port > 65535) {
		throw std::runtime_error("Invalid port number");
	}

	return static_cast<uint16_t>(port);
}

int main(int argc, char *argv[]) {
	try {
		if (argc != 1 && argc != 3 && argc != 4) {
			std::cerr << "Usage: worker [udp_port tcp_port [bind_ip]]" << std::endl;
			return 1;
		}

		uint16_t udp_port = argc >= 3 ? parse_port(argv[1]) : UDP_PORT;
		uint16_t tcp_port = argc >= 3 ? parse_port(argv[2]) : TCP_PORT;
		std::string bind_ip = argc == 4 ? argv[3] : "127.0.0.1";

		NetworkContext net;
		AddressPtr local_addr;

		if (bind_ip != "0.0.0.0") {
			local_addr = resolve_address(bind_ip);
		}

		DatagramPtr udp_sock(NET_CreateDatagramSocket(local_addr.get(), udp_port, 0));
		if (!udp_sock) {
			throw std::runtime_error(std::string("UDP socket: ") + SDL_GetError());
		}

		ServerPtr tcp_server(NET_CreateServer(local_addr.get(), tcp_port, 0));
		if (!tcp_server) {
			throw std::runtime_error(std::string("TCP server: ") + SDL_GetError());
		}

		std::cout << "Worker started on " << bind_ip
			<< ". UDP: " << udp_port << ", TCP: " << tcp_port << std::endl;

		std::vector<ClientThread> clients;
		clients.reserve(MAX_CLIENTS);
		std::string fatal_error;

		while (fatal_error.empty()) {
			// Join threads that have completed their jobs.
			for (auto it = clients.begin(); it != clients.end();) {
				if (it->finished->load()) {
					it->thread.join();
					it = clients.erase(it);
				} else {
					++it;
				}
			}

			NET_Datagram *dgram = nullptr;
			if (!NET_ReceiveDatagram(udp_sock.get(), &dgram)) {
				fatal_error = std::string("UDP receive: ") + SDL_GetError();
				break;
			}

			PacketPtr packet(dgram);
			if (packet) {
				std::string raw_json(
					reinterpret_cast<const char *>(packet->buf), packet->buflen
				);

				json req = json::parse(raw_json, nullptr, false);
				if (req.is_object() && req.contains("type") &&
					req["type"].is_string() && req["type"] == "PING") {

					json res = {{"type", "PONG"}, {"tcp_port", tcp_port}};
					std::string res_str = res.dump();

					if (!NET_SendDatagram(
						udp_sock.get(), packet->addr, packet->port,
						res_str.data(), static_cast<int>(res_str.size())
					)) {
						std::cerr << "Failed to send PONG: " << SDL_GetError() << std::endl;
					}
				}
			}

			while (true) {
				NET_StreamSocket *client = nullptr;
				if (!NET_AcceptClient(tcp_server.get(), &client)) {
					fatal_error = std::string("TCP accept: ") + SDL_GetError();
					break;
				}
				if (!client) break;

				// Reap threads before checking the connection limit.
				for (auto it = clients.begin(); it != clients.end();) {
					if (it->finished->load()) {
						it->thread.join();
						it = clients.erase(it);
					} else {
						++it;
					}
				}

				if (clients.size() >= MAX_CLIENTS) {
					std::cerr << "Worker busy, closing connection" << std::endl;
					NET_DestroyStreamSocket(client);
					continue;
				}

				auto finished = std::make_shared<std::atomic<bool>>(false);

				clients.push_back({
					std::thread([client, finished]() {
						try {
							process_job(client);
						} catch (const std::exception &e) {
							std::cerr << "Client error: " << e.what() << std::endl;
						}
						finished->store(true);
					}),
					finished
				});
			}

			SDL_Delay(10);
		}

		for (auto &entry : clients) {
			entry.thread.join();
		}

		throw std::runtime_error(fatal_error);
	} catch (const std::exception &e) {
		std::cerr << "Worker error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}

