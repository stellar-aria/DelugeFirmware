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

#include "storage/audio/stream/sample_stream.h"
#include "definitions_cxx.hpp"
#include "io/debug/log.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cluster.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include <memory>
#include <new>
#include <utility>

#include "deluge_resource.h" // resource manager: a Sample is an Asset, its SAMPLE clusters the Chunks

namespace deluge::audio::stream {

// === Resource-manager Source for SAMPLE clusters =============================
// See the doc comments in sample_stream.h. Bodies moved verbatim from sample.cpp's former
// file-static clusterMaterialize/clusterConstruct/clusterEvict; `owner` is the Sample*, and the
// residency table now lives on that Sample's own SampleStream (`sample->stream().table_`) --
// reached here via `sample->stream()` since these are SampleStream statics, not members, so they
// don't have an implicit `sample_`/`table_` of their own to fall back on.

bool SampleStream::cluster_materialize(void* /*ctx*/, void* owner, uint32_t index, void* dest, size_t /*len*/) {
	auto* sample = static_cast<Sample*>(owner);
	auto* cluster = new (dest) StreamedChunk();
	cluster->sample = sample;
	cluster->cluster_index = index;
	cluster->resource_slot = deluge_resource_slot_of(GeneralMemoryAllocator::get().resourceManager(), dest);

	bool ok = audioFileManager.readClusterData(*cluster, 0);
	if (ok) {
		sample->stream().table_[index].cluster = cluster;
	}
	else {
		cluster->~StreamedChunk(); // manager frees the slab slot
	}
	return ok;
}

void SampleStream::cluster_construct(void* /*ctx*/, void* owner, uint32_t index, void* dest) {
	auto* sample = static_cast<Sample*>(owner);
	auto* cluster = new (dest) StreamedChunk();
	cluster->sample = sample;
	cluster->cluster_index = index;
	cluster->resource_slot = deluge_resource_slot_of(GeneralMemoryAllocator::get().resourceManager(), dest);
	// cluster->loaded stays false — the loader reads it.
	sample->stream().table_[index].cluster = cluster;
}

void SampleStream::cluster_evict(void* /*ctx*/, void* owner, uint32_t index) {
	auto* sample = static_cast<Sample*>(owner);
	SampleStream& stream = sample->stream();
	StreamedChunk* cluster = stream.table_[index].cluster;
	stream.table_[index].cluster = nullptr;
	if (cluster != nullptr) {
		// A constructed-but-not-yet-loaded chunk may still be in the loader queue — de-queue it so the
		// queue can't dangle onto freed memory. (Eviction also resets the slot, but be explicit.)
		deluge_resource_loader_remove(GeneralMemoryAllocator::get().resourceManager(), cluster->resource_slot);
		cluster->~StreamedChunk(); // manager frees the slab slot
	}
}

uint32_t SampleStream::ensure_resource_asset() {
	if (resource_asset_id_ != DELUGE_RESOURCE_NO_ASSET) {
		return resource_asset_id_;
	}
	// The manager is the sole SDRAM evictor now, so every Sample (playback or recording) is
	// manager-owned. A missing manager / full asset table is fatal — no legacy fallback.
	// Cost reflects rebuild expense: a converted sample (float / wrong-endian) costs a read PLUS a
	// format re-conversion, so it's kept resident longer than a native one (one plain read).
	uint32_t clusterCost =
	    (sample_.rawDataFormat != RawDataFormat::NATIVE) ? DELUGE_RESOURCE_COST_IO_CONVERTED : DELUGE_RESOURCE_COST_IO;
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	resource_asset_id_ = (mgr != nullptr)
	                         ? deluge_resource_define_asset(mgr, &sample_, cluster_materialize, cluster_evict, nullptr,
	                                                        clusterCost, DELUGE_RESOURCE_BACKING_SLAB)
	                         : DELUGE_RESOURCE_NO_ASSET;
	if (resource_asset_id_ == DELUGE_RESOURCE_NO_ASSET) {
		FREEZE_WITH_ERROR("RSA1"); // resource asset table exhausted (raise kAssetCap)
	}
	// Attach the async-prefetch path so CLUSTER_ENQUEUE can request (construct now, load later).
	deluge_resource_set_construct(mgr, resource_asset_id_, cluster_construct);
	// If the sample is already project-relevant (a holder gained it before its first stream), apply the
	// soft-reference now — numReasonsIncreasedFromZero fired before the asset existed, so it was a no-op.
	if (sample_.isProjectReferenced()) {
		deluge_resource_reference(mgr, resource_asset_id_);
	}
	return resource_asset_id_;
}

void SampleStream::release_asset() {
	if (resource_asset_id_ != DELUGE_RESOURCE_NO_ASSET) {
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_release_asset(mgr, resource_asset_id_);
		}
		resource_asset_id_ = DELUGE_RESOURCE_NO_ASSET;
	}
}

bool SampleStream::open_read_stream(std::string_view path, DelugeStreamMode mode, uint32_t num_clusters) {
	auto openedStream = deluge::io::Stream::open(path, mode);
	if (!openedStream) {
		return false;
	}
	read_stream_ = std::move(openedStream.value());
	for (uint32_t i = 0; i < num_clusters; i++) {
		uint32_t sector = 0;
		auto sectorResult = read_stream_->sector_of(i); // best-effort; only meaningful on FatFS-family backends
		if (sectorResult) {
			sector = *sectorResult;
		}
		table_[i].sdAddress = sector;
	}
	return true;
}

std::unique_ptr<ReadSource> SampleStream::make_read_source() const {
	if (read_stream_.has_value()) {
		return std::make_unique<StreamReadSource>(read_stream_.value(), static_cast<uint8_t>(Cluster::size_magnitude));
	}
	return std::make_unique<BlockReadSource>(sample_);
}

// === Cluster residency dispatch + table accessors (Phase 4, Task 2; table internalized Task 5) ===
// Moved verbatim from the former SampleCluster::getCluster (sample_cluster.cpp:63-146), rebased onto
// `table_[index]` -- see sample_stream.h's doc comment.
//
// Calling this will add a reason to the loaded Cluster! priority_rating is only relevant if enqueuing.
StreamedChunk* SampleStream::get_cluster(uint32_t index, int32_t load_instruction, uint32_t priority_rating,
                                         Error* error) {

	if (error != nullptr) {
		*error = Error::NONE;
	}

	// Manager-owned residency. The manager is the sole SDRAM evictor: every Sample (playback or
	// recording) is manager-owned (ensure_resource_asset() FREEZEs if the asset table is exhausted —
	// no legacy fallback). The hard-lease count lives in the manager's chunk slot (the
	// construct/materialize callback records the slot handle); add_lease/request take the lease.
	// non-null `cluster` <=> manager-resident (on_evict nulls it).
	uint32_t asset = ensure_resource_asset();
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	bool wasResident = (table_[index].cluster != nullptr);

	if (load_instruction == CLUSTER_DONT_LOAD) {
		// "Allocate but don't read from the card" — recording / convert write target. Resident ⇒ just
		// pin (lease); not-resident ⇒ construct an empty cluster (no I/O). Held *dirty* so the manager
		// never evicts the unflushed data; writeCluster clears dirty once it is on the card, after
		// which it is reconstructable like any sample cluster.
		if (wasResident) {
			deluge_resource_add_lease(mgr, table_[index].cluster);
		}
		else {
			void* p = deluge_resource_request(mgr, asset, index, sizeof(StreamedChunk) + Cluster::size);
			if (p == nullptr) {
				if (error != nullptr) {
					*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::INSUFFICIENT_RAM;
				}
				return nullptr;
			}
			table_[index].cluster = reinterpret_cast<StreamedChunk*>(p);
		}
		deluge_resource_mark_dirty(mgr, table_[index].cluster, true);
		return table_[index].cluster;
	}

	if (load_instruction == CLUSTER_ENQUEUE) {
		// Async prefetch: construct + lease now (NO I/O), then schedule the read on the loader
		// (the existing loadingQueue, pumped off the audio thread) so the audio thread never
		// blocks on SD. Returns the cluster (loaded==false until the loader reads it).
		void* p = deluge_resource_request(mgr, asset, index, sizeof(StreamedChunk) + Cluster::size);
		if (p == nullptr) {
			if (error != nullptr) {
				*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::INSUFFICIENT_RAM;
			}
			return nullptr;
		}
		table_[index].cluster = reinterpret_cast<StreamedChunk*>(p);
		if (!table_[index].cluster->loaded) {
			deluge_resource_loader_enqueue(mgr, table_[index].cluster->resource_slot, priority_rating);
		}
		return table_[index].cluster;
	}

	// CLUSTER_LOAD_IMMEDIATELY / _OR_ENQUEUE: must have it loaded now → acquire (full
	// materialize on a miss; this may block on I/O, which is the must-load-now contract).
	void* p = deluge_resource_acquire(mgr, asset, index, sizeof(StreamedChunk) + Cluster::size);
	if (p == nullptr) {
		if (error != nullptr) {
			*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::UNSPECIFIED;
		}
		return nullptr;
	}
	table_[index].cluster = reinterpret_cast<StreamedChunk*>(p);
	// Hit on a cluster that was prefetch-constructed but not yet read → read it now.
	if (!table_[index].cluster->loaded) {
		bool ok = audioFileManager.readClusterData(*table_[index].cluster, 0);
		deluge_resource_loader_remove(mgr, table_[index].cluster->resource_slot); // it no longer needs the loader
		if (!ok) {
			if (load_instruction == CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE) {
				deluge_resource_loader_enqueue(mgr, table_[index].cluster->resource_slot,
				                               priority_rating); // fall back to async
			}
			else {
				if (error != nullptr) {
					*error = Error::UNSPECIFIED;
				}
				return nullptr; // must-load-now failed; cluster stays resident+leased, caller may retry
			}
		}
	}
	return table_[index].cluster;
}

StreamedChunk* SampleStream::chunk_at(uint32_t index) const {
	return table_[index].cluster;
}

SampleCluster& SampleStream::entry(uint32_t index) {
	return table_[index];
}
const SampleCluster& SampleStream::entry(uint32_t index) const {
	return table_[index];
}

uint32_t SampleStream::sd_address_at(uint32_t index) const {
	return table_[index].sdAddress;
}
void SampleStream::set_sd_address_at(uint32_t index, uint32_t sector) {
	table_[index].sdAddress = sector;
}

size_t SampleStream::num_clusters() const {
	return table_.size();
}
void SampleStream::resize(size_t n) {
	table_.resize(n);
}

void SampleStream::erase_from(size_t index) {
	table_.erase(table_.begin() + static_cast<std::ptrdiff_t>(index), table_.end());
}

} // namespace deluge::audio::stream
