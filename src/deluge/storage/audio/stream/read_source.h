#pragma once

#include "io/stream.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

extern "C" {
#include "libdeluge/types.h" // DelugeStatus
}

class Sample;

// The audio-stream module's read seam. See design spec §6/§7. A ReadSource pulls one FAT-cluster-sized
// block of a sample's on-card bytes into a caller buffer. Two impls, both first-class: StreamReadSource
// (normal playback, over deluge::io::Stream) and BlockReadSource (recorder read-back of a mid-write file,
// by physical sector address). The reconstruction core (a later phase) reads through this and stays pure.
namespace deluge::audio::stream {

class ReadSource {
public:
	virtual ~ReadSource() = default;

	// Read exactly dst.size() bytes for cluster `cluster_index` (byte offset = cluster_index << magnitude,
	// or physical sector, depending on the impl). Returns bytes read on success, or a DelugeStatus error.
	virtual std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) = 0;
};

// Normal playback / load path: reads via deluge::io::Stream::read_at at a cluster-aligned byte offset.
class StreamReadSource final : public ReadSource {
public:
	StreamReadSource(deluge::io::Stream& stream, uint8_t cluster_size_magnitude)
	    : stream_{stream}, cluster_size_magnitude_{cluster_size_magnitude} {}

	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	deluge::io::Stream& stream_;
	uint8_t cluster_size_magnitude_;
};

// Recorder read-back path: the sample has no open read stream (it's still being written), so read the
// physically-written sectors directly by the recorder-maintained per-cluster sdAddress. See §7.
class BlockReadSource final : public ReadSource {
public:
	explicit BlockReadSource(const Sample& sample) : sample_{sample} {}

	std::expected<uint32_t, DelugeStatus> read(uint32_t cluster_index, std::span<std::byte> dst) override;

private:
	const Sample& sample_;
};

// Selects the read source from the sample's backing state: an open read stream (normal, loaded-from-card
// sample) -> StreamReadSource; otherwise (a recording still being written) -> BlockReadSource. This is the
// single place the block-vs-stream decision is made — no caller branches on it.
std::unique_ptr<ReadSource> make_read_source(Sample& sample);

} // namespace deluge::audio::stream
