#include "storage/audio/stream/convert.h"
#include "util/audio_format_helpers.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace deluge::audio::stream {
int32_t convert_word(int32_t word, RawDataFormat format) {
	switch (format) {
	case RawDataFormat::FLOAT:
		return q31_from_float(std::bit_cast<float>(word));
	case RawDataFormat::ENDIANNESS_WRONG_32:
		return swapEndianness32(word);
	case RawDataFormat::ENDIANNESS_WRONG_16:
		return swapEndianness2x16(word);
	case RawDataFormat::UNSIGNED_8:
		return word ^ 0x80808080;
	case RawDataFormat::ENDIANNESS_WRONG_24:
		[[fallthrough]];
	case RawDataFormat::NATIVE:
		break;
	}
	return word;
}

// Verbatim port of Cluster::convertDataIfNecessary (cluster.cpp:60-153) — see convert.h for the
// substitution list vs. the original. Every local below is renamed to snake_case; the control flow,
// loop bounds, and pointer arithmetic are otherwise byte-identical to the original.
void convert_cluster_data(std::span<std::byte> data, int32_t cluster_index, RawDataFormat format,
                          ConvertGeometry geometry, size_t cluster_size, size_t cluster_size_magnitude,
                          std::span<std::byte, 3> first_three_pre_conversion_out, YieldFn yield, void* yield_ctx) {
	char* char_data = reinterpret_cast<char*>(data.data());

	// We haven't yet figured out where the audio data starts
	if (geometry.audio_data_start_pos_bytes == 0) {
		return;
	}

	if (format != RawDataFormat::NATIVE) {
		std::copy(char_data, &char_data[3], reinterpret_cast<char*>(first_three_pre_conversion_out.data()));

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

				if (yield) {
					yield(yield_ctx);
				}
			}
		}

		// Or, all other bit depths
		else {
			int32_t* pos;

			if (cluster_index == start_cluster) {
				pos = (int32_t*)&char_data[start_pos & (cluster_size - 1)];
			}
			else {
				pos = (int32_t*)&char_data[start_pos & 0b11];
			}

			int32_t* end_pos;
			if (cluster_index == geometry.first_cluster_index_with_no_audio_data - 1) {
				uint32_t end_at_byte_pos = geometry.audio_data_start_pos_bytes + geometry.audio_data_length_bytes;
				uint32_t end_at_pos_within_cluster = end_at_byte_pos & (cluster_size - 1);
				end_pos = (int32_t*)&char_data[end_at_pos_within_cluster];
			}
			else {
				end_pos = (int32_t*)&char_data[cluster_size - 3];
			}

			for (; pos < end_pos; pos++) {

				if (!((uintptr_t)pos & 0b1111111100)) {
					if (yield) {
						yield(yield_ctx);
					}
				}

				*pos = convert_word(*pos, format);
			}
		}
	}
}
} // namespace deluge::audio::stream
