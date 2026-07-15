# `deluge::io` migration — Tier 2: an adapter-side directory cache (`FilePointer` mostly stays)

**Date:** 2026-07-14
**Status:** Design
**Context:** Tier 2 of `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §3 — the last remaining tier after Tier 4 (`smsysex.cpp`, merged) and before Tier 3 (`FileReader`/`FileWriter`, independent, not started).
**Scope note (read this first):** this doc went through two real narrowings while being written. The roadmap's original four-file framing narrowed to "just build an adapter-side cache" (§1-§5, below). Then, mid-brainstorm, tracing `FilePointer`'s actual downstream consumer in `audio_file_manager.cpp` found the cache alone doesn't let three of the four files retire `FilePointer` at all — §6 covers that finding and why it's a real, permanent scope cut, not a deferred task. **The only things this design actually delivers are the adapter cache (§4) and `instrument_clip_view.cpp`'s migration (§6) — read §8 for the real rollout.**

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

**No validation on a cache hit** — matches today's exact behavior; `openFilePointer` doesn't validate today either.

**Invalidation policy: any write-shaped call anywhere clears the cache unconditionally.** `deluge_file_open(path, DELUGE_FILE_WRITE_CREATE)`, `deluge_file_mkdir`, `deluge_file_unlink`, `deluge_file_rename` all clear `cache.valid = false` before doing their own work. This is deliberately coarse — it doesn't try to reason about whether a given write actually touches the cached directory — because writes are rare relative to scans/reads in this app's usage pattern, so the performance cost of being conservative is negligible, and a coarse, easy-to-prove-correct invariant is worth far more here than a marginal cache-hit-rate improvement.

Writes to an *already-open* handle (`deluge::io::File::write`, extending a file past the size it had when its directory was last scanned) are **not** separately invalidated — the file's `sclust` never changes on append (only `objsize` might go stale), so a subsequent fast-path hit can never return a *different* file's data, only a same-file read that's short by however many bytes were appended since the last scan. This is a deliberately accepted, narrow residual (confirmed during the final review, not merely assumed): the invariant this cache actually defends is "never open the wrong file," not "always report the exact current size."

## 5. `resolveFilePointer`'s alternate-dir probe: no new capability needed — **superseded, see §6**

**This section is no longer actionable for Tier 2.** It was written before §6's finding that `audio_file_manager.cpp`'s `FilePointer` usage is excluded from this design entirely (its `buildAudioFileFromCard` consumer needs raw FAT cluster numbers for real-time sample streaming, not just a fast directory re-resolve). Kept here as reference reasoning — the "iterate instead of raw internals costs the same asymptotically" argument may still be relevant to a future `deluge-stream` boundary design — but no implementation plan should treat this section as a Tier 2 task.

`AudioFileManager::resolveFilePointer`'s `tryAlternateName` (today: `create_name`+`dir_find`+`ld_clust`+`ld_dword` on a kept-open `alternateLoadDir`, checking several candidate filenames in sequence without opening any of them) doesn't need a new boundary "stat by name" function. It becomes ordinary `deluge::io::Directory` iteration: open the alternate directory once (already what happens today — `alternateLoadDir` is opened once and reused, just via raw FatFS), then for each candidate name, iterate `Directory::read()` comparing names until a match or the directory is exhausted. This costs the same asymptotically as the raw internals it replaces (§3), and — because it goes through `deluge_dir_read` — it warms the adapter's cache as a side effect, so the *actual* open that follows a successful match gets the fast path for free.

## 6. Why `browser.cpp`, `sample_browser.cpp`, and `audio_file_manager.cpp` are excluded from this design — a correction made mid-brainstorm

An earlier draft of this section proposed deleting `FileItem::filePointer` entirely and migrating all three files' scan loops to `deluge::io::Directory`, on the premise that the adapter cache (§4) makes every reopen fast without the app needing to carry a locator. That premise is **wrong for one real consumer**, discovered by tracing `AudioFileManager::buildAudioFileFromCard`'s `AudioFileType::SAMPLE` branch (`audio_file_manager.cpp:801-816`) all the way through:

```cpp
uint32_t currentSDCluster = effectiveFilePointer.sclust; // First cluster, whose address we already got.
while (true) {
	static_cast<Sample*>(audioFile)->clusters[currentClusterIndex].sdAddress = clst2sect(&fileSystem, currentSDCluster);
	...
	currentSDCluster = get_fat_from_fs(&fileSystem, currentSDCluster);
	...
}
```

This isn't avoiding a directory walk — it's using `effectiveFilePointer.sclust` as the seed for its own raw FAT cluster-chain walk (`get_fat_from_fs`/`clst2sect`, more FatFS-internal functions), building a complete cluster→sector address table for the sample file. That table feeds `ClusterByteSource`, which streams audio data directly from SD card sectors during playback, deliberately bypassing FatFS's buffered file-read API for real-time performance. **This is a categorically different capability than "open a file portably"** — it's raw block/cluster-level access, and no reasonable design for `file_io.h`/`deluge::io` should expose it; a Linux backend has no FAT clusters to walk at all.

`Browser::readFileItemsForFolder` (the scan loop this design proposed migrating) is the *shared base-class* implementation used by both `SampleBrowser` (whose selections feed straight into the cluster-walk above) and the preset/song browsers (`LoadInstrumentPresetUI`, `DxSyxBrowser`, via `SlotBrowser`/`LoadUI` — same base class, same `FileItem` struct). Because one real consumer of `FileItem::filePointer` has this irreducible dependency, `FileItem::filePointer` **cannot** be deleted or made portable, and `browser.cpp`'s scan loop **cannot** migrate off `staticDIR.read_and_get_filepointer()` — not until the deeper problem (below) has its own answer.

### Where this actually belongs: `deluge-stream`, not `file_io.h`

Checked this against two things before concluding it's genuinely out of scope, not just hard:

1. **The resource manager (`crates/deluge_resource/`, Rust, fully merged into `next`) already gets this right and doesn't need to change.** Its `Source.materialize` callback for sample clusters is `AudioFileManager::readClusterData` (`audio_file_manager.cpp:924`), which does a raw sector read at an *already-resolved* `sdAddress` — it never touches FAT structures itself. The cluster-chain walk above runs once, upstream, at file-open time, entirely before the resource manager is involved; it produces the input `Source.materialize` later consumes. The resource manager's abstraction is already disk-*address*-oriented, not path/FAT-oriented — correctly layered, nothing to fix there.
2. **`docs/dev/target_architecture.md` already names this exact split as intentional, not-yet-built future work** (lines 262-272): `deluge-files` ("task-context storage: browsing, preset/song load/save... blocking-allowed") versus `deluge-stream` ("audio-context storage: cluster cache, sample streaming, SD read scheduling... Split from deluge-files because the two halves have opposite realtime contracts"). `file_io.h`/`deluge::io` is `deluge-files`'s boundary. The cluster-chain walk is squarely `deluge-stream`'s territory — an upstream, one-time "resolve this path to a cluster→sector table" operation, distinct from both `file_io.h` and the resource manager's `Source` abstraction.

**Conclusion:** `browser.cpp`, `sample_browser.cpp`, and `audio_file_manager.cpp`'s `FilePointer` usage stays exactly as it is. It isn't blocked on more design work in *this* doc — it's blocked on a future `deluge-stream` boundary, a separate, substantial architecture project (real-time audio streaming storage) that doesn't exist yet and isn't scoped here. `storage_manager.cpp`'s `openFilePointer`/`openXMLFile`/`openJsonFile`(`FilePointer*`, ...) overlaps this only through `staticDIR`/`staticFNO`'s shared-global retirement (blocked for the same reason) and through the WAVETABLE path's use of `FileReader` — that piece is Tier 3's territory (once `FileReader` holds a `deluge::io::File` instead of a raw `FIL`, `openXMLFile` can take a path directly, no locator involved) and resolves as a side effect of Tier 3, not something this doc or Tier 2 needs to solve.

### `instrument_clip_view.cpp` — unaffected, still migrates cleanly
Its randomize-drum-sample scan migrates to a local `deluge::io::Directory` — genuinely mechanical, since it never touched `FilePointer`, `staticDIR`, or `staticFNO` for anything but convenience, and has no relationship to the cluster-streaming path above.

### `staticFNO.altname`-based recording-slot numbering
The roadmap doc's catalogue (§6 item 2) separately flagged `audio_file_manager.cpp`'s use of `staticFNO.altname` (raw 8.3 short-filename bytes) for `"REC*.WAV"` auto-numbering. Unrelated to `FilePointer`/locators — a `DelugeDirEntry` gap (no short-name field), not a caching one. Left as a tracked, separate `TODO.md` item.

## 7. Testing

This is adapter-internal logic with no app-visible behavior difference on the happy path *by design* — both the cache-hit and cache-miss paths must produce identical results, which makes "did we actually take the fast path" unobservable from black-box behavior alone. Add a plain (non-atomic, single-threaded firmware) diagnostic counter in the `deluge::fatfs_adapter` namespace — e.g. `size_t g_dir_cache_hits`, `g_dir_cache_misses` — incremented at the two `deluge_file_open` branch points, inspectable by tests but not part of `file_io.h`'s public surface. `tests/spec/file_io_spec.cpp` (the existing real-FatFS-linked suite) gets new cases:
- A scan followed by an in-cache open increments the hit counter and returns correct file contents.
- Opening a path whose directory was never scanned increments the miss counter and still succeeds via the normal path.
- Opening a *different* directory invalidates the cache (subsequent same-directory open misses again).
- Any write operation (`mkdir`/`unlink`/`rename`/a `DELUGE_FILE_WRITE_CREATE` open) invalidates the cache.
- A folder larger than `kDirCacheCapacity` degrades gracefully — entries beyond the cap simply don't get cached, no crash, no corruption, opens for those entries just take the normal (miss) path.

## 8. Rollout

Two tasks, both real, both worth landing on their own — this is the design's *entire* rollout, not a first phase of a larger one:

1. **Adapter cache** (`src/fatfs/file_io_internal.hpp`/`file_io.cpp`) — self-contained, testable via `tests/spec/file_io_spec.cpp` alone, no app-side changes. `deluge_file_open`'s observable behavior is unchanged (same results, just sometimes faster) — genuine, immediate value for every already-migrated call site (Tiers 1 and 4) and every future one (Tier 3), independent of anything else in this doc.
2. **`instrument_clip_view.cpp`** — migrates its scan loop to a local `deluge::io::Directory`. Small, fully independent, no `FilePointer` involvement.

`browser.cpp`, `sample_browser.cpp`, `audio_file_manager.cpp`, and `storage_manager.cpp`'s `FilePointer`/`staticDIR`/`staticFNO` usage are **not** part of this rollout (§6) — they stay as they are pending a future `deluge-stream` boundary design, tracked as a separate `TODO.md` item, not a task of this plan.

## 9. Out of scope

- **`browser.cpp`, `sample_browser.cpp`, `audio_file_manager.cpp`'s `FilePointer` usage, and `staticDIR`/`staticFNO`'s retirement** (§6) — blocked on a future `deluge-stream` boundary (`docs/dev/target_architecture.md` lines 262-272), a separate, substantial architecture project for real-time audio-streaming storage. Not part of this migration.
- Tier 3 (`storage_manager.cpp`'s `FileReader`/`FileWriter`) — independent, not started, tracked separately. Its completion incidentally simplifies `openXMLFile`/`openJsonFile`'s `FilePointer*` dependency (once `FileReader` holds a `deluge::io::File`, those functions can take a path directly), but that's a side effect of Tier 3, not something Tier 2 does.
- `staticFNO.altname`-based recording-slot numbering (§6) — a different `DelugeDirEntry` gap, not a locator/caching one.
- Any change to the adapter cache's *policy* beyond what's described here (e.g. a smarter multi-directory LRU) — the single-slot design is deliberately matched to this app's actual usage pattern (§3), not a general-purpose cache; revisit only if a future use site's access pattern genuinely doesn't fit it.
- Designing the `deluge-stream` boundary itself — a real, valuable, separate future project. Its rough shape (a `deluge_stream_open`/`deluge_stream_read_at`-style port, sibling to `file_io.h`, feeding the resource manager's `Source.materialize` the same way `readClusterData` does today) is sketched in this doc's brainstorm history but not designed here.
