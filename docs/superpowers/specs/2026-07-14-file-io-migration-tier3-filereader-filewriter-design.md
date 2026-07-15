# `deluge::io` migration — Tier 3: `FileReader`/`FileWriter`

**Date:** 2026-07-14
**Status:** Design
**Context:** Tier 3 of `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §4 — the last remaining tier after Tier 1, Tier 2, and Tier 4 (all merged into `next`). The roadmap doc calls this "the single largest, most architecturally central piece of the whole migration."

## 1. Why this doc exists

`storage_manager.h`'s `FileReader`/`FileWriter` (lines ~54-113) hold a raw `FIL readFIL{}`/`writeFIL{}` as a base-class member — not even the `FatFS::File` C++ wrapper, bare vendored FatFS. Every serializer/deserializer in the app — `Serializer`+`FileWriter` → `XMLSerializer`/`JsonSerializer`; `Deserializer`+`FileReader` → `FileDeserializer` → `XMLDeserializer`/`JsonDeserializer` — inherits from these two classes. Every song, preset, XML, and JSON load/save in the app rides on this.

Investigating the actual code (not just the class declarations) found the real scope is larger than "redesign two base classes": `StorageManager::openXMLFile`/`openJsonFile` and every wrapper around them thread a `FilePointer*` through 13 function signatures, all doing the same raw-internals fast-reopen trick Tier 2 already replaced for directory-driven opens (`StorageManager::openFilePointer`, `storage_manager.cpp:284`, hand-constructs a `FIL`'s internal fields exactly like the fast path Tier 2 built into the adapter). Since Tier 2's adapter cache already makes a plain `deluge::io::File::open(path)` fast after a directory scan — and this XML/JSON-open path is exactly that access pattern — `FilePointer` can be dropped from it entirely, not just re-typed.

## 2. Core member redesign

`FileReader::readFIL`/`FileWriter::writeFIL` (`FIL`) → `std::optional<deluge::io::File> file;`. Matches the established pattern from Tier 2 (`DirCache`) and Tier 4 (`FILdata`'s pool) — `deluge::io::File` is move-only with no default/empty state, so "not currently open" needs `std::optional`, not a sentinel value.

Every raw call in `FileReader::readFileCluster`/`FileWriter::writeBufferToFile`/`FileReader::closeWriter`/`FileWriter::closeWriter` becomes the matching `file->read/write/close(...)` call, gated on `memoryBased` exactly as today (the `memoryBased` branches never touch `readFIL`/`writeFIL` today and won't touch `file` after — `std::nullopt` represents "no real file" more explicitly than an unused-but-allocated `FIL` does).

## 3. `StorageManager::createFile` migrates too

Tightly coupled, not optional to include: `createFile`'s entire purpose is producing a `File` for a `FileWriter` to write into. Today it returns `std::expected<FatFS::File, Error>` (the low-level C++ wrapper, not `deluge::io::File`); `createXMLFile`/`createJsonFile` extract `.inner()` (the raw `FIL`) and assign it directly into `writer.writeFIL`. Migrates to `deluge::io::File::open(path, DELUGE_FILE_WRITE_CREATE)` for the open, `deluge::io::mkdir` for the folder-creation retry — callers then just move-assign: `writer.file = std::move(created.value());`.

**Bonus cleanup, not a separate task:** this makes Tier 2 Task 7's two `deluge_file_invalidate_cache()` calls inside `createFile` *redundant* — `deluge::io`'s own write-create-open and `mkdir` already invalidate internally (that's the whole point of the boundary owning invalidation). Remove both calls as part of this migration; leaving them in would be harmless but stale, and this doc's whole thrust is retiring raw-internals workarounds Tier 2 already made unnecessary.

## 4. `closeAfterWriting`'s verification read-back

After writing, `FileWriter::closeAfterWriting` currently reopens the *same* `writeFIL` for reading — checks total size, checks the beginning bytes match a caller-supplied string, seeks to the end and checks the tail bytes match another caller-supplied string. A real, deliberate paranoia check (most serialized files get their exact prologue/epilogue string verified byte-for-byte after write), not something to simplify away.

With `std::optional<File>`: `file->close()`, then `file = deluge::io::File::open(path, DELUGE_FILE_READ)` — a clean reassignment of the same optional member, using `deluge::io::File::seek`/`read` for the size/beginning/end checks. No new concept; this is the exact same "close and reopen the same underlying file for a different purpose" pattern already established for `FileWriter` vs. `FileReader` roles.

## 5. `FilePointer` retirement — the full blast radius

Confirmed via a full call-graph audit before finalizing this design (grep-only investigation surfaces false confidence about scope on a file this central — this was verified by actually reading every call site, following each wrapper down to where it terminates).

### In scope: 13 signatures, ~20 call sites, all mechanically similar

- `StorageManager::openXMLFile`/`openJsonFile` (free functions, `storage_manager.h:366`/`368`) — drop `FilePointer*`, take `path`.
- `XMLDeserializer::openXMLFile`/`JsonDeserializer::openJsonFile` (member functions, `storage_manager.h:234`/`319`; bodies in `Deserializer.cpp:834`/`JsonDeserializer.cpp:569`) — the `FilePointer* filePointer` parameter is **confirmed dead** in both bodies (referenced only in the signature line, never inside). Drop the parameter entirely, not just its type.
- Four wrapper-opens: `openMidiDeviceDefinitionFile`/`openPatternFile`/`openFavouriteFile`/`openInstrumentFile` (`storage_manager.h:388/392/396/406`) — each has an `if (!filePointer->sclust) return Error::FILE_NOT_FOUND;` guard that becomes moot once path-based (a failed `deluge::io::File::open` already reports `NOT_FOUND` cleanly) — drop the guard along with the parameter.
- Five wrapper-loads: `loadMidiDeviceDefinitionFile`/`loadPatternFile`/`loadFavouriteFile`/`loadInstrumentFromFile`/`loadSynthToDrum` (`storage_manager.h:389/393/397/384/401`) — same treatment, one level up.
- `StorageManager::openFilePointer` (`storage_manager.h:402`) — becomes fully dead once every caller above stops passing it a `FilePointer`. **Delete it.** This also resolves the `TODO.md`-tracked raw `FFOBJID.id` poke (`storage_manager.cpp:292-300`, `reader.readFIL.obj.id = fileSystem.id;` and neighbors) — it lives entirely inside this function, so deleting the function resolves the TODO as a side effect, not a separate task.
- Roughly 8 `fileExists(path, &fp)` call sites whose output feeds directly into one of the above: the 4 already-Tier-1-migrated files (`performance_view.cpp`, `midi_device_manager.cpp`, `midi_follow.cpp`, `runtime_feature_settings.cpp` — each currently does a `fileExists(path,&fp)`-then-`openXMLFile(&fp,...)` two-step, sometimes with a retry/legacy-path-migration loop in between), plus `load_song_ui.cpp:559`, `favourite_manager.cpp:71`, `load_instrument_preset_ui.cpp:596`, `load_instrument_preset_ui.cpp:912`. Each collapses to either the one-arg `fileExists(path)` (when a real pre-check/retry decision needs the boolean) or drops the pre-check entirely (when the only thing that mattered was feeding a locator into an open that now just takes the path).

### Out of scope, confirmed and unchanged

- `browser.cpp:101,417` and `load_instrument_preset_ui.cpp:407` — genuine `FileItem::filePointer`/`checkFP()`-adjacent uses. `load_instrument_preset_ui.cpp:407` specifically writes into the real, shared `FileItem::filePointer` field (the one Tier 2 confirmed must stay, since `SampleBrowser` needs it for the real-time cluster-streaming path) — this call site stays even though *this specific consumer* could theoretically do without it, because other code may still read that field afterward. Not touched.
- The whole SAMPLE-streaming bucket (`audio_file_holder.{h,cpp}`, `audio_engine.{h,cpp}`'s `previewSample`, `audio_file_manager.{h,cpp}`'s `getAudioFileFromFilename`/`resolveFilePointer`/`buildAudioFileFromCard`) — Tier 2's already-established exclusion (`docs/superpowers/specs/2026-07-14-file-io-migration-tier2-locator-cache-design.md` §6), unrelated to `FileReader`/`FileWriter` entirely.
- `StorageManager::fileExists(path, FilePointer*)`'s own implementation (`storage_manager.cpp:222-237`, a local `FIL fil` + raw `f_open`/`f_close`) — **stays raw FatFS, deliberately.** Its job is producing a `FilePointer{sclust,objsize}` value, a FatFS-specific type `deluge::io::File` deliberately never exposes (that's the boundary's whole point). This function still has real, out-of-scope callers (`browser.cpp`, `load_instrument_preset_ui.cpp:407`) after this tier lands — it just loses several of its in-scope callers. Migrating its internals is structurally impossible without reintroducing the exact FatFS-internals leak this whole migration exists to remove; it belongs in the same permanent-exemption bucket as `audio_file_manager.cpp`'s raw FAT parsing.
- `StorageManager::fileExists(pathName)` (one-arg, `storage_manager.cpp:211-217`) — uses `f_stat`+the shared `staticFNO` global, Tier 2's already-excluded territory (`staticDIR`/`staticFNO` stay referenced by `browser.cpp`/`sample_browser.cpp`/`storage_manager.h`/`.cpp`, confirmed in Tier 2's final regression check). Tangential to `FileReader`/`FileWriter`, not part of this redesign. Left exactly as-is.

## 6. Testing

Same accepted constraint established in Tiers 2 and 4: `tests/spec/`'s target (`deluge_spec`/`all_specs`) has no real mountable filesystem (`mock_diskio.cpp` reports "no disk" unconditionally).

- **The `memoryBased` path needs no real I/O at all.** `FileReader(char*, uint32_t)`/`FileWriter(bool inMem)` never touch the optional `File` member — it stays `std::nullopt` for the object's whole lifetime. This is fully testable today and stays fully testable after the redesign, with no change in coverage.
- **The real file-backed path's byte-level read/write correctness** (`readFileCluster` genuinely reading N bytes from disk, `writeBufferToFile` genuinely writing N bytes) can't be exercised end-to-end without real disk I/O — same accepted gap as Tier 2's adapter-cache tests. What stays testable without real I/O: the `std::optional<File>` state-transition structure itself (a fresh `FileReader`/`FileWriter` starts with `file == std::nullopt`; a hand-constructed scenario can verify `closeAfterWriting`'s close-then-reopen-for-verification sequence transitions the optional correctly) via the same hand-constructed-input pattern established in `tests/spec/file_io_spec.cpp`.

## 7. Rollout — scale expectation, not a task breakdown

~13 function signatures and ~20 call sites across ~8 files is a meaningfully larger implementation plan than Tier 2 (6 tasks) or Tier 4 (8 tasks, after two rounds of review-driven additions) — likely 8-10+ tasks. Real task boundaries aren't decided here; that's writing-plans' job, and it will need its own exhaustive per-site investigation (the same way Tier 4's plan absorbed the full `smsysex.cpp` investigation) — probably via forked research passes given the file count, not something to front-load into this design doc.

A natural (not committed) task grouping, given the dependency order established above:
1. `FileReader`/`FileWriter` member redesign (§2) — self-contained, the `memoryBased` path proves it compiles/tests independently of any real-file consumer.
2. `createFile`/`createXMLFile`/`createJsonFile` migration (§3) — depends on (1)'s new member existing.
3. `closeAfterWriting`'s verification read-back (§4) — depends on (1).
4. `openXMLFile`/`openJsonFile` signature change, both free and member forms, + `openFilePointer` deletion (§5) — depends on (1)-(3) landing (the functions being called must already accept the new member shape).
5. The four wrapper-opens + five wrapper-loads' signature changes (§5) — depends on (4).
6. Call-site updates across the ~8 consumer files, including the `fileExists(path,&fp)` simplifications (§5) — depends on (5).
7. Final regression check.

## 8. Out of scope

- Any change to `StorageManager::fileExists`'s two-arg overload's internals, or the one-arg overload's `f_stat`/`staticFNO` usage (§5) — both deliberately excluded, for different reasons (FatFS-internals-leak permanent exemption vs. Tier-2-excluded-territory).
- The whole SAMPLE-streaming / `deluge-stream` boundary question (§5) — Tier 2's territory, tracked separately in `TODO.md`.
- Any change to the wire/serialization format itself (XML/JSON tag structure, attribute encoding) — this tier is purely about the storage backend `FileReader`/`FileWriter` write to/read from, not what they write.
