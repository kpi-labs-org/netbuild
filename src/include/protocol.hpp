#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <SDL3_net/SDL_net.h>

using json = nlohmann::json;

struct tcp_message {
	json header;
	std::vector<uint8_t> payload;
};

std::vector<uint8_t> serialize_tcp_message(const std::string &type, 
		int task_id, 
		const std::vector<uint8_t> &payload, 
		const std::string &error_msg = "");

bool read_exact(NET_StreamSocket *sock, void *buf, size_t len);
bool write_exact(NET_StreamSocket *sock, const void *buf, size_t len);
