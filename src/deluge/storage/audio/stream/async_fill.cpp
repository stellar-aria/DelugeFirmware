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
#include <optional>
#include <span>

#include "deluge_resource.h" // deluge_resource_mark_ready

// Body lifted verbatim (behaviour-inert split) from the pre-refactor
// deluge::audio::stream::SampleStream::read_cluster_data — see git history for the monolithic
// version. begin_fill() is the "resolve where/how much" half (sample_stream.cpp:161-176 +
// sd_address_at lookup); finish_fill() is the post-read "convert + stitch + publish" half
// (sample_stream.cpp:237-289). Both stay reachable through the extern "C" wrappers below, which
// are the only things read_cluster_data() now calls directly.

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
			return StreamingFillDescriptor{.dest = nullptr, .sector = 0, .num_sectors = 0, .ok = false};
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

} // extern "C"
