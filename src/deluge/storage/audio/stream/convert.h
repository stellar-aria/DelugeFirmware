#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::audio::stream {
// Pure per-word format conversion — the body of Sample::convertToNative(int32_t), depending only on the
// format. ENDIANNESS_WRONG_24 and NATIVE return the word unchanged (the 24-bit 3-byte swap is done
// group-wise in convert_cluster_data).
int32_t convert_word(int32_t word, RawDataFormat format);

// Cooperative-yield callback: invoked periodically (roughly every 1024 bytes) during a long in-place
// cluster conversion, so a caller can pump its audio routine without convert_cluster_data knowing
// anything about AudioEngine. Pass yield == nullptr to skip yielding entirely.
using YieldFn = void (*)(void* ctx);

// The audio-data geometry convert_cluster_data needs from the owning Sample. Gathered by the caller
// (Cluster owns none of this itself).
struct ConvertGeometry {
	uint32_t audio_data_start_pos_bytes;
	uint64_t audio_data_length_bytes;
	int32_t first_cluster_index_with_no_audio_data; // = sample->getFirstClusterIndexWithNoAudioData()
};

// Pure core of Cluster::convertDataIfNecessary: converts data[0..cluster_size) in place from `format` to
// native, given this cluster's index and the sample's audio-data geometry. On format != NATIVE, backs up
// the pre-conversion first 3 bytes of `data` into first_three_pre_conversion_out (mirrors
// Cluster::firstThreeBytesPreDataConversion, used to undo the 24-bit swap on a scan reversal) before doing
// any conversion. Calls yield(yield_ctx) periodically during the conversion loop; pass yield == nullptr to
// skip.
void convert_cluster_data(std::span<std::byte> data, int32_t cluster_index, RawDataFormat format,
                          ConvertGeometry geometry, size_t cluster_size, size_t cluster_size_magnitude,
                          std::span<std::byte, 3> first_three_pre_conversion_out, YieldFn yield, void* yield_ctx);
} // namespace deluge::audio::stream
