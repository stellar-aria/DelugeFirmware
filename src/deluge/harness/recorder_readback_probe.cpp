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

#include "harness/recorder_readback_probe.h"

#ifdef DELUGE_HOST

#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "libdeluge/sample_source.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "storage/audio/stream/loader.h"
#include "storage/cluster/cluster.h"

#include <vector>

namespace {

SampleRecorder* g_recorder = nullptr;
DelugeSampleSource* g_source = nullptr;

int32_t rampSample(uint32_t frameIndex, uint32_t channel) {
	uint32_t v = (frameIndex * 97u + channel * 131u + 17u) % (1u << 24);
	int32_t v24 = static_cast<int32_t>(v) - (1 << 23);
	return v24 << 8;
}

void teardown() {
	if (g_source != nullptr) {
		deluge_sample_source_close(g_source);
		g_source = nullptr;
	}
	if (g_recorder != nullptr) {
		// abort() is the correct teardown for a recorder that's still CAPTURING_DATA -- it marks
		// the recorder for file/sample cleanup on its next cardRoutine() tick (see cardRoutine()'s
		// ABORTED branch); we don't drive that drain here, this is a one-shot diagnostic process.
		g_recorder->abort();
		delete g_recorder;
		g_recorder = nullptr;
	}
}

} // namespace

extern "C" {

uint8_t deluge_harness_recorder_probe(uint8_t numChannels, uint32_t numFrames, uint32_t pumpDrainTicks) {
	teardown(); // idempotent; drop any previous probe first

	g_recorder = new SampleRecorder();
	Error err = g_recorder->setup(numChannels, AudioInputChannel::MIX, /*newKeepingReasons=*/false,
	                              /*shouldRecordExtraMargins=*/false, AudioRecordingFolder::RESAMPLE,
	                              /*buttonPressLatency=*/0, /*outputRecordingFrom=*/nullptr);
	if (err != Error::NONE) {
		teardown();
		return 0;
	}

	std::vector<StereoSample> input(numFrames);
	for (uint32_t i = 0; i < numFrames; i++) {
		input[i].l = rampSample(i, 0);
		input[i].r = (numChannels == 2) ? rampSample(i, 1) : 0;
	}
	g_recorder->feedAudio(std::span<StereoSample>(input));

	// Drive the fiber drain a few times BEFORE probing, same as the sim round-trip harness does --
	// this is what actually flushes the fed audio to the SD file (writeOneCompletedCluster()), which
	// is a precondition for RecordingReadSource to have anything to read on the sim's synchronous
	// path. Deliberately do NOT call endSyncedRecording()/finalize: status stays CAPTURING_DATA, so
	// the Sample genuinely has no efatfs read handle for the whole probe.
	for (uint32_t i = 0; i < pumpDrainTicks; i++) {
		g_recorder->cardRoutine();
	}

	Sample* sample = g_recorder->sample;
	DelugeSampleGeometry geometry{};
	geometry.audio_data_start_bytes = sample->audioDataStartPosBytes;
	geometry.audio_data_length_bytes = sample->audioDataLengthBytes; // the still-recording sentinel
	geometry.cluster_size_bytes = static_cast<uint32_t>(Cluster::size);
	geometry.byte_depth = sample->byteDepth;
	geometry.num_channels = sample->numChannels;
	geometry.raw_data_format = static_cast<uint8_t>(sample->rawDataFormat);

	g_source = deluge_sample_source_open(&sample->stream(), geometry);
	if (g_source == nullptr) {
		return 0;
	}

	DelugeSampleRegion out{};
	DelugeRegionState state = deluge_sample_region_acquire_ex(g_source, /*index=*/0, /*direction=*/+1,
	                                                          /*priority=*/0, &out);

	// A first CLUSTER_ENQUEUE-style acquire is expected to come back LOADING (the fill is scheduled,
	// not synchronous) -- retry a bounded number of times, driving the C++ fiber loader drain
	// ourselves in between (deluge::audio::stream::loader::pump()) exactly as the sim's own scheduler
	// task would. This is a NO-OP when deluge_streaming_async_active() is true (loader.cpp's pump()
	// early-returns, per the SR3b routing spike) -- i.e. on host_app this loop will not by itself
	// resolve the state; that's the point being probed, not a harness bug. See
	// deluge_harness_recorder_probe_poll() for the Rust-driven retry that also lets the ASYNC fill
	// task (the thing that actually owns the drain on host_app) run between checks.
	for (int i = 0; i < 32 && state == DELUGE_REGION_LOADING; i++) {
		deluge::audio::stream::loader::pump();
		state = deluge_sample_region_acquire_ex(g_source, /*index=*/0, /*direction=*/+1, /*priority=*/0, &out);
	}

	return static_cast<uint8_t>(state);
}

uint8_t deluge_harness_recorder_probe_poll() {
	if (g_source == nullptr) {
		return static_cast<uint8_t>(DELUGE_REGION_UNAVAILABLE);
	}
	// Drive the C++ fiber drain (a no-op under async_streaming_loader; see the comment in
	// deluge_harness_recorder_probe()) and re-acquire (not just deluge_sample_region_state(), which
	// never itself progresses a fill) so a caller polling this in a loop actually gives EITHER drain
	// mechanism -- the C++ pump on sim, or (by calling this repeatedly while its own async executor
	// keeps ticking) the Rust async fill task on host_app -- a real chance to resolve the region.
	deluge::audio::stream::loader::pump();
	DelugeSampleRegion out{};
	DelugeRegionState state =
	    deluge_sample_region_acquire_ex(g_source, /*index=*/0, /*direction=*/+1, /*priority=*/0, &out);
	return static_cast<uint8_t>(state);
}

void deluge_harness_recorder_probe_end() {
	teardown();
}

} // extern "C"

#endif // DELUGE_HOST
