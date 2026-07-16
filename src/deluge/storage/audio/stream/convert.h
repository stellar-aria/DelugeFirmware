#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <algorithm>
#include <argon.hpp>
#include <argon/helpers/size.hpp> // argon::helpers::vectorizeable_size (not pulled in by argon.hpp itself)
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility> // std::unreachable

namespace deluge::audio::stream {
// Pure per-word format conversion — the body of Sample::convertToNative(int32_t), depending only on the
// format. ENDIANNESS_WRONG_24 and NATIVE return the word unchanged (the 24-bit 3-byte swap is done
// group-wise in convert_cluster_data).
int32_t convert_word(int32_t word, RawDataFormat format);

// UB-free int32-over-byte access: memcpy through a local int32_t, which the compiler folds into a
// single unaligned load/store at -O2 (zero cost), unlike `*reinterpret_cast<int32_t*>(p)` which is both
// unaligned access and a strict-aliasing violation on a char/std::byte buffer.
inline int32_t load_word_unaligned(const std::byte* p) {
	int32_t w;
	std::memcpy(&w, p, sizeof(w));
	return w;
}
inline void store_word_unaligned(std::byte* p, int32_t w) {
	std::memcpy(p, &w, sizeof(w));
}
// Convert one word at a (possibly unaligned) byte address in place — UB-free replacement for
// `*(int32_t*)p = convert_word(*(int32_t*)p, format)`.
inline void convert_word_in_place(std::byte* p, RawDataFormat format) {
	store_word_unaligned(p, convert_word(load_word_unaligned(p), format));
}

// The audio-data geometry convert_cluster_data needs from the owning Sample. Gathered by the caller
// (Cluster owns none of this itself).
struct ConvertGeometry {
	uint32_t audio_data_start_pos_bytes;
	uint64_t audio_data_length_bytes;
	int32_t first_cluster_index_with_no_audio_data; // = sample->getFirstClusterIndexWithNoAudioData()
};

// Vectorized transform over the 48-byte-aligned (16 lanes x 3-byte groups) prefix of [begin, end), for
// ENDIANNESS_WRONG_24: de-interleaves 16 consecutive 3-byte groups into 3 channel vectors via vld3
// (LoadInterleaved<3>), then stores them back with channel 0 and channel 2 swapped via vst3
// (store_interleaved) -- the vectorized form of the scalar byte0<->byte2 swap below. The caller's `begin`
// is already 3-byte-group-aligned (see convert_cluster_data), so the 48-byte SIMD grid lines up with the
// scalar grid with no prologue needed. Yields roughly every 1024 bytes, matching convert_24bit_range's
// cadence. Returns the (48-byte-aligned) point where the caller's scalar tail should pick up.
template <class Yield>
char* convert_24bit_range_simd(char* begin, char const* end, Yield yield) {
	constexpr size_t lanes = Argon<uint8_t>::lanes; // 16
	constexpr size_t group_bytes = lanes * 3;       // 48

	size_t const num_groups = (static_cast<size_t>(end - begin) / 3) / lanes;
	char* const vec_end = begin + num_groups * group_bytes;

	size_t bytes_since_yield = 0;
	for (; begin < vec_end; begin += group_bytes) {
		auto* p = reinterpret_cast<uint8_t*>(begin);
		auto [c0, c1, c2] = Argon<uint8_t>::LoadInterleaved<3>(p);
		argon::store_interleaved(p, c2, c1, c0);

		bytes_since_yield += group_bytes;
		if (bytes_since_yield >= 1024) {
			yield();
			bytes_since_yield = 0;
		}
	}
	return begin;
}

// The ENDIANNESS_WRONG_24 byteswap loop: swaps byte 0 and byte 2 of every 3-byte group in [begin, end),
// yielding roughly every 1024 bytes. `yield` is skipped after the final chunk (see convert_cluster_data's
// doc comment for what it's for). Runs vectorized over the 48-byte-aligned prefix first
// (convert_24bit_range_simd); this loop then only covers the (< 48-byte) scalar tail.
template <class Yield>
void convert_24bit_range(char* begin, char const* end, Yield yield) {
	begin = convert_24bit_range_simd(begin, end, yield);

	while (true) {
		char const* end_pos_now = begin + 1024; // Every this many bytes, we'll pause and do an audio routine
		end_pos_now = std::min(end_pos_now, end);

		while (begin < end_pos_now) {
			uint8_t temp = begin[0];
			begin[0] = begin[2];
			begin[2] = temp;
			begin += 3;
		}

		if (begin >= end) {
			break;
		}

		yield();
	}
}

// Vectorized transform over the 16-byte-aligned prefix of [begin, end), for the formats that have a
// SIMD path (UNSIGNED_8, ENDIANNESS_WRONG_32, ENDIANNESS_WRONG_16, and FLOAT on real NEON only -- see
// below).
// UNSIGNED_8: bytewise XOR 0x80 is exactly equivalent to the scalar path's word-wise `word ^
// 0x80808080` (convert_word) for every byte position regardless of host endianness, since every byte
// of the XOR key is the same — so this is bit-exact to the scalar reference, not just an approximation
// of it.
// ENDIANNESS_WRONG_32/16: Reverse32bit/Reverse16bit (vrev32q_u8/vrev16q_u8) reverse the bytes within
// each 4-byte/2-byte lane group of the 16-byte vector -- the same word-relative-to-`begin` grid the
// scalar convert_word (swapEndianness32/swapEndianness2x16) walks, so this is bit-exact too.
// FLOAT is gated on real NEON/MVE (`__ARM_NEON` / `__ARM_FEATURE_MVE`, i.e. the arm-none firmware,
// qemu-arm, and Apple Silicon -- see argon's arm_simd.hpp dispatch): on those targets
// `Argon<float>::Load(...).ConvertTo<int32_t, 31>()` (vcvtq_n_s32_f32) is used, verified bit-exact to
// the scalar `q31_from_float` reference by tests/qemu/spec/convert_simd_neon_parity_spec.cpp under
// qemu-arm's instruction-accurate real-NEON emulation (~20,025 cases incl. saturation boundaries,
// denormals, NaN/inf, and a pseudo-random sweep -- zero mismatches; see
// docs/superpowers/plans/2026-07-15-audio-stream-phase2d-task4b-qemu-neon-float.md Task 1). On
// x86-SIMDe hosts (where neither macro is defined -- argon runs via SIMDe there, and this is the exact
// path the golden-master sim renders through), the SIMD FLOAT case is compiled out entirely and FLOAT
// falls through to the scalar `convert_word_range` loop (`convert_word` -> `q31_from_float`), because
// SIMDe's software emulation of `vcvtq_n_s32_f32` diverges from the scalar reference: 1.0f (the exact
// saturation boundary) produced 0x80000000 instead of the saturated 0x7FFFFFFF, and NaN produced
// 0x00000000 instead of saturating to 0x7FFFFFFF (see TODO.md and Phase 2d Task 4's investigation). A
// host-divergent FLOAT SIMD must not ship on the path the goldens render through, hence the
// compile-time gate rather than a runtime check.
// Yields roughly every 1024 bytes (64 lanes), matching convert_word_range's cadence. Returns the
// (16-byte-aligned) point where the caller's scalar tail should pick up; formats with no SIMD path
// yet are returned unchanged so the caller's scalar loop covers the whole range as before.
template <class Yield>
std::byte* convert_range_simd(std::byte* begin, std::byte* end, RawDataFormat format, Yield yield) {
#if defined(__ARM_NEON) || defined(__ARM_FEATURE_MVE)
	bool const has_simd_path = format == RawDataFormat::UNSIGNED_8 || format == RawDataFormat::ENDIANNESS_WRONG_32
	                           || format == RawDataFormat::ENDIANNESS_WRONG_16 || format == RawDataFormat::FLOAT;
#else
	bool const has_simd_path = format == RawDataFormat::UNSIGNED_8 || format == RawDataFormat::ENDIANNESS_WRONG_32
	                           || format == RawDataFormat::ENDIANNESS_WRONG_16;
#endif
	if (!has_simd_path) {
		return begin;
	}

	auto* p = reinterpret_cast<uint8_t*>(begin);
	uint8_t* const vec_end = p + argon::helpers::vectorizeable_size<uint8_t>(static_cast<size_t>(end - begin));
	Argon<uint8_t> const xor_key{uint8_t{0x80}};

	size_t bytes_since_yield = 0;
	for (; p < vec_end; p += Argon<uint8_t>::lanes) {
		switch (format) {
		case RawDataFormat::UNSIGNED_8:
			(Argon<uint8_t>::Load(p) ^ xor_key).StoreTo(p);
			break;
		case RawDataFormat::ENDIANNESS_WRONG_32:
			Argon<uint8_t>::Load(p).Reverse32bit().StoreTo(p);
			break;
		case RawDataFormat::ENDIANNESS_WRONG_16:
			Argon<uint8_t>::Load(p).Reverse16bit().StoreTo(p);
			break;
#if defined(__ARM_NEON) || defined(__ARM_FEATURE_MVE)
		case RawDataFormat::FLOAT: {
			auto converted = Argon<float>::Load(reinterpret_cast<const float*>(p)).ConvertTo<int32_t, 31>();
			converted.StoreTo(reinterpret_cast<int32_t*>(p));
			break;
		}
#endif
		default:
			std::unreachable();
		}

		bytes_since_yield += Argon<uint8_t>::lanes;
		if (bytes_since_yield >= 1024) {
			yield();
			bytes_since_yield = 0;
		}
	}
	return reinterpret_cast<std::byte*>(p);
}

// The other-bit-depths word loop: converts every 4-byte word in [begin, end) in place via
// convert_word_in_place, yielding on a 1024-byte address-aligned cadence. Formats with a SIMD path
// (convert_range_simd) run vectorized over their 16-byte-aligned prefix first; this loop then only
// covers the (< 16-byte) scalar tail for those, and the whole range for everything else.
template <class Yield>
void convert_word_range(std::byte* begin, std::byte* end, RawDataFormat format, Yield yield) {
	begin = convert_range_simd(begin, end, format, yield);

	for (; begin < end; begin += 4) {

		if (!((uintptr_t)begin & 0b1111111100)) {
			yield();
		}

		convert_word_in_place(begin, format);
	}
}

// Pure core of Cluster::convertDataIfNecessary: converts data[0..cluster_size) in place from `format` to
// native, given this cluster's index and the sample's audio-data geometry. On format != NATIVE, backs up
// the pre-conversion first 3 bytes of `data` into unconverted_head_out (mirrors
// Cluster::firstThreeBytesPreDataConversion, used to undo the 24-bit swap on a scan reversal) before doing
// any conversion.
//
// `yield` is a cooperative-scheduling shim: it's called (as `yield()`, no args) roughly every 1024 bytes
// to pump the caller's audio routine during a long in-place conversion, without this function knowing
// anything about AudioEngine. It's only needed because the conversion currently runs cooperatively on the
// audio context; once audio is preemptively scheduled it becomes unnecessary and removable — see TODO.md.
// Pass a no-op callable (e.g. `[]{}`) if no yielding is desired.
template <class Yield>
void convert_cluster_data(std::span<std::byte> data, int32_t cluster_index, RawDataFormat format,
                          ConvertGeometry geometry, size_t cluster_size, size_t cluster_size_magnitude,
                          std::span<std::byte, 3> unconverted_head_out, Yield yield) {
	char* char_data = reinterpret_cast<char*>(data.data());

	// We haven't yet figured out where the audio data starts
	if (geometry.audio_data_start_pos_bytes == 0) {
		return;
	}

	if (format != RawDataFormat::NATIVE) {
		std::copy(char_data, &char_data[3], reinterpret_cast<char*>(unconverted_head_out.data()));

		int32_t start_pos = geometry.audio_data_start_pos_bytes;
		int32_t start_cluster = start_pos >> cluster_size_magnitude;

		if (cluster_index < start_cluster) { // Hmm, there must have been a case where this happens...
			return;
		}

		// Special case for 24-bit with its uneven number of bytes
		if (format == RawDataFormat::ENDIANNESS_WRONG_24) {
			char* pos;

			if (cluster_index == start_cluster) {
				pos = &char_data[start_pos & (cluster_size - 1)];
			}
			else {
				uint32_t bytes_before_start_of_cluster =
				    cluster_index * cluster_size - geometry.audio_data_start_pos_bytes;
				int32_t bytes_eating_into_another_3byte = bytes_before_start_of_cluster % 3;
				if (bytes_eating_into_another_3byte == 0) {
					bytes_eating_into_another_3byte = 3;
				}
				pos = &char_data[3 - bytes_eating_into_another_3byte];
			}

			char const* end_pos;
			if (cluster_index == geometry.first_cluster_index_with_no_audio_data - 1) {
				uint32_t end_at_byte_pos = geometry.audio_data_start_pos_bytes + geometry.audio_data_length_bytes;
				uint32_t end_at_pos_within_cluster = end_at_byte_pos & (cluster_size - 1);
				end_pos = &char_data[end_at_pos_within_cluster];
			}
			else {
				end_pos = &char_data[cluster_size - 2];
			}

			convert_24bit_range(pos, end_pos, yield);
		}

		// Or, all other bit depths
		else {
			std::byte* pos;

			if (cluster_index == start_cluster) {
				pos = data.data() + (start_pos & (cluster_size - 1));
			}
			else {
				pos = data.data() + (start_pos & 0b11);
			}

			std::byte* end_pos;
			if (cluster_index == geometry.first_cluster_index_with_no_audio_data - 1) {
				uint32_t end_at_byte_pos = geometry.audio_data_start_pos_bytes + geometry.audio_data_length_bytes;
				uint32_t end_at_pos_within_cluster = end_at_byte_pos & (cluster_size - 1);
				end_pos = data.data() + end_at_pos_within_cluster;
			}
			else {
				end_pos = data.data() + (cluster_size - 3);
			}

			convert_word_range(pos, end_pos, format, yield);
		}
	}
}
} // namespace deluge::audio::stream
