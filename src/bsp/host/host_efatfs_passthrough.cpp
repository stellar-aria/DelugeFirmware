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

/// Host-sim streaming-read passthrough: STRONG definitions of the three
/// `libdeluge/streaming_fill.h` efatfs-read symbols (`deluge_efatfs_open` / `_close` / `_read_at`),
/// overriding the `__attribute__((weak))` no-op fallbacks in async_fill.cpp at link time.
///
/// Why this exists: `SampleStream::open_read_stream` (sample_stream.cpp) is efatfs-only — there is
/// no C-FatFS fallback for the streaming read. The host-sim `deluge_render`/
/// `deluge_loadcheck` link no Rust efatfs provider (that only exists on the Rust/Embassy BSP), so
/// without this file every streamed sample fails to open and the golden renders are silent. This
/// gives the host sim a real streaming-read backend over plain POSIX file I/O against the
/// RECONSTRUCTED PROJECT DIRECTORY (not the packed FAT image used for task-context I/O — see
/// `host_render_main.cpp`'s `DELUGE_SD_ROOT` setenv).
///
/// Design: a small fixed handle table (handle -> open fd), no heap churn per read. `read_at`
/// mirrors `efatfs_core::fill`'s (src/bsp/rust/src/efatfs_core.rs) over-EOF behaviour exactly: a
/// read that runs past the end of the file is zero-padded rather than failed (the short-final-cluster
/// fix) and reports `*out_read == count` on success regardless of how many bytes actually came off
/// disk.

#include "libdeluge/streaming_fill.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <strings.h> // strcasecmp
#include <sys/stat.h>
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

} // extern "C"
