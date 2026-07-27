// U1 Task 5's real-chunk harness + the current-read-path ORACLE. Two things live here:
//
//  1. Real `StreamedChunk` construct/accessors (`region_read_diff_chunk_construct`,
//     `deluge_streaming_chunk_payload`/`_set_loaded`/`_convert_state`/`_set_convert_state`,
//     `deluge_streaming_resource_manager`) — a straight copy of `region_fill_differential`'s
//     `cpp/native_finish_shim.cpp`, which already proved this is the minimal real seam
//     `deluge_sample_fill::native_begin`/`native_finish` need (see that file's own module doc for
//     the "why not compile async_fill.cpp itself" reasoning — identical here: `deluge_sample_reader`
//     drives the SAME `native_begin`/`native_finish` internally, via `Reader::fill_now`). The four
//     accessor bodies are, again, character-for-character identical to `async_fill.cpp`'s own
//     definitions, guarded by `tests/differential.rs`'s own
//     `accessor_bodies_match_async_fill_cpp_verbatim` test.
//
//  2. `region_read_diff_frame_direct`/`region_read_diff_frame_via_origin` — the DIFFERENTIAL'S OWN
//     oracle: the current non-voice read path's frame->bytes mapping, reproduced from the real,
//     unmodified `StreamedChunk::frame_read_origin` (`cluster.h`) and `payload_with_trailing_slack()`
//     — not re-derived by hand. `frame_read_origin(pos, byte_depth)` returns
//     `payload().data() + pos - 4 + byte_depth`; the REAL byte_depth-byte sample at absolute
//     within-cluster position `pos` is the LAST `byte_depth` bytes of that 4-byte window (see the
//     real production callers, `sample.cpp`/`voice_sample.cpp`/`time_stretcher.cpp`, which all read
//     it as a little-endian 32-bit word and keep only the high `byte_depth` bytes) — i.e.
//     `frame_read_origin(pos, byte_depth) + (4 - byte_depth) == payload().data() + pos`. For a
//     multi-channel frame, each channel's own `byte_depth`-byte sample sits at `pos + channel *
//     byte_depth` (see `sample.cpp`'s own two-channel perc-cache read, which advances the SAME
//     origin pointer by `byteDepth` for the second channel) — so a whole interleaved frame's bytes
//     are simply `payload_with_trailing_slack()[pos, pos + byte_depth*num_channels)`.
//     `region_read_diff_frame_direct` reads that span directly (generalizes to any channel count);
//     `region_read_diff_frame_via_origin` reads it PER CHANNEL through the real `frame_read_origin`
//     call, literally exercising the production function. `tests/differential.rs`'s
//     `direct_oracle_matches_frame_read_origin_oracle` proves the two agree, so the differential's
//     bulk cases can use the simpler, channel-count-agnostic `_direct` form while staying tied, by
//     proof, to the real `frame_read_origin`.
#include "storage/cluster/cluster.h"

#include "libdeluge/streaming_fill.h" // the real DelugeChunkConvertState C-ABI type

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Out-of-class definitions for Cluster's static data members (declared, not defined, in cluster.h;
// normally defined in cluster.cpp, which this shim deliberately does not compile -- see
// region_fill_differential's native_finish_shim.cpp for why). payload()/payload_with_trailing_slack()
// read Cluster::size at call time, so this must be set (via region_read_diff_set_cluster_size below)
// before any chunk backing this shim constructs is used.
size_t Cluster::size = 0;
size_t Cluster::size_magnitude = 0;

extern "C" {

/// Set the session cluster size, mirroring what `Cluster::set_size()` (cluster.cpp, not compiled
/// here) would do at boot.
void region_read_diff_set_cluster_size(size_t size, size_t magnitude) {
	Cluster::size = size;
	Cluster::size_magnitude = magnitude;
}

/// Byte offset of a chunk's payload from its slab-slot/backing base -- `kChunkPayloadOffset`
/// (cluster.h), the REAL compiler-computed value. Always nonzero -- the property that makes a
/// "backing pointer used as payload" bug detectable by seeding distinguishable bytes in each
/// region (this harness never seeds payload == backing -- see `ChunkHarness::seed`).
size_t region_read_diff_chunk_payload_offset(void) {
	return kChunkPayloadOffset;
}

/// Total backing-allocation size for one chunk of `cluster_size` bytes: header + front guard
/// (`kChunkPayloadOffset`) + payload (`cluster_size`) + trailing guard (`kChunkTrailingGuard`).
size_t region_read_diff_chunk_backing_size(size_t cluster_size) {
	return kChunkPayloadOffset + cluster_size + kChunkTrailingGuard;
}

/// Placement-news a real `StreamedChunk` at `dest`, mirroring `SampleStream::cluster_construct`
/// minus the `sample`/`resource_slot` field writes this harness's calls never dereference. Signature
/// matches `deluge_resource::ConstructFn` exactly (`ctx, owner, index, dest`).
void region_read_diff_chunk_construct(void* /*ctx*/, void* /*owner*/, uint32_t index, void* dest) {
	auto* cluster = new (dest) StreamedChunk();
	cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kChunkPayloadOffset; // slot-provenance payload
	cluster->cluster_index = index;
}

static void* g_active_manager = nullptr;

/// Set the "one process-wide resource manager" `deluge_streaming_resource_manager` below returns.
/// Must be called before any `deluge_resource_*`/`native_begin`/`native_finish`/reader call.
void region_read_diff_set_active_manager(void* mgr) {
	g_active_manager = mgr;
}

DelugeResource* deluge_streaming_resource_manager(void) {
	return reinterpret_cast<DelugeResource*>(g_active_manager);
}

// -- The four accessors `deluge_sample_fill::native`'s `unsafe extern "C"` block needs --
// character-for-character identical to async_fill.cpp's own bodies (verified by
// `tests/differential.rs`'s `accessor_bodies_match_async_fill_cpp_verbatim` test).

uint8_t* deluge_streaming_chunk_payload(void* chunk_backing) {
	return reinterpret_cast<uint8_t*>(reinterpret_cast<StreamedChunk*>(chunk_backing)->payload().data());
}

void deluge_streaming_chunk_set_loaded(void* chunk_backing) {
	reinterpret_cast<StreamedChunk*>(chunk_backing)->loaded = true;
}

DelugeChunkConvertState deluge_streaming_chunk_convert_state(void* chunk_backing) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	DelugeChunkConvertState state{};
	for (size_t i = 0; i < 3; ++i) {
		state.first_three_bytes[i] = static_cast<uint8_t>(cluster->first_three_bytes_pre_data_conversion[i]);
	}
	state.start_converted = cluster->extra_bytes_at_start_converted;
	state.end_converted = cluster->extra_bytes_at_end_converted;
	return state;
}

void deluge_streaming_chunk_set_convert_state(void* chunk_backing, DelugeChunkConvertState state) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	for (size_t i = 0; i < 3; ++i) {
		cluster->first_three_bytes_pre_data_conversion[i] = static_cast<char>(state.first_three_bytes[i]);
	}
	cluster->extra_bytes_at_start_converted = state.start_converted;
	cluster->extra_bytes_at_end_converted = state.end_converted;
}

// -- The differential's own oracle: the CURRENT non-voice read path's frame->bytes mapping --
// (see this file's own module doc above).

/// The direct oracle: `frame_bytes` bytes of a frame's interleaved samples, starting at within-
/// cluster byte offset `byte_offset`, read straight from `payload_with_trailing_slack()` -- valid
/// for any `byte_offset + frame_bytes <= cluster_size + kTrailingSlackBytes` (the straddle case).
void region_read_diff_frame_direct(void* chunk_backing, uint32_t byte_offset, uint32_t frame_bytes, uint8_t* out) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	auto span = cluster->payload_with_trailing_slack();
	std::memcpy(out, span.data() + byte_offset, frame_bytes);
}

/// The `frame_read_origin` oracle: per-channel, through the REAL production function -- see this
/// file's own module doc for the `+ (4 - byte_depth)` derivation. `byte_depth` must be in `[1, 4]`
/// (`kFrontSlackBytes`); `num_channels` bounded by this harness's own test geometry.
void region_read_diff_frame_via_origin(void* chunk_backing, uint32_t byte_offset, uint8_t byte_depth,
                                       uint8_t num_channels, uint8_t* out) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	for (uint8_t c = 0; c < num_channels; ++c) {
		std::byte* origin = cluster->frame_read_origin(byte_offset + c * byte_depth, byte_depth);
		std::memcpy(out + c * byte_depth, origin + (4 - byte_depth), byte_depth);
	}
}

} // extern "C"
