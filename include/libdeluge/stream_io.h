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

/// libdeluge/stream_io.h — real-time Sample audio streaming (playback read,
/// recording write).
///
/// Scoped specifically to `Sample` cluster streaming (`ClusterByteSource`,
/// `SampleStream::read_cluster_data`, `SampleRecorder`) -- not a general
/// file API. `WaveTable`/presets/songs stay on `file_io.h`. See
/// docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md.
///
/// `write_at`'s real contract is sequential append, not general random-access
/// write -- callers write whole clusters in increasing index order, and `count`
/// may not exceed one cluster per call (checked; violating this desyncs the
/// write-side "most recently written cluster" bookkeeping `sector_of` relies on).
/// There is no `read_at` -- the streaming read path goes through embedded-fatfs
/// (`deluge_efatfs_read_at`); this boundary is write (recording) plus the
/// cold-path `sector_of` identity check only.
#ifndef LIBDELUGE_STREAM_IO_H
#define LIBDELUGE_STREAM_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An open stream. Opaque; owned by the BSP implementation.
typedef struct DelugeStream DelugeStream;

typedef enum DelugeStreamMode {
	DELUGE_STREAM_READ,             ///< open an existing file (sector_of()-only; no read_at)
	DELUGE_STREAM_WRITE_CREATE,     ///< create the file, truncating if it exists
	DELUGE_STREAM_WRITE_CREATE_NEW, ///< create the file; fails with DELUGE_ERR_EXISTS if it already exists
	DELUGE_STREAM_WRITE_APPEND,     ///< open an existing file for writing at its current size; no truncation
} DelugeStreamMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually pass to
/// `deluge_stream_close`. [task]
DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out);

/// Append `count` bytes at `byte_offset` (must equal the stream's current end-of-file --
/// sequential append only). `count` must not exceed one cluster (`DELUGE_ERR_PARAM` otherwise) --
/// write_at writes at most one cluster per call, same limit as `read_at`. `*out_written` is the
/// number of bytes actually written. [task]
DelugeStatus deluge_stream_write_at(DelugeStream* stream, uint32_t byte_offset, const void* src, uint32_t count,
                                    uint32_t* out_written);

/// Truncate (or, if smaller than the current position, no-op) the stream to `new_size` bytes. [task]
DelugeStatus deluge_stream_truncate(DelugeStream* stream, uint32_t new_size);

/// Total size of the stream, in bytes. [task]
DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size);

/// Close a stream opened with `deluge_stream_open`. `stream` is invalid after this call
/// regardless of the returned status. [task]
DelugeStatus deluge_stream_close(DelugeStream* stream);

/// Best-effort: the physical sector address backing cluster `cluster_index` (0-based).
/// In `DELUGE_STREAM_READ` mode, walks the FAT chain from the file's start cluster on
/// demand (no cached layout table is kept). In a write mode, only the most recently
/// `write_at`-completed cluster (returns `DELUGE_ERR_PARAM` for any other index -- this
/// boundary never keeps a full write-side layout table). Only meaningful for
/// sector-addressed backends (FatFS-family); a backend without sector geometry (e.g. a
/// future Linux/POSIX implementation) returns `DELUGE_ERR_UNSUPPORTED`. Exists for two
/// FatFS-specific, non-hot-path callers: `AudioFileManager`'s cold-path "did the card's
/// file change" identity re-validation on the read side, and `SampleRecorder`'s
/// per-cluster `sdAddress` bookkeeping on the write side -- the real-time read path goes
/// through embedded-fatfs (`deluge_efatfs_read_at`), not this boundary. [task]
DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector);

/// R3 Task 2 -- the embedded-fatfs (Rust `efatfs_streaming`) persistent stream-write C-ABI.
/// `deluge::io::Stream` (stream.cpp, Task 3) routes recorder writes through these instead of the
/// `deluge_stream_*` functions above whenever `deluge_streaming_efatfs_active()` (declared in
/// streaming_fill.h) is true; every non-efatfs BSP/config links the `__attribute__((weak))`
/// fallback in `async_fill.cpp` (always false/no-op), so these symbols always link regardless of
/// backend. Implemented in `efatfs_fs.rs` (device) / `efatfs_host_shim.rs` (host), both composing
/// the storage-generic `efatfs_core::write_context_noflush`/`flush_context`/
/// `read_at_via_context` primitives -- see those files' doc comments for the handle-table/locking
/// discipline.
///
/// The handle holds ONE persistent `FileContext` across the life of a recording -- unlike the
/// `deluge_stream_*` handles above (which round-trip through FatFS on every op), this table keeps
/// the file's in-memory size/mtime edit resident between writes and only persists it to the
/// on-disk directory entry on `deluge_efatfs_stream_flush`/`_close` (see `write_context_noflush`'s
/// doc). Every handle-taking function's handle is an opaque `u32` (the caller, Task 3, boxes it
/// into a `DelugeStream*`, mirroring the R1 streaming handle).
///
/// Unlike `deluge_stream_*`, these are `bool` (not `DelugeStatus`): the Rust side has no notion of
/// `DelugeStatus`'s finer-grained error codes, only success/failure. `stream.cpp` maps `false` to
/// `Status::ERR`.

/// @brief Open a persistent stream-write handle. `mode` matches `DelugeStreamMode`'s
///        declaration-order values (0=READ, 1=WRITE_CREATE, 2=WRITE_CREATE_NEW, 3=WRITE_APPEND).
bool deluge_efatfs_stream_open(const char* path, uint8_t mode, uint32_t* out_handle);

/// @brief Write `count` bytes at absolute `byte_offset`, advancing the handle's persisted
///        `FileContext` by the bytes actually written (`*out_written`). Does NOT flush the
///        advanced size/mtime to the on-disk directory entry -- see `deluge_efatfs_stream_flush`.
bool deluge_efatfs_stream_write_at(uint32_t handle, uint32_t byte_offset, const void* src, uint32_t count,
                                   uint32_t* out_written);

/// @brief EOF-honest read at absolute `byte_offset`, bounded by the handle's IN-MEMORY size (i.e.
///        this can read back data written earlier in the same unflushed session). `*out_read` is
///        the TRUE byte count actually read (`<= count`, less at EOF), never zero-padded.
bool deluge_efatfs_stream_read_at_via(uint32_t handle, uint32_t byte_offset, void* dst, uint32_t count,
                                      uint32_t* out_read);

/// @brief Persist the handle's accumulated in-memory size/mtime edit (built up by prior
///        `deluge_efatfs_stream_write_at` calls) to the on-disk directory entry.
bool deluge_efatfs_stream_flush(uint32_t handle);

/// @brief Truncate the file behind `handle` to `new_len` bytes.
bool deluge_efatfs_stream_truncate(uint32_t handle, uint32_t new_len);

/// @brief Size of the file behind `handle`, in bytes.
bool deluge_efatfs_stream_size(uint32_t handle, uint32_t* out_size);

/// @brief Flush (see `deluge_efatfs_stream_flush`) and close a persistent stream-write handle
///        opened via `deluge_efatfs_stream_open`, freeing its slot.
bool deluge_efatfs_stream_close(uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_STREAM_IO_H
