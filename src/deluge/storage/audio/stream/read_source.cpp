#include "storage/audio/stream/read_source.h"
#include "io/file.hpp" // deluge::io::to_deluge_status

namespace deluge::audio::stream {

std::expected<uint32_t, DelugeStatus> RecordingReadSource::read(uint32_t cluster_index, std::span<std::byte> dst) {
	if (write_stream_ == nullptr || !write_stream_->has_value()) {
		return std::unexpected(DELUGE_ERR_IO); // recording finished / write context not open
	}
	uint32_t byte_offset = cluster_index << cluster_size_magnitude_;
	auto readResult = write_stream_->value().read_at_via(byte_offset, dst);
	if (!readResult) {
		return std::unexpected(deluge::io::to_deluge_status(readResult.error()));
	}
	return readResult.value();
}

std::expected<uint32_t, DelugeStatus> EfatfsReadSource::read(uint32_t cluster_index, std::span<std::byte> dst) {
	uint32_t out_read = 0;
	uint32_t byte_offset = cluster_index << cluster_size_magnitude_;
	if (!deluge_efatfs_read_at(handle_, byte_offset, dst.data(), static_cast<uint32_t>(dst.size()), &out_read)) {
		return std::unexpected(DELUGE_ERR_IO);
	}
	return out_read;
}

} // namespace deluge::audio::stream
