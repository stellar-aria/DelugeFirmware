/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "definitions.h"
#include "definitions_cxx.hpp"
#include "memory/general_memory_allocator.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

class Sample;
class SampleCluster;
class SampleCache;

// Shared FAT-geometry config holder for the cluster payload types. No longer instantiated as a
// payload itself (StreamedChunk / ComputedChunk are the real chunk structs below); it just carries
// the session-wide cluster size + the Type enum both payloads reference.
//
// A chunk's backing comes from the resource manager's uniform cluster slab; the manager owns its
// residency + eviction (leased SAMPLE / PERC clusters via the owner's Asset; unleased SAMPLE_CACHE
// clusters via the cache's Asset). Constructed via the asset materialize/construct callbacks (placement
// `new` into a slab slot), not a self-allocating factory.
class Cluster final {
public:
	constexpr static size_t kSizeFAT16Max = 32768;
	// The computed-chunk kinds (ComputedChunk::type). StreamedChunk carries no type — its role is
	// fixed by its C++ type. (EMPTY / SAMPLE were retired when the overloaded Cluster split.)
	enum class Type {
		SAMPLE_CACHE,
		PERC_CACHE_FORWARDS,
		PERC_CACHE_REVERSED,
	};

	static size_t size;
	static size_t size_magnitude;
	static void set_size(size_t size);
};

// The slab-release safety net shared by both chunk structs: chunks are allocated from the backing
// slab, so their memory MUST be released through the slab (GMA::freeSdram -> slab_release) to clear
// the slab's table entry. The normal recycle path is deluge::cluster::free_chunk(); this operator
// delete is a safety net so a stray `delete someChunk` routes to freeSdram too instead of the global
// operator delete (which frees the heap block but leaks the slab slot — exhausting the slab table
// and breaking later allocations).

// The edge-slack contract shared by both chunk structs: the application's boundary-straddle reach
// past the payload edges (a multi-byte sample frame that spans a cluster boundary). This is distinct
// from — and covered by — the DMA cache-line guard documented on the structs below.
inline constexpr size_t kFrontSlackBytes = 4;    // the `-4 + byte_depth` frame-cursor front-underread
inline constexpr size_t kTrailingSlackBytes = 7; // the stitch `Cluster::size + 7` / recorder `+5` over-reads

// deluge_resource_request/acquire take a `size` argument that BACKING_SLAB assets ignore — the slab
// always hands back its fixed slot_size (computed once in general_memory_allocator.cpp), never the
// caller's value. Every StreamedChunk/ComputedChunk asset is slab-backed, so call sites requesting one
// pass this rather than a `sizeof(chunk) + Cluster::size` figure that reads as load-bearing but isn't.
inline constexpr size_t kSlabBackedSizeIgnored = 0;

/// A file-backed streamed sample-audio chunk (the streamed SAMPLE role). Its backing comes from the
/// resource-manager cluster slab; the actual cluster data lives in the same allocation, after this
/// struct — allocate Cluster::size bytes past it with enough padding to absorb an offset of at least
/// CACHE_LINE_SIZE. Warning: only construct via placement `new` into a slab slot (asset callbacks).
struct StreamedChunk final {
	uint32_t cluster_index = 0;

	// Handle to this cluster's chunk slot in the resource manager (set at creation via
	// deluge_resource_slot_of). 0xFFFFFFFF == DELUGE_RESOURCE_NO_SLOT (literal here so this widely-
	// included header needn't pull in deluge_resource.h).
	uint32_t resource_slot = 0xFFFFFFFF;
	int8_t num_reasons_held_by_sample_recorder = 0;
	bool unloadable = false;
	bool extra_bytes_at_start_converted = false;
	bool extra_bytes_at_end_converted = false;
	bool loaded = false;
	Sample* sample = nullptr;
	char first_three_bytes_pre_data_conversion[3]{};

	StreamedChunk() = default;
	void convert_data_if_necessary();

	// The resource-manager Asset that owns this chunk's residency (the sample's asset), or
	// DELUGE_RESOURCE_NO_ASSET if it has no sample. Used to route a reason to a manager lease.
	uint32_t resource_lease_asset_id() const;

	static void operator delete(void* ptr); // slab-release safety net (see above)

	/// @brief The cluster's audio payload — Cluster::size bytes DMA'd from the card, living in the
	///        slab slot after this header. `data` is a placeholder; the real region is over-allocated
	///        (see the guard note below).
	[[nodiscard]] std::span<std::byte> payload() { return {reinterpret_cast<std::byte*>(data), Cluster::size}; }
	[[nodiscard]] std::span<const std::byte> payload() const {
		return {reinterpret_cast<const std::byte*>(data), Cluster::size};
	}

	/// @brief Byte pointer positioned so a 32-bit word read yields the `byte_depth`-byte little-endian
	///        sample frame at `pos`, left-justified per the Deluge fixed-point convention.
	/// @note For the first frame of a cluster (`pos == 0`) this points up to kFrontSlackBytes BEFORE
	///       the payload, into the leading guard — those bytes are don't-care (masked out of the
	///       read). Called at cluster-boundary cadence, not per sample.
	[[nodiscard]] std::byte* frame_read_origin(uint32_t pos, uint8_t byte_depth) {
		return payload().data() + pos - 4 + byte_depth;
	}

	/// @brief The payload plus its trailing edge-slack — for the boundary stitch + recorder overshoot
	///        that read/write up to kTrailingSlackBytes past the nominal payload end (a frame
	///        straddling into the next cluster). The slack is guard bytes, guaranteed by the trailing
	///        guard.
	[[nodiscard]] std::span<std::byte> payload_with_trailing_slack() {
		return {payload().data(), Cluster::size + kTrailingSlackBytes};
	}

	// The guards below serve two distinct jobs:
	//   (a) DMA cache-line rounding absorption — `data` is not cache-line-aligned, and the SD read's
	//       cache maintenance rounds the buffer range OUT to CACHE_LINE_SIZE-byte lines, so each guard
	//       must be >= CACHE_LINE_SIZE to keep that rounding off live neighbour data (the header before
	//       `dummy`, the next slab slot after the trailing gap).
	//   (b) Application edge-slack — kFrontSlackBytes/kTrailingSlackBytes, the boundary-straddle frame
	//       reads (frame_read_origin / payload_with_trailing_slack). The DMA guard (a) dominates (b),
	//       so the app slack rides for free.
	// MUST BE THE LAST TWO MEMBERS (the FAM over-allocation past `data` requires it; enforced below by
	// the static_asserts in cluster.cpp).
	alignas(4) char dummy[CACHE_LINE_SIZE]{};
	alignas(4) char data[CACHE_LINE_SIZE]{};
};

/// A computed/cached chunk (Cluster::Type::SAMPLE_CACHE / PERC_CACHE_FORWARDS / PERC_CACHE_REVERSED)
/// — repitch SampleCache + perc-cache scratch. Backing + placement-new discipline as StreamedChunk.
struct ComputedChunk final {
	Cluster::Type type; // one of SAMPLE_CACHE / PERC_CACHE_FORWARDS / PERC_CACHE_REVERSED
	uint32_t cluster_index = 0;

	// Handle to this chunk's slot in the resource manager (see StreamedChunk::resource_slot).
	uint32_t resource_slot = 0xFFFFFFFF;
	Sample* sample = nullptr;
	SampleCache* sampleCache = nullptr; // written by sampleCacheConstruct; currently no reads

	ComputedChunk() = default;

	// The resource-manager Asset that owns this chunk's residency for the *leased* (reason-tracked)
	// perc kinds (the sample's per-direction perc asset), or DELUGE_RESOURCE_NO_ASSET otherwise.
	// SAMPLE_CACHE chunks are unleased (never reasoned) and managed via the cache's own Asset.
	uint32_t resource_lease_asset_id() const;

	static void operator delete(void* ptr); // slab-release safety net (see above)

	/// @brief The cluster's audio payload — Cluster::size bytes DMA'd from the card, living in the
	///        slab slot after this header. `data` is a placeholder; the real region is over-allocated
	///        (see the guard note below).
	[[nodiscard]] std::span<std::byte> payload() { return {reinterpret_cast<std::byte*>(data), Cluster::size}; }
	[[nodiscard]] std::span<const std::byte> payload() const {
		return {reinterpret_cast<const std::byte*>(data), Cluster::size};
	}

	/// @brief Byte pointer positioned so a 32-bit word read yields the `byte_depth`-byte little-endian
	///        sample frame at `pos`, left-justified per the Deluge fixed-point convention.
	/// @note For the first frame of a cluster (`pos == 0`) this points up to kFrontSlackBytes BEFORE
	///       the payload, into the leading guard — those bytes are don't-care (masked out of the
	///       read). Called at cluster-boundary cadence, not per sample.
	[[nodiscard]] std::byte* frame_read_origin(uint32_t pos, uint8_t byte_depth) {
		return payload().data() + pos - 4 + byte_depth;
	}

	/// @brief The payload plus its trailing edge-slack — for the boundary stitch + recorder overshoot
	///        that read/write up to kTrailingSlackBytes past the nominal payload end (a frame
	///        straddling into the next cluster). The slack is guard bytes, guaranteed by the trailing
	///        guard.
	[[nodiscard]] std::span<std::byte> payload_with_trailing_slack() {
		return {payload().data(), Cluster::size + kTrailingSlackBytes};
	}

	// The guards below serve two distinct jobs:
	//   (a) DMA cache-line rounding absorption — `data` is not cache-line-aligned, and the SD read's
	//       cache maintenance rounds the buffer range OUT to CACHE_LINE_SIZE-byte lines, so each guard
	//       must be >= CACHE_LINE_SIZE to keep that rounding off live neighbour data (the header before
	//       `dummy`, the next slab slot after the trailing gap).
	//   (b) Application edge-slack — kFrontSlackBytes/kTrailingSlackBytes, the boundary-straddle frame
	//       reads (frame_read_origin / payload_with_trailing_slack). The DMA guard (a) dominates (b),
	//       so the app slack rides for free.
	// MUST BE THE LAST TWO MEMBERS (the FAM over-allocation past `data` requires it; enforced below by
	// the static_asserts in cluster.cpp).
	alignas(4) char dummy[CACHE_LINE_SIZE]{};
	alignas(4) char data[CACHE_LINE_SIZE]{};
};

// Shared lease bookkeeping, lifted off Cluster as free functions so it can be shared once
// StreamedChunk/ComputedChunk become independent types (neither needs a common base for this).
// add_lease/release_lease take the chunk's own backing pointer (the resource manager identifies a
// lease by the chunk address); lease_count is queried by resource_slot instead, since callers that
// only need the count (freeze-checks, eviction gates) commonly don't have the chunk pointer handy
// (e.g. right after a slot lookup) — matching the underlying C ABI's two lookup shapes.
namespace deluge::cluster {
/// Take a lease on the resident chunk at `chunk` (its own backing pointer). No-op if the resource
/// manager isn't up yet (mirrors the former Cluster::add_reason null-check).
void add_lease(void* chunk);

/// Drop one lease on the resident chunk at `chunk` (its own backing pointer). No-op if the resource
/// manager isn't up yet. Most callers want remove_reason() instead, which also does the
/// ALPHA/BETA zero-lease freeze-check.
void release_lease(void* chunk);

/// Hard-lease ("reason") count for the chunk resident in slot `resource_slot` — the single source of
/// truth lives in the resource manager's chunk slot. 0 if the resource manager isn't up yet (mirrors
/// the former Cluster::lease_count null-check).
[[nodiscard]] uint32_t lease_count(uint32_t resource_slot);

/// Release a chunk (StreamedChunk or ComputedChunk) back to the backing slab so its slab-table entry
/// is cleared. Both chunk structs are trivially destructible, so this is a role-agnostic free.
void free_chunk(void* chunk);

/// Drop one lease ("reason") on `chunk`, freezing (ALPHA/BETA only) if it already had zero leases —
/// i.e. a reason removed that was never held. The single reason-drop entry point for the streamed
/// SAMPLE role; forwards to release_lease().
/// @param chunk The resident StreamedChunk to drop a lease on.
/// @param error_code FREEZE_WITH_ERROR code reported (ALPHA/BETA only) if `chunk` had zero leases.
void remove_reason(StreamedChunk& chunk, char const* error_code);

/// Drop one lease ("reason") on `chunk`, freezing (ALPHA/BETA only) if it already had zero leases —
/// i.e. a reason removed that was never held. The single reason-drop entry point for the computed/
/// cached chunk role; forwards to release_lease().
/// @param chunk The resident ComputedChunk to drop a lease on.
/// @param error_code FREEZE_WITH_ERROR code reported (ALPHA/BETA only) if `chunk` had zero leases.
void remove_reason(ComputedChunk& chunk, char const* error_code);
} // namespace deluge::cluster
