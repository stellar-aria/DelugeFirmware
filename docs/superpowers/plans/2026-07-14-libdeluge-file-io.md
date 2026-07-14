# `libdeluge` file-I/O boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a real `libdeluge` boundary (`include/libdeluge/file_io.h`) for file-level storage access, backed on rza1/host/Embassy by a new thin C-ABI adapter over the existing `FatFS::File`/`Directory` C++ wrapper, so app code stops calling FatFS directly.

**Architecture:** A new opaque-handle, `DelugeStatus`-returning C-ABI header (matching `block_device.h`/`storage_wait.h` conventions), implemented once in `src/fatfs/file_io.cpp` (shared by every BSP that already links the `fatfs` target — rza1, host, Embassy — since FatFS itself is identical across all three; only `block_device.h`'s diskio differs, and that's unaffected by this work). This plan covers the boundary + shared adapter + error translator only — it does **not** migrate the ~65 existing app call sites (that's a separate follow-on plan, once this interface is proven) and does **not** touch the Linux BSP (which doesn't exist yet and will implement this boundary natively over POSIX instead).

**Tech Stack:** C11 header (`file_io.h`), C++26 implementation (matches `src/fatfs`'s existing `CXX_STANDARD 26`), the existing `src/fatfs/fatfs.hpp` wrapper (`FatFS::File`, `FatFS::Directory`, `FatFS::mkdir`/`unlink`/`rename`), CppSpec for host-buildable unit tests (`tests/spec/`).

**Spec:** `docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md`.

## Global Constraints

- **No behavior change on rza1/host/Embassy in this plan.** Nothing in the app calls the new boundary yet — this is pure addition. The existing golden-master harness must stay bit-exact throughout (verified in Task 5).
- **`FF_USE_MKFS` is `0`** in this project's FatFS config (`src/fatfs/ffconf.h:33`) — there is no way to format a fresh in-memory filesystem at runtime. Real I/O round-trip testing of the adapter therefore isn't practical without new fixture-image infrastructure (which is out of scope here — see Task 4's testing note); it happens naturally via the existing golden-harness once the follow-on plan wires real call sites to this boundary.
- **New C++ must be idiomatic modern C++** (namespaces, `enum class`, no C-casts, RAII) per project convention — this codebase already sets `C++26` for `src/fatfs`.
- **New public headers use the project's GPL license header** (copy the exact block from `include/libdeluge/block_device.h` or `storage_wait.h`).
- **`DelugeFile`/`DelugeDir` are opaque C structs**; the C++ implementation casts between them and the real `FatFS::File`/`FatFS::Directory` objects (`reinterpret_cast`), matching how this project already isolates C++ types behind a C-ABI boundary elsewhere.
- **Paths are plain forward-slash strings**, no FatFS drive prefix — matches existing app usage (confirmed: no `"0:/"` syntax anywhere in the app today).

---

## Task 1: Boundary header — `types.h` additions + `file_io.h`

**Files:**
- Modify: `include/libdeluge/types.h`
- Create: `include/libdeluge/file_io.h`

**Interfaces:**
- Produces: `DelugeStatus` gains `DELUGE_ERR_NOT_FOUND`, `DELUGE_ERR_EXISTS`, `DELUGE_ERR_NO_SPACE`, `DELUGE_ERR_NO_FILESYSTEM`, `DELUGE_ERR_WRITE_PROTECTED`. New opaque types `DelugeFile`, `DelugeDir`; enum `DelugeFileOpenMode`; struct `DelugeDirEntry`; the 12 boundary functions listed below.

This is a pure declarations task — no behavior to test-first. Verification is "does it compile standalone."

- [ ] **Step 1: Add the five new status codes to `types.h`**

Current end of the `DelugeStatus` enum (`include/libdeluge/types.h:32-41`):

```c
typedef enum DelugeStatus {
	DELUGE_OK = 0,
	DELUGE_ERR = -1,         ///< unspecified failure
	DELUGE_ERR_PARAM = -2,   ///< invalid argument
	DELUGE_ERR_BUSY = -3,    ///< resource temporarily unavailable
	DELUGE_ERR_TIMEOUT = -4, ///< operation timed out
	DELUGE_ERR_IO = -5,      ///< hardware / transport I/O error
	DELUGE_ERR_NODEV = -6,   ///< no such device / not present
	DELUGE_ERR_UNSUPPORTED = -7,
} DelugeStatus;
```

Change to:

```c
typedef enum DelugeStatus {
	DELUGE_OK = 0,
	DELUGE_ERR = -1,               ///< unspecified failure
	DELUGE_ERR_PARAM = -2,         ///< invalid argument
	DELUGE_ERR_BUSY = -3,          ///< resource temporarily unavailable
	DELUGE_ERR_TIMEOUT = -4,       ///< operation timed out
	DELUGE_ERR_IO = -5,            ///< hardware / transport I/O error
	DELUGE_ERR_NODEV = -6,         ///< no such device / not present
	DELUGE_ERR_UNSUPPORTED = -7,
	DELUGE_ERR_NOT_FOUND = -8,       ///< path does not exist
	DELUGE_ERR_EXISTS = -9,          ///< path already exists
	DELUGE_ERR_NO_SPACE = -10,       ///< storage full / allocation failed
	DELUGE_ERR_NO_FILESYSTEM = -11,  ///< media present but has no valid filesystem
	DELUGE_ERR_WRITE_PROTECTED = -12, ///< media is read-only / locked
} DelugeStatus;
```

- [ ] **Step 2: Create `include/libdeluge/file_io.h`**

```c
/*
 * Copyright © 2014-2025 Synthstrom Audible Limited
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

/// libdeluge/file_io.h — file-level storage access.
///
/// The application's portable code (storage_manager, audio_file_manager, the
/// browser, the companion SysEx protocol, ...) reads/writes files and
/// directories through this boundary instead of a specific filesystem
/// library. On rza1/host/Embassy this is backed by the vendored FatFS
/// library over `block_device.h`; that dependency is a BSP-internal
/// implementation detail, not something the app links against directly. A
/// BSP may instead back it with the host OS's native filesystem (e.g. a
/// Linux BSP mapping straight to POSIX I/O) with no FatFS involved at all.
///
/// Paths are plain forward-slash strings rooted at the storage volume (no
/// FatFS drive-number prefix, e.g. "SONGS/mysong.XML" not "0:/SONGS/...").
#ifndef LIBDELUGE_FILE_IO_H
#define LIBDELUGE_FILE_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An open file. Opaque; owned by the BSP implementation.
typedef struct DelugeFile DelugeFile;

/// An open directory iterator. Opaque; owned by the BSP implementation.
typedef struct DelugeDir DelugeDir;

/// Longest filename this boundary will report/accept, including the NUL
/// terminator (FAT LFN max is 255 characters).
#define DELUGE_MAX_FILENAME 256

typedef enum DelugeFileOpenMode {
	DELUGE_FILE_READ,        ///< open an existing file for reading
	DELUGE_FILE_WRITE_CREATE, ///< create the file, truncating if it exists
} DelugeFileOpenMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually
/// pass to `deluge_file_close`. [task]
DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out);

/// Read up to `count` bytes into `dst`. `*out_read` is the number of bytes
/// actually read (may be less than `count` at end of file). [task]
DelugeStatus deluge_file_read(DelugeFile* file, void* dst, uint32_t count, uint32_t* out_read);

/// Write `count` bytes from `src`. `*out_written` is the number of bytes
/// actually written. [task]
DelugeStatus deluge_file_write(DelugeFile* file, const void* src, uint32_t count, uint32_t* out_written);

/// Move the file position to an absolute byte offset. [task]
DelugeStatus deluge_file_seek(DelugeFile* file, uint32_t offset);

/// Total size of the file, in bytes. [task]
DelugeStatus deluge_file_size(DelugeFile* file, uint32_t* out_size);

/// Close a file opened with `deluge_file_open`. `file` is invalid after this
/// call regardless of the returned status. [task]
DelugeStatus deluge_file_close(DelugeFile* file);

/// One directory entry returned by `deluge_dir_read`.
typedef struct DelugeDirEntry {
	char name[DELUGE_MAX_FILENAME];
	bool is_directory;
} DelugeDirEntry;

/// Open `path` as a directory for iteration. [task]
DelugeStatus deluge_dir_open(const char* path, DelugeDir** out);

/// Read the next directory entry. If there are no more entries,
/// `*out_has_entry` is set to `false` and the call still returns
/// `DELUGE_OK` (end of directory is not an error). [task]
DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry);

/// Close a directory opened with `deluge_dir_open`. [task]
DelugeStatus deluge_dir_close(DelugeDir* dir);

/// Create a directory. Returns `DELUGE_ERR_EXISTS` if `path` already exists
/// (matches the existing app idiom of treating that as non-fatal). [task]
DelugeStatus deluge_file_mkdir(const char* path);

/// Delete a file or empty directory. [task]
DelugeStatus deluge_file_unlink(const char* path);

/// Rename/move a file or directory. [task]
DelugeStatus deluge_file_rename(const char* old_path, const char* new_path);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_FILE_IO_H
```

- [ ] **Step 3: Verify both headers compile standalone**

Run:

```bash
cat > /tmp/file_io_smoke.c << 'EOF'
#include "libdeluge/file_io.h"
int main(void) {
    DelugeFile* f = 0;
    DelugeStatus s = deluge_file_open("x", DELUGE_FILE_READ, &f);
    return (int)s;
}
EOF
gcc -std=c11 -I include -c /tmp/file_io_smoke.c -o /tmp/file_io_smoke.o
```

Expected: no output, exit code 0 (compiles cleanly). This proves `types.h`'s new enum values and `file_io.h`'s declarations parse correctly as C.

- [ ] **Step 4: Commit**

```bash
git add include/libdeluge/types.h include/libdeluge/file_io.h
git commit -m "libdeluge: add the file_io.h boundary (open/read/write/seek/dir/mkdir/unlink/rename)"
```

---

## Task 2: Pure adapter helpers (mode mapping, directory-entry conversion) — TDD

**Files:**
- Create: `src/fatfs/file_io_internal.hpp`
- Create: `src/fatfs/file_io.cpp` (started here; extended in Task 4)
- Create: `tests/spec/file_io_spec.cpp`

**Interfaces:**
- Consumes: `FatFS::FileInfo` (= `FILINFO`, from `src/fatfs/fatfs.hpp`), `FatFS::Error` (from the same), `DelugeFileOpenMode`/`DelugeDirEntry`/`DelugeStatus` (Task 1).
- Produces: `namespace deluge::fatfs_adapter { FileAccessMode to_fatfs_mode(DelugeFileOpenMode); DelugeStatus to_deluge_status(FatFS::Error); void to_dir_entry(const FatFS::FileInfo&, DelugeDirEntry&, bool& out_has_entry); }` — consumed by Task 4's full adapter and by this task's test.

These three helpers are the only genuinely pure (no real filesystem I/O) pieces of the adapter, so they're the only part of it unit-testable without a mounted filesystem (`FF_USE_MKFS=0` rules out synthesizing one at test time — see Global Constraints).

- [ ] **Step 1: Write the failing test**

```cpp
// tests/spec/file_io_spec.cpp
#include "fatfs/file_io_internal.hpp"

#include "cppspec.hpp"

#include <cstring>

// clang-format off
describe file_io("file_io adapter", $ {
	it("maps DELUGE_FILE_READ to FA_READ", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_READ)).to_equal((FileAccessMode)FA_READ);
	});

	it("maps DELUGE_FILE_WRITE_CREATE to FA_WRITE|FA_CREATE_ALWAYS", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_WRITE_CREATE))
		    .to_equal((FileAccessMode)(FA_WRITE | FA_CREATE_ALWAYS));
	});

	it("maps every FatFS::Error to a non-generic DelugeStatus where one exists", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILE)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_PATH)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::EXIST)).to_equal(DELUGE_ERR_EXISTS);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::WRITE_PROTECTED)).to_equal(DELUGE_ERR_WRITE_PROTECTED);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILESYSTEM)).to_equal(DELUGE_ERR_NO_FILESYSTEM);
	});

	it("maps unrecognized FatFS::Error values to the generic DELUGE_ERR_IO", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::INT_ERR)).to_equal(DELUGE_ERR_IO);
	});

	it("converts a plain file's FILINFO into a DelugeDirEntry", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "SONG1.XML");
		info.fattrib = 0;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(has_entry).to_equal(true);
		expect(std::string(out.name)).to_equal("SONG1.XML");
		expect(out.is_directory).to_equal(false);
	});

	it("marks directories via the AM_DIR attribute bit", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "SONGS");
		info.fattrib = AM_DIR;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(out.is_directory).to_equal(true);
	});

	it("signals end-of-directory when fname is empty, without an error", _ {
		FatFS::FileInfo info{};
		info.fname[0] = 0;
		DelugeDirEntry out{};
		bool has_entry = true;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(has_entry).to_equal(false);
	});
});

CPPSPEC_SPEC(file_io)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake -B build-tests -S tests -G Ninja && cmake --build build-tests --target all_specs
```

Expected: FAIL — `fatfs/file_io_internal.hpp` doesn't exist yet.

- [ ] **Step 3: Implement the three helpers**

```cpp
// src/fatfs/file_io_internal.hpp
#pragma once

#include "fatfs.hpp"
#include "libdeluge/file_io.h"

namespace deluge::fatfs_adapter {

/// Maps the boundary's open mode to FatFS's `FA_*` flag combination.
FileAccessMode to_fatfs_mode(DelugeFileOpenMode mode);

/// Maps a FatFS error to the closest `DelugeStatus`; unrecognized codes
/// (anything not explicitly listed) fall back to `DELUGE_ERR_IO`.
DelugeStatus to_deluge_status(FatFS::Error error);

/// Converts a raw `FILINFO` (as returned by `FatFS::Directory::read`) into a
/// `DelugeDirEntry`. FatFS signals end-of-directory by returning a
/// zero-length `fname` with no error (not by failing the call) — this
/// function reproduces that as `out_has_entry = false`.
void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry);

} // namespace deluge::fatfs_adapter
```

```cpp
// src/fatfs/file_io.cpp
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
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target all_specs && ctest --test-dir build-tests -R file_io --output-on-failure
```

Expected: PASS (7/7 assertions across the 6 `it` blocks).

- [ ] **Step 5: Commit**

```bash
git add src/fatfs/file_io_internal.hpp src/fatfs/file_io.cpp tests/spec/file_io_spec.cpp
git commit -m "fatfs: file_io adapter pure helpers (mode mapping, error mapping, dir-entry conversion)"
```

---

## Task 3: `DelugeStatus` → `Error` translator

**Files:**
- Modify: `src/deluge/util/functions.h`
- Modify: `src/deluge/util/functions.cpp`

**Interfaces:**
- Consumes: `DelugeStatus` (Task 1), the app's `Error` enum (`src/definitions_cxx.hpp`).
- Produces: `Error delugeStatusToError(DelugeStatus status);` — for the follow-on call-site-migration plan to adopt; not called by anything yet.

This sits alongside the two existing translators (`fresultToDelugeErrorCode`, `fatfsErrorToDelugeError`, both in these same two files) — same shape, one layer up, per the design doc. Like its two siblings, this function has no dedicated unit test today (`functions.cpp` pulls in `gui/`/`hid/` dependencies that make it impractical to include in the lean `tests/spec` CppSpec target — confirmed neither existing translator has one); verification is build-success plus this task's manual assertions in Step 3, consistent with the existing precedent rather than introducing a new, one-off test-infra pattern for just this function.

- [ ] **Step 1: Add the declaration**

In `src/deluge/util/functions.h`, immediately after the existing two translator declarations (around line 409-413):

```cpp
Error fresultToDelugeErrorCode(FRESULT result);
namespace FatFS {
enum class Error;
}
Error fatfsErrorToDelugeError(FatFS::Error result);
Error delugeStatusToError(DelugeStatus status);
```

Add `#include "libdeluge/file_io.h"` near the top of `functions.h` (it already includes `fatfs/ff.h`, so add the new include alongside it at line 22):

```cpp
#include "fatfs/ff.h"
#include "libdeluge/file_io.h"
```

- [ ] **Step 2: Add the implementation**

In `src/deluge/util/functions.cpp`, immediately after `fatfsErrorToDelugeError`'s closing brace (around line 2185, following the existing function):

```cpp
Error delugeStatusToError(DelugeStatus status) {
	switch (status) {
	case DELUGE_OK:
		return Error::NONE;
	case DELUGE_ERR_NOT_FOUND:
		return Error::FILE_NOT_FOUND;
	case DELUGE_ERR_EXISTS:
		return Error::FILE_ALREADY_EXISTS;
	case DELUGE_ERR_NO_SPACE:
		return Error::SD_CARD_FULL;
	case DELUGE_ERR_NO_FILESYSTEM:
		return Error::SD_CARD_NO_FILESYSTEM;
	case DELUGE_ERR_WRITE_PROTECTED:
		return Error::WRITE_PROTECTED;
	case DELUGE_ERR_NODEV:
		return Error::SD_CARD_NOT_PRESENT;
	default:
		return Error::SD_CARD;
	}
}
```

- [ ] **Step 3: Build and manually verify**

```bash
dbt build Debug
```

Expected: builds clean (no new warnings). Since nothing calls `delugeStatusToError` yet, there's no runtime behavior to exercise — this step confirms it compiles against the real `Error`/`DelugeStatus` enums with no missing-case warnings (the switch is exhaustive over `DelugeStatus`'s named values via `default`, so `-Wswitch` stays quiet).

- [ ] **Step 4: Commit**

```bash
git add src/deluge/util/functions.h src/deluge/util/functions.cpp
git commit -m "util: add delugeStatusToError, the file_io.h-side counterpart to the existing FatFS translators"
```

---

## Task 4: The full FatFS-backed adapter

**Files:**
- Modify: `src/fatfs/file_io.cpp` (extends Task 2's file)
- Modify: `src/fatfs/CMakeLists.txt`

**Interfaces:**
- Consumes: `FatFS::File`, `FatFS::Directory`, `FatFS::mkdir`, `FatFS::unlink`, `FatFS::rename` (all `src/fatfs/fatfs.hpp`), `deluge::fatfs_adapter::{to_fatfs_mode, to_deluge_status, to_dir_entry}` (Task 2).
- Produces: the 12 `deluge_file_*`/`deluge_dir_*` C-ABI functions declared in `file_io.h` (Task 1) — this is the shared implementation rza1/host/Embassy will all link once `fatfs` is part of their build (it already is, per `docs/dev/target_architecture.md`'s baseline and the M0 link-closure finding for the Rust BSP).

Real I/O round-trip testing isn't practical here (see Global Constraints — `FF_USE_MKFS=0`). Verification for this task is compile/link success against the real `fatfs` target across the two buildable configs available today (rza1 cross-build, host build) — a real, meaningful gate: if the `FatFS::File`/`Directory` API is used incorrectly (wrong argument types, wrong `std::expected` handling), this fails to compile. Full I/O-level exercise happens naturally via the existing golden-master harness once the follow-on plan wires real call sites to this boundary.

- [ ] **Step 1: Extend `src/fatfs/file_io.cpp` with the boundary functions**

Append to `src/fatfs/file_io.cpp` (after the `to_dir_entry` function from Task 2, still inside considering the file structure — these go at file scope, `extern "C"`, after the closing `}` of the `deluge::fatfs_adapter` namespace):

```cpp
extern "C" {

DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out) {
	auto opened = FatFS::File::open(path, deluge::fatfs_adapter::to_fatfs_mode(mode));
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeFile*>(new FatFS::File(std::move(opened.value())));
	return DELUGE_OK;
}

DelugeStatus deluge_file_read(DelugeFile* file, void* dst, uint32_t count, uint32_t* out_read) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->read(std::span{static_cast<std::byte*>(dst), count});
	if (!result) {
		*out_read = 0;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	*out_read = static_cast<uint32_t>(result->size());
	return DELUGE_OK;
}

DelugeStatus deluge_file_write(DelugeFile* file, const void* src, uint32_t count, uint32_t* out_written) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	// FatFS::File::write takes a non-const span (matches f_write's signature); the
	// data is only read, never mutated, so casting away const here is safe.
	auto result = f->write(std::span{static_cast<std::byte*>(const_cast<void*>(src)), count});
	if (!result) {
		*out_written = 0;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	*out_written = *result;
	return DELUGE_OK;
}

DelugeStatus deluge_file_seek(DelugeFile* file, uint32_t offset) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->lseek(offset);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_size(DelugeFile* file, uint32_t* out_size) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	*out_size = static_cast<uint32_t>(f->size());
	return DELUGE_OK;
}

DelugeStatus deluge_file_close(DelugeFile* file) {
	auto* f = reinterpret_cast<FatFS::File*>(file);
	auto result = f->close();
	delete f;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_dir_open(const char* path, DelugeDir** out) {
	auto opened = FatFS::Directory::open(path);
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeDir*>(new FatFS::Directory(std::move(opened.value())));
	return DELUGE_OK;
}

DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->read();
	if (!result) {
		*out_has_entry = false;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	deluge::fatfs_adapter::to_dir_entry(*result, *out, *out_has_entry);
	return DELUGE_OK;
}

DelugeStatus deluge_dir_close(DelugeDir* dir) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->close();
	delete d;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_mkdir(const char* path) {
	auto result = FatFS::mkdir(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_unlink(const char* path) {
	auto result = FatFS::unlink(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_rename(const char* old_path, const char* new_path) {
	auto result = FatFS::rename(old_path, new_path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

} // extern "C"
```

Note: `FatFS::File`'s default constructor is private but its move constructor is public (`File(File&&) = default;` — see `src/fatfs/fatfs.hpp`), so `new FatFS::File(std::move(opened.value()))` is valid; same shape for `FatFS::Directory`.

- [ ] **Step 2: Wire the file into the `fatfs` CMake target**

`src/fatfs/CMakeLists.txt` currently reads:

```cmake
# DO NOT EDIT! This file was automatically generated by `dbt buildgen`
# External but in-source libraries
add_library(fatfs STATIC
    ff.c
    ffsystem.c
    ffunicode.c
    fatfs.cpp
)
```

`file_io.cpp` was already created in Task 2 but not yet added to this list (Task 2's build/test relied on it being pulled directly into `tests/spec`'s `file_io_spec.cpp` compilation unit via the CppSpec test driver, not via this CMake target). Add it now so it's part of the real `fatfs` static library every BSP links:

```cmake
# DO NOT EDIT! This file was automatically generated by `dbt buildgen`
# External but in-source libraries
add_library(fatfs STATIC
    ff.c
    ffsystem.c
    ffunicode.c
    fatfs.cpp
    file_io.cpp
)
```

- [ ] **Step 3: Cross-build for rza1 and verify link**

```bash
dbt build Debug
```

Expected: builds clean. `file_io.cpp`'s new symbols (`deluge_file_open` etc.) are compiled into `libfatfs.a` but not yet referenced by anything — a static library, so no "unused function" link error is expected (only `-Wunused-function` would fire for `static` functions, and these are `extern "C"`, externally visible).

- [ ] **Step 4: Build for host and re-run the spec suite**

```bash
cmake -B build-tests -S tests -G Ninja && cmake --build build-tests --target all_specs
ctest --test-dir build-tests -R file_io --output-on-failure
```

Expected: builds clean, same 7/7 pass as Task 2 (the pure-helper tests are unaffected by this task's additions).

- [ ] **Step 5: Commit**

```bash
git add src/fatfs/file_io.cpp src/fatfs/CMakeLists.txt
git commit -m "fatfs: implement the file_io.h boundary over FatFS::File/Directory"
```

---

## Task 5: Full regression check

**Files:** none (verification only).

Confirms Task 1-4's additions are truly inert on every existing BSP — nothing in the app calls the new boundary yet, so behavior must be unchanged.

- [ ] **Step 1: Full rza1 build**

```bash
dbt build Debug
```

Expected: builds clean, same as before this plan started.

- [ ] **Step 2: Full host/sim build + golden-master check**

```bash
cmake -B build-sim-cpp -S . -DDELUGE_BUILD_SIM=ON && cmake --build build-sim-cpp
scripts/golden_mixdown.sh check
```

(Use whatever the currently-established golden-check invocation is if it differs from this — see `docs/dev/` for the harness's exact entry point if `scripts/golden_mixdown.sh` isn't present in this checkout.)

Expected: all existing goldens report bit-exact. This is the safety net for "adding a whole new library's worth of unused code didn't change anything" — no golden should move, since nothing calls the new functions yet.

- [ ] **Step 3: Full spec suite**

```bash
cmake --build build-tests --target all_specs && ctest --test-dir build-tests --output-on-failure
```

Expected: all specs pass, not just `file_io`.

- [ ] **Step 4: Commit** (only if any of the above required a fix; otherwise this task is verification-only and produces no diff)

---

## Self-Review

**Spec coverage:** design doc §4 (the boundary shape) → Task 1. §5 (error mapping) → Tasks 1 & 3. §6 (per-BSP strategy, rza1/host/Embassy thin-forward half) → Tasks 2 & 4. §6's Linux-BSP half and §7 (migration scope) are explicitly out of scope for this plan (stated in Global Constraints and the plan's own Architecture line) — deferred to the follow-on call-site-migration plan per the brainstorming conversation's scope split. §8 (testing strategy) → Tasks 2, 4, 5. §9 (relationship to other docs) is documentation-only, no code task needed.

**Placeholder scan:** every step has complete code or an exact command with expected output. The two testing-scope notes (Task 3's "no dedicated test, matches sibling translators"; Task 4's "no I/O round-trip test, matches `FF_USE_MKFS=0` constraint") are explicit, justified scope decisions, not deferred-without-explanation placeholders.

**Type consistency:** `DelugeFile`/`DelugeDir` (opaque, Task 1) ↔ `reinterpret_cast<FatFS::File*>`/`reinterpret_cast<FatFS::Directory*>` (Task 4) used consistently. `deluge::fatfs_adapter::to_fatfs_mode`/`to_deluge_status`/`to_dir_entry` (declared Task 2, used Task 2's test and Task 4's adapter) match in signature everywhere. `delugeStatusToError` (Task 3) is additive and not yet called by Tasks 1/2/4 — correct, since call-site migration is out of scope.
