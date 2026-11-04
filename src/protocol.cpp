#include "include/protocol.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_endian.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <stdexcept>

using clock_type = std::chrono::steady_clock;

int remaining_ms(clock_type::time_point deadline) {
	auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
		deadline - clock_type::now()
	).count();

	if (left <= 0) return 0;

	return static_cast<int>(
		std::min<long long>(left, std::numeric_limits<int>::max())
	);
}

std::vector<uint8_t> serialize_tcp_message(
	const std::string &type,
	int task_id,
	const std::vector<uint8_t> &payload,
	const std::string &error_msg
) {
	if (payload.size() > MAX_PAYLOAD_SIZE) {
		throw std::runtime_error("Payload exceeds limit");
	}

	json j = {
		{"type", type},
		{"payload_size", payload.size()}
	};

	if (task_id >= 0) {
		j["task_id"] = task_id;
	}

	if (!error_msg.empty()) {
		j["message"] = error_msg;
	}

	std::string json_str = j.dump();

	if (json_str.empty() || json_str.size() > MAX_HEADER_SIZE) {
		throw std::runtime_error("JSON header exceeds limit");
	}

	uint32_t header_size = static_cast<uint32_t>(json_str.size());
	uint32_t header_size_be = SDL_Swap32BE(header_size);

	std::vector<uint8_t> frame(sizeof(uint32_t) + header_size + payload.size());

	std::memcpy(frame.data(), &header_size_be, sizeof(uint32_t));
	std::memcpy(frame.data() + sizeof(uint32_t), json_str.data(), header_size);

	if (!payload.empty()) {
		std::memcpy(
			frame.data() + sizeof(uint32_t) + header_size,
			payload.data(),
			payload.size()
		);
	}

	return frame;
}

bool read_exact(NET_StreamSocket *sock, void *buf, size_t len, int timeout_ms) {
	if (!sock || (!buf && len != 0) || timeout_ms < 0) return false;

	uint8_t *ptr = static_cast<uint8_t *>(buf);
	size_t total = 0;

	auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);

	while (total < len) {
		int chunk = static_cast<int>(
			std::min<size_t>(len - total, 64 * 1024)
		);

		int r = NET_ReadFromStreamSocket(sock, ptr + total, chunk);

		if (r < 0) return false;

		if (r > 0) {
			total += static_cast<size_t>(r);
			continue;
		}

		int left = remaining_ms(deadline);
		if (left == 0) return false;

		void *sockets[] = {sock};

		int wait_result = NET_WaitUntilInputAvailable(
			sockets, 1, std::min(left, 50)
		);

		if (wait_result < 0) return false;
	}

	return true;
}

bool write_exact(NET_StreamSocket *sock, const void *buf, size_t len, int timeout_ms) {
	if (!sock || (!buf && len != 0) || timeout_ms < 0) return false;

	const uint8_t *ptr = static_cast<const uint8_t *>(buf);
	size_t total = 0;

	auto deadline = clock_type::now() + std::chrono::milliseconds(timeout_ms);

	while (total < len) {
		int chunk = static_cast<int>(
			std::min<size_t>(len - total, 64 * 1024)
		);

		if (!NET_WriteToStreamSocket(sock, ptr + total, chunk)) {
			return false;
		}

		total += static_cast<size_t>(chunk);

		int pending = NET_GetStreamSocketPendingWrites(sock);
		if (pending < 0) return false;

		if (pending > 256 * 1024) {
			int left = remaining_ms(deadline);

			if (left == 0) return false;

			if (NET_WaitUntilStreamSocketDrained(sock, left) != 0) {
				return false;
			}
		}
	}

	int left = remaining_ms(deadline);

	return left > 0 && NET_WaitUntilStreamSocketDrained(sock, left) == 0;
}

bool read_tcp_message(NET_StreamSocket *sock, Message &message, std::string &error) {
	message = {};
	error.clear();

	uint32_t header_size_be = 0;

	if (!read_exact(sock, &header_size_be, sizeof(uint32_t))) {
		error = "Failed to read frame length";
		return false;
	}

	uint32_t header_size = SDL_Swap32BE(header_size_be);

	if (header_size == 0 || header_size > MAX_HEADER_SIZE) {
		error = "Invalid header size";
		return false;
	}

	std::string json_data(header_size, '\0');

	if (!read_exact(sock, json_data.data(), header_size)) {
		error = "Failed to read JSON header";
		return false;
	}

	message.header = json::parse(json_data, nullptr, false);

	if (!message.header.is_object()) {
		error = "Invalid JSON header";
		return false;
	}

	const json &header = message.header;

	if (!header.contains("type") || !header["type"].is_string() ||
		!header.contains("task_id") || !header["task_id"].is_number_integer() ||
		!header.contains("payload_size") || !header["payload_size"].is_number_unsigned()) {
		error = "Missing or invalid header fields";
		return false;
	}

	int64_t task_id = header["task_id"].get<int64_t>();

	if (task_id < 0 || task_id > std::numeric_limits<int>::max()) {
		error = "Invalid task ID";
		return false;
	}

	uint64_t payload_size = header["payload_size"].get<uint64_t>();

	if (payload_size > MAX_PAYLOAD_SIZE) {
		error = "Payload exceeds limit";
		return false;
	}

	message.payload.resize(static_cast<size_t>(payload_size));

	if (payload_size > 0) {
		if (!read_exact(sock, message.payload.data(), message.payload.size())) {
			error = "Failed to read payload";
			return false;
		}
	}

	return true;
}
