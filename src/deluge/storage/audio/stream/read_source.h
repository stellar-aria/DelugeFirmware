#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

extern "C" {
#include "libdeluge/streaming_fill.h"
#include "libdeluge/types.h" // DelugeStatus
}

class Sample;

namespace deluge::audio::stream {

/// @brief The audio-stream module's read seam (design spec §6/§7).
///
/// A ReadSource pulls one FAT-cluster-sized block of a sample's on-card bytes into a caller buffer.
/// Two impls, both first-class: EfatfsReadSource (streaming read via the embedded-fatfs handle, the
/// R1 read path) and BlockReadSource (recorder read-back of a mid-write file, by physical sector
/// address). The reconstruction core reads through this and stays pure.
class ReadSource {
public:
	virtual ~ReadSource() = default;

	/// @brief Read exactly dst.size() bytes for the given cluster.
	///
	/// @param cluster_index Cluster index; interpreted per-impl as a byte offset
	///                      (cluster_index << magnitude) or a physical sector address.
	/// @param dst           Destination buffer; exactly dst.size() bytes are read on success.
	/// @return Bytes read on success, or a DelugeStatus error.
	virtual std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) = 0;
};

/// @brief Recorder read-back path: the sample has no open read stream (it's still being written), so this
///        reads the physically-written sectors directly by the recorder-maintained per-cluster sdAddress.
///        See design spec §7.
class BlockReadSource final : public ReadSource {
public:
	explicit BlockReadSource(const Sample& sample) : sample_{sample} {}

	/// @copydoc ReadSource::read
	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	const Sample& sample_;
};

/// @brief Streaming read path (R1): reads via the embedded-fatfs handle at a cluster-aligned byte
///        offset (deluge_efatfs_read_at). Selected when the sample has an open efatfs handle.
class EfatfsReadSource final : public ReadSource {
public:
	EfatfsReadSource(uint32_t handle, uint8_t cluster_size_magnitude)
	    : handle_{handle}, cluster_size_magnitude_{cluster_size_magnitude} {}

	/// @copydoc ReadSource::read
	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	uint32_t handle_;
	uint8_t cluster_size_magnitude_;
};

// The block-vs-stream ReadSource selection lives on deluge::audio::stream::SampleStream
// (SampleStream::make_read_source(), storage/audio/stream/sample_stream.h) -- it owns the Sample's
// read-stream handle, so it's the only place that can make the selection without a caller branching on
// Sample internals.

} // namespace deluge::audio::stream
