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
/// `AudioFileManager::readClusterData`, `SampleRecorder`) -- not a general
/// file API. `WaveTable`/presets/songs stay on `file_io.h`. See
/// docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md.
///
/// `write_at`'s real contract is sequential append, not general random-access
/// write -- callers write whole clusters in increasing index order, and `count`
/// may not exceed one cluster per call (checked; violating this desyncs the
/// write-side "most recently written cluster" bookkeeping `sector_of` relies on).
/// `read_at` reads exactly one cluster per call, from a cluster-aligned offset --
/// not a general arbitrary-byte-range reader.
#ifndef LIBDELUGE_STREAM_IO_H
#define LIBDELUGE_STREAM_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An open stream. Opaque; owned by the BSP implementation.
typedef struct DelugeStream DelugeStream;

typedef enum DelugeStreamMode {
	DELUGE_STREAM_READ,             ///< open an existing file; resolves full layout at open
	DELUGE_STREAM_WRITE_CREATE,     ///< create the file, truncating if it exists
	DELUGE_STREAM_WRITE_CREATE_NEW, ///< create the file; fails with DELUGE_ERR_EXISTS if it already exists
	DELUGE_STREAM_WRITE_APPEND,     ///< open an existing file for writing at its current size; no truncation
} DelugeStreamMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually pass to
/// `deluge_stream_close`. [task]
DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out);

/// Read exactly `count` bytes at cluster-aligned `byte_offset` into `dst`.
/// `*out_read` is the number of bytes actually read. Internally rounds `count` up to a
/// whole sector (512-byte) boundary for the underlying block read, so `dst` must be sized
/// to that sector-rounded count, not just `count` bytes -- up to 511 bytes may be written
/// past `count`. Safe under this codebase's fixed cluster-size-buffer convention (matches
/// pre-migration behavior). [task]
DelugeStatus deluge_stream_read_at(DelugeStream* stream, uint32_t byte_offset, void* dst, uint32_t count,
                                   uint32_t* out_read);

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
/// In `DELUGE_STREAM_READ` mode, any already-resolved cluster index. In a write mode,
/// only the most recently `write_at`-completed cluster (returns `DELUGE_ERR_PARAM` for
/// any other index -- this boundary never keeps a full write-side layout table). Only
/// meaningful for sector-addressed backends (FatFS-family); a backend without sector
/// geometry (e.g. a future Linux/POSIX implementation) returns `DELUGE_ERR_UNSUPPORTED`.
/// Exists for two FatFS-specific, non-hot-path callers: `AudioFileManager`'s cold-path
/// "did the card's file change" identity re-validation on the read side, and
/// `SampleRecorder`'s per-cluster `sdAddress` bookkeeping on the write side -- the
/// real-time paths use `deluge_stream_read_at`/`write_at` exclusively. [task]
DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_STREAM_IO_H
