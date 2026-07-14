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
