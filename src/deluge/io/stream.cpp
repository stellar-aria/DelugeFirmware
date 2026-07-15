#include "io/stream.hpp"

namespace deluge::io {

std::expected<Stream, Status> Stream::open(std::string_view path, DelugeStreamMode mode) {
	DelugeStream* handle = nullptr;
	DelugeStatus status = deluge_stream_open(path.data(), mode, &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return Stream(handle);
}

std::expected<std::span<std::byte>, Status> Stream::read_at(uint32_t byte_offset, std::span<std::byte> buffer) {
	uint32_t out_read = 0;
	DelugeStatus status =
	    deluge_stream_read_at(handle_, byte_offset, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_read);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return buffer.subspan(0, out_read);
}

std::expected<uint32_t, Status> Stream::write_at(uint32_t byte_offset, std::span<const std::byte> buffer) {
	uint32_t out_written = 0;
	DelugeStatus status =
	    deluge_stream_write_at(handle_, byte_offset, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_written);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_written;
}

std::expected<void, Status> Stream::truncate(uint32_t new_size) {
	DelugeStatus status = deluge_stream_truncate(handle_, new_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<uint32_t, Status> Stream::size() {
	uint32_t out_size = 0;
	DelugeStatus status = deluge_stream_size(handle_, &out_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_size;
}

std::expected<uint32_t, Status> Stream::sector_of(uint32_t cluster_index) {
	uint32_t out_sector = 0;
	DelugeStatus status = deluge_stream_sector_of(handle_, cluster_index, &out_sector);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_sector;
}

std::expected<void, Status> Stream::close() {
	DelugeStatus status = deluge_stream_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

} // namespace deluge::io
