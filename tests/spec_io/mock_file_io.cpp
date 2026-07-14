// In-memory fake backing include/libdeluge/file_io.h for host-only deluge::io
// tests. Not a real filesystem -- no FatFS, no disk I/O. Mirrors the
// mock_diskio.cpp/mock_display.cpp pattern already used in
// tests/32bit_unit_tests/mocks/, one layer higher (file_io.h instead of diskio.h).
#include "mock_file_io.h"

extern "C" {
#include "libdeluge/file_io.h"
}

#include <cstring>
#include <map>
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

} // namespace

void mock_file_io_reset() {
	g_entries.clear();
}

struct DelugeFile {
	std::string path;
	uint32_t position = 0;
};

struct DelugeDir {
	struct ChildInfo {
		std::string name;
		MockEntry entry; // snapshotted at open time
	};
	std::vector<ChildInfo> children;
	size_t index = 0;
};

extern "C" {

DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out) {
	std::string p(path);
	if (mode == DELUGE_FILE_READ) {
		auto it = g_entries.find(p);
		if (it == g_entries.end() || it->second.is_directory) {
			return DELUGE_ERR_NOT_FOUND;
		}
		*out = new DelugeFile{p, 0};
		return DELUGE_OK;
	}
	// DELUGE_FILE_WRITE_CREATE: create, truncating if it exists.
	g_entries[p] = MockEntry{};
	*out = new DelugeFile{p, 0};
	return DELUGE_OK;
}

DelugeStatus deluge_file_read(DelugeFile* file, void* dst, uint32_t count, uint32_t* out_read) {
	auto& entry = g_entries.at(file->path);
	uint32_t available =
	    entry.data.size() > file->position ? static_cast<uint32_t>(entry.data.size()) - file->position : 0;
	uint32_t n = count < available ? count : available;
	std::memcpy(dst, entry.data.data() + file->position, n);
	file->position += n;
	*out_read = n;
	return DELUGE_OK;
}

DelugeStatus deluge_file_write(DelugeFile* file, const void* src, uint32_t count, uint32_t* out_written) {
	auto& entry = g_entries.at(file->path);
	if (file->position + count > entry.data.size()) {
		entry.data.resize(file->position + count);
	}
	std::memcpy(entry.data.data() + file->position, src, count);
	file->position += count;
	*out_written = count;
	return DELUGE_OK;
}

DelugeStatus deluge_file_seek(DelugeFile* file, uint32_t offset) {
	file->position = offset;
	return DELUGE_OK;
}

DelugeStatus deluge_file_size(DelugeFile* file, uint32_t* out_size) {
	*out_size = static_cast<uint32_t>(g_entries.at(file->path).data.size());
	return DELUGE_OK;
}

DelugeStatus deluge_file_close(DelugeFile* file) {
	delete file;
	return DELUGE_OK;
}

DelugeStatus deluge_dir_open(const char* path, DelugeDir** out) {
	std::string prefix(path);
	if (!prefix.empty() && prefix.back() != '/') {
		prefix += '/';
	}
	auto* dir = new DelugeDir{};
	for (auto& [p, entry] : g_entries) {
		if (p.size() > prefix.size() && p.compare(0, prefix.size(), prefix) == 0) {
			std::string rest = p.substr(prefix.size());
			if (rest.find('/') == std::string::npos) {
				dir->children.push_back({rest, entry});
			}
		}
	}
	*out = dir;
	return DELUGE_OK;
}

DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry) {
	if (dir->index >= dir->children.size()) {
		*out_has_entry = false;
		return DELUGE_OK;
	}
	const auto& child = dir->children[dir->index++];
	std::strncpy(out->name, child.name.c_str(), DELUGE_MAX_FILENAME - 1);
	out->name[DELUGE_MAX_FILENAME - 1] = 0;
	out->is_directory = child.entry.is_directory;
	out->size = static_cast<uint32_t>(child.entry.data.size());
	out->modified_time = child.entry.modified_time;
	out->is_read_only = child.entry.is_read_only;
	out->is_hidden = child.entry.is_hidden;
	out->is_system = child.entry.is_system;
	out->is_archive = child.entry.is_archive;
	*out_has_entry = true;
	return DELUGE_OK;
}

DelugeStatus deluge_dir_close(DelugeDir* dir) {
	delete dir;
	return DELUGE_OK;
}

DelugeStatus deluge_file_mkdir(const char* path) {
	std::string p(path);
	if (g_entries.contains(p)) {
		return DELUGE_ERR_EXISTS;
	}
	g_entries[p] = MockEntry{true, {}};
	return DELUGE_OK;
}

DelugeStatus deluge_file_unlink(const char* path) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it == g_entries.end()) {
		return DELUGE_ERR_NOT_FOUND;
	}
	g_entries.erase(it);
	return DELUGE_OK;
}

DelugeStatus deluge_file_rename(const char* old_path, const char* new_path) {
	std::string o(old_path), n(new_path);
	auto it = g_entries.find(o);
	if (it == g_entries.end()) {
		return DELUGE_ERR_NOT_FOUND;
	}
	g_entries[n] = std::move(it->second);
	g_entries.erase(it);
	return DELUGE_OK;
}

DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it == g_entries.end()) {
		return DELUGE_ERR_NOT_FOUND;
	}
	it->second.modified_time = timestamp;
	return DELUGE_OK;
}

} // extern "C"
