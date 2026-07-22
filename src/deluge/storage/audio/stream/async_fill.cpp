/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
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

#include "storage/audio/stream/async_fill.h"

#include "io/debug/log.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "storage/audio/stream/sample_stream.h"
#include "storage/audio/stream/stitch.h"
#include "storage/cluster/cluster.h"
#include <cstddef>
#include <optional>
#include <span>

#include "deluge_resource.h" // deluge_resource_mark_ready

// FFI layout guard (M4): `StreamingFillDescriptor` crosses the C++/Rust boundary by value (see
// streaming_fill.h) with a hand-written `#[repr(C)]` mirror in streaming_loader.rs. These
// static_asserts catch field drift at compile time on whichever side notices first. Expressed
// pointer-width-relative (not hardcoded byte offsets) so the same assertions hold unchanged on
// both the 32-bit ARM device and the 64-bit host_app build: `dest` leads at offset 0, `sector`
// and `num_sectors` follow packed at 4-byte strides, then `ok` (1 byte, +8 past the pointer),
// then 3 pad bytes, then `handle` at +12 and `byte_offset` at +16 (both u32, 4-byte packed), and
// the struct's overall size pads up to the pointer's own alignment (its strictest member) —
// 2*sizeof(ptr)+16 covers that on both widths (24 on 32-bit: dest[4]+sector[4]+num_sectors[4]+
// ok[1]->pad[3]+handle[4]+byte_offset[4]=24; 32 on 64-bit: dest[8]+sector[4]+num_sectors[4]+
// ok[1]->pad[3]+handle[4]+byte_offset[4]->pad[4]=32). Verified by compiling both builds, not
// derived from a naive "trailing pad" guess (which undercounts the 64-bit case, whose 8-byte
// pointer alignment pads the tail further).
static_assert(offsetof(StreamingFillDescriptor, dest) == 0);
static_assert(offsetof(StreamingFillDescriptor, sector) == sizeof(uint8_t*));
static_assert(offsetof(StreamingFillDescriptor, num_sectors) == sizeof(uint8_t*) + 4);
static_assert(offsetof(StreamingFillDescriptor, ok) == sizeof(uint8_t*) + 8);
static_assert(offsetof(StreamingFillDescriptor, handle) == sizeof(uint8_t*) + 12);
static_assert(offsetof(StreamingFillDescriptor, byte_offset) == sizeof(uint8_t*) + 16);
static_assert(sizeof(StreamingFillDescriptor) == 2 * sizeof(uint8_t*) + 16);

// begin_fill() mirrors read_cluster_data's "resolve where/how much" step (including the
// sd_address_at lookup); finish_fill() mirrors its post-read "convert + stitch + publish" step.
// Both are reached only through the extern "C" wrappers below.

namespace deluge::audio::stream {

StreamingFillDescriptor begin_fill(StreamedChunk& cluster) {
	Sample* sample = cluster.sample;
	int32_t clusterIndex = cluster.cluster_index;

	int32_t numSectors = Cluster::size >> 9;

	// If this is the last Cluster, and we do know what the audio data length is...
	if (sample->audioDataLengthBytes && sample->audioDataLengthBytes != 0x8FFFFFFFFFFFFFFF) {
		uint32_t audioDataEndPosBytes = sample->audioDataLengthBytes + sample->audioDataStartPosBytes;
		uint32_t startByteThisCluster = clusterIndex << Cluster::size_magnitude;
		int32_t bytesToRead = audioDataEndPosBytes - startByteThisCluster;
		if (bytesToRead <= 0) {
			D_PRINTLN("fail thing"); // Shouldn't really still happen
			return StreamingFillDescriptor{
			    .dest = nullptr, .sector = 0, .num_sectors = 0, .ok = false, .handle = 0, .byte_offset = 0};
		}
		if (bytesToRead < Cluster::size) {
			numSectors = ((bytesToRead - 1) >> 9) + 1;
		}
		// Otherwise, just leave it at the normal number of sectors
	}

	return StreamingFillDescriptor{
	    .dest = reinterpret_cast<uint8_t*>(cluster.payload().data()),
	    .sector = sample->stream().sd_address_at(static_cast<uint32_t>(clusterIndex)),
	    .num_sectors = static_cast<uint32_t>(numSectors),
	    .ok = true,
	    .handle = sample->stream().efatfs_handle(),
	    .byte_offset = static_cast<uint32_t>(clusterIndex) << Cluster::size_magnitude,
	};
}

bool finish_fill(StreamedChunk& cluster, bool read_ok) {
	if (!read_ok) {
		return false;
	}

	Sample* sample = cluster.sample;
	int32_t clusterIndex = cluster.cluster_index;
	deluge::audio::stream::SampleStream& stream = sample->stream();

	cluster.convert_data_if_necessary();

#if ALPHA_OR_BETA_VERSION
	// i040: checkpoint after convert_data_if_necessary() and before the stitch step below.
	// convert_data_if_necessary() cooperatively yields back to AudioEngine::runRoutine() roughly
	// every 1024 bytes while converting (see convert.h:220-226) -- the same re-entrancy window
	// loader.cpp's pump() guards against (loader.cpp:78-82). i038 (in read_cluster_data, just
	// after the read) does not cover this window, so this check is a distinct, non-redundant
	// safety net rather than a duplicate of i038. All current callers of read_cluster_data pass
	// min_reasons_after == 0, so this reduces to "still leased".
	if (deluge::cluster::lease_count(cluster.resource_slot) < 1) {
		FREEZE_WITH_ERROR("i040");
	}
#endif

	// Gather the neighbor edge spans and hand off to the pure stitch core. A neighbor is only
	// passed when it is both present and loaded.
	std::optional<deluge::audio::stream::StitchPrevEdge> prev_edge;
	if (clusterIndex > 0) {
		StreamedChunk* prevCluster = stream.chunk_at(cluster.cluster_index - 1);
		if (prevCluster && prevCluster->loaded) {
			prev_edge = deluge::audio::stream::StitchPrevEdge{
			    .tail = std::span<std::byte>(prevCluster->payload().data() + (Cluster::size - 4), 11),
			    .end_boundary_converted = &prevCluster->extra_bytes_at_end_converted,
			};
		}
	}
	deluge::audio::stream::StitchPrevEdge* prev_ptr = prev_edge ? &*prev_edge : nullptr;

	std::optional<deluge::audio::stream::StitchNextEdge> next_edge;
	if (clusterIndex < static_cast<int32_t>(stream.num_clusters()) - 1) {
		StreamedChunk* nextCluster = stream.chunk_at(cluster.cluster_index + 1);
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

} // namespace deluge::audio::stream

extern "C" {

StreamingFillDescriptor deluge_streaming_begin_fill(void* chunk_backing) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	return deluge::audio::stream::begin_fill(*cluster);
}

bool deluge_streaming_finish_fill(void* chunk_backing, bool read_ok) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	return deluge::audio::stream::finish_fill(*cluster, read_ok);
}

DelugeResource* deluge_streaming_resource_manager(void) {
	return GeneralMemoryAllocator::get().resourceManager();
}

bool deluge_streaming_chunk_unloadable(void* chunk_backing) {
	return reinterpret_cast<StreamedChunk*>(chunk_backing)->unloadable;
}

// Weak fallbacks for the two async-streaming-loader selector/wakeup symbols. The Rust Embassy BSP
// provides the real definitions (streaming_loader.rs) whenever it links this crate —
// unconditionally, so `deluge_streaming_async_active()` always resolves there regardless of
// whether `async_streaming_loader` is enabled (its return value depends on the cargo feature; the
// symbol's existence does not). Every other BSP/config (legacy/host-cooperative sim, rza1) never
// links that crate, so these weak definitions are what resolve instead: "no async backing, never
// signalled" — i.e. today's synchronous-fiber-pump behaviour.
__attribute__((weak)) bool deluge_streaming_async_active(void) {
	return false;
}

__attribute__((weak)) void deluge_streaming_signal_fill(void) {
	// No async task to wake on this BSP/config.
}

// Weak fallbacks for the embedded-fatfs streaming READ symbols. The Rust Embassy BSP provides the
// real definitions (efatfs_fs.rs / streaming_loader.rs) whenever it links this crate with the
// `efatfs_streaming` feature; every other BSP/config resolves these instead: "no efatfs backing" —
// open always fails (caller falls back to the C-FatFS sector path), close is a no-op, and the
// selector is false.
__attribute__((weak)) bool deluge_efatfs_open(const char* /*path*/, uint32_t* /*out_handle*/) {
	return false;
}

__attribute__((weak)) void deluge_efatfs_close(uint32_t /*handle*/) {
	// No efatfs handle table on this BSP/config.
}

__attribute__((weak)) bool deluge_efatfs_read_at(uint32_t /*handle*/, uint32_t /*byte_offset*/, void* /*dst*/,
                                                 uint32_t /*count*/, uint32_t* /*out_read*/) {
	return false; // No efatfs handle table on this BSP/config.
}

__attribute__((weak)) bool deluge_streaming_efatfs_active(void) {
	return false;
}

} // extern "C"
