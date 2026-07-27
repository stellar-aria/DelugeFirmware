/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "storage/audio/stream/sample_residency.h"

#include "definitions_cxx.hpp"
#include "deluge_resource.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "storage/audio/stream/chunk_residency.h"
#include "storage/cluster/cluster.h"

namespace {

// The residency-dispatch body relocated from the former SampleStream::get_cluster (retired in C1 of the
// SampleStream collapse). Non-inline; the facade's prefetch/load_now/request route through it so the
// dispatch lives in one place. On a must-load-now miss it still reaches SampleStream::read_cluster_data
// (the sync-fill orchestrator retired in C2).
[[nodiscard]] StreamedChunk* acquire_cluster(Sample& sample, uint32_t index, int32_t load_instruction,
                                             uint32_t priority_rating, Error* error) {
	if (error != nullptr) {
		*error = Error::NONE;
	}

	uint32_t asset = deluge_streaming_define_asset(&sample);
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();

	if (load_instruction == CLUSTER_ENQUEUE) {
		// Async prefetch: construct + lease now (NO I/O), then schedule the read on the loader so the
		// audio thread never blocks on SD. Returns the cluster (loaded==false until the loader reads it).
		void* p = deluge_resource_request(mgr, asset, index, kSlabBackedSizeIgnored);
		if (p == nullptr) {
			if (error != nullptr) {
				*error = sample.unloadable ? Error::FILE_NOT_FOUND : Error::INSUFFICIENT_RAM;
			}
			return nullptr;
		}
		auto* cluster = reinterpret_cast<StreamedChunk*>(p);
		if (!cluster->loaded) {
			deluge_resource_loader_enqueue(mgr, cluster->resource_slot, priority_rating);
			deluge_streaming_signal_fill();
		}
		return cluster;
	}

	// CLUSTER_LOAD_IMMEDIATELY / _OR_ENQUEUE: must have it loaded now -> acquire (full materialize on a
	// miss; this may block on I/O, which is the must-load-now contract).
	void* p = deluge_resource_acquire(mgr, asset, index, kSlabBackedSizeIgnored);
	if (p == nullptr) {
		if (error != nullptr) {
			*error = sample.unloadable ? Error::FILE_NOT_FOUND : Error::UNSPECIFIED;
		}
		return nullptr;
	}
	auto* cluster = reinterpret_cast<StreamedChunk*>(p);
	// Hit on a cluster that was prefetch-constructed but not yet read -> read it now.
	if (!cluster->loaded) {
		bool ok = sample.stream().read_cluster_data(*cluster, 0);
		deluge_resource_loader_remove(mgr, cluster->resource_slot); // it no longer needs the loader
		if (!ok) {
			if (load_instruction == CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE) {
				deluge_resource_loader_enqueue(mgr, cluster->resource_slot, priority_rating); // async fallback
				deluge_streaming_signal_fill();
			}
			else {
				if (error != nullptr) {
					*error = Error::UNSPECIFIED;
				}
				return nullptr; // must-load-now failed; cluster stays resident+leased, caller may retry
			}
		}
	}
	return cluster;
}

} // namespace

namespace deluge::audio::stream {

StreamedChunk* peek(const Sample& sample, uint32_t clusterIndex) {
	const uint32_t assetId = sample.stream().resource_asset_id();
	if (assetId == DELUGE_RESOURCE_NO_ASSET) {
		return nullptr; // no Asset defined yet, so nothing can be resident
	}
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	return reinterpret_cast<StreamedChunk*>(deluge_resource_peek(mgr, assetId, clusterIndex));
}

StreamedChunk* prefetch(Sample& sample, uint32_t clusterIndex) {
	return acquire_cluster(sample, clusterIndex, CLUSTER_ENQUEUE, 0xFFFFFFFF, nullptr);
}

StreamedChunk* request(Sample& sample, uint32_t clusterIndex, int32_t loadInstruction) {
	return acquire_cluster(sample, clusterIndex, loadInstruction, 0xFFFFFFFF, nullptr);
}

StreamedChunk* load_now(Sample& sample, uint32_t clusterIndex, Error* error) {
	// priority_rating is unused on the LOAD_IMMEDIATELY path; pass get_cluster's own default (0xFFFFFFFF).
	return acquire_cluster(sample, clusterIndex, CLUSTER_LOAD_IMMEDIATELY, 0xFFFFFFFF, error);
}

void dequeue(StreamedChunk& chunk) {
	deluge_resource_loader_remove(GeneralMemoryAllocator::get().resourceManager(), chunk.resource_slot);
}

} // namespace deluge::audio::stream
