#pragma once

#include "libdeluge/file_io.h"

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

} // namespace deluge::io
