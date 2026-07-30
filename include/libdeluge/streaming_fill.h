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

/// libdeluge/streaming_fill.h — the cluster-fill descriptor + resource-manager/asset C-ABI.
///
/// The Rust async fill task (deluge-bsp-rust's `streaming_loader.rs`, over `deluge_sample_fill`'s
/// `native_begin`/`native_finish`) resolves each queued chunk's read geometry into a
/// `StreamingFillDescriptor`, performs the read, then converts/stitches/publishes. This header
/// carries the shared FFI types (the descriptor) plus the resource-manager/asset and
/// signal/readiness entry points that cross the C++/Rust boundary. (The old synchronous C-ABI
/// split — `deluge_streaming_begin_fill`/`_finish_fill` — was retired with
/// `SampleStream::read_cluster_data`; the async task calls `native_begin`/`native_finish` directly.)
#pragma once
#include "libdeluge/types.h" // DelugeStatus
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct DelugeResource DelugeResource;

/// @brief Descriptor the fill resolves from a queued chunk's geometry (`native_begin`): where to
///        DMA, and how much.
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

/// @brief The resource-manager instance the streaming loader queue lives on.
/// @return The single DelugeResource instance — the same one returned by
///         GeneralMemoryAllocator's resourceManager().
DelugeResource* deluge_streaming_resource_manager(void);

/// @brief The resource-manager Asset id backing @p stream_backing, defining it first if needed.
///
/// The region-port `open()` bridge (SR2d-5 Task 1): the reader passes its
/// `deluge::audio::stream::SampleStream*` as `deluge_sample_source_open`'s opaque `stream_backing`,
/// and the Rust cursor resolves it to `{deluge_streaming_resource_manager(), this accessor's
/// return}` before building its own residency provider — the same `{manager handle, asset id}`
/// pair `SampleStream::ensure_resource_asset()` itself defines against
/// `deluge_streaming_resource_manager()`. Calls the LAZY-init entry (`ensure_resource_asset()`,
/// not the plain `resource_asset_id()` getter): the asset may not be defined yet on a reader's
/// first open (e.g. a sample that has never been through `SampleStream::get_cluster()`), and this
/// is the one entry point that defines it on demand.
/// @param stream_backing Opaque `SampleStream*` backing pointer, as passed to
///                        deluge_sample_source_open. The real definition (`sample_stream.cpp`)
///                        requires this to be a live `SampleStream*`; the `__attribute__((weak))`
///                        no-op fallback (`async_fill.cpp`, for build configs without a real
///                        `SampleStream`) ignores it.
/// @return The Asset id, or DELUGE_RESOURCE_NO_ASSET on the weak fallback.
uint32_t deluge_sample_stream_asset_id(void* stream_backing);

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

/// Mark a queued/resident chunk unloadable (the write half of `deluge_streaming_chunk_unloadable`),
/// so the async loader skips it rather than completing a fill with stale bytes. Called by
/// `deluge_sample_invalidate` when a sample's backing file has gone away.
void deluge_streaming_chunk_set_unloadable(void* chunk_backing);

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

/// @brief Read the chunk's loaded/ready flag (the read half of deluge_streaming_chunk_set_loaded).
///
/// Returns `StreamedChunk::loaded` — the flag the async drain's `native_finish` tail flips true
/// once the chunk is read + converted + stitched. Polled by deluge_streaming_fill_chunk_blocking's
/// on-fiber yield-wait to tell when the drain has landed a chunk it is blocking on.
/// @param chunk_backing Opaque `StreamedChunk` backing pointer.
/// @return true if the chunk's payload is loaded/ready.
bool deluge_streaming_chunk_loaded(void* chunk_backing);

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

/// @brief The byte offset a streamed chunk's payload sits at within its slab slot (U4d). The C++
///        slab setup sizes the shared cluster slot as max(this, ComputedChunk's kChunkPayloadOffset)
///        + Cluster::size + trailing guard. Rust-owned (deluge_sample_fill).
uint32_t deluge_streamed_chunk_payload_offset(void);

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

/// @brief Fill a reserved chunk through the async drain, blocking on the worker fiber.
///
/// The async-BSP replacement for the range reader's synchronous `fill_now` (deluge_sample_reader's
/// `reader.rs`), reached only when deluge_streaming_async_active() is true. Enqueues @p chunk_backing
/// on the loader queue at the most-urgent priority and wakes `streaming_fill_task`, then:
/// - ON the worker fiber: yields (the executor drains the fill task while suspended) until the
///   chunk's `loaded` flag flips, returning readiness. Byte-equivalent to the synchronous fill —
///   same `native_finish` convert/stitch/publish tail — just awaited instead of run inline, and
///   without the off-fiber `block_on` that deadlocks a single-threaded executor against a
///   fiber-suspended load holding the embedded-fatfs mutex.
/// - OFF the worker fiber: returns false immediately (degrade-to-eventual). An off-fiber caller has
///   no stack to suspend; the chunk stays enqueued. The sole off-fiber caller today is the
///   display-only background waveform overview pre-scan, whose consumer already retries a not-ready
///   read on the next tick.
///
/// The real implementation lives in `streaming_loader.rs` (always compiled on the Rust/Embassy BSP);
/// every other BSP/config links the `__attribute__((weak))` fallback in `async_fill.cpp` (returns
/// false, never reached on the hot path since those BSPs report deluge_streaming_async_active()
/// false and take the synchronous `fill_now` branch).
/// @param chunk_backing Opaque `StreamedChunk` backing pointer for a reserved, still-leased chunk.
/// @return true once the chunk is resident+ready; false on the off-fiber degrade or a failed fill.
bool deluge_streaming_fill_chunk_blocking(void* chunk_backing);

/// @brief Drain the WHOLE loader queue through the async fill task, blocking on the worker fiber.
///
/// The offline stem-export drain (`StemExport::renderWait`'s async-BSP branch), reached only when
/// deluge_streaming_async_active() is true. Unlike deluge_streaming_fill_chunk_blocking, which blocks
/// on ONE named chunk, this wakes `streaming_fill_task` and yield-waits until the loader queue is
/// empty — matching the C-host between-routines `loader::pump()` that drains everything the preceding
/// `AudioEngine::routine()` enqueued. renderWait's offline loop has no other yield point, so this is
/// what lets the single Embassy executor actually run the fill task between audio routines.
/// - ON the worker fiber (renderWait's context): yields until the queue drains, bounded by a cycle
///   cap so a persistently-failing read degrades to a not-fully-drained result instead of wedging.
/// - OFF the worker fiber: returns false immediately (no stack to suspend; renderWait is always
///   on-fiber, so this is only a safety net).
///
/// The real implementation lives in `streaming_loader.rs` (always compiled on the Rust/Embassy BSP);
/// every other BSP/config links the `__attribute__((weak))` fallback in `async_fill.cpp` (returns
/// false, never reached on the hot path since those BSPs report deluge_streaming_async_active() false
/// and take the synchronous `loader::pump()` branch).
/// @return true once the queue is fully drained; false on the off-fiber degrade or a hit cycle cap.
bool deluge_streaming_drain_queue_blocking(void);

/// @brief Open a sample file for streaming reads through embedded-fatfs, writing its handle out.
///
/// The sync→async bridge for the streaming READ path: C++ calls this synchronously at sample-load,
/// and the Rust implementation (`efatfs_fs.rs`, cargo feature `efatfs_streaming`) bridges to the
/// async handle table via the worker fiber's `block_on_fiber`. Valid only while on the worker fiber.
/// @param path           NUL-terminated absolute file path.
/// @param out_handle     Receives the opaque handle on success; untouched on failure.
/// @param out_table_full Always written (on both success and failure): true iff the open failed
///                       specifically because the streaming-read handle table has no free slot (as
///                       opposed to the file genuinely not existing, or the FS being unmounted) --
///                       the caller maps a `true` value to a dedicated `Error::TOO_MANY_OPEN_STREAMS`
///                       rather than the misleading `Error::FILE_NOT_FOUND`. Always `false` when
///                       @p out_handle was written.
/// @return true if the file was opened and @p out_handle written; false (caller falls back to the
///         C-FatFS sector path) if not on the worker fiber, the path/pointer is invalid, the FS is
///         unmounted, or the open failed. Every non-efatfs BSP/config links the weak no-op fallback
///         in `async_fill.cpp`, which always returns false and writes `*out_table_full = false`.
bool deluge_efatfs_open(const char* path, uint32_t* out_handle, bool* out_table_full);

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
	uint8_t byte_depth;   ///< Sample::byteDepth — bytes per channel-sample (e.g. 2 for 16-bit).
	uint8_t num_channels; ///< Sample::numChannels — channel count (1 = mono, 2 = stereo).
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
