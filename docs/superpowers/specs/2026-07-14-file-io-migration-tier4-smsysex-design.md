# `deluge::io` migration — Tier 4: `smsysex.cpp`

**Date:** 2026-07-14
**Status:** Design
**Context:** Tier 4 of `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md` §5 — one of three independent remaining tiers (2/3/4) after Tier 1 (`docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md`) landed on `next`.

## 1. Why this doc exists

The roadmap doc identified `smsysex.cpp` (the companion SysEx file-transfer protocol, ~1026 lines, ~40 FatFS references) as structurally self-contained — its own private `FILdata openFiles[4]` pool and file-local `DIR sxDIR`, never touching the shared `staticDIR`/`staticFNO` globals that block Tier 2 — but gated on two bounded gaps:

1. `file_io.h`/`deluge::io` has no `f_utime` equivalent (timestamp-setting).
2. The wire protocol echoes raw `FRESULT` numeric values to the companion app.

Investigating both surfaced a **third gap**, not in the original roadmap doc: `getDirEntries` (the directory-listing command) reads `FILINFO.fsize`/`fdate`/`ftime`/`fattrib` directly and forwards all four to the wire. `DelugeDirEntry` only carries `{name, is_directory}` today. This doc folds that in — the alternative (leaving `getDirEntries` on raw FatFS as a permanent exemption) would leave a FatFS-internals leak in a file this migration is meant to fully clear.

## 2. Boundary extensions

### 2.1 `DelugeTimestamp` (new, `types.h`)

```c
typedef struct DelugeTimestamp {
    uint16_t year;   // e.g. 2026 — full year, not DOS's 1980-offset encoding
    uint8_t month;   // 1-12
    uint8_t day;     // 1-31
    uint8_t hour;    // 0-23
    uint8_t minute;  // 0-59
    uint8_t second;  // 0-59 (a DOS-backed implementation rounds to its native 2s resolution)
} DelugeTimestamp;
```

Broken-out y/m/d/h/m/s fields, not epoch seconds. FAT DOS date/time has no timezone concept; epoch seconds would force inventing one where none exists today. The wire protocol already sends/receives values that are trivially decomposed to/from this shape (see §4.2).

### 2.2 New setter function (`file_io.h`)

```c
DelugeStatus deluge_file_set_time(const char* path, DelugeTimestamp timestamp);
```

FatFS adapter implementation (`src/fatfs/file_io.cpp`) packs `DelugeTimestamp` into a DOS-packed `FILINFO` and calls `f_utime`. A future non-FatFS backend (e.g. a Linux BSP) would map this to `utimensat` or equivalent — the point of keeping this portable rather than DOS-packed at the ABI layer.

### 2.3 Extended `DelugeDirEntry` (`file_io.h`)

```c
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

Portable bit-flags rather than a raw `fattrib` byte passthrough — consistent with the boundary's existing goal of not leaking FatFS internals. The FatFS adapter's `to_dir_entry` helper (already existing in `src/fatfs/file_io_internal.hpp`) gains the mapping both ways.

### 2.4 `deluge::io` wrapper additions

```cpp
std::expected<void, Status> set_time(std::string_view path, DelugeTimestamp timestamp);
```

No new C++ wrapper type for `DelugeTimestamp` or the extended `DelugeDirEntry` — both stay plain C structs used directly by value, matching the existing precedent (`Directory::read()` already returns `DelugeDirEntry` by value, unwrapped).

## 3. `smsysex.cpp`'s internal redesign

### 3.1 File pool (`FILdata`)

`FIL file;` → `std::optional<deluge::io::File> file;`. `deluge::io::File` is deliberately RAII-only with no empty/default state (every constructed instance owns a real handle) — the pool's "this slot isn't currently open" state needs to live in the `std::optional`, not in `File` itself.

- `openFIL`: calls `deluge::io::File::open(...)`; on success, assigns into the slot's optional.
- `closeFIL`: if the optional is engaged, calls `file->close()` then `.reset()`; if not engaged (never opened), no-op. This is slightly *more* correct than today's code, which unconditionally calls `f_close` on a possibly-never-opened `FIL`.

### 3.2 Directory scan state (`sxDIR`/`activeDirName`/`dirOffsetCounter`)

`DIR sxDIR` (file-scope global) → `std::optional<deluge::io::Directory> sxDir`, opened/closed the same pattern as §3.1.

**Incidental bug fix:** the current end-of-directory check is `fno.altname[0] == 0`. `altname` is FatFS's short-filename fallback, populated only when a file's real name doesn't fit 8.3 — an entry whose long name already fits 8.3 has an empty `altname`, so today's code can terminate a listing early on such an entry. `deluge::io::Directory::read()`'s `has_entry` flag is backed by FatFS's real `fname[0]==0` end check, so migrating this function fixes that bug as a side effect. This will be called out explicitly in the implementation plan and its commit message — not left to look accidental.

### 3.3 Two local translators (file-scope `static` in `smsysex.cpp`, not shared elsewhere)

**`toWireFresult(deluge::io::Status) -> FRESULT`** — maps each `Status` value to the matching legacy `FRESULT` numeric constant, used only at the exact point a reply's `"err"` attribute is written. This freezes the wire's error-code contract permanently at today's FRESULT-numeric values, regardless of what backend implements `file_io.h` underneath (FatFS-based today; a hypothetical Linux BSP tomorrow) — the translator *is* the abstraction boundary, not a stopgap for this migration. One lossy mapping, already-accepted precedent from Tier 1: `Status::NOT_FOUND` can't distinguish `FR_NO_FILE` from `FR_NO_PATH` (both collapsed into `DELUGE_ERR_NOT_FOUND` at the original boundary design); this translator picks `FR_NO_FILE` as the canonical value.

**Timestamp/attribute packers** — `DelugeTimestamp` ↔ DOS-packed `uint16_t` date/time pair, and the four `bool` attribute flags ↔ raw `fattrib` byte. Used at every wire-read point (`getDirEntries`, packing portable → wire-native) and wire-write point (`openFile`/`createDirectory`/`updateTime`/`moveFile`, unpacking wire-native → portable before calling `deluge::io::set_time`).

### 3.4 Call-site migration

Every remaining FatFS call (`f_open`/`f_read`/`f_write`/`f_lseek`/`f_mkdir`/`f_unlink`/`f_rename`/`f_opendir`/`f_readdir`/`f_closedir`, ~40 references) becomes the matching `deluge::io` call. `errCode` becomes `deluge::io::Status` internally throughout; it's converted to `FRESULT` only at the `"err"` attribute write via `toWireFresult` (§3.3).

`performFileCopy`'s two raw locals (`FIL srcFile, dstFile`) become two local `deluge::io::File` values from `File::open(...)` directly — a simple scoped RAII pair, no pooling involved.

## 4. Testing

### 4.1 Unit-testable (host-side, `tests/spec_io/`)

- DOS-packed ↔ `DelugeTimestamp` conversion, both directions, including 2-second-resolution rounding and the DOS 1980-epoch offset (handled inside the FatFS adapter — invisible to `deluge::io` callers).
- `fattrib` byte ↔ four-bool-flags conversion, both directions.
- `to_status`/`to_deluge_status` already cover `DelugeStatus`↔`Status`; unchanged.

`smsysex.cpp`-local translators (`toWireFresult`, the DOS/attr packers) are trivial 1:1 table lookups; covered via the functional path (§4.2) rather than forced into a separate unit-test seam.

### 4.2 Functional verification

`smsysex.cpp` has no existing host-side functional test, and it's not on the golden-master render path (bit-exactness there gives no signal either way). Verification for this plan:

1. Full rza1 (`./dbt build Debug`) and host/sim (`build-sim-cpp`) builds stay clean.
2. New `spec_io` conversion-table tests (§4.1) pass.
3. **Manual smoke pass** — connect the companion app (or replay a raw SysEx dump if available) and exercise open/read/write/close/mkdir/rename/delete/getDirEntries/updateTime against a real card, confirming the wire's `err`/`date`/`time`/`attr`/`size` values are byte-identical to pre-migration behavior.

Step 3 is a real gap worth naming plainly: there is no automated coverage of the actual wire protocol today, migrated or not. This plan doesn't reduce coverage, but doesn't create automated coverage either — the manual pass is the only check for wire-level correctness.

## 5. Rollout

Single Tier 4 implementation plan, same SDD process as Tier 1:

1. Boundary extension: `types.h` (`DelugeTimestamp`) → `file_io.h` (`deluge_file_set_time`, extended `DelugeDirEntry`) → FatFS adapter (`src/fatfs/file_io.cpp`/`file_io_internal.hpp`, packing/unpacking + `to_dir_entry` extension) → `deluge::io` wrapper (`set_time`).
2. `spec_io` conversion-table tests for the new packing/unpacking helpers.
3. `smsysex.cpp`'s pool/state redesign (§3.1, §3.2) as its own task — this changes `FILdata` and the directory-scan state without yet touching most call sites.
4. The two local translators (§3.3) as their own task.
5. Call-site migration, grouped by function: `openFile`/`closeFile`; `deleteFile`/`createDirectory`/`rename`; `getDirEntries`; `readBlock`/`writeBlock`; `updateTime`/`copyFile`/`moveFile`/`createPathDirectories`/`setFileTimestamp`.
6. Full regression check (builds + spec suite + golden-master, though the latter is a no-op signal here) + the manual smoke pass (§4.2), documented as a known outstanding gate the same way Tier 1's hardware tests are tracked.

## 6. Out of scope

- Tiers 2 (`FilePointer`/shared globals) and 3 (`FileReader`/`FileWriter`) — independent, tracked separately in the roadmap doc.
- Retiring `fresultToDelugeErrorCode`/`fatfsErrorToDelugeError` (the legacy translators) — gated on every tier landing, not just this one.
- Any change to the SysEx wire protocol's `"err"`/`"date"`/`"time"`/`"attr"` value *shapes* — this plan preserves them exactly; `toWireFresult` and the timestamp/attribute packers exist specifically to make that preservation permanent and backend-independent, not to change it.
