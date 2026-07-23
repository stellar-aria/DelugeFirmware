/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
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

// SR1 Task 2: the region port (include/libdeluge/sample_source.h) backed by the existing C++
// `deluge::audio::stream::SampleStream` / `StreamedChunk` residency machinery. A seam, not a
// behaviour change -- this mirrors get_cluster()'s residency semantics exactly (SampleLowLevelReader's
// assignClusters()/moveOnToNextCluster() do the same null/!loaded checks and CLUSTER_ENQUEUE dispatch
// this file performs). SR2 later swaps the backing to Rust without touching callers of the C ABI.

#include "libdeluge/sample_source.h"

#include "definitions_cxx.hpp" // ClusterLoad (CLUSTER_ENQUEUE), kMaxNumVoicesUnison, kNumSources
#include "foundation/panic.h"  // FREEZE_WITH_ERROR (pool-exhaustion freeze)
#include "storage/audio/stream/sample_stream.h"
#include "storage/cluster/cluster.h"

#include <array>
#include <cstddef>
#include <cstdint>

// The reader's residency for one region-port cursor: the current pinned chunk plus one prefetched
// neighbour -- the same two leases that used to live in SampleLowLevelReader::clusters[0] and
// clusters[kNumClustersLoadedAhead - 1]. Defined here (not in the public header) since the header
// only forward-declares this as an opaque type.
struct DelugeSampleSource {
	deluge::audio::stream::SampleStream* stream;
	DelugeSampleGeometry geo;

	// The reader's residency: the current pinned chunk + one prefetched neighbour. These are the
	// leases that used to live in SampleLowLevelReader::clusters[].
	StreamedChunk* current = nullptr;  ///< == the lease handed out (lease == reinterpret_cast<uint64_t>(current))
	StreamedChunk* prefetch = nullptr; ///< held so the next acquire is a resident hit
	uint32_t prefetch_index = UINT32_MAX;
};

namespace {

// SR1 Task 4 (render-thread alloc-freeness): `deluge_sample_source_open` used to `new DelugeSampleSource`,
// which routes through deluge::memory::alloc_external (the SDRAM heap — it can walk/lock the heap and throw
// BAD_ALLOC). That `new` fires at note-start (the first assignClusters() for a sample) on the audio render
// thread, violating "no allocation on the render thread". The pre-migration get_cluster() residency path was
// pool/steal (allocation-free), so this cursor must be too. We back the opaque `DelugeSampleSource*` with a
// fixed static pool instead — claim/release is a bare in-use-flag flip, no heap ops. The C ABI is unchanged:
// `open` still returns an opaque `DelugeSampleSource*` (now a pointer into the pool), `close` frees the slot.
//
// Sizing — the pool must cover every SampleLowLevelReader that can hold an open source concurrently. There are
// exactly two value-instance sites of SampleLowLevelReader in the tree, and each owns at most one `source_`:
//   * VoiceSample                (subclass) — from AudioEngine's VoiceSamplePool, shared by synth voices AND
//                                 audio clips; drives up to kNumSources × kMaxNumVoicesUnison readers per Sound
//                                 voice but all draw from the one shared pool (default capacity 48).
//   * TimeStretcher::olderPartReader (base) — one per TimeStretcher, from AudioEngine's TimeStretcherPool
//                                 (default capacity 48).
// So the concurrent open-source ceiling at default pool sizing is 48 + 48 = 96. ObjectPool::acquire() can
// transiently grow past a pool's capacity under a burst of same-tick note-ons before AudioEngine's CPU culler
// (numSamplesLimit) reclaims voices, so we size to 256 — ~2.6× the 96-reader combined default, absorbing that
// spike with wide margin. This is not a mathematically unexceedable bound (the source ObjectPools have no hard
// cap), so we adopt the same contract the resource asset table uses for its fixed table: generous size,
// exhaustion is a fatal freeze (see sample_stream.cpp's kAssetCap / RSA1). At ~64 B/slot the pool is ~16 KB.
constexpr size_t kSampleSourcePoolSize =
    16 * kMaxNumVoicesUnison * kNumSources; // = 256; see derivation above (96-reader ceiling, ~2.6× headroom)

/// One pooled cursor plus its claimed/free marker. `source` is the first member so a `DelugeSampleSource*`
/// handed out by open() casts straight back to its enclosing slot in release_source_slot().
struct SampleSourceSlot {
	DelugeSampleSource source;
	bool in_use = false;
};

// File-scope static: lives in .bss, never touches the heap. Claimed/released only on the audio render thread
// (open() is reached via SampleLowLevelReader::ensureSource() → assignClusters() at note-start; close() via the
// reader destructor / voice unassignment — both render-thread), the SAME single-threaded discipline the
// get_cluster()/release_lease() calls in this file already run under. So a plain scan-and-flag needs no atomics
// and no mutex: unlike efatfs's HandleTable (guarded by a CriticalSectionRawMutex because it is reached from the
// async streaming task and is Send across executors), nothing off the render thread opens or closes a source.
std::array<SampleSourceSlot, kSampleSourcePoolSize> g_source_pool{};

/// @brief Claim a free pool slot, or nullptr if the pool is exhausted.
[[nodiscard]] DelugeSampleSource* claim_source_slot() {
	for (SampleSourceSlot& slot : g_source_pool) {
		if (!slot.in_use) {
			slot.in_use = true;
			return &slot.source;
		}
	}
	return nullptr;
}

/// @brief Return @p src's slot to the pool. @p src must be a live pointer previously handed out by open().
void release_source_slot(DelugeSampleSource* src) {
	static_assert(offsetof(SampleSourceSlot, source) == 0,
	              "release relies on &slot.source == &slot to recover the enclosing slot");
	reinterpret_cast<SampleSourceSlot*>(src)->in_use = false;
}

/// @brief Valid payload bytes for cluster @p index, clamping the last cluster to the geometry's
///        audio-data end.
///
/// Mirrors `deluge::audio::stream::begin_fill`'s short-last-cluster sector-count calc
/// (async_fill.cpp, ~line 68), expressed in bytes rather than sectors since the region port hands
/// out `resident_bytes` directly rather than a sector count.
[[nodiscard]] uint32_t resident_bytes_for(uint32_t index, const DelugeSampleGeometry& geo) {
	uint32_t resident_bytes = geo.cluster_size_bytes;

	// Sentinel for "still recording, length unknown" (see sample_recorder.cpp) -- leave the full
	// cluster size, same as begin_fill's `sample->audioDataLengthBytes && != sentinel` guard.
	constexpr uint64_t kUnknownLengthSentinel = 0x8FFFFFFFFFFFFFFF;
	if (geo.audio_data_length_bytes != 0 && geo.audio_data_length_bytes != kUnknownLengthSentinel) {
		uint64_t audio_data_end_bytes = geo.audio_data_length_bytes + geo.audio_data_start_bytes;
		uint64_t start_byte_this_cluster = static_cast<uint64_t>(index) * geo.cluster_size_bytes;
		if (audio_data_end_bytes <= start_byte_this_cluster) {
			// Shouldn't really happen (begin_fill logs "fail thing" here too) -- no valid bytes.
			return 0;
		}
		uint64_t bytes_to_read = audio_data_end_bytes - start_byte_this_cluster;
		if (bytes_to_read < geo.cluster_size_bytes) {
			resident_bytes = static_cast<uint32_t>(bytes_to_read);
		}
	}
	return resident_bytes;
}

/// @brief Whether cluster @p index is in range for this source's stream.
[[nodiscard]] bool index_in_range(const DelugeSampleSource& src, uint32_t index) {
	return index < src.stream->num_clusters();
}

} // namespace

extern "C" {

DelugeSampleSource* deluge_sample_source_open(void* stream_backing, DelugeSampleGeometry geometry) {
	auto* stream = reinterpret_cast<deluge::audio::stream::SampleStream*>(stream_backing);
	DelugeSampleSource* src = claim_source_slot();
	if (src == nullptr) {
		// Pool exhausted. A caller silently mishandling a nullptr open() would misbehave (leak a note, read a
		// stale cursor), so freeze distinctly — mirrors sample_stream.cpp's RSA1 resource-asset-table freeze.
		FREEZE_WITH_ERROR("SSP1"); // sample-source pool exhausted (raise kSampleSourcePoolSize)
		return nullptr;
	}
	// Assign only the `source` payload, leaving the slot's in_use marker set by claim_source_slot(). Default
	// member initializers reset current/prefetch/prefetch_index to their empty state.
	*src = DelugeSampleSource{.stream = stream, .geo = geometry};
	return src;
}

bool deluge_sample_region_acquire(DelugeSampleSource* src, uint32_t index, int8_t direction, uint32_t priority,
                                  DelugeSampleRegion* out) {
	StreamedChunk* chunk = nullptr;

	// 1. A hit on the standing prefetch promotes it to current -- no new get_cluster(), the lease
	//    just transfers ownership from src->prefetch to the local `chunk`.
	if (src->prefetch != nullptr && src->prefetch_index == index) {
		chunk = src->prefetch;
		src->prefetch = nullptr;
		src->prefetch_index = UINT32_MAX;
	}
	else {
		chunk = src->stream->get_cluster(index, CLUSTER_ENQUEUE, priority);
	}

	// 2. NotReady: mirrors moveOnToNextCluster's/assignClusters' null/!loaded -> false path. The
	//    lease taken above (fresh or promoted) isn't tracked anywhere in src's fields at this point,
	//    so it must be released here or it leaks.
	if (chunk == nullptr || !chunk->loaded) {
		if (chunk != nullptr) {
			deluge::cluster::release_lease(chunk);
		}
		return false;
	}

	// 3. Pin it as current and fill the region descriptor. If a different chunk was already pinned as
	//    `current`, its lease is fused into this advance -- mirrors moveOnToNextCluster's fused
	//    old-cluster remove_reason (sample_low_level_reader.cpp:343). This makes acquire self-
	//    releasing: the caller need not release before re-acquiring. Re-acquiring the SAME resident
	//    chunk (src->current == chunk) must NOT release -- that's its only lease.
	if (src->current != nullptr && src->current != chunk) {
		deluge::cluster::release_lease(src->current);
	}
	src->current = chunk;
	*out = DelugeSampleRegion{
	    .payload_base = chunk->payload().data(),
	    .region_index = index,
	    .resident_bytes = resident_bytes_for(index, src->geo),
	    .lease = reinterpret_cast<uint64_t>(chunk),
	};

	// 4. Prefetch the next cluster in `direction`, if in range and not already held.
	int64_t next_signed = static_cast<int64_t>(index) + direction;
	if (next_signed >= 0 && index_in_range(*src, static_cast<uint32_t>(next_signed))) {
		auto next_index = static_cast<uint32_t>(next_signed);
		if (src->prefetch_index != next_index) {
			if (src->prefetch != nullptr) {
				deluge::cluster::release_lease(src->prefetch);
				src->prefetch = nullptr;
				src->prefetch_index = UINT32_MAX;
			}
			StreamedChunk* next_chunk = src->stream->get_cluster(next_index, CLUSTER_ENQUEUE, priority);
			src->prefetch = next_chunk;
			src->prefetch_index = (next_chunk != nullptr) ? next_index : UINT32_MAX;
		}
		// else: already the standing prefetch -- nothing to do.
	}

	return true;
}

void deluge_sample_region_release(DelugeSampleSource* src, uint64_t lease) {
	if (lease == 0 || src == nullptr) {
		return;
	}
	if (reinterpret_cast<uint64_t>(src->current) == lease) {
		deluge::cluster::release_lease(src->current);
		src->current = nullptr;
	}
	// Any other lease value (a stale/prefetch lease, or one already released) is a no-op --
	// prefetch leases are released by close() or when superseded in acquire().
}

void deluge_sample_source_close(DelugeSampleSource* src) {
	if (src == nullptr) {
		return;
	}
	if (src->current != nullptr) {
		deluge::cluster::release_lease(src->current);
		src->current = nullptr;
	}
	if (src->prefetch != nullptr) {
		deluge::cluster::release_lease(src->prefetch);
		src->prefetch = nullptr;
	}
	release_source_slot(src); // return the slot to the static pool (no heap free)
}

} // extern "C"
