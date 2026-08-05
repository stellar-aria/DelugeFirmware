/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

/// Host-sim efatfs passthrough: STRONG definitions of the `libdeluge/streaming_fill.h`
/// streaming-read symbols (`deluge_efatfs_open` / `_close` / `_read_at`) and the
/// `libdeluge/file_io.h` task-context file symbols (`deluge_efatfs_file_*`), overriding the
/// `__attribute__((weak))` no-op fallbacks in async_fill.cpp at link time.
///
/// Why this exists: `SampleStream::open_read_stream` (sample_stream.cpp) and `deluge::io::File`
/// (file.cpp, when `deluge_streaming_efatfs_active()` is true) are efatfs-only — there is no
/// C-FatFS fallback for either path. The host-sim `deluge_render`/`deluge_loadcheck` link no Rust
/// efatfs provider (that only exists on the Rust/Embassy BSP), so without this file every streamed
/// sample fails to open and every task-context file operation no-ops. This gives the host sim a
/// real backend for both over plain POSIX file I/O against the RECONSTRUCTED PROJECT DIRECTORY
/// (not the packed FAT image a real device uses — see `host_render_main.cpp`'s `DELUGE_SD_ROOT`
/// setenv).
///
/// Design: a small fixed handle table per concern (`g_slots` for streaming reads, `g_files` for
/// task-context files), no heap churn per read. The streaming `read_at` mirrors
/// `efatfs_core::fill`'s (src/bsp/rust/src/efatfs_core.rs) over-EOF behaviour exactly: a read that
/// runs past the end of the file is zero-padded rather than failed (the short-final-cluster fix)
/// and reports `*out_read == count` on success regardless of how many bytes actually came off disk.
/// `deluge_efatfs_file_read` matches that same FILL convention; `deluge_efatfs_file_read_exact` is
/// the EOF-honest counterpart `deluge::io::File::read` actually wants (see file_io.h).

#include "libdeluge/file_io.h"
#include "libdeluge/stream_io.h"
#include "libdeluge/streaming_fill.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <strings.h> // strcasecmp
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// Sample-heavy songs (e.g. the icoustic golden fixture, which layers many time-stretched/perc
// samples) can hold several hundred streams open concurrently — well above what a naive small
// table survives. 4096 is comfortably above any real project's concurrent-open-stream count and
// far under the host's fd ulimit.
constexpr uint32_t kMaxHandles = 4096;

struct HandleSlot {
	int fd = -1;
};

std::array<HandleSlot, kMaxHandles> g_slots{};

// Task-context files/dirs/streams are few-at-a-time (parse, save, recorder); 64 slots is well above
// any concurrent use and keeps the tables small. (Streaming samples use the larger g_slots table.)
constexpr uint32_t kMaxAuxHandles = 64;

struct FileSlot {
	int fd = -1;
	uint32_t pos = 0;
};
std::array<FileSlot, kMaxAuxHandles> g_files{};

FileSlot* file_slot(uint32_t handle) {
	if (handle == 0 || handle > kMaxAuxHandles) {
		return nullptr;
	}
	FileSlot& s = g_files[handle - 1];
	return s.fd >= 0 ? &s : nullptr;
}

// Persistent stream-write handles (deluge_efatfs_stream_*): unlike g_files, the size tracked here
// is an IN-MEMORY extent advanced by write_at, distinct from the on-disk size until flush/close.
struct StreamSlot {
	int fd = -1;
	uint32_t size = 0; // in-memory extent (advanced by write_at, persisted-equivalent under POSIX)
};
std::array<StreamSlot, kMaxAuxHandles> g_streams{};

StreamSlot* stream_slot(uint32_t handle) {
	if (handle == 0 || handle > kMaxAuxHandles) {
		return nullptr;
	}
	StreamSlot& s = g_streams[handle - 1];
	return s.fd >= 0 ? &s : nullptr;
}

// mkdir -p the parent directory chain of a resolved absolute path (matches efatfs's create-parents).
void mkdir_parents(const std::string& full) {
	size_t last = full.find_last_of('/');
	if (last == std::string::npos || last == 0) {
		return;
	}
	for (size_t i = 1; i < last; i++) {
		if (full[i] == '/') {
			std::string prefix = full.substr(0, i);
			mkdir(prefix.c_str(), 0777); // ignore result; EEXIST is fine
		}
	}
	mkdir(full.substr(0, last).c_str(), 0777);
}

// Case-insensitive retry on just the final path component (see file comment / streaming_fill.h):
// Deluge paths are conventionally uppercase and the reconstructed project trees match, so an
// exact match is expected to work; this is a fallback, not the primary lookup.
std::string case_insensitive_retry(const std::string& full_path) {
	size_t slash = full_path.find_last_of('/');
	std::string dir = (slash == std::string::npos) ? "." : full_path.substr(0, slash);
	std::string base = (slash == std::string::npos) ? full_path : full_path.substr(slash + 1);

	DIR* d = opendir(dir.c_str());
	if (d == nullptr) {
		return {};
	}
	std::string found;
	while (dirent* entry = readdir(d)) {
		if (strcasecmp(entry->d_name, base.c_str()) == 0) {
			found = dir + "/" + entry->d_name;
			break;
		}
	}
	closedir(d);
	return found;
}

// Resolve a Deluge-relative path (e.g. "SAMPLES/FOO.WAV") against DELUGE_SD_ROOT.
std::string resolve_root_relative(const char* path) {
	const char* root = getenv("DELUGE_SD_ROOT");
	if (root == nullptr || root[0] == '\0' || path == nullptr) {
		return {};
	}
	std::string rel{path};
	while (!rel.empty() && rel.front() == '/') {
		rel.erase(rel.begin());
	}
	return std::string(root) + "/" + rel;
}

struct DirSlot {
	DIR* dir = nullptr;
	std::string path; // resolved dir path, for stat-ing each entry
};
std::array<DirSlot, kMaxAuxHandles> g_dirs{};

DirSlot* dir_slot(uint32_t handle) {
	if (handle == 0 || handle > kMaxAuxHandles) {
		return nullptr;
	}
	DirSlot& s = g_dirs[handle - 1];
	return s.dir != nullptr ? &s : nullptr;
}

// Pack a broken-down local time into FAT (dos_date << 16) | dos_time, matching efatfs pack_fat_datetime.
// Note the 2-second resolution (sec / 2).
uint32_t pack_fat_datetime(const struct tm& t) {
	uint32_t year = static_cast<uint32_t>(t.tm_year + 1900);
	uint32_t dos_date = (year >= 1980 ? ((year - 1980) << 9) : 0) | (static_cast<uint32_t>(t.tm_mon + 1) << 5)
	                    | static_cast<uint32_t>(t.tm_mday);
	uint32_t dos_time = (static_cast<uint32_t>(t.tm_hour) << 11) | (static_cast<uint32_t>(t.tm_min) << 5)
	                    | (static_cast<uint32_t>(t.tm_sec) / 2);
	return (dos_date << 16) | dos_time;
}

} // namespace

extern "C" {

bool deluge_efatfs_open(const char* path, uint32_t* out_handle, bool* out_table_full) {
	if (path == nullptr || out_handle == nullptr || out_table_full == nullptr) {
		return false;
	}
	*out_table_full = false;
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false; // DELUGE_SD_ROOT unset — no passthrough root configured.
	}

	int fd = open(full.c_str(), O_RDONLY);
	if (fd < 0) {
		// Exact-match miss: retry the final path component case-insensitively rather than
		// failing the load outright (see file comment).
		std::string retry = case_insensitive_retry(full);
		if (!retry.empty()) {
			fd = open(retry.c_str(), O_RDONLY);
		}
	}
	if (fd < 0) {
		fprintf(stderr, "[host-efatfs] cannot open '%s' (root-relative '%s'): %s\n", path, full.c_str(),
		        strerror(errno));
		return false;
	}

	for (uint32_t i = 0; i < kMaxHandles; i++) {
		if (g_slots[i].fd < 0) {
			g_slots[i].fd = fd;
			*out_handle = i + 1; // handle 0 is the "unset" sentinel (sample_stream.cpp)
			return true;
		}
	}
	// 4096 slots (see kMaxHandles's comment) is far above any real project's concurrent-open-stream
	// count, so this is not expected to fire in practice -- but report it distinguishably rather
	// than silently colliding with a real "file not found" the way a single bool return would.
	fprintf(stderr, "[host-efatfs] handle table full (%u slots)\n", kMaxHandles);
	*out_table_full = true;
	close(fd);
	return false;
}

void deluge_efatfs_close(uint32_t handle) {
	if (handle == 0 || handle > kMaxHandles) {
		return;
	}
	HandleSlot& slot = g_slots[handle - 1];
	if (slot.fd >= 0) {
		close(slot.fd);
		slot.fd = -1;
	}
}

bool deluge_efatfs_read_at(uint32_t handle, uint32_t byte_offset, void* dst, uint32_t count, uint32_t* out_read) {
	if (dst == nullptr || out_read == nullptr || handle == 0 || handle > kMaxHandles) {
		return false;
	}
	int fd = g_slots[handle - 1].fd;
	if (fd < 0) {
		return false;
	}

	auto* buf = static_cast<uint8_t*>(dst);
	uint32_t filled = 0;
	while (filled < count) {
		ssize_t n = pread(fd, buf + filled, count - filled, static_cast<off_t>(byte_offset) + filled);
		if (n == 0) {
			// EOF before dst is full: zero-pad the rest and report success, mirroring
			// efatfs_core::fill's over-EOF behaviour exactly, including reporting *out_read == count
			// (the full requested length) on success.
			std::memset(buf + filled, 0, count - filled);
			*out_read = count;
			return true;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		filled += static_cast<uint32_t>(n);
	}
	*out_read = count;
	return true;
}

bool deluge_efatfs_file_open(const char* path, uint8_t mode, uint32_t* out_handle) {
	if (path == nullptr || out_handle == nullptr) {
		return false;
	}
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false;
	}
	int flags = 0;
	switch (mode) {
	case DELUGE_FILE_READ:
		flags = O_RDONLY;
		break;
	case DELUGE_FILE_WRITE_CREATE:
		flags = O_RDWR | O_CREAT | O_TRUNC;
		break;
	case DELUGE_FILE_WRITE_CREATE_NEW:
		flags = O_RDWR | O_CREAT | O_EXCL;
		break;
	default:
		return false;
	}
	if (mode != DELUGE_FILE_READ) {
		mkdir_parents(full); // WRITE modes auto-create missing parent dirs (efatfs create_context)
	}
	int fd = open(full.c_str(), flags, 0666);
	if (fd < 0 && mode == DELUGE_FILE_READ) {
		std::string retry = case_insensitive_retry(full);
		if (!retry.empty()) {
			fd = open(retry.c_str(), O_RDONLY);
		}
	}
	if (fd < 0) {
		return false;
	}
	for (uint32_t i = 0; i < kMaxAuxHandles; i++) {
		if (g_files[i].fd < 0) {
			g_files[i] = {fd, 0};
			*out_handle = i + 1;
			return true;
		}
	}
	close(fd);
	return false;
}

bool deluge_efatfs_file_read(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr || dst == nullptr || out_read == nullptr) {
		return false;
	}
	auto* buf = static_cast<uint8_t*>(dst);
	uint32_t filled = 0;
	while (filled < count) {
		ssize_t n = pread(s->fd, buf + filled, count - filled, static_cast<off_t>(s->pos) + filled);
		if (n == 0) {
			std::memset(buf + filled, 0, count - filled); // EOF: zero-pad the remainder
			break;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		filled += static_cast<uint32_t>(n);
	}
	s->pos += count;   // FILL semantics: advance by the full requested count
	*out_read = count; // always the full count on success
	return true;
}

bool deluge_efatfs_file_read_exact(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr || dst == nullptr || out_read == nullptr) {
		return false;
	}
	auto* buf = static_cast<uint8_t*>(dst);
	uint32_t got = 0;
	while (got < count) {
		ssize_t n = pread(s->fd, buf + got, count - got, static_cast<off_t>(s->pos) + got);
		if (n == 0) {
			break; // EOF: stop, do NOT zero-pad
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		got += static_cast<uint32_t>(n);
	}
	s->pos += got; // advance by the true bytes read
	*out_read = got;
	return true;
}

bool deluge_efatfs_file_write(uint32_t handle, const void* src, uint32_t count, uint32_t* out_written) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr || src == nullptr || out_written == nullptr) {
		return false;
	}
	const auto* buf = static_cast<const uint8_t*>(src);
	uint32_t done = 0;
	while (done < count) {
		ssize_t n = pwrite(s->fd, buf + done, count - done, static_cast<off_t>(s->pos) + done);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false; // hard I/O error (e.g. ENOSPC/EIO/EBADF): propagate as failure, matching
			              // the read paths and the device efatfs's write-error behaviour
		}
		if (n == 0) {
			break;
		}
		done += static_cast<uint32_t>(n);
	}
	s->pos += done;
	*out_written = done;
	return true;
}

bool deluge_efatfs_file_seek(uint32_t handle, uint32_t offset) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr) {
		return false;
	}
	s->pos = offset; // absolute; no FS access (matches efatfs TaskFileTable::seek)
	return true;
}

bool deluge_efatfs_file_size(uint32_t handle, uint32_t* out_size) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr || out_size == nullptr) {
		return false;
	}
	struct stat st{};
	if (fstat(s->fd, &st) != 0) {
		return false;
	}
	*out_size = static_cast<uint32_t>(st.st_size); // cursor untouched
	return true;
}

bool deluge_efatfs_file_truncate(uint32_t handle, uint32_t new_len) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr) {
		return false;
	}
	struct stat st{};
	if (fstat(s->fd, &st) != 0) {
		return false;
	}
	uint32_t clamped = std::min<uint32_t>(new_len, static_cast<uint32_t>(st.st_size)); // shrink-only
	return ftruncate(s->fd, clamped) == 0;                                             // cursor untouched
}

void deluge_efatfs_file_close(uint32_t handle) {
	FileSlot* s = file_slot(handle);
	if (s == nullptr) {
		return;
	}
	close(s->fd);
	s->fd = -1;
}

bool deluge_efatfs_dir_open(const char* path, uint32_t* out_handle) {
	if (out_handle == nullptr) {
		return false;
	}
	// Empty/null path opens the volume root (DELUGE_SD_ROOT) directly.
	std::string full;
	if (path == nullptr || path[0] == '\0') {
		const char* root = getenv("DELUGE_SD_ROOT");
		full = (root != nullptr) ? root : std::string{};
	}
	else {
		full = resolve_root_relative(path);
	}
	if (full.empty()) {
		return false;
	}
	DIR* d = opendir(full.c_str());
	if (d == nullptr) {
		return false;
	}
	for (uint32_t i = 0; i < kMaxAuxHandles; i++) {
		if (g_dirs[i].dir == nullptr) {
			g_dirs[i].dir = d;
			g_dirs[i].path = full;
			*out_handle = i + 1;
			return true;
		}
	}
	closedir(d);
	return false;
}

bool deluge_efatfs_dir_read(uint32_t handle, char* out_name, uint32_t out_name_cap, bool* out_is_dir,
                            uint32_t* out_size, uint32_t* out_modified, uint8_t* out_attrs, bool* out_has_entry) {
	DirSlot* s = dir_slot(handle);
	if (s == nullptr || out_name == nullptr || out_is_dir == nullptr || out_size == nullptr || out_modified == nullptr
	    || out_attrs == nullptr || out_has_entry == nullptr) {
		return false;
	}
	while (dirent* e = readdir(s->dir)) {
		if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
			continue; // efatfs/FAT never surface . or ..
		}
		size_t namelen = std::strlen(e->d_name);
		if (namelen >= out_name_cap) {
			continue; // skip (never truncate) a name that wouldn't fit + its NUL
		}
		std::string entry_path = s->path + "/" + e->d_name;
		struct stat st{};
		if (stat(entry_path.c_str(), &st) != 0) {
			continue; // unreadable entry: skip
		}
		bool is_dir = S_ISDIR(st.st_mode);
		std::memcpy(out_name, e->d_name, namelen);
		out_name[namelen] = '\0';
		*out_is_dir = is_dir;
		*out_size = is_dir ? 0u : static_cast<uint32_t>(st.st_size); // 0 for dirs
		struct tm tmv{};
		localtime_r(&st.st_mtime, &tmv);
		*out_modified = pack_fat_datetime(tmv);
		uint8_t attrs = is_dir ? 0x10 /*DIR*/ : 0x20 /*ARC*/;
		if ((st.st_mode & S_IWUSR) == 0) {
			attrs |= 0x01; // RDO
		}
		*out_attrs = attrs;
		*out_has_entry = true;
		return true;
	}
	*out_has_entry = false; // end of directory: success, not an error
	return true;
}

void deluge_efatfs_dir_close(uint32_t handle) {
	DirSlot* s = dir_slot(handle);
	if (s == nullptr) {
		return;
	}
	closedir(s->dir);
	s->dir = nullptr;
	s->path.clear();
}

bool deluge_efatfs_mkdir(const char* path) {
	if (path == nullptr) {
		return false;
	}
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false;
	}
	for (size_t i = 1; i <= full.size(); i++) {
		if (i == full.size() || full[i] == '/') {
			std::string prefix = full.substr(0, i);
			if (!prefix.empty() && mkdir(prefix.c_str(), 0777) != 0 && errno != EEXIST) {
				return false;
			}
		}
	}
	struct stat st{};
	return stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool deluge_efatfs_unlink(const char* path) {
	if (path == nullptr) {
		return false;
	}
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false;
	}
	if (::unlink(full.c_str()) == 0) {
		return true;
	}
	if (errno == EISDIR || errno == EPERM) {
		return ::rmdir(full.c_str()) == 0; // empty-dir removal (rmdir fails if non-empty)
	}
	return false;
}

bool deluge_efatfs_rename(const char* old_path, const char* new_path) {
	if (old_path == nullptr || new_path == nullptr) {
		return false;
	}
	std::string from = resolve_root_relative(old_path);
	std::string to = resolve_root_relative(new_path);
	if (from.empty() || to.empty()) {
		return false;
	}
	struct stat st{};
	if (stat(to.c_str(), &st) == 0) {
		return false; // efatfs rename fails if the destination exists; POSIX would overwrite
	}
	return ::rename(from.c_str(), to.c_str()) == 0;
}

bool deluge_efatfs_set_time(const char* path, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                            uint8_t second) {
	if (path == nullptr || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
		return false; // range-validate like efatfs (rejects out-of-range before touching the FS)
	}
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false;
	}
	struct stat st{};
	if (stat(full.c_str(), &st) != 0) {
		return false; // missing file: fail (efatfs open_file first), NOT a silent no-op
	}
	struct tm tmv{};
	tmv.tm_year = static_cast<int>(year) - 1900;
	tmv.tm_mon = static_cast<int>(month) - 1;
	tmv.tm_mday = day;
	tmv.tm_hour = hour;
	tmv.tm_min = minute;
	tmv.tm_sec = (second / 2) * 2; // 2-second DOS resolution (matches the packed round-trip)
	tmv.tm_isdst = -1;
	time_t mt = mktime(&tmv);
	if (mt == static_cast<time_t>(-1)) {
		return false;
	}
	struct timeval times[2] = {{mt, 0}, {mt, 0}};
	return utimes(full.c_str(), times) == 0;
}

bool deluge_efatfs_stream_open(const char* path, uint8_t mode, uint32_t* out_handle) {
	if (path == nullptr || out_handle == nullptr) {
		return false;
	}
	std::string full = resolve_root_relative(path);
	if (full.empty()) {
		return false;
	}
	int flags = 0;
	switch (mode) {
	case DELUGE_STREAM_READ:
		flags = O_RDONLY;
		break;
	case DELUGE_STREAM_WRITE_CREATE:
		flags = O_RDWR | O_CREAT | O_TRUNC;
		break;
	case DELUGE_STREAM_WRITE_CREATE_NEW:
		flags = O_RDWR | O_CREAT | O_EXCL;
		break;
	case DELUGE_STREAM_WRITE_APPEND:
		flags = O_RDWR; // open existing, no truncation (position is per-write_at, not seek-to-end)
		break;
	default:
		return false;
	}
	if (mode == DELUGE_STREAM_WRITE_CREATE || mode == DELUGE_STREAM_WRITE_CREATE_NEW) {
		mkdir_parents(full);
	}
	int fd = open(full.c_str(), flags, 0666);
	if (fd < 0) {
		return false;
	}
	uint32_t size = 0;
	if (mode == DELUGE_STREAM_READ || mode == DELUGE_STREAM_WRITE_APPEND) {
		struct stat st{};
		if (fstat(fd, &st) == 0) {
			size = static_cast<uint32_t>(st.st_size);
		}
	}
	for (uint32_t i = 0; i < kMaxAuxHandles; i++) {
		if (g_streams[i].fd < 0) {
			g_streams[i] = {fd, size};
			*out_handle = i + 1;
			return true;
		}
	}
	close(fd);
	return false;
}

bool deluge_efatfs_stream_write_at(uint32_t handle, uint32_t byte_offset, const void* src, uint32_t count,
                                   uint32_t* out_written) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr || src == nullptr || out_written == nullptr) {
		return false;
	}
	const auto* buf = static_cast<const uint8_t*>(src);
	uint32_t done = 0;
	while (done < count) {
		ssize_t n = pwrite(s->fd, buf + done, count - done, static_cast<off_t>(byte_offset) + done);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (n == 0) {
			break;
		}
		done += static_cast<uint32_t>(n);
	}
	if (byte_offset + done > s->size) {
		s->size = byte_offset + done; // extend the in-memory extent
	}
	*out_written = done;
	return true;
}

bool deluge_efatfs_stream_read_at_via(uint32_t handle, uint32_t byte_offset, void* dst, uint32_t count,
                                      uint32_t* out_read) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr || dst == nullptr || out_read == nullptr) {
		return false;
	}
	uint32_t avail = (byte_offset < s->size) ? (s->size - byte_offset) : 0; // bound by in-memory size
	uint32_t want = std::min(count, avail);
	auto* buf = static_cast<uint8_t*>(dst);
	uint32_t got = 0;
	while (got < want) {
		ssize_t n = pread(s->fd, buf + got, want - got, static_cast<off_t>(byte_offset) + got);
		if (n == 0) {
			break;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		got += static_cast<uint32_t>(n);
	}
	*out_read = got; // EOF-honest, never zero-padded
	return true;
}

bool deluge_efatfs_stream_flush(uint32_t handle) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr) {
		return false;
	}
	return fsync(s->fd) == 0; // POSIX pwrite is already durable; on-disk size already matches
}

bool deluge_efatfs_stream_truncate(uint32_t handle, uint32_t new_len) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr) {
		return false;
	}
	uint32_t clamped = std::min(new_len, s->size); // shrink-only
	if (ftruncate(s->fd, clamped) != 0) {
		return false;
	}
	s->size = clamped;
	return true;
}

bool deluge_efatfs_stream_size(uint32_t handle, uint32_t* out_size) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr || out_size == nullptr) {
		return false;
	}
	*out_size = s->size; // in-memory extent, not on-disk dir entry
	return true;
}

bool deluge_efatfs_stream_close(uint32_t handle) {
	StreamSlot* s = stream_slot(handle);
	if (s == nullptr) {
		return false;
	}
	bool ok = (fsync(s->fd) == 0); // flush first...
	close(s->fd);
	s->fd = -1;
	s->size = 0;
	return ok; // ...slot is freed either way
}

} // extern "C"
