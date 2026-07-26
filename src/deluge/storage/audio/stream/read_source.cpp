#include "storage/audio/stream/read_source.h"

namespace deluge::audio::stream {

std::expected<uint32_t, DelugeStatus> EfatfsReadSource::read(uint32_t cluster_index, std::span<std::byte> dst) {
	uint32_t out_read = 0;
	uint32_t byte_offset = cluster_index << cluster_size_magnitude_;
	if (!deluge_efatfs_read_at(handle_, byte_offset, dst.data(), static_cast<uint32_t>(dst.size()), &out_read)) {
		return std::unexpected(DELUGE_ERR_IO);
	}
	return out_read;
}

} // namespace deluge::audio::stream
