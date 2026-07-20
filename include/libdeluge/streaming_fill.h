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

/// libdeluge/streaming_fill.h — the split cluster-fill boundary.
///
/// `SampleStream::read_cluster_data` used to do "resolve where/how much to read, do the read,
/// convert+stitch+publish" as one synchronous call. This header carries the C-ABI split of the
/// two halves either side of the actual I/O: `deluge_streaming_begin_fill` resolves the
/// destination buffer and physical sector range (pure lookup + arithmetic, no SD access, no
/// FatFS), and `deluge_streaming_finish_fill` runs the post-read convert/stitch/publish tail.
/// The caller (today: the synchronous fiber pump in read_cluster_data; later: the Rust async
/// fill task) performs the actual read between the two calls.
#pragma once
#include "libdeluge/types.h" // DelugeStatus
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DelugeResource DelugeResource;

/// @brief Descriptor filled by deluge_streaming_begin_fill: where to DMA, and how much.
typedef struct StreamingFillDescriptor {
	uint8_t* dest;        ///< cluster payload base; write exactly num_sectors*512 bytes here
	uint32_t sector;      ///< physical LBA (from SampleCluster::sdAddress)
	uint32_t num_sectors; ///< sectors to read (accounts for a short final cluster)
	bool ok;              ///< false => skip the read (unloadable / geometry error); do not call finish
} StreamingFillDescriptor;

#ifdef __cplusplus
extern "C" {
#endif

/// Phase 1: resolve the fill for a queued StreamedChunk backing pointer (as returned by
/// deluge_resource_loader_next). Pure lookup + arithmetic; touches no SD hardware, no FatFS.
StreamingFillDescriptor deluge_streaming_begin_fill(void* chunk_backing);

/// Phase 2: run convert + stitch on the just-read payload and publish readiness.
/// read_ok=false => mark the read failed (caller re-enqueues at lowest priority, as pump() does today).
/// Returns true on success.
bool deluge_streaming_finish_fill(void* chunk_backing, bool read_ok);

/// The single resource-manager instance the loader queue lives on (== GeneralMemoryAllocator resourceManager()).
DelugeResource* deluge_streaming_resource_manager(void);

/// @brief Whether the Rust async streaming-fill task (`streaming_fill_task`, cargo feature
///        `async_streaming_loader`) owns the loader queue on this build/BSP.
///
/// A runtime getter rather than a compile-time `#define`: the C++ `deluge_app` is built once by
/// CMake and linked into whichever BSP, so a Rust cargo feature can't reach a C++ preprocessor
/// define. True only on the Rust/Embassy BSP with `async_streaming_loader` enabled (the real
/// implementation lives in `streaming_loader.rs`); every other BSP/config links the
/// `__attribute__((weak))` fallback in `async_fill.cpp`, which always returns false. When true,
/// `deluge::audio::stream::loader::pump()`/`request_pump()` no-op — the task drains the (streaming-
/// only) loader queue instead.
bool deluge_streaming_async_active(void);

/// @brief Wake the async streaming-fill task out of its idle wait.
///
/// Called unconditionally at every streaming CLUSTER_ENQUEUE site (`sample_stream.cpp`), after
/// `deluge_resource_loader_enqueue()`. Harmless when the async backing isn't active: on the
/// Embassy BSP with the feature off it signals a `Signal` nobody awaits; on every other BSP it
/// hits the weak no-op fallback in `async_fill.cpp`.
void deluge_streaming_signal_fill(void);

#ifdef __cplusplus
}
#endif
