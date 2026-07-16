#pragma once

#include "io/stream.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

extern "C" {
#include "libdeluge/types.h" // DelugeStatus
}

class Sample;

namespace deluge::audio::stream {

/// @brief The audio-stream module's read seam (design spec §6/§7).
///
/// A ReadSource pulls one FAT-cluster-sized block of a sample's on-card bytes into a caller buffer.
/// Two impls, both first-class: StreamReadSource (normal playback, over deluge::io::Stream) and
/// BlockReadSource (recorder read-back of a mid-write file, by physical sector address). The
/// reconstruction core reads through this and stays pure.
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

/// @brief Normal playback/load path: reads via deluge::io::Stream::read_at at a cluster-aligned byte offset.
class StreamReadSource final : public ReadSource {
public:
	StreamReadSource(deluge::io::Stream& stream, uint8_t cluster_size_magnitude)
	    : stream_{stream}, cluster_size_magnitude_{cluster_size_magnitude} {}

	/// @copydoc ReadSource::read
	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	deluge::io::Stream& stream_;
	uint8_t cluster_size_magnitude_;
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

// The block-vs-stream ReadSource selection lives on deluge::audio::stream::SampleStream
// (SampleStream::make_read_source(), storage/audio/stream/sample_stream.h) -- it owns the Sample's
// read-stream handle, so it's the only place that can make the selection without a caller branching on
// Sample internals.

} // namespace deluge::audio::stream
