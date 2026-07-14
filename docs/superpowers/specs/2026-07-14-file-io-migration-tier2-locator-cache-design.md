# `deluge::io` migration — Tier 2: retiring `FilePointer` via a transparent adapter-side cache

**Date:** 2026-07-14
**Status:** Design
**Context:** Tier 2 of `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §3 — the last remaining tier after Tier 4 (`smsysex.cpp`, merged) and before Tier 3 (`FileReader`/`FileWriter`, independent, not started).

## 1. Why this doc exists, and how it revises the roadmap's framing

The roadmap doc framed Tier 2 as "one shared-state problem" spanning `browser.cpp`, `sample_browser.cpp`, `instrument_clip_view.cpp`, and `audio_file_manager.cpp`, unified by their common use of the `staticDIR`/`staticFNO` externs and FatFS's `FilePointer` (`{sclust, objsize}`) fast-reopen mechanism. Investigating each use site directly (not just grepping for the shared symbols) found a narrower and deeper problem than that framing suggested:

- **The `staticDIR`/`staticFNO` sharing is incidental, not structural.** Every use site (`browser.cpp::readFileItemsForFolder`, `sample_browser.cpp::loadAllSamplesInFolder`, `instrument_clip_view.cpp`'s randomize-drum-sample scan) opens, fully consumes, and closes the directory within a single function call. None of them need cross-call persistence. They share the globals because they were convenient pre-existing FatFS state to reuse, not because the design requires sharing. `instrument_clip_view.cpp` in particular never touches `FilePointer` at all — it only wants filenames and directory-ness, which `deluge::io::Directory`/`DelugeDirEntry` (already built, Tier-1-ready) already fully provides.
- **The real hard problem is `FilePointer` itself — a deliberate embedded-systems performance hack, one level deeper than the roadmap doc's original audit caught.** `StorageManager::openFilePointer` (`storage_manager.cpp:284`) doesn't call `f_open` at all: it hand-constructs a raw `FIL`'s internal fields (`obj.sclust`, `obj.objsize`, `obj.fs`, `obj.id`, `flag`, `err`, `sect`, `fptr`) directly, bypassing FatFS's public API to skip the expensive part of opening a file — walking the directory tree from the path to find where the file's data starts. `AudioFileManager::resolveFilePointer` has a matching probe on the discovery side (`create_name`/`dir_find`/`ld_clust`/`ld_dword` — FatFS-internal functions, not its public API) for looking up a filename's locator without a full open, used when checking several candidate filenames in sequence (`SAMPLES/`'s "alternate load" folder).

This doc addresses the real problem directly: eliminate the FAT directory-walk cost on a re-resolve, without requiring the app to carry an opaque locator value around.

## 2. Why FAT directory walks are worth avoiding here

FAT directory entries are linearly scanned 32-byte records within a directory's own cluster chain; each path component requires its own scan-and-chain-walk from its parent. Each step is a separate SD card transaction, and small random SD reads routinely cost single-digit-to-tens of milliseconds on the hardware this firmware targets — a real, perceptible stall for a synth that can't block its UI or audio engine. This isn't a newly-invented concern: `FilePointer`'s existence (hand-rolled, bypassing FatFS's own API) is strong circumstantial evidence someone already measured and fixed a real stall this way. This design preserves that fix's *effect*, not its *shape*.

## 3. Rejected alternative: an opaque `DelugeFileLocator` boundary type

The first design considered (and initially proposed) was a direct, faithful port of `FilePointer`: a new opaque `DelugeFileLocator` C-ABI type, a `deluge_file_open_by_locator` function, and a locator field added to `DelugeDirEntry`, with the app explicitly threading the locator through `FileItem` and back into open calls — architecturally sound, but it keeps a FatFS-shaped performance concept visible at the app layer indefinitely, and requires every future backend (including a hypothetical Linux BSP, whose filesystem already has fast dentry/inode caching and doesn't need this trick at all) to at least accept the concept in its API surface even when it's a no-op internally.

**Rejected in favor of the design below** after establishing two things: (1) tracing every real call site showed the "in-flight" working set is always small and singular — at most `browser.cpp`'s 20-entry navigation batch (`FILE_ITEMS_MAX_NUM_ELEMENTS_FOR_NAVIGATION`), and every other use (`sample_browser.cpp`'s bulk loader, `resolveFilePointer`'s alternate-dir probe, `fileExists`→`openXMLFile`) uses a locator immediately and discards it — never a large, long-lived, multi-directory working set; (2) FatFS's own `dir_find` is *also* a linear scan under the hood (FAT has no indexed/hashed directories), so an app-visible iterate-and-compare using `deluge::io::Directory` costs the same asymptotically as the raw internals hack it would replace. Both facts together mean the optimization can move entirely into the FatFS adapter as a small, bounded, single-slot cache — with **zero new app-visible boundary surface**.

## 4. The design: a single-slot "last-scanned-directory" cache, adapter-internal only

Lives entirely in `src/fatfs/file_io.cpp`/`file_io_internal.hpp` (BSP-internal, not exposed via `file_io.h`). Not a general-purpose cache — one slot, wholesale-replaced, matching the real usage pattern established in §3.

```cpp
namespace deluge::fatfs_adapter {

constexpr size_t kDirCacheCapacity = 64; // generous headroom over browser.cpp's 20-entry nav batch

struct DirCacheEntry {
	etl::string<DELUGE_MAX_FILENAME - 1> name;
	DWORD sclust;
	FSIZE_t objsize;
};

// Adapter-internal, single-slot, wholesale-replaced. Not exposed via file_io.h.
struct DirCache {
	std::string directory_path; // the parent directory this cache is valid for
	etl::vector<DirCacheEntry, kDirCacheCapacity> entries;
	bool valid = false;
};

} // namespace deluge::fatfs_adapter
```

**Populated as a side effect of normal iteration** — `deluge_dir_open(path)` sets `directory_path = path`, clears `entries`, sets `valid = true`; `deluge_dir_read` appends each entry's `(name, sclust, objsize)` as it's read (silently stops appending, without invalidating the cache, once `kDirCacheCapacity` is reached — graceful degradation, not a correctness issue, just a missed optimization for unusually large folders). `deluge_dir_close` does **not** clear the cache — the cache deliberately outlives the `DelugeDir` handle, since the entire point is serving opens *after* the directory that produced them has already been closed.

**Consulted by `deluge_file_open`** — only for `DELUGE_FILE_READ` (a `DELUGE_FILE_WRITE_CREATE` is creating/truncating, not looking up an existing entry, so it always takes the normal path). Split `path` into `(dirname, basename)`; if `dirname == cache.directory_path` and `basename` is found in `cache.entries`, construct the `FIL` via the same raw-internals fast path `openFilePointer` uses today (`obj.sclust`/`obj.objsize`/`obj.fs`/`obj.id` set directly, skipping `f_open`'s directory walk) — this logic *moves into* the adapter, it doesn't change. Otherwise, fall through to an ordinary `f_open`.

**No validation on a cache hit** — matches today's exact behavior; `openFilePointer` doesn't validate today either (see §6 on the one place this note has an app-visible consequence).

**Invalidation policy: any write-shaped call anywhere clears the cache unconditionally.** `deluge_file_open(path, DELUGE_FILE_WRITE_CREATE)`, `deluge_file_mkdir`, `deluge_file_unlink`, `deluge_file_rename` all clear `cache.valid = false` before doing their own work. This is deliberately coarse — it doesn't try to reason about whether a given write actually touches the cached directory — because writes are rare relative to scans/reads in this app's usage pattern, so the performance cost of being conservative is negligible, and a coarse, easy-to-prove-correct invariant is worth far more here than a marginal cache-hit-rate improvement.

## 5. `resolveFilePointer`'s alternate-dir probe: no new capability needed

`AudioFileManager::resolveFilePointer`'s `tryAlternateName` (today: `create_name`+`dir_find`+`ld_clust`+`ld_dword` on a kept-open `alternateLoadDir`, checking several candidate filenames in sequence without opening any of them) doesn't need a new boundary "stat by name" function. It becomes ordinary `deluge::io::Directory` iteration: open the alternate directory once (already what happens today — `alternateLoadDir` is opened once and reused, just via raw FatFS), then for each candidate name, iterate `Directory::read()` comparing names until a match or the directory is exhausted. This costs the same asymptotically as the raw internals it replaces (§3), and — because it goes through `deluge_dir_read` — it warms the adapter's cache as a side effect, so the *actual* open that follows a successful match gets the fast path for free.

## 6. App-side consequences, file by file

### `browser.cpp`
- `readFileItemsForFolder`'s scan loop migrates to a local `deluge::io::Directory` (no more `staticDIR`/`staticFNO`).
- `FileItem::filePointer` (`FilePointer`, a raw FatFS type) is **deleted**. The fast-reopen behavior that motivated it is now automatic and invisible.
- Three call sites used `.sclust` as an identity/staleness token, not for reopening — each is addressed independently:
  - `checkFP()` (`browser.cpp:96-115`, gated `#if ALPHA_OR_BETA_VERSION` — a debug assertion, not user-facing production behavior) compared a fresh lookup's `.sclust` against the cached `.sclust` to detect "the file at this exact path was deleted and replaced by a different file with the same name" between scan-time and now. Without a locator to compare, this narrows to just re-confirming the path still resolves to *some* file (`deluge::io::File::open(filePath, DELUGE_FILE_READ)` succeeding) — losing the specific same-path-different-file detection. **This is an intentional, explicit narrowing of a debug-only safety net**, not a silent behavior change; flagged here for review the same way prior tiers flagged their accepted collapses.
  - `deleteFolderAndDuplicateItems` (`browser.cpp:356-357,383-384`) used `.sclust == 0` as a sentinel for "this `FileItem` doesn't actually exist on the card yet" (e.g. a virtual/placeholder entry for an `Instrument` reference with no backing file). This is a pure existence flag, unrelated to reopening — becomes an explicit `bool resolved` (or reuse of the existing `maybeExistsOnCard`, if that field already carries the same meaning — needs a one-line confirmation during implementation) instead of overloading a locator field's zero-ness.
  - `predictExtendedText` (`browser.cpp:919-923,1023`) compared `.sclust` before/after a folder re-read to detect "the currently-selected file changed" (captured as a POD value specifically because the `fileItems` vector can reallocate during the re-read, making the old `FileItem*` unsafe to dereference afterward). Filename comparison (`std::string`, already stored by value in `FileItem`, equally reallocation-safe) is a direct, equally-correct substitute — no loss of behavior, since two *different* files never legitimately share a name within one directory.

### `sample_browser.cpp`
- `loadAllSamplesInFolder`'s scan loop migrates to a local `deluge::io::Directory`.
- `AudioEngine::previewSample`/`AudioFileManager::getAudioFileFromFilename`'s `FilePointer*` parameters are dropped; both become plain path-based calls, fast automatically when the path was recently scanned.

### `instrument_clip_view.cpp`
- Its randomize-drum-sample scan migrates to a local `deluge::io::Directory` — genuinely mechanical, since it never touched `FilePointer`, `staticDIR`, or `staticFNO` for anything but convenience.

### `audio_file_manager.cpp`
- `resolveFilePointer` (likely renamed, since "FilePointer" as a concept is gone from the app) simplifies to: the "regular path" case becomes a plain `deluge::io::File::open(path)`; the "alternate dir" case becomes the `Directory`-iteration described in §5. All raw `ld_clust`/`ld_dword`/`create_name`/`dir_find`/`readFIL.obj.sclust` access disappears from app code — it doesn't vanish, it *relocates* into the FatFS adapter as part of the cache implementation (§4), which is exactly where FatFS-internals access belongs.

### `storage_manager.cpp`/`.h`
- `openFilePointer` and its raw `FIL` construction are deleted — the adapter does the equivalent internally now (§4).
- `openXMLFile`/`openJsonFile` collapse to their path-based `deluge::io::File::open`, relying on the adapter cache for speed.
- `staticDIR`/`staticFNO` (`storage_manager.h:410-411`) are deleted entirely once all three UI consumers hold their own local `deluge::io::Directory` instances — a full retirement of this shared-global pattern, not a partial one.

### `staticFNO.altname`-based recording-slot numbering
The roadmap doc's catalogue (§6 item 2) separately flagged `audio_file_manager.cpp`'s use of `staticFNO.altname` (raw 8.3 short-filename bytes) for `"REC*.WAV"` auto-numbering. This is unrelated to `FilePointer`/locators and is **out of scope for this design** — it's a `DelugeDirEntry` gap (no short-name field), not a caching one. Left as a tracked, separate `TODO.md` item if not already resolved by the time this lands.

## 7. Testing

This is adapter-internal logic with no app-visible behavior difference on the happy path *by design* — both the cache-hit and cache-miss paths must produce identical results, which makes "did we actually take the fast path" unobservable from black-box behavior alone. Add a plain (non-atomic, single-threaded firmware) diagnostic counter in the `deluge::fatfs_adapter` namespace — e.g. `size_t g_dir_cache_hits`, `g_dir_cache_misses` — incremented at the two `deluge_file_open` branch points, inspectable by tests but not part of `file_io.h`'s public surface. `tests/spec/file_io_spec.cpp` (the existing real-FatFS-linked suite) gets new cases:
- A scan followed by an in-cache open increments the hit counter and returns correct file contents.
- Opening a path whose directory was never scanned increments the miss counter and still succeeds via the normal path.
- Opening a *different* directory invalidates the cache (subsequent same-directory open misses again).
- Any write operation (`mkdir`/`unlink`/`rename`/a `DELUGE_FILE_WRITE_CREATE` open) invalidates the cache.
- A folder larger than `kDirCacheCapacity` degrades gracefully — entries beyond the cap simply don't get cached, no crash, no corruption, opens for those entries just take the normal (miss) path.

## 8. Rollout

Two independent halves, in this order:
1. **Adapter cache** (`src/fatfs/file_io_internal.hpp`/`file_io.cpp`) — self-contained, testable via `tests/spec/file_io_spec.cpp` alone, no app-side changes yet. `deluge_file_open`'s observable behavior is unchanged (same results, just sometimes faster) — this half is safe to land and golden-master-verify on its own.
2. **App-side migration**, file by file, roughly ascending complexity: `instrument_clip_view.cpp` (fully independent, no `FilePointer` involvement) → `browser.cpp` (needs the three identity-comparison decisions from §6 resolved) → `sample_browser.cpp` → `audio_file_manager.cpp`/`storage_manager.cpp` (the deepest, since `openFilePointer`'s raw internals and `resolveFilePointer`'s alternate-dir probe live here).

`staticDIR`/`staticFNO` can only be deleted once *all three* UI consumers (browser.cpp, sample_browser.cpp, instrument_clip_view.cpp) have migrated off them — tracked as the rollout's completion gate, not a per-file task.

## 9. Out of scope

- Tier 3 (`storage_manager.cpp`'s `FileReader`/`FileWriter`) — independent, not started, tracked separately.
- `staticFNO.altname`-based recording-slot numbering (§6) — a different `DelugeDirEntry` gap, not a locator/caching one.
- Any change to the adapter cache's *policy* beyond what's described here (e.g. a smarter multi-directory LRU) — the single-slot design is deliberately matched to this app's actual usage pattern (§3), not a general-purpose cache; revisit only if a future use site's access pattern genuinely doesn't fit it.
