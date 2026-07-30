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
	    .byte_depth = static_cast<uint8_t>(sample_.byteDepth),
	    .num_channels = static_cast<uint8_t>(sample_.numChannels),
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

Error SampleStream::open_read_stream(std::string_view path) {
	// R1: efatfs IS the streaming read path — no C-FatFS fallback. Open the efatfs file handle; a
	// failure to open is a stream-open failure propagated to the caller (the sample won't load).
	// The old deluge::io::Stream (read_stream_) + its sdAddress sector seeding are gone. A
	// still-recording sample has no read handle at all (see make_read_source()) until this same
	// path reopens one once recording finishes.
	std::string cpath{path}; // NUL-terminate for the C-ABI (path is a non-terminated string_view)
	uint32_t handle = 0;
	bool table_full = false;
	if (!deluge_efatfs_open(cpath.c_str(), &handle, &table_full)) {
		return table_full ? Error::TOO_MANY_OPEN_STREAMS : Error::FILE_NOT_FOUND;
	}
	efatfs_handle_ = handle;
	// Re-register the fill-context now the handle is known (SR2d-4 Task 1): a no-op today on every
	// known call site (open_read_stream() always runs before deluge_streaming_define_asset()'s first
	// call — see that function's comment, chunk_residency.cpp — so the handle is already registered
	// there), but keeps the table correct if a future caller ever opens the stream after the asset was
	// already defined.
	register_fill_context();
	return Error::NONE;
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
