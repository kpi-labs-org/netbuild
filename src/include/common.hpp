#pragma once

#include <SDL3/SDL.h>
#include <SDL3_net/SDL_net.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct NetworkContext {
	NetworkContext() {
		if (!SDL_Init(0)) {
			throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
		}

		if (!NET_Init()) {
			std::string error = SDL_GetError();
			SDL_Quit();
			throw std::runtime_error("NET_Init: " + error);
		}
	}

	~NetworkContext() {
		NET_Quit();
		SDL_Quit();
	}

	NetworkContext(const NetworkContext &) = delete;
	NetworkContext &operator=(const NetworkContext &) = delete;
};

struct StreamDeleter {
	void operator()(NET_StreamSocket *sock) const {
		if (sock) NET_DestroyStreamSocket(sock);
	}
};

struct DatagramDeleter {
	void operator()(NET_DatagramSocket *sock) const {
		if (sock) NET_DestroyDatagramSocket(sock);
	}
};

struct ServerDeleter {
	void operator()(NET_Server *server) const {
		if (server) NET_DestroyServer(server);
	}
};

struct AddressDeleter {
	void operator()(NET_Address *addr) const {
		if (addr) NET_UnrefAddress(addr);
	}
};

struct PacketDeleter {
	void operator()(NET_Datagram *packet) const {
		if (packet) NET_DestroyDatagram(packet);
	}
};

using StreamPtr = std::unique_ptr<NET_StreamSocket, StreamDeleter>;
using DatagramPtr = std::unique_ptr<NET_DatagramSocket, DatagramDeleter>;
using ServerPtr = std::unique_ptr<NET_Server, ServerDeleter>;
using AddressPtr = std::unique_ptr<NET_Address, AddressDeleter>;
using PacketPtr = std::unique_ptr<NET_Datagram, PacketDeleter>;

inline AddressPtr resolve_address(const std::string &host) {
	AddressPtr addr(NET_ResolveHostname(host.c_str()));

	if (!addr) {
		throw std::runtime_error(std::string("NET_ResolveHostname: ") + SDL_GetError());
	}

	NET_Status status = NET_WaitUntilResolved(addr.get(), 5000);

	if (status != NET_SUCCESS) {
		if (status == NET_WAITING) {
			throw std::runtime_error("Address resolution timed out");
		}

		throw std::runtime_error(SDL_GetError());
	}

	return addr;
}

class TempDir {
	std::filesystem::path dir;

public:
	TempDir() {
		std::string name = (
			std::filesystem::temp_directory_path() / "netbuild-XXXXXX"
		).string();

		std::vector<char> writable(name.begin(), name.end());
		writable.push_back('\0');

		char *created = mkdtemp(writable.data());

		if (!created) {
			throw std::runtime_error(std::string("mkdtemp: ") + std::strerror(errno));
		}

		dir = created;
	}

	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}

	TempDir(const TempDir &) = delete;
	TempDir &operator=(const TempDir &) = delete;

	const std::filesystem::path &path() const {
		return dir;
	}
};

inline int run_process(const std::vector<std::string> &args) {
	if (args.empty()) return -1;

	std::vector<char *> argv;
	for (const auto &arg : args) {
		argv.push_back(const_cast<char *>(arg.c_str()));
	}
	argv.push_back(nullptr);

	pid_t pid = 0;
	int spawn_error = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);

	if (spawn_error != 0) {
		std::cerr << "posix_spawnp failed: " << std::strerror(spawn_error) << std::endl;
		return -1;
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			std::cerr << "waitpid failed: " << std::strerror(errno) << std::endl;
			return -1;
		}
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}

	return -1;
}

inline bool load_file(
	const std::filesystem::path &path,
	std::vector<uint8_t> &data,
	size_t max_size
) {
	std::error_code ec;
	uintmax_t file_size = std::filesystem::file_size(path, ec);

	if (ec || file_size > max_size) return false;

	std::ifstream ifs(path, std::ios::binary);
	if (!ifs.is_open()) return false;

	data.resize(static_cast<size_t>(file_size));

	if (!data.empty()) {
		ifs.read(reinterpret_cast<char *>(data.data()), data.size());
		if (!ifs) return false;
	}

	return true;
}

inline bool save_file(
	const std::filesystem::path &path,
	const std::vector<uint8_t> &data
) {
	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	if (!ofs.is_open()) return false;

	if (!data.empty()) {
		ofs.write(reinterpret_cast<const char *>(data.data()), data.size());
	}

	ofs.close();
	return static_cast<bool>(ofs);
}
