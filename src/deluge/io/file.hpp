#pragma once

#include "libdeluge/file_io.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace deluge::io {

/// A real enum class over DelugeStatus's C enum, matching the existing
/// FatFS::Error precedent (src/fatfs/fatfs.hpp) for wrapping a C error code
/// as a type-safe C++ one.
enum class Status {
	OK,
	ERR,
	PARAM,
	BUSY,
	TIMEOUT,
	IO,
	NODEV,
	UNSUPPORTED,
	NOT_FOUND,
	EXISTS,
	NO_SPACE,
	NO_FILESYSTEM,
	WRITE_PROTECTED,
	NO_MEMORY,
};

Status to_status(DelugeStatus status);

class File {
public:
	File(File&) = delete;
	File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	File& operator=(File&) = delete;
	File& operator=(File&& other) noexcept {
		if (this != &other) {
			if (handle_) {
				deluge_file_close(handle_);
			}
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~File() {
		if (handle_) {
			deluge_file_close(handle_);
		}
	}

	[[nodiscard]] static std::expected<File, Status> open(std::string_view path, DelugeFileOpenMode mode);
	std::expected<std::span<std::byte>, Status> read(std::span<std::byte> buffer);
	std::expected<uint32_t, Status> write(std::span<const std::byte> buffer);
	std::expected<void, Status> seek(uint32_t offset);
	std::expected<uint32_t, Status> size();
	std::expected<void, Status> close();

private:
	explicit File(DelugeFile* handle) : handle_(handle) {}
	DelugeFile* handle_ = nullptr;
};

class Directory {
public:
	Directory(Directory&) = delete;
	Directory(Directory&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	Directory& operator=(Directory&) = delete;
	Directory& operator=(Directory&& other) noexcept {
		if (this != &other) {
			if (handle_) {
				deluge_dir_close(handle_);
			}
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~Directory() {
		if (handle_) {
			deluge_dir_close(handle_);
		}
	}

	[[nodiscard]] static std::expected<Directory, Status> open(std::string_view path);
	std::expected<std::optional<DelugeDirEntry>, Status> read();
	std::expected<void, Status> close();

private:
	explicit Directory(DelugeDir* handle) : handle_(handle) {}
	DelugeDir* handle_ = nullptr;
};

} // namespace deluge::io
