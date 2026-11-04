#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <filesystem>
#include <stdexcept>

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include "include/protocol.hpp"
#include "include/common.hpp"

namespace fs = std::filesystem;

uint16_t discover_worker(NET_Address *worker_addr) {
	DatagramPtr udp_sock(
		NET_CreateDatagramSocket(nullptr, 0, 0)
	);

	if (!udp_sock) {
		throw std::runtime_error(std::string("UDP socket: ") + SDL_GetError());
	}

	json ping_req = {{"type", "PING"}};
	std::string ping_str = ping_req.dump();

	if (!NET_SendDatagram(
		udp_sock.get(), worker_addr, UDP_PORT,
		ping_str.data(), static_cast<int>(ping_str.size())
	)) {
		throw std::runtime_error(std::string("Failed to send PING: ") + SDL_GetError());
	}

	auto deadline = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(DISCOVERY_TIMEOUT_MS);

	while (std::chrono::steady_clock::now() < deadline) {
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

			json pong_res = json::parse(raw_json, nullptr, false);

			if (pong_res.is_object() &&
				pong_res.contains("type") &&
				pong_res["type"].is_string() &&
				pong_res["type"] == "PONG" &&
				pong_res.contains("tcp_port") &&
				pong_res["tcp_port"].is_number_unsigned()) {

				uint64_t port = pong_res["tcp_port"].get<uint64_t>();

				if (port > 0 && port <= 65535) {
					return static_cast<uint16_t>(port);
				}
			}
		}

		SDL_Delay(10);
	}

	throw std::runtime_error("No available worker");
}

int main() {
	try {
		NetworkContext net;

		std::string worker_ip = "127.0.0.1";
		AddressPtr worker_addr = resolve_address(worker_ip);

		uint16_t worker_tcp_port = discover_worker(worker_addr.get());

		std::cout << "Worker discovered. TCP port: "
			<< worker_tcp_port << std::endl;

		TempDir temp;

		fs::path ii_file = temp.path() / "main.ii";
		fs::path obj_file = temp.path() / "main.o";

		std::cout << "Preprocessing main.cpp..." << std::endl;

		int ret = run_process({
			"g++", "-E", "-x", "c++",
			"main.cpp", "-o", ii_file.string()
		});

		if (ret != 0) {
			throw std::runtime_error("Preprocessing failed");
		}

		std::vector<uint8_t> ii_data;

		if (!load_file(ii_file, ii_data, MAX_PAYLOAD_SIZE) || ii_data.empty()) {
			throw std::runtime_error("Failed to read preprocessed file");
		}

		StreamPtr tcp_client(
			NET_CreateClient(worker_addr.get(), worker_tcp_port, 0)
		);

		if (!tcp_client) {
			throw std::runtime_error(std::string("TCP socket: ") + SDL_GetError());
		}

		NET_Status status = NET_WaitUntilConnected(tcp_client.get(), 5000);

		if (status != NET_SUCCESS) {
			if (status == NET_WAITING) {
				throw std::runtime_error("TCP connection timed out");
			}

			throw std::runtime_error(std::string("TCP connection failed: ") + SDL_GetError());
		}

		auto job_msg = serialize_tcp_message("JOB", 1, ii_data);

		if (!write_exact(tcp_client.get(), job_msg.data(), job_msg.size())) {
			throw std::runtime_error(std::string("Failed to send JOB: ") + SDL_GetError());
		}

		std::cout << "JOB sent (" << ii_data.size() << " bytes)" << std::endl;

		Message response;
		std::string error;

		if (!read_tcp_message(tcp_client.get(), response, error)) {
			throw std::runtime_error("Failed to read response: " + error);
		}

		if (response.header["task_id"] != 1) {
			throw std::runtime_error("Unexpected task ID");
		}

		std::string type = response.header["type"].get<std::string>();

		if (type == "ERROR") {
			std::string message = "Compilation failed";

			if (response.header.contains("message") &&
				response.header["message"].is_string()) {
				message = response.header["message"].get<std::string>();
			}

			throw std::runtime_error("Worker error: " + message);
		}

		if (type != "RESULT" || response.payload.empty()) {
			throw std::runtime_error("Invalid RESULT response");
		}

		if (!save_file(obj_file, response.payload)) {
			throw std::runtime_error("Failed to save object file");
		}

		// Release the TCP connection before linking.
		tcp_client.reset();

		std::cout << "Linking..." << std::endl;

		ret = run_process({
			"g++", obj_file.string(), "-o", "result_app"
		});

		if (ret != 0) {
			throw std::runtime_error("Linking failed");
		}

		std::cout << "Build completed: ./result_app" << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "Coordinator error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
