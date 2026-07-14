# `deluge::io` call-site migration, Plan 1: mechanical files — Design

**Date:** 2026-07-14 (amended same-day: targets `deluge::io`, not raw `file_io.h`, now that the wrapper has landed — see §1)
**Status:** Design (approved in brainstorming) → plan next
**Branch:** `feat/libdeluge-file-io-migration-1`, rebased onto `next` post-`deluge::io` merge

## 1. Goal

Migrate the five simplest, lowest-risk FatFS call sites in the app from raw FatFS calls to **`deluge::io`** (merged into `next`; design at `docs/superpowers/specs/2026-07-14-deluge-io-wrapper-design.md`) — the idiomatic C++23 wrapper (`Status` enum class, move-only RAII `File`/`Directory`, `std::expected`-returning methods, `mkdir`/`unlink`/`rename` free functions) over `include/libdeluge/file_io.h` (also merged into `next`; design at `docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md`). Call sites migrate to `deluge::io`, **not** to raw `file_io.h` C-ABI calls directly — that raw boundary is BSP-implementation territory, not app-facing (see the wrapper design doc §1 for why). This is Plan 1 of a five-plan migration sequence covering the ~65 call sites the boundary design doc's §7 catalogued:

1. **Plan 1 (this doc)** — the five mechanical files.
2. **Plan 2** — `storage/storage_manager.cpp` (the central file-I/O engine; excludes its `fileSystem.mount(...)` calls, which are SD-card mount/detection lifecycle already covered by `block_device.h`, not `file_io.h`).
3. **Plan 3** — `gui/ui/browser/browser.cpp` (directory browsing).
4. **Plan 4** — `storage/smsysex.cpp` (the companion SysEx protocol; ~40 FatFS references woven through real business logic — multi-step file-copy-with-retry, rename-with-path-autocreate, paginated directory listing. Deliberately last, once the boundary's real-world shape is proven on four simpler plans first).
5. **Plan 5** — retire `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` (`src/deluge/util/functions.h`/`.cpp`) once no call site anywhere in the app still calls raw FatFS. Gated on Plans 1-4 all landing.

Each plan is independently shippable and golden-master-verifiable (or, for this plan, verifiable by the alternative means below) on its own — no plan depends on a later one landing first.

## 2. Scope: five files, two patterns, eleven call sites

All eleven call sites are in `src/deluge/`, all already build against the merged `deluge::io` (`src/deluge/io/file.hpp`).

**`gui/ui/browser/sample_browser.cpp` was in the original scope, dropped after reading the actual surrounding code (not visible from a call-site grep alone).** Its `AM_DIR` check sits inside a folder-load loop built on `FatFS::Directory::read_and_get_filepointer()`, which returns a `FilePointer` (`{DWORD sclust; FSIZE_t objsize;}` — a raw FAT starting-cluster + size fast-path) forwarded into `AudioFileManager::getAudioFileFromFilename` so it can jump straight to a file's location instead of re-resolving the path. `file_io.h`/`deluge::io` deliberately expose no cluster-level concept, so there is nothing to migrate this call to without either a real fast-handle extension to the boundary or a redesign of the loop to resolve by path instead — neither is mechanical. Tracked in `TODO.md` as its own item; out of scope for every plan in this migration series until that's designed.

### Pattern A — settings-folder bootstrap (four files, eight call sites)

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

Translation, identical at every mkdir site:

```cpp
// before
FRESULT result = f_mkdir(SETTINGS_FOLDER);
if (result == FR_OK || result == FR_EXIST) { ... }

// after
auto result = deluge::io::mkdir(SETTINGS_FOLDER);
if (result.has_value() || result.error() == deluge::io::Status::EXISTS) { ... }
```

`f_rename(a, b)` → `deluge::io::rename(a, b)`; `f_unlink(path)` → `deluge::io::unlink(path)`; both keep their existing error-tolerant call sites unchanged (return value already discarded or only loosely checked today — no new error handling introduced, none removed). Confirmed against the landed header (`src/deluge/io/file.hpp:98-100`): `mkdir`/`unlink`/`rename` are **not** `[[nodiscard]]` (only `File::open`/`Directory::open` are), so the existing best-effort, discard-the-result call sites need no `(void)` cast to stay warning-clean.

### Pattern B — directory listing / is-this-a-directory (one file, three call sites)

| file:line | operation |
|---|---|
| `gui/views/instrument_clip_view.cpp:2092` | `staticDIR = D_TRY_CATCH(FatFS::Directory::open(path), error, {...})` (wrapper, reassigning a `static`-lifetime `Directory`) |
| `gui/views/instrument_clip_view.cpp:2101` | `f_readdir(&staticDIR.inner(), &staticFNO)` (raw call on the wrapper's inner `DIR` — a level-mixing pre-existing wart this migration also cleans up) |
| `gui/views/instrument_clip_view.cpp:2103` | `staticFNO.fattrib & AM_DIR` |

`instrument_clip_view.cpp` currently mixes the `FatFS::Directory` C++ wrapper (for `open`) with a raw `f_readdir` call on its inner `DIR` — this migration replaces the whole sequence with `deluge::io::Directory::open`/`.read()` (RAII close; no explicit `.close()` call needed, since `staticDIR`'s *reassignment* on the next call — see below — is what actually needs to release the old handle, not scope exit). No `FilePointer`/fast-path dependency here (unlike the dropped `sample_browser.cpp` site above) — this loop only reads directory entries, checks `AM_DIR`, and does reservoir sampling; genuinely mechanical.

**`staticDIR`'s reassignment needs `D_TRY_CATCH_MOVE`, not `D_TRY_CATCH`** (both already exist as generic macros in `src/deluge/util/try.h`, not part of `deluge::io` — `D_TRY_CATCH`'s expansion ends in `result.value();`, a copy/lvalue-bind that won't compile against `deluge::io::Directory`'s deleted copy-assignment; `D_TRY_CATCH_MOVE`'s expansion ends in `std::move(result.value());`, which correctly selects `Directory`'s move-assignment — already self-guarded and already closes its own prior handle before taking the new one, verified in the wrapper's own review). The block body is unchanged (it doesn't reference the caught error variable at all today — `{ *slashAddress = '/'; display->displayError(Error::SD_CARD); return ActionResult::DEALT_WITH; }` — so no error-type translation is needed either):

```cpp
// before
staticDIR = D_TRY_CATCH(FatFS::Directory::open(path), error, {
	*slashAddress = '/';
	display->displayError(Error::SD_CARD);
	return ActionResult::DEALT_WITH;
});

// after
staticDIR = D_TRY_CATCH_MOVE(deluge::io::Directory::open(path), error, {
	*slashAddress = '/';
	display->displayError(Error::SD_CARD);
	return ActionResult::DEALT_WITH;
});
```

**The read-loop itself is not a purely mechanical rename** (flagged by the `deluge::io` wrapper's final whole-branch review as a real migration-planning risk, not a blocker): the current `while (f_readdir(&staticDIR.inner(), &staticFNO) == FR_OK && staticFNO.fname[0] != 0) { ... }` loop inspects a returned struct's name field to detect end-of-directory. `deluge::io::Directory::read()` instead returns `std::expected<std::optional<DelugeDirEntry>, Status>` — end-of-directory is `std::nullopt`, checked directly, no name-field inspection needed. The loop needs a real rewrite (loop-until-`nullopt`), not a token-for-token substitution — budget real implementation time for this, not just a search-and-replace.

## 3. Testing — not golden-harness-covered; code-review verified

The boundary design doc's §8 assumed golden-master bit-exactness would cover this whole migration series. That holds for later plans (`storage_manager.cpp`'s song load/save path is squarely on the golden Cordae render's path) but **not for Plan 1's specific call sites**: Pattern A only executes on first-boot or upgrade-from-a-legacy-filename (no golden fixture simulates that), and Pattern B is UI directory-browsing code, never reached by an audio-render test.

Verification for this plan:
- **Build success** on rza1 (`dbt build Debug`) and host/sim — a real gate, since a mistranslated `Status`/`FRESULT` comparison won't compile-check silently (they're different, unrelated enum types).
- **Code-level correctness by inspection** — every site is a narrow swap (documented exactly in §2 above; Pattern B's directory-iteration rewrite needs real attention per the note above, Pattern A stays purely mechanical); the risk profile is low enough that this is proportionate.
- **Full existing spec/unit suite** stays green (regression net for anything this migration might unexpectedly touch) — including `tests/spec_io/`'s mock-based `deluge::io` tests, which don't change, but should stay green as a sanity check that nothing about this migration's usage pattern is exercising `deluge::io` in a way its own tests didn't anticipate.
- **This plan is `deluge::io`'s first real integration test against the shipping FatFS-backed adapter** — worth calling out explicitly, not just implicitly relying on it. `deluge::io`'s own test suite only exercises it against an in-memory mock (`tests/spec_io/mock_file_io.cpp`), and that mock's final review flagged it as more permissive than real FatFS on open-error paths (e.g. `Directory::open` on a missing path succeeds against the mock but would fail with a real error against FatFS; `DELUGE_FILE_WRITE_CREATE` never requires a parent directory to exist against the mock, unlike real FatFS). Pattern A's `mkdir`-then-write-in-that-folder sequencing already exists in the code today (unaffected by this migration), so it isn't a NEW risk this plan introduces — but it's the first time `deluge::io`'s translation logic runs against the real adapter at all, so treat any behavior surprise during manual/on-device verification (below) as potentially a `deluge::io`-level bug, not just a call-site translation mistake, and report it against the wrapper's own repo location if so.
- **Manual/on-device verification of the actual behavior is out of this plan's automated scope** — exercising a real first-boot-with-legacy-filenames scenario, and the directory-browsing UI, needs a person at a keyboard or hardware, not a CI gate. Flagged the same way the Linux BSP plan flagged its own hardware-only checks — a to-do, not a blocker for landing.

## 4. Out of scope

- `sample_browser.cpp`'s `AM_DIR` call site (§2) — blocked on the `FilePointer` fast-path gap, tracked in `TODO.md`, not part of any Plan in this migration series until that's designed.
- `storage_manager.cpp`'s `fileSystem.mount(...)` calls — SD-card lifecycle, not file access; not part of any Plan in this migration series.
- Removing `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` — still load-bearing for every file not yet migrated (Plans 2-4); removal is Plan 5, gated on all of them landing.
- Any behavior change beyond the mechanical translation — Pattern A's "tolerate already-exists," "best-effort unlink," and "rename legacy filename" semantics are preserved exactly as they exist today.
