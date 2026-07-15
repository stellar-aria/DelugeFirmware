#include "storage/audio/stream/read_source.h"
#include "model/sample/sample.h"

extern "C" {
#include "libdeluge/block_device.h"
}

namespace deluge::audio::stream {

std::expected<uint32_t, DelugeStatus> StreamReadSource::read(uint32_t clusterIndex, std::span<std::byte> dst) {
	auto result = stream_.read_at(clusterIndex << clusterSizeMagnitude_, dst);
	if (!result) {
		return std::unexpected(deluge::io::to_deluge_status(result.error()));
	}
	return static_cast<uint32_t>(result->size());
}

std::expected<uint32_t, DelugeStatus> BlockReadSource::read(uint32_t clusterIndex, std::span<std::byte> dst) {
	uint32_t numSectors = static_cast<uint32_t>(dst.size()) >> 9;
	DelugeStatus status = deluge_block_read(deluge_block_sd_unit(), reinterpret_cast<uint8_t*>(dst.data()),
	                                        sample_.clusters[clusterIndex].sdAddress, numSectors);
	if (status != DELUGE_OK) {
		return std::unexpected(status);
	}
	return static_cast<uint32_t>(numSectors) * 512u;
}

} // namespace deluge::audio::stream
