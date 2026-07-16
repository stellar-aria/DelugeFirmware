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

// The universal size of all clusters
size_t Cluster::size = 32768;
size_t Cluster::size_magnitude = 15;

void Cluster::set_size(size_t size) {
	Cluster::size = size;

	// Find the highest bit set
	Cluster::size_magnitude = 32 - __builtin_clz(size) - 1;
}

void Cluster::destroy() {
	this->~Cluster();
	// Release back to the slab so its table entry is cleared (a bare deluge_free /
	// delugeDealloc would leave a dangling slot pointing at freed memory).
	GeneralMemoryAllocator::get().freeSdram(this);
}

// Safety net (see the header): release through the slab so the table entry is cleared.
// freeSdram() falls back to a plain heap free for any non-slab pointer.
void Cluster::operator delete(void* ptr) {
	GeneralMemoryAllocator::get().freeSdram(ptr);
}

/**
 * @brief This function goes through the contents of the cluster,
 *        and converts them to the Deluge's native PCM 24-bit format if needed
 */
void Cluster::convert_data_if_necessary() {
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

// The resource-manager Asset that owns this cluster's residency for the *leased* (reason-tracked)
// kinds — SAMPLE (the sample's asset) and PERC_CACHE_* (the sample's per-direction perc asset).
// SAMPLE_CACHE clusters are unleased (never reasoned), so they return NO_ASSET here and are managed
// via their cache's own Asset instead.
uint32_t Cluster::resource_lease_asset_id() const {
	switch (type) {
	case Type::SAMPLE:
		return (sample != nullptr) ? sample->resourceAssetId : DELUGE_RESOURCE_NO_ASSET;
	case Type::PERC_CACHE_FORWARDS:
		return (sample != nullptr) ? sample->percCacheAssetId[0] : DELUGE_RESOURCE_NO_ASSET;
	case Type::PERC_CACHE_REVERSED:
		return (sample != nullptr) ? sample->percCacheAssetId[1] : DELUGE_RESOURCE_NO_ASSET;
	default:
		return DELUGE_RESOURCE_NO_ASSET; // SAMPLE_CACHE is unleased
	}
}

void Cluster::add_reason() {
	// Manager-owned leased clusters (SAMPLE / PERC) are pinned by a resource-manager lease (they're
	// never on a stealable queue). Take a lease so the manager won't evict a cluster the caller still
	// holds. The lease count lives in the manager's chunk slot (read via lease_count()).
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	if (mgr != nullptr) {
		deluge_resource_add_lease(mgr, this); // hit-only lease bump on this resident chunk
	}
}

uint32_t Cluster::lease_count() const {
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	return (mgr != nullptr) ? deluge_resource_lease_count_by_slot(mgr, resource_slot) : 0;
}
