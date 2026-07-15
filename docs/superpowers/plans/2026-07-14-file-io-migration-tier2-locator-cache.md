# File-IO Migration Tier 2 (Adapter Directory Cache) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the single-slot "last-scanned-directory" cache inside the FatFS adapter, close the stale-cache hazard Task 3's own review found in five call sites that bypass `file_io.h` (Task 4, added mid-execution — see that task's own explanation), and migrate `instrument_clip_view.cpp`'s directory scan to `deluge::io::Directory`. This is Tier 2's *entire* real scope — see the design doc's scope note for why `browser.cpp`/`sample_browser.cpp`/`audio_file_manager.cpp` are explicitly excluded.

**Architecture:** `FatFS::File` gains a fast, non-I/O `open_by_locator` factory (mirrors `StorageManager::openFilePointer`'s existing raw-FIL-construction contract, moved to where FatFS internals belong). The adapter (`src/fatfs/file_io.cpp`) gains a bounded, single-slot cache populated as a side effect of normal directory iteration and consulted by `deluge_file_open` for read opens, invalidated wholesale by any write-shaped call.

**Tech Stack:** C++23, real FatFS engine (`src/fatfs/`), `deluge::io` (`src/deluge/io/file.hpp`), cppspec (`tests/spec/file_io_spec.cpp`).

**Design doc:** `docs/superpowers/specs/2026-07-14-file-io-migration-tier2-locator-cache-design.md`

## Global Constraints

- **No new dependency on the `fatfs` CMake target.** `src/fatfs/CMakeLists.txt` is auto-generated (`dbt buildgen`) and doesn't currently link `etl`. The cache uses plain fixed-size C arrays (`char[]`, not `etl::string`/`etl::vector`) — a deliberate, conservative choice made while investigating this plan, not what the design doc's example code literally showed.
- **`tests/spec/`'s `deluge_spec`/`all_specs` target has no real mountable filesystem.** `tests/32bit_unit_tests/mocks/mock_diskio.cpp` is a pure "no disk" stub (`disk_status` always reports `STA_NOINIT`) — it exists only to satisfy the linker (`file_io.cpp` links the real FatFS engine). Every existing case in `tests/spec/file_io_spec.cpp` is a hand-constructed-input unit test (a `FILINFO` built by hand, not read from a real directory); this plan's tests must follow the same pattern — no test may assume `f_open`/`f_opendir` succeed against a real path. `FatFS::File::open_by_locator` (Task 1) and the cache's fast-path branch in `deluge_file_open` (Task 3) are testable anyway because neither one calls `f_open`/`f_opendir` at all — that's the whole point of the fast path.
- **`FatFS::File::inner()` already exists** (`fatfs.hpp:119`, mirrors `Directory::inner()`) — no need to add it, just use it for testing.
- **`open_by_locator` zero-initializes before setting fields**, explicitly (`file.file_ = {};`), not relying on `File file{};`'s own value-initialization semantics — safer and unambiguous for a reader, even if technically redundant.
- **Coarse invalidation, unconditionally, before the operation runs**: `deluge_file_mkdir`/`unlink`/`rename`, and `deluge_file_open`'s `DELUGE_FILE_WRITE_CREATE` branch, all call `dir_cache_invalidate()` as their first line. No attempt to reason about whether a given write actually touches the cached directory.
- **The fast path is consulted only for `DELUGE_FILE_READ`.**
- **`deluge_dir_read`'s underlying FatFS call changes from `Directory::read()` (`f_readdir`) to `Directory::read_and_get_filepointer()` (`f_readdir_get_filepointer`)** to obtain the `sclust`/`objsize` needed for cache population. This is a behavior-preserving swap for every existing caller (Tiers 1/4's already-migrated code): `f_readdir_get_filepointer` populates the same `FILINFO` `f_readdir` does, plus the extra `FilePointer` extraction — `browser.cpp`'s pre-existing `readFileItemsForFolder` already calls this exact function today via `staticDIR.read_and_get_filepointer()`, so its `FILINFO` output is already proven identical in shipped code.
- **Cache entries carry `fs`/`id` (the owning `FATFS*` and its mount-generation id), not just `sclust`/`objsize`.** Sourced from the currently-open `Directory`'s own `inner().obj.fs`/`.id` at scan time — **not** from the app-layer `extern FatFS::Filesystem fileSystem` global (`storage_manager.h:36`). Referencing that global from `src/fatfs/` would be a real layering violation (a "library" reaching up into "app" headers) — exactly the kind of inversion this whole migration exists to remove. `Directory::inner()` already gives safe, correctly-layered access to the same information.
- **`active_handle` correlation is a deliberate, small addition beyond the design doc's original sketch.** The design doc's traced call sites confirm this app never has two directory scans in flight at once *today*, but nothing enforces that invariant. Tagging each cache generation with the `DelugeDir*` that's populating it, and having `dir_cache_append` no-op if a different handle is now active, turns "two concurrent scans" from a silent cache-corruption risk (attributing one directory's filenames to another's path) into a safe no-op (the second scan's entries just don't get cached) — a genuinely justified robustness improvement, not scope creep.
- New code is idiomatic C++23 (enum class, `std::string_view`, no unnecessary C-casts).
- Never use `git commit --amend` after a clang-format pre-commit hook failure — create a fresh commit instead.

---

### Task 1: `FatFS::File::open_by_locator`

**Files:**
- Modify: `src/fatfs/fatfs.hpp`
- Modify: `src/fatfs/fatfs.cpp`
- Modify: `tests/spec/file_io_spec.cpp`

**Interfaces:**
- Produces: `FatFS::File::open_by_locator(FATFS* fs, WORD id, DWORD sclust, FSIZE_t objsize) -> File` (cannot fail — pure field construction, no I/O; matches `StorageManager::openFilePointer`'s existing "Function can't fail" contract). Consumed by Task 3's `deluge_file_open` fast path.

- [ ] **Step 1: declare `open_by_locator`**

`src/fatfs/fatfs.hpp` — add to `class File`'s public section, right after `open` (after line 63):

```cpp
  /* Open a file directly via a previously-resolved locator (starting
     cluster + size), skipping the directory-tree walk f_open performs
     internally. Cannot fail -- pure field construction, no I/O. Always
     read-only (FA_READ); the caller must already know this file exists and
     where it starts (e.g. from a prior directory scan). */
  static File open_by_locator(FATFS *fs, WORD id, DWORD sclust, FSIZE_t objsize);
```

(Matches this file's existing 2-space indentation, not the project's usual tabs — `fatfs.hpp`/`fatfs.cpp` are formatted this way throughout, an established exception per Tier 4's Task 1.)

- [ ] **Step 2: implement it**

`src/fatfs/fatfs.cpp` — add after the existing `File::open` implementation (find it via `grep -n "std::expected<File, Error> File::open" src/fatfs/fatfs.cpp` and insert right after that function's closing brace):

```cpp
File File::open_by_locator(FATFS *fs, WORD id, DWORD sclust, FSIZE_t objsize) {
  File file{};
  file.file_ = {}; // zero every field first -- File's own file_ member has no
                    // in-class initializer (unlike FileReader::readFIL{}, whose
                    // starting state this mirrors), so this isn't redundant.
  file.file_.obj.fs = fs;
  file.file_.obj.id = id;
  file.file_.obj.sclust = sclust;
  file.file_.obj.objsize = objsize;
  file.file_.flag = FA_READ;
  file.file_.err = 0;
  file.file_.sect = 0;
  file.file_.fptr = 0;
  return file;
}
```

- [ ] **Step 3: test it**

`tests/spec/file_io_spec.cpp` — add before the closing `});` of the `file_io` describe block:

```cpp
	it("open_by_locator constructs a File with exactly the given locator fields, matching openFilePointer's contract", _ {
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 42, 100, 5000);
		expect(file.inner().obj.fs).to_equal(&fakeFs);
		expect(file.inner().obj.id).to_equal(static_cast<WORD>(42));
		expect(file.inner().obj.sclust).to_equal(static_cast<DWORD>(100));
		expect(file.inner().obj.objsize).to_equal(static_cast<FSIZE_t>(5000));
		expect(file.inner().flag).to_equal(static_cast<BYTE>(FA_READ));
		expect(file.inner().err).to_equal(static_cast<BYTE>(0));
		expect(file.inner().sect).to_equal(static_cast<DWORD>(0));
		expect(file.inner().fptr).to_equal(static_cast<FSIZE_t>(0));
		expect(file.size()).to_equal(5000u);
		expect(file.tell()).to_equal(0u);

		// fakeFs is zero-initialized (fs_type == 0), so FatFS's own validate()
		// safely rejects it as FR_INVALID_OBJECT on close -- confirms this
		// doesn't crash against an object that was never really f_open'd. This
		// test target has no real disk backing (see this file's scope note),
		// so genuine I/O was never on the table anyway -- this only verifies
		// the field construction itself.
		auto closed = file.close();
		expect(closed.has_value()).to_equal(false);
		expect(closed.error()).to_equal(FatFS::Error::INVALID_OBJECT);
	});
```

- [ ] **Step 4: build and test**

```bash
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: all specs pass (including the new case), both builds clean, golden-master bit-exact (this task adds a new, currently-unused function — no existing code calls it yet).

- [ ] **Step 5: commit**

```bash
git add src/fatfs/fatfs.hpp src/fatfs/fatfs.cpp tests/spec/file_io_spec.cpp
git commit -m "fatfs: add File::open_by_locator, a non-I/O fast-reopen constructor"
```

---

### Task 2: the adapter cache — struct and population (additive, no behavior change yet)

**Files:**
- Modify: `src/fatfs/file_io_internal.hpp`
- Modify: `src/fatfs/file_io.cpp`
- Modify: `tests/spec/file_io_spec.cpp`

**Interfaces:**
- Produces: `deluge::fatfs_adapter::{DirCache, DirCacheEntry, g_dir_cache, g_dir_cache_hits, g_dir_cache_misses, dir_cache_reset_for_test, dir_cache_begin, dir_cache_append, dir_cache_lookup, dir_cache_invalidate}`. Task 3 consumes `dir_cache_lookup`/`dir_cache_invalidate`/the hit-miss counters; this task only wires `dir_cache_begin`/`dir_cache_append` (population), leaving `deluge_file_open` untouched.
- This task deliberately does **not** change `deluge_file_open`'s behavior at all — `dir_cache_lookup` exists and is tested directly, but nothing calls it yet from the extern "C" layer. That's Task 3.

- [ ] **Step 1: declare the cache**

`src/fatfs/file_io_internal.hpp` — add to the `deluge::fatfs_adapter` namespace, after the existing `to_dir_entry` declaration (after line 31):

```cpp
constexpr size_t kDirCacheCapacity = 64;                       // generous headroom over browser.cpp's 20-entry nav batch
constexpr size_t kDirCacheMaxPathLength = DELUGE_MAX_FILENAME; // reuses the boundary's filename-length bound

struct DirCacheEntry {
	char name[DELUGE_MAX_FILENAME];
	FATFS* fs;
	WORD id;
	DWORD sclust;
	FSIZE_t objsize;
};

/// Adapter-internal, single-slot, wholesale-replaced "last-scanned-directory"
/// cache. Not exposed via file_io.h -- see
/// docs/superpowers/specs/2026-07-14-file-io-migration-tier2-locator-cache-design.md.
struct DirCache {
	char directory_path[kDirCacheMaxPathLength];
	size_t directory_path_length = 0;
	DirCacheEntry entries[kDirCacheCapacity];
	size_t entry_count = 0;
	bool valid = false;
	const void* active_handle = nullptr; // the DelugeDir* currently populating this cache generation
};

/// The single adapter-wide cache instance. A real (non-static) definition so
/// tests can inspect it directly via this header.
extern DirCache g_dir_cache;

/// Diagnostic counters, incremented at deluge_file_open's two branch points
/// (Task 3). Not part of file_io.h's public surface -- inspectable by tests
/// only.
extern size_t g_dir_cache_hits;
extern size_t g_dir_cache_misses;

/// Resets g_dir_cache and the hit/miss counters to their initial empty
/// state. Test-only -- production code never needs to do this.
void dir_cache_reset_for_test();

/// Starts a fresh cache generation for `path`, tagged to `handle` (normally
/// the DelugeDir* the scan belongs to). Wholesale-replaces any prior cache.
/// If `path` doesn't fit in kDirCacheMaxPathLength, leaves the cache invalid
/// (graceful degradation -- this one scan just isn't cached).
void dir_cache_begin(std::string_view path, const void* handle);

/// Appends one entry to the cache. No-ops silently if `handle` isn't the
/// active scan, the cache isn't valid, or the cache is already at
/// kDirCacheCapacity (graceful degradation for oversized folders).
void dir_cache_append(const void* handle, std::string_view name, FATFS* fs, WORD id, DWORD sclust, FSIZE_t objsize);

/// Splits `path` into (dirname, basename) and returns the matching cached
/// entry, or nullptr if the cache is invalid, the directory doesn't match,
/// or the basename isn't among its entries.
const DirCacheEntry* dir_cache_lookup(std::string_view path);

/// Unconditionally invalidates the cache. Called from every write-shaped
/// file_io.h function.
void dir_cache_invalidate();
```

- [ ] **Step 2: implement the cache functions**

`src/fatfs/file_io.cpp` — add to the `deluge::fatfs_adapter` namespace, after `to_dir_entry` (after line 78, before the namespace's closing brace):

```cpp
DirCache g_dir_cache{};
size_t g_dir_cache_hits = 0;
size_t g_dir_cache_misses = 0;

void dir_cache_reset_for_test() {
	g_dir_cache = DirCache{};
	g_dir_cache_hits = 0;
	g_dir_cache_misses = 0;
}

void dir_cache_begin(std::string_view path, const void* handle) {
	g_dir_cache.entry_count = 0;
	g_dir_cache.active_handle = handle;
	if (path.size() >= kDirCacheMaxPathLength) {
		g_dir_cache.valid = false;
		return;
	}
	std::memcpy(g_dir_cache.directory_path, path.data(), path.size());
	g_dir_cache.directory_path[path.size()] = 0;
	g_dir_cache.directory_path_length = path.size();
	g_dir_cache.valid = true;
}

void dir_cache_append(const void* handle, std::string_view name, FATFS* fs, WORD id, DWORD sclust, FSIZE_t objsize) {
	if (!g_dir_cache.valid || g_dir_cache.active_handle != handle) {
		return;
	}
	if (g_dir_cache.entry_count >= kDirCacheCapacity) {
		return;
	}
	DirCacheEntry& entry = g_dir_cache.entries[g_dir_cache.entry_count];
	size_t copy_length = name.size() < DELUGE_MAX_FILENAME - 1 ? name.size() : DELUGE_MAX_FILENAME - 1;
	std::memcpy(entry.name, name.data(), copy_length);
	entry.name[copy_length] = 0;
	entry.fs = fs;
	entry.id = id;
	entry.sclust = sclust;
	entry.objsize = objsize;
	g_dir_cache.entry_count++;
}

const DirCacheEntry* dir_cache_lookup(std::string_view path) {
	if (!g_dir_cache.valid) {
		return nullptr;
	}
	size_t slash = path.rfind('/');
	std::string_view dirname = slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
	std::string_view basename = slash == std::string_view::npos ? path : path.substr(slash + 1);
	if (dirname != std::string_view{g_dir_cache.directory_path, g_dir_cache.directory_path_length}) {
		return nullptr;
	}
	for (size_t i = 0; i < g_dir_cache.entry_count; i++) {
		if (basename == std::string_view{g_dir_cache.entries[i].name}) {
			return &g_dir_cache.entries[i];
		}
	}
	return nullptr;
}

void dir_cache_invalidate() {
	g_dir_cache.valid = false;
	g_dir_cache.entry_count = 0;
	g_dir_cache.active_handle = nullptr;
}
```

- [ ] **Step 3: wire population into `deluge_dir_open`/`deluge_dir_read`**

`src/fatfs/file_io.cpp` — replace `deluge_dir_open` (lines 153-160):

```cpp
DelugeStatus deluge_dir_open(const char* path, DelugeDir** out) {
	auto opened = FatFS::Directory::open(path);
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeDir*>(new FatFS::Directory(std::move(opened.value())));
	deluge::fatfs_adapter::dir_cache_begin(path, *out);
	return DELUGE_OK;
}
```

Replace `deluge_dir_read` (lines 162-171):

```cpp
DelugeStatus deluge_dir_read(DelugeDir* dir, DelugeDirEntry* out, bool* out_has_entry) {
	auto* d = reinterpret_cast<FatFS::Directory*>(dir);
	auto result = d->read_and_get_filepointer();
	if (!result) {
		*out_has_entry = false;
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	const auto& [info, file_pointer] = *result;
	deluge::fatfs_adapter::to_dir_entry(info, *out, *out_has_entry);
	if (*out_has_entry) {
		deluge::fatfs_adapter::dir_cache_append(dir, out->name, d->inner().obj.fs, d->inner().obj.id,
		                                        file_pointer.sclust, file_pointer.objsize);
	}
	return DELUGE_OK;
}
```

`deluge_dir_close` is unchanged — the cache deliberately outlives the `DelugeDir` handle (§4 of the design doc).

- [ ] **Step 4: test the pure cache functions directly**

`tests/spec/file_io_spec.cpp` — add before the closing `});`. These test `dir_cache_*` directly with hand-constructed values (matching this file's established no-real-I/O style), not through `deluge_dir_open`/`deluge_dir_read` (which need a real mounted filesystem this test target doesn't have):

```cpp
	it("dir_cache_begin+append populates entries findable by dir_cache_lookup", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG2.XML", &fakeFs, 7, 200, 3000);

		const auto* entry = deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG2.XML");
		expect(entry != nullptr).to_equal(true);
		expect(entry->fs).to_equal(&fakeFs);
		expect(entry->id).to_equal(static_cast<WORD>(7));
		expect(entry->sclust).to_equal(static_cast<DWORD>(200));
		expect(entry->objsize).to_equal(static_cast<FSIZE_t>(3000));
	});

	it("dir_cache_lookup misses on a directory that doesn't match", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SAMPLES/SONG1.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup misses on a filename that isn't cached", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG_MISSING.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_append no-ops if handle doesn't match the active scan", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handleA = reinterpret_cast<const void*>(0x1000);
		const void* handleB = reinterpret_cast<const void*>(0x2000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handleA);
		deluge::fatfs_adapter::dir_cache_append(handleB, "INTRUDER.XML", &fakeFs, 7, 999, 999);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/INTRUDER.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup returns nullptr when the cache was never populated", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG1.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup handles a root-level (no-slash) path", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "ROOT.XML", &fakeFs, 3, 50, 500);

		const auto* entry = deluge::fatfs_adapter::dir_cache_lookup("ROOT.XML");
		expect(entry != nullptr).to_equal(true);
		expect(entry->sclust).to_equal(static_cast<DWORD>(50));
	});

	it("dir_cache_append silently drops entries beyond kDirCacheCapacity", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("BIGDIR", handle);
		char name[16];
		for (size_t i = 0; i < deluge::fatfs_adapter::kDirCacheCapacity + 5; i++) {
			std::snprintf(name, sizeof(name), "F%zu.WAV", i);
			deluge::fatfs_adapter::dir_cache_append(handle, name, &fakeFs, 1, static_cast<DWORD>(i + 100), 10);
		}
		expect(deluge::fatfs_adapter::g_dir_cache.entry_count).to_equal(deluge::fatfs_adapter::kDirCacheCapacity);

		// The first kDirCacheCapacity entries are still found; nothing crashed on overflow.
		const auto* first = deluge::fatfs_adapter::dir_cache_lookup("BIGDIR/F0.WAV");
		expect(first != nullptr).to_equal(true);
	});
```

- [ ] **Step 5: build and test**

```bash
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: all specs pass, both builds clean, golden-master bit-exact (`deluge_dir_open`/`deluge_dir_read` produce identical `DelugeDirEntry`/`has_entry` output for all existing callers — cache population is a pure side effect, and `f_readdir_get_filepointer` is already proven behavior-identical to `f_readdir` in shipped code via `browser.cpp`).

- [ ] **Step 6: commit**

```bash
git add src/fatfs/file_io_internal.hpp src/fatfs/file_io.cpp tests/spec/file_io_spec.cpp
git commit -m "file_io: add the adapter directory cache (population only, no consultation yet)"
```

---

### Task 3: consult the cache in `deluge_file_open`, invalidate on writes

**Files:**
- Modify: `src/fatfs/file_io.cpp`
- Modify: `tests/spec/file_io_spec.cpp`

**Interfaces:**
- Consumes: `dir_cache_lookup`, `dir_cache_invalidate`, `g_dir_cache_hits`/`g_dir_cache_misses` (Task 2), `FatFS::File::open_by_locator` (Task 1).

- [ ] **Step 1: `deluge_file_open`'s fast path**

`src/fatfs/file_io.cpp` — replace `deluge_file_open` (lines 84-91):

```cpp
DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out) {
	if (mode == DELUGE_FILE_READ) {
		if (const auto* entry = deluge::fatfs_adapter::dir_cache_lookup(path)) {
			deluge::fatfs_adapter::g_dir_cache_hits++;
			auto file = FatFS::File::open_by_locator(entry->fs, entry->id, entry->sclust, entry->objsize);
			*out = reinterpret_cast<DelugeFile*>(new FatFS::File(std::move(file)));
			return DELUGE_OK;
		}
		deluge::fatfs_adapter::g_dir_cache_misses++;
	}
	else {
		deluge::fatfs_adapter::dir_cache_invalidate();
	}
	auto opened = FatFS::File::open(path, deluge::fatfs_adapter::to_fatfs_mode(mode));
	if (!opened) {
		return deluge::fatfs_adapter::to_deluge_status(opened.error());
	}
	*out = reinterpret_cast<DelugeFile*>(new FatFS::File(std::move(opened.value())));
	return DELUGE_OK;
}
```

- [ ] **Step 2: invalidate on the other write-shaped calls**

`src/fatfs/file_io.cpp` — add one line to the start of each of `deluge_file_mkdir`, `deluge_file_unlink`, `deluge_file_rename` (their bodies otherwise unchanged):

```cpp
DelugeStatus deluge_file_mkdir(const char* path) {
	deluge::fatfs_adapter::dir_cache_invalidate();
	auto result = FatFS::mkdir(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_unlink(const char* path) {
	deluge::fatfs_adapter::dir_cache_invalidate();
	auto result = FatFS::unlink(path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_file_rename(const char* old_path, const char* new_path) {
	deluge::fatfs_adapter::dir_cache_invalidate();
	auto result = FatFS::rename(old_path, new_path);
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}
```

- [ ] **Step 3: test the fast path, the miss path, and invalidation**

`tests/spec/file_io_spec.cpp` — add before the closing `});`. The hit-path test genuinely runs end-to-end through `deluge_file_open`'s real C-ABI function (not just the pure `dir_cache_*` helpers) because the fast path never calls `f_open`/`f_opendir` — it's pure construction via `open_by_locator`, so it works with no real disk backing:

```cpp
	it("deluge_file_open takes the fast path on a cache hit", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		size_t hits_before = deluge::fatfs_adapter::g_dir_cache_hits;

		DelugeFile* file = nullptr;
		DelugeStatus status = deluge_file_open("SONGS/SONG1.XML", DELUGE_FILE_READ, &file);
		expect(status).to_equal(DELUGE_OK);
		expect(deluge::fatfs_adapter::g_dir_cache_hits).to_equal(hits_before + 1);

		uint32_t size = 0;
		expect(deluge_file_size(file, &size)).to_equal(DELUGE_OK);
		expect(size).to_equal(2000u);

		// fakeFs isn't really mounted, so closing correctly reports an error
		// rather than crashing -- same reasoning as Task 1's open_by_locator test.
		DelugeStatus closeStatus = deluge_file_close(file);
		expect(closeStatus).to_equal(DELUGE_ERR_IO);
	});

	it("deluge_file_open falls through to the normal path on a cache miss", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		size_t misses_before = deluge::fatfs_adapter::g_dir_cache_misses;

		DelugeFile* file = nullptr;
		// No real mounted disk in this test target, so this will fail --
		// what matters is that it took the miss path (counter incremented)
		// and returned a defined error rather than crashing.
		DelugeStatus status = deluge_file_open("NOWHERE/MISSING.XML", DELUGE_FILE_READ, &file);
		expect(status != DELUGE_OK).to_equal(true);
		expect(deluge::fatfs_adapter::g_dir_cache_misses).to_equal(misses_before + 1);
	});

	it("a write-create open invalidates the cache", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(true);

		DelugeFile* file = nullptr;
		(void)deluge_file_open("SONGS/NEWFILE.XML", DELUGE_FILE_WRITE_CREATE, &file);
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);
	});

	it("mkdir/unlink/rename each invalidate the cache", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_mkdir("SONGS/NEWDIR");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_unlink("SONGS/SONG1.XML");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_rename("SONGS/SONG1.XML", "SONGS/SONG2.XML");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);
	});
```

- [ ] **Step 4: build and test**

```bash
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: all specs pass, both builds clean, golden-master bit-exact. Every already-migrated call site (Tiers 1/4) that opens a file for reading takes the miss path exactly as before *unless* it happens to follow a scan of the same directory — same observable result either way, just sometimes faster.

- [ ] **Step 5: commit**

```bash
git add src/fatfs/file_io.cpp tests/spec/file_io_spec.cpp
git commit -m "file_io: consult the adapter directory cache in deluge_file_open, invalidate on writes"
```

---

### Task 4: close the stale-cache hazard from raw-FatFS writers outside `file_io.h`

**Why this task exists — read before starting:** Task 3's own review found a real, live wrong-file-open hazard, not anticipated when this plan was written. `smsysex.cpp` (migrated in an earlier, separate plan) already lets a companion app list a directory through `file_io.h` — which now populates the Task 2/3 cache — and later reopen a file from it by path, benefiting from Task 3's fast path. But five files elsewhere in the app mutate the filesystem via **raw FatFS calls that bypass `file_io.h` entirely** (not even through the `FatFS::` C++ wrapper) and never invalidate the cache. If one of them deletes, renames, or creates a file in a directory the cache has stashed, a subsequent SysEx-driven open can silently return stale or wrong-file data. Before Task 3, the cache was inert (populated but never consulted) — this gap was latent. Task 3 made it live. This task closes it: a minimal safety-net call, not a migration of these five files to `file_io.h` (that's separate, larger work, out of scope here).

**Files:**
- Modify: `include/libdeluge/file_io.h`
- Modify: `src/fatfs/file_io.cpp`
- Modify: `src/deluge/gui/context_menu/delete_file.cpp`
- Modify: `src/deluge/gui/ui/save/save_song_ui.cpp`
- Modify: `src/deluge/model/sample/sample_recorder.cpp`
- Modify: `src/deluge/processing/stem_export/stem_export.cpp`
- Modify: `src/deluge/deluge.cpp`

**Interfaces:**
- Produces: `deluge_file_invalidate_cache(void)` — a new, minimal, public `file_io.h` boundary function. Wraps the already-existing `deluge::fatfs_adapter::dir_cache_invalidate()` (Task 2) so app code can trigger invalidation without reaching into the adapter's internal namespace (`file_io_internal.hpp` is not meant to be included outside `src/fatfs/`).

- [ ] **Step 1: add the boundary function**

`include/libdeluge/file_io.h` — add after `deluge_file_rename`'s declaration (the last function in the file, before the closing `#ifdef __cplusplus`/`}`/`#endif` block):

```c
/// Invalidates any internal caching this boundary maintains for directory
/// contents. Call this after performing a filesystem write through some
/// mechanism OTHER than this boundary's own write functions (mkdir/unlink/
/// rename/write-create open, which already invalidate internally) -- e.g.
/// legacy code that still calls the underlying filesystem library directly.
/// Cannot fail. New code should prefer this boundary's own write functions,
/// which need no separate call. [task]
void deluge_file_invalidate_cache(void);
```

- [ ] **Step 2: implement it**

`src/fatfs/file_io.cpp` — add to the `extern "C"` block, after `deluge_file_rename` (the last function before the block's closing brace):

```cpp
void deluge_file_invalidate_cache(void) {
	deluge::fatfs_adapter::dir_cache_invalidate();
}
```

- [ ] **Step 3: call it from the five bypass sites**

For each site below: add `#include "libdeluge/file_io.h"` (as the file's second `#include`, right after its own primary header — clang-format will settle final ordering on commit) if not already present, and add `deluge_file_invalidate_cache();` as the line immediately before the raw FatFS write call, matching Task 3's established "invalidate first, unconditionally, before the operation" convention.

**`src/deluge/gui/context_menu/delete_file.cpp`** — add the include after line 18 (`#include "gui/context_menu/delete_file.h"`):
```cpp
#include "libdeluge/file_io.h"
```
Then at line 63, before `FRESULT result = f_unlink(filePath.c_str());`:
```cpp
		deluge_file_invalidate_cache();
		FRESULT result = f_unlink(filePath.c_str());
```

**`src/deluge/gui/ui/save/save_song_ui.cpp`** — add the include after line 17 (`#include "gui/ui/save/save_song_ui.h"`):
```cpp
#include "libdeluge/file_io.h"
```
Then at line 187, before the `f_rename` call:
```cpp
					deluge_file_invalidate_cache();
					FRESULT result = f_rename(sample.tempFilePathForRecording.c_str(), audioFile->filePath.c_str());
```
Then at lines 474/482 (the save-overwrite sequence — both calls need their own invalidation, since each is an independent write):
```cpp
		// Delete the old file
		deluge_file_invalidate_cache();
		FRESULT result = f_unlink(filePath.c_str());
		if (result != FR_OK) {
cardError:
			error = fresultToDelugeErrorCode(result);
			goto gotError;
		}

		// Rename the new file
		deluge_file_invalidate_cache();
		result = f_rename(filePathDuringWrite.c_str(), filePath.c_str());
		if (result != FR_OK) {
			goto cardError;
		}
```

**`src/deluge/model/sample/sample_recorder.cpp`** — add the include after line 18 (`#include "model/sample/sample_recorder.h"`):
```cpp
#include "libdeluge/file_io.h"
```
Then at line 387, before the `f_unlink` call:
```cpp
			deluge_file_invalidate_cache();
			FRESULT result = f_unlink(filePathCreated.c_str());
```

**`src/deluge/processing/stem_export/stem_export.cpp`** — this file already includes `fatfs/ff.h` directly (line 21); add the new include right after it:
```cpp
#include "libdeluge/file_io.h"
```
Then at each of the three `f_mkdir` call sites (lines 923, 946, 995):
```cpp
	// try to create the STEMS folder if it doesn't exist
	deluge_file_invalidate_cache();
	FRESULT result = f_mkdir(tempPath.c_str());
```
```cpp
	// try to create folder
	deluge_file_invalidate_cache();
	result = f_mkdir(tempPath.c_str());
```
```cpp
			// try to create folder
			deluge_file_invalidate_cache();
			result = f_mkdir(tempPathForSearch.c_str());
```

**`src/deluge/deluge.cpp`** — add the include after line 18 (`#include "deluge.h"`):
```cpp
#include "libdeluge/file_io.h"
```
Then at each of the three `f_unlink(failSafePath.c_str());` call sites (lines 451, 463, 482), replace with:
```cpp
					deluge_file_invalidate_cache();
					f_unlink(failSafePath.c_str());
```
(matching each occurrence's own existing indentation level — the three sites are at different nesting depths in the surrounding `if`/`else`/`switch`; match what's already there, don't force identical indentation across all three).

- [ ] **Step 4: build and test**

```bash
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Expected: all clean, golden-master bit-exact (none of these five call sites are on the render path; `deluge_file_invalidate_cache()` is a pure state-reset with no I/O, matching `dir_cache_invalidate`'s existing behavior).

Confirm none of the five call sites were missed:

```bash
grep -n "f_unlink\|f_rename\|f_mkdir" src/deluge/gui/context_menu/delete_file.cpp src/deluge/gui/ui/save/save_song_ui.cpp src/deluge/model/sample/sample_recorder.cpp src/deluge/processing/stem_export/stem_export.cpp src/deluge/deluge.cpp
```

For every line this prints, confirm (by reading the surrounding context) that the immediately preceding non-blank line is `deluge_file_invalidate_cache();`.

- [ ] **Step 5: commit**

```bash
git add include/libdeluge/file_io.h src/fatfs/file_io.cpp \
  src/deluge/gui/context_menu/delete_file.cpp src/deluge/gui/ui/save/save_song_ui.cpp \
  src/deluge/model/sample/sample_recorder.cpp src/deluge/processing/stem_export/stem_export.cpp \
  src/deluge/deluge.cpp
git commit -m "file_io: add deluge_file_invalidate_cache, call it from raw-FatFS writers outside the boundary

Task 3's own review found that activating the directory cache exposes a
real wrong-file-open hazard: five call sites elsewhere in the app mutate
the filesystem via raw FatFS calls that bypass file_io.h entirely, and
never invalidated the cache smsysex.cpp's already-migrated directory
listing/open calls now populate and consult. Adds a minimal public
boundary function these five sites call as a safety net -- not a
migration of these files to file_io.h, which remains separate, larger,
future work."
```

---

### Task 5: `instrument_clip_view.cpp` migration

**Files:**
- Modify: `src/deluge/gui/views/instrument_clip_view.cpp`

**Interfaces:**
- Consumes: `deluge::io::Directory` (existing, Tier-1-ready).

- [ ] **Step 1: swap the include**

`src/deluge/gui/views/instrument_clip_view.cpp` — `fatfs.hpp` (line 21) is used *only* by the one function this task migrates (confirmed via `grep -n "FatFS::\|staticDIR\|staticFNO\|f_readdir\|AM_DIR" src/deluge/gui/views/instrument_clip_view.cpp` — all 4 matches are within `potentiallyRandomizeDrumSample`), so it becomes dead. Replace:

```cpp
#include "fatfs.hpp"
```

with:

```cpp
#include "io/file.hpp"
```

- [ ] **Step 2: migrate the scan**

Replace the directory-open-and-scan portion of `potentiallyRandomizeDrumSample` (`instrument_clip_view.cpp:2101-2120`, everything from the `// Open directory of current audio file` comment through the closing `}` of the `while` loop):

```cpp
	// Open directory of current audio file
	*slashAddress = 0;
	auto dirOpened = deluge::io::Directory::open(path);
	if (!dirOpened.has_value()) {
		*slashAddress = '/';
		display->displayError(Error::SD_CARD);
		return ActionResult::DEALT_WITH;
	}
	deluge::io::Directory dir = std::move(*dirOpened);
	*slashAddress = '/';

	// Select random audio file from directory
	int32_t fileCount = 0;
	while (true) {
		auto entry = dir.read();
		if (!entry.has_value() || !entry->has_value()) {
			break;
		}
		audioFileManager.loadAnyEnqueuedClusters();
		if ((*entry)->is_directory || !isAudioFilename((*entry)->name)) {
			continue;
		}
		if (random(fileCount++) == 0) { // Algorithm: Reservoir Sampling with k=1
			strncpy(chosenFilename, (*entry)->name, 256);
		}
	}
```

Note `audioFileManager.loadAnyEnqueuedClusters()` still runs on *every* successfully-read entry, including ones about to be skipped — matching the original's exact ordering (it ran before the `AM_DIR`/`isAudioFilename` check there too, not just on accepted entries).

- [ ] **Step 3: build and test**

```bash
./dbt build Debug
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Expected: all clean, golden-master bit-exact (this function isn't on the render path — it's a UI-triggered "randomize drum sample" action).

Confirm no FatFS symbols remain in this file:

```bash
grep -n "FatFS::\|staticDIR\|staticFNO\|f_readdir\|AM_DIR" src/deluge/gui/views/instrument_clip_view.cpp
```

Expected: no matches.

- [ ] **Step 4: commit**

```bash
git add src/deluge/gui/views/instrument_clip_view.cpp
git commit -m "instrument_clip_view: migrate the randomize-drum-sample scan to deluge::io::Directory"
```

---

### Task 6: Full regression check

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

Expected: clean build, `PASS — MIXDOWN matches golden`.

- [ ] **Step 3: full spec suite**

```bash
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Expected: 100% pass, including every case added across Tasks 1-3.

- [ ] **Step 4: confirm `instrument_clip_view.cpp` is FatFS-free**

```bash
grep -n "FatFS::\|staticDIR\|staticFNO\|f_readdir\|AM_DIR" src/deluge/gui/views/instrument_clip_view.cpp
```

Expected: no matches.

- [ ] **Step 5: confirm `staticDIR`/`staticFNO` still have real remaining consumers** (they are *not* being retired by this plan — this is a sanity check that nothing was accidentally deleted)

```bash
grep -rln "staticDIR\|staticFNO" src/deluge/
```

Expected: `browser.cpp`, `sample_browser.cpp`, and `storage_manager.h`/`.cpp` still reference them — confirming Tier 2's exclusion (design doc §6) is reflected correctly in the actual tree, not just in docs.

- [ ] **Step 6: commit** (only if any of the above needed a fix; otherwise this task has no commit)

