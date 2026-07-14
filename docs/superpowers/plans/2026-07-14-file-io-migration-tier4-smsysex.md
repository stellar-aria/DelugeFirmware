# File-IO Migration Tier 4 (`smsysex.cpp`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate `smsysex.cpp` (the companion SysEx file-transfer protocol) off raw FatFS onto `deluge::io`, resolving the roadmap's two bounded gaps (`f_utime` equivalent, FRESULT wire-compat) plus a third found during design (`DelugeDirEntry` missing size/timestamp/attributes for `getDirEntries`), while keeping the wire protocol's `"err"`/`"date"`/`"time"`/`"attr"`/`"size"` values bit-identical to today's output.

**Architecture:** Extend `file_io.h`/`deluge::io` with a portable `DelugeTimestamp` type, a `deluge_file_set_time` function, and a richer `DelugeDirEntry`. Give `smsysex.cpp` its own local, file-scope translators (`toWireFresult`, wire-native DOS date/time pack/unpack, wire-native attribute pack) that convert `deluge::io::Status`/`DelugeTimestamp`/`DelugeDirEntry` to/from the wire's pre-existing byte shapes — these translators are the permanent abstraction boundary between "whatever backend implements `file_io.h`" and "what the companion app has always seen on the wire," not a migration shim.

**Tech Stack:** C++23, `deluge::io` (`src/deluge/io/file.hpp`), `file_io.h`/`types.h` (`include/libdeluge/`), `FatFS::` C++ wrapper (`src/fatfs/fatfs.hpp`), cppspec (`tests/spec/`, `tests/spec_io/`).

**Design doc:** `docs/superpowers/specs/2026-07-14-file-io-migration-tier4-smsysex-design.md`

## Global Constraints

- **Wire byte-shapes are frozen.** Every `"err"`/`"date"`/`"time"`/`"attr"`/`"size"` value the companion app sees must be bit-identical to today's output for the same underlying FatFS state, with two narrow, explicitly-documented exceptions (both already-accepted precedent from the boundary's original design, not new decisions): (1) `Status::NOT_FOUND` cannot distinguish the original `FR_NO_FILE` from `FR_NO_PATH`; `toWireFresult` always produces `FR_NO_FILE`. (2) `closeFIL`'s "no such file handle" case has no `deluge::io::Status` equivalent to `FR_INVALID_OBJECT`; it is special-cased to return `FR_INVALID_OBJECT` directly, without going through the `Status` pipeline at all — so this one is actually **not** a deviation, and every task below must follow the same "special-case any FRESULT value with no `Status` equivalent directly; only route real I/O results through `toWireFresult`" pattern rather than force every FRESULT value through `Status`.
- **Preserve existing control flow, including `goto`**, wherever the underlying local variables stay trivial (raw pointers, enums, `FRESULT`) — this file uses `goto retry`/`goto errorFound` as ordinary control flow and modernizing it is out of scope. The one exception is `performFileCopy`'s `goto retry_copy`, which must become a `for (;;)` loop: its retry point is `FIL srcFile, dstFile;`, which becomes `deluge::io::File` locals, and jumping backward past a non-trivial object's declaration without a proper scope exit does not re-run its destructor — a real correctness hazard the original raw-`FIL` version didn't have. This is a targeted, structurally-necessary change, not a stylistic one; call it out explicitly in that task's commit message.
- **Preserve existing quirks/bugs exactly**, with one deliberate, explicitly-flagged exception (`getDirEntries`'s end-of-directory check, Task 4). Two quirks in particular must survive: `createPathDirectories` continues looping over remaining path segments even after an intermediate `mkdir` fails (only a genuine `opendir`-style probe error returns early); `performFileCopy`'s short-write case (fewer bytes written than requested, but no I/O error) is not itself flagged as an error — `status` stays `OK`. Both are marked with `// preserving pre-existing quirk` comments at the point they're reproduced.
- **`deluge::io::File`/`Directory` are move-only with no default/empty state.** Anywhere this file needs a "not currently open" slot (the `FILdata` pool, the directory-scan state), wrap in `std::optional<deluge::io::File>` / `std::optional<deluge::io::Directory>` rather than adding an empty state to the wrapper classes themselves.
- **New code is idiomatic C++23**, matching the rest of this migration (enum class, designated initializers, `std::span`/`std::optional`, no C-style casts where a `static_cast`/`reinterpret_cast` reads better, no new `goto`). Two small, justified simplifications ride along with this task because they become *dead* once the FatFS layer is gone, not because of a general cleanup mandate: `openFIL`'s `int forWrite` parameter (only ever called with a bool-truncated value — confirmed via `SysExProtocolNotes.md`, which documents `"write"` as `0`/`1` only) becomes `bool forWrite`; `FileOpParams::getFromTC()`/`getToTC()` (TCHAR casts with no remaining caller after this migration) are deleted, and `setFileTimestamp`'s `const TCHAR* path` parameter becomes `std::string_view path`.
- FAT DOS date/time has 2-second resolution and no timezone; `DelugeTimestamp`'s `second` field can lose its low bit on a round trip through a FatFS backend — expected, not a bug, and already noted in the design doc.
- Never use `git commit --amend` after a clang-format pre-commit hook failure; create a fresh commit instead.
- `smsysex.cpp` has no existing host-side functional test and is not on the golden-master render path; full rza1 + host/sim builds staying clean, plus the new `spec`/`spec_io` cases, are this plan's automated signal. A manual smoke pass against the companion app (or a replayed SysEx dump) is the only check for actual wire-level correctness — call this out as a known outstanding gate in Task 5, the same way Tier 1 tracked its hardware-test gate.

---

### Task 1: Boundary extension — `DelugeTimestamp`, `deluge_file_set_time`, extended `DelugeDirEntry`

**Files:**
- Modify: `include/libdeluge/types.h`
- Modify: `include/libdeluge/file_io.h`
- Modify: `src/fatfs/fatfs.hpp` (no signature change — `utime` is already declared, just currently unimplemented)
- Modify: `src/fatfs/fatfs.cpp`
- Modify: `src/fatfs/file_io_internal.hpp`
- Modify: `src/fatfs/file_io.cpp`
- Modify: `src/deluge/io/file.hpp`
- Modify: `src/deluge/io/file.cpp`
- Modify: `tests/spec_io/mock_file_io.h`
- Modify: `tests/spec_io/mock_file_io.cpp`
- Modify: `tests/spec/file_io_spec.cpp`
- Modify: `tests/spec_io/free_functions_spec.cpp`
- Modify: `tests/spec_io/directory_spec.cpp`

**Interfaces:**
- Produces: `DelugeTimestamp` (C struct, `types.h`), `deluge_file_set_time(const char*, DelugeTimestamp) -> DelugeStatus` (`file_io.h`), extended `DelugeDirEntry` (adds `size`, `modified_time`, `is_read_only`, `is_hidden`, `is_system`, `is_archive`), `deluge::io::set_time(std::string_view, DelugeTimestamp) -> std::expected<void, Status>`, `deluge::fatfs_adapter::to_fat_date(DelugeTimestamp) -> WORD`, `to_fat_time(DelugeTimestamp) -> WORD`, `from_fat_date_time(WORD, WORD) -> DelugeTimestamp`. Task 2 (and later tasks) consume all of these from `smsysex.cpp`.

- [ ] **Step 1: `DelugeTimestamp` in `types.h`**

Add after the `DelugeStatus` enum (`include/libdeluge/types.h`, after line 47):

```c
/// A portable, timezone-free timestamp — matches FAT DOS date/time's own
/// decomposition (year/month/day/hour/minute/second, no epoch, no TZ) so no
/// backend needs to invent a timezone. A DOS-backed implementation rounds
/// `second` down to its native 2-second resolution.
typedef struct DelugeTimestamp {
	uint16_t year;  ///< full year, e.g. 2026 (not DOS's 1980-relative offset)
	uint8_t month;  ///< 1-12
	uint8_t day;    ///< 1-31
	uint8_t hour;   ///< 0-23
	uint8_t minute; ///< 0-59
	uint8_t second; ///< 0-59
} DelugeTimestamp;
```

- [ ] **Step 2: extend `file_io.h`**

Add after `DelugeFileOpenMode`/before `deluge_file_open` (`include/libdeluge/file_io.h`, after line 53):

```c
/// Set a file or directory's last-modified timestamp. [task]
DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp);
```

Replace the `DelugeDirEntry` struct (lines 78-81) with:

```c
/// One directory entry returned by `deluge_dir_read`.
typedef struct DelugeDirEntry {
	char name[DELUGE_MAX_FILENAME];
	bool is_directory;
	uint32_t size;
	DelugeTimestamp modified_time;
	bool is_read_only;
	bool is_hidden;
	bool is_system;
	bool is_archive;
} DelugeDirEntry;
```

- [ ] **Step 3: implement `FatFS::utime`**

`FatFS::utime` is declared at `src/fatfs/fatfs.hpp:203` but has no implementation anywhere in the tree (confirmed via `grep -rn utime src/fatfs/` — only the declaration, `ff.h`'s `f_utime` prototype, and `ffconf.h`'s `FF_USE_CHMOD` comment turn up). Add the implementation to `src/fatfs/fatfs.cpp`, immediately after the existing `stat` function (after line 128, still inside the `#if FF_FS_READONLY == 0 and FF_FS_MINIMIZE == 0` block — no, `utime` is declared under a *different* guard, `#if FF_USE_CHMOD`, so give it its own guarded block right after that one closes):

```cpp
#if FF_USE_CHMOD
std::expected<void, Error> utime(std::string_view path, const FILINFO *fno) {
  FF_TRY(f_utime(path.data(), fno));
  return {};
}
#endif
```

(Matches this file's existing 2-space indentation, not the project's usual tabs — `fatfs.cpp` is a vendored-adjacent file already formatted this way; follow the surrounding style, not the project default.)

- [ ] **Step 4: FatFS adapter — DOS pack/unpack helpers + extended `to_dir_entry` + `deluge_file_set_time`**

`src/fatfs/file_io_internal.hpp` — add after `to_deluge_status`'s declaration (after line 15):

```cpp
/// Packs a DelugeTimestamp into a FAT DOS-format date WORD (the format
/// FILINFO::fdate/f_utime's FILINFO::fdate use — see ff.h's FILINFO comment).
WORD to_fat_date(DelugeTimestamp timestamp);

/// Packs a DelugeTimestamp into a FAT DOS-format time WORD (FILINFO::ftime).
WORD to_fat_time(DelugeTimestamp timestamp);

/// Unpacks a FAT DOS-format date/time WORD pair back into a DelugeTimestamp.
DelugeTimestamp from_fat_date_time(WORD date, WORD time);
```

`src/fatfs/file_io.cpp` — add these three functions to the `deluge::fatfs_adapter` namespace, right before `to_dir_entry` (before line 44):

```cpp
WORD to_fat_date(DelugeTimestamp timestamp) {
	return static_cast<WORD>(((timestamp.year - 1980) << 9) | (timestamp.month << 5) | timestamp.day);
}

WORD to_fat_time(DelugeTimestamp timestamp) {
	return static_cast<WORD>((timestamp.hour << 11) | (timestamp.minute << 5) | (timestamp.second / 2));
}

DelugeTimestamp from_fat_date_time(WORD date, WORD time) {
	DelugeTimestamp timestamp{};
	timestamp.year = static_cast<uint16_t>(1980 + ((date >> 9) & 0x7F));
	timestamp.month = static_cast<uint8_t>((date >> 5) & 0x0F);
	timestamp.day = static_cast<uint8_t>(date & 0x1F);
	timestamp.hour = static_cast<uint8_t>((time >> 11) & 0x1F);
	timestamp.minute = static_cast<uint8_t>((time >> 5) & 0x3F);
	timestamp.second = static_cast<uint8_t>((time & 0x1F) * 2);
	return timestamp;
}
```

Replace `to_dir_entry`'s body (lines 44-53) with the extended version:

```cpp
void to_dir_entry(const FatFS::FileInfo& info, DelugeDirEntry& out, bool& out_has_entry) {
	if (info.fname[0] == 0) {
		out_has_entry = false;
		return;
	}
	out_has_entry = true;
	std::strncpy(out.name, info.fname, DELUGE_MAX_FILENAME - 1);
	out.name[DELUGE_MAX_FILENAME - 1] = 0;
	out.is_directory = (info.fattrib & AM_DIR) != 0;
	out.size = static_cast<uint32_t>(info.fsize);
	out.modified_time = from_fat_date_time(info.fdate, info.ftime);
	out.is_read_only = (info.fattrib & AM_RDO) != 0;
	out.is_hidden = (info.fattrib & AM_HID) != 0;
	out.is_system = (info.fattrib & AM_SYS) != 0;
	out.is_archive = (info.fattrib & AM_ARC) != 0;
}
```

Add `deluge_file_set_time` to the `extern "C"` block, after `deluge_file_close` (after line 115):

```cpp
DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp) {
	FatFS::FileInfo info{};
	info.fdate = deluge::fatfs_adapter::to_fat_date(timestamp);
	info.ftime = deluge::fatfs_adapter::to_fat_time(timestamp);
	auto result = FatFS::utime(path, &info);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}
```

- [ ] **Step 5: `deluge::io::set_time`**

`src/deluge/io/file.hpp` — add after the `Directory` class, before the free `mkdir`/`unlink`/`rename` declarations (after line 97):

```cpp
std::expected<void, Status> set_time(std::string_view path, DelugeTimestamp timestamp);
```

`src/deluge/io/file.cpp` — add alongside `mkdir`/`unlink`/`rename` (after `rename`, before the closing `} // namespace deluge::io`, i.e. after line 179):

```cpp
std::expected<void, Status> set_time(std::string_view path, DelugeTimestamp timestamp) {
	DelugeStatus status = deluge_file_set_time(path.data(), timestamp);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}
```

- [ ] **Step 6: extend the mock**

`tests/spec_io/mock_file_io.cpp` — extend `MockEntry` (lines 19-22):

```cpp
struct MockEntry {
	bool is_directory = false;
	std::vector<uint8_t> data; // unused for directories
	DelugeTimestamp modified_time{};
	bool is_read_only = false;
	bool is_hidden = false;
	bool is_system = false;
	bool is_archive = false;
};
```

Replace `DelugeDir` (lines 37-40) so each snapshotted child carries its full metadata, not just `(name, is_directory)`:

```cpp
struct DelugeDir {
	struct ChildInfo {
		std::string name;
		MockEntry entry; // snapshotted at open time
	};
	std::vector<ChildInfo> children;
	size_t index = 0;
};
```

Update `deluge_dir_open`'s population loop (lines 97-113) to snapshot the full entry:

```cpp
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
```

Update `deluge_dir_read` (lines 115-126) to populate every field:

```cpp
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
```

Add `deluge_file_set_time` to the `extern "C"` block, after `deluge_file_rename` (after line 161):

```cpp
DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp) {
	std::string p(path);
	auto it = g_entries.find(p);
	if (it == g_entries.end()) {
		return DELUGE_ERR_NOT_FOUND;
	}
	it->second.modified_time = timestamp;
	return DELUGE_OK;
}
```

`tests/spec_io/mock_file_io.h` needs no change — `DelugeTimestamp` reaches it transitively via `libdeluge/file_io.h`.

- [ ] **Step 7: new test cases**

`tests/spec/file_io_spec.cpp` — add before the closing `});` of the `file_io` describe block (before line 68):

```cpp
	it("packs a DelugeTimestamp into FAT-native date/time WORDs", _ {
		DelugeTimestamp ts{.year = 2026, .month = 7, .day = 14, .hour = 16, .minute = 45, .second = 30};
		WORD date = deluge::fatfs_adapter::to_fat_date(ts);
		WORD time = deluge::fatfs_adapter::to_fat_time(ts);
		expect(date).to_equal(static_cast<WORD>(((2026 - 1980) << 9) | (7 << 5) | 14));
		expect(time).to_equal(static_cast<WORD>((16 << 11) | (45 << 5) | (30 / 2)));
	});

	it("round-trips a FAT date/time pair through from_fat_date_time", _ {
		DelugeTimestamp original{.year = 2001, .month = 3, .day = 9, .hour = 8, .minute = 5, .second = 44};
		WORD date = deluge::fatfs_adapter::to_fat_date(original);
		WORD time = deluge::fatfs_adapter::to_fat_time(original);
		DelugeTimestamp round_tripped = deluge::fatfs_adapter::from_fat_date_time(date, time);
		expect(round_tripped.year).to_equal(original.year);
		expect(round_tripped.month).to_equal(original.month);
		expect(round_tripped.day).to_equal(original.day);
		expect(round_tripped.hour).to_equal(original.hour);
		expect(round_tripped.minute).to_equal(original.minute);
		expect(round_tripped.second).to_equal(44u); // 44 is even: survives the 2s-resolution rounding exactly
	});

	it("rounds odd seconds down to FAT's 2-second resolution", _ {
		DelugeTimestamp original{.year = 2020, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 45};
		WORD time = deluge::fatfs_adapter::to_fat_time(original);
		DelugeTimestamp round_tripped = deluge::fatfs_adapter::from_fat_date_time(0, time);
		expect(round_tripped.second).to_equal(44u);
	});

	it("converts a FILINFO's size, timestamp, and attribute bits into a DelugeDirEntry", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "REC001.WAV");
		info.fsize = 123456;
		info.fdate = deluge::fatfs_adapter::to_fat_date({.year = 2024, .month = 12, .day = 25, .hour = 0, .minute = 0, .second = 0});
		info.ftime = deluge::fatfs_adapter::to_fat_time({.year = 2024, .month = 12, .day = 25, .hour = 9, .minute = 30, .second = 0});
		info.fattrib = AM_RDO | AM_ARC;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(out.size).to_equal(123456u);
		expect(out.modified_time.year).to_equal(2024);
		expect(out.modified_time.hour).to_equal(9);
		expect(out.is_read_only).to_equal(true);
		expect(out.is_archive).to_equal(true);
		expect(out.is_hidden).to_equal(false);
		expect(out.is_system).to_equal(false);
	});
```

`tests/spec_io/free_functions_spec.cpp` — add before the closing `});` (before line 44):

```cpp
	it("set_time updates a file's stored modification time", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		expect(f.has_value()).to_equal(true);
		expect(f->close()).to_have_value();
		DelugeTimestamp ts{.year = 2026, .month = 7, .day = 14, .hour = 12, .minute = 0, .second = 0};
		expect(deluge::io::set_time("a.txt", ts)).to_have_value();
	});

	it("set_time on a missing path reports NOT_FOUND", _ {
		mock_file_io_reset();
		DelugeTimestamp ts{};
		auto result = deluge::io::set_time("missing.txt", ts);
		expect(result.has_value()).to_equal(false);
		expect(result.error()).to_equal(deluge::io::Status::NOT_FOUND);
	});
```

`tests/spec_io/directory_spec.cpp` — add before the closing `});` (before line 55):

```cpp
	it("propagates size through Directory::read", _ {
		mock_file_io_reset();
		deluge_file_mkdir("SONGS");
		auto f = deluge::io::File::open("SONGS/big.wav", DELUGE_FILE_WRITE_CREATE);
		std::byte buf[10]{};
		(void)f->write(buf);
		(void)f->close();

		auto dir = deluge::io::Directory::open("SONGS");
		expect(dir.has_value()).to_equal(true);
		auto entry = dir->read();
		expect(entry).to_have_value();
		expect(entry->has_value()).to_equal(true);
		expect((*entry)->size).to_equal(10u);
	});
```

- [ ] **Step 8: build and test**

```bash
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: all specs pass (including the new `file_io_spec`/`free_functions_spec`/`directory_spec` cases), both builds clean, golden-master still bit-exact (this task touches no render-path code).

- [ ] **Step 9: commit**

```bash
git add include/libdeluge/types.h include/libdeluge/file_io.h \
  src/fatfs/fatfs.cpp src/fatfs/file_io_internal.hpp src/fatfs/file_io.cpp \
  src/deluge/io/file.hpp src/deluge/io/file.cpp \
  tests/spec_io/mock_file_io.cpp tests/spec/file_io_spec.cpp \
  tests/spec_io/free_functions_spec.cpp tests/spec_io/directory_spec.cpp
git commit -m "file_io: add timestamp-setting and extend DelugeDirEntry (Tier 4 boundary)"
```

---

### Task 2: `smsysex.cpp` — translators + the always-local-handle functions

Migrates every function that never touches the shared `openFiles` pool or `sxDIR` global: `createPathDirectories`, `openFile`'s FRESULT-preserving helper `openFIL`'s *signature only is NOT part of this task* (that's Task 3 — `openFIL`/`closeFIL` touch the pool). This task covers `deleteFile`, `createDirectory`, `rename`, `updateTime`, `setFileTimestamp`, `performFileCopy`, `copyFile`, `moveFile`, `createPathDirectories`, plus the new local translators they all depend on.

**Files:**
- Modify: `src/deluge/storage/smsysex.h`
- Modify: `src/deluge/storage/smsysex.cpp`

**Interfaces:**
- Consumes: `deluge::io::{mkdir, unlink, rename, set_time, File, Status}` (Task 1's `set_time`, plus the existing Tier-1-proven rest), `DelugeTimestamp` (Task 1).
- Produces: three file-scope `static` translators in `smsysex.cpp` — `FRESULT toWireFresult(deluge::io::Status)`, `std::optional<DelugeTimestamp> timestampFromWireDateTime(uint32_t date, uint32_t time)` — consumed by every later task (3 and 4) that builds a wire reply from a `deluge::io::Status`, or accepts wire date/time ints.

- [ ] **Step 1: add `#include "io/file.hpp"` and `<optional>`**

`src/deluge/storage/smsysex.cpp` — add after `#include "storage/smsysex.h"` (after line 1):

```cpp
#include "io/file.hpp"
```

`src/deluge/storage/smsysex.h` — add near the top (after line 3, before `struct FILdata;`):

```cpp
#include "io/file.hpp"
#include <optional>
```

- [ ] **Step 2: update `smsysex.h`'s declarations**

Replace `FileOpParams` (lines 12-23) — drop the now-dead `getFromTC`/`getToTC`:

```cpp
struct FileOpParams {
	std::string fromName{};
	std::string toName{};
	uint32_t date = 0;
	uint32_t time = 0;

	const char* getFromPath() const { return fromName.c_str(); }
	const char* getToPath() const { return toName.c_str(); }
	bool hasTimestamp() const { return date != 0 || time != 0; }
};
```

Update the function declarations that change signature (lines 25, 45, 56-57):

```cpp
FILdata* openFIL(const char* fPath, bool forWrite, FRESULT* eCode);
```
```cpp
FRESULT createPathDirectories(std::string& path, std::optional<DelugeTimestamp> timestamp);
```
```cpp
FRESULT performFileCopy(const FileOpParams& params);
void setFileTimestamp(std::string_view path, uint32_t date, uint32_t time);
```

(`performFileCopy`'s declared type doesn't change — it was already `FRESULT`; only `setFileTimestamp`'s parameter type and `openFIL`/`createPathDirectories`'s signatures change. `closeFIL`'s declaration, line 26, is unchanged — still `FRESULT closeFIL(FILdata* fd);`.)

- [ ] **Step 3: add the local translators**

`src/deluge/storage/smsysex.cpp` — add in an anonymous namespace right after the existing globals (after line 50, before `smSysex::noteSessionIdUse`):

```cpp
namespace {

// Freezes the wire's "err" attribute at today's FRESULT-numeric values,
// independent of whatever backend implements file_io.h underneath — this is
// the permanent abstraction boundary between the companion protocol and
// deluge::io, not a migration shim. One accepted, documented collapse:
// Status::NOT_FOUND can't distinguish the original FR_NO_FILE from
// FR_NO_PATH (both already collapsed into DELUGE_ERR_NOT_FOUND at the
// boundary's original design); this always produces FR_NO_FILE.
// Status::UNSUPPORTED has no real FRESULT analog; FR_INVALID_PARAMETER is
// the closest fit.
FRESULT toWireFresult(deluge::io::Status status) {
	switch (status) {
	case deluge::io::Status::OK:
		return FRESULT::FR_OK;
	case deluge::io::Status::ERR:
	case deluge::io::Status::IO:
		return FRESULT::FR_DISK_ERR;
	case deluge::io::Status::PARAM:
	case deluge::io::Status::UNSUPPORTED:
		return FRESULT::FR_INVALID_PARAMETER;
	case deluge::io::Status::BUSY:
		return FRESULT::FR_LOCKED;
	case deluge::io::Status::TIMEOUT:
		return FRESULT::FR_TIMEOUT;
	case deluge::io::Status::NODEV:
		return FRESULT::FR_NOT_READY;
	case deluge::io::Status::NOT_FOUND:
		return FRESULT::FR_NO_FILE;
	case deluge::io::Status::EXISTS:
		return FRESULT::FR_EXIST;
	case deluge::io::Status::NO_SPACE:
		return FRESULT::FR_DENIED;
	case deluge::io::Status::NO_FILESYSTEM:
		return FRESULT::FR_NO_FILESYSTEM;
	case deluge::io::Status::WRITE_PROTECTED:
		return FRESULT::FR_WRITE_PROTECTED;
	case deluge::io::Status::NO_MEMORY:
		return FRESULT::FR_NOT_ENOUGH_CORE;
	}
	return FRESULT::FR_DISK_ERR; // unreachable while the switch above stays exhaustive
}

// The wire's "date"/"time" ints are already FAT DOS-packed WORDs (the
// companion app speaks FatFS's native format directly) -- this decodes them
// into the boundary's portable DelugeTimestamp. Returns nullopt when both
// are zero, matching every existing call site's "date != 0 || time != 0"
// gate for "no timestamp requested."
std::optional<DelugeTimestamp> timestampFromWireDateTime(uint32_t date, uint32_t time) {
	if (date == 0 && time == 0) {
		return std::nullopt;
	}
	DelugeTimestamp ts{};
	ts.year = static_cast<uint16_t>(1980 + ((date >> 9) & 0x7F));
	ts.month = static_cast<uint8_t>((date >> 5) & 0x0F);
	ts.day = static_cast<uint8_t>(date & 0x1F);
	ts.hour = static_cast<uint8_t>((time >> 11) & 0x1F);
	ts.minute = static_cast<uint8_t>((time >> 5) & 0x3F);
	ts.second = static_cast<uint8_t>((time & 0x1F) * 2);
	return ts;
}

} // namespace
```

- [ ] **Step 4: `deleteFile`**

Replace the whole function (lines 305-333):

```cpp
void smSysex::deleteFile(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string path;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!path.empty()) {
		D_PRINTLN(path.c_str());
		auto result = deluge::io::unlink(path);
		FRESULT errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^delete", false, true);
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}
```

- [ ] **Step 5: `createDirectory`**

Replace the whole function (lines 335-378):

```cpp
void smSysex::createDirectory(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string path;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!path.empty()) {
		D_PRINTLN(path.c_str());
		auto made = deluge::io::mkdir(path);
		deluge::io::Status status = made.has_value() ? deluge::io::Status::OK : made.error();
		auto timestamp = timestampFromWireDateTime(date, time);
		if (made.has_value() && timestamp.has_value()) {
			auto timed = deluge::io::set_time(path, *timestamp);
			status = timed.has_value() ? deluge::io::Status::OK : timed.error();
		}
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^mkdir", false, true);
		jWriter.writeAttribute("path", path.c_str());
		jWriter.writeAttribute("err", toWireFresult(status));
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}
```

- [ ] **Step 6: `rename`**

Replace the whole function (lines 380-417). Note: `deluge::io::rename` must be fully qualified inside `smSysex::rename`'s body — an unqualified call would resolve to `smSysex::rename` itself (this function), not the free function.

```cpp
void smSysex::rename(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string fromName;
	std::string toName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "from")) {
			reader.readTagOrAttributeValueString(fromName);
		}
		else if (!strcmp(tagName, "to")) {
			reader.readTagOrAttributeValueString(toName);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!fromName.empty() && !toName.empty()) {
		D_PRINTLN(fromName.c_str());
		D_PRINTLN(toName.c_str());
		auto result = deluge::io::rename(fromName, toName);
		FRESULT errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^rename", false, true);
		jWriter.writeAttribute("from", fromName.c_str());
		jWriter.writeAttribute("to", toName.c_str());
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}
```

- [ ] **Step 7: `updateTime`**

Replace the whole function (lines 666-707):

```cpp
void smSysex::updateTime(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	std::string path;

	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FRESULT errCode;
	auto timestamp = timestampFromWireDateTime(date, time);
	if (!path.empty() && timestamp.has_value()) {
		auto result = deluge::io::set_time(path, *timestamp);
		errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
	}
	else {
		errCode = FRESULT::FR_INVALID_PARAMETER;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^utime", false, true);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
```

- [ ] **Step 8: `createPathDirectories`**

Replace the whole function (lines 174-222). The two preserved quirks are marked inline: (a) an intermediate `mkdir` failure does not return early — the loop continues over remaining path segments, same as today; (b) the opened probe directory closes via RAII (no explicit `close()` call needed) where the original called `f_closedir` explicitly.

```cpp
FRESULT smSysex::createPathDirectories(std::string& path, std::optional<DelugeTimestamp> timestamp) {
	if (path.size() > 256) {
		return FRESULT::FR_INVALID_PARAMETER;
	}

	char working[257];
	char pathPart[257];
	strcpy(working, path.c_str());
	int len = strlen(working);
	int lastSlash;
	for (lastSlash = len - 1; lastSlash >= 0; lastSlash--) {
		if (working[lastSlash] == '/')
			break;
	}
	if (lastSlash == 0) {
		return FRESULT::FR_INVALID_PARAMETER;
	}

	deluge::io::Status status = deluge::io::Status::OK;
	int jx = 1; // skip the leading slash.
	while (jx <= lastSlash) {
		if (working[jx] == '/') {
			working[jx] = 0;
			strcpy(pathPart, working);
			working[jx] = '/';
			if (strlen(pathPart)) {
				auto dir = deluge::io::Directory::open(pathPart);
				if (!dir.has_value() && dir.error() == deluge::io::Status::NOT_FOUND) {
					auto made = deluge::io::mkdir(pathPart);
					// preserving pre-existing quirk: a failed mkdir here does not
					// return early, it just leaves `status` set and the loop
					// continues to the next path segment.
					status = made.has_value() ? deluge::io::Status::OK : made.error();
					if (made.has_value() && timestamp.has_value()) {
						auto timed = deluge::io::set_time(pathPart, *timestamp);
						status = timed.has_value() ? deluge::io::Status::OK : timed.error();
					}
				}
				else if (!dir.has_value()) {
					return toWireFresult(dir.error());
				}
				// else: pathPart already exists as a directory; `dir` closes via
				// RAII when it goes out of scope at the end of this block.
			}
		}
		jx++;
	}
	return toWireFresult(status);
}
```

- [ ] **Step 9: `setFileTimestamp`**

Replace the whole function (lines 877-884):

```cpp
void smSysex::setFileTimestamp(std::string_view path, uint32_t date, uint32_t time) {
	auto timestamp = timestampFromWireDateTime(date, time);
	if (timestamp.has_value()) {
		(void)deluge::io::set_time(path, *timestamp);
	}
}
```

- [ ] **Step 10: `performFileCopy`**

Replace the whole function (lines 887-949). This is the one place `goto` becomes a `for (;;)` loop (see Global Constraints — jumping backward past a `deluge::io::File` declaration would skip its destructor, a real correctness hazard the original raw-`FIL` version didn't have). The short-write quirk is preserved and marked inline.

```cpp
FRESULT smSysex::performFileCopy(const FileOpParams& params) {
	D_PRINTLN(params.fromName.c_str());
	D_PRINTLN(params.toName.c_str());

	bool pathCreateTried = false;
	for (;;) {
		auto src = deluge::io::File::open(params.fromName, DELUGE_FILE_READ);
		if (!src.has_value()) {
			return toWireFresult(src.error());
		}

		auto dst = deluge::io::File::open(params.toName, DELUGE_FILE_WRITE_CREATE);
		if (!dst.has_value()) {
			if (dst.error() == deluge::io::Status::NOT_FOUND && !pathCreateTried) {
				// Don't set timestamps on directories - let them use current time
				std::string toNameCopy = params.toName;
				createPathDirectories(toNameCopy, std::nullopt);
				pathCreateTried = true;
				continue; // `src` closes via RAII at the end of this iteration
			}
			return toWireFresult(dst.error());
		}

		deluge::io::Status status = deluge::io::Status::OK;
		if (!readBlockBuffer) {
			readBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
		}
		if (readBlockBuffer) {
			for (;;) {
				auto bytesRead = src->read(std::span{reinterpret_cast<std::byte*>(readBlockBuffer), blockBufferMax});
				if (!bytesRead.has_value()) {
					status = bytesRead.error();
					break;
				}
				if (bytesRead->empty()) {
					break;
				}
				auto bytesWritten = dst->write(*bytesRead);
				if (!bytesWritten.has_value()) {
					status = bytesWritten.error();
					break;
				}
				if (*bytesWritten != bytesRead->size()) {
					// preserving pre-existing quirk: a short write here is not
					// itself flagged as an error, `status` stays OK.
					break;
				}
				if (bytesRead->size() < blockBufferMax) {
					break;
				}
			}
		}
		else {
			status = deluge::io::Status::NO_MEMORY;
		}

		if (status == deluge::io::Status::OK && params.hasTimestamp()) {
			setFileTimestamp(params.toName, params.date, params.time);
		}
		return toWireFresult(status);
	}
}
```

- [ ] **Step 11: `copyFile`**

Replace the whole function (lines 951-967):

```cpp
void smSysex::copyFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	FRESULT errCode = performFileCopy(params);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^copy", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
```

- [ ] **Step 12: `moveFile`**

Replace the whole function (lines 969-1026):

```cpp
void smSysex::moveFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	D_PRINTLN(params.fromName.c_str());
	D_PRINTLN(params.toName.c_str());

	// Try rename first (works if source and destination are on same filesystem)
	auto renamed = deluge::io::rename(params.fromName, params.toName);
	FRESULT errCode = toWireFresult(renamed.has_value() ? deluge::io::Status::OK : renamed.error());

	// If rename failed due to missing path, try creating directories
	if (!renamed.has_value() && renamed.error() == deluge::io::Status::NOT_FOUND) {
		// Don't set timestamps on directories - let them use current time
		std::string toNameCopy = params.toName;
		createPathDirectories(toNameCopy, std::nullopt);
		renamed = deluge::io::rename(params.fromName, params.toName);
		errCode = toWireFresult(renamed.has_value() ? deluge::io::Status::OK : renamed.error());
	}

	// If rename still fails (e.g., cross-filesystem move), fall back to copy+delete
	if (!renamed.has_value()) {
		// Use the shared copy function
		errCode = performFileCopy(params);

		// If copy was successful, delete the source file
		if (errCode == FRESULT::FR_OK) {
			auto deleted = deluge::io::unlink(params.fromName);

			// For move operation, both copy and delete must succeed
			if (!deleted.has_value()) {
				FRESULT deleteResult = toWireFresult(deleted.error());
				D_PRINTLN("Move: copy succeeded but delete failed: %d", deleteResult);
				// Clean up the destination file since move failed
				(void)deluge::io::unlink(params.toName);
				errCode = deleteResult;
			}
		}
	}
	else {
		// Rename was successful, set timestamp if provided
		if (params.hasTimestamp()) {
			setFileTimestamp(params.toName, params.date, params.time);
		}
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^move", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
```

- [ ] **Step 13: `parseFileOpParams`**

Replace the return statement (line 873) — `getFromTC()`/`getToTC()` are gone (Step 2):

```cpp
	return !params.fromName.empty() && !params.toName.empty();
```

- [ ] **Step 14: build and test**

```bash
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

`openFile`/`closeFile`/`readBlock`/`writeBlock`/`getDirEntries` still use raw FatFS at this point (Tasks 3/4) — that's expected, both halves of the file must compile together and this task doesn't touch them.

- [ ] **Step 15: commit**

```bash
git add src/deluge/storage/smsysex.h src/deluge/storage/smsysex.cpp
git commit -m "smsysex: migrate the local-handle functions to deluge::io, add wire translators"
```

---

### Task 3: `smsysex.cpp` — the `FILdata` pool (`openFIL`/`closeFIL`/`openFile`/`closeFile`/`readBlock`/`writeBlock`)

**Files:**
- Modify: `src/deluge/storage/smsysex.cpp`

**Interfaces:**
- Consumes: `toWireFresult`, `timestampFromWireDateTime` (Task 2).
- Produces: `FILdata::file` as `std::optional<deluge::io::File>` — Task 4 does not touch this, but any future work on this file must know the pool no longer holds a raw `FIL`.

- [ ] **Step 1: `FILdata` struct**

Replace the struct (lines 36-45):

```cpp
struct FILdata {
	std::string fName;
	uint32_t fileID;
	uint32_t LRUstamp = 0;
	uint32_t fSize = 0;
	uint32_t fPosition = 0; // file offset noted after last read/write operation.
	bool fileOpen = false;
	bool forWrite = false;
	std::optional<deluge::io::File> file;
};
```

- [ ] **Step 2: `openFIL`**

Replace the whole function (lines 137-158). The dead `fsize` out-parameter (never written by the original body — `openFile`'s caller always re-reads `fp->fSize` instead) is dropped.

```cpp
FILdata* smSysex::openFIL(const char* fPath, bool forWrite, FRESULT* eCode) {
	FILdata* fp = findEmptyFIL();
	fp->fName = fPath;
	fp->fileID = FIDcounter++;
	noteFileIdUse(fp);

	auto opened = deluge::io::File::open(fPath, forWrite ? DELUGE_FILE_WRITE_CREATE : DELUGE_FILE_READ);
	*eCode = toWireFresult(opened.has_value() ? deluge::io::Status::OK : opened.error());
	if (!opened.has_value()) {
		return nullptr;
	}
	fp->file = std::move(*opened);
	auto size = fp->file->size();
	fp->fSize = size.has_value() ? *size : 0;
	fp->fileOpen = true;
	fp->forWrite = forWrite;
	fp->fPosition = 0;
	return fp;
}
```

- [ ] **Step 3: `closeFIL`**

Replace the whole function (lines 160-169). `FR_INVALID_OBJECT` has no `deluge::io::Status` equivalent — special-cased directly, matching the Global Constraints pattern.

```cpp
FRESULT smSysex::closeFIL(FILdata* fp) {
	if (fp == nullptr) {
		return FRESULT::FR_INVALID_OBJECT;
	}

	deluge::io::Status status = deluge::io::Status::OK;
	if (fp->file.has_value()) {
		auto result = fp->file->close();
		status = result.has_value() ? deluge::io::Status::OK : result.error();
		fp->file.reset();
	}
	fp->fileOpen = false;
	fp->forWrite = false;
	fp->fSize = 0;
	return toWireFresult(status);
}
```

- [ ] **Step 4: `openFile`**

Replace the whole function (lines 224-277). `forWrite` becomes `bool` (Global Constraints); `date`/`time` route through `timestampFromWireDateTime` for the `createPathDirectories` retry call. The `goto retry` stays — `FRESULT`/`uint32_t` locals are trivial, no RAII hazard.

```cpp
void smSysex::openFile(MIDICable& cable, JsonDeserializer& reader) {
	bool forWrite = false;
	std::string path;
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "write")) {
			forWrite = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}

	reader.match('}');
	bool pathCreateTried = false;
retry:
	FRESULT errCode;
	uint32_t fSize = 0;

	FILdata* fp = openFIL(path.c_str(), forWrite, &errCode);

	if (fp != nullptr) {
		fSize = fp->fSize;
	}
	if (forWrite && !pathCreateTried && errCode == FRESULT::FR_NO_FILE) { // was the path missing?
		createPathDirectories(path, timestampFromWireDateTime(date, time));
		pathCreateTried = true;
		goto retry;
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^open", false, true);
	jWriter.writeAttribute("fid", fp != nullptr ? fp->fileID : 0);
	jWriter.writeAttribute("size", fSize);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}
```

Note the retry condition changed from `errCode == FRESULT::FR_NO_PATH` to `errCode == FRESULT::FR_NO_FILE`: `toWireFresult` always produces `FR_NO_FILE` for `Status::NOT_FOUND` (the documented collapse), so this is the value `openFIL` will actually return for a missing path now — checking the old `FR_NO_PATH` here would silently break the retry-on-missing-path behavior. This is the one place in the whole file where the `NOT_FOUND` collapse has a *behavioral* consequence beyond the wire value, not just a cosmetic one — call it out explicitly in the commit message.

`path` is passed to `createPathDirectories` directly (by reference), matching the original exactly (`createPathDirectories(path, date, time);`) — `createPathDirectories` never mutates the caller's string through the reference, it copies into a local `char working[257]` immediately via `strcpy(working, path.c_str())`, so aliasing is safe.

- [ ] **Step 5: `closeFile`**

Replace the whole function (lines 279-303):

```cpp
void smSysex::closeFile(MIDICable& cable, JsonDeserializer& reader) {
	int32_t fid = 0;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fd = entryForFID(fid);
	FRESULT errCode = closeFIL(fd);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^close", false, true);
	jWriter.writeAttribute("fid", (uint32_t)fid);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}
```

- [ ] **Step 6: `readBlock`**

Replace the whole function (lines 506-598). `FR_NOT_ENABLED` (the "no such fid" case) has no `deluge::io::Status` equivalent — special-cased directly, exactly like `closeFIL`'s `FR_INVALID_OBJECT`, so it survives on the wire with zero drift.

```cpp
void smSysex::readBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	int32_t fid = 0;

	auto repSN = reader.getReplySeqNum();
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fp = entryForFID(fid);
	FRESULT errCode = FRESULT::FR_OK;

	if (fp == nullptr) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	uint8_t* srcAddr = (uint8_t*)addr;
	if (errCode == FRESULT::FR_OK) {
		if (!readBlockBuffer && fp) {
			readBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
		}

		if (readBlockBuffer && fp) {
			noteFileIdUse(fp);
			deluge::io::Status status = deluge::io::Status::OK;
			// If file position requested is not what we expect, seek to requested.
			if (fp->fPosition != addr) {
				auto seeked = fp->file->seek(addr);
				status = seeked.has_value() ? deluge::io::Status::OK : seeked.error();
			}
			if (status == deluge::io::Status::OK) {
				auto result = fp->file->read(std::span{reinterpret_cast<std::byte*>(readBlockBuffer), size});
				if (result.has_value()) {
					uint32_t actuallyRead = static_cast<uint32_t>(result->size());
					size = actuallyRead;
					srcAddr = readBlockBuffer;
					fp->fPosition = addr + actuallyRead;
				}
				else {
					status = result.error();
				}
			}
			else {
				D_PRINTLN("lseek issue: %d", toWireFresult(status));
			}
			errCode = toWireFresult(status);
		}
	}
	else {
		size = 0;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^read", false, true);
	jWriter.writeAttribute("fid", fid);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	jWriter.writeByte(0); // spacer between Json and encoded block.

	uint8_t working[8];
	if (size == 0) {
		D_PRINTLN("Read size 0");
	}
	for (uint32_t ix = 0; ix < size; ix += 7) {
		int pktSize = 7;
		if (ix + pktSize > size) {
			pktSize = size - ix;
		}
		uint8_t hiBits = 0;
		uint8_t rotBit = 1;
		for (int i = 1; i <= pktSize; ++i) {
			working[i] = (*srcAddr) & 0x7F;
			if ((*srcAddr) & 0x80) {
				hiBits |= rotBit;
			}
			srcAddr++;
			rotBit <<= 1;
		}
		working[0] = hiBits;
		jWriter.writeBlock(working, pktSize + 1);
	}
	sendMsg(cable, jWriter);
}
```

- [ ] **Step 7: `writeBlock`**

Replace the whole function (lines 600-664). Same `FR_NOT_ENABLED` special-case as Step 6.

```cpp
void smSysex::writeBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t fileId = 0;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else if (!strcmp(tagName, "fid")) {
			fileId = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	if (!writeBlockBuffer) {
		writeBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
	}
	reader.match('}');
	reader.match('}'); // skip box too.

	char aChar;
	if (reader.peekChar(&aChar) && aChar != 0) {
		D_PRINTLN("Missing Separater error in writeBlock");
	}
	uint32_t decodedSize = decodeDataFromReader(reader, writeBlockBuffer, size);
	D_PRINTLN("Decoded block len: %d", decodedSize);

	FRESULT errCode = FRESULT::FR_OK;
	FILdata* fp = entryForFID(fileId);

	if (fp == nullptr) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	if (writeBlockBuffer && (fp != nullptr)) {
		deluge::io::Status status = deluge::io::Status::OK;
		if (addr != fp->fPosition) {
			auto seeked = fp->file->seek(addr);
			status = seeked.has_value() ? deluge::io::Status::OK : seeked.error();
		}
		if (status == deluge::io::Status::OK) {
			noteFileIdUse(fp);
			auto result =
			    fp->file->write(std::span{reinterpret_cast<const std::byte*>(writeBlockBuffer), decodedSize});
			if (result.has_value()) {
				size = *result;
				fp->fPosition = addr + *result;
			}
			else {
				status = result.error();
				size = 0;
			}
		}
		errCode = toWireFresult(status);
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^write", false, true);
	jWriter.writeAttribute("fid", fileId);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}
```

- [ ] **Step 8: build and test**

```bash
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

- [ ] **Step 9: commit**

```bash
git add src/deluge/storage/smsysex.cpp
git commit -m "smsysex: migrate the FILdata pool (open/close/read/write) to deluge::io"
```

---

### Task 4: `smsysex.cpp` — directory-scan state and `getDirEntries`

**Files:**
- Modify: `src/deluge/storage/smsysex.cpp`

**Interfaces:**
- Consumes: `toWireFresult` (Task 2), extended `DelugeDirEntry` (Task 1).
- Produces: two more local translators (`wireDateFromTimestamp`/`wireTimeFromTimestamp`, `wireAttribFromFlags`), plus `sxDir` as `std::optional<deluge::io::Directory>` replacing the raw `DIR sxDIR` global.

- [ ] **Step 1: replace the `DIR sxDIR` global**

Replace line 25:

```cpp
std::optional<deluge::io::Directory> sxDir;
```

- [ ] **Step 2: add the two remaining local translators**

Add to the same anonymous namespace from Task 2 Step 3, after `timestampFromWireDateTime`:

```cpp
// Inverse of timestampFromWireDateTime — packs a portable DelugeTimestamp
// back into the wire's FAT DOS-packed WORD shape for getDirEntries's reply.
WORD wireDateFromTimestamp(const DelugeTimestamp& ts) {
	return static_cast<WORD>(((ts.year - 1980) << 9) | (ts.month << 5) | ts.day);
}

WORD wireTimeFromTimestamp(const DelugeTimestamp& ts) {
	return static_cast<WORD>((ts.hour << 11) | (ts.minute << 5) | (ts.second / 2));
}

// Packs DelugeDirEntry's portable attribute flags back into the wire's raw
// FatFS fattrib byte shape.
BYTE wireAttribFromFlags(const DelugeDirEntry& entry) {
	BYTE attrib = 0;
	if (entry.is_read_only)
		attrib |= AM_RDO;
	if (entry.is_hidden)
		attrib |= AM_HID;
	if (entry.is_system)
		attrib |= AM_SYS;
	if (entry.is_directory)
		attrib |= AM_DIR;
	if (entry.is_archive)
		attrib |= AM_ARC;
	return attrib;
}
```

These stay file-scope `static`/anonymous-namespace, same as `toWireFresult` — trivial 1:1 mappings, covered via the functional path rather than a dedicated unit-test seam (design doc §4.1).

- [ ] **Step 3: `getDirEntries`**

Replace the whole function (lines 420-504). The end-of-directory bug fix is deliberate and documented inline: the original checked `fno.altname[0] == 0`, which is wrong (`altname` is only populated when a file's real name doesn't fit 8.3, so a long-name-fits-8.3 entry has an empty `altname` and would end the listing early); `deluge::io::Directory::read()`'s `has_entry` is backed by FatFS's real `fname[0]==0` check, so this fixes it as a side effect of the migration. The `sxDir.has_value()` guard in the second loop is a **necessary safety addition**, not a behavior change: the original's raw `DIR` tolerated being read while never successfully opened (an early `f_readdir` on an unopened `DIR` just returns an error), but calling `->read()` on an empty `std::optional` is undefined behavior — the guard prevents that on the `goto errorFound` path when `sxDir` was never previously assigned.

```cpp
void smSysex::getDirEntries(MIDICable& cable, JsonDeserializer& reader) {
	std::string path;
	path = "/";
	uint32_t lineOffset = 0;
	uint32_t linesWanted = 20;

	FRESULT errCode = FRESULT::FR_OK;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "offset")) {
			lineOffset = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "lines")) {
			linesWanted = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');
	if (linesWanted > MAX_DIR_LINES)
		linesWanted = MAX_DIR_LINES;
	// We should pick up on path changes and out-of-order offset requests.

	if (lineOffset == 0 || activeDirName != path || lineOffset != dirOffsetCounter) {
		auto opened = deluge::io::Directory::open(path);
		if (!opened.has_value()) {
			errCode = toWireFresult(opened.error());
			goto errorFound;
		}
		sxDir = std::move(*opened);
		dirOffsetCounter = 0;
		activeDirName = path;
		if (lineOffset > 0) {
			for (uint32_t ix = 0; ix < lineOffset; ++ix) {
				auto entry = sxDir->read();
				if (!entry.has_value()) {
					errCode = toWireFresult(entry.error());
					break;
				}
				if (!entry->has_value()) {
					break;
				}
				dirOffsetCounter++;
			}
		}
	}
errorFound:;
	jWriter.reset();
	jWriter.setMemoryBased();
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^dir", false, true);
	jWriter.writeArrayStart("list", true, false);

	for (uint32_t ix = 0; ix < linesWanted && sxDir.has_value(); ++ix) {
		auto entry = sxDir->read();
		if (!entry.has_value() || !entry->has_value()) {
			break;
		}
		const DelugeDirEntry& fno = **entry;

		jWriter.writeOpeningTag(NULL, true);
		jWriter.writeAttribute("name", fno.name);
		jWriter.writeAttribute("size", fno.size);
		jWriter.writeAttribute("date", wireDateFromTimestamp(fno.modified_time));
		jWriter.writeAttribute("time", wireTimeFromTimestamp(fno.modified_time));

		// AM_RDO  0x01 Read only
		// AM_HID  0x02 Hidden
		// AM_SYS  0x04 System

		// AM_DIR  0x10 Directory
		// AM_ARC  0x20 Archive
		jWriter.writeAttribute("attr", wireAttribFromFlags(fno));

		jWriter.closeTag();
		dirOffsetCounter++;
	}
	jWriter.writeArrayEnding("list", true, false);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
```

- [ ] **Step 4: build and test**

```bash
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Confirm no FatFS symbols remain in this file:

```bash
grep -n "f_open\|f_read\|f_write\|f_lseek\|f_close\|f_mkdir\|f_unlink\|f_rename\|f_opendir\|f_readdir\|f_closedir\|FIL \|DIR \|FILINFO" src/deluge/storage/smsysex.cpp
```

Expected: no matches for actual FatFS calls/types (the string `FRESULT` itself is expected to remain throughout — it's the frozen wire type, not a FatFS call).

- [ ] **Step 5: commit**

```bash
git add src/deluge/storage/smsysex.cpp
git commit -m "smsysex: migrate directory-scan state and getDirEntries to deluge::io

Fixes a latent end-of-directory bug as a side effect: the previous
fno.altname[0]==0 check missed entries whose long name already fits
8.3 (altname is only populated as a short-name fallback); the boundary's
has_entry signal is backed by the correct fname[0]==0 check."
```

---

### Task 5: Full regression check and manual smoke-test gate

**Files:** none (verification only)

- [ ] **Step 1: full rza1 build**

```bash
./dbt build Debug
```

Expected: clean build, no new warnings.

- [ ] **Step 2: full host/sim build + golden-master**

```bash
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: clean build, `PASS — MIXDOWN matches golden`. `smsysex.cpp` is not on the render path — this is a no-op signal for this plan specifically, but confirms nothing else broke.

- [ ] **Step 3: full spec suite**

```bash
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Expected: 100% pass, including every case added in Task 1.

- [ ] **Step 4: confirm no leftover FatFS call sites**

```bash
grep -n "f_open\|f_read\|f_write\|f_lseek\|f_mkdir\|f_unlink\|f_rename\|f_opendir\|f_readdir\|f_closedir\|f_utime" src/deluge/storage/smsysex.cpp
```

Expected: no matches.

- [ ] **Step 5: record the outstanding manual-smoke gate**

`smsysex.cpp` has no automated wire-level test (Global Constraints). Add one line to `TODO.md`, matching the style of the existing Tier 2/3/4 entries this plan resolves — replace the Tier 4 entry (the one referencing `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md §5`) with:

```markdown
- [] smsysex.cpp manual smoke-test gate: Tier 4 of the file_io.h migration (docs/superpowers/specs/2026-07-14-file-io-migration-tier4-smsysex-design.md) is implemented and unit-tested, but has no automated wire-level test (none existed before this migration either). Before the next companion-app-facing release, do a manual pass — connect the companion app (or replay a raw SysEx dump) and exercise open/read/write/close/mkdir/rename/delete/getDirEntries/updateTime against a real card, confirming the wire's err/date/time/attr/size values are byte-identical to pre-migration behavior.
```

- [ ] **Step 6: commit**

```bash
git add TODO.md
git commit -m "docs: replace the Tier 4 TODO with its manual smoke-test gate"
```
