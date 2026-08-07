// In-memory fake backing include/libdeluge/file_io.h's deluge_efatfs_* C-ABI,
// for host-only deluge::io tests. Not a real filesystem -- no FatFS, no disk
// I/O, no real handle-table locking. Mirrors the mock_diskio.cpp/mock_display.cpp
// pattern already used in tests/32bit_unit_tests/mocks/, one layer higher
// (file_io.h instead of diskio.h).
#include "mock_file_io.h"

extern "C" {
#include "libdeluge/file_io.h"
}

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct MockEntry {
	bool is_directory = false;
	std::vector<uint8_t> data; // unused for directories
	DelugeTimestamp modified_time{};
	bool is_read_only = false;
	bool is_hidden = false;
	bool is_system = false;
	bool is_archive = false;
};

std::map<std::string, MockEntry> g_entries;

// Open file/directory handle tables, keyed by the `uint32_t` slot handed back
// to callers -- mirrors the real efatfs backend's TaskFileTable/DirHandleTable
// convention (lowest free slot starting at 0; see file.cpp's box_file_handle
// doc). deluge::io::File/Directory box these into their opaque
// DelugeFile*/DelugeDir* by adding 1, so slot 0 is a perfectly valid handle
// here -- this table doesn't need to reserve it for anything.

struct OpenFile {
	std::string path;
	uint32_t position = 0;
};

struct OpenDir {
	struct ChildInfo {
		std::string name;
		MockEntry entry; // snapshotted at open time
	};
	std::vector<ChildInfo> children;
	size_t index = 0;
};

std::vector<std::optional<OpenFile>> g_open_files;
std::vector<std::optional<OpenDir>> g_open_dirs;

template <typename T>
uint32_t allocate_slot(std::vector<std::optional<T>>& table, T value) {
	for (size_t i = 0; i < table.size(); ++i) {
		if (!table[i]) {
			table[i] = std::move(value);
			return static_cast<uint32_t>(i);
		}
	}
	table.push_back(std::move(value));
	return static_cast<uint32_t>(table.size() - 1);
}

// FAT attribute bits (mirrors file.cpp's kFatAttr* constants -- see
// efatfs_core.rs's DirEntryInfo doc).
constexpr uint8_t kFatAttrReadOnly = 0x01;
constexpr uint8_t kFatAttrHidden = 0x02;
constexpr uint8_t kFatAttrSystem = 0x04;
constexpr uint8_t kFatAttrArchive = 0x20;

// Packs a DelugeTimestamp into deluge_efatfs_dir_read's `out_modified`
// convention (`(dos_date << 16) | dos_time`) -- the inverse of file.cpp's
// unpack_fat_datetime.
uint32_t pack_fat_datetime(const DelugeTimestamp& ts) {
	auto dos_date =
	    static_cast<uint16_t>((((ts.year - 1980) & 0x7F) << 9) | ((ts.month & 0x0F) << 5) | (ts.day & 0x1F));
	auto dos_time =
	    static_cast<uint16_t>(((ts.hour & 0x1F) << 11) | ((ts.minute & 0x3F) << 5) | ((ts.second / 2) & 0x1F));
	return (static_cast<uint32_t>(dos_date) << 16) | dos_time;
}

uint8_t pack_fat_attrs(const MockEntry& entry) {
	uint8_t attrs = 0;
	if (entry.is_read_only) {
		attrs |= kFatAttrReadOnly;
	}
	if (entry.is_hidden) {
		attrs |= kFatAttrHidden;
	}
	if (entry.is_system) {
		attrs |= kFatAttrSystem;
	}
	if (entry.is_archive) {
		attrs |= kFatAttrArchive;
	}
	return attrs;
}

} // namespace

void mock_file_io_reset() {
	g_entries.clear();
	g_open_files.clear();
	g_open_dirs.clear();
}

extern "C" {

bool deluge_efatfs_file_open(const char* path, uint8_t mode, uint32_t* out_handle) {
	std::string p(path);
	if (static_cast<DelugeFileOpenMode>(mode) == DELUGE_FILE_READ) {
		auto it = g_entries.find(p);
		if (it == g_entries.end() || it->second.is_directory) {
			return false;
		}
		*out_handle = allocate_slot(g_open_files, OpenFile{p, 0});
		return true;
	}
	// DELUGE_FILE_WRITE_CREATE / DELUGE_FILE_WRITE_CREATE_NEW: create, truncating if it exists.
	g_entries[p] = MockEntry{};
	*out_handle = allocate_slot(g_open_files, OpenFile{p, 0});
	return true;
}

bool deluge_efatfs_file_read(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read) {
	auto& file = g_open_files.at(handle).value();
	auto& entry = g_entries.at(file.path);
	uint32_t available =
	    entry.data.size() > file.position ? static_cast<uint32_t>(entry.data.size()) - file.position : 0;
	uint32_t real = count < available ? count : available;
	std::memcpy(dst, entry.data.data() + file.position, real);
	if (real < count) {
		// Fill semantics: zero-pad a short tail at real EOF.
		std::memset(static_cast<uint8_t*>(dst) + real, 0, count - real);
	}
	file.position += real;
	*out_read = count;
	return true;
}

bool deluge_efatfs_file_read_exact(uint32_t handle, void* dst, uint32_t count, uint32_t* out_read) {
	auto& file = g_open_files.at(handle).value();
	auto& entry = g_entries.at(file.path);
	uint32_t available =
	    entry.data.size() > file.position ? static_cast<uint32_t>(entry.data.size()) - file.position : 0;
	uint32_t real = count < available ? count : available;
	std::memcpy(dst, entry.data.data() + file.position, real);
	file.position += real;
	*out_read = real;
	return true;
}

bool deluge_efatfs_file_write(uint32_t handle, const void* src, uint32_t count, uint32_t* out_written) {
	auto& file = g_open_files.at(handle).value();
	auto& entry = g_entries.at(file.path);
	if (file.position + count > entry.data.size()) {
		entry.data.resize(file.position + count);
	}
	std::memcpy(entry.data.data() + file.position, src, count);
	file.position += count;
	*out_written = count;
	return true;
}

bool deluge_efatfs_file_seek(uint32_t handle, uint32_t offset) {
	g_open_files.at(handle).value().position = offset;
	return true;
}

bool deluge_efatfs_file_size(uint32_t handle, uint32_t* out_size) {
	auto& file = g_open_files.at(handle).value();
	*out_size = static_cast<uint32_t>(g_entries.at(file.path).data.size());
	return true;
}

bool deluge_efatfs_file_truncate(uint32_t handle, uint32_t new_len) {
	auto& file = g_open_files.at(handle).value();
	g_entries.at(file.path).data.resize(new_len);
	return true;
}

void deluge_efatfs_file_close(uint32_t handle) {
	g_open_files.at(handle).reset();
}

bool deluge_efatfs_dir_open(const char* path, uint32_t* out_handle) {
	std::string prefix(path);
	if (!prefix.empty() && prefix.back() != '/') {
		prefix += '/';
	}
	OpenDir dir;
	for (auto& [p, entry] : g_entries) {
		if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0) {
			std::string rest = p.substr(prefix.size());
			if (rest.find('/') == std::string::npos) {
				dir.children.push_back({rest, entry});
			}
		}
	}
	*out_handle = allocate_slot(g_open_dirs, std::move(dir));
	return true;
}

bool deluge_efatfs_dir_read(uint32_t handle, char* out_name, uint32_t out_name_cap, bool* out_is_dir,
                            uint32_t* out_size, uint32_t* out_modified, uint8_t* out_attrs, bool* out_has_entry) {
	auto& dir = g_open_dirs.at(handle).value();
	if (dir.index >= dir.children.size()) {
		*out_has_entry = false;
		return true;
	}
	const auto& child = dir.children[dir.index++];
	std::strncpy(out_name, child.name.c_str(), out_name_cap - 1);
	out_name[out_name_cap - 1] = 0;
	*out_is_dir = child.entry.is_directory;
	*out_size = static_cast<uint32_t>(child.entry.data.size());
	*out_modified = pack_fat_datetime(child.entry.modified_time);
	*out_attrs = pack_fat_attrs(child.entry);
	*out_has_entry = true;
	return true;
}

void deluge_efatfs_dir_close(uint32_t handle) {
	g_open_dirs.at(handle).reset();
}

bool deluge_efatfs_mkdir(const char* path) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it != g_entries.end()) {
		// embedded-fatfs's create_dir is idempotent: calling it on an existing
		// directory succeeds (unlike C-FatFS's f_mkdir, which reports EXIST).
		// A pre-existing FILE at this path is still a hard conflict.
		return it->second.is_directory;
	}
	g_entries[p] = MockEntry{.is_directory = true};
	return true;
}

bool deluge_efatfs_unlink(const char* path) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it == g_entries.end()) {
		return false;
	}
	g_entries.erase(it);
	return true;
}

bool deluge_efatfs_rename(const char* old_path, const char* new_path) {
	std::string o(old_path), n(new_path);
	auto it = g_entries.find(o);
	if (it == g_entries.end()) {
		return false;
	}
	g_entries[n] = std::move(it->second);
	g_entries.erase(it);
	return true;
}

bool deluge_efatfs_set_time(const char* path, uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute,
                            uint8_t second) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it == g_entries.end()) {
		return false;
	}
	it->second.modified_time =
	    DelugeTimestamp{.year = year, .month = month, .day = day, .hour = hour, .minute = minute, .second = second};
	return true;
}

} // extern "C"
