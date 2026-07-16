// FatFS reentrancy stress harness -- host-native, real ff.c against a RAM
// disk, built with FF_FS_REENTRANT=1 (see ffconf.h) and run under
// ThreadSanitizer.
//
// Task 1 (this file, single-threaded): mount -> f_mkfs -> write a known
// pattern -> close -> reopen -> read back -> assert byte-equal -> unlink.
// Proves the whole FatFS-on-host-with-reentrancy toolchain works before
// Task 2 adds the concurrent workload.
#include "ff.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* kTestFile = "smoke.bin";
constexpr size_t kPatternSize = 4096;

// Deterministic, position-dependent byte pattern -- catches both
// truncation/offset bugs (not just "all zero" corruption) on read-back.
std::array<uint8_t, kPatternSize> makePattern() {
	std::array<uint8_t, kPatternSize> pattern{};
	for (size_t i = 0; i < pattern.size(); ++i) {
		pattern[i] = static_cast<uint8_t>((i * 31) + 7);
	}
	return pattern;
}

bool check(FRESULT fr, const char* what) {
	if (fr != FR_OK) {
		std::fprintf(stderr, "FAIL: %s returned FRESULT=%d\n", what, static_cast<int>(fr));
		return false;
	}
	return true;
}

} // namespace

int main() {
	// opt=0 (delayed mount): the RAM disk has no FAT structure on it yet, so
	// forcing an immediate mount (opt=1) here would fail with FR_NO_FILESYSTEM.
	// f_mount() still creates the volume's sync object either way (see
	// src/fatfs/ff.c's f_mount()); f_mkfs() below writes the filesystem, and
	// the first real file op mounts it lazily.
	FATFS fs{};
	if (!check(f_mount(&fs, "", 0), "f_mount")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	std::array<BYTE, FF_MAX_SS> work{};
	if (!check(f_mkfs("", nullptr, work.data(), work.size()), "f_mkfs")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	const auto pattern = makePattern();

	FIL file{};
	if (!check(f_open(&file, kTestFile, FA_WRITE | FA_CREATE_ALWAYS), "f_open(write)")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	UINT written = 0;
	FRESULT fr = f_write(&file, pattern.data(), pattern.size(), &written);
	if (!check(fr, "f_write") || written != pattern.size()) {
		std::fprintf(stderr, "FAIL: f_write wrote %u of %zu bytes\n", written, pattern.size());
		f_close(&file);
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	if (!check(f_close(&file), "f_close(write)")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	FIL rfile{};
	if (!check(f_open(&rfile, kTestFile, FA_READ), "f_open(read)")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	std::array<uint8_t, kPatternSize> readback{};
	UINT bytesRead = 0;
	fr = f_read(&rfile, readback.data(), readback.size(), &bytesRead);
	if (!check(fr, "f_read") || bytesRead != pattern.size()) {
		std::fprintf(stderr, "FAIL: f_read got %u of %zu bytes\n", bytesRead, pattern.size());
		f_close(&rfile);
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	if (!check(f_close(&rfile), "f_close(read)")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	if (std::memcmp(pattern.data(), readback.data(), pattern.size()) != 0) {
		std::fprintf(stderr, "FAIL: read-back pattern mismatch\n");
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	if (!check(f_unlink(kTestFile), "f_unlink")) {
		std::puts("FAIL: fatfs_stress smoke");
		return 1;
	}

	std::puts("PASS: fatfs_stress smoke (mount/mkfs/write/close/reopen/read/unlink, single-threaded)");
	return 0;
}
