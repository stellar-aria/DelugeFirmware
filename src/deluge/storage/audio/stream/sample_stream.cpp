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
#include <optional>
#include <span>
#include <utility>

#include "deluge_resource.h"             // resource manager: a Sample is an Asset, its SAMPLE clusters the Chunks
#include "storage/audio/stream/stitch.h" // StitchPrevEdge/StitchNextEdge/stitch_boundaries

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
	Sample* sample = cluster.sample;
	int32_t clusterIndex = cluster.cluster_index;

	// Failure exits jump here (kept above the local inits so the backward gotos don't cross them).
	if (false) {
getOutEarly:
		return false;
	}

	int32_t numSectors = Cluster::size >> 9;

	// If this is the last Cluster, and we do know what the audio data length is...
	if (sample->audioDataLengthBytes && sample->audioDataLengthBytes != 0x8FFFFFFFFFFFFFFF) {
		uint32_t audioDataEndPosBytes = sample->audioDataLengthBytes + sample->audioDataStartPosBytes;
		uint32_t startByteThisCluster = clusterIndex << Cluster::size_magnitude;
		int32_t bytesToRead = audioDataEndPosBytes - startByteThisCluster;
		if (bytesToRead <= 0) {
			D_PRINTLN("fail thing"); // Shouldn't really still happen
			goto getOutEarly;
		}
		if (bytesToRead < Cluster::size) {
			numSectors = ((bytesToRead - 1) >> 9) + 1;
		}
		// Otherwise, just leave it at the normal number of sectors
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

	uint32_t bytesRequested = static_cast<uint32_t>(numSectors) * 512u;
	uint32_t bytesRead = 0;
	DelugeStatus status;
	{
		// Read seam: SampleStream::make_read_source owns source selection (Stream for a loaded
		// sample, Block for a still-being-written recording). See storage/audio/stream/
		// sample_stream.h and design §6/§7.
		auto source = make_read_source();
		auto readResult = source->read(static_cast<uint32_t>(clusterIndex),
		                               std::span<std::byte>(cluster.payload().data(), bytesRequested));
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

	// If that failed, get out
	if (status != DELUGE_OK) {
		goto getOutEarly;
	}

	cluster.convert_data_if_necessary();

#if ALPHA_OR_BETA_VERSION
	if (static_cast<int32_t>(deluge::cluster::lease_count(cluster.resource_slot)) < min_reasons_after + 1) {
		FREEZE_WITH_ERROR("i040"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	// Gather the neighbor edge spans and hand off to the pure stitch core (Phase 2b). A neighbor is
	// only passed when present AND loaded, matching the original inline gates exactly.
	std::optional<deluge::audio::stream::StitchPrevEdge> prev_edge;
	if (clusterIndex > 0) {
		StreamedChunk* prevCluster = chunk_at(cluster.cluster_index - 1);
		if (prevCluster && prevCluster->loaded) {
			prev_edge = deluge::audio::stream::StitchPrevEdge{
			    .tail = std::span<std::byte>(prevCluster->payload().data() + (Cluster::size - 4), 11),
			    .end_boundary_converted = &prevCluster->extra_bytes_at_end_converted,
			};
		}
	}
	deluge::audio::stream::StitchPrevEdge* prev_ptr = prev_edge ? &*prev_edge : nullptr;

	std::optional<deluge::audio::stream::StitchNextEdge> next_edge;
	if (clusterIndex < static_cast<int32_t>(num_clusters()) - 1) {
		StreamedChunk* nextCluster = chunk_at(cluster.cluster_index + 1);
		if (nextCluster && nextCluster->loaded) {
			next_edge = deluge::audio::stream::StitchNextEdge{
			    .head = std::span<std::byte>(nextCluster->payload().data(), 7),
			    .unconverted_head = std::span<const std::byte, 3>(
			        reinterpret_cast<const std::byte*>(nextCluster->first_three_bytes_pre_data_conversion), 3),
			    .start_boundary_converted = &nextCluster->extra_bytes_at_start_converted,
			};
		}
	}
	deluge::audio::stream::StitchNextEdge* next_ptr = next_edge ? &*next_edge : nullptr;

	std::span<std::byte> self_span = cluster.payload_with_trailing_slack();
	deluge::audio::stream::stitch_boundaries(
	    self_span, clusterIndex, sample->rawDataFormat, sample->audioDataStartPosBytes, Cluster::size,
	    cluster.extra_bytes_at_start_converted, cluster.extra_bytes_at_end_converted, prev_ptr, next_ptr);

	cluster.loaded = true;
	// Manager-owned readiness: a chunk fetched via `request` (CLUSTER_ENQUEUE prefetch) was reserved in
	// the Loading state; now its data is read, signal the manager so the async/RT `try_acquire` path
	// sees it ready. `cluster.loaded` stays the C++ sync-path field; this keeps the manager in sync.
	{
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_mark_ready(mgr, &cluster);
		}
	}
	return true;
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

void SampleStream::erase_from(size_t index) {
	table_.erase(table_.begin() + static_cast<std::ptrdiff_t>(index), table_.end());
}

} // namespace deluge::audio::stream
