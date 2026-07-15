// tests/spec_audio_stream/convert_cluster_spec.cpp
//
// Own file (not a second `describe` inside convert_spec.cpp): the harness's create_test_sourcelist
// dispatches ctest -> driver by matching a function named after the *file's* stem, so a second
// CPPSPEC_SPEC in convert_spec.cpp would compile but never actually run under ctest. A second
// *_spec.cpp file is auto-picked-up by the existing glob in CMakeLists.txt with no build-file changes.
#include "storage/audio/stream/convert.h"

#include "cppspec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

using namespace deluge::audio::stream;

// clang-format off
describe convert_cluster("convert_cluster_data", $ {
	it("NATIVE leaves data untouched and does not write the backup out-param", _ {
		std::array<std::byte, 16> data{};
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = static_cast<std::byte>(i);
		}
		std::array<std::byte, 16> original = data;
		std::array<std::byte, 3> backup{std::byte{0xEE}, std::byte{0xEE}, std::byte{0xEE}};

		convert_cluster_data(std::span<std::byte>(data), /*cluster_index=*/0, RawDataFormat::NATIVE,
		                      ConvertGeometry{.audio_data_start_pos_bytes = 5,
		                                      .audio_data_length_bytes = 10,
		                                      .first_cluster_index_with_no_audio_data = 1},
		                      /*cluster_size=*/16, /*cluster_size_magnitude=*/4,
		                      std::span<std::byte, 3>(backup), [] {});

		expect(data == original).to_equal(true);
		expect(std::to_integer<int>(backup[0])).to_equal(0xEE);
		expect(std::to_integer<int>(backup[1])).to_equal(0xEE);
		expect(std::to_integer<int>(backup[2])).to_equal(0xEE);
	});

	it("ENDIANNESS_WRONG_24 swaps byte 0/2 of each 3-byte group in the audio region "
	   "and backs up the original first 3 bytes", _ {
		constexpr size_t cluster_size = 32; // 2^5
		std::array<std::byte, cluster_size> data{};
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = static_cast<std::byte>(i);
		}
		std::array<std::byte, 3> backup{};

		convert_cluster_data(std::span<std::byte>(data), /*cluster_index=*/0, RawDataFormat::ENDIANNESS_WRONG_24,
		                      ConvertGeometry{.audio_data_start_pos_bytes = 2,
		                                      .audio_data_length_bytes = 1000,
		                                      .first_cluster_index_with_no_audio_data = 5},
		                      cluster_size, /*cluster_size_magnitude=*/5,
		                      std::span<std::byte, 3>(backup), [] {});

		// Backup captured before any swap: the cluster's absolute first 3 bytes.
		expect(std::to_integer<int>(backup[0])).to_equal(0);
		expect(std::to_integer<int>(backup[1])).to_equal(1);
		expect(std::to_integer<int>(backup[2])).to_equal(2);

		// Bytes before the audio start (offset 2) are untouched.
		expect(std::to_integer<int>(data[0])).to_equal(0);
		expect(std::to_integer<int>(data[1])).to_equal(1);

		// First processed 3-byte group at offset 2: [2,3,4] -> [4,3,2].
		expect(std::to_integer<int>(data[2])).to_equal(4);
		expect(std::to_integer<int>(data[3])).to_equal(3);
		expect(std::to_integer<int>(data[4])).to_equal(2);

		// A middle group at offset 14: [14,15,16] -> [16,15,14].
		expect(std::to_integer<int>(data[14])).to_equal(16);
		expect(std::to_integer<int>(data[15])).to_equal(15);
		expect(std::to_integer<int>(data[16])).to_equal(14);

		// Last processed group at offset 29: [29,30,31] -> [31,30,29].
		expect(std::to_integer<int>(data[29])).to_equal(31);
		expect(std::to_integer<int>(data[30])).to_equal(30);
		expect(std::to_integer<int>(data[31])).to_equal(29);
	});

	it("UNSIGNED_8 XORs 0x80 per byte across the word region", _ {
		constexpr size_t cluster_size = 16; // 2^4
		std::array<std::byte, cluster_size> data{};
		std::array<std::byte, 3> backup{};

		convert_cluster_data(std::span<std::byte>(data), /*cluster_index=*/0, RawDataFormat::UNSIGNED_8,
		                      ConvertGeometry{.audio_data_start_pos_bytes = 4,
		                                      .audio_data_length_bytes = 1000,
		                                      .first_cluster_index_with_no_audio_data = 5},
		                      cluster_size, /*cluster_size_magnitude=*/4,
		                      std::span<std::byte, 3>(backup), [] {});

		// Bytes before the (4-byte-aligned) start pos are untouched.
		expect(std::to_integer<int>(data[0])).to_equal(0x00);
		expect(std::to_integer<int>(data[1])).to_equal(0x00);
		expect(std::to_integer<int>(data[2])).to_equal(0x00);
		expect(std::to_integer<int>(data[3])).to_equal(0x00);

		// The word region [4..16) is XORed with 0x80 per byte.
		for (size_t i = 4; i < cluster_size; ++i) {
			expect(std::to_integer<int>(data[i])).to_equal(0x80);
		}
	});

	it("audio_data_start_pos_bytes == 0 is a no-op (early-out before any backup or conversion)", _ {
		std::array<std::byte, 16> data{};
		for (size_t i = 0; i < data.size(); ++i) {
			data[i] = static_cast<std::byte>(i);
		}
		std::array<std::byte, 16> original = data;
		std::array<std::byte, 3> backup{std::byte{0xEE}, std::byte{0xEE}, std::byte{0xEE}};

		convert_cluster_data(std::span<std::byte>(data), /*cluster_index=*/0, RawDataFormat::ENDIANNESS_WRONG_24,
		                      ConvertGeometry{.audio_data_start_pos_bytes = 0,
		                                      .audio_data_length_bytes = 10,
		                                      .first_cluster_index_with_no_audio_data = 1},
		                      /*cluster_size=*/16, /*cluster_size_magnitude=*/4,
		                      std::span<std::byte, 3>(backup), [] {});

		expect(data == original).to_equal(true);
		expect(std::to_integer<int>(backup[0])).to_equal(0xEE);
		expect(std::to_integer<int>(backup[1])).to_equal(0xEE);
		expect(std::to_integer<int>(backup[2])).to_equal(0xEE);
	});

	it("invokes yield at least once for a buffer large enough to cross the ~1024-byte cadence", _ {
		constexpr size_t cluster_size = 4096; // 2^12
		auto data = std::make_unique<std::array<std::byte, cluster_size>>();
		std::array<std::byte, 3> backup{};
		int yield_count = 0;

		convert_cluster_data(std::span<std::byte>(*data), /*cluster_index=*/0, RawDataFormat::ENDIANNESS_WRONG_24,
		                      ConvertGeometry{.audio_data_start_pos_bytes = 3,
		                                      .audio_data_length_bytes = 100000,
		                                      .first_cluster_index_with_no_audio_data = 5},
		                      cluster_size, /*cluster_size_magnitude=*/12,
		                      std::span<std::byte, 3>(backup), [&] { ++yield_count; });

		expect(yield_count >= 1).to_equal(true);
	});
});

CPPSPEC_SPEC(convert_cluster)
