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
#include <stdint.h>

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

typedef enum DelugeFileOpenMode : uint8_t {
	DELUGE_FILE_READ,             ///< open an existing file for reading
	DELUGE_FILE_WRITE_CREATE,     ///< create the file, truncating if it exists
	DELUGE_FILE_WRITE_CREATE_NEW, ///< create the file; fails with DELUGE_ERR_EXISTS if it already exists
} DelugeFileOpenMode;

/// Set a file or directory's last-modified timestamp. [task]
DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp);

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
	uint32_t size;
	DelugeTimestamp modified_time;
	bool is_read_only;
	bool is_hidden;
	bool is_system;
	bool is_archive;
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

/// Invalidates any internal caching this boundary maintains for directory
/// contents. Call this after performing a filesystem write through some
/// mechanism OTHER than this boundary's own write functions (mkdir/unlink/
/// rename/write-create open, which already invalidate internally) -- e.g.
/// legacy code that still calls the underlying filesystem library directly.
/// Cannot fail. New code should prefer this boundary's own write functions,
/// which need no separate call. [task]
void deluge_file_invalidate_cache(void);

/// R2 Task 4 -- the embedded-fatfs (Rust `efatfs_streaming`) task-context file/
/// directory C-ABI. `deluge::io::File`/`Directory` (file.cpp) route to these
/// instead of the `deluge_file_*`/`deluge_dir_*` functions above whenever
/// `deluge_streaming_efatfs_active()` (declared in streaming_fill.h) is true;
/// every non-efatfs BSP/config links the `__attribute__((weak))` fallback in
/// `async_fill.cpp` (always false/no-op), so these symbols always link
/// regardless of backend. Implemented in `efatfs_fs.rs` (device) /
/// `efatfs_host_shim.rs` (host), both composing the storage-generic
/// `efatfs_core` primitives -- see those files' doc comments for the handle-
/// table/locking discipline. Every handle-taking function's handle is an
/// opaque `u32` boxed into the corresponding `DelugeFile*`/`DelugeDir*`
/// pointer (cast, not dereferenced) -- mirrors the R1 streaming handle.
///
/// Unlike `deluge_file_*`, these are `bool` (not `DelugeStatus`): the Rust side
/// has no notion of `DelugeStatus`'s finer-grained error codes, only
/// success/failure. `file.cpp` maps `false` to `Status::ERR`.

/// @brief Open a task-context file. `mode` matches `DelugeFileOpenMode`'s
///        declaration-order values (0=READ, 1=WRITE_CREATE, 2=WRITE_CREATE_NEW).
bool deluge_efatfs_file_open(const char* path, uint8_t mode, uint32_t* out_handle);

/// @brief Fill-semantics read at the handle's current position: on success
///        `*out_read == count` always (a short tail at real EOF is zero-padded,
///        matching the streaming read path's tolerance for a sector-rounded
///        last-cluster request) -- NOT what `deluge::io::File::read` wants; see
///        `deluge_efatfs_file_read_exact`.
bool deluge_efatfs_file_read(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read);

/// @brief EOF-honest read at the handle's current position -- the efatfs
///        backend for `deluge::io::File::read`. `*out_read` is the TRUE byte
///        count actually read (`<= count`, less at real EOF), never zero-padded.
bool deluge_efatfs_file_read_exact(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read);

/// @brief Write `count` bytes at the handle's current position, advancing it by
///        the bytes actually written (`*out_written`).
bool deluge_efatfs_file_write(uint32_t handle, const void* src, uint32_t count, uint32_t* out_written);

/// @brief Move the handle's cursor to an absolute byte offset.
bool deluge_efatfs_file_seek(uint32_t handle, uint32_t offset);

/// @brief Total size of the file behind `handle`, in bytes.
bool deluge_efatfs_file_size(uint32_t handle, uint32_t* out_size);

/// @brief Truncate the file behind `handle` to `new_len` bytes. The handle's
///        cursor position is left unchanged (matches POSIX `ftruncate`).
bool deluge_efatfs_file_truncate(uint32_t handle, uint32_t new_len);

/// @brief Close a task-context file handle opened via `deluge_efatfs_file_open`.
void deluge_efatfs_file_close(uint32_t handle);

/// @brief Open `path` as a directory for iteration.
bool deluge_efatfs_dir_open(const char* path, uint32_t* out_handle);

/// @brief Read the next directory entry's fields into the caller's out-params
///        (mirrors `DelugeDirEntry`'s fields). If there are no more entries,
///        `*out_has_entry` is set to `false` and the call still returns `true`
///        (end of directory is not an error, matching `deluge_dir_read`). An
///        entry whose name doesn't fit in `DELUGE_MAX_FILENAME` bytes
///        (including the NUL) is skipped internally -- never truncated into
///        `out_name` -- so this never fails the whole enumeration over one
///        oversized filename.
/// @param out_name NUL-terminated on success; must point at `DELUGE_MAX_FILENAME`
///                  writable bytes.
/// @param out_modified Packed FAT date/time: `(dos_date << 16) | dos_time`,
///                       the same convention `deluge_efatfs_set_time` packs.
/// @param out_attrs Raw FAT attribute byte (`RDO`=0x01, `HID`=0x02, `SYS`=0x04,
///                    `DIR`=0x10, `ARC`=0x20).
bool deluge_efatfs_dir_read(uint32_t handle, char* out_name, uint32_t out_name_cap, bool* out_is_dir,
                            uint32_t* out_size, uint32_t* out_modified, uint8_t* out_attrs, bool* out_has_entry);

/// @brief Close a directory handle opened via `deluge_efatfs_dir_open`.
void deluge_efatfs_dir_close(uint32_t handle);

/// @brief Create a directory.
bool deluge_efatfs_mkdir(const char* path);

/// @brief Delete a file or empty directory.
bool deluge_efatfs_unlink(const char* path);

/// @brief Rename/move a file or directory.
bool deluge_efatfs_rename(const char* old_path, const char* new_path);

/// @brief Set a file or directory's last-modified timestamp.
bool deluge_efatfs_set_time(const char* path, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                            uint8_t second);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_FILE_IO_H
