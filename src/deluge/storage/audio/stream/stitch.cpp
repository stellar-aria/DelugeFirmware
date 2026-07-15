#include "storage/audio/stream/stitch.h"
#include "storage/audio/stream/convert.h"
#include <cstddef>
#include <cstring>

namespace deluge::audio::stream {

namespace {
// Swaps byte 0 and byte 2 of a 3-byte group in place (the ENDIANNESS_WRONG_24 fixup).
void byteswap3(std::byte* bytes) {
	std::byte temp = bytes[0];
	bytes[0] = bytes[2];
	bytes[2] = temp;
}
} // namespace

// Faithful port of the inter-cluster boundary-stitch block in AudioFileManager::readClusterData
// (audio_file_manager.cpp:1044-1223). See stitch.h for the edge-struct index bases.
void stitch_boundaries(std::span<std::byte> self_data, int32_t cluster_index, RawDataFormat format,
                       uint32_t audio_data_start_pos_bytes, size_t cluster_size, bool& self_start_boundary_converted,
                       bool& self_end_boundary_converted, StitchPrevEdge* prev, StitchNextEdge* next) {
	int32_t misalignment = static_cast<int32_t>(audio_data_start_pos_bytes & 0b11);

	// Give extra bytes to the previous cluster.
	if (prev != nullptr) {
		// We first copy our first 7 bytes from here to the end of the prev cluster's overhang
		// (prev->tail[4..11) == prevCluster->data[cluster_size..cluster_size+7))...
		std::memcpy(&prev->tail[4], self_data.data(), 7);

		// If 24-bit wrong-endian data...
		if (format == RawDataFormat::ENDIANNESS_WRONG_24) {

			// If we hadn't previously written the "extra" bytes to the end of the prev cluster and
			// converted them, do so now...
			if (!*prev->end_boundary_converted) {

				uint32_t bytes_before_start_of_cluster =
				    static_cast<uint32_t>(cluster_index) * static_cast<uint32_t>(cluster_size)
				    - audio_data_start_pos_bytes;
				int32_t bytes_unconverted_before_cluster = static_cast<int32_t>(bytes_before_start_of_cluster % 3);
				if (bytes_unconverted_before_cluster != 0) {

					// There'll be one word in there which hasn't yet been converted. Do it now. (We've
					// probably just copied over the next one and a bit, which already was converted)
					int32_t start_pos = 4 - bytes_unconverted_before_cluster;
					byteswap3(&prev->tail[start_pos]);

					// And now, copy 2 bytes back to this cluster (that's the maximum that the float
					// could have been overhanging the boundary)
					std::memcpy(self_data.data(), &prev->tail[4], 2);
				}

				*prev->end_boundary_converted = true;
			}
		}

		// Or, all other types of raw data conversion
		else if (format != RawDataFormat::NATIVE) {

			// If we haven't previously written the "extra" bytes to the end of the prev cluster and
			// converted them, do so now...
			if (!*prev->end_boundary_converted) {

				// If misaligned from the 4-byte boundary
				if (misalignment != 0) {

					// There'll be one word in there which hasn't yet been converted. Do it now. (We've
					// probably also just moved over the next one too, which already was converted)
					convert_word_in_place(&prev->tail[misalignment], format);

					// And now, copy 3 bytes back to this cluster (that's the maximum that the float
					// could have been overhanging the boundary)
					std::memcpy(self_data.data(), &prev->tail[4], 3);
				}

				*prev->end_boundary_converted = true;
			}
		}
		// NATIVE: nothing to convert.

		self_start_boundary_converted = true;
	}

	// Grab extra bytes from the next cluster.
	if (next != nullptr) {
		bool need_copy7 = false;

		// If 24-bit wrong-endian data...
		if (format == RawDataFormat::ENDIANNESS_WRONG_24) {

			uint32_t bytes_before_start_of_next_cluster =
			    static_cast<uint32_t>(cluster_index + 1) * static_cast<uint32_t>(cluster_size)
			    - audio_data_start_pos_bytes;
			int32_t bytes_unconverted_before_next_cluster =
			    static_cast<int32_t>(bytes_before_start_of_next_cluster % 3);

			// If one word missed conversion...
			if (bytes_unconverted_before_next_cluster != 0) {

				// If we hadn't previously converted the first couple of bytes of the next cluster...
				if (!*next->start_boundary_converted) {
					// We first copy the next cluster's first 7 bytes to the end of this cluster
					std::memcpy(&self_data[cluster_size], next->head.data(), 7);
				}
				// Or, if we *had* previously converted the first bytes of the next cluster...
				else {
					// Grab the unconverted bytes back from where we backed them up to
					std::memcpy(&self_data[cluster_size], next->unconverted_head.data(), 2);
				}

				// There'll be one word in there which hasn't yet been converted. Do it now. (We've
				// probably just copied over the next one and a bit, which already was converted)
				int32_t start_pos = static_cast<int32_t>(cluster_size) - bytes_unconverted_before_next_cluster;
				byteswap3(&self_data[start_pos]);

				// If we hadn't previously converted the first couple of bytes of the next cluster, do so
				// now...
				if (!*next->start_boundary_converted) {
					*next->start_boundary_converted = true;

					// And now, copy 2 bytes back to the next cluster (that's the maximum that the 24-bit
					// int32_t could have been overhanging the boundary)
					std::memcpy(next->head.data(), &self_data[cluster_size], 2);
				}
				// Or, if we *had* previously converted the first bytes of the next cluster...
				else {
					need_copy7 = true;
				}
			}

			// Or if no words missed conversion
			else {
				need_copy7 = true;
			}
		}

		// Or, all other types of raw data conversion
		else if (format != RawDataFormat::NATIVE) {

			// If one word missed conversion...
			if (misalignment != 0) {
				int32_t start_pos = static_cast<int32_t>(cluster_size) - 4 + misalignment;

				// If we hadn't previously converted the first couple of bytes of the next cluster, do so
				// now...
				if (!*next->start_boundary_converted) {

					// We first copy the next cluster's first 7 bytes to the end of this cluster. This
					// MUST happen before the convert below: start_pos can reach cluster_size-1, so the
					// straddling word overlaps this freshly-staged overhang.
					std::memcpy(&self_data[cluster_size], next->head.data(), 7);

					// There'll be one word in there which hasn't yet been converted from float. Do it now
					convert_word_in_place(&self_data[start_pos], format);

					// And now, copy 3 bytes back to the next cluster (that's the maximum that the float
					// could have been overhanging the boundary)
					std::memcpy(next->head.data(), &self_data[cluster_size], 3);

					*next->start_boundary_converted = true;
				}

				// Or, if we *had* previously converted the first bytes of the next cluster...
				else {
					// Grab the unconverted bytes back from where we backed them up to (transient staging
					// — the straddling word can still overlap this range — the need_copy7 finalize below
					// overwrites it with the real next->head regardless)
					std::memcpy(&self_data[cluster_size], next->unconverted_head.data(), 3);

					// There'll be one word in there which hasn't yet been converted from float. Do it now
					convert_word_in_place(&self_data[start_pos], format);

					// And now just copy the converted-from-float first bytes from the next cluster to the
					// end of this one
					need_copy7 = true;
				}
			}
			else {
				need_copy7 = true;
			}
		}

		// NATIVE
		else {
			need_copy7 = true;
		}

		if (need_copy7) {
			// We copy the next cluster's first 7 bytes to the end of this cluster
			std::memcpy(&self_data[cluster_size], next->head.data(), 7);
		}

		self_end_boundary_converted = true;
	}
}

} // namespace deluge::audio::stream
