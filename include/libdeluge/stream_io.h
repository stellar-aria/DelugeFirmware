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
/// Scoped specifically to `Sample` cluster streaming (`SampleStream`'s read
/// source for playback, `SampleRecorder` for the write side) -- not a general
/// file API. `WaveTable`/presets/songs stay on `file_io.h`. See
/// docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md.
///
/// `write_at` is positional but never sparse: `byte_offset` may be anywhere at or
/// below the stream's current end of file, never past it. Callers append whole
/// clusters in increasing index order, then rewrite already-written bytes in place
/// (`SampleRecorder`'s finalize header patch and `alterFile`'s cluster rewrites).
/// `count` may not exceed one cluster per call (checked; violating this desyncs the
/// write-side "most recently written cluster" bookkeeping `sector_of` relies on).
/// `read_at` is NOT the streaming read path (that goes through embedded-fatfs,
/// `deluge_efatfs_read_at`) -- it is only the read-BACK of a stream this boundary is
/// itself writing; plus the cold-path `sector_of` identity check.
#ifndef LIBDELUGE_STREAM_IO_H
#define LIBDELUGE_STREAM_IO_H

#include "types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// An open stream. Opaque; owned by the BSP implementation.
typedef struct DelugeStream DelugeStream;

/// @brief Mode for deluge_stream_open: what the stream is opened for.
typedef enum DelugeStreamMode : uint8_t {
	DELUGE_STREAM_READ,             ///< open an existing file (sector_of()-only; no read_at)
	DELUGE_STREAM_WRITE_CREATE,     ///< create the file, truncating if it exists
	DELUGE_STREAM_WRITE_CREATE_NEW, ///< create the file; fails with DELUGE_ERR_EXISTS if it already exists
	DELUGE_STREAM_WRITE_APPEND,     ///< open an existing file for writing at its current size; no truncation
} DelugeStreamMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually pass to
/// `deluge_stream_close`. [task]
DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out);

/// @brief Write @p count bytes at @p byte_offset into the stream.
///
/// @p byte_offset may be at the stream's current end-of-file (an append, extending it) or anywhere
/// below it (an in-place rewrite); past the end is DELUGE_ERR_PARAM, since that would leave a hole.
/// @p count must not exceed one cluster (DELUGE_ERR_PARAM otherwise) -- write_at writes at most one
/// cluster per call, same limit as read_at.
/// @param stream      The open stream.
/// @param byte_offset Absolute byte offset to write at (see above for the append/rewrite rule).
/// @param src         Source buffer of @p count bytes.
/// @param count       Number of bytes to write; must not exceed one cluster.
/// @param out_written Receives the number of bytes actually written.
/// @return DELUGE_OK on success, or an error status.
DelugeStatus deluge_stream_write_at(DelugeStream* stream, uint32_t byte_offset, const void* src, uint32_t count,
                                    uint32_t* out_written);

/// @brief Read up to @p count bytes at absolute @p byte_offset into @p dst.
///
/// Bounded by the stream's LIVE size -- including bytes written earlier through this same
/// still-open handle, whose size the on-disk directory entry has not caught up with yet.
/// EOF-honest: @p out_read is the TRUE count read (<= count, 0 at or past EOF), never
/// zero-padded. This is NOT the streaming read path (that goes through embedded-fatfs,
/// deluge_efatfs_read_at) -- it is only the read-BACK of a stream this boundary is itself
/// writing; plus the cold-path sector_of identity check. The C-FatFS counterpart of
/// deluge_efatfs_stream_read_at_via; SampleRecorder's mid-write read-back of an evicted cluster
/// of the file it is still recording (see storage/audio/stream/read_source.h's
/// RecordingReadSource) is its only caller.
/// @param stream      The open stream.
/// @param byte_offset Absolute byte offset to read from.
/// @param dst         Destination buffer; up to @p count bytes are written.
/// @param count       Maximum number of bytes to read.
/// @param out_read    Receives the true number of bytes read (<= count, 0 at/past EOF).
/// @return DELUGE_OK on success, or an error status.
DelugeStatus deluge_stream_read_at(DelugeStream* stream, uint32_t byte_offset, void* dst, uint32_t count,
                                   uint32_t* out_read);

/// Truncate (or, if smaller than the current position, no-op) the stream to `new_size` bytes. [task]
DelugeStatus deluge_stream_truncate(DelugeStream* stream, uint32_t new_size);

/// Total size of the stream, in bytes. [task]
DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size);

/// Close a stream opened with `deluge_stream_open`. `stream` is invalid after this call
/// regardless of the returned status. [task]
DelugeStatus deluge_stream_close(DelugeStream* stream);

/// @brief Best-effort: the physical sector address backing cluster @p cluster_index (0-based).
///
/// In DELUGE_STREAM_READ mode, walks the FAT chain from the file's start cluster on demand (no
/// cached layout table is kept). In a write mode, only the most recently write_at-completed
/// cluster (returns DELUGE_ERR_PARAM for any other index -- this boundary never keeps a full
/// write-side layout table). Only meaningful for sector-addressed backends (FatFS-family); a
/// backend without sector geometry (e.g. a future Linux/POSIX implementation) returns
/// DELUGE_ERR_UNSUPPORTED. Its only remaining caller is AudioFileManager's cold-path "did the
/// card's file change" identity re-validation on the read side -- the real-time read path goes
/// through embedded-fatfs (deluge_efatfs_read_at), not this boundary. Present-but-dead on the
/// C++ side whenever efatfs is the active backend (deluge::io::Stream::sector_of doesn't call
/// this in that case -- see its doc); kept until the rest of the C-FatFS deluge_stream_* write
/// backing is removed.
/// @param stream        The open stream.
/// @param cluster_index Cluster index (0-based) to resolve.
/// @param out_sector    Receives the physical sector address on success.
/// @return DELUGE_OK on success, DELUGE_ERR_PARAM for an unresolvable write-mode index, or
///         DELUGE_ERR_UNSUPPORTED on a backend without sector geometry.
DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector);

/// The embedded-fatfs (Rust `efatfs_streaming`) persistent stream-write C-ABI.
/// `deluge::io::Stream` (stream.cpp) routes recorder writes through these instead of the
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
/// doc). Every handle-taking function's handle is an opaque `u32` (the caller boxes it into a
/// `DelugeStream*`, mirroring the streaming-read handle in sample_stream.h).
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
