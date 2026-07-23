/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
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

#include "arm_neon_shim.h"

#include "definitions_cxx.hpp"
#include "dsp/interpolate/interpolate.h"
#include "libdeluge/sample_source.h"
#include <array>
#include <cstdint>
#define REASSESSMENT_ACTION_STOP_OR_LOOP 0
#define REASSESSMENT_ACTION_NEXT_CLUSTER 1

class VoiceSamplePlaybackGuide;
class Voice;
class Sample;
struct StreamedChunk;      // file-backed streamed sample-audio chunk (see storage/cluster/cluster.h)
struct DelugeSampleSource; // per-reader residency cursor over the region port (libdeluge/sample_source.h)
class TimeStretcher;
class SamplePlaybackGuide;

class SampleLowLevelReader {
public:
	SampleLowLevelReader() = default;
	virtual ~SampleLowLevelReader();
	explicit SampleLowLevelReader(SampleLowLevelReader&, bool stealReasons = false);
	SampleLowLevelReader(SampleLowLevelReader&& other) noexcept;
	SampleLowLevelReader& operator=(const SampleLowLevelReader& other) = delete;
	SampleLowLevelReader& operator=(SampleLowLevelReader&& other) noexcept;

	void unassignAllReasons(bool wontBeUsedAgain);
	void jumpForwardLinear(int32_t numChannels, int32_t byteDepth, uint32_t bitMask, int32_t jumpAmount,
	                       int32_t phaseIncrement);
	void jumpForwardZeroes(int32_t bufferSize, int32_t numChannels, int32_t phaseIncrement);
	void fillInterpolationBufferRetrospectively(Sample* sample, int32_t bufferSize, int32_t startI,
	                                            int32_t playDirection);
	void jumpBackSamples(Sample* sample, int32_t numToJumpBack, int32_t playDirection);
	void setupForPlayPosMovedIntoNewCluster(SamplePlaybackGuide* guide, Sample* sample, char* clusterBase,
	                                        int32_t bytePosWithinNewCluster, int32_t byteDepth);
	bool setupClusersForInitialPlay(SamplePlaybackGuide* guide, Sample* sample, int32_t byteOvershoot = 0,
	                                bool justLooped = false, int32_t priorityRating = 1);
	bool moveOnToNextCluster(SamplePlaybackGuide* guide, Sample* sample, int32_t priorityRating = 1);
	bool changeClusterIfNecessary(SamplePlaybackGuide* guide, Sample* sample, bool loopingAtLowLevel,
	                              int32_t priorityRating = 1);
	bool considerUpcomingWindow(SamplePlaybackGuide* guide, Sample* sample, int32_t* numSamples, int32_t phaseIncrement,
	                            bool loopingAtLowLevel, int32_t bufferSize, bool allowEndlessSilenceAtEnd = false,
	                            int32_t priorityRating = 1);
	void setupReassessmentLocation(SamplePlaybackGuide* guide, Sample* sample);
	void misalignPlaybackParameters(Sample* sample);
	void realignPlaybackParameters(Sample* sample);
	bool reassessReassessmentLocation(SamplePlaybackGuide* guide, Sample* sample, int32_t priorityRating);
	int32_t getPlayByteLowLevel(Sample* sample, SamplePlaybackGuide* guide,
	                            bool compensateForInterpolationBuffer = false);

	bool setupClustersForPlayFromByte(SamplePlaybackGuide* guide, Sample* sample, int32_t startPlaybackAtByte,
	                                  int32_t priorityRating);

	[[nodiscard]] virtual bool shouldObeyMarkers() const { return false; }

	void readSamplesNative(int32_t** __restrict__ oscBufferPos, int32_t numSamplesTotal, Sample* sample,
	                       int32_t jumpAmount, int32_t numChannels, int32_t numChannelsAfterCondensing,
	                       int32_t* amplitude, int32_t amplitudeIncrement, TimeStretcher* timeStretcher = nullptr,
	                       bool bufferingToTimeStretcher = false);

	void readSamplesResampled(int32_t** __restrict__ oscBufferPos, int32_t numSamples, Sample* sample,
	                          int32_t jumpAmount, int32_t numChannels, int32_t numChannelsAfterCondensing,
	                          int32_t phaseIncrement, int32_t* amplitude, int32_t amplitudeIncrement,
	                          int32_t bufferSize, bool writingCache, char** __restrict__ cacheWritePos,
	                          bool* doneAnySamplesYet, TimeStretcher* timeStretcher, bool bufferingToTimeStretcher,
	                          int32_t whichKernel);

	bool readSamplesForTimeStretching(int32_t* oscBufferPos, SamplePlaybackGuide* guide, Sample* sample,
	                                  int32_t numSamples, int32_t numChannels, int32_t numChannelsAfterCondensing,
	                                  int32_t phaseIncrement, int32_t amplitude, int32_t amplitudeIncrement,
	                                  bool loopingAtLowLevel, int32_t jumpAmount, int32_t bufferSize,
	                                  TimeStretcher* timeStretcher, bool bufferingToTimeStretcher,
	                                  int32_t whichPlayHead, int32_t whichKernel, int32_t priorityRating);
	void steal_clusters(SampleLowLevelReader& other, bool stealReasons);

	void bufferIndividualSampleForInterpolation(int32_t numChannels, int32_t byteDepth, char* playPosNow);
	void bufferZeroForInterpolation(int32_t numChannels);

	uint32_t oscPos{};
	char* currentPlayPos{};
	char* reassessmentLocation{};
	char* clusterStartLocation{}; // You're allowed to read from this location, but not move any further "back" past it
	uint8_t reassessmentAction{};
	int8_t interpolationBufferSizeLastTime{}; // 0 if was previously switched off

	deluge::dsp::Interpolator interpolator_{};

	// SR1 Task 8: the region port (source_/region_ below) is the reader's residency seam for
	// steady-state uncached playback -- where region_ mirrors clusters[0] exactly (i023) -- and the
	// reader's own interpolation-window logic (setupReassessmentLocation / setup*ForPlay*) now sources
	// its base + cluster index from region_, not this array. `clusters[]` is NOT retired, though: it
	// stays as the reader's *authoritative* current-cluster residency for the paths the port does not
	// own, and its leases (add_lease at each port acquire; remove_reason in unassignAllReasons) remain
	// the single uniform release those paths depend on. It is still read/populated by:
	//   * attemptLateSampleStart's WAIT-probe -- the FAILURE-vs-WAIT-vs-defer decision reads
	//     clusters[0]/[1] + ->loaded directly (the port's bool acquire cannot express it) and holds the
	//     probe leases across a WAIT. Golden-UNVALIDATED (the deterministic sim has no SD latency), so
	//     deliberately left on clusters[].
	//   * VoiceSample::render's cache-resync tracking + stopReadingFromCache -- while replaying a
	//     repitch/time-stretch cache the reader tracks the uncached resume cluster in clusters[]
	//     (get_cluster, NOT the port), so region_ is STALE there; getPlayByteLowLevel and
	//     reassessReassessmentLocation's pre-reacquire reads therefore also stay on clusters[0].
	//   * external presence consumers -- reads made by code outside this class hierarchy (VoiceSample's
	//     own clusters[] reads, covered by the bullets above, are internal): TimeStretcher's
	//     olderPartReader.clusters[0]/voiceSample->clusters[0] presence checks (time_stretcher.cpp
	//     ~570, ~880, ~1015) and SamplePlaybackGuide::adjustPitchToCorrectDriftFromSync's
	//     voiceSample->clusters[0] "clusters not set up yet" guard (sample_playback_guide.cpp ~131).
	//     voice.cpp itself no longer reads clusters[] at all (SR1 Task 8 removed its last read).
	// Fully retiring clusters[] would require routing those port-uncovered paths through the port so
	// region_ becomes authoritative during cache/probe too -- deferred (see SR1 Task 8 report).
	std::array<StreamedChunk*, kNumClustersLoadedAhead> clusters = {nullptr, nullptr};

private:
	// SR1 Task 4: residency acquisition goes through the region port (libdeluge/sample_source.h)
	// instead of a per-slot SampleStream::get_cluster() loop. `source_` is a per-reader cursor opened
	// lazily against the sample's `stream()` (see ensureSource); it holds the current + prefetch pins.
	DelugeSampleSource* source_ = nullptr;
	void* source_backing_ = nullptr; ///< the &sample->stream() `source_` was opened against (reader reuse)

	// SR1 Task 6: the last region acquired through the port (assignClusters / moveOnToNextCluster). Its
	// `payload_base` is the source of the interpolation window's base pointer -- the `clusterStartLocation`
	// look-behind floor and the `reassessmentLocation` trailing/front slack reach are computed from it in
	// setupReassessmentLocation(), instead of reaching through the clusters[0] mirror into
	// StreamedChunk::payload(). By the port contract region_.payload_base == clusters[0]->payload().data()
	// (asserted there), so the interpolation DSP stays byte-for-byte identical; only the base's *source*
	// moves onto the region port.
	DelugeSampleRegion region_{};

	/// @brief Open `source_` once for @p sample (re-opening if the reader is reused for a new sample).
	void ensureSource(Sample* sample);

	bool assignClusters(SamplePlaybackGuide* guide, Sample* sample, int32_t clusterIndex, int32_t priorityRating);
	bool fillInterpolationBufferForward(SamplePlaybackGuide* guide, Sample* sample, int32_t interpolationBufferSize,
	                                    bool loopingAtLowLevel, int32_t numSpacesToFill, int32_t priorityRating);
};
