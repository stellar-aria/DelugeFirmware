#pragma once

#include "fatfs.hpp"
#include "libdeluge/file_io.h"

namespace deluge::fatfs_adapter {

using FileAccessMode = FatFS::FileAccessMode;

/// Maps the boundary's open mode to FatFS's `FA_*` flag combination.
FileAccessMode to_fatfs_mode(DelugeFileOpenMode mode);

/// Maps a FatFS error to the closest `DelugeStatus`; unrecognized codes
/// (anything not explicitly listed) fall back to `DELUGE_ERR_IO`.
DelugeStatus to_deluge_status(FatFS::Error error);

/// Packs a DelugeTimestamp into a FAT DOS-format date WORD (the format
/// FILINFO::fdate/f_utime's FILINFO::fdate use — see ff.h's FILINFO comment).
WORD to_fat_date(DelugeTimestamp timestamp);

/// Packs a DelugeTimestamp into a FAT DOS-format time WORD (FILINFO::ftime).
WORD to_fat_time(DelugeTimestamp timestamp);

/// Unpacks a FAT DOS-format date/time WORD pair back into a DelugeTimestamp.
DelugeTimestamp from_fat_date_time(WORD date, WORD time);

/// Converts a raw `FILINFO` (as returned by `FatFS::Directory::read`) into a
/// `DelugeDirEntry`. FatFS signals end-of-directory by returning a
/// zero-length `fname` with no error (not by failing the call) — this
/// function reproduces that as `out_has_entry = false`.
void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry);

constexpr size_t kDirCacheCapacity = 64;                       // generous headroom over browser.cpp's 20-entry nav batch
constexpr size_t kDirCacheMaxPathLength = DELUGE_MAX_FILENAME; // reuses the boundary's filename-length bound

struct DirCacheEntry {
	char name[DELUGE_MAX_FILENAME];
	FATFS* fs;
	WORD id;
	DWORD sclust;
	FSIZE_t objsize;
};

/// Adapter-internal, single-slot, wholesale-replaced "last-scanned-directory"
/// cache. Not exposed via file_io.h -- see
/// docs/superpowers/specs/2026-07-14-file-io-migration-tier2-locator-cache-design.md.
struct DirCache {
	char directory_path[kDirCacheMaxPathLength];
	size_t directory_path_length = 0;
	DirCacheEntry entries[kDirCacheCapacity];
	size_t entry_count = 0;
	bool valid = false;
	const void* active_handle = nullptr; // the DelugeDir* currently populating this cache generation
};

/// The single adapter-wide cache instance. A real (non-static) definition so
/// tests can inspect it directly via this header.
extern DirCache g_dir_cache;

/// Diagnostic counters, incremented at deluge_file_open's two branch points
/// (Task 3). Not part of file_io.h's public surface -- inspectable by tests
/// only.
extern size_t g_dir_cache_hits;
extern size_t g_dir_cache_misses;

/// Resets g_dir_cache and the hit/miss counters to their initial empty
/// state. Test-only -- production code never needs to do this.
void dir_cache_reset_for_test();

/// Starts a fresh cache generation for `path`, tagged to `handle` (normally
/// the DelugeDir* the scan belongs to). Wholesale-replaces any prior cache.
/// If `path` doesn't fit in kDirCacheMaxPathLength, leaves the cache invalid
/// (graceful degradation -- this one scan just isn't cached).
void dir_cache_begin(std::string_view path, const void* handle);

/// Appends one entry to the cache. No-ops silently if `handle` isn't the
/// active scan, the cache isn't valid, or the cache is already at
/// kDirCacheCapacity (graceful degradation for oversized folders).
void dir_cache_append(const void* handle, std::string_view name, FATFS* fs, WORD id, DWORD sclust, FSIZE_t objsize);

/// Splits `path` into (dirname, basename) and returns the matching cached
/// entry, or nullptr if the cache is invalid, the directory doesn't match,
/// or the basename isn't among its entries.
const DirCacheEntry* dir_cache_lookup(std::string_view path);

/// Unconditionally invalidates the cache. Called from every write-shaped
/// file_io.h function.
void dir_cache_invalidate();

} // namespace deluge::fatfs_adapter
