#include "stream_io_internal.hpp"

#include "file_io_internal.hpp" // reuses deluge::fatfs_adapter::to_deluge_status(FatFS::Error)
#include "libdeluge/block_device.h"

#include <new>

namespace deluge::fatfs_adapter {

DelugeStatus resolve_read_layout(StreamImpl& impl) {
	FIL& fil = impl.file.inner();
	impl.file_size = static_cast<uint32_t>(fil.obj.objsize);
	impl.cluster_size_bytes = fil.obj.fs->csize * 512u;

	if (impl.file_size == 0 || impl.cluster_size_bytes == 0) {
		impl.num_clusters = 0;
		impl.layout = nullptr;
		return DELUGE_OK;
	}

	impl.num_clusters = (impl.file_size + impl.cluster_size_bytes - 1) / impl.cluster_size_bytes;
	impl.layout = new (std::nothrow) StreamLayoutEntry[impl.num_clusters];
	if (impl.layout == nullptr) {
		impl.num_clusters = 0;
		return DELUGE_ERR_NO_MEMORY;
	}

	uint32_t current_cluster = fil.obj.sclust;
	for (uint32_t i = 0; i < impl.num_clusters; i++) {
		impl.layout[i].sector = static_cast<uint32_t>(clst2sect(fil.obj.fs, current_cluster));
		if (i + 1 >= impl.num_clusters) {
			break;
		}
		current_cluster = get_fat_from_fs(fil.obj.fs, current_cluster);
		if (current_cluster == 0xFFFFFFFF || current_cluster < 2) {
			delete[] impl.layout;
			impl.layout = nullptr;
			impl.num_clusters = 0;
			return DELUGE_ERR_IO; // FAT chain shorter than the file's recorded size -- corrupted file
		}
	}
	return DELUGE_OK;
}

} // namespace deluge::fatfs_adapter

extern "C" {

DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out) {
	using namespace deluge::fatfs_adapter;

	if (mode != DELUGE_STREAM_READ) {
		return DELUGE_ERR_UNSUPPORTED; // Task 7 implements the write modes
	}

	auto opened = FatFS::File::open(path, FA_READ);
	if (!opened) {
		return to_deluge_status(opened.error());
	}

	auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
	if (impl == nullptr) {
		return DELUGE_ERR_NO_MEMORY;
	}

	DelugeStatus status = resolve_read_layout(*impl);
	if (status != DELUGE_OK) {
		delete impl;
		return status;
	}

	*out = reinterpret_cast<DelugeStream*>(impl);
	return DELUGE_OK;
}

DelugeStatus deluge_stream_read_at(DelugeStream* stream, uint32_t byte_offset, void* dst, uint32_t count,
                                   uint32_t* out_read) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_read = 0;

	if (impl->cluster_size_bytes == 0 || byte_offset % impl->cluster_size_bytes != 0
	    || count > impl->cluster_size_bytes) {
		return DELUGE_ERR_PARAM; // read_at reads exactly one cluster at a time, from a cluster-aligned offset
	}
	// Deliberately no file_size bound here: file_size is the file's *logical* byte count, but FAT
	// allocates whole clusters, so the last cluster's on-disk allocation is always >= file_size (padded
	// up to the cluster boundary). Callers legitimately request sector-rounded counts for the last
	// cluster that exceed file_size while still being fully within its allocated physical space. The
	// cluster_index bound below (derived from file_size via num_clusters in resolve_read_layout, and
	// tight thanks to the cluster-aligned/one-cluster-max check above) is what actually guarantees the
	// read stays within a resolved, physically-allocated cluster.
	uint32_t cluster_index = byte_offset / impl->cluster_size_bytes;
	if (cluster_index >= impl->num_clusters) {
		return DELUGE_ERR_PARAM;
	}

	uint32_t num_sectors = (count + 511u) / 512u;
	DelugeStatus status = deluge_block_read(deluge_block_sd_unit(), static_cast<uint8_t*>(dst),
	                                        impl->layout[cluster_index].sector, num_sectors);
	if (status != DELUGE_OK) {
		return status;
	}
	*out_read = count;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_write_at(DelugeStream* /*stream*/, uint32_t /*byte_offset*/, const void* /*src*/,
                                    uint32_t /*count*/, uint32_t* out_written) {
	*out_written = 0;
	return DELUGE_ERR_UNSUPPORTED; // Task 7
}

DelugeStatus deluge_stream_truncate(DelugeStream* /*stream*/, uint32_t /*new_size*/) {
	return DELUGE_ERR_UNSUPPORTED; // Task 7
}

DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_size = impl->file_size;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_close(DelugeStream* stream) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	delete[] impl->layout;
	auto result = impl->file.close();
	delete impl;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	if (impl->mode != DELUGE_STREAM_READ || cluster_index >= impl->num_clusters) {
		return DELUGE_ERR_PARAM;
	}
	*out_sector = impl->layout[cluster_index].sector;
	return DELUGE_OK;
}

} // extern "C"
