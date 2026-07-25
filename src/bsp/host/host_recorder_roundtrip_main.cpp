/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

/// deluge_recorder_roundtrip — headless byte-exact round-trip oracle for `SampleRecorder`.
///
/// SR3b (the recorder-decouple sub-project, docs/superpowers/specs/2026-07-25-sr3b-recorder-decouple-design.md)
/// removes the recorder from the shared residency machinery. This binary is the byte-exact proof the
/// refactor is not allowed to perturb: feed a known deterministic PCM ramp into a REAL `SampleRecorder`
/// (constructed/driven directly, no UI/song-load ceremony), pump its fiber drain (`cardRoutine()`)
/// synchronously on this one thread, finalize, then read the produced SD file back through
/// `deluge::io::File` (the general-purpose read API — `deluge::io::Stream`'s READ mode is
/// `sector_of()`-only by design, see `stream_io.h`) and assert every byte: the 44/112-byte WAV header
/// (RIFF/data chunk sizes patched at finalize) plus the audio data, byte-for-byte, against the input.
///
/// Boots the full host-sim app (same `deluge_platform_init()`/`deluge_main()` sequence as
/// `deluge_render`/`deluge_loadcheck`) so every dependency `SampleRecorder::setup()`/`cardRoutine()`
/// touches (currentSong, AudioFileManager, the resource manager, the efatfs-backed
/// `deluge::io::Stream`/`File`) is genuinely live — this is real production code, not a fake/mock.
///
/// Usage: deluge_recorder_roundtrip   (no arguments; runs the whole geometry matrix, deterministic)
/// Exit code 0 on all cases passing, 1 otherwise. No mtools/project dependency — the disk image is
/// freshly `mformat`-ed empty (we only ever WRITE new files, never read a pre-seeded project).

#include "definitions_cxx.hpp"
#include "io/file.hpp"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/cluster/cluster.h"

#include "OSLikeStuff/scheduler_api.h" // TaskHandle
#include "libdeluge/system.h"          // deluge_platform_init

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" int32_t deluge_main(void);
extern TaskHandle startupConditionalTask;

// SR3b Task 2 Step 4's sim-side sanity check for the recorder live-readback probe (see
// harness/recorder_readback_probe.h) — the C-host sim is the "control" target the spike predicts
// SHOULD resolve READY (no async loader; the fiber pump drains through RecordingReadSource), so
// exercising it here first validates the probe itself before trusting its result on host_app.
extern "C" {
uint8_t deluge_harness_recorder_probe(uint8_t numChannels, uint32_t numFrames, uint32_t pumpDrainTicks);
void deluge_harness_recorder_probe_end();
}

namespace {

char g_temp_image[512] = {0};
int g_failures = 0;
int g_cases = 0;

void cleanup_temp_image() {
	if (g_temp_image[0] != '\0') {
		unlink(g_temp_image);
		g_temp_image[0] = '\0';
	}
}

bool format_empty_image(char* out_path, size_t out_size) {
	char tmpl[] = "/tmp/deluge_recorder_roundtrip_XXXXXX.img";
	int fd = mkstemps(tmpl, 4);
	if (fd < 0) {
		perror("[recorder_roundtrip] mkstemps");
		return false;
	}
	close(fd);
	snprintf(out_path, out_size, "%s", tmpl);

	// Same geometry as host_loadcheck_main.cpp / host_render_main.cpp: >= 2.5 GB sparse image so it
	// formats as a valid FAT32 with 32 KB clusters (the geometry the firmware's FatFS/efatfs expects).
	const long long bytes = 2560LL << 20;
	char cmd[512];
	snprintf(cmd, sizeof cmd, "truncate -s %lld '%s'", bytes, out_path);
	if (system(cmd) != 0) {
		fprintf(stderr, "[recorder_roundtrip] truncate failed\n");
		return false;
	}
	snprintf(cmd, sizeof cmd, "mformat -i '%s' -F -c 64 ::", out_path);
	if (system(cmd) != 0) {
		fprintf(stderr, "[recorder_roundtrip] mformat failed (is mtools installed?)\n");
		return false;
	}
	return true;
}

/// Deterministic 24-bit ramp, distinct per channel, spanning near the full signed 24-bit range —
/// pseudo-random-looking (so it can't pass by coincidence, e.g. all-zero bytes) but perfectly
/// reproducible. Returned already left-shifted into a q31_t's top 24 bits (bits 31..8), because
/// SampleRecorder::feedAudio() writes exactly bytes [1,2,3] of the little-endian int32 sample (i.e.
/// bits 8..31) to the file, discarding the low byte — pre-zeroing that low byte makes the stored
/// 3-byte value byte-identical to `expected24`, with no truncation ambiguity to account for.
int32_t expected24(uint32_t frameIndex, uint32_t channel) {
	uint32_t v = (frameIndex * 97u + channel * 131u + 17u) % (1u << 24);
	return static_cast<int32_t>(v) - (1 << 23); // centre around 0, full signed 24-bit span
}

int32_t rampSample(uint32_t frameIndex, uint32_t channel) {
	return expected24(frameIndex, channel) << 8;
}

struct Case {
	const char* name;
	uint8_t channels;
	bool extraMargins;
	uint32_t frames;
};

// Read the whole file back via deluge::io::File (open/seek/read — the general-purpose, read-capable
// port; deluge::io::Stream's READ mode is documented sector_of()-only) into a plain host buffer.
bool readWholeFile(const std::string& path, std::vector<std::byte>& out) {
	auto opened = deluge::io::File::open(path, DELUGE_FILE_READ);
	if (!opened) {
		fprintf(stderr, "  FAIL: could not reopen '%s' for read (status=%d)\n", path.c_str(),
		        static_cast<int>(opened.error()));
		return false;
	}
	deluge::io::File file = std::move(opened.value());
	auto sizeResult = file.size();
	if (!sizeResult) {
		fprintf(stderr, "  FAIL: size() failed on '%s'\n", path.c_str());
		return false;
	}
	out.assign(*sizeResult, std::byte{0});
	size_t totalRead = 0;
	while (totalRead < out.size()) {
		auto chunk = file.read(std::span<std::byte>(out.data() + totalRead, out.size() - totalRead));
		if (!chunk) {
			fprintf(stderr, "  FAIL: read() failed on '%s' at offset %zu\n", path.c_str(), totalRead);
			return false;
		}
		if (chunk->empty()) {
			break; // EOF
		}
		totalRead += chunk->size();
	}
	if (totalRead != out.size()) {
		fprintf(stderr, "  FAIL: short read on '%s' (%zu of %zu bytes)\n", path.c_str(), totalRead, out.size());
		return false;
	}
	return true;
}

uint32_t readU32LE(const std::vector<std::byte>& b, size_t off) {
	return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8)
	       | (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
}

bool tagEquals(const std::vector<std::byte>& b, size_t off, const char* tag) {
	return std::memcmp(b.data() + off, tag, 4) == 0;
}

// Pump cardRoutine() until the recorder reaches COMPLETE, bounded so a stuck state machine fails the
// test instead of hanging forever.
bool pumpToComplete(SampleRecorder& rec) {
	for (int i = 0; i < 200000; i++) {
		if (rec.status.load(std::memory_order_acquire) == RecorderStatus::COMPLETE) {
			return true;
		}
		Error e = rec.cardRoutine();
		if (e != Error::NONE && e != Error::MAX_FILE_SIZE_REACHED) {
			fprintf(stderr, "  FAIL: cardRoutine() returned error %d\n", static_cast<int>(e));
			return false;
		}
	}
	fprintf(stderr, "  FAIL: cardRoutine() never reached COMPLETE\n");
	return false;
}

bool runCase(const Case& c) {
	printf("CASE %s (channels=%u margins=%d frames=%u)\n", c.name, c.channels, c.extraMargins, c.frames);
	g_cases++;

	// Build the input ramp.
	std::vector<StereoSample> input(c.frames);
	for (uint32_t i = 0; i < c.frames; i++) {
		input[i].l = rampSample(i, 0);
		input[i].r = (c.channels == 2) ? rampSample(i, 1) : 0;
	}

	SampleRecorder rec;
	Error err = rec.setup(c.channels, AudioInputChannel::MIX, /*newKeepingReasons=*/false,
	                      /*shouldRecordExtraMargins=*/c.extraMargins, AudioRecordingFolder::RESAMPLE,
	                      /*buttonPressLatency=*/0, /*outputRecordingFrom=*/nullptr);
	if (err != Error::NONE) {
		fprintf(stderr, "  FAIL: setup() returned error %d\n", static_cast<int>(err));
		return false;
	}

	rec.feedAudio(std::span<StereoSample>(input));

	// endSyncedRecording(0): with MIX-mode (numSamplesExtraToCaptureAtEndSyncingWise == 0) and no
	// button-latency compensation, numMoreSamplesToCapture is 0 when extraMargins is off (so
	// finishCapturing() runs synchronously inside this call) and kAudioClipMarginSizePostEnd when
	// extraMargins is on (status goes to CAPTURING_DATA_WAITING_TO_STOP; feed that many more silent
	// frames to push it through to finishCapturing() — mirrors the real end-of-recording sequence).
	rec.endSyncedRecording(0);
	if (rec.status.load(std::memory_order_acquire) == RecorderStatus::CAPTURING_DATA_WAITING_TO_STOP) {
		// finishCapturing() only fires once feedAudio() observes numSamplesCaptured has reached (or
		// passed) sample->lengthInSamples -- i.e. it needs a call whose span EXTENDS PAST the target,
		// not one that lands exactly on it (matching how real callers feed fixed-size audio blocks
		// and the final block naturally overshoots by less than one block). Feed a few extra silent
		// frames beyond the margin so a later iteration inside feedAudio's own do-while loop re-checks
		// the stop condition and finds it satisfied.
		std::vector<StereoSample> silence(kAudioClipMarginSizePostEnd + 64, StereoSample{0, 0});
		rec.feedAudio(std::span<StereoSample>(silence));
	}

	if (!pumpToComplete(rec)) {
		return false;
	}

	std::string path = rec.filePathCreated;
	if (path.empty()) {
		fprintf(stderr, "  FAIL: no file path recorded\n");
		return false;
	}

	std::vector<std::byte> bytes;
	if (!readWholeFile(path, bytes)) {
		return false;
	}

	const uint32_t headerLen = c.extraMargins ? 112 : 44;
	const uint32_t frameBytes = static_cast<uint32_t>(c.channels) * 3;
	// recordingExtraMargins genuinely appends kAudioClipMarginSizePostEnd extra samples past the
	// ramp (the post-end margin the feature exists to capture) — silence here, since the extra
	// frames fed above (after endSyncedRecording) are zeroed. Non-margin cases capture exactly
	// c.frames (endSyncedRecording's numMoreSamplesToCapture is 0 for those — see the call site).
	const uint32_t totalFrames = c.frames + (c.extraMargins ? static_cast<uint32_t>(kAudioClipMarginSizePostEnd) : 0);
	const uint32_t dataLen = totalFrames * frameBytes;

	bool ok = true;
	if (bytes.size() != headerLen + dataLen) {
		fprintf(stderr, "  FAIL: file size %zu != expected %u\n", bytes.size(), headerLen + dataLen);
		ok = false;
	}
	if (!tagEquals(bytes, 0, "RIFF")) {
		fprintf(stderr, "  FAIL: missing RIFF tag\n");
		ok = false;
	}
	if (!tagEquals(bytes, 8, "WAVE")) {
		fprintf(stderr, "  FAIL: missing WAVE tag\n");
		ok = false;
	}
	if (!tagEquals(bytes, 12, "fmt ")) {
		fprintf(stderr, "  FAIL: missing fmt tag\n");
		ok = false;
	}
	uint32_t riffSize = readU32LE(bytes, 4);
	if (riffSize != dataLen + headerLen - 8) {
		fprintf(stderr, "  FAIL: RIFF chunk size %u != expected %u\n", riffSize, dataLen + headerLen - 8);
		ok = false;
	}
	uint16_t fmtChannels = static_cast<uint16_t>(bytes[22]) | (static_cast<uint16_t>(bytes[23]) << 8);
	if (fmtChannels != c.channels) {
		fprintf(stderr, "  FAIL: fmt numChannels %u != expected %u\n", fmtChannels, c.channels);
		ok = false;
	}
	uint16_t bitsPerSample = static_cast<uint16_t>(bytes[34]) | (static_cast<uint16_t>(bytes[35]) << 8);
	if (bitsPerSample != 24) {
		fprintf(stderr, "  FAIL: bitsPerSample %u != 24\n", bitsPerSample);
		ok = false;
	}
	if (!tagEquals(bytes, headerLen - 8, "data")) {
		fprintf(stderr, "  FAIL: missing data tag at offset %u\n", headerLen - 8);
		ok = false;
	}
	uint32_t dataChunkSize = readU32LE(bytes, headerLen - 4);
	if (dataChunkSize != dataLen) {
		fprintf(stderr, "  FAIL: data chunk size %u != expected %u\n", dataChunkSize, dataLen);
		ok = false;
	}

	// Audio data, byte-for-byte, against the input ramp (frames >= c.frames are the silent
	// post-end margin for the extraMargins cases; expected value 0 there).
	if (bytes.size() >= static_cast<size_t>(headerLen) + dataLen) {
		size_t mismatches = 0;
		for (uint32_t i = 0; i < totalFrames && mismatches < 5; i++) {
			for (uint32_t ch = 0; ch < c.channels; ch++) {
				size_t off = headerLen + static_cast<size_t>(i) * frameBytes + static_cast<size_t>(ch) * 3;
				int32_t expected = (i < c.frames) ? expected24(i, ch) : 0;
				uint32_t got = static_cast<uint32_t>(bytes[off]) | (static_cast<uint32_t>(bytes[off + 1]) << 8)
				               | (static_cast<uint32_t>(bytes[off + 2]) << 16);
				// Sign-extend the 24-bit value read off disk for comparison against expected24()'s
				// signed range.
				int32_t gotSigned = static_cast<int32_t>(got << 8) >> 8;
				if (gotSigned != expected) {
					fprintf(stderr, "  FAIL: frame %u ch %u: got %d expected %d (offset %zu)\n", i, ch, gotSigned,
					        expected, off);
					mismatches++;
					ok = false;
				}
			}
		}
	}

	printf("  %s\n", ok ? "PASS" : "FAIL");
	return ok;
}

void deluge_recorder_roundtrip_driver() {
	static bool started = false;
	if (started) {
		return;
	}
	started = true;

	printf("Cluster::size = %zu\n", Cluster::size);

	const uint32_t clusterSize = static_cast<uint32_t>(Cluster::size);

	// Frame counts chosen relative to the runtime Cluster::size so the matrix genuinely spans
	// multiple clusters plus a partial final cluster, and also hits an exact cluster-byte-boundary
	// file length, without hardcoding a cluster size the host FAT geometry might not actually use.
	auto framesForBytes = [](uint32_t headerLen, uint32_t frameBytes, uint32_t targetBytes) -> uint32_t {
		uint32_t avail = (targetBytes > headerLen) ? (targetBytes - headerLen) : 0;
		return avail / frameBytes;
	};
	// Smallest frame count >= minFrames such that (headerLen + frames*frameBytes) lands exactly on a
	// cluster boundary (a multiple of clusterSize). frameBytes is always odd (3 or 6 is even, but 3
	// itself is coprime to the power-of-two clusterSize; 6 = 2*3, still coprime to the odd part —
	// exhaustive search over one clusterSize period always finds a solution).
	auto exactBoundaryFrames = [&](uint32_t headerLen, uint32_t frameBytes, uint32_t minFrames) -> uint32_t {
		uint32_t frames = minFrames;
		for (uint32_t tries = 0; tries < clusterSize; tries++, frames++) {
			if ((static_cast<uint64_t>(headerLen) + static_cast<uint64_t>(frames) * frameBytes) % clusterSize == 0) {
				return frames;
			}
		}
		return minFrames; // Shouldn't happen; falls back to a non-exact (still valid) case.
	};

	std::vector<Case> cases;

	// Small, well under one cluster.
	cases.push_back({"mono_small", 1, false, 137});
	cases.push_back({"stereo_small", 2, false, 211});

	// Multi-cluster with a partial final cluster (the common case).
	cases.push_back({"mono_multi_cluster_partial", 1, false, framesForBytes(44, 3, 3 * clusterSize) + 97});
	cases.push_back({"stereo_multi_cluster_partial", 2, false, framesForBytes(44, 6, 3 * clusterSize) + 53});

	// Exact cluster-boundary file length (no partial final cluster).
	cases.push_back(
	    {"mono_exact_cluster_boundary", 1, false, exactBoundaryFrames(44, 3, framesForBytes(44, 3, 2 * clusterSize))});
	cases.push_back({"stereo_exact_cluster_boundary", 2, false,
	                 exactBoundaryFrames(44, 6, framesForBytes(44, 6, 2 * clusterSize))});

	// recordingExtraMargins (112-byte header) variant, spanning multiple clusters + partial.
	cases.push_back({"mono_extra_margins", 1, true, framesForBytes(112, 3, 2 * clusterSize) + 61});
	cases.push_back({"stereo_extra_margins", 2, true, framesForBytes(112, 6, 2 * clusterSize) + 61});

	for (const Case& c : cases) {
		if (!runCase(c)) {
			g_failures++;
		}
	}

	printf("%d/%d cases passed\n", g_cases - g_failures, g_cases);

	// SR3b routing-spike sanity check (Step 4's sim-side control): probe live read-back of a
	// still-recording sample on THIS target. The spike predicts sim (no async_streaming_loader)
	// resolves READY via the fiber-pump -> RecordingReadSource path.
	uint8_t state = deluge_harness_recorder_probe(/*numChannels=*/1, /*numFrames=*/50, /*pumpDrainTicks=*/10);
	printf("recorder live-readback probe (sim): state=%u (1=READY 2=LOADING 3=UNAVAILABLE 0=harness-error)\n", state);
	deluge_harness_recorder_probe_end();
	if (state != 1) {
		fprintf(stderr, "  NOTE: sim live-readback probe did not resolve READY (state=%u)\n", state);
		g_failures++;
	}

	fflush(nullptr);
	quick_exit(g_failures == 0 ? 0 : 1);
}

} // namespace

int main(int, char**) {
	if (!format_empty_image(g_temp_image, sizeof g_temp_image)) {
		return 1;
	}
	at_quick_exit(cleanup_temp_image);
	atexit(cleanup_temp_image);
	setenv("DELUGE_SD_IMAGE", g_temp_image, 1);

	if (getenv("DELUGE_HOST_DETERMINISTIC") == nullptr) {
		setenv("DELUGE_HOST_DETERMINISTIC", "1", 1);
	}
	if (getenv("DELUGE_HOST_AUDIO") == nullptr) {
		setenv("DELUGE_HOST_AUDIO", "off", 1);
	}

	startupConditionalTask = deluge_recorder_roundtrip_driver; // override before deluge_main registers it

	deluge_platform_init();
	deluge_main(); // never returns; deluge_recorder_roundtrip_driver quick_exit()s

	cleanup_temp_image();
	return 0;
}
