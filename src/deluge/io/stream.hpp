#pragma once

#include "io/file.hpp" // reuses deluge::io::Status / to_status / to_deluge_status
#include "libdeluge/stream_io.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace deluge::io {

class Stream {
public:
	Stream(Stream&) = delete;
	Stream(Stream&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	Stream& operator=(Stream&) = delete;
	Stream& operator=(Stream&& other) noexcept {
		if (this != &other) {
			if (handle_) {
				deluge_stream_close(handle_);
			}
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~Stream() {
		if (handle_) {
			deluge_stream_close(handle_);
		}
	}

	[[nodiscard]] static std::expected<Stream, Status> open(std::string_view path, DelugeStreamMode mode);
	std::expected<std::span<std::byte>, Status> read_at(uint32_t byte_offset, std::span<std::byte> buffer);
	std::expected<uint32_t, Status> write_at(uint32_t byte_offset, std::span<const std::byte> buffer);
	std::expected<void, Status> truncate(uint32_t new_size);
	std::expected<uint32_t, Status> size();
	std::expected<uint32_t, Status> sector_of(uint32_t cluster_index);
	std::expected<void, Status> close();

private:
	explicit Stream(DelugeStream* handle) : handle_(handle) {}
	DelugeStream* handle_ = nullptr;
};

} // namespace deluge::io
