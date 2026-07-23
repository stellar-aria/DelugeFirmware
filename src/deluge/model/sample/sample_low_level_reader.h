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

	std::array<StreamedChunk*, kNumClustersLoadedAhead> clusters = {nullptr, nullptr};

private:
	// SR1 Task 4: residency acquisition now goes through the region port (libdeluge/sample_source.h)
	// instead of a per-slot SampleStream::get_cluster() loop. `source_` is a per-reader cursor opened
	// lazily against the sample's `stream()` (see ensureSource); `clusters[]` is kept populated in
	// parallel (it still holds its own leases) so the untouched moveOnToNextCluster / steal_clusters /
	// DSP consumers stay byte-for-byte identical until they migrate in a later task.
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
