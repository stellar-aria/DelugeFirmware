#pragma once

#include "fatfs.hpp"
#include "libdeluge/stream_io.h"

namespace deluge::fatfs_adapter {

extern "C" {
DWORD get_fat_from_fs(FATFS* fs, DWORD clst);
LBA_t clst2sect(FATFS* fs, DWORD clst);
}

/// One resolved cluster's physical sector address.
struct StreamLayoutEntry {
	uint32_t sector = 0;
};

/// The opaque `DelugeStream` handle's real type. `layout` is a heap array of
/// `num_clusters` entries (READ mode only; nullptr in WRITE mode), resolved once
/// at open via the FAT chain walk (`resolve_read_layout`) so `read_at` never
/// re-walks the FAT.
struct StreamImpl {
	FatFS::File file;
	DelugeStreamMode mode;
	uint32_t cluster_size_bytes = 0;
	uint32_t num_clusters = 0;
	uint32_t file_size = 0;
	StreamLayoutEntry* layout = nullptr; // read mode only
	// Write modes only: the cluster index `write_at` most recently completed, or
	// UINT32_MAX if none yet. sector_of() only answers for this exact index -- see
	// stream_io.h's doc comment for why (no write-side layout table is kept).
	uint32_t last_written_cluster_index = 0xFFFFFFFFu;
};

/// Walks the FAT chain once for an already-open READ-mode `impl.file`, filling
/// `impl.layout`/`num_clusters`/`file_size`/`cluster_size_bytes`. Mirrors the
/// pre-boundary logic that used to live directly in
/// `AudioFileManager::buildAudioFileFromCard`, relocated here.
DelugeStatus resolve_read_layout(StreamImpl& impl);

} // namespace deluge::fatfs_adapter
