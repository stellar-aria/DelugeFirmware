#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <algorithm>
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

			while (true) {
				char const* end_pos_now = pos + 1024; // Every this many bytes, we'll pause and do an audio routine
				end_pos_now = std::min(end_pos_now, end_pos);

				while (pos < end_pos_now) {
					uint8_t temp = pos[0];
					pos[0] = pos[2];
					pos[2] = temp;
					pos += 3;
				}

				if (pos >= end_pos) {
					break;
				}

				yield();
			}
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

			for (; pos < end_pos; pos += 4) {

				if (!((uintptr_t)pos & 0b1111111100)) {
					yield();
				}

				convert_word_in_place(pos, format);
			}
		}
	}
}
} // namespace deluge::audio::stream
