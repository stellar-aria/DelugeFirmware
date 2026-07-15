#include "file_io_internal.hpp"

#include <cstring>

namespace deluge::fatfs_adapter {

FileAccessMode to_fatfs_mode(DelugeFileOpenMode mode) {
	switch (mode) {
	case DELUGE_FILE_READ:
		return FA_READ;
	case DELUGE_FILE_WRITE_CREATE:
		return FA_WRITE | FA_CREATE_ALWAYS;
	}
	return FA_READ;
}

DelugeStatus to_deluge_status(FatFS::Error error) {
	switch (error) {
	case FatFS::Error::NO_FILE:
	case FatFS::Error::NO_PATH:
		return DELUGE_ERR_NOT_FOUND;
	case FatFS::Error::EXIST:
		return DELUGE_ERR_EXISTS;
	case FatFS::Error::WRITE_PROTECTED:
		return DELUGE_ERR_WRITE_PROTECTED;
	case FatFS::Error::NO_FILESYSTEM:
		return DELUGE_ERR_NO_FILESYSTEM;
	case FatFS::Error::NOT_ENOUGH_CORE:
		return DELUGE_ERR_NO_MEMORY;
	case FatFS::Error::DENIED:
		return DELUGE_ERR_NO_SPACE;
	case FatFS::Error::INVALID_PARAMETER:
	case FatFS::Error::INVALID_NAME:
		return DELUGE_ERR_PARAM;
	case FatFS::Error::NOT_READY:
		return DELUGE_ERR_NODEV;
	case FatFS::Error::TIMEOUT:
		return DELUGE_ERR_TIMEOUT;
	default:
		return DELUGE_ERR_IO;
	}
}

WORD to_fat_date(DelugeTimestamp timestamp) {
	return static_cast<WORD>(((timestamp.year - 1980) << 9) | (timestamp.month << 5) | timestamp.day);
}

WORD to_fat_time(DelugeTimestamp timestamp) {
	return static_cast<WORD>((timestamp.hour << 11) | (timestamp.minute << 5) | (timestamp.second / 2));
}

DelugeTimestamp from_fat_date_time(WORD date, WORD time) {
	DelugeTimestamp timestamp{};
	timestamp.year = static_cast<uint16_t>(1980 + ((date >> 9) & 0x7F));
	timestamp.month = static_cast<uint8_t>((date >> 5) & 0x0F);
	timestamp.day = static_cast<uint8_t>(date & 0x1F);
	timestamp.hour = static_cast<uint8_t>((time >> 11) & 0x1F);
	timestamp.minute = static_cast<uint8_t>((time >> 5) & 0x3F);
	timestamp.second = static_cast<uint8_t>((time & 0x1F) * 2);
	return timestamp;
}

void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry) {
	if (info.fname[0] == 0) {
		out_has_entry = false;
		return;
	}
	out_has_entry = true;
	std::strncpy(out.name, info.fname, DELUGE_MAX_FILENAME - 1);
	out.name[DELUGE_MAX_FILENAME - 1] = 0;
	out.is_directory = (info.fattrib & AM_DIR) != 0;
	out.size = static_cast<uint32_t>(info.fsize);
	out.modified_time = from_fat_date_time(info.fdate, info.ftime);
	out.is_read_only = (info.fattrib & AM_RDO) != 0;
	out.is_hidden = (info.fattrib & AM_HID) != 0;
	out.is_system = (info.fattrib & AM_SYS) != 0;
	out.is_archive = (info.fattrib & AM_ARC) != 0;
}

DirCache g_dir_cache{};
size_t g_dir_cache_hits = 0;
size_t g_dir_cache_misses = 0;

void dir_cache_reset_for_test() {
	g_dir_cache = DirCache{};
	g_dir_cache_hits = 0;
	g_dir_cache_misses = 0;
}

void dir_cache_begin(std::string_view path, const void* handle) {
	g_dir_cache.entry_count = 0;
	g_dir_cache.active_handle = handle;
	if (path.size() >= kDirCacheMaxPathLength) {
		g_dir_cache.valid = false;
		return;
	}
	std::memcpy(g_dir_cache.directory_path, path.data(), path.size());
	g_dir_cache.directory_path[path.size()] = 0;
	g_dir_cache.directory_path_length = path.size();
	g_dir_cache.valid = true;
}

void dir_cache_append(const void* handle, std::string_view name, FATFS* fs, WORD id, DWORD sclust, FSIZE_t objsize) {
	if (!g_dir_cache.valid || g_dir_cache.active_handle != handle) {
		return;
	}
	if (g_dir_cache.entry_count >= kDirCacheCapacity) {
		return;
	}
	DirCacheEntry& entry = g_dir_cache.entries[g_dir_cache.entry_count];
	size_t copy_length = name.size() < DELUGE_MAX_FILENAME - 1 ? name.size() : DELUGE_MAX_FILENAME - 1;
	std::memcpy(entry.name, name.data(), copy_length);
	entry.name[copy_length] = 0;
	entry.fs = fs;
	entry.id = id;
	entry.sclust = sclust;
	entry.objsize = objsize;
	g_dir_cache.entry_count++;
}

const DirCacheEntry* dir_cache_lookup(std::string_view path) {
	if (!g_dir_cache.valid) {
		return nullptr;
	}
	size_t slash = path.rfind('/');
	std::string_view dirname = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
	std::string_view basename = slash == std::string_view::npos ? path : path.substr(slash + 1);
	if (dirname != std::string_view{g_dir_cache.directory_path, g_dir_cache.directory_path_length}) {
		return nullptr;
	}
	for (size_t i = 0; i < g_dir_cache.entry_count; i++) {
		if (basename == std::string_view{g_dir_cache.entries[i].name}) {
			return &g_dir_cache.entries[i];
		}
	}
	return nullptr;
}

void dir_cache_invalidate() {
	g_dir_cache.valid = false;
	g_dir_cache.entry_count = 0;
	g_dir_cache.active_handle = nullptr;
}

} // namespace deluge::fatfs_adapter

extern "C" {

DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out) {
	auto opened = FatFS::File::open(path, deluge::fatfs_adapter::to_fatfs_mode(mode));
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeFile*>(new FatFS::File(std::move(opened.value())));
	return DELUGE_OK;
}

DelugeStatus deluge_file_read(DelugeFile* file, void* dst, uint32_t count, uint32_t* out_read) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->read(std::span{static_cast<std::byte*>(dst), count});
	if (!result) {
		*out_read = 0;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	*out_read = static_cast<uint32_t>(result->size());
	return DELUGE_OK;
}

DelugeStatus deluge_file_write(DelugeFile* file, const void* src, uint32_t count, uint32_t* out_written) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	// FatFS::File::write takes a non-const span (matches f_write's signature); the
	// data is only read, never mutated, so casting away const here is safe.
	auto result = f->write(std::span{static_cast<std::byte*>(const_cast<void*>(src)), count});
	if (!result) {
		*out_written = 0;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	*out_written = *result;
	return DELUGE_OK;
}

DelugeStatus deluge_file_seek(DelugeFile* file, uint32_t offset) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->lseek(offset);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_size(DelugeFile* file, uint32_t* out_size) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	*out_size = static_cast<uint32_t>(f->size());
	return DELUGE_OK;
}

DelugeStatus deluge_file_close(DelugeFile* file) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->close();
	delete f;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp) {
	FatFS::FileInfo info{};
	info.fdate = deluge::fatfs_adapter::to_fat_date(timestamp);
	info.ftime = deluge::fatfs_adapter::to_fat_time(timestamp);
	auto result = FatFS::utime(path, &info);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_dir_open(const char* path, DelugeDir** out) {
	auto opened = FatFS::Directory::open(path);
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeDir*>(new FatFS::Directory(std::move(opened.value())));
	deluge::fatfs_adapter::dir_cache_begin(path, *out);
	return DELUGE_OK;
}

DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->read_and_get_filepointer();
	if (!result) {
		*out_has_entry = false;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	const auto& [info, file_pointer] = *result;
	deluge::fatfs_adapter::to_dir_entry(info, *out, *out_has_entry);
	if (*out_has_entry) {
		deluge::fatfs_adapter::dir_cache_append(dir, out->name, d->inner().obj.fs, d->inner().obj.id,
		                                        file_pointer.sclust, file_pointer.objsize);
	}
	return DELUGE_OK;
}

DelugeStatus deluge_dir_close(DelugeDir* dir) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->close();
	delete d;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_mkdir(const char* path) {
	auto result = FatFS::mkdir(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_unlink(const char* path) {
	auto result = FatFS::unlink(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_rename(const char* old_path, const char* new_path) {
	auto result = FatFS::rename(old_path, new_path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

} // extern "C"
