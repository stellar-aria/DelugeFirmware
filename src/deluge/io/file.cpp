#include "io/file.hpp"

#include "libdeluge/streaming_fill.h" // deluge_streaming_efatfs_active

#include <cstdint>

namespace deluge::io {

namespace {

// R2 Task 4: the efatfs backend's `u32` handles are boxed into the opaque
// DelugeFile*/DelugeDir* pointers, mirroring R1's streaming handle -- these
// are never dereferenced, only cast back to `u32` before crossing into Rust.
DelugeFile* box_file_handle(uint32_t handle) {
	return reinterpret_cast<DelugeFile*>(static_cast<uintptr_t>(handle));
}
uint32_t unbox_file_handle(DelugeFile* handle) {
	return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
}
DelugeDir* box_dir_handle(uint32_t handle) {
	return reinterpret_cast<DelugeDir*>(static_cast<uintptr_t>(handle));
}
uint32_t unbox_dir_handle(DelugeDir* handle) {
	return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
}

// FAT attribute bits (embedded_fatfs::FileAttributes::bits(), bit-for-bit
// C-FatFS's AM_* -- see efatfs_core.rs's DirEntryInfo doc).
constexpr uint8_t kFatAttrReadOnly = 0x01;
constexpr uint8_t kFatAttrHidden = 0x02;
constexpr uint8_t kFatAttrSystem = 0x04;
constexpr uint8_t kFatAttrArchive = 0x20;

// Unpacks deluge_efatfs_dir_read's `out_modified` -- the same
// `(dos_date << 16) | dos_time` convention deluge_efatfs_set_time packs and
// efatfs_core.rs's `pack_fat_datetime`/`set_time` document, mirroring
// src/fatfs/file_io.cpp's `from_fat_date_time` for the C-FatFS backend (not
// shared with it -- that helper is private to the fatfs_adapter namespace).
DelugeTimestamp unpack_fat_datetime(uint32_t packed) {
	auto dos_date = static_cast<uint16_t>(packed >> 16);
	auto dos_time = static_cast<uint16_t>(packed & 0xFFFFU);
	DelugeTimestamp ts{};
	ts.year = static_cast<uint16_t>(1980 + ((dos_date >> 9) & 0x7F));
	ts.month = static_cast<uint8_t>((dos_date >> 5) & 0x0F);
	ts.day = static_cast<uint8_t>(dos_date & 0x1F);
	ts.hour = static_cast<uint8_t>((dos_time >> 11) & 0x1F);
	ts.minute = static_cast<uint8_t>((dos_time >> 5) & 0x3F);
	ts.second = static_cast<uint8_t>((dos_time & 0x1F) * 2);
	return ts;
}

} // namespace

Status to_status(DelugeStatus status) {
	switch (status) {
	case DELUGE_OK:
		return Status::OK;
	case DELUGE_ERR:
		return Status::ERR;
	case DELUGE_ERR_PARAM:
		return Status::PARAM;
	case DELUGE_ERR_BUSY:
		return Status::BUSY;
	case DELUGE_ERR_TIMEOUT:
		return Status::TIMEOUT;
	case DELUGE_ERR_IO:
		return Status::IO;
	case DELUGE_ERR_NODEV:
		return Status::NODEV;
	case DELUGE_ERR_UNSUPPORTED:
		return Status::UNSUPPORTED;
	case DELUGE_ERR_NOT_FOUND:
		return Status::NOT_FOUND;
	case DELUGE_ERR_EXISTS:
		return Status::EXISTS;
	case DELUGE_ERR_NO_SPACE:
		return Status::NO_SPACE;
	case DELUGE_ERR_NO_FILESYSTEM:
		return Status::NO_FILESYSTEM;
	case DELUGE_ERR_WRITE_PROTECTED:
		return Status::WRITE_PROTECTED;
	case DELUGE_ERR_NO_MEMORY:
		return Status::NO_MEMORY;
	}
	return Status::ERR; // unreachable while the switch above stays exhaustive
}

DelugeStatus to_deluge_status(Status status) {
	switch (status) {
	case Status::OK:
		return DELUGE_OK;
	case Status::ERR:
		return DELUGE_ERR;
	case Status::PARAM:
		return DELUGE_ERR_PARAM;
	case Status::BUSY:
		return DELUGE_ERR_BUSY;
	case Status::TIMEOUT:
		return DELUGE_ERR_TIMEOUT;
	case Status::IO:
		return DELUGE_ERR_IO;
	case Status::NODEV:
		return DELUGE_ERR_NODEV;
	case Status::UNSUPPORTED:
		return DELUGE_ERR_UNSUPPORTED;
	case Status::NOT_FOUND:
		return DELUGE_ERR_NOT_FOUND;
	case Status::EXISTS:
		return DELUGE_ERR_EXISTS;
	case Status::NO_SPACE:
		return DELUGE_ERR_NO_SPACE;
	case Status::NO_FILESYSTEM:
		return DELUGE_ERR_NO_FILESYSTEM;
	case Status::WRITE_PROTECTED:
		return DELUGE_ERR_WRITE_PROTECTED;
	case Status::NO_MEMORY:
		return DELUGE_ERR_NO_MEMORY;
	}
	return DELUGE_ERR; // unreachable while the switch above stays exhaustive
}

std::expected<File, Status> File::open(std::string_view path, DelugeFileOpenMode mode) {
	if (deluge_streaming_efatfs_active()) {
		uint32_t handle = 0;
		if (!deluge_efatfs_file_open(path.data(), static_cast<uint8_t>(mode), &handle)) {
			return std::unexpected(Status::ERR);
		}
		return File(box_file_handle(handle));
	}
	DelugeFile* handle = nullptr;
	DelugeStatus status = deluge_file_open(path.data(), mode, &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return File(handle);
}

std::expected<std::span<std::byte>, Status> File::read(std::span<std::byte> buffer) {
	uint32_t out_read = 0;
	if (deluge_streaming_efatfs_active()) {
		// EOF-honest read (the true short count, never zero-padded) -- the
		// efatfs backend for this port method.
		if (!deluge_efatfs_file_read_exact(unbox_file_handle(handle_), buffer.data(),
		                                   static_cast<uint32_t>(buffer.size()), &out_read)) {
			return std::unexpected(Status::ERR);
		}
		return buffer.subspan(0, out_read);
	}
	DelugeStatus status = deluge_file_read(handle_, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_read);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return buffer.subspan(0, out_read);
}

std::expected<uint32_t, Status> File::write(std::span<const std::byte> buffer) {
	uint32_t out_written = 0;
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_file_write(unbox_file_handle(handle_), buffer.data(), static_cast<uint32_t>(buffer.size()),
		                              &out_written)) {
			return std::unexpected(Status::ERR);
		}
		return out_written;
	}
	DelugeStatus status = deluge_file_write(handle_, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_written);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_written;
}

std::expected<void, Status> File::seek(uint32_t offset) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_file_seek(unbox_file_handle(handle_), offset)) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_file_seek(handle_, offset);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<uint32_t, Status> File::size() {
	uint32_t out_size = 0;
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_file_size(unbox_file_handle(handle_), &out_size)) {
			return std::unexpected(Status::ERR);
		}
		return out_size;
	}
	DelugeStatus status = deluge_file_size(handle_, &out_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_size;
}

std::expected<void, Status> File::close() {
	if (deluge_streaming_efatfs_active()) {
		deluge_efatfs_file_close(unbox_file_handle(handle_));
		handle_ = nullptr; // matters even on error: don't let the destructor double-close
		return {};
	}
	DelugeStatus status = deluge_file_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<Directory, Status> Directory::open(std::string_view path) {
	if (deluge_streaming_efatfs_active()) {
		uint32_t handle = 0;
		if (!deluge_efatfs_dir_open(path.data(), &handle)) {
			return std::unexpected(Status::ERR);
		}
		return Directory(box_dir_handle(handle));
	}
	DelugeDir* handle = nullptr;
	DelugeStatus status = deluge_dir_open(path.data(), &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return Directory(handle);
}

std::expected<std::optional<DelugeDirEntry>, Status> Directory::read() {
	if (deluge_streaming_efatfs_active()) {
		DelugeDirEntry entry{};
		entry.locator = DELUGE_EFATFS_NO_LOCATOR;
		bool has_entry = false;
		uint32_t modified = 0;
		uint8_t attrs = 0;
		if (!deluge_efatfs_dir_read(unbox_dir_handle(handle_), entry.name, DELUGE_MAX_FILENAME, &entry.is_directory,
		                            &entry.size, &modified, &attrs, &entry.locator, &has_entry)) {
			return std::unexpected(Status::ERR);
		}
		if (!has_entry) {
			return std::nullopt;
		}
		entry.modified_time = unpack_fat_datetime(modified);
		entry.is_read_only = (attrs & kFatAttrReadOnly) != 0;
		entry.is_hidden = (attrs & kFatAttrHidden) != 0;
		entry.is_system = (attrs & kFatAttrSystem) != 0;
		entry.is_archive = (attrs & kFatAttrArchive) != 0;
		return entry;
	}
	DelugeDirEntry entry{};
	entry.locator = DELUGE_EFATFS_NO_LOCATOR; // C-FatFS backend has no locator concept
	bool has_entry = false;
	DelugeStatus status = deluge_dir_read(handle_, &entry, &has_entry);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	if (!has_entry) {
		return std::nullopt;
	}
	return entry;
}

std::expected<void, Status> Directory::close() {
	if (deluge_streaming_efatfs_active()) {
		deluge_efatfs_dir_close(unbox_dir_handle(handle_));
		handle_ = nullptr; // matters even on error: don't let the destructor double-close
		return {};
	}
	DelugeStatus status = deluge_dir_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> mkdir(std::string_view path) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_mkdir(path.data())) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_file_mkdir(path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> unlink(std::string_view path) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_unlink(path.data())) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_file_unlink(path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> rename(std::string_view old_path, std::string_view new_path) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_rename(old_path.data(), new_path.data())) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_file_rename(old_path.data(), new_path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> set_time(std::string_view path, DelugeTimestamp timestamp) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_set_time(path.data(), timestamp.year, timestamp.month, timestamp.day, timestamp.hour,
		                            timestamp.minute, timestamp.second)) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_file_set_time(path.data(), timestamp);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

} // namespace deluge::io
