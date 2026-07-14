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

/// Converts a raw `FILINFO` (as returned by `FatFS::Directory::read`) into a
/// `DelugeDirEntry`. FatFS signals end-of-directory by returning a
/// zero-length `fname` with no error (not by failing the call) — this
/// function reproduces that as `out_has_entry = false`.
void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry);

} // namespace deluge::fatfs_adapter
