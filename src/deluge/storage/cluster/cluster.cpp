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

#include "storage/cluster/cluster.h"
#include "definitions_cxx.hpp"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cache.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/audio/stream/convert.h"
#include "util/misc.h"

#include "deluge_resource.h" // resource manager: manager-owned clusters lease instead of queueing
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <type_traits>

// Both chunk payloads must stay standard-layout + non-polymorphic: they're placement-new'd into raw
// slab slots and their `data[]` tail is over-allocated by Cluster::size extra bytes.
static_assert(std::is_standard_layout_v<StreamedChunk> && !std::is_polymorphic_v<StreamedChunk>);
static_assert(std::is_standard_layout_v<ComputedChunk> && !std::is_polymorphic_v<ComputedChunk>);

// The universal size of all clusters
size_t Cluster::size = 32768;
size_t Cluster::size_magnitude = 15;

void Cluster::set_size(size_t size) {
	Cluster::size = size;

	// Find the highest bit set
	Cluster::size_magnitude = 32 - __builtin_clz(size) - 1;
}

// Safety nets (see the header): release through the slab so the table entry is cleared.
// freeSdram() falls back to a plain heap free for any non-slab pointer.
void StreamedChunk::operator delete(void* ptr) {
	GeneralMemoryAllocator::get().freeSdram(ptr);
}

void ComputedChunk::operator delete(void* ptr) {
	GeneralMemoryAllocator::get().freeSdram(ptr);
}

/**
 * @brief This function goes through the contents of the cluster,
 *        and converts them to the Deluge's native PCM 24-bit format if needed
 */
void StreamedChunk::convert_data_if_necessary() {
	deluge::audio::stream::convert_cluster_data(
	    std::span<std::byte>(reinterpret_cast<std::byte*>(data), Cluster::size), cluster_index, sample->rawDataFormat,
	    {.audio_data_start_pos_bytes = sample->audioDataStartPosBytes,
	     .audio_data_length_bytes = sample->audioDataLengthBytes,
	     .first_cluster_index_with_no_audio_data = sample->getFirstClusterIndexWithNoAudioData()},
	    Cluster::size, Cluster::size_magnitude,
	    std::span<std::byte, 3>(reinterpret_cast<std::byte*>(first_three_bytes_pre_data_conversion), 3),
	    // Cooperative yield during long conversions. Both of convert_cluster_data's yield sites route
	    // here, so the "from convert-data" marker now also fires on the non-24-bit path (originally only
	    // the 24-bit path logged it) — a deliberate, audio-neutral widening (goldens bit-exact).
	    [] {
		    AudioEngine::logAction("from convert-data");
		    AudioEngine::runRoutine();
	    });
}

// The resource-manager Asset that owns this chunk's residency (the sample's asset), or NO_ASSET if
// it has no sample. Used to route a reason to a manager lease.
uint32_t StreamedChunk::resource_lease_asset_id() const {
	return (sample != nullptr) ? sample->stream().resource_asset_id() : DELUGE_RESOURCE_NO_ASSET;
}

// The resource-manager Asset that owns this chunk's residency for the *leased* (reason-tracked) perc
// kinds (the sample's per-direction perc asset). SAMPLE_CACHE chunks are unleased (never reasoned),
// so they return NO_ASSET here and are managed via their cache's own Asset instead.
uint32_t ComputedChunk::resource_lease_asset_id() const {
	switch (type) {
	case Cluster::Type::PERC_CACHE_FORWARDS:
		return (sample != nullptr) ? sample->percCacheAssetId[0] : DELUGE_RESOURCE_NO_ASSET;
	case Cluster::Type::PERC_CACHE_REVERSED:
		return (sample != nullptr) ? sample->percCacheAssetId[1] : DELUGE_RESOURCE_NO_ASSET;
	default:
		return DELUGE_RESOURCE_NO_ASSET; // SAMPLE_CACHE is unleased
	}
}

namespace deluge::cluster {

void free_chunk(void* chunk) {
	// Release back to the slab so its table entry is cleared (a bare deluge_free / delugeDealloc
	// would leave a dangling slot pointing at freed memory). Both chunk structs are trivially
	// destructible (POD / char-array members), so no explicit destructor call is needed.
	GeneralMemoryAllocator::get().freeSdram(chunk);
}

void add_lease(void* chunk) {
	// Manager-owned leased clusters (SAMPLE / PERC) are pinned by a resource-manager lease (they're
	// never on a stealable queue). Take a lease so the manager won't evict a chunk the caller still
	// holds. The lease count lives in the manager's chunk slot (read via deluge::cluster::lease_count()).
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	if (mgr != nullptr) {
		deluge_resource_add_lease(mgr, chunk); // hit-only lease bump on this resident chunk
	}
}

void release_lease(void* chunk) {
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	if (mgr != nullptr) {
		deluge_resource_release(mgr, chunk); // unlease (backing ptr == the chunk's own address)
	}
}

uint32_t lease_count(uint32_t resource_slot) {
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	return (mgr != nullptr) ? deluge_resource_lease_count_by_slot(mgr, resource_slot) : 0;
}

// Shared reason-drop body for either chunk role. Every cluster is a manager-owned chunk — SAMPLE /
// PERC leased via the owner's Asset, SAMPLE_CACHE resident via the cache's Asset (and leased by the
// low-level reader while it streams it). A removed reason is just a manager lease drop: the cluster
// stays resident (cached, evictable under pressure), never enqueued/destroyed here. Only the chunk's
// own address + resource_slot are needed, so this is role-agnostic (the two chunk types share no base).
static void remove_reason_impl(void* chunk, uint32_t resource_slot, [[maybe_unused]] char const* error_code) {
	if (ALPHA_OR_BETA_VERSION && lease_count(resource_slot) == 0) {
		FREEZE_WITH_ERROR(error_code); // removing a reason that was never there
	}
	release_lease(chunk); // unlease (backing ptr == the chunk's own address)
}

void remove_reason(StreamedChunk& chunk, char const* error_code) {
	remove_reason_impl(&chunk, chunk.resource_slot, error_code);
}

void remove_reason(ComputedChunk& chunk, char const* error_code) {
	remove_reason_impl(&chunk, chunk.resource_slot, error_code);
}

} // namespace deluge::cluster
