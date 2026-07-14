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

void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry) {
	if (info.fname[0] == 0) {
		out_has_entry = false;
		return;
	}
	out_has_entry = true;
	std::strncpy(out.name, info.fname, DELUGE_MAX_FILENAME - 1);
	out.name[DELUGE_MAX_FILENAME - 1] = 0;
	out.is_directory = (info.fattrib & AM_DIR) != 0;
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

DelugeStatus deluge_dir_open(const char* path, DelugeDir** out) {
	auto opened = FatFS::Directory::open(path);
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeDir*>(new FatFS::Directory(std::move(opened.value())));
	return DELUGE_OK;
}

DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->read();
	if (!result) {
		*out_has_entry = false;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	deluge::fatfs_adapter::to_dir_entry(*result, *out, *out_has_entry);
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
