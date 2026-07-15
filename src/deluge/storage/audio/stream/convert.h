#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstdint>

namespace deluge::audio::stream {
// Pure per-word format conversion — the body of Sample::convertToNative(int32_t), depending only on the
// format. ENDIANNESS_WRONG_24 and NATIVE return the word unchanged (the 24-bit 3-byte swap is done
// group-wise in convert_cluster_data, added in Task 2).
int32_t convert_word(int32_t word, RawDataFormat format);
} // namespace deluge::audio::stream
