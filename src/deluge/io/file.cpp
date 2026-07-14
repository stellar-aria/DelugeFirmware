#include "io/file.hpp"

namespace deluge::io {

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
	DelugeFile* handle = nullptr;
	DelugeStatus status = deluge_file_open(path.data(), mode, &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return File(handle);
}

std::expected<std::span<std::byte>, Status> File::read(std::span<std::byte> buffer) {
	uint32_t out_read = 0;
	DelugeStatus status = deluge_file_read(handle_, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_read);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return buffer.subspan(0, out_read);
}

std::expected<uint32_t, Status> File::write(std::span<const std::byte> buffer) {
	uint32_t out_written = 0;
	DelugeStatus status = deluge_file_write(handle_, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_written);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_written;
}

std::expected<void, Status> File::seek(uint32_t offset) {
	DelugeStatus status = deluge_file_seek(handle_, offset);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<uint32_t, Status> File::size() {
	uint32_t out_size = 0;
	DelugeStatus status = deluge_file_size(handle_, &out_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_size;
}

std::expected<void, Status> File::close() {
	DelugeStatus status = deluge_file_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<Directory, Status> Directory::open(std::string_view path) {
	DelugeDir* handle = nullptr;
	DelugeStatus status = deluge_dir_open(path.data(), &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return Directory(handle);
}

std::expected<std::optional<DelugeDirEntry>, Status> Directory::read() {
	DelugeDirEntry entry{};
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
	DelugeStatus status = deluge_dir_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> mkdir(std::string_view path) {
	DelugeStatus status = deluge_file_mkdir(path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> unlink(std::string_view path) {
	DelugeStatus status = deluge_file_unlink(path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<void, Status> rename(std::string_view old_path, std::string_view new_path) {
	DelugeStatus status = deluge_file_rename(old_path.data(), new_path.data());
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

} // namespace deluge::io
