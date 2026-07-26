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

#include "storage/audio/stream/chunk_residency.h"

#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "storage/cluster/cluster.h"
#include <new>

#include "deluge_resource.h" // resource manager: a Sample is an Asset, its SAMPLE clusters the Chunks

// Resource-manager Source callbacks (contract documented in sample_stream.h and chunk_residency.h).
// `owner` is the Sample* registered by deluge_streaming_define_asset() below; these reach the
// residency table through that sample's own SampleStream via `sample->stream().table_` -- a
// deliberate later-slice (SR3d) edge, not something to clean up here.

uint32_t deluge_streaming_define_asset(Sample* sample) {
	deluge::audio::stream::SampleStream& stream = sample->stream();
	if (stream.resource_asset_id() != DELUGE_RESOURCE_NO_ASSET) {
		return stream.resource_asset_id();
	}
	// The manager is the sole SDRAM evictor now, so every Sample (playback or recording) is
	// manager-owned. A missing manager / full asset table is fatal — no legacy fallback.
	// Cost reflects rebuild expense: a converted sample (float / wrong-endian) costs a read PLUS a
	// format re-conversion, so it's kept resident longer than a native one (one plain read).
	uint32_t clusterCost =
	    (sample->rawDataFormat != RawDataFormat::NATIVE) ? DELUGE_RESOURCE_COST_IO_CONVERTED : DELUGE_RESOURCE_COST_IO;
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	uint32_t asset_id = (mgr != nullptr) ? deluge_resource_define_asset(mgr, sample, deluge_streaming_chunk_materialize,
	                                                                    deluge_streaming_chunk_evict, nullptr,
	                                                                    clusterCost, DELUGE_RESOURCE_BACKING_SLAB)
	                                     : DELUGE_RESOURCE_NO_ASSET;
	stream.set_resource_asset_id(asset_id);
	if (stream.resource_asset_id() == DELUGE_RESOURCE_NO_ASSET) {
		FREEZE_WITH_ERROR("RSA1"); // resource asset table exhausted (raise kAssetCap)
	}
	// Attach the async-prefetch path so CLUSTER_ENQUEUE can request (construct now, load later).
	deluge_resource_set_construct(mgr, stream.resource_asset_id(), deluge_streaming_chunk_construct);
	// If the sample is already project-relevant (a holder gained it before its first stream), apply the
	// soft-reference now — numReasonsIncreasedFromZero fired before the asset existed, so it was a no-op.
	if (sample->isProjectReferenced()) {
		deluge_resource_reference(mgr, stream.resource_asset_id());
	}
	// Register this asset's fill-context (SR2d-4 Task 1). Ordering: open_read_stream() is always
	// called before this point on the only path that ever assigns an efatfs handle
	// (AudioFileManager::buildAudioFileFromCard opens the stream, then loadFile()'s cluster reads
	// trigger this function on first use) — so efatfs_handle_ is already whatever it will be for this
	// stream's life (a real handle for a card-loaded sample, still 0 for a sample under construction
	// by the recorder, which never opens one). register_fill_context() is called again from
	// open_read_stream() as a defensive re-registration, in case that ordering ever changes.
	stream.register_fill_context();
	return stream.resource_asset_id();
}

bool deluge_streaming_chunk_materialize(void* /*ctx*/, void* owner, uint32_t index, void* dest, size_t /*len*/) {
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

void deluge_streaming_chunk_construct(void* /*ctx*/, void* owner, uint32_t index, void* dest) {
	auto* sample = static_cast<Sample*>(owner);
	auto* cluster = new (dest) StreamedChunk();
	cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kChunkPayloadOffset; // slot-provenance payload
	cluster->sample = sample;
	cluster->cluster_index = index;
	cluster->resource_slot = deluge_resource_slot_of(GeneralMemoryAllocator::get().resourceManager(), dest);
	// cluster->loaded stays false — the loader reads it.
	sample->stream().table_[index].cluster = cluster;
}

void deluge_streaming_chunk_evict(void* /*ctx*/, void* owner, uint32_t index) {
	auto* sample = static_cast<Sample*>(owner);
	deluge::audio::stream::SampleStream& stream = sample->stream();
	StreamedChunk* cluster = stream.table_[index].cluster;
	stream.table_[index].cluster = nullptr;
	if (cluster != nullptr) {
		// A constructed-but-not-yet-loaded chunk may still be in the loader queue — de-queue it so the
		// queue can't dangle onto freed memory. (Eviction also resets the slot, but be explicit.)
		deluge_resource_loader_remove(GeneralMemoryAllocator::get().resourceManager(), cluster->resource_slot);
		cluster->~StreamedChunk(); // manager frees the slab slot
	}
}
