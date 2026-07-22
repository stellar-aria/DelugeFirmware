#pragma once

#include "fatfs.hpp"
#include "libdeluge/stream_io.h"

namespace deluge::fatfs_adapter {

extern "C" {
DWORD get_fat_from_fs(FATFS* fs, DWORD clst);
LBA_t clst2sect(FATFS* fs, DWORD clst);
}

/// The opaque `DelugeStream` handle's real type. `num_clusters`/`file_size`/`cluster_size_bytes`
/// are populated in WRITE mode only (see `deluge_stream_open`'s write branch); READ mode leaves
/// them at their defaults -- the R1 streaming read path goes through embedded-fatfs
/// (`deluge_efatfs_read_at`), not this FatFS-backed `DelugeStream`. `deluge_stream_sector_of`'s
/// READ-mode branch walks the FAT chain from the file's start cluster on demand instead of
/// consulting a cached per-cluster table.
struct StreamImpl {
	FatFS::File file;
	DelugeStreamMode mode;
	uint32_t cluster_size_bytes = 0;
	uint32_t num_clusters = 0;
	uint32_t file_size = 0;
	// Write modes only: the cluster index `write_at` most recently completed, or
	// UINT32_MAX if none yet. sector_of() only answers for this exact index -- see
	// stream_io.h's doc comment for why (no write-side layout table is kept).
	uint32_t last_written_cluster_index = 0xFFFFFFFFu;
};

} // namespace deluge::fatfs_adapter
