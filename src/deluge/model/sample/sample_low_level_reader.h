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
	//     deliberately left on clusters[] -- but it now calls mirrorRegionOnPinnedCluster() on the chunks
	//     it pins, so even a deferring probe leaves region_ describing clusters[0] rather than empty.
	//   * VoiceSample::render's cache-resync tracking + stopReadingFromCache -- while replaying a
	//     repitch/time-stretch cache the reader tracks the uncached resume cluster here. 8b Task 2
	//     moved the resync's ACQUIRE onto the port (voice_sample.cpp ~900), so `region_` now tracks the
	//     cache position too and is no longer stale during cache replay; clusters[0] is written from
	//     that same acquire and the two move together. stopReadingFromCache still reads
	//     clusters[0]->loaded directly (the port's bool acquire cannot express "held but not loaded").
	//   * external presence consumers -- reads made by code outside this class hierarchy (VoiceSample's
	//     own clusters[] reads, covered by the bullets above, are internal): TimeStretcher's
	//     olderPartReader.clusters[0]/voiceSample->clusters[0] presence checks (time_stretcher.cpp
	//     ~570, ~880, ~1015) and SamplePlaybackGuide::adjustPitchToCorrectDriftFromSync's
	//     voiceSample->clusters[0] "clusters not set up yet" guard (sample_playback_guide.cpp ~131).
	//     voice.cpp itself no longer reads clusters[] at all (SR1 Task 8 removed its last read).
	// Fully retiring clusters[] would require routing those port-uncovered paths through the port so
	// region_ becomes authoritative during cache/probe too -- deferred (see SR1 Task 8 report).
	std::array<StreamedChunk*, kNumClustersLoadedAhead> clusters = {nullptr, nullptr};

protected:
	// 8b Task 2: the cursor + its retained region are `protected`, not `private`, because the
	// cache-replay resync in VoiceSample::render (the subclass) acquires the uncached resume cluster
	// through this same cursor -- that is what keeps `region_` tracking the CACHE position instead of
	// the stale pre-cache one. Everything else about them is unchanged; no other class can reach them.
	//
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

	/// @brief Point `region_` at the chunk currently pinned in `clusters[0]`, for the paths that pin a
	///        chunk BELOW the port and so have no acquired `DelugeSampleRegion` to assign.
	///
	/// The port only ever hands a region out for a chunk that is resident AND loaded, so a path that
	/// deliberately holds a not-yet-loaded chunk leased (attemptLateSampleStart's defer) cannot obtain
	/// its mirror from an acquire without changing which clusters get pinned. This builds the very
	/// descriptor the port would have built for that chunk: `payload_base` / `region_index` read straight
	/// off it, `resident_bytes` from the port's own `deluge_sample_region_resident_bytes`, and `lease` in
	/// the port's encoding (the chunk pointer — see `deluge_sample_region_acquire_ex`), so
	/// `region_.lease == (uint64_t)clusters[0]` identifies the mirror exactly as it does after an acquire.
	///
	/// This is what keeps the reader's standing invariant — *`clusters[0] != nullptr` implies `region_`
	/// describes that same chunk* — true on those paths too. A null `clusters[0]` clears the mirror.
	/// @param sample the sample `clusters[0]` belongs to; supplies the geometry for `resident_bytes`.
	void mirrorRegionOnPinnedCluster(const Sample& sample);

private:
	/// @brief The port geometry for @p sample — the immutable per-sample fields parsed above the port.
	[[nodiscard]] static DelugeSampleGeometry geometryFor(const Sample& sample);

	bool assignClusters(SamplePlaybackGuide* guide, Sample* sample, int32_t clusterIndex, int32_t priorityRating);
	bool fillInterpolationBufferForward(SamplePlaybackGuide* guide, Sample* sample, int32_t interpolationBufferSize,
	                                    bool loopingAtLowLevel, int32_t numSpacesToFill, int32_t priorityRating);
};
