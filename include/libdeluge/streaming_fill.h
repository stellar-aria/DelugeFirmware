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
	uint32_t sector;      ///< physical LBA (from SampleCluster::sdAddress) — the flag-off C-FatFS path
	uint32_t num_sectors; ///< sectors to read (accounts for a short final cluster)
	bool ok;              ///< false => skip the read (unloadable / geometry error); do not call finish
	uint32_t handle;      ///< efatfs file handle for this stream (0 = none); the flag-on embedded-fatfs path
	uint32_t byte_offset; ///< absolute byte offset of this cluster within the file (efatfs read position)
} StreamingFillDescriptor;

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Resolve the destination buffer and physical sector range for a queued chunk fill.
///
/// @note Pure lookup and arithmetic — touches no SD hardware and no FatFS.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
/// @return The fill descriptor; `ok` is false if the chunk is unloadable or the geometry lookup
///         failed.
StreamingFillDescriptor deluge_streaming_begin_fill(void* chunk_backing);

/// @brief Convert, stitch, and publish readiness for a chunk's just-read payload.
///
/// @param chunk_backing Opaque `StreamedChunk` backing pointer; the same value passed to the
///                       matching deluge_streaming_begin_fill call.
/// @param read_ok        False if the underlying sector read failed: the fill is abandoned (this
///                        function returns false without touching the chunk) and the caller must
///                        re-enqueue it at lowest priority, as pump() does.
/// @return true on success.
bool deluge_streaming_finish_fill(void* chunk_backing, bool read_ok);

/// @brief The resource-manager instance the streaming loader queue lives on.
/// @return The single DelugeResource instance — the same one returned by
///         GeneralMemoryAllocator's resourceManager().
DelugeResource* deluge_streaming_resource_manager(void);

/// @brief Whether a queued chunk has been marked unloadable since it was enqueued.
///
/// Mirrors pump()'s safety-net skip right after deluge_resource_loader_next() (loader.cpp):
/// markAsUnloadable already de-queued it and loader_next cleared its queued flag, so skipping
/// here can't loop, and an unloadable chunk doesn't count against the fill budget. Only ever
/// called from the Rust async task's fill_once — the fiber pump() performs the equivalent check
/// inline, so it has no need of this getter.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
/// @return true if the chunk is currently marked unloadable.
bool deluge_streaming_chunk_unloadable(void* chunk_backing);

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
/// @return true if the async task owns the loader queue on this build/BSP.
bool deluge_streaming_async_active(void);

/// @brief Wake the async streaming-fill task out of its idle wait.
///
/// Called unconditionally at every streaming CLUSTER_ENQUEUE site (`sample_stream.cpp`), after
/// `deluge_resource_loader_enqueue()`. Harmless when the async backing isn't active: on the
/// Embassy BSP with the feature off it signals a `Signal` nobody awaits; on every other BSP it
/// hits the weak no-op fallback in `async_fill.cpp`.
void deluge_streaming_signal_fill(void);

/// @brief Open a sample file for streaming reads through embedded-fatfs, writing its handle out.
///
/// The sync→async bridge for the streaming READ path: C++ calls this synchronously at sample-load,
/// and the Rust implementation (`efatfs_fs.rs`, cargo feature `efatfs_streaming`) bridges to the
/// async handle table via the worker fiber's `block_on_fiber`. Valid only while on the worker fiber.
/// @param path       NUL-terminated absolute file path.
/// @param out_handle Receives the opaque handle on success; untouched on failure.
/// @return true if the file was opened and @p out_handle written; false (caller falls back to the
///         C-FatFS sector path) if not on the worker fiber, the path/pointer is invalid, the FS is
///         unmounted, or the open failed. Every non-efatfs BSP/config links the weak no-op fallback
///         in `async_fill.cpp`, which always returns false.
bool deluge_efatfs_open(const char* path, uint32_t* out_handle);

/// @brief Close a streaming file handle previously returned by deluge_efatfs_open.
///
/// @note An off-fiber close cannot bridge to the async table, so the slot leaks until reuse —
///       acceptable for the flag-gated SP1a path (few handles); a deferred-close is future work.
/// @param handle The handle to close. A no-op on every non-efatfs BSP/config (weak fallback).
void deluge_efatfs_close(uint32_t handle);

/// @brief Whether the embedded-fatfs streaming READ path (cargo feature `efatfs_streaming`) owns
///        the read on this build/BSP.
///
/// A runtime getter, mirroring deluge_streaming_async_active: the C++ `deluge_app` is built once by
/// CMake and linked into whichever BSP, so a Rust cargo feature can't reach a C++ preprocessor
/// define. True only on the Rust/Embassy BSP with `efatfs_streaming` enabled; every other
/// BSP/config links the `__attribute__((weak))` fallback in `async_fill.cpp`, which returns false.
/// @return true if the efatfs streaming read path is active on this build/BSP.
bool deluge_streaming_efatfs_active(void);

#ifdef __cplusplus
}
#endif
