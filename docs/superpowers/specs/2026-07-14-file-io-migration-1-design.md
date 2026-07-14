# `file_io.h` call-site migration, Plan 1: mechanical files — Design

**Date:** 2026-07-14
**Status:** Design (approved in brainstorming) → plan next
**Branch:** `feat/libdeluge-file-io-migration-1`, based off `next`

## 1. Goal

Migrate the six simplest, lowest-risk FatFS call sites in the app from raw FatFS calls to the `include/libdeluge/file_io.h` boundary (merged into `next`; design at `docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md`). This is Plan 1 of a five-plan migration sequence covering the ~65 call sites the boundary design doc's §7 catalogued:

1. **Plan 1 (this doc)** — the six mechanical files.
2. **Plan 2** — `storage/storage_manager.cpp` (the central file-I/O engine; excludes its `fileSystem.mount(...)` calls, which are SD-card mount/detection lifecycle already covered by `block_device.h`, not `file_io.h`).
3. **Plan 3** — `gui/ui/browser/browser.cpp` (directory browsing).
4. **Plan 4** — `storage/smsysex.cpp` (the companion SysEx protocol; ~40 FatFS references woven through real business logic — multi-step file-copy-with-retry, rename-with-path-autocreate, paginated directory listing. Deliberately last, once the boundary's real-world shape is proven on four simpler plans first).
5. **Plan 5** — retire `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` (`src/deluge/util/functions.h`/`.cpp`) once no call site anywhere in the app still calls raw FatFS. Gated on Plans 1-4 all landing.

Each plan is independently shippable and golden-master-verifiable (or, for this plan, verifiable by the alternative means below) on its own — no plan depends on a later one landing first.

## 2. Scope: six files, two patterns, eleven call sites

All eleven call sites are in `src/deluge/`, all already build against the merged `file_io.h`.

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
DelugeStatus result = deluge_file_mkdir(SETTINGS_FOLDER);
if (result == DELUGE_OK || result == DELUGE_ERR_EXISTS) { ... }
```

`f_rename(a, b)` → `deluge_file_rename(a, b)`; `f_unlink(path)` → `deluge_file_unlink(path)`; both keep their existing error-tolerant call sites unchanged (return value already discarded or only loosely checked today — no new error handling introduced, none removed).

### Pattern B — directory listing / is-this-a-directory (two files, three call sites)

| file:line | operation |
|---|---|
| `gui/ui/browser/sample_browser.cpp:1264` | `staticFNO.fattrib & AM_DIR` (already reads from a `FatFS::Directory`-wrapper-produced `FILINFO`) |
| `gui/views/instrument_clip_view.cpp:2092` | `FatFS::Directory::open(path)` (wrapper) |
| `gui/views/instrument_clip_view.cpp:2101` | `f_readdir(&staticDIR.inner(), &staticFNO)` (raw call on the wrapper's inner `DIR` — a level-mixing pre-existing wart this migration also cleans up) |
| `gui/views/instrument_clip_view.cpp:2103` | `staticFNO.fattrib & AM_DIR` |

`instrument_clip_view.cpp` currently mixes the `FatFS::Directory` C++ wrapper (for `open`) with a raw `f_readdir` call on its inner `DIR` — this migration replaces the whole sequence with `deluge_dir_open`/`deluge_dir_read`/`deluge_dir_close`, so it no longer touches FatFS at any level, wrapper or raw. `sample_browser.cpp` already goes through `FatFS::Directory::read()` (via a helper) for the entry itself; only the `AM_DIR` bit-check line changes, reading `DelugeDirEntry::is_directory` instead (already computed by `deluge_dir_read`, since the boundary's `to_dir_entry` helper — landed as part of the boundary work — does this exact bit-test internally).

## 3. Testing — not golden-harness-covered; code-review verified

The boundary design doc's §8 assumed golden-master bit-exactness would cover this whole migration series. That holds for later plans (`storage_manager.cpp`'s song load/save path is squarely on the golden Cordae render's path) but **not for Plan 1's specific call sites**: Pattern A only executes on first-boot or upgrade-from-a-legacy-filename (no golden fixture simulates that), and Pattern B is UI directory-browsing code, never reached by an audio-render test.

Verification for this plan:
- **Build success** on rza1 (`dbt build Debug`) and host/sim — a real gate, since a mistranslated `DelugeStatus`/`FRESULT` comparison won't compile-check silently (they're different, unrelated enum types).
- **Code-level correctness by inspection** — every site is a narrow, mechanical 1:1 swap (documented exactly in §2 above); the risk profile is low enough that this is proportionate, matching how trivial single-line swaps were treated within the boundary work itself.
- **Full existing spec/unit suite** stays green (regression net for anything this migration might unexpectedly touch).
- **Manual/on-device verification of the actual behavior is out of this plan's automated scope** — exercising a real first-boot-with-legacy-filenames scenario, and the directory-browsing UI, needs a person at a keyboard or hardware, not a CI gate. Flagged the same way the Linux BSP plan flagged its own hardware-only checks — a to-do, not a blocker for landing.

## 4. Out of scope

- `storage_manager.cpp`'s `fileSystem.mount(...)` calls — SD-card lifecycle, not file access; not part of any Plan in this migration series.
- Removing `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` — still load-bearing for every file not yet migrated (Plans 2-4); removal is Plan 5, gated on all of them landing.
- Any behavior change beyond the mechanical translation — Pattern A's "tolerate already-exists," "best-effort unlink," and "rename legacy filename" semantics are preserved exactly as they exist today.
