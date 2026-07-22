#include "stream_io_internal.hpp"

#include "file_io_internal.hpp" // reuses deluge::fatfs_adapter::to_deluge_status(FatFS::Error)

#include <new>
#include <span>

extern "C" {

DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out) {
	using namespace deluge::fatfs_adapter;

	if (mode == DELUGE_STREAM_READ) {
		auto opened = FatFS::File::open(path, FA_READ);
		if (!opened) {
			return to_deluge_status(opened.error());
		}
		auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
		if (impl == nullptr) {
			return DELUGE_ERR_NO_MEMORY;
		}
		*out = reinterpret_cast<DelugeStream*>(impl);
		return DELUGE_OK;
	}

	// DELUGE_STREAM_WRITE_CREATE / _CREATE_NEW / _APPEND
	FatFS::FileAccessMode fatfsMode = FA_WRITE;
	if (mode == DELUGE_STREAM_WRITE_CREATE) {
		fatfsMode |= FA_CREATE_ALWAYS;
	}
	else if (mode == DELUGE_STREAM_WRITE_CREATE_NEW) {
		fatfsMode |= FA_CREATE_NEW;
	}
	// DELUGE_STREAM_WRITE_APPEND: FA_WRITE alone -- opens an existing file at its
	// current size, no truncation. Used by SampleRecorder::finalizeRecordedFile to
	// reopen a file whose already-recorded audio (up to the eventual trim point) must
	// survive the reopen -- DELUGE_STREAM_WRITE_CREATE's truncate-on-open would destroy
	// it before truncateFileDownToSize ever runs.
	auto opened = FatFS::File::open(path, fatfsMode);
	if (!opened) {
		return to_deluge_status(opened.error());
	}
	auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
	if (impl == nullptr) {
		return DELUGE_ERR_NO_MEMORY;
	}
	impl->cluster_size_bytes = impl->file.inner().obj.fs->csize * 512u;
	// WRITE_APPEND opens an existing file -- its real current size; WRITE_CREATE*
	// always starts a fresh, empty file.
	impl->file_size = (mode == DELUGE_STREAM_WRITE_APPEND) ? static_cast<uint32_t>(impl->file.inner().obj.objsize) : 0;
	impl->num_clusters = 0;

	if (mode == DELUGE_STREAM_WRITE_APPEND) {
		// Plain FA_WRITE (no FA_OPEN_APPEND) leaves FatFS's internal file pointer at 0, not EOF --
		// without this seek, the first write_at() would silently land at byte 0 and corrupt the
		// file's start instead of appending.
		auto seeked = impl->file.lseek(impl->file_size);
		if (!seeked) {
			DelugeStatus status = to_deluge_status(seeked.error());
			delete impl;
			return status;
		}
	}

	*out = reinterpret_cast<DelugeStream*>(impl);
	return DELUGE_OK;
}

DelugeStatus deluge_stream_write_at(DelugeStream* stream, uint32_t byte_offset, const void* src, uint32_t count,
                                    uint32_t* out_written) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_written = 0;

	if (count > impl->cluster_size_bytes) {
		// write_at writes at most one cluster per call -- see stream_io.h's contract note. Without
		// this guard, a write spanning more than one cluster would leave last_written_cluster_index
		// (computed from byte_offset below) desynced from FatFS's live current cluster (fil.clust),
		// which sector_of() reads afterward -- producing a silently wrong sector, not a clean error.
		return DELUGE_ERR_PARAM;
	}

	if (byte_offset != impl->file_size) {
		return DELUGE_ERR_PARAM; // sequential append only -- see stream_io.h's contract note
	}

	// FatFS owns cluster allocation -- a normal buffered write extends the file and allocates as
	// needed. sector_of() reads the resulting cluster's address on demand from this same `file`
	// afterward, matching SampleRecorder's pre-boundary logic (clst2sect off the FIL's live .clust).
	auto written = impl->file.write(std::span{const_cast<std::byte*>(static_cast<const std::byte*>(src)),
	                                          static_cast<size_t>(count)});
	if (!written || *written != count) {
		return DELUGE_ERR_IO;
	}

	impl->file_size += count;
	impl->last_written_cluster_index = byte_offset / impl->cluster_size_bytes;
	*out_written = count;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_truncate(DelugeStream* stream, uint32_t new_size) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	auto seeked = impl->file.lseek(new_size);
	if (!seeked) {
		return deluge::fatfs_adapter::to_deluge_status(seeked.error());
	}
	auto truncated = impl->file.truncate();
	if (!truncated) {
		return deluge::fatfs_adapter::to_deluge_status(truncated.error());
	}
	impl->file_size = new_size;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_size = impl->file_size;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_close(DelugeStream* stream) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	auto result = impl->file.close();
	delete impl;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	if (impl->mode == DELUGE_STREAM_READ) {
		// No cached per-cluster layout table anymore (map (a) is deleted with the C-FatFS read
		// path) -- walk the FAT chain from the file's start cluster on demand. Cold-path only:
		// AudioFileManager's post-remount "did this file move" identity check (cluster_index 0)
		// and SampleRecorder's write-side bookkeeping never take this branch.
		FIL& fil = impl->file.inner();
		uint32_t current_cluster = fil.obj.sclust;
		for (uint32_t i = 0; i < cluster_index; i++) {
			current_cluster = deluge::fatfs_adapter::get_fat_from_fs(fil.obj.fs, current_cluster);
			if (current_cluster == 0xFFFFFFFF || current_cluster < 2) {
				return DELUGE_ERR_PARAM; // FAT chain shorter than the requested index
			}
		}
		*out_sector = static_cast<uint32_t>(deluge::fatfs_adapter::clst2sect(fil.obj.fs, current_cluster));
		return DELUGE_OK;
	}
	// Write modes: only the cluster write_at() most recently completed -- no write-side
	// layout table is kept (see stream_io.h's doc comment).
	if (cluster_index != impl->last_written_cluster_index) {
		return DELUGE_ERR_PARAM;
	}
	FIL& fil = impl->file.inner();
	*out_sector = static_cast<uint32_t>(deluge::fatfs_adapter::clst2sect(fil.obj.fs, fil.clust));
	return DELUGE_OK;
}

} // extern "C"
