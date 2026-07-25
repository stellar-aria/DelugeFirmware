/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#pragma once

#include "definitions_cxx.hpp"
#include "dsp/envelope_follower/absolute_value.h"
#include "dsp/stereo_sample.h"
#include "io/stream.hpp"
#include "memory/fast_allocator.h"
#include "util/segmented_vector.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <gsl/gsl>
#include <optional>
#include <span>
#include <string>

enum class MonitoringAction {
	NONE = 0,
	REMOVE_RIGHT_CHANNEL = 1,
	SUBTRACT_RIGHT_CHANNEL = 2,
};

enum class RecorderStatus {
	CAPTURING_DATA = 0,
	CAPTURING_DATA_WAITING_TO_STOP = 1,
	FINISHED_CAPTURING_BUT_STILL_WRITING = 2,
	COMPLETE = 3,

	// Means RAM error only. SD errors are noted separately and won't affect operation, as long as RAM lasts
	ABORTED = 4,
	AWAITING_DELETION = 5,
};

class Sample;
struct StreamedChunk; // file-backed streamed sample-audio chunk (see storage/cluster/cluster.h)
class AudioClip;
class Output;
struct RecorderConfig {
	bool neverUseThreshold = false;
};

class SampleRecorder {
public:
	SampleRecorder() = default;
	~SampleRecorder();
	Error setup(int32_t newNumChannels, AudioInputChannel newMode, bool newKeepingReasons,
	            bool shouldRecordExtraMargins, AudioRecordingFolder newFolderID, int32_t buttonPressLatency,
	            Output* outputRecordingFrom, RecorderConfig config = {});
	void setRecordingThreshold(RecorderConfig config);
	void feedAudio(std::span<StereoSample> input, bool applyGain = false, uint8_t gainToApply = 5);
	Error cardRoutine();
	void endSyncedRecording(int32_t buttonLatencyForTempolessRecording);
	bool inputLooksDifferential();
	bool inputHasNoRightChannel();
	void removeFromOutput() {
		if (status < RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
			abort();
		}
		outputRecordingFrom = nullptr;
	};
	void abort();

	SampleRecorder* next{};

	gsl::owner<Sample*> sample{};

	int32_t numSamplesToRunBeforeBeginningCapturing{};
	uint32_t numSamplesBeenRunning{};
	uint32_t numSamplesCaptured{};

	uint32_t numSamplesExtraToCaptureAtEndSyncingWise{};

	int32_t firstUnwrittenClusterIndex = 0;

	// Put things in valid state so if we get destructed before any recording, it's all ok.
	// Atomic: the producer (audio) publishes a completed buffer via a release store at createNextCluster;
	// the consumer (fiber) reads it acquire as its drain bound -- the index into our own private
	// bufferTable_ (see below), not a shared residency table. See docs/dev/known-concurrency-bugs.md (B3).
	std::atomic<int32_t> currentRecordClusterIndex = -1;

	// SR3b: bytes flushed to SD, release-advanced by the fiber (writeCluster()) after each successful
	// write_at, replacing currentRecordClusterIndex + shared-table growth as the published growth
	// signal. This is the live extent of the recording a reader may consult -- publishing it here is
	// this task's whole job; wiring a reader to it is a later task (see the SR3b design doc).
	std::atomic<uint64_t> committedBytes{0};

	uint32_t audioFileNumber{};
	AudioRecordingFolder folderID;

	char* writePos{};
	char* clusterEndPos{};

	// When this gets set, we add the Sample to the master list. This is stored here in addition to in the Sample,
	// so we can delete an aborted file even after the Sample has been detached / destructed.
	// This will be the temp file path if there is one.
	std::string filePathCreated{};

	// Atomic: the finishCapturing (audio) -> ABORTED (either thread) transitions are release stores; the
	// fiber's cardRoutine() decision reads are acquire loads, so seeing FINISHED_CAPTURING_BUT_STILL_WRITING
	// (or ABORTED) also makes visible the producer's final currentRecordClusterIndex/payload writes before
	// the fiber takes over as producer in finalizeRecordedFile(). See docs/dev/known-concurrency-bugs.md (B3).
	std::atomic<RecorderStatus> status = RecorderStatus::CAPTURING_DATA;
	static_assert(std::atomic<RecorderStatus>::is_always_lock_free);
	AudioInputChannel mode;
	Output* outputRecordingFrom{}; // for when recording from a specific output

	// Need to keep track of this, so we know whether to remove it. Well I guess we could just look and see if it's
	// there... but this is nice.
	bool haveAddedSampleToArray = false;

	bool allowFileAlterationAfter = false;
	bool allowNormalization = true;
	bool autoDeleteWhenDone = false;
	bool keepingReasonsForFirstClusters{};
	uint8_t recordingNumChannels{};
	bool hadCardError = false;
	bool reachedMaxFileSize = false;
	bool recordingExtraMargins = false;
	bool pointerHeldElsewhere = false;
	bool capturedTooMuch = false;
	bool thresholdRecording = false;
	uint32_t minThresholdMargin = 0;

	// Most of these are not captured in the case of BALANCED input for AudioClips
	bool recordingClippedRecently{};
	int32_t recordPeakL{};
	int32_t recordPeakR{};
	int32_t recordPeakLMinusR{};
	uint64_t recordSumL{};
	uint64_t recordSumR{};
	uint64_t recordSumLPlusR{};  // L and R are halved before these two are calculated
	uint64_t recordSumLMinusR{}; // --------

	int32_t recordMax{};
	int32_t recordMin{};

	uint32_t audioDataLengthBytesAsWrittenToFile{};
	uint32_t loopEndSampleAsWrittenToFile{};

	float startValueThreshold{};

	int32_t* sourcePos{};

	// NOTE (Kate, deluge-stream boundary migration): this used to be an inline
	// std::optional<FatFS::File> member, then briefly a heap-allocated DelugeStream*
	// handle behind stream_io.h (Task 8), and is now back to an inline
	// std::optional<deluge::io::Stream> RAII wrapper over that same handle (Task 10).
	// The write-dispatch logic itself was confirmed structurally identical
	// pre/post-Task-8-migration (same buffer, same underlying FatFS calls, just
	// relocated behind the boundary), but that type change (inline optional -> heap
	// pointer) altered SampleRecorder's object size and allocation timing. One
	// `highsiderr` TRACK-mode golden-master fixture (of 5 in that session) showed
	// small-magnitude PCM sample differences against a pre-migration A/B (same file
	// size/WAV structure, converging back to identical near the end) -- suspected to
	// be this layout/allocation-timing shift interacting with this fixture's
	// documented prior history of repitch/time-stretch fragility under unrelated
	// code-shape changes, not a genuine regression. Reviewed and knowingly accepted
	// rather than further bisected (a proposed isolation test -- padding
	// SampleRecorder back to its old size -- was not run). If you're chasing a
	// highsiderr-adjacent audio bug, start here.
	std::optional<deluge::io::Stream> file;

private:
	void setExtraBytesOnPreviousCluster(StreamedChunk* currentCluster, int32_t currentClusterIndex);
	Error writeCluster(int32_t clusterIndex, size_t numBytes);
	Error alterFile(MonitoringAction action, int32_t lshiftAmount, uint32_t idealFileSizeBeforeAction,

	                uint64_t dataLengthAfterAction);
	Error finalizeRecordedFile();
	Error createNextCluster();
	Error writeAnyCompletedClusters();
	void finishCapturing();
	void updateDataLengthInHeader(std::span<std::byte> headerBuf);
	void totalSampleLengthNowKnown(uint32_t totalLength, uint32_t loopEndPointSamples = 0);
	void detachSample();
	Error truncateFileDownToSize(uint32_t newFileSize);
	Error writeOneCompletedCluster();

	// SR3b: a minimal fixed-capacity lock-free SPSC ring of recycled buffer pointers, private to
	// SampleRecorder rather than deluge::util::SpscRing -- that header declares `namespace
	// deluge::util`, which collides with this codebase's separate top-level `namespace util`
	// (util/misc.h) wherever both become visible in the same translation unit, and sample_recorder.h
	// is included far too widely to risk that. Same release/acquire discipline as deluge::util::SpscRing
	// (see that header's doc for the full reasoning): the producer (fiber, recycleBuffer())
	// release-stores tail_ after writing a slot; the consumer (audio thread, allocateBuffer())
	// acquire-loads tail_ before reading a slot and release-stores head_ after freeing it, which the
	// producer acquire-loads to know the slot is free again.
	class RecycleRing {
	public:
		static constexpr std::size_t kCapacity = 16; // must be a power of two
		[[nodiscard]] bool try_push(std::byte* value) {
			const std::size_t tail = tail_.load(std::memory_order_relaxed);
			const std::size_t head = head_.load(std::memory_order_acquire);
			if (tail - head == kCapacity) {
				return false; // full
			}
			slots_[tail & kMask] = value;
			tail_.store(tail + 1, std::memory_order_release);
			return true;
		}
		[[nodiscard]] bool try_pop(std::byte*& out) {
			const std::size_t head = head_.load(std::memory_order_relaxed);
			const std::size_t tail = tail_.load(std::memory_order_acquire);
			if (head == tail) {
				return false; // empty
			}
			out = slots_[head & kMask];
			head_.store(head + 1, std::memory_order_release);
			return true;
		}

	private:
		static constexpr std::size_t kMask = kCapacity - 1;
		std::array<std::byte*, kCapacity> slots_{};
		std::atomic<std::size_t> head_{0};
		std::atomic<std::size_t> tail_{0};
	};

	// SR3b: the recorder's own private capture buffers -- Cluster::size (+ trailing overshoot slack)
	// plain allocations, never registered with the shared residency table. `bufferTable_` maps a
	// buffer's logical index (the same index space `currentRecordClusterIndex`/
	// `firstUnwrittenClusterIndex` already walk) to its physical storage; growth (audio-thread-only,
	// single-writer) is pre-`reserve`d up front in setup() so it never reallocates the segment-pointer
	// index concurrently with the fiber's `operator[]` reads -- same B2 discipline the shared table
	// used to require. `freeBuffers_` recycles a flushed buffer's memory back for reuse: the fiber
	// pushes after a successful write_at, the audio thread pops when it needs a fresh buffer. Either
	// side of that ring can "miss" without correctness cost -- a full push just frees the buffer
	// outright, an empty pop just allocates fresh -- so buffer *reuse* is best-effort while buffer
	// *availability* (the audio thread never blocks/drops) is unconditional.
	deluge::SegmentedVector<std::byte*, 256, deluge::memory::fast_allocator> bufferTable_{};
	RecycleRing freeBuffers_{};
	std::byte* currentRecordBuffer = nullptr; // the buffer at bufferTable_[currentRecordClusterIndex]

	[[nodiscard]] std::byte* allocateBuffer();
	void recycleBuffer(std::byte* buffer);
	void releaseCaptureBuffers();

	AbsValueFollower envelopeFollower{};
};
