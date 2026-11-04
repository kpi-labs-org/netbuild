#pragma once

#include <SDL3_net/SDL_net.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

using json = nlohmann::json;

constexpr uint16_t UDP_PORT = 41000;
constexpr uint16_t TCP_PORT = 41001;

constexpr int IO_TIMEOUT_MS = 15000;
constexpr int DISCOVERY_TIMEOUT_MS = 2000;

constexpr size_t MAX_HEADER_SIZE = 16 * 1024;
constexpr size_t MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;

struct Message {
	json header;
	std::vector<uint8_t> payload;
};

std::vector<uint8_t> serialize_tcp_message(
	const std::string &type,
	int task_id,
	const std::vector<uint8_t> &payload,
	const std::string &error_msg = ""
);

bool read_exact(NET_StreamSocket *sock, void *buf, size_t len, int timeout_ms = IO_TIMEOUT_MS);
bool write_exact(NET_StreamSocket *sock, const void *buf, size_t len, int timeout_ms = IO_TIMEOUT_MS);
bool read_tcp_message(NET_StreamSocket *sock, Message &message, std::string &error);
