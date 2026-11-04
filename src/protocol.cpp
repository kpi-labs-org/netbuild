#include "include/protocol.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_endian.h>
#include <cstring>

std::vector<uint8_t> serialize_tcp_message(const std::string &type, 
		int task_id, 
		const std::vector<uint8_t> &payload, 
		const std::string &error_msg) {
	json j;
	j["type"] = type;
	j["payload_size"] = payload.size();

	if (task_id >= 0) {
		j["task_id"] = task_id;
	}
	if (!error_msg.empty()) {
		j["message"] = error_msg;
	}

	std::string json_str = j.dump();
	uint32_t header_size = static_cast<uint32_t>(json_str.size());
	uint32_t header_size_be = SDL_Swap32BE(header_size);

	std::vector<uint8_t> frame(sizeof(uint32_t) + header_size + payload.size());

	std::memcpy(frame.data(), &header_size_be, sizeof(uint32_t));
	std::memcpy(frame.data() + sizeof(uint32_t), json_str.data(), header_size);

	if (!payload.empty()) {
		std::memcpy(frame.data() + sizeof(uint32_t) + header_size, payload.data(), payload.size());
	}

	return frame;
}

bool read_exact(NET_StreamSocket *sock, void *buf, size_t len) {
	uint8_t *ptr = static_cast<uint8_t *>(buf);
	size_t total = 0;
	while (total < len) {
		int r = NET_ReadFromStreamSocket(sock, ptr + total, static_cast<int>(len - total));
		if (r < 0) {
			return false;
		}
		if (r == 0) {
			SDL_Delay(1);
			continue;
		}
		total += static_cast<size_t>(r);
	}
	return true;
}

bool write_exact(NET_StreamSocket *sock, const void *buf, size_t len) {
	if (!NET_WriteToStreamSocket(sock, buf, static_cast<int>(len))) {
		return false;
	}
	if (NET_WaitUntilStreamSocketDrained(sock, -1) < 0) {
		return false;
	}
	return true;
}
