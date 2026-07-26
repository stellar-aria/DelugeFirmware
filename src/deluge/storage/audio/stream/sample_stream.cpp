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
#include "model/sample/sample_recorder.h"
#include "processing/engines/audio_engine.h"
#include "storage/cluster/cluster.h"
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "deluge_resource.h"                 // resource manager: a Sample is an Asset, its SAMPLE clusters the Chunks
#include "libdeluge/streaming_fill.h"        // deluge_streaming_signal_fill
#include "storage/audio/stream/async_fill.h" // deluge_streaming_begin_fill/finish_fill (StreamingFillDescriptor)

namespace deluge::audio::stream {

namespace {
// While a Sample's length is still unknown (mid-recording), its cluster count isn't geometric --
// it's however far the live SampleRecorder has actually written. Mirrors the walk in
// WaveformRenderer::investigateWholeCluster (waveform_renderer.cpp, ~line 618): AudioEngine::firstRecorder
// via ->next, matching on SampleRecorder::sample.
size_t liveRecorderClusterCount(const Sample& sample) {
	for (SampleRecorder* recorder = AudioEngine::firstRecorder; recorder != nullptr; recorder = recorder->next) {
		if (recorder->sample == &sample) {
			int32_t index = recorder->currentRecordClusterIndex.load(std::memory_order_acquire);
			return index < 0 ? 0 : static_cast<size_t>(index);
		}
	}
	return 0;
}
} // namespace

void SampleStream::register_fill_context() {
	if (resource_asset_id_ == DELUGE_RESOURCE_NO_ASSET) {
		return;
	}
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	if (mgr == nullptr) {
		return;
	}
	DelugeStreamingFillContext ctx{
	    .efatfs_handle = efatfs_handle_,
	    .audio_data_start_pos_bytes = sample_.audioDataStartPosBytes,
	    .audio_data_length_bytes = sample_.audioDataLengthBytes,
	    .first_cluster_index_with_no_audio_data = sample_.getFirstClusterIndexWithNoAudioData(),
	    .cluster_size = static_cast<uint32_t>(Cluster::size),
	    .cluster_size_magnitude = static_cast<uint32_t>(Cluster::size_magnitude),
	    .raw_data_format = static_cast<uint8_t>(sample_.rawDataFormat),
	};
	deluge_streaming_set_fill_context(mgr, resource_asset_id_, ctx);
}

void SampleStream::release_asset() {
	if (resource_asset_id_ != DELUGE_RESOURCE_NO_ASSET) {
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_release_asset(mgr, resource_asset_id_);
		}
		resource_asset_id_ = DELUGE_RESOURCE_NO_ASSET;
	}
	// Close the embedded-fatfs read handle (if the `efatfs_streaming` path opened one). The `!= 0`
	// guard makes this idempotent, so `~SampleStream`'s backstop call after `~Sample`'s explicit one
	// is a no-op. `deluge_efatfs_close` is a weak no-op on every non-efatfs BSP/config.
	if (efatfs_handle_ != 0) {
		deluge_efatfs_close(efatfs_handle_);
		efatfs_handle_ = 0;
	}
}

bool SampleStream::open_read_stream(std::string_view path) {
	// R1: efatfs IS the streaming read path — no C-FatFS fallback. Open the efatfs file handle; a
	// failure to open is a stream-open failure propagated to the caller (the sample won't load).
	// The old deluge::io::Stream (read_stream_) + its sdAddress sector seeding are gone. A
	// still-recording sample has no read handle at all (see make_read_source()) until this same
	// path reopens one once recording finishes.
	std::string cpath{path}; // NUL-terminate for the C-ABI (path is a non-terminated string_view)
	uint32_t handle = 0;
	if (!deluge_efatfs_open(cpath.c_str(), &handle)) {
		return false;
	}
	efatfs_handle_ = handle;
	// Re-register the fill-context now the handle is known (SR2d-4 Task 1): a no-op today on every
	// known call site (open_read_stream() always runs before deluge_streaming_define_asset()'s first
	// call — see that function's comment, chunk_residency.cpp — so the handle is already registered
	// there), but keeps the table correct if a future caller ever opens the stream after the asset was
	// already defined.
	register_fill_context();
	return true;
}

std::unique_ptr<ReadSource> SampleStream::make_read_source() {
	// R1: an open efatfs read handle = the streaming read path (a normal card-loaded sample). A
	// still-recording sample (efatfs_handle_ == 0 -- no read handle is opened until recording
	// finishes) has no reader at all: SR3b deleted RecordingReadSource, the former recorder-write-
	// context read-back, as a dead end on the real device (the async Rust loader reads via a raw
	// efatfs handle and never routed through this C++ abstraction anyway). A caller that could reach
	// a still-recording Sample must guard against it itself (see
	// WaveformRenderer::investigateWholeCluster()) -- a handle-0 read here just fails cleanly
	// (deluge_efatfs_read_at()/efatfs_fs::read_at() both treat handle 0 as "no such handle").
	return std::make_unique<EfatfsReadSource>(efatfs_handle_, static_cast<uint8_t>(Cluster::size_magnitude));
}

#define REPORT_LOAD_TIME 0

// The cluster data reader (contract documented in sample_stream.h): the pure data work — sector count,
// read from the read source, conversion, and the inter-cluster boundary fixups. No orchestration (the
// card-state guards, the loading "reason", and the loading queue stay with the caller).
bool SampleStream::read_cluster_data(StreamedChunk& cluster, [[maybe_unused]] int32_t min_reasons_after) {
	int32_t clusterIndex = cluster.cluster_index;

	// Resolve the fill (destination buffer, physical sector, sector count): pure lookup +
	// arithmetic (the last-cluster short-read sector-count calc lives here), no SD access, no
	// FatFS. See storage/audio/stream/async_fill.{h,cpp}.
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
		// Read seam: SampleStream::make_read_source owns source selection -- unconditionally
		// EfatfsReadSource now (SR3b deleted the RecordingReadSource branch; see that method's doc).
		// See storage/audio/stream/sample_stream.h and design §6/§7.
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

	// i040 (the post-convert/pre-stitch lease-count check) lives inside deluge_streaming_finish_fill
	// (async_fill.cpp), immediately after convert_data_if_necessary() -- see the comment there for
	// why it must stay distinct from i038/i039 rather than collapse into them.

	// Convert + stitch the just-read payload and publish readiness; deluge_streaming_finish_fill
	// is a no-op that returns false when the read above failed.
	return deluge_streaming_finish_fill(&cluster, status == DELUGE_OK);
}

// Cluster residency dispatch + table accessors (contract documented in sample_stream.h).
StreamedChunk* SampleStream::get_cluster(uint32_t index, int32_t load_instruction, uint32_t priority_rating,
                                         Error* error) {

	if (error != nullptr) {
		*error = Error::NONE;
	}

	// Manager-owned residency. The manager is the sole SDRAM evictor: every Sample (playback or
	// recording) is manager-owned (deluge_streaming_define_asset() FREEZEs if the asset table is
	// exhausted — no legacy fallback). The hard-lease count lives in the manager's chunk slot (the
	// construct/materialize callback records the slot handle); add_lease/request take the lease.
	// Residency lives in the manager; the returned chunk pointer IS the manager backing (a
	// StreamedChunk placement-new'd into the slab slot). SR3d: no app-side table_ mirror.
	uint32_t asset = deluge_streaming_define_asset(&sample_);
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();

	if (load_instruction == CLUSTER_DONT_LOAD) {
		// "Allocate but don't read from the card" — recording / convert write target. Resident ⇒ just
		// pin (lease); not-resident ⇒ construct an empty cluster (no I/O). Held *dirty* so the manager
		// never evicts the unflushed data; writeCluster clears dirty once it is on the card, after
		// which it is reconstructable like any sample cluster.
		void* existing = deluge_resource_peek(mgr, asset, index); // resident (ready-or-not) backing, or null
		StreamedChunk* cluster;
		if (existing != nullptr) {
			// peek -> add_lease is a two-step on an unleased pointer. Safe under the current
			// cooperative-eviction model: the only preemptor is the audio ISR (which uses try_acquire and
			// never evicts), and a live write target is dirty/leased so evict_lowest skips it -- nothing can
			// evict `existing` in this window. Revisit (masked peek+lease, or re-validate) when the Embassy
			// preemptive evictor lands.
			deluge_resource_add_lease(mgr, existing);
			cluster = reinterpret_cast<StreamedChunk*>(existing);
		}
		else {
			void* p = deluge_resource_request(mgr, asset, index, kSlabBackedSizeIgnored);
			if (p == nullptr) {
				if (error != nullptr) {
					*error = sample_.unloadable ? Error::FILE_NOT_FOUND : Error::INSUFFICIENT_RAM;
				}
				return nullptr;
			}
			cluster = reinterpret_cast<StreamedChunk*>(p);
		}
		deluge_resource_mark_dirty(mgr, cluster, true);
		return cluster;
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
		auto* cluster = reinterpret_cast<StreamedChunk*>(p);
		if (!cluster->loaded) {
			deluge_resource_loader_enqueue(mgr, cluster->resource_slot, priority_rating);
			// Wake the async streaming-fill task; a no-op unless it's the active backing — see
			// deluge_streaming_async_active()'s doc.
			deluge_streaming_signal_fill();
		}
		return cluster;
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
	auto* cluster = reinterpret_cast<StreamedChunk*>(p);
	// Hit on a cluster that was prefetch-constructed but not yet read → read it now.
	if (!cluster->loaded) {
		bool ok = read_cluster_data(*cluster, 0);
		deluge_resource_loader_remove(mgr, cluster->resource_slot); // it no longer needs the loader
		if (!ok) {
			if (load_instruction == CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE) {
				deluge_resource_loader_enqueue(mgr, cluster->resource_slot,
				                               priority_rating); // fall back to async
				// Same wakeup as the CLUSTER_ENQUEUE path above — this fallback is also an async
				// enqueue, so it needs the same signal (see deluge_streaming_signal_fill()'s doc).
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

StreamedChunk* SampleStream::chunk_at(uint32_t index) const {
	if (resource_asset_id_ == DELUGE_RESOURCE_NO_ASSET) {
		return nullptr; // matches a never-resident table entry
	}
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	return reinterpret_cast<StreamedChunk*>(deluge_resource_peek(mgr, resource_asset_id_, index));
}

SampleCluster& SampleStream::entry(uint32_t index) {
	return table_[index];
}
const SampleCluster& SampleStream::entry(uint32_t index) const {
	return table_[index];
}

size_t SampleStream::num_clusters() const {
	return sample_.isLengthKnown() ? sample_.geometricClusterCount() : liveRecorderClusterCount(sample_);
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

extern "C" {

// The region-port open() bridge's stream-backing -> resource-asset accessor (SR2d-5 Task 1;
// declared in libdeluge/streaming_fill.h alongside its sibling deluge_streaming_resource_manager).
// The real body: SampleStream is always available wherever this TU compiles, so this just forwards
// to the relocated asset-definition entry point (chunk_residency.cpp). The `__attribute__((weak))`
// no-op fallback for build configs without a real SampleStream lives in async_fill.cpp, mirroring
// that file's other weak fallbacks.
uint32_t deluge_sample_stream_asset_id(void* stream_backing) {
	auto* stream = reinterpret_cast<deluge::audio::stream::SampleStream*>(stream_backing);
	return deluge_streaming_define_asset(&stream->sample());
}

} // extern "C"
