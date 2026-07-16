// FatFS reentrancy stress harness -- host-native, real ff.c against a RAM
// disk, built with FF_FS_REENTRANT=1 (see ffconf.h) and run under
// ThreadSanitizer.
//
// A single-threaded smoke (mount -> f_mkfs -> write a known pattern -> close ->
// reopen -> read back -> assert byte-equal -> unlink) runs first as a cheap
// pre-check that the FatFS-on-host-with-reentrancy toolchain works, then the
// concurrent workload exercises the FF_FS_REENTRANT=1 volume grant under real
// thread concurrency.
//
//   - Distinct-file workers: N threads x M iterations, each writing to its
//     OWN files ("/w<t>_<i>.bin"), then reading back and verifying. Distinct
//     paths per thread mean no two threads ever touch the same FIL or the
//     same file -- FF_FS_REENTRANT protects the *volume* (FAT/dir
//     structures), not individual FILs, so sharing a FIL across threads
//     would be undefined even with the grant held and is deliberately out of
//     scope (see the correctness-bound note below).
//   - Shared-path metadata workers: threads doing read-only f_stat / f_open
//     FA_READ+read / f_opendir+f_readdir concurrently on the SAME
//     pre-created files and the same root directory. This exercises
//     directory traversal and shared read access under the grant. (Note:
//     FF_USE_LFN=2 -- forced by FF_FS_REENTRANT=1, see ffconf.h -- means the
//     LFN work buffer is a per-call STACK buffer, not a shared static, so
//     this workload is not targeting that specific static-buffer race; its
//     value is validating the grant covers the FAT/dir structures and
//     traversal under concurrent access.)
//   - A watchdog thread guards against a real lock-ordering hang: if the
//     workers don't finish within a wall-clock budget, it hard-exits with a
//     DEADLOCK verdict rather than hanging CI forever.
//
// CORRECTNESS BOUND: FF_FS_REENTRANT serializes access to the volume, not to
// an individual FIL. Every write-integrity worker below opens and closes its
// own FIL against its own path; no FIL and no path is ever shared between
// two threads for writing. The metadata workers only ever open their own FIL
// (FA_READ) against paths pre-created before the concurrent phase starts and
// never mutated during it, so concurrent read-only opens of the same path
// are the ordinary multi-reader case, not a shared-FIL race.
#include "ff.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Shared failure/reporting state
// ---------------------------------------------------------------------------

std::atomic<bool> g_failed{false};
std::mutex g_reportMutex; // serializes stderr output only; not load-bearing
                          // for the property under test.

void reportFailure(const std::string& msg) {
	std::lock_guard<std::mutex> lock(g_reportMutex);
	std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
	g_failed.store(true, std::memory_order_relaxed);
}

bool check(FRESULT fr, const std::string& what) {
	if (fr != FR_OK) {
		reportFailure(what + " returned FRESULT=" + std::to_string(static_cast<int>(fr)));
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Single-thread smoke: a cheap pre-check before the concurrent workload
// ---------------------------------------------------------------------------

constexpr const char* kSmokeFile = "smoke.bin";
constexpr size_t kSmokePatternSize = 4096;

std::array<uint8_t, kSmokePatternSize> makeSmokePattern() {
	std::array<uint8_t, kSmokePatternSize> pattern{};
	for (size_t i = 0; i < pattern.size(); ++i) {
		pattern[i] = static_cast<uint8_t>((i * 31) + 7);
	}
	return pattern;
}

bool runSmoke() {
	const auto pattern = makeSmokePattern();

	FIL file{};
	if (!check(f_open(&file, kSmokeFile, FA_WRITE | FA_CREATE_ALWAYS), "f_open(write)")) {
		return false;
	}

	UINT written = 0;
	FRESULT fr = f_write(&file, pattern.data(), pattern.size(), &written);
	if (!check(fr, "f_write") || written != pattern.size()) {
		std::fprintf(stderr, "FAIL: f_write wrote %u of %zu bytes\n", written, pattern.size());
		f_close(&file);
		return false;
	}

	if (!check(f_close(&file), "f_close(write)")) {
		return false;
	}

	FIL rfile{};
	if (!check(f_open(&rfile, kSmokeFile, FA_READ), "f_open(read)")) {
		return false;
	}

	std::array<uint8_t, kSmokePatternSize> readback{};
	UINT bytesRead = 0;
	fr = f_read(&rfile, readback.data(), readback.size(), &bytesRead);
	if (!check(fr, "f_read") || bytesRead != pattern.size()) {
		std::fprintf(stderr, "FAIL: f_read got %u of %zu bytes\n", bytesRead, pattern.size());
		f_close(&rfile);
		return false;
	}

	if (!check(f_close(&rfile), "f_close(read)")) {
		return false;
	}

	if (std::memcmp(pattern.data(), readback.data(), pattern.size()) != 0) {
		std::fprintf(stderr, "FAIL: read-back pattern mismatch\n");
		return false;
	}

	if (!check(f_unlink(kSmokeFile), "f_unlink")) {
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// Concurrent workload configuration
// ---------------------------------------------------------------------------

constexpr int kWriteThreads = 8;
constexpr int kWriteIters = 500; // per thread
constexpr int kSharedFiles = 16; // pre-created, read-only during the concurrent phase
constexpr int kMetaThreads = 6;
constexpr int kMetaIters = 300; // per thread
constexpr auto kWatchdogBudget = std::chrono::seconds(60);

// Deterministic-but-varied pattern seeded by (t, i): a fixed seed always
// reproduces the same length + bytes, so a mismatch is a real bug, not
// nondeterminism in the test itself.
std::vector<uint8_t> makePattern(int t, int i) {
	std::mt19937 rng(static_cast<uint32_t>(t) * 100003u + static_cast<uint32_t>(i) + 1u);
	std::uniform_int_distribution<size_t> lenDist(1, 8192);
	const size_t len = lenDist(rng);
	std::vector<uint8_t> pattern(len);
	for (auto& b : pattern) {
		b = static_cast<uint8_t>(rng());
	}
	return pattern;
}

// ---------------------------------------------------------------------------
// Distinct-file write/read-back workers
// ---------------------------------------------------------------------------

void writeReadWorker(int t) {
	for (int i = 0; i < kWriteIters; ++i) {
		if (g_failed.load(std::memory_order_relaxed)) {
			return;
		}

		const std::string path = "/w" + std::to_string(t) + "_" + std::to_string(i) + ".bin";
		const auto pattern = makePattern(t, i);

		FIL file{};
		if (!check(f_open(&file, path.c_str(), FA_WRITE | FA_CREATE_ALWAYS), "f_open(write) " + path)) {
			return;
		}

		UINT written = 0;
		FRESULT fr = f_write(&file, pattern.data(), pattern.size(), &written);
		if (!check(fr, "f_write " + path) || written != pattern.size()) {
			reportFailure("f_write short write on " + path + " (" + std::to_string(written) + " of "
			              + std::to_string(pattern.size()) + ")");
			f_close(&file);
			return;
		}
		if (!check(f_close(&file), "f_close(write) " + path)) {
			return;
		}

		FIL rfile{};
		if (!check(f_open(&rfile, path.c_str(), FA_READ), "f_open(read) " + path)) {
			return;
		}

		std::vector<uint8_t> readback(pattern.size());
		UINT bytesRead = 0;
		fr = f_read(&rfile, readback.data(), readback.size(), &bytesRead);
		if (!check(fr, "f_read " + path) || bytesRead != pattern.size()) {
			reportFailure("f_read short read on " + path + " (" + std::to_string(bytesRead) + " of "
			              + std::to_string(pattern.size()) + ")");
			f_close(&rfile);
			return;
		}
		if (!check(f_close(&rfile), "f_close(read) " + path)) {
			return;
		}

		if (std::memcmp(pattern.data(), readback.data(), pattern.size()) != 0) {
			reportFailure("integrity mismatch on " + path);
			return;
		}

		if (!check(f_unlink(path.c_str()), "f_unlink " + path)) {
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// Shared-path metadata contention workers
// ---------------------------------------------------------------------------

struct SharedFile {
	std::string path;
	std::vector<uint8_t> contents;
};

std::vector<SharedFile> createSharedFiles() {
	std::vector<SharedFile> files;
	files.reserve(kSharedFiles);
	for (int k = 0; k < kSharedFiles; ++k) {
		SharedFile sf;
		sf.path = "/shared_" + std::to_string(k) + ".bin";
		sf.contents = makePattern(-1, k); // disjoint seed space from write workers' (t >= 0)

		FIL file{};
		if (!check(f_open(&file, sf.path.c_str(), FA_WRITE | FA_CREATE_ALWAYS), "f_open(seed write) " + sf.path)) {
			return {};
		}
		UINT written = 0;
		FRESULT fr = f_write(&file, sf.contents.data(), sf.contents.size(), &written);
		if (!check(fr, "f_write(seed) " + sf.path) || written != sf.contents.size()) {
			reportFailure("seed write short on " + sf.path);
			f_close(&file);
			return {};
		}
		if (!check(f_close(&file), "f_close(seed write) " + sf.path)) {
			return {};
		}
		files.push_back(std::move(sf));
	}
	return files;
}

void metadataWorker(const std::vector<SharedFile>& shared, unsigned seed) {
	std::mt19937 rng(seed);
	std::uniform_int_distribution<size_t> pickFile(0, shared.size() - 1);
	std::uniform_int_distribution<int> pickOp(0, 2);

	for (int i = 0; i < kMetaIters; ++i) {
		if (g_failed.load(std::memory_order_relaxed)) {
			return;
		}

		const SharedFile& sf = shared[pickFile(rng)];
		switch (pickOp(rng)) {
		case 0: {
			FILINFO info{};
			if (!check(f_stat(sf.path.c_str(), &info), "f_stat " + sf.path)) {
				return;
			}
			if (static_cast<size_t>(info.fsize) != sf.contents.size()) {
				reportFailure("f_stat size mismatch on " + sf.path + " (got " + std::to_string(info.fsize)
				              + ", expected " + std::to_string(sf.contents.size()) + ")");
				return;
			}
			break;
		}
		case 1: {
			FIL file{};
			if (!check(f_open(&file, sf.path.c_str(), FA_READ), "f_open(shared read) " + sf.path)) {
				return;
			}
			std::vector<uint8_t> buf(sf.contents.size());
			UINT bytesRead = 0;
			FRESULT fr = f_read(&file, buf.data(), buf.size(), &bytesRead);
			if (!check(fr, "f_read(shared) " + sf.path) || bytesRead != buf.size()) {
				reportFailure("shared read short on " + sf.path);
				f_close(&file);
				return;
			}
			if (!check(f_close(&file), "f_close(shared read) " + sf.path)) {
				return;
			}
			if (std::memcmp(buf.data(), sf.contents.data(), buf.size()) != 0) {
				reportFailure("shared-file integrity mismatch on " + sf.path);
				return;
			}
			break;
		}
		default: {
			DIR dir{};
			if (!check(f_opendir(&dir, "/"), "f_opendir /")) {
				return;
			}
			FILINFO entry{};
			FRESULT fr = FR_OK;
			int count = 0;
			while ((fr = f_readdir(&dir, &entry)) == FR_OK && entry.fname[0] != '\0') {
				++count;
				if (count > 100000) { // guard against a corrupt/cyclic directory chain
					reportFailure("f_readdir runaway on / (possible corrupt chain)");
					break;
				}
			}
			if (fr != FR_OK) {
				reportFailure("f_readdir error on / FRESULT=" + std::to_string(static_cast<int>(fr)));
			}
			f_closedir(&dir);
			break;
		}
		}
	}
}

// ---------------------------------------------------------------------------
// Watchdog
// ---------------------------------------------------------------------------

std::atomic<bool> g_allJoined{false};

void watchdog() {
	std::this_thread::sleep_for(kWatchdogBudget);
	if (!g_allJoined.load(std::memory_order_acquire)) {
		std::fprintf(stderr, "DEADLOCK/HANG: workers did not complete within %lld s\n",
		             static_cast<long long>(kWatchdogBudget.count()));
		std::fflush(stderr);
		std::_Exit(2); // hard-exit: a stuck join means we cannot cleanly tear down
	}
}

} // namespace

int main() {
	FATFS fs{};
	if (!check(f_mount(&fs, "", 0), "f_mount")) {
		std::puts("FAIL: fatfs_stress");
		return 1;
	}

	std::array<BYTE, FF_MAX_SS> work{};
	if (!check(f_mkfs("", nullptr, work.data(), work.size()), "f_mkfs")) {
		std::puts("FAIL: fatfs_stress");
		return 1;
	}

	if (!runSmoke()) {
		std::puts("FAIL: fatfs_stress (single-thread smoke)");
		return 1;
	}
	std::puts("PASS: single-thread smoke (mount/mkfs/write/close/reopen/read/unlink)");

	const auto shared = createSharedFiles();
	if (g_failed.load(std::memory_order_relaxed) || shared.size() != kSharedFiles) {
		std::puts("FAIL: fatfs_stress (shared-file seeding)");
		return 1;
	}

	std::thread wd(watchdog);
	wd.detach();

	std::vector<std::thread> workers;
	workers.reserve(kWriteThreads + kMetaThreads);
	for (int t = 0; t < kWriteThreads; ++t) {
		workers.emplace_back(writeReadWorker, t);
	}
	for (int m = 0; m < kMetaThreads; ++m) {
		// Distinct seed per metadata thread; disjoint from makePattern's seed space.
		workers.emplace_back(metadataWorker, std::cref(shared), 0xC0FFEEu + static_cast<unsigned>(m));
	}

	for (auto& w : workers) {
		w.join();
	}
	g_allJoined.store(true, std::memory_order_release);

	bool cleanupOk = true;
	for (const auto& sf : shared) {
		if (!check(f_unlink(sf.path.c_str()), "f_unlink(shared) " + sf.path)) {
			cleanupOk = false;
		}
	}

	const bool failed = g_failed.load(std::memory_order_relaxed) || !cleanupOk;

	std::printf("%d write/read threads x %d iters, %d shared files x %d metadata-thread iters, %s\n", kWriteThreads,
	            kWriteIters, kSharedFiles, kMetaIters,
	            failed ? "FAILURES SEEN (see FAIL lines above)" : "0 mismatches, 0 FRESULT errors");

	if (failed) {
		std::puts("FAIL: fatfs_stress");
		return 1;
	}

	std::puts("PASS: fatfs_stress (single-thread smoke + concurrent distinct-file + shared-metadata workload)");
	return 0;
}
