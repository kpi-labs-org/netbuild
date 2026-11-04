#include <iostream>
#include <vector>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <limits>

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include "include/protocol.hpp"
#include "include/common.hpp"

namespace fs = std::filesystem;

struct WorkerConfig {
	std::string host;
	uint16_t udp_port;
};

struct WorkerInfo {
	std::string host;
	uint16_t udp_port;
	uint16_t tcp_port;
	AddressPtr address;
};

uint16_t discover_worker(NET_Address *worker_addr, uint16_t udp_port) {
	DatagramPtr udp_sock(NET_CreateDatagramSocket(nullptr, 0, 0));

	if (!udp_sock) {
		throw std::runtime_error(std::string("UDP socket: ") + SDL_GetError());
	}

	json ping_req = {{"type", "PING"}};
	std::string ping_str = ping_req.dump();

	if (!NET_SendDatagram(
		udp_sock.get(), worker_addr, udp_port,
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

		if (packet && packet->port == udp_port &&
			NET_CompareAddresses(packet->addr, worker_addr) == 0) {
			std::string raw_json(
				reinterpret_cast<const char *>(packet->buf), packet->buflen
			);

			json pong_res = json::parse(raw_json, nullptr, false);

			if (pong_res.is_object() &&
				pong_res.contains("type") && pong_res["type"].is_string() &&
				pong_res["type"] == "PONG" &&
				pong_res.contains("tcp_port") && pong_res["tcp_port"].is_number_unsigned()) {

				uint64_t port = pong_res["tcp_port"].get<uint64_t>();

				if (port > 0 && port <= 65535) {
					return static_cast<uint16_t>(port);
				}
			}
		}

		SDL_Delay(10);
	}

	return 0;
}

void compile_file(
	NET_Address *worker_addr,
	uint16_t worker_tcp_port,
	const fs::path &source_file,
	const fs::path &object_file,
	const fs::path &temp_dir,
	int task_id
) {
	fs::path ii_file = temp_dir / ("task_" + std::to_string(task_id) + ".ii");

	int ret = run_process({
		"g++", "-E", "-x", "c++",
		source_file.string(), "-o", ii_file.string()
	});

	if (ret != 0) {
		throw std::runtime_error("Preprocessing failed: " + source_file.string());
	}

	std::vector<uint8_t> ii_data;

	if (!load_file(ii_file, ii_data, MAX_PAYLOAD_SIZE) || ii_data.empty()) {
		throw std::runtime_error("Failed to read preprocessed file: " + source_file.string());
	}

	{
		std::error_code ec;
		fs::remove(ii_file, ec);
	}

	StreamPtr tcp_client(NET_CreateClient(worker_addr, worker_tcp_port, 0));

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

	auto job_msg = serialize_tcp_message("JOB", task_id, ii_data);

	if (!write_exact(tcp_client.get(), job_msg.data(), job_msg.size())) {
		throw std::runtime_error(std::string("Failed to send JOB: ") + SDL_GetError());
	}

	// GCC may take longer than a regular socket read timeout.
	void *sockets[] = {tcp_client.get()};
	int ready = NET_WaitUntilInputAvailable(sockets, 1, 120000);

	if (ready <= 0) {
		throw std::runtime_error(ready == 0 ? "Compilation timed out" :
			std::string("Waiting for RESULT failed: ") + SDL_GetError());
	}

	Message response;
	std::string error;

	if (!read_tcp_message(tcp_client.get(), response, error)) {
		throw std::runtime_error("Failed to read response: " + error);
	}

	if (response.header["task_id"].get<int>() != task_id) {
		throw std::runtime_error("Unexpected task ID");
	}

	std::string type = response.header["type"].get<std::string>();

	if (type == "ERROR") {
		std::string message = "Compilation failed";

		if (response.header.contains("message") && response.header["message"].is_string()) {
			message = response.header["message"].get<std::string>();
		}

		throw std::runtime_error("Worker error: " + message);
	}

	if (type != "RESULT" || response.payload.empty()) {
		throw std::runtime_error("Invalid RESULT response");
	}

	if (!save_file(object_file, response.payload)) {
		throw std::runtime_error("Failed to save object file: " + object_file.string());
	}
}

int main(int argc, char *argv[]) {
	try {
		NetworkContext net;
		std::vector<fs::path> source_files;

		if (argc == 1) {
			source_files.push_back("main.cpp");
		} else {
			for (int i = 1; i < argc; ++i) {
				fs::path source = argv[i];

				if (source.extension() != ".cpp") {
					throw std::runtime_error("Expected .cpp file: " + source.string());
				}

				source_files.push_back(source);
			}
		}

		if (source_files.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
			throw std::runtime_error("Too many source files");
		}

		for (const auto &source : source_files) {
			if (!fs::is_regular_file(source)) {
				throw std::runtime_error("Source file not found: " + source.string());
			}
		}

		std::vector<WorkerConfig> configured_workers = {
			{"127.0.0.1", 41000},
			{"127.0.0.1", 41002}
		};

		std::vector<WorkerInfo> workers;

		for (const auto &config : configured_workers) {
			try {
				AddressPtr addr = resolve_address(config.host);
				uint16_t tcp_port = discover_worker(addr.get(), config.udp_port);

				if (tcp_port == 0) {
					std::cerr << "Worker unavailable: " << config.host
						<< ":" << config.udp_port << std::endl;
					continue;
				}

				std::cout << "Worker discovered: " << config.host
					<< " (TCP " << tcp_port << ")" << std::endl;

				workers.push_back({config.host, config.udp_port, tcp_port, std::move(addr)});
			} catch (const std::exception &e) {
				std::cerr << "Worker unavailable: " << config.host
					<< ":" << config.udp_port << " (" << e.what() << ")" << std::endl;
			}
		}

		if (workers.empty()) {
			throw std::runtime_error("No available workers");
		}

		std::cout << "NetBuild: " << source_files.size() << " translation unit(s), "
			<< workers.size() << " worker(s)" << std::endl;

		TempDir temp;
		std::vector<fs::path> object_files;

		for (size_t i = 0; i < source_files.size(); ++i) {
			object_files.push_back(temp.path() / ("task_" + std::to_string(i + 1) + ".o"));
		}

		std::atomic<size_t> next_task{0};
		std::atomic<bool> failed{false};
		std::mutex result_mutex;
		std::string first_error;
		std::vector<std::thread> threads;

		for (auto &worker : workers) {
			WorkerInfo *current = &worker;

			threads.emplace_back([&, current]() {
				while (!failed.load()) {
					size_t index = next_task.fetch_add(1);
					if (index >= source_files.size()) break;

					int task_id = static_cast<int>(index + 1);

					{
						std::lock_guard<std::mutex> lock(result_mutex);
						std::cout << "[" << task_id << "] " << current->host
							<< ":" << current->tcp_port << " compiling "
							<< source_files[index].string() << std::endl;
					}

					try {
						compile_file(
							current->address.get(), current->tcp_port,
							source_files[index], object_files[index], temp.path(), task_id
						);

						std::lock_guard<std::mutex> lock(result_mutex);
						std::cout << "[" << task_id << "] Completed" << std::endl;
					} catch (const std::exception &e) {
						std::lock_guard<std::mutex> lock(result_mutex);
						if (!failed.exchange(true)) {
							first_error = "Task " + std::to_string(task_id) + ": " + e.what();
						}
						break;
					}
				}
			});
		}

		for (auto &thread : threads) {
			thread.join();
		}

		if (failed) {
			throw std::runtime_error(first_error);
		}

		std::vector<std::string> link_args = {"g++"};
		for (const auto &object_file : object_files) {
			link_args.push_back(object_file.string());
		}
		link_args.push_back("-o");
		link_args.push_back("result_app");

		std::cout << "Linking " << object_files.size() << " object file(s)..." << std::endl;

		if (run_process(link_args) != 0) {
			throw std::runtime_error("Linking failed");
		}

		std::cout << "Build completed: ./result_app" << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "Coordinator error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}

