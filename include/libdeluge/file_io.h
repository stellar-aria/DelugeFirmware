/*
 * Copyright © 2014-2025 Synthstrom Audible Limited
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

/// libdeluge/file_io.h — file-level storage access.
///
/// The application's portable code (storage_manager, audio_file_manager, the
/// browser, the companion SysEx protocol, ...) reads/writes files and
/// directories through this boundary instead of a specific filesystem
/// library. On rza1/host/Embassy this is backed by the vendored FatFS
/// library over `block_device.h`; that dependency is a BSP-internal
/// implementation detail, not something the app links against directly. A
/// BSP may instead back it with the host OS's native filesystem (e.g. a
/// Linux BSP mapping straight to POSIX I/O) with no FatFS involved at all.
///
/// Paths are plain forward-slash strings rooted at the storage volume (no
/// FatFS drive-number prefix, e.g. "SONGS/mysong.XML" not "0:/SONGS/...").
#ifndef LIBDELUGE_FILE_IO_H
#define LIBDELUGE_FILE_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An open file. Opaque; owned by the BSP implementation.
typedef struct DelugeFile DelugeFile;

/// An open directory iterator. Opaque; owned by the BSP implementation.
typedef struct DelugeDir DelugeDir;

/// Longest filename this boundary will report/accept, including the NUL
/// terminator (FAT LFN max is 255 characters).
#define DELUGE_MAX_FILENAME 256

typedef enum DelugeFileOpenMode {
	DELUGE_FILE_READ,         ///< open an existing file for reading
	DELUGE_FILE_WRITE_CREATE, ///< create the file, truncating if it exists
} DelugeFileOpenMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually
/// pass to `deluge_file_close`. [task]
DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out);

/// Read up to `count` bytes into `dst`. `*out_read` is the number of bytes
/// actually read (may be less than `count` at end of file). [task]
DelugeStatus deluge_file_read(DelugeFile* file, void* dst, uint32_t count, uint32_t* out_read);

/// Write `count` bytes from `src`. `*out_written` is the number of bytes
/// actually written. [task]
DelugeStatus deluge_file_write(DelugeFile* file, const void* src, uint32_t count, uint32_t* out_written);

/// Move the file position to an absolute byte offset. [task]
DelugeStatus deluge_file_seek(DelugeFile* file, uint32_t offset);

/// Total size of the file, in bytes. [task]
DelugeStatus deluge_file_size(DelugeFile* file, uint32_t* out_size);

/// Close a file opened with `deluge_file_open`. `file` is invalid after this
/// call regardless of the returned status. [task]
DelugeStatus deluge_file_close(DelugeFile* file);

/// One directory entry returned by `deluge_dir_read`.
typedef struct DelugeDirEntry {
	char name[DELUGE_MAX_FILENAME];
	bool is_directory;
} DelugeDirEntry;

/// Open `path` as a directory for iteration. [task]
DelugeStatus deluge_dir_open(const char* path, DelugeDir** out);

/// Read the next directory entry. If there are no more entries,
/// `*out_has_entry` is set to `false` and the call still returns
/// `DELUGE_OK` (end of directory is not an error). [task]
DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry);

/// Close a directory opened with `deluge_dir_open`. [task]
DelugeStatus deluge_dir_close(DelugeDir* dir);

/// Create a directory. Returns `DELUGE_ERR_EXISTS` if `path` already exists
/// (matches the existing app idiom of treating that as non-fatal). [task]
DelugeStatus deluge_file_mkdir(const char* path);

/// Delete a file or empty directory. [task]
DelugeStatus deluge_file_unlink(const char* path);

/// Rename/move a file or directory. [task]
DelugeStatus deluge_file_rename(const char* old_path, const char* new_path);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_FILE_IO_H
