#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::audio::stream {

// Edge of the PREVIOUS cluster: a mutable view of prevCluster->data over [cluster_size-4, cluster_size+7)
// (11 bytes) — so tail[4+k] == prevCluster->data[cluster_size+k], tail[0..4) == prevCluster->data[size-4..size).
// `end_boundary_converted` points at prevCluster->extraBytesAtEndConverted.
struct StitchPrevEdge {
	std::span<std::byte> tail; // 11 bytes: prevCluster->data[cluster_size-4 .. cluster_size+7)
	bool* end_boundary_converted;
};

// Edge of the NEXT cluster: a mutable view of nextCluster->data[0..7), plus a read-only view of
// nextCluster->firstThreeBytesPreDataConversion[0..3). `start_boundary_converted` points at
// nextCluster->extraBytesAtStartConverted.
struct StitchNextEdge {
	std::span<std::byte> head;                      // 7 bytes: nextCluster->data[0..7)
	std::span<const std::byte, 3> unconverted_head; // nextCluster->firstThreeBytesPreDataConversion
	bool* start_boundary_converted;
};

// Pure boundary stitch for ONE cluster, matching audio_file_manager.cpp:1044-1223. Mutates `self_data`
// (this cluster's data + overhang; must be cluster_size+7 bytes usable), the neighbor edge spans, and
// the four flags, all in place. `prev`/`next` are nullptr when that neighbor is absent-or-not-loaded
// (the caller only supplies a loaded neighbor). No Cluster/Sample/AudioEngine reach.
void stitch_boundaries(std::span<std::byte> self_data, int32_t cluster_index, RawDataFormat format,
                       uint32_t audio_data_start_pos_bytes, size_t cluster_size, bool& self_start_boundary_converted,
                       bool& self_end_boundary_converted, StitchPrevEdge* prev, StitchNextEdge* next);

} // namespace deluge::audio::stream
