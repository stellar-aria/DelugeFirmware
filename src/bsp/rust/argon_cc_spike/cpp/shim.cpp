// SPIKE (SR2b) — THROWAWAY de-risk, superseded by SR2d.
//
// extern "C" shims over the app's convert.cpp / stitch.cpp so a Rust build.rs can `cc::Build`-compile
// them and a Rust #[test] can call them across the FFI boundary. The point is to prove the argon-SIMD
// translation units compile under `cc` (x86-SIMDe + armv7a-NEON) and that the resulting objects behave
// correctly — the same behaviour tests/spec_audio_stream/{convert,stitch,convert_cluster}_spec.cpp pin.
//
// The only non-trivial shim is convert_cluster_data: it is TEMPLATED on `Yield`. SR2d's Rust fill task
// will call it, so this proves a concrete no-op-Yield instantiation is reachable behind extern "C".
#include "storage/audio/stream/convert.h"
#include "storage/audio/stream/stitch.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

using namespace deluge::audio::stream;

extern "C" {

// --- convert_word: the scalar core (no template). --------------------------------------------------
int32_t spike_convert_word(int32_t word, uint8_t format) {
	return convert_word(word, static_cast<RawDataFormat>(format));
}

// --- convert_cluster_data: the TEMPLATED entry point, concretized here with a counting Yield. -------
// yield_counter may be null (the no-op case SR2d cares about); when non-null it is bumped once per
// yield so the test can assert the cooperative-scheduling cadence still fires through the shim.
void spike_convert_cluster_data(std::byte* data, size_t data_len, int32_t cluster_index, uint8_t format,
                                uint32_t audio_data_start_pos_bytes, uint64_t audio_data_length_bytes,
                                int32_t first_cluster_index_with_no_audio_data, size_t cluster_size,
                                size_t cluster_size_magnitude, std::byte* unconverted_head_out,
                                int32_t* yield_counter) {
	ConvertGeometry geometry{
	    .audio_data_start_pos_bytes = audio_data_start_pos_bytes,
	    .audio_data_length_bytes = audio_data_length_bytes,
	    .first_cluster_index_with_no_audio_data = first_cluster_index_with_no_audio_data,
	};
	std::span<std::byte, 3> head_out{unconverted_head_out, 3};
	convert_cluster_data(std::span<std::byte>{data, data_len}, cluster_index, static_cast<RawDataFormat>(format),
	                     geometry, cluster_size, cluster_size_magnitude, head_out, [yield_counter] {
		                     if (yield_counter != nullptr) {
			                     ++*yield_counter;
		                     }
	                     });
}

// --- stitch_boundaries: plain function, wrapped directly. -------------------------------------------
// Pass prev_tail==nullptr for "no prev neighbor", next_head==nullptr for "no next neighbor".
void spike_stitch_boundaries(std::byte* self_data, size_t self_len, int32_t cluster_index, uint8_t format,
                             uint32_t audio_data_start_pos_bytes, size_t cluster_size, bool* self_start, bool* self_end,
                             std::byte* prev_tail, size_t prev_tail_len, bool* prev_end_converted, std::byte* next_head,
                             size_t next_head_len, const std::byte* next_unconverted_head, bool* next_start_converted) {
	// StitchNextEdge holds a fixed-extent std::span<const std::byte, 3> (not default-constructible), so
	// the edges are built via std::optional rather than default-then-assign.
	std::optional<StitchPrevEdge> prev_edge;
	std::optional<StitchNextEdge> next_edge;

	if (prev_tail != nullptr) {
		prev_edge.emplace(StitchPrevEdge{.tail = std::span<std::byte>{prev_tail, prev_tail_len},
		                                 .end_boundary_converted = prev_end_converted});
	}
	if (next_head != nullptr) {
		next_edge.emplace(StitchNextEdge{.head = std::span<std::byte>{next_head, next_head_len},
		                                 .unconverted_head = std::span<const std::byte, 3>{next_unconverted_head, 3},
		                                 .start_boundary_converted = next_start_converted});
	}

	stitch_boundaries(std::span<std::byte>{self_data, self_len}, cluster_index, static_cast<RawDataFormat>(format),
	                  audio_data_start_pos_bytes, cluster_size, *self_start, *self_end,
	                  prev_edge ? &*prev_edge : nullptr, next_edge ? &*next_edge : nullptr);
}

} // extern "C"
