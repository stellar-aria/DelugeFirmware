# `deluge::io` call-site migration — real roadmap (supersedes the original 5-plan sequence)

**Date:** 2026-07-14
**Status:** Design (roadmap, not an implementation plan itself)
**Supersedes:** the "five-plan migration sequence" originally sketched in `docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md` §7/§9 and restated in `docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md` §1.

## 1. Why this doc exists

The original 5-plan sequence (Plan 1: six mechanical files, Plan 2: `storage_manager.cpp`, Plan 3: `browser.cpp`, Plan 4: `smsysex.cpp`, Plan 5: retire the legacy translators) was built from a grep-level survey of FatFS call sites — real, but shallow. Writing Plan 1's actual implementation plan required reading the real surrounding code, and two of its six files (`sample_browser.cpp`, `instrument_clip_view.cpp`) turned out to depend on things a mechanical swap can't solve. A deeper, parallel audit of the remaining files in the original sequence (`storage_manager.cpp`, `browser.cpp`, `smsysex.cpp`, plus a newly-discovered fourth consumer, `audio_file_manager.cpp`) found the same pattern repeatedly: **the plan-per-file grouping doesn't match the real dependency structure.** Several "different files" are actually one shared problem; one "single file" (`storage_manager.cpp`) is actually the most architecturally central piece of the whole migration, bigger than the other four plans combined.

This doc replaces the file-grouped sequence with a **tier-grouped** one, reflecting what's actually independent versus what's gated on a shared resource or a missing boundary capability.

## 2. Tier 1 — genuinely mechanical, safe now

Confirmed by direct code reading (not just grep) to be fully self-contained: local variables only, no shared global state, no FatFS-internals type forwarded elsewhere.

- `gui/views/performance_view.cpp` (2 sites), `io/midi/midi_follow.cpp` (1 site), `io/midi/midi_device_manager.cpp` (3 sites), `model/settings/runtime_feature_settings.cpp` (3 sites) — the original four "settings-folder bootstrap" files. Each uses only a local, stack-only `FilePointer` inside `StorageManager::fileExists`/`openXMLFile` (an already-app-level API, unrelated to the problematic `FilePointer` usage found elsewhere — see §3).
- `gui/ui/browser/browser.cpp:530-537`, `:1729-1732`, `:1753-1756` — three plain `f_mkdir` sites, unrelated to that file's problematic directory-scan loop (§3). Two distinct sub-idioms: `:530` and `:1753` tolerate `FR_EXIST`/reuse the "already there is fine" pattern (`:1753`'s `createFoldersRecursiveIfNotExists` also recurses down the path); `:1729` does not — any nonzero result is treated as failure, no EXISTS tolerance. Preserve both idioms exactly; don't unify them into one shape as part of a "mechanical" migration. Error paths here route through `fresultToDelugeErrorCode` (the existing legacy translator) — migrating to `deluge::io::mkdir` means swapping to `delugeStatusToError` (the parallel translator added alongside it in the boundary work, its first real consumer).
- `storage/storage_manager.cpp:764-795` (`StorageManager::buildPathToFile`) — a recursive parent-mkdir helper, self-contained (local `etl::string` buffer, no shared state), tolerant of `FR_EXIST`, with a retry-on-`FR_NO_PATH` recursive branch.

**Total: 6 files, ~13 call sites.** This is the real "Plan 1" — see `docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md` (amended to this scope).

## 3. Tier 2 — the shared directory-listing / `FilePointer` fast-path problem (one refactor, four files)

Not four independent migrations — one shared-state problem spanning:

- `gui/ui/browser/browser.cpp`'s main scan loop (`readFileItemsForFolder`, ~lines 242-369) — opens `staticDIR`/`staticFNO` (`extern` globals declared `storage_manager.h:410-411`), and is where `FilePointer` (`{DWORD sclust; FSIZE_t objsize;}`, a raw FAT starting-cluster + size) gets read off `staticDIR.read_and_get_filepointer()` and **stored directly onto every `FileItem`** (`thisItem->filePointer = thisFilePointer;`). `FileItem::filePointer` is then load-bearing across the whole browsing UI: cache-invalidation checks, list-merge/dedup, identity comparisons (`browser.cpp:106-108,400-401,427-428,1168,1269`).
- `gui/ui/browser/sample_browser.cpp` — consumes `FileItem::filePointer` as a fast-path handle, forwarded into `AudioEngine::previewSample`/`AudioFileManager::getAudioFileFromFilename` (`sample_browser.cpp:569`, `:1290`) so a chosen file can be opened without re-resolving its path.
- `gui/views/instrument_clip_view.cpp` — a third consumer of the shared `staticDIR`/`staticFNO` globals (different calling shape: raw `f_readdir(&staticDIR.inner(), ...)`).
- `storage/audio/audio_file_manager.cpp` — not just a fourth consumer; this file is `FilePointer`'s actual **home**. `resolveFilePointer`/`getAudioFileFromFilename`/`buildAudioFileFromCard` (lines ~514-853) thread raw cluster numbers through as first-class parameters, and go a full level deeper than any other file found so far: `resolveFilePointer` hand-parses raw FAT directory-entry bytes directly (`ld_clust`/`ld_dword` on a raw `alternateLoadDir.dir` buffer, no FatFS wrapper API involved at all, lines ~551-552).

**Why this can't be a mechanical swap:** `file_io.h`/`deluge::io` deliberately expose no cluster-level concept — that's the point of the boundary (hide FatFS internals). Migrating this tier means a real design decision: either extend the boundary with a fast-handle concept (a `deluge::io` equivalent of "open this file without re-resolving its path, given a previously-obtained opaque locator"), or redesign `FileItem`/the browsing UI to resolve by path every time (accepting whatever performance cost that has, or caching differently). `audio_file_manager.cpp`'s raw FAT-parsing is deeper still and may need its own bespoke treatment (a documented, permanent exemption from the abstraction is a real possible outcome here, not necessarily a design failure).

**Status: not designed yet.** Needs its own brainstorm/design pass before any implementation plan. Tracked in `TODO.md`.

## 4. Tier 3 — `storage_manager.cpp`'s `FileReader`/`FileWriter` base-class redesign

The largest, most architecturally central piece of the whole migration — not "Plan 2, a call-site swap."

`FileReader`/`FileWriter` (`storage_manager.h:54-113`) hold a raw `FIL readFIL{}`/`writeFIL{}` as a **base-class member** — not even the `FatFS::File` C++ wrapper, bare FatFS `FIL`. Every serializer/deserializer in the app — `Serializer`, `XMLSerializer`, `XMLDeserializer`, `JsonSerializer`, `JsonDeserializer`, `FileDeserializer` (`storage_manager.h:120-297`) — inherits from these two classes. Every song, preset, XML, and JSON load/save in the app rides on this. Migrating it means giving `FileReader`/`FileWriter` a `deluge::io::File` member instead of a raw `FIL`, which ripples through the entire hierarchy — every raw `f_read`/`f_write`/`f_close`/`f_lseek`/`f_size` call in `storage_manager.cpp` (lines 863, 928, 967, 1012, 1043, 1050, 1058, 1072, 1077) is a method of these two classes operating on that member, not an independent call site to swap one at a time.

A second FatFS-internals leak found in the same file: `reader.readFIL.obj.id = fileSystem.id;` (`storage_manager.cpp:292`) — a raw `FFOBJID.id` poke, same class of problem as `FilePointer` (§3), no boundary equivalent.

**Status: not designed yet.** Given the blast radius (every serialization consumer in the app), this deserves its own dedicated brainstorm/design pass — likely the single highest-value piece of the whole migration once tackled, and the highest-risk. Do not attempt as an incremental call-site plan; it needs an explicit before/after class design first.

## 5. Tier 4 — `smsysex.cpp`, gated on two specific, bounded gaps

Confirmed structurally self-contained: its own private pool of `FIL`/`DIR` handles (`FILdata openFiles[4]`, a file-local `DIR sxDIR`), never touching the shared `staticDIR`/`staticFNO` globals from §3. Its size (~40 FatFS references) and complexity (retry loops, path auto-creation, an LRU-ish file-handle pool) are ordinary business logic, not a structural blocker — **once two specific gaps are resolved:**

1. **`f_utime` has no `file_io.h`/`deluge::io` equivalent.** Used (via `setFileTimestamp`) at `smsysex.cpp:209,370,698,883` to set a file/directory's date/time from client-supplied values, needed by `openFile`'s path-creation branch, `createDirectory`, `updateTime`, and `moveFile`'s post-rename timestamp step. Blocks fully migrating those specific paths unless the boundary gains a timestamp-setting function, or timestamp-setting stays a permanent raw-FatFS touch point.
2. **The SysEx wire protocol echoes raw `FRESULT` numeric values to clients** (`jWriter.writeAttribute("err", errCode)`, `errCode` typed `FRESULT`, at least 9 sites e.g. `smsysex.cpp:274,300,330,375,414,502,571,661,705`). The companion software on the other end of this protocol parses those numbers. Migrating to `deluge::io::Status`/`DelugeStatus` changes the wire values unless deliberately remapped back to FRESULT-compatible codes at the boundary, or the protocol version bumps — an external-compatibility question (`docs/dev/target_architecture.md` already flags the companion protocol as needing "a stability promise"), not a code-structure problem.

**Status: two bounded design questions, not designed yet.** Smaller and more tractable than Tiers 2/3 — likely the next tier worth tackling once one of them is resolved, since the rest of the file is otherwise ordinary migration work.

## 6. Catalogue: FatFS-internals leaks with no boundary equivalent

Four found so far, all requiring either a boundary extension or a documented permanent exemption — none is a simple oversight fixable by adding one function:

1. **`FilePointer`** (`{sclust, objsize}`) — cluster/size fast-path. §3.
2. **`staticFNO.altname`** — raw 8.3 short-filename byte-pattern matching, used by `audio_file_manager.cpp`'s recording-slot auto-numbering (`"REC*.WAV"` detection) to derive the next unused slot number.
3. **`readFIL.obj.id`** — raw `FFOBJID.id` poke. §4.
4. **`audio_file_manager.cpp`'s direct `ld_clust`/`ld_dword` FAT directory-entry parsing** — deeper than the other three; a full level below even `f_open`/`f_read`. §3.

## 7. Sequencing

No fixed order is dictated across Tiers 2-4 — each needs its own brainstorm/design pass before it can be turned into an implementation plan, and they're independent of each other (Tier 3's `FileReader`/`FileWriter` redesign doesn't block Tier 2's directory-listing work or vice versa). Tier 1 (the expanded Plan 1) is ready now and doesn't wait on any of them landing first. Retiring the legacy translators (`fresultToDelugeErrorCode`/`fatfsErrorToDelugeError`, the original "Plan 5") remains gated on every tier landing — including Tiers 2-4, which don't exist as implementation plans yet.
