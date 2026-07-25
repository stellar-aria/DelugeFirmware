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
	uint32_t num_sectors; ///< sectors to read (accounts for a short final cluster) — the read LENGTH
	bool ok;              ///< false => skip the read (unloadable / geometry error); do not call finish
	uint32_t handle;      ///< efatfs file handle for this stream (streaming read is efatfs-only in R1)
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

/// @brief The chunk's payload buffer base — where its cluster data lives once read.
///
/// The ONLY other `StreamedChunk` field-touch the native Rust fill task needs, alongside
/// deluge_streaming_chunk_set_loaded: `StreamedChunk::payload().data()`, reinterpreted as a
/// `uint8_t*` DMA/read destination (mirrors the `dest` field deluge_streaming_begin_fill resolves
/// today).
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
/// @return The chunk's payload buffer base.
uint8_t* deluge_streaming_chunk_payload(void* chunk_backing);

/// @brief Mark the chunk's payload as loaded/ready.
///
/// Sets `StreamedChunk::loaded = true` — the flag the C++ region cursor reads to see a chunk is
/// ready. The ONLY other `StreamedChunk` field-touch the native Rust fill task needs, alongside
/// deluge_streaming_chunk_payload.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
void deluge_streaming_chunk_set_loaded(void* chunk_backing);

/// @brief The chunk's pre-conversion convert-state, mirrored from `StreamedChunk`'s own fields.
///
/// `first_three_bytes` is the PRE-conversion first 3 bytes of the chunk's raw data (read by a
/// neighbour's boundary stitch, which needs the byte pattern spanning the cluster boundary before
/// this chunk's own in-place conversion overwrote it); `start_converted`/`end_converted` are
/// idempotency guards so a boundary is never re-stitched once it's already been handled from the
/// other side.
typedef struct DelugeChunkConvertState {
	uint8_t first_three_bytes[3]; ///< Pre-conversion first 3 raw bytes of the chunk's payload.
	bool start_converted;         ///< Whether this chunk's start boundary has already been stitched.
	bool end_converted;           ///< Whether this chunk's end boundary has already been stitched.
} DelugeChunkConvertState;

/// @brief Read the chunk's current convert-state.
///
/// Mirrors `StreamedChunk::first_three_bytes_pre_data_conversion` /
/// `extra_bytes_at_start_converted` / `extra_bytes_at_end_converted` directly — the same fields the
/// legacy sync-fiber `finish_fill` path reads/writes today. Added ahead of the native Rust fill
/// task rewiring onto this store (a later step); not yet called from anywhere.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
/// @return The chunk's current convert-state.
DelugeChunkConvertState deluge_streaming_chunk_convert_state(void* chunk_backing);

/// @brief Write the chunk's convert-state.
///
/// The inverse of deluge_streaming_chunk_convert_state — mirrors @p state back onto
/// `StreamedChunk`'s own fields.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer, as returned by
///                       deluge_resource_loader_next.
/// @param state The convert-state to store.
void deluge_streaming_chunk_set_convert_state(void* chunk_backing, DelugeChunkConvertState state);

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

/// @brief Synchronously read @p count bytes at absolute @p byte_offset of the file behind @p handle.
///
/// The sync-path counterpart of the async `ProdOps::read` efatfs branch: the C++ synchronous cluster
/// loader (`SampleStream::read_cluster_data`) reads through this. The Rust implementation
/// (`efatfs_fs.rs` / `efatfs_host_shim.rs`, cargo feature `efatfs_streaming`) bridges to the async
/// handle table via the worker fiber's `block_on_fiber` — valid only while on the worker fiber.
/// @param handle     A handle previously returned by deluge_efatfs_open.
/// @param byte_offset Absolute byte offset within the file to read from.
/// @param dst        Destination buffer; exactly @p count bytes are written on success.
/// @param count      Number of bytes to read.
/// @param out_read   Receives the bytes read on success (== @p count); untouched on failure.
/// @return true iff the full @p count bytes were read. false (off-fiber, unmounted, bad handle, or a
///         short/failed read) leaves @p dst partially written and @p out_read untouched — the caller
///         must treat it as a read error. Every non-efatfs BSP/config links the weak no-op fallback.
bool deluge_efatfs_read_at(uint32_t handle, uint32_t byte_offset, void* dst, uint32_t count, uint32_t* out_read);

/// @brief Whether the embedded-fatfs streaming READ path (cargo feature `efatfs_streaming`) owns
///        the read on this build/BSP.
///
/// A runtime getter, mirroring deluge_streaming_async_active: the C++ `deluge_app` is built once by
/// CMake and linked into whichever BSP, so a Rust cargo feature can't reach a C++ preprocessor
/// define. True only on the Rust/Embassy BSP with `efatfs_streaming` enabled; every other
/// BSP/config links the `__attribute__((weak))` fallback in `async_fill.cpp`, which returns false.
/// @return true if the efatfs streaming read path is active on this build/BSP.
bool deluge_streaming_efatfs_active(void);

/// @brief Per-asset geometry the native Rust fill task will read directly, once it exists.
///
/// Registered by `deluge_streaming_set_fill_context` at sample-load; today nothing reads it back —
/// this struct and its setter only ADD the storage, ahead of the task that will consume it (a later
/// step rewrites `ProdOps::begin`/`finish`, in `streaming_loader.rs`, to resolve a queued chunk's
/// fill descriptor from this table natively instead of round-tripping through
/// `deluge_streaming_begin_fill`/`deluge_streaming_finish_fill`). Mirrors the fields
/// `deluge::audio::stream::begin_fill` (async_fill.cpp) itself reads off `Sample`/`Cluster` today.
typedef struct DelugeStreamingFillContext {
	uint32_t efatfs_handle;              ///< This sample's open embedded-fatfs read handle (0 = none yet).
	uint32_t audio_data_start_pos_bytes; ///< Offset from the start of the file to the first audio byte.
	uint64_t audio_data_length_bytes;    ///< Audio payload length in bytes; 0x8FFFFFFFFFFFFFFF = still recording.
	int32_t first_cluster_index_with_no_audio_data; ///< First cluster index past the end of the audio data.
	uint32_t cluster_size;                          ///< Cluster::size — bytes per (non-final) cluster.
	uint32_t cluster_size_magnitude;                ///< Cluster::size_magnitude — log2(cluster_size).
	uint8_t raw_data_format;                        ///< Sample::rawDataFormat (RawDataFormat's uint8_t representation).
} DelugeStreamingFillContext;

/// @brief Register (or replace) asset @p asset's streaming fill-context.
///
/// Called unconditionally from `SampleStream::ensure_resource_asset()` once the asset is defined
/// (and again from `open_read_stream()` if the efatfs handle becomes known afterwards) —
/// see `sample_stream.cpp`. Always compiled on the Rust/Embassy BSP regardless of the
/// `async_streaming_loader` cargo feature (asset definition happens on every BSP, not just the ones
/// with the native fill task built in); every other BSP/config links the `__attribute__((weak))`
/// no-op fallback in `async_fill.cpp`.
/// @param mgr   The resource manager instance (see deluge_streaming_resource_manager). Unused by the
///              Rust implementation today (there is exactly one process-wide manager); kept in the
///              signature for parity with the rest of the manager-scoped C ABI.
/// @param asset The asset id (`SampleStream::resource_asset_id()`).
/// @param ctx   The geometry to register for @p asset.
void deluge_streaming_set_fill_context(DelugeResource* mgr, uint32_t asset, DelugeStreamingFillContext ctx);

#ifdef __cplusplus
}
#endif
