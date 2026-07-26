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

// SR3b Task 3's regression gate (see harness/recorder_readback_probe.h): "shared
// SampleStream::table_ left under-sized after a normal recording finishes" -- a real,
// FINALIZED (not still-recording) multi-cluster recording, read back through the exact
// deluge_sample_source_* region port real playback uses.
uint8_t deluge_harness_recorder_finalized_multicluster_probe(uint8_t numChannels, uint32_t numFrames,
                                                             uint32_t regionIndex);
uint32_t deluge_harness_recorder_finalized_multicluster_probe_table_clusters();
uint32_t deluge_harness_recorder_finalized_multicluster_probe_expected_clusters();
uint8_t deluge_harness_recorder_finalized_multicluster_probe_bytes_ok();
void deluge_harness_recorder_finalized_multicluster_probe_end();
}

namespace {

char g_temp_image[512] = {0};
char g_sd_root[512] = {0};
int g_failures = 0;
int g_cases = 0;

void cleanup_temp_image() {
	if (g_temp_image[0] != '\0') {
		unlink(g_temp_image);
		g_temp_image[0] = '\0';
	}
}

void cleanup_sd_root() {
	if (g_sd_root[0] != '\0') {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_sd_root);
		if (system(cmd) != 0) {
			// Best-effort cleanup; not a test failure.
		}
		g_sd_root[0] = '\0';
	}
}

// SR3b Task 3's regression gate (see recorder_readback_probe.cpp's mirrorFinalizedFileToSdRoot()):
// this binary's streaming read path (open_read_stream(), via host_efatfs_passthrough.cpp) is
// backed by a plain POSIX DELUGE_SD_ROOT directory, separate from the mounted FAT image
// (DELUGE_SD_IMAGE) the recorder writes into -- set one up so
// deluge_harness_recorder_finalized_multicluster_probe() can mirror a finalized file there and
// open a genuine streaming-read cursor on it.
bool make_sd_root(char* out_path, size_t out_size) {
	char tmpl[] = "/tmp/deluge_recorder_roundtrip_sdroot_XXXXXX";
	char* dir = mkdtemp(tmpl);
	if (dir == nullptr) {
		perror("[recorder_roundtrip] mkdtemp");
		return false;
	}
	snprintf(out_path, out_size, "%s", dir);
	return true;
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

// Byte-exact characterization of `SampleRecorder::alterFile()` — the post-capture, in-place file
// transform that downmixes/normalizes a just-recorded WAV (sample_recorder.cpp, currently
// cluster-based; being re-expressed as a positional file->file transform). Records a KNOWN ramp
// through the SAME trusted feedAudio()/finalize() pipeline the round-trip cases above use
// (`allowFileAlterationAfter` left false, so finalizeRecordedFile() takes its no-alteration branch
// and the file lands on disk exactly as fed — already proven byte-exact by the cases above), then
// calls `alterFile()` DIRECTLY with an explicit action/lshiftAmount/geometry, bypassing
// finalizeRecordedFile()'s auto-detection heuristics entirely (SUBTRACT_RIGHT_CHANNEL in particular
// needs a plugged-in line-input jack the host sim can't simulate). The expected output bytes are
// computed HERE, independently, straight from the per-frame algorithm alterFile() implements
// (read a 24-bit little-endian sample with its low byte cleared, combine per `action`, left-shift by
// `lshiftAmount`, keep the top 3 bytes) — not by calling alterFile() itself — so a byte-for-byte
// match proves the implementation matches the spec, not just "does what its own code does".
bool runAlterFileCase(const char* name, MonitoringAction action, int32_t lshiftAmount, uint8_t channels,
                      uint32_t frames) {
	printf("CASE alterFile_%s (channels=%u frames=%u lshift=%d)\n", name, channels, frames, lshiftAmount);
	g_cases++;

	std::vector<StereoSample> input(frames);
	for (uint32_t i = 0; i < frames; i++) {
		input[i].l = rampSample(i, 0);
		input[i].r = (channels == 2) ? rampSample(i, 1) : 0;
	}

	SampleRecorder rec;
	Error err = rec.setup(channels, AudioInputChannel::MIX, /*newKeepingReasons=*/false,
	                      /*shouldRecordExtraMargins=*/false, AudioRecordingFolder::RESAMPLE,
	                      /*buttonPressLatency=*/0, /*outputRecordingFrom=*/nullptr);
	if (err != Error::NONE) {
		fprintf(stderr, "  FAIL: setup() returned error %d\n", static_cast<int>(err));
		return false;
	}

	rec.feedAudio(std::span<StereoSample>(input));
	rec.endSyncedRecording(0); // MIX-mode, no button latency -> finishCapturing() runs synchronously.

	if (!pumpToComplete(rec)) {
		return false;
	}

	std::string path = rec.filePathCreated;
	if (path.empty()) {
		fprintf(stderr, "  FAIL: no file path recorded\n");
		return false;
	}

	// Read back the as-recorded (pre-alteration) file. Its header and audio bytes are already
	// proven byte-exact by the plain round-trip cases above, so it's a trustworthy baseline for the
	// header fields alterFile() does NOT touch (RIFF/WAVE/fmt tags, sample rate, bits-per-sample).
	std::vector<std::byte> recorded;
	if (!readWholeFile(path, recorded)) {
		return false;
	}

	const uint32_t headerLen = 44; // shouldRecordExtraMargins=false above.
	const uint32_t frameBytesIn = static_cast<uint32_t>(channels) * 3;
	const uint32_t dataLenBefore = frames * frameBytesIn;
	const uint32_t idealFileSizeBeforeAction = headerLen + dataLenBefore;
	const uint32_t dataLenAfter = (action != MonitoringAction::NONE) ? (dataLenBefore >> 1) : dataLenBefore;

	if (recorded.size() != idealFileSizeBeforeAction) {
		fprintf(stderr, "  FAIL: pre-alteration file size %zu != expected %u\n", recorded.size(),
		        idealFileSizeBeforeAction);
		return false;
	}

	// --- Build the expected POST-alteration file, analytically, independent of alterFile() itself. ---
	std::vector<std::byte> expected(headerLen + dataLenAfter);
	std::memcpy(expected.data(), recorded.data(), headerLen);

	if (action != MonitoringAction::NONE) {
		uint16_t numCh = 1;
		std::memcpy(expected.data() + 22, &numCh, 2);
		uint32_t dataRate = kSampleRate * 1 * 3;
		std::memcpy(expected.data() + 28, &dataRate, 4);
		uint16_t blockSize = 1 * 3;
		std::memcpy(expected.data() + 32, &blockSize, 2);
	}
	// updateDataLengthInHeader() always runs, regardless of `action`.
	uint32_t riffSize = dataLenAfter + headerLen - 8;
	std::memcpy(expected.data() + 4, &riffSize, 4);
	std::memcpy(expected.data() + (headerLen - 4), &dataLenAfter, 4);

	// Audio: per-frame transform per alterFile()'s documented algorithm. `rampSample(i, ch)` is
	// exactly the int32 value alterFile() reconstructs when it reads the stored 3 bytes back
	// (`*(int32_t*)(readPos-1) & 0xFFFFFF00`) — the round-trip cases above already establish the
	// on-disk 3 bytes are the top 3 bytes of `rampSample(i, ch)` byte-for-byte, and its low byte is
	// always 0 already, so no information is lost reconstructing it.
	for (uint32_t i = 0; i < frames; i++) {
		int32_t left = rampSample(i, 0);
		int32_t value = left;
		if (action == MonitoringAction::SUBTRACT_RIGHT_CHANNEL) {
			int32_t right = rampSample(i, 1);
			value = (left >> 1) - (right >> 1);
		}
		// REMOVE_RIGHT_CHANNEL discards the right channel without reading it into `value`; NONE never
		// has a right channel to begin with (channels == 1 for that case).
		uint32_t processed = static_cast<uint32_t>(value) << lshiftAmount;
		uint32_t topBytes = processed >> 8; // bytes [1,2,3] of `processed`, little-endian.
		std::byte* out = expected.data() + headerLen + static_cast<size_t>(i) * 3;
		out[0] = static_cast<std::byte>(topBytes & 0xFF);
		out[1] = static_cast<std::byte>((topBytes >> 8) & 0xFF);
		out[2] = static_cast<std::byte>((topBytes >> 16) & 0xFF);
	}

	Error alterErr = rec.alterFile(action, lshiftAmount, idealFileSizeBeforeAction, dataLenAfter);
	if (alterErr != Error::NONE) {
		fprintf(stderr, "  FAIL: alterFile() returned error %d\n", static_cast<int>(alterErr));
		return false;
	}

	std::vector<std::byte> got;
	if (!readWholeFile(path, got)) {
		return false;
	}

	bool ok = true;
	if (got.size() != expected.size()) {
		fprintf(stderr, "  FAIL: altered file size %zu != expected %zu\n", got.size(), expected.size());
		ok = false;
	}
	else if (std::memcmp(got.data(), expected.data(), expected.size()) != 0) {
		size_t firstDiff = 0;
		for (; firstDiff < expected.size(); firstDiff++) {
			if (got[firstDiff] != expected[firstDiff]) {
				break;
			}
		}
		fprintf(stderr, "  FAIL: byte mismatch at offset %zu (got 0x%02x expected 0x%02x)\n", firstDiff,
		        static_cast<unsigned>(got[firstDiff]), static_cast<unsigned>(expected[firstDiff]));
		ok = false;
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

	// alterFile() byte-exact characterization: one case per MonitoringAction, each spanning several
	// clusters (audio bytes > 2 * clusterSize) so a cluster-boundary-straddling frame is genuinely
	// exercised against the current cluster-based implementation.
	if (!runAlterFileCase("remove_right_channel", MonitoringAction::REMOVE_RIGHT_CHANNEL, /*lshiftAmount=*/0,
	                      /*channels=*/2, framesForBytes(44, 6, 3 * clusterSize) + 777)) {
		g_failures++;
	}
	if (!runAlterFileCase("subtract_right_channel", MonitoringAction::SUBTRACT_RIGHT_CHANNEL, /*lshiftAmount=*/0,
	                      /*channels=*/2, framesForBytes(44, 6, 3 * clusterSize) + 555)) {
		g_failures++;
	}
	if (!runAlterFileCase("none_with_lshift", MonitoringAction::NONE, /*lshiftAmount=*/4, /*channels=*/1,
	                      framesForBytes(44, 3, 3 * clusterSize) + 999)) {
		g_failures++;
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

	// SR3b Task 3 regression gate: a real, FINALIZED (COMPLETE) multi-cluster mono recording --
	// finalizeRecordedFile()'s no-alteration else-branch, the only branch AudioClip recording ever
	// takes -- must leave SampleStream::table_ sized to the real cluster count, and region index 1+
	// must resolve READY with correct bytes through the same port real playback uses. On the
	// unfixed commit 5bb397c2b, table_ stays at its Sample::initialize(1) default (tableClusters==1
	// while expectedClusters is several), demonstrating the regression this gate exists to catch.
	g_cases++;
	uint32_t regressionFrames = framesForBytes(44, 3, 3 * clusterSize) + 400;
	printf("CASE finalized_multicluster_regression (channels=1 margins=0 frames=%u regionIndex=1)\n", regressionFrames);
	uint8_t regressionState =
	    deluge_harness_recorder_finalized_multicluster_probe(/*numChannels=*/1, regressionFrames, /*regionIndex=*/1);
	uint32_t tableClusters = deluge_harness_recorder_finalized_multicluster_probe_table_clusters();
	uint32_t expectedClusters = deluge_harness_recorder_finalized_multicluster_probe_expected_clusters();
	uint8_t bytesOk = deluge_harness_recorder_finalized_multicluster_probe_bytes_ok();
	printf("  table_clusters()=%u expected_clusters()=%u acquire_state=%u (1=READY) bytes_ok=%u\n", tableClusters,
	       expectedClusters, regressionState, bytesOk);
	deluge_harness_recorder_finalized_multicluster_probe_end();
	bool regressionOk =
	    (tableClusters != 0) && (tableClusters >= expectedClusters) && (regressionState == 1) && (bytesOk == 1);
	if (!regressionOk) {
		fprintf(stderr,
		        "  FAIL: finalized multi-cluster regression gate -- table_clusters=%u expected_clusters=%u "
		        "acquire_state=%u bytes_ok=%u\n",
		        tableClusters, expectedClusters, regressionState, bytesOk);
		g_failures++;
	}
	printf("  %s\n", regressionOk ? "PASS" : "FAIL");

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

	if (!make_sd_root(g_sd_root, sizeof g_sd_root)) {
		return 1;
	}
	at_quick_exit(cleanup_sd_root);
	atexit(cleanup_sd_root);
	setenv("DELUGE_SD_ROOT", g_sd_root, 1);

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
	cleanup_sd_root();
	return 0;
}
