#include "storage/audio/stream/read_source.h"
#include "model/sample/sample.h"

extern "C" {
#include "libdeluge/block_device.h"
}

namespace deluge::audio::stream {

std::expected<uint32_t, DelugeStatus> StreamReadSource::read(uint32_t cluster_index, std::span<std::byte> dst) {
	auto result = stream_.read_at(cluster_index << cluster_size_magnitude_, dst);
	if (!result) {
		return std::unexpected(deluge::io::to_deluge_status(result.error()));
	}
	return static_cast<uint32_t>(result->size());
}

std::expected<uint32_t, DelugeStatus> BlockReadSource::read(uint32_t cluster_index, std::span<std::byte> dst) {
	uint32_t num_sectors = static_cast<uint32_t>(dst.size()) >> 9;
	DelugeStatus status = deluge_block_read(deluge_block_sd_unit(), reinterpret_cast<uint8_t*>(dst.data()),
	                                        sample_.clusters[cluster_index].sdAddress, num_sectors);
	if (status != DELUGE_OK) {
		return std::unexpected(status);
	}
	return static_cast<uint32_t>(num_sectors) * 512u;
}

} // namespace deluge::audio::stream
