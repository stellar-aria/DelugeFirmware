# `deluge::io` call-site migration, Plan 1: mechanical files — Design

**Date:** 2026-07-14 (amended twice same-day: first to target `deluge::io` not raw `file_io.h`; then expanded after a full roadmap audit found `browser.cpp`'s plain mkdir sites and `storage_manager.cpp`'s `buildPathToFile` are ALSO genuinely mechanical — see §1)
**Status:** Design (approved in brainstorming) → plan next
**Branch:** `feat/libdeluge-file-io-migration-1`, rebased onto `next` post-`deluge::io` merge

## 1. Goal

Migrate the genuinely mechanical, lowest-risk FatFS call sites in the app to **`deluge::io`** (merged into `next`; design at `docs/superpowers/specs/2026-07-14-deluge-io-wrapper-design.md`) — the idiomatic C++23 wrapper (`Status` enum class, move-only RAII `File`/`Directory`, `std::expected`-returning methods, `mkdir`/`unlink`/`rename` free functions) over `include/libdeluge/file_io.h` (also merged into `next`; design at `docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md`). Call sites migrate to `deluge::io`, **not** to raw `file_io.h` C-ABI calls directly — that raw boundary is BSP-implementation territory, not app-facing (see the wrapper design doc §1 for why).

**This is "Tier 1" of the real migration roadmap** (`docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md`), which supersedes the original 5-plan file-grouped sequence — a deeper audit found that grouping by file didn't match the real dependency structure (some "different files" share one gating problem; one "single file," `storage_manager.cpp`, is actually the most architecturally central piece of the whole migration). Read the roadmap doc for the full picture (Tiers 2-4, none designed yet) — this doc covers only Tier 1.

## 2. Scope: six files, one pattern, thirteen call sites

All thirteen call sites are in `src/deluge/`, all already build against the merged `deluge::io` (`src/deluge/io/file.hpp`).

**Two files originally considered for this plan were dropped after reading the actual surrounding code (not visible from a call-site grep alone) — both are part of Tier 2 of the roadmap, not this plan:**
- `gui/ui/browser/sample_browser.cpp`'s `AM_DIR` check sits inside a folder-load loop built on `FatFS::Directory::read_and_get_filepointer()`, which returns a `FilePointer` (`{DWORD sclust; FSIZE_t objsize;}` — a raw FAT starting-cluster + size fast-path) forwarded into `AudioFileManager::getAudioFileFromFilename`.
- `gui/views/instrument_clip_view.cpp`'s `staticDIR`/`staticFNO` are `extern` globals (`storage_manager.h:410-411`) shared with `browser.cpp`, `sample_browser.cpp`, and `audio_file_manager.cpp` — changing their type in isolation would break the other three files.

Neither is mechanical; both are tracked in `TODO.md` and the roadmap doc's Tier 2.

**Two files/sites were added after the same audit found them genuinely independent, expanding this plan beyond its original four files:** three plain `f_mkdir` sites in `gui/ui/browser/browser.cpp` (unrelated to that file's problematic scan loop, which stays in Tier 2), and `storage/storage_manager.cpp`'s `buildPathToFile` recursive-mkdir helper (self-contained, unrelated to that file's much larger `FileReader`/`FileWriter` redesign, which is Tier 3).

### Pattern A — settings-folder bootstrap (six files, thirteen call sites)

A one-time, best-effort idiom repeated identically at each site: ensure a settings folder exists, then rename a legacy filename to a new one if it exists (or delete a superseded file), tolerating "already there" as success rather than failure.

| file:line | operation |
|---|---|
| `gui/views/performance_view.cpp:1866` | `f_mkdir(SETTINGS_FOLDER)` |
| `gui/views/performance_view.cpp:1868` | `f_rename("PerformanceView.XML", PERFORM_DEFAULTS_XML)` |
| `io/midi/midi_follow.cpp:1669` | `f_mkdir(SETTINGS_FOLDER)` |
| `io/midi/midi_device_manager.cpp:467` | `f_unlink(MIDI_DEVICES_XML)` (best-effort, error already ignored today) |
| `io/midi/midi_device_manager.cpp:518` | `f_mkdir(SETTINGS_FOLDER)` |
| `io/midi/midi_device_manager.cpp:520` | `f_rename("MIDIDevices.XML", MIDI_DEVICES_XML)` |
| `model/settings/runtime_feature_settings.cpp:207` | `f_mkdir(SETTINGS_FOLDER)` |
| `model/settings/runtime_feature_settings.cpp:209` | `f_rename("CommunityFeatures.XML", RUNTIME_FEATURE_SETTINGS_FILE)` |
| `model/settings/runtime_feature_settings.cpp:278` | `f_unlink(RUNTIME_FEATURE_SETTINGS_FILE)` (best-effort, error already ignored today) |
| `gui/ui/browser/browser.cpp:530` | `f_mkdir(defaultDirToAlsoTry)` — **different idiom, only `FR_OK` is success**, see below |
| `gui/ui/browser/browser.cpp:1729` | `f_mkdir(newDirPath.c_str())` — **different idiom, does not tolerate EXISTS**, see below |
| `gui/ui/browser/browser.cpp:1753` (in `createFoldersRecursiveIfNotExists`, a per-path-component loop) | `f_mkdir(tempPath)`, tolerant of `FR_EXIST` |
| `storage/storage_manager.cpp:781` (in `StorageManager::buildPathToFile`, recursive) | `f_mkdir(s)`, tolerant of `FR_EXIST`, with a retry-on-`FR_NO_PATH` recursive branch |

Translation, identical at every EXISTS-tolerant mkdir site (the four original files, `browser.cpp:1753`, and `storage_manager.cpp:781`):

```cpp
// before
FRESULT result = f_mkdir(SETTINGS_FOLDER);
if (result == FR_OK || result == FR_EXIST) { ... }

// after
auto result = deluge::io::mkdir(SETTINGS_FOLDER);
if (result.has_value() || result.error() == deluge::io::Status::EXISTS) { ... }
```

`f_rename(a, b)` → `deluge::io::rename(a, b)`; `f_unlink(path)` → `deluge::io::unlink(path)`; both keep their existing error-tolerant call sites unchanged (return value already discarded or only loosely checked today — no new error handling introduced, none removed). Confirmed against the landed header (`src/deluge/io/file.hpp:98-100`): `mkdir`/`unlink`/`rename` are **not** `[[nodiscard]]` (only `File::open`/`Directory::open` are), so the existing best-effort, discard-the-result call sites need no `(void)` cast to stay warning-clean.

**Caveat on "identical":** the condition shape is the same everywhere, but `browser.cpp:1753`'s site (`createFoldersRecursiveIfNotExists`) inverts it to a guard clause and, on failure, always returns a **hardcoded** `fresultToDelugeErrorCode(FR_NO_PATH)` regardless of what the real result was:
```cpp
// before
FRESULT result = f_mkdir(tempPath);
if (result != FR_OK && result != FR_EXIST) {
	return fresultToDelugeErrorCode(FR_NO_PATH);
}

// after
auto result = deluge::io::mkdir(tempPath);
if (!result.has_value() && result.error() != deluge::io::Status::EXISTS) {
	return delugeStatusToError(DELUGE_ERR_NOT_FOUND); // hardcoded, preserving today's exact (pre-existing) quirk
}
```
Preserve the hardcoding exactly — it's an existing quirk, not something this migration should "fix" (see §4).

**Two sites use a genuinely different idiom — preserve both exactly, don't unify them:**

- `browser.cpp:530-537` — only `FR_OK` is treated as success; any other result (including `FR_EXIST`) falls through to the `else` branch, which calls `fresultToDelugeErrorCode(result)` and returns that `Error`. This is the one Pattern-A site whose block body actually branches on the specific error value, so it can't reuse the trivial swap above unchanged:
  ```cpp
  // before
  FRESULT result = f_mkdir(defaultDirToAlsoTry);
  if (result == FR_OK) {
  	triedCreatingFolder = true;
  	goto tryReadingItems;
  }
  else {
  	return fresultToDelugeErrorCode(result);
  }

  // after
  auto result = deluge::io::mkdir(defaultDirToAlsoTry);
  if (result.has_value()) {
  	triedCreatingFolder = true;
  	goto tryReadingItems;
  }
  else {
  	return delugeStatusToError(/* result.error(), converted from deluge::io::Status to DelugeStatus */);
  }
  ```
  **Real, small open item for implementation (not guessed here):** `delugeStatusToError` (`src/deluge/util/functions.h`/`.cpp`) takes a `DelugeStatus` (the raw C enum), but `deluge::io::mkdir`'s error is `deluge::io::Status` (the wrapper's `enum class`) — there is currently no reverse mapping from one to the other. Add a small `DelugeStatus to_deluge_status(deluge::io::Status)` helper (mirroring `to_status`'s forward direction, same 14 values) as part of implementing this site, so the call becomes `delugeStatusToError(to_deluge_status(result.error()))`.
- `browser.cpp:1729-1732` — any nonzero result (FatFS's `FRESULT` is falsy only at `FR_OK`) is treated as failure, returning a generic `Error::SD_CARD` — no EXISTS tolerance at all:
  ```cpp
  // before
  FRESULT result = f_mkdir(newDirPath.c_str());
  if (result) {
  	return Error::SD_CARD;
  }

  // after
  auto result = deluge::io::mkdir(newDirPath.c_str());
  if (!result.has_value()) {
  	return Error::SD_CARD;
  }
  ```

### Pattern B — dropped from this plan entirely

The original Pattern B candidate, `instrument_clip_view.cpp`'s directory-iteration loop, is **not** in this plan. Its `staticDIR`/`staticFNO` are the same shared `extern` globals used by `browser.cpp`, `sample_browser.cpp`, and `audio_file_manager.cpp` (`storage_manager.h:410-411`) — changing their declared type in isolation would break the other three files. This is part of the roadmap's Tier 2 (shared directory-listing state / `FilePointer` fast-path — `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §3), not a Plan-1-shaped mechanical swap. This plan is Pattern A only.

## 3. Testing — not golden-harness-covered; code-review verified

The boundary design doc's §8 assumed golden-master bit-exactness would cover this whole migration series. That holds for later tiers (`storage_manager.cpp`'s `FileReader`/`FileWriter` redesign, Tier 3, sits squarely on the golden Cordae render's song-load/save path) but **not for this plan's call sites**: they only execute on first-boot or upgrade-from-a-legacy-filename (no golden fixture simulates that).

Verification for this plan:
- **Build success** on rza1 (`dbt build Debug`) and host/sim — a real gate, since a mistranslated `Status`/`FRESULT`/`Error` comparison won't compile-check silently (they're different, unrelated types).
- **Code-level correctness by inspection** — every site is a narrow swap (documented exactly in §2 above, including the two non-EXISTS-tolerant sub-idioms in `browser.cpp` and the new `deluge::io::Status`→`DelugeStatus` reverse-mapping helper one of them needs); the risk profile is low enough that this is proportionate for a plan this size.
- **Full existing spec/unit suite** stays green (regression net for anything this migration might unexpectedly touch) — including `tests/spec_io/`'s mock-based `deluge::io` tests, which don't change, but should stay green as a sanity check that nothing about this migration's usage pattern is exercising `deluge::io` in a way its own tests didn't anticipate.
- **This plan is `deluge::io`'s first real integration test against the shipping FatFS-backed adapter** — worth calling out explicitly, not just implicitly relying on it. `deluge::io`'s own test suite only exercises it against an in-memory mock (`tests/spec_io/mock_file_io.cpp`), and that mock's final review flagged it as more permissive than real FatFS on open-error paths (e.g. `DELUGE_FILE_WRITE_CREATE` never requires a parent directory to exist against the mock, unlike real FatFS). This plan's `mkdir`-then-write/rename sequencing already exists in the code today (unaffected by this migration), so it isn't a NEW risk this plan introduces — but it's the first time `deluge::io`'s translation logic runs against the real adapter at all, so treat any behavior surprise during manual/on-device verification (below) as potentially a `deluge::io`-level bug, not just a call-site translation mistake, and report it against the wrapper's own repo location if so.
- **Manual/on-device verification of the actual behavior is out of this plan's automated scope** — exercising a real first-boot-with-legacy-filenames scenario needs a person at a keyboard or hardware, not a CI gate. Flagged the same way the Linux BSP plan flagged its own hardware-only checks — a to-do, not a blocker for landing.

## 4. Out of scope

- `sample_browser.cpp`'s `AM_DIR` call site and `instrument_clip_view.cpp`'s directory-iteration loop (§2) — both part of the roadmap's Tier 2 (shared `staticDIR`/`staticFNO` globals + the `FilePointer` fast-path), not designed yet. Tracked in `TODO.md` and `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §3.
- `storage_manager.cpp`'s `fileSystem.mount(...)` calls (SD-card lifecycle) and its `FileReader`/`FileWriter` raw-`FIL`-as-base-class-member redesign (Tier 3 of the roadmap, the largest piece of the whole migration) — neither is part of this plan.
- `browser.cpp`'s main directory-scan loop (`readFileItemsForFolder`) — the same Tier 2 problem as above; only its three standalone `f_mkdir` sites (§2) are in this plan.
- `smsysex.cpp` (Tier 4 of the roadmap) — gated on two of its own bounded gaps (a missing `f_utime` boundary function, a wire-protocol `FRESULT`-compatibility question), not designed yet.
- Removing `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` — still load-bearing for every file not yet migrated across every tier; gated on all of them landing (see the roadmap doc §7).
- Any behavior change beyond the mechanical translation — every idiom identified in §2 (EXISTS-tolerant, `FR_OK`-only, no-tolerance) is preserved exactly as it exists today, including the pre-existing quirk where `createFoldersRecursiveIfNotExists` always returns a hardcoded `FR_NO_PATH`-derived error regardless of the actual failure code — not something this migration should silently "fix."
