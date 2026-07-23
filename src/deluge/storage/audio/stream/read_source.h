#pragma once

#include "io/stream.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

extern "C" {
#include "libdeluge/streaming_fill.h"
#include "libdeluge/types.h" // DelugeStatus
}

namespace deluge::audio::stream {

/// @brief The audio-stream module's read seam (design spec §6/§7).
///
/// A ReadSource pulls one FAT-cluster-sized block of a sample's on-card bytes into a caller buffer.
/// Two impls, both first-class: EfatfsReadSource (streaming read via the embedded-fatfs handle, the
/// R1 read path) and RecordingReadSource (recorder read-back of a mid-write file, via the recorder's
/// own open efatfs write context, R3). The reconstruction core reads through this and stays pure.
class ReadSource {
public:
	virtual ~ReadSource() = default;

	/// @brief Read exactly dst.size() bytes for the given cluster.
	///
	/// @param cluster_index Cluster index; the byte offset is cluster_index << magnitude.
	/// @param dst           Destination buffer; exactly dst.size() bytes are read on success.
	/// @return Bytes read on success, or a DelugeStatus error.
	virtual std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) = 0;
};

/// @brief Recorder read-back path (R3): the sample has no open read stream (it's still being
///        recorded), so this reads the evicted cluster back through the recorder's OWN OPEN efatfs
///        write context (`deluge::io::Stream::read_at_via`, which composes
///        `deluge_efatfs_stream_read_at_via`). This works because the write context's IN-MEMORY size
///        reflects the written extent even though the on-disk directory entry is stale until the
///        recording closes -- see design spec §7 and stream_io.h's doc.
///
/// @note Holds a POINTER to the recorder's `std::optional<deluge::io::Stream>` member (not a
///       reference), because that optional gets reset()/re-emplaced across the recording's several
///       open/close windows (initial open, mid-alteration reopen, final close) -- its own address is
///       stable for the SampleRecorder's whole lifetime, but its contained Stream is not always
///       engaged. read() treats "no stream" or "not open" as a plain read failure, not UB.
class RecordingReadSource final : public ReadSource {
public:
	explicit RecordingReadSource(std::optional<deluge::io::Stream>* write_stream, uint8_t cluster_size_magnitude)
	    : write_stream_{write_stream}, cluster_size_magnitude_{cluster_size_magnitude} {}

	/// @copydoc ReadSource::read
	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	std::optional<deluge::io::Stream>* write_stream_;
	uint8_t cluster_size_magnitude_;
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

// The stream-vs-recording ReadSource selection lives on deluge::audio::stream::SampleStream
// (SampleStream::make_read_source(), storage/audio/stream/sample_stream.h) -- it owns the Sample's
// read-stream handle, so it's the only place that can make the selection without a caller branching on
// Sample internals.

} // namespace deluge::audio::stream
