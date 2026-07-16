#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <algorithm>
#include <argon.hpp>
#include <argon/helpers/size.hpp> // argon::helpers::vectorizeable_size (not pulled in by argon.hpp itself)
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

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

// The ENDIANNESS_WRONG_24 byteswap loop: swaps byte 0 and byte 2 of every 3-byte group in [begin, end),
// yielding roughly every 1024 bytes. `yield` is skipped after the final chunk (see convert_cluster_data's
// doc comment for what it's for).
template <class Yield>
void convert_24bit_range(char* begin, char const* end, Yield yield) {
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
// SIMD path (currently just UNSIGNED_8; the remaining formats join in a later pass — see
// docs/superpowers/plans/2026-07-15-audio-stream-phase2d-simd-rewrite.md Task 3/4).
// Bytewise XOR 0x80 is exactly equivalent to the scalar path's word-wise `word ^ 0x80808080`
// (convert_word) for every byte position regardless of host endianness, since every byte of the XOR
// key is the same — so this is bit-exact to the scalar reference, not just an approximation of it.
// Yields roughly every 1024 bytes (64 lanes), matching convert_word_range's cadence. Returns the
// (16-byte-aligned) point where the caller's scalar tail should pick up; formats with no SIMD path
// yet are returned unchanged so the caller's scalar loop covers the whole range as before.
template <class Yield>
std::byte* convert_range_simd(std::byte* begin, std::byte* end, RawDataFormat format, Yield yield) {
	if (format != RawDataFormat::UNSIGNED_8) {
		return begin;
	}

	auto* p = reinterpret_cast<uint8_t*>(begin);
	uint8_t* const vec_end = p + argon::helpers::vectorizeable_size<uint8_t>(static_cast<size_t>(end - begin));
	Argon<uint8_t> const xor_key{uint8_t{0x80}};

	size_t bytes_since_yield = 0;
	for (; p < vec_end; p += Argon<uint8_t>::lanes) {
		(Argon<uint8_t>::Load(p) ^ xor_key).StoreTo(p);

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
