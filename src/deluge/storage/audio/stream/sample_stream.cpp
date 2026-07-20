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
#include "processing/engines/audio_engine.h"
#include "storage/cluster/cluster.h"
#include <memory>
#include <new>
#include <span>
#include <utility>

#include "deluge_resource.h"                 // resource manager: a Sample is an Asset, its SAMPLE clusters the Chunks
#include "libdeluge/streaming_fill.h"        // deluge_streaming_signal_fill
#include "storage/audio/stream/async_fill.h" // deluge_streaming_begin_fill/finish_fill (StreamingFillDescriptor)

namespace deluge::audio::stream {

// Resource-manager Source callbacks (contract documented in sample_stream.h). `owner` is the Sample*
// registered by ensure_resource_asset(); being statics, these reach the residency table through that
// sample's own SampleStream via `sample->stream().table_`.

bool SampleStream::cluster_materialize(void* /*ctx*/, void* owner, uint32_t index, void* dest, size_t /*len*/) {
	auto* sample = static_cast<Sample*>(owner);
	auto* cluster = new (dest) StreamedChunk();
	cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kChunkPayloadOffset; // slot-provenance payload
	cluster->sample = sample;
	cluster->cluster_index = index;
	cluster->resource_slot = deluge_resource_slot_of(GeneralMemoryAllocator::get().resourceManager(), dest);

	bool ok = sample->stream().read_cluster_data(*cluster, 0); // uses payload() — payload_ set above
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
	cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kChunkPayloadOffset; // slot-provenance payload
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

std::unique_ptr<ReadSource> SampleStream::make_read_source() {
	if (read_stream_.has_value()) {
		return std::make_unique<StreamReadSource>(read_stream_.value(), static_cast<uint8_t>(Cluster::size_magnitude));
	}
	return std::make_unique<BlockReadSource>(sample_);
}

#define REPORT_LOAD_TIME 0

// The cluster data reader (contract documented in sample_stream.h): the pure data work — sector count,
// read from the read source, conversion, and the inter-cluster boundary fixups. No orchestration (the
// card-state guards, the loading "reason", and the loading queue stay with the caller).
bool SampleStream::read_cluster_data(StreamedChunk& cluster, [[maybe_unused]] int32_t min_reasons_after) {
	int32_t clusterIndex = cluster.cluster_index;

	// Phase 1: resolve the fill (destination buffer, physical sector, sector count). Pure
	// lookup + arithmetic (the last-cluster short-read sector-count calc lives here) — no SD
	// access, no FatFS. See storage/audio/stream/async_fill.{h,cpp}.
	StreamingFillDescriptor fill = deluge_streaming_begin_fill(&cluster);
	if (!fill.ok) {
		return false;
	}

#if ALPHA_OR_BETA_VERSION
	if ((uintptr_t)cluster.payload().data() & 0b11) {
		D_PRINTLN("SD read address misaligned by  %d", (int32_t)((uintptr_t)cluster.payload().data() & 0b11));
	}
#endif

	AudioEngine::logAction("read_cluster_data");

#if REPORT_LOAD_TIME
	uint16_t startTime = MTU2.TCNT_0;
#endif

#if ALPHA_OR_BETA_VERSION
	if (static_cast<int32_t>(deluge::cluster::lease_count(cluster.resource_slot)) < min_reasons_after + 1) {
		FREEZE_WITH_ERROR("i039"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	uint32_t bytesRequested = fill.num_sectors * 512u;
	uint32_t bytesRead = 0;
	DelugeStatus status;
	{
		// Read seam: SampleStream::make_read_source owns source selection (Stream for a loaded
		// sample, Block for a still-being-written recording). See storage/audio/stream/
		// sample_stream.h and design §6/§7.
		auto source = make_read_source();
		auto readResult = source->read(static_cast<uint32_t>(clusterIndex),
		                               std::span<std::byte>(reinterpret_cast<std::byte*>(fill.dest), bytesRequested));
		if (readResult) {
			bytesRead = readResult.value();
			status = DELUGE_OK;
		}
		else {
			status = readResult.error();
		}
	}

#if REPORT_LOAD_TIME
	uint16_t endTime = MTU2.TCNT_0;
	uint16_t duration = endTime - startTime;
	int32_t uSec = timerCountToUS(duration);
	if (uSec > 7000) {
		D_PRINTLN(uSec);
	}
#endif

#if ALPHA_OR_BETA_VERSION
	if (cluster.sample == nullptr) {
		FREEZE_WITH_ERROR("E208");
	}

	if (static_cast<int32_t>(deluge::cluster::lease_count(cluster.resource_slot)) < min_reasons_after + 1) {
		FREEZE_WITH_ERROR("i038"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	// i040 (the post-convert/pre-stitch checkpoint) now lives inside deluge_streaming_finish_fill
	// (async_fill.cpp), immediately after convert_data_if_necessary() -- see the comment there for
	// why it must stay distinct from i038/i039 rather than collapse into them.

	// Phase 2: convert + stitch the just-read payload and publish readiness (a no-op that
	// returns false when the read above failed).
	return deluge_streaming_finish_fill(&cluster, status == DELUGE_OK);
}

// Cluster residency dispatch + table accessors (contract documented in sample_stream.h).
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
			void* p = deluge_resource_request(mgr, asset, index, kSlabBackedSizeIgnored);
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
		void* p = deluge_resource_request(mgr, asset, index, kSlabBackedSizeIgnored);
		if (p == nullptr) {
			if (error != nullptr) {
				*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::INSUFFICIENT_RAM;
			}
			return nullptr;
		}
		table_[index].cluster = reinterpret_cast<StreamedChunk*>(p);
		if (!table_[index].cluster->loaded) {
			deluge_resource_loader_enqueue(mgr, table_[index].cluster->resource_slot, priority_rating);
			// R2.1: wake the async streaming-fill task (no-op unless it's the active backing —
			// see deluge_streaming_async_active()'s doc).
			deluge_streaming_signal_fill();
		}
		return table_[index].cluster;
	}

	// CLUSTER_LOAD_IMMEDIATELY / _OR_ENQUEUE: must have it loaded now → acquire (full
	// materialize on a miss; this may block on I/O, which is the must-load-now contract).
	void* p = deluge_resource_acquire(mgr, asset, index, kSlabBackedSizeIgnored);
	if (p == nullptr) {
		if (error != nullptr) {
			*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::UNSPECIFIED;
		}
		return nullptr;
	}
	table_[index].cluster = reinterpret_cast<StreamedChunk*>(p);
	// Hit on a cluster that was prefetch-constructed but not yet read → read it now.
	if (!table_[index].cluster->loaded) {
		bool ok = read_cluster_data(*table_[index].cluster, 0);
		deluge_resource_loader_remove(mgr, table_[index].cluster->resource_slot); // it no longer needs the loader
		if (!ok) {
			if (load_instruction == CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE) {
				deluge_resource_loader_enqueue(mgr, table_[index].cluster->resource_slot,
				                               priority_rating); // fall back to async
				// R2.1: same wakeup as the CLUSTER_ENQUEUE path above — this enqueue is also a
				// streaming CLUSTER_ENQUEUE fallback (see deluge_streaming_signal_fill()'s doc).
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

size_t SampleStream::num_clusters() const {
	return table_.size();
}
void SampleStream::resize(size_t n) {
	table_.resize(n);
}

void SampleStream::reserve(size_t num_clusters) {
	table_.reserve(num_clusters);
}

void SampleStream::erase_from(size_t index) {
	table_.resize(index); // SegmentedVector: shrink-to-size destroys the removed tail (same effect as erase-to-end)
}

} // namespace deluge::audio::stream
