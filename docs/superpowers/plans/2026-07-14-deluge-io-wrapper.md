# `deluge::io` C++ Wrapper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `deluge::io` — an idiomatic C++23 wrapper (`Status` enum class, move-only RAII `File`/`Directory`, `std::expected`-returning methods) over the existing `include/libdeluge/file_io.h` C-ABI boundary, so app code migrating off raw FatFS gets the same ergonomics `FatFS::File`/`Directory` used to provide, one layer higher.

**Architecture:** `src/deluge/io/file.hpp`/`file.cpp`, namespace `deluge::io`, portable app code depending only on `file_io.h`. A new host-only test target (`tests/spec_io/`) links this wrapper against a hand-written in-memory mock of `file_io.h`'s twelve C-ABI functions (not the real FatFS-backed adapter, which stays in `deluge_spec`/`all_specs` for its own existing tests) — giving genuine open/read/write/seek/close and directory-iteration round-trip coverage without a real mounted filesystem.

**Tech Stack:** C++26 (this project's `DELUGE_CXX_STANDARD`), `std::expected`/`std::span`/`std::optional`, CppSpec (host-buildable unit tests, same framework as `tests/spec/`).

**Spec:** `docs/superpowers/specs/2026-07-14-deluge-io-wrapper-design.md`.

## Global Constraints

- **Move-only from the start.** `File` and `Directory` both delete their copy constructor and copy-assignment operator. This is deliberate, informed by two real bugs found during the `file_io.h` boundary's final review (move-unsound RAII in `FatFS::File`/`Directory`, and a latent copy-assignment double-close bug in `sample_recorder.cpp`) — copying a live file/directory handle must not compile.
- **Move-assignment operators are self-assignment-guarded** (`if (this != &other)`) — a Minor finding from that same review flagged an unguarded case; don't repeat it here.
- **`Status` is a real `enum class`**, not a type alias for `DelugeStatus` — mirrors the `FatFS::Error`/`FRESULT` precedent in `src/fatfs/fatfs.hpp`.
- **No behavior change to anything else.** Nothing outside this plan's own new files and the new `tests/spec_io/` directory changes. Nothing in the app calls `deluge::io` yet (that's the follow-on migration plan).
- **New C++ is idiomatic C++23/26**: `enum class`, `std::expected`, `std::optional`, `std::span`, no C-style casts, RAII.
- `src/deluge/`'s sources are picked up by a `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` in `src/deluge/CMakeLists.txt` — a new file under `src/deluge/io/` needs **no CMakeLists edit** to be compiled into `deluge_app` (verify this in Task 6, don't assume it silently).

---

## Task 1: `Status` enum class + `to_status()` — TDD, plus the `tests/spec_io/` scaffolding

**Files:**
- Create: `src/deluge/io/file.hpp` (declarations only for this task: `Status`, `to_status`)
- Create: `src/deluge/io/file.cpp` (started here: `to_status`'s implementation)
- Create: `tests/spec_io/CMakeLists.txt`
- Create: `tests/spec_io/status_spec.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Produces: `namespace deluge::io { enum class Status { OK, ERR, PARAM, BUSY, TIMEOUT, IO, NODEV, UNSUPPORTED, NOT_FOUND, EXISTS, NO_SPACE, NO_FILESYSTEM, WRITE_PROTECTED, NO_MEMORY }; Status to_status(DelugeStatus status); }` — consumed by every later task in this plan.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec_io/status_spec.cpp
#include "io/file.hpp"

#include "cppspec.hpp"

// clang-format off
describe status("deluge::io::Status", $ {
	it("maps DELUGE_OK to Status::OK", _ {
		expect(deluge::io::to_status(DELUGE_OK)).to_equal(deluge::io::Status::OK);
	});
	it("maps DELUGE_ERR_NOT_FOUND to Status::NOT_FOUND", _ {
		expect(deluge::io::to_status(DELUGE_ERR_NOT_FOUND)).to_equal(deluge::io::Status::NOT_FOUND);
	});
	it("maps DELUGE_ERR_EXISTS to Status::EXISTS", _ {
		expect(deluge::io::to_status(DELUGE_ERR_EXISTS)).to_equal(deluge::io::Status::EXISTS);
	});
	it("maps DELUGE_ERR_NO_MEMORY to Status::NO_MEMORY", _ {
		expect(deluge::io::to_status(DELUGE_ERR_NO_MEMORY)).to_equal(deluge::io::Status::NO_MEMORY);
	});
	it("maps DELUGE_ERR_WRITE_PROTECTED to Status::WRITE_PROTECTED", _ {
		expect(deluge::io::to_status(DELUGE_ERR_WRITE_PROTECTED)).to_equal(deluge::io::Status::WRITE_PROTECTED);
	});
});

CPPSPEC_SPEC(status)
```

- [ ] **Step 2: Create the `tests/spec_io/` CMake scaffolding so the test above can build**

```cmake
# tests/spec_io/CMakeLists.txt
#
# deluge::io (idiomatic C++ wrapper over libdeluge/file_io.h) specs. Links the
# wrapper against a hand-written in-memory mock of file_io.h's C-ABI (mock_file_io.cpp),
# NOT the real FatFS-backed adapter (that's tests/spec/'s concern, for the adapter's
# own tests) -- so these specs get real open/read/write/seek/close and
# directory-iteration round-trip coverage with no mounted filesystem required.
include(FetchContent)
FetchContent_Declare(CppSpec
  URL https://github.com/toroidal-code/cppspec/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(CppSpec)

function(create_specs_driver driver_name spec_dir)
  file(GLOB_RECURSE spec_sources RELATIVE ${spec_dir} ${spec_dir}/*_spec.cpp)
  create_test_sourcelist(specs ${driver_name}.cpp ${spec_sources})
  add_executable(${driver_name}
    ${specs}
    mock_file_io.cpp
    ../../src/deluge/io/file.cpp
  )
  target_link_libraries(${driver_name} PRIVATE c++spec)
  target_include_directories(${driver_name} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ../../include      # libdeluge/file_io.h, libdeluge/types.h
    ../../src/deluge    # io/file.hpp
  )
  set_target_properties(${driver_name} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED YES
  )
  foreach(spec IN LISTS spec_sources)
    cmake_path(GET spec STEM LAST_ONLY spec_name)
    add_test(NAME ${spec_name} COMMAND ${driver_name} ${spec_name} --verbose)
  endforeach()
endfunction()

create_specs_driver(io_specs ${CMAKE_CURRENT_LIST_DIR})
```

Note: this references `mock_file_io.cpp`, which doesn't exist until Task 2 —
create an empty placeholder for now so this task's build succeeds standalone:

```cpp
// tests/spec_io/mock_file_io.cpp
// Populated in Task 2.
```

Add the new subdirectory to `tests/CMakeLists.txt`, immediately after the existing
`add_subdirectory(spec)` line (unconditional — `deluge::io` has no -m32/allocator
dependency, so this does NOT go inside the `if (UNIX AND NOT APPLE)` block that
`spec_alloc`/`spec_resource` are nested in):

```cmake
add_subdirectory(spec)
add_subdirectory(spec_io)
add_subdirectory(unit)
```

- [ ] **Step 3: Run to verify it fails**

```bash
cmake -B build-tests -S tests -G Ninja && cmake --build build-tests --target io_specs
```

Expected: FAIL — `io/file.hpp` doesn't exist yet.

- [ ] **Step 4: Implement `Status` and `to_status`**

```cpp
// src/deluge/io/file.hpp
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
```

```cpp
// src/deluge/io/file.cpp
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

} // namespace deluge::io
```

- [ ] **Step 5: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R status --output-on-failure
```

Expected: PASS (5/5).

- [ ] **Step 6: Commit**

```bash
git add src/deluge/io/file.hpp src/deluge/io/file.cpp tests/spec_io/CMakeLists.txt tests/spec_io/status_spec.cpp tests/spec_io/mock_file_io.cpp tests/CMakeLists.txt
git commit -m "deluge::io: Status enum class + to_status, tests/spec_io scaffolding"
```

---

## Task 2: In-memory mock of `file_io.h`, self-tested

**Files:**
- Modify: `tests/spec_io/mock_file_io.cpp` (replaces Task 1's placeholder)
- Create: `tests/spec_io/mock_file_io.h`
- Create: `tests/spec_io/mock_file_io_spec.cpp`

**Interfaces:**
- Produces: `void mock_file_io_reset();` (test-only helper, not part of `file_io.h`) — used by every later task's specs to reset state between test cases. Also provides real definitions of all twelve `file_io.h` functions, and defines the concrete bodies of the opaque `DelugeFile`/`DelugeDir` structs (`file_io.h` only forward-declares them; this mock is one of the places, alongside the real `src/fatfs/file_io.cpp` adapter, that gives them a real layout — the two are never linked into the same binary, so there's no conflict).

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec_io/mock_file_io_spec.cpp
#include "mock_file_io.h"

extern "C" {
#include "libdeluge/file_io.h"
}

#include "cppspec.hpp"

#include <cstring>

// clang-format off
describe mock_file_io("the file_io.h mock backing", $ {
	it("round-trips a write then read", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &f)).to_equal(DELUGE_OK);
		const char* msg = "hello";
		uint32_t written = 0;
		expect(deluge_file_write(f, msg, 5, &written)).to_equal(DELUGE_OK);
		expect(written).to_equal((uint32_t)5);
		expect(deluge_file_close(f)).to_equal(DELUGE_OK);

		f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_OK);
		char buf[8] = {};
		uint32_t read = 0;
		expect(deluge_file_read(f, buf, 8, &read)).to_equal(DELUGE_OK);
		expect(read).to_equal((uint32_t)5);
		expect(std::string(buf, 5)).to_equal("hello");
		expect(deluge_file_close(f)).to_equal(DELUGE_OK);
	});

	it("returns DELUGE_ERR_NOT_FOUND opening a missing file for read", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		expect(deluge_file_open("missing.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
	});

	it("mkdir then mkdir again returns DELUGE_ERR_EXISTS", _ {
		mock_file_io_reset();
		expect(deluge_file_mkdir("SONGS")).to_equal(DELUGE_OK);
		expect(deluge_file_mkdir("SONGS")).to_equal(DELUGE_ERR_EXISTS);
	});

	it("unlink removes an entry", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		deluge_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		expect(deluge_file_unlink("a.txt")).to_equal(DELUGE_OK);
		f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
	});

	it("rename moves an entry to a new path", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		deluge_file_open("old.txt", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		expect(deluge_file_rename("old.txt", "new.txt")).to_equal(DELUGE_OK);
		f = nullptr;
		expect(deluge_file_open("old.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
		f = nullptr;
		expect(deluge_file_open("new.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_OK);
		deluge_file_close(f);
	});

	it("lists direct children of a directory, distinguishing files from subdirectories", _ {
		mock_file_io_reset();
		deluge_file_mkdir("SONGS");
		DelugeFile* f = nullptr;
		deluge_file_open("SONGS/a.xml", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		deluge_file_mkdir("SONGS/sub");

		DelugeDir* d = nullptr;
		expect(deluge_dir_open("SONGS", &d)).to_equal(DELUGE_OK);
		bool saw_file = false, saw_dir = false;
		DelugeDirEntry entry{};
		bool has_entry = true;
		while (has_entry) {
			expect(deluge_dir_read(d, &entry, &has_entry)).to_equal(DELUGE_OK);
			if (!has_entry) break;
			if (std::string(entry.name) == "a.xml") { saw_file = true; expect(entry.is_directory).to_equal(false); }
			if (std::string(entry.name) == "sub") { saw_dir = true; expect(entry.is_directory).to_equal(true); }
		}
		expect(saw_file).to_equal(true);
		expect(saw_dir).to_equal(true);
		expect(deluge_dir_close(d)).to_equal(DELUGE_OK);
	});
});

CPPSPEC_SPEC(mock_file_io)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build-tests --target io_specs
```

Expected: FAIL — `mock_file_io.h` doesn't exist yet, and Task 1's placeholder `mock_file_io.cpp` doesn't define any of the `file_io.h` functions (link errors for `deluge_file_open` etc.).

- [ ] **Step 3: Implement the mock**

```cpp
// tests/spec_io/mock_file_io.h
#pragma once

/// Test-only: clears all mock state. Call at the start of every spec that
/// touches the mock, so cases don't see each other's files/directories.
void mock_file_io_reset();
```

```cpp
// tests/spec_io/mock_file_io.cpp
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
	std::vector<std::pair<std::string, bool>> children; // (basename, is_directory), snapshotted at open time
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
	uint32_t available = entry.data.size() > file->position ? static_cast<uint32_t>(entry.data.size()) - file->position : 0;
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
				dir->children.emplace_back(rest, entry.is_directory);
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
	const auto& [name, is_dir] = dir->children[dir->index++];
	std::strncpy(out->name, name.c_str(), DELUGE_MAX_FILENAME - 1);
	out->name[DELUGE_MAX_FILENAME - 1] = 0;
	out->is_directory = is_dir;
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

} // extern "C"
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R mock_file_io --output-on-failure
```

Expected: PASS (6/6).

- [ ] **Step 5: Commit**

```bash
git add tests/spec_io/mock_file_io.h tests/spec_io/mock_file_io.cpp tests/spec_io/mock_file_io_spec.cpp
git commit -m "deluge::io: in-memory mock of file_io.h, self-tested"
```

---

## Task 3: `File` — move-only RAII, `std::expected` methods

**Files:**
- Modify: `src/deluge/io/file.hpp` (add the `File` class declaration)
- Modify: `src/deluge/io/file.cpp` (add its implementation)
- Create: `tests/spec_io/file_spec.cpp`

**Interfaces:**
- Consumes: `Status`, `to_status` (Task 1); the mock (Task 2, via `mock_file_io_reset()`).
- Produces: `deluge::io::File` — `open`, `read`, `write`, `seek`, `size`, `close`, move ctor/assignment. Consumed by Task 4 only insofar as `Directory` follows the identical shape (no direct dependency).

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec_io/file_spec.cpp
#include "io/file.hpp"
#include "mock_file_io.h"

#include "cppspec.hpp"

#include <cstring>

// clang-format off
describe file("deluge::io::File", $ {
	it("writes then reads back the same bytes", _ {
		mock_file_io_reset();
		auto written = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		expect(written).to_have_value();
		const char msg[] = "hello";
		auto w = written->write(std::as_bytes(std::span{msg, 5}));
		expect(w).to_have_value();
		expect(*w).to_equal((uint32_t)5);
		expect(written->close()).to_have_value();

		auto opened = deluge::io::File::open("a.txt", DELUGE_FILE_READ);
		expect(opened).to_have_value();
		char buf[8] = {};
		auto r = opened->read(std::as_writable_bytes(std::span{buf, 8}));
		expect(r).to_have_value();
		expect(r->size()).to_equal((size_t)5);
		expect(std::string(buf, 5)).to_equal("hello");
	});

	it("returns Status::NOT_FOUND opening a missing file", _ {
		mock_file_io_reset();
		auto opened = deluge::io::File::open("missing.txt", DELUGE_FILE_READ);
		expect(opened).to_not_have_value();
		expect(opened.error()).to_equal(deluge::io::Status::NOT_FOUND);
	});

	it("reports size after writing", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		const char msg[] = "hi";
		f->write(std::as_bytes(std::span{msg, 2}));
		auto sz = f->size();
		expect(sz).to_have_value();
		expect(*sz).to_equal((uint32_t)2);
	});

	it("seek moves the read/write position", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		const char msg[] = "hello";
		f->write(std::as_bytes(std::span{msg, 5}));
		expect(f->seek(0)).to_have_value();
		char buf[5] = {};
		auto r = f->read(std::as_writable_bytes(std::span{buf, 5}));
		expect(std::string(buf, 5)).to_equal("hello");
	});

	it("is move-only: moving leaves the source unable to double-close", _ {
		mock_file_io_reset();
		auto opened = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		deluge::io::File moved = std::move(*opened);
		// The moved-from File's destructor must not touch the handle the new
		// owner (moved) still holds -- if it did, moved's own later close()
		// would fail because the mock already freed/invalidated the handle.
		auto closed = moved.close();
		expect(closed).to_have_value();
	});
});

CPPSPEC_SPEC(file)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build-tests --target io_specs
```

Expected: FAIL — `deluge::io::File` doesn't exist yet.

- [ ] **Step 3: Implement `File`**

Add to `src/deluge/io/file.hpp` (after the `to_status` declaration):

```cpp
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace deluge::io {

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

} // namespace deluge::io
```

Add to `src/deluge/io/file.cpp` (after `to_status`'s implementation):

```cpp
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
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R "^file$" --output-on-failure
```

Expected: PASS (5/5).

- [ ] **Step 5: Commit**

```bash
git add src/deluge/io/file.hpp src/deluge/io/file.cpp tests/spec_io/file_spec.cpp
git commit -m "deluge::io: File, move-only RAII over file_io.h"
```

---

## Task 4: `Directory` — move-only RAII, `std::optional`-terminated iteration

**Files:**
- Modify: `src/deluge/io/file.hpp` (add the `Directory` class declaration)
- Modify: `src/deluge/io/file.cpp` (add its implementation)
- Create: `tests/spec_io/directory_spec.cpp`

**Interfaces:**
- Consumes: `Status`, `to_status` (Task 1); the mock (Task 2).
- Produces: `deluge::io::Directory` — `open`, `read` (returns `std::expected<std::optional<DelugeDirEntry>, Status>`), `close`, move ctor/assignment.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec_io/directory_spec.cpp
#include "io/file.hpp"
#include "mock_file_io.h"

#include "cppspec.hpp"

// clang-format off
describe directory("deluge::io::Directory", $ {
	it("iterates direct children, ending with nullopt", _ {
		mock_file_io_reset();
		deluge_file_mkdir("SONGS");
		auto f = deluge::io::File::open("SONGS/a.xml", DELUGE_FILE_WRITE_CREATE);
		f->close();
		deluge_file_mkdir("SONGS/sub");

		auto dir = deluge::io::Directory::open("SONGS");
		expect(dir).to_have_value();

		int count = 0;
		bool saw_file = false, saw_dir = false;
		while (true) {
			auto entry = dir->read();
			expect(entry).to_have_value();
			if (!entry->has_value()) break;
			count++;
			if (std::string((*entry)->name) == "a.xml") { saw_file = true; expect((*entry)->is_directory).to_equal(false); }
			if (std::string((*entry)->name) == "sub") { saw_dir = true; expect((*entry)->is_directory).to_equal(true); }
		}
		expect(count).to_equal(2);
		expect(saw_file).to_equal(true);
		expect(saw_dir).to_equal(true);
	});

	it("an empty directory reads nullopt immediately", _ {
		mock_file_io_reset();
		deluge_file_mkdir("EMPTY");
		auto dir = deluge::io::Directory::open("EMPTY");
		expect(dir).to_have_value();
		auto entry = dir->read();
		expect(entry).to_have_value();
		expect(entry->has_value()).to_equal(false);
	});

	it("is move-only: moving leaves the source unable to double-close", _ {
		mock_file_io_reset();
		deluge_file_mkdir("SONGS");
		auto opened = deluge::io::Directory::open("SONGS");
		deluge::io::Directory moved = std::move(*opened);
		auto closed = moved.close();
		expect(closed).to_have_value();
	});
});

CPPSPEC_SPEC(directory)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build-tests --target io_specs
```

Expected: FAIL — `deluge::io::Directory` doesn't exist yet.

- [ ] **Step 3: Implement `Directory`**

Add to `src/deluge/io/file.hpp` (after `File`'s declaration, before the closing `namespace deluge::io {` brace):

```cpp
#include <optional>

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
```

Add to `src/deluge/io/file.cpp` (after `File`'s implementation):

```cpp
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
	handle_ = nullptr;
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R directory --output-on-failure
```

Expected: PASS (3/3).

- [ ] **Step 5: Commit**

```bash
git add src/deluge/io/file.hpp src/deluge/io/file.cpp tests/spec_io/directory_spec.cpp
git commit -m "deluge::io: Directory, move-only RAII, optional-terminated iteration"
```

---

## Task 5: Free functions — `mkdir`, `unlink`, `rename`

**Files:**
- Modify: `src/deluge/io/file.hpp` (add the three free-function declarations)
- Modify: `src/deluge/io/file.cpp` (add their implementation)
- Create: `tests/spec_io/free_functions_spec.cpp`

**Interfaces:**
- Consumes: `Status`, `to_status` (Task 1); the mock (Task 2).
- Produces: `deluge::io::mkdir(std::string_view) -> std::expected<void, Status>`, `deluge::io::unlink(std::string_view) -> std::expected<void, Status>`, `deluge::io::rename(std::string_view, std::string_view) -> std::expected<void, Status>`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec_io/free_functions_spec.cpp
#include "io/file.hpp"
#include "mock_file_io.h"

#include "cppspec.hpp"

// clang-format off
describe free_functions("deluge::io free functions", $ {
	it("mkdir succeeds, then reports EXISTS on a repeat", _ {
		mock_file_io_reset();
		expect(deluge::io::mkdir("SONGS")).to_have_value();
		auto again = deluge::io::mkdir("SONGS");
		expect(again).to_not_have_value();
		expect(again.error()).to_equal(deluge::io::Status::EXISTS);
	});

	it("unlink removes a file", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		f->close();
		expect(deluge::io::unlink("a.txt")).to_have_value();
		auto opened = deluge::io::File::open("a.txt", DELUGE_FILE_READ);
		expect(opened).to_not_have_value();
	});

	it("rename moves a file to a new path", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("old.txt", DELUGE_FILE_WRITE_CREATE);
		f->close();
		expect(deluge::io::rename("old.txt", "new.txt")).to_have_value();
		auto old_opened = deluge::io::File::open("old.txt", DELUGE_FILE_READ);
		expect(old_opened).to_not_have_value();
		auto new_opened = deluge::io::File::open("new.txt", DELUGE_FILE_READ);
		expect(new_opened).to_have_value();
	});

	it("unlink on a missing path reports NOT_FOUND", _ {
		mock_file_io_reset();
		auto result = deluge::io::unlink("missing.txt");
		expect(result).to_not_have_value();
		expect(result.error()).to_equal(deluge::io::Status::NOT_FOUND);
	});
});

CPPSPEC_SPEC(free_functions)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build-tests --target io_specs
```

Expected: FAIL — `deluge::io::mkdir`/`unlink`/`rename` don't exist yet.

- [ ] **Step 3: Implement the free functions**

Add to `src/deluge/io/file.hpp` (after `Directory`'s declaration, still inside `namespace deluge::io`):

```cpp
std::expected<void, Status> mkdir(std::string_view path);
std::expected<void, Status> unlink(std::string_view path);
std::expected<void, Status> rename(std::string_view old_path, std::string_view new_path);
```

Add to `src/deluge/io/file.cpp` (after `Directory`'s implementation, still inside `namespace deluge::io`):

```cpp
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
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R free_functions --output-on-failure
```

Expected: PASS (4/4).

- [ ] **Step 5: Commit**

```bash
git add src/deluge/io/file.hpp src/deluge/io/file.cpp tests/spec_io/free_functions_spec.cpp
git commit -m "deluge::io: mkdir/unlink/rename free functions"
```

---

## Task 6: Full regression check

**Files:** none (verification only).

Confirms Tasks 1-5's additions are inert on every existing BSP (`deluge::io` is compiled into `deluge_app` via `src/deluge/CMakeLists.txt`'s source glob, but nothing calls it yet) and that the new `tests/spec_io/` target coexists cleanly with the existing `tests/spec/` target (which links the REAL FatFS-backed `file_io.h` adapter) without symbol collisions — they're separate executables, never linked together, but worth confirming explicitly since both now provide real definitions of the twelve `deluge_file_*`/`deluge_dir_*` functions.

- [ ] **Step 1: Confirm `src/deluge/io/file.cpp` was picked up by `deluge_app`'s source glob with no CMakeLists edit**

```bash
./dbt build Debug
```

Expected: builds clean. Then confirm the new object file is actually part of the link:

```bash
find build/Debug -iname "*file.cpp.obj" -path "*io*"
```

Expected: a path under `deluge_app.dir/.../io/file.cpp.obj` — proving the glob picked it up without any `src/deluge/CMakeLists.txt` change.

- [ ] **Step 2: Full host/sim build + golden-master check**

```bash
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: bit-exact against the stored golden — `deluge::io` isn't called by anything yet, so nothing should move.

- [ ] **Step 3: Full spec suite, confirming both `tests/spec/` and `tests/spec_io/` build and pass side by side**

```bash
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Expected: all suites pass, including the four new `tests/spec_io/` specs (`status`, `mock_file_io`, `file`, `directory`, `free_functions` — 5 executables/suites from this plan) alongside the existing ones (`file_io_spec` from the `file_io.h` boundary plan, still linking the REAL FatFS engine in `tests/spec/`, unaffected).

- [ ] **Step 4: Commit** (only if any step above required a fix; otherwise this task is verification-only and produces no diff)

---

## Self-Review

**Spec coverage:** design doc §3.1 (`Status`) → Task 1. §3.2 (`File`/`Directory`, move-only, free functions) → Tasks 3-5. §4 (mock-based testing) → Task 2, exercised throughout Tasks 3-5. §5 (roadmap effect) is a documentation note about a *different* doc (the migration Plan 1 design), not something this plan implements — correctly out of scope here.

**Placeholder scan:** every step has complete code or an exact command with expected output. Task 1's `mock_file_io.cpp` placeholder is explicitly a one-step bridge to Task 2, not a deferred requirement — Task 2 fully replaces it in the same plan.

**Type consistency:** `Status`/`to_status` (Task 1) used identically by `File` (Task 3), `Directory` (Task 4), and the free functions (Task 5). `DelugeFile*`/`DelugeDir*` handle types match `file_io.h`'s declarations and the mock's own concrete struct definitions (Task 2) throughout. `mock_file_io_reset()` (Task 2) is called consistently at the start of every test case in Tasks 3-5 that touches file/directory state.
