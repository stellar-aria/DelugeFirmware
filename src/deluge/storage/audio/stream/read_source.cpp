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
	                                        sample_.stream().sd_address_at(cluster_index), num_sectors);
	if (status != DELUGE_OK) {
		return std::unexpected(status);
	}
	return static_cast<uint32_t>(num_sectors) * 512u;
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
