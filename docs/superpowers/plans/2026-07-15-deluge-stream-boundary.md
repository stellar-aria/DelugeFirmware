# `deluge-stream` Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace raw FatFS-internals-in-app-code (`get_fat_from_fs`/`clst2sect` in `audio_file_manager.cpp`, `file->inner().clust` + raw `FatFS::File` in `sample_recorder.cpp`) and an ad-hoc, non-boundary `disk_read_without_streaming_first`/`disk_write_without_streaming_first` pair with a real `libdeluge` boundary (`include/libdeluge/stream_io.h`), completing `block_device.h` along the way.

**Architecture:** Two independent sub-phases, each independently shippable and golden-master-verifiable: (A) finish `block_device.h`'s `deluge_block_read`/`write` on all 3 BSPs and switch the FatFS diskio glue over to them, retiring the ad-hoc pair; (B) add `stream_io.h` + one shared FatFS-family implementation (`src/fatfs/stream_io.cpp`), then migrate `audio_file_manager.cpp` (read) and `sample_recorder.cpp` (write) onto it, retiring `StorageManager::createFileRaw`.

**Tech Stack:** C (libdeluge C-ABI headers), C++23 (app code, FatFS adapter), Rust (in-tree Rust BSP), CppSpec (host-buildable unit tests in `tests/spec/`).

## Global Constraints

- Design doc: `docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md` — every task below implements a specific section of it; re-read it if a task's rationale is unclear.
- `tests/spec/` has **no real mountable filesystem** (`mock_diskio.cpp` reports `STA_NOINIT` unconditionally) — CppSpec tests in this plan verify pure logic and defined-behavior-against-unmounted/fake `FATFS` objects only (matching `tests/spec/file_io_spec.cpp`'s established pattern), never real byte-level I/O. Real read/write correctness is verified by building the host **sim** (`./dbt sim` or equivalent) and running the golden-master sweep — a different target from `tests/spec`.
- `src/fatfs/CMakeLists.txt` is auto-generated ("DO NOT EDIT" banner) by `dbt buildgen`; `tests/spec/CMakeLists.txt` is hand-maintained.
- RZA1 and the in-tree Rust BSP are not host-buildable or host-testable; per this project's standing convention, do not add a manual hardware-verification checklist item — the existing hardware/ARM-sim gate applies without prompting for it.
- New C code follows this codebase's existing `libdeluge/*.h` conventions exactly: opaque handles, `[task]`-context comments, `DelugeStatus` returns, plain C, `extern "C"`. New C++ follows idiomatic C++23 (no C-casts/goto in new code; `reinterpret_cast`/`static_cast` only, matching `src/fatfs/file_io.cpp`'s existing style).
- No `deluge::io::Stream` C++ RAII wrapper is being added — `block_device.h` has no C++ wrapper either (only `file_io.h` does, because it has ~65 call sites across ~10 files; `stream_io.h`'s calls are concentrated in 2 files, the same shape as `block_device.h`'s usage) — app code manages the raw `DelugeStream*` handle directly, same as `Sample`/`SampleRecorder` already manage `std::optional<FatFS::File>` manually today.

---

## Phase A: finish `block_device.h`

### Task 1: Host BSP — `deluge_block_read`/`deluge_block_write`

**Files:**
- Modify: `src/bsp/host/host_platform.c` (add real `deluge_block_read`/`deluge_block_write`, keep the existing `disk_read_without_streaming_first`/`disk_write_without_streaming_first` untouched for now — both exist side by side until Task 4)
- Modify: `src/bsp/host/host_bsp.c:296-323` (delete the two `DELUGE_ERR_NODEV` stub bodies — they'd otherwise duplicate-conflict with the new real ones)

**Interfaces:**
- Produces: `DelugeStatus deluge_block_read(uint8_t unit, uint8_t* dst, uint32_t sector, uint32_t count)`, `DelugeStatus deluge_block_write(uint8_t unit, const uint8_t* src, uint32_t sector, uint32_t count)` — real, callable from any BSP-neutral code including `libdeluge/block_device.h` consumers. Task 4 consumes these.

- [ ] **Step 1: Add `deluge_block_read`/`deluge_block_write` to `host_platform.c`**

Add `#include "libdeluge/block_device.h"` to `host_platform.c`'s include block (alongside its existing `#include "diskio.h"` at line 30), then add these two functions immediately after the existing `disk_write_without_streaming_first` (after line 191):

```c
DelugeStatus deluge_block_read(uint8_t unit, uint8_t* dst, uint32_t sector, uint32_t count) {
	(void)unit;
	if (host_img_fd < 0) {
		return DELUGE_ERR_NODEV;
	}
	if ((uint64_t)sector + count > host_img_sectors) {
		return DELUGE_ERR_PARAM;
	}
	size_t total = (size_t)count * HOST_SECTOR_SIZE;
	off_t base = (off_t)sector * HOST_SECTOR_SIZE;
	size_t done = 0;
	while (done < total) {
		ssize_t n = pread(host_img_fd, dst + done, total - done, base + (off_t)done);
		if (n <= 0) {
			return DELUGE_ERR_IO;
		}
		done += (size_t)n;
	}
	return DELUGE_OK;
}

DelugeStatus deluge_block_write(uint8_t unit, const uint8_t* src, uint32_t sector, uint32_t count) {
	(void)unit;
	if (host_img_fd < 0) {
		return DELUGE_ERR_NODEV;
	}
	if (!host_img_writable) {
		return DELUGE_ERR_WRITE_PROTECTED;
	}
	if ((uint64_t)sector + count > host_img_sectors) {
		return DELUGE_ERR_PARAM;
	}
	size_t total = (size_t)count * HOST_SECTOR_SIZE;
	off_t base = (off_t)sector * HOST_SECTOR_SIZE;
	size_t done = 0;
	while (done < total) {
		ssize_t n = pwrite(host_img_fd, src + done, total - done, base + (off_t)done);
		if (n <= 0) {
			return DELUGE_ERR_IO;
		}
		done += (size_t)n;
	}
	return DELUGE_OK;
}
```

- [ ] **Step 2: Delete the stub bodies from `host_bsp.c`**

In `src/bsp/host/host_bsp.c`, delete exactly these two functions (lines 310-323 today):

```c
DelugeStatus deluge_block_read(uint8_t unit, uint8_t* dst, uint32_t sector, uint32_t count) {
	(void)unit;
	(void)dst;
	(void)sector;
	(void)count;
	return DELUGE_ERR_NODEV;
}
DelugeStatus deluge_block_write(uint8_t unit, const uint8_t* src, uint32_t sector, uint32_t count) {
	(void)unit;
	(void)src;
	(void)sector;
	(void)count;
	return DELUGE_ERR_NODEV;
}
```

Leave `deluge_block_sd_unit`, `deluge_block_init`, `deluge_block_ready`, `deluge_block_sector_count`, `deluge_block_sector_size`, `deluge_block_sync`, `deluge_block_poll_card_event` in `host_bsp.c` untouched — out of scope (§2 of the design doc).

- [ ] **Step 3: Verify the host build compiles**

Run: `./dbt build Debug --sim` (or this project's equivalent host-sim build command — confirm the exact invocation via `firmware build command` project convention if unsure).
Expected: build succeeds with no duplicate-symbol or undefined-reference errors. Nothing calls `deluge_block_read`/`write` yet, so no behavior change is observable at this step — this is a compile-only gate.

- [ ] **Step 4: Commit**

```bash
git add src/bsp/host/host_platform.c src/bsp/host/host_bsp.c
git commit -m "feat(block_device): implement deluge_block_read/write on the host BSP"
```

---

### Task 2: RZA1 BSP — `deluge_block_read`/`deluge_block_write`

**Files:**
- Modify: `src/RZA1/diskio.c` (add real `deluge_block_read`/`deluge_block_write`, keep `disk_read_without_streaming_first`/`disk_write_without_streaming_first` untouched until Task 4)

**Interfaces:**
- Produces: same signatures as Task 1, real on RZA1.

- [ ] **Step 1: Add the include and the two functions**

Add `#include "libdeluge/block_device.h"` to `src/RZA1/diskio.c`'s include block (alongside its existing `#include "libdeluge/system.h"` at line 45), then add these two functions immediately after the existing `disk_write_without_streaming_first` (after line 266, before the `/* Miscellaneous Functions */` comment at line 268):

```c
DelugeStatus deluge_block_read(uint8_t unit, uint8_t* dst, uint32_t sector, uint32_t count)
{
    (void)unit;
    BYTE err;

    if (currentlyAccessingCard)
    {
        if (ALPHA_OR_BETA_VERSION)
        {
            FREEZE_WITH_ERROR("E259");
        }
    }

    currentlyAccessingCard = 1;
    err = sd_read_sect(SD_PORT, dst, sector, count);
    currentlyAccessingCard = 0;

    return (err == 0) ? DELUGE_OK : DELUGE_ERR_IO;
}

DelugeStatus deluge_block_write(uint8_t unit, const uint8_t* src, uint32_t sector, uint32_t count)
{
    (void)unit;
    BYTE err;

    if (currentlyAccessingCard)
    {
        if (ALPHA_OR_BETA_VERSION)
        {
            FREEZE_WITH_ERROR("E258");
        }
    }

    currentlyAccessingCard = 1;
    err = sd_write_sect(SD_PORT, src, sector, count, 0x0001u);
    currentlyAccessingCard = 0;

    return (err == 0) ? DELUGE_OK : DELUGE_ERR_IO;
}
```

This is a direct copy of the existing `disk_read_without_streaming_first`/`disk_write_without_streaming_first` bodies (same file, same `currentlyAccessingCard`/`SD_PORT`/`sd_read_sect`/`sd_write_sect`/`FREEZE_WITH_ERROR` already in scope — no new includes needed beyond `block_device.h` itself), with `DRESULT`/`RES_OK`/`RES_ERROR` replaced by `DelugeStatus`/`DELUGE_OK`/`DELUGE_ERR_IO`.

- [ ] **Step 2: Verify the ARM cross-build compiles**

Run this project's ARM firmware build command (per the `firmware build command` convention: `dbt build Debug`, not raw cmake).
Expected: build succeeds. No host-buildable verification is possible for this BSP — per this project's standing convention, do not add a manual hardware-verification checklist item; the existing hardware gate applies without prompting for it.

- [ ] **Step 3: Commit**

```bash
git add src/RZA1/diskio.c
git commit -m "feat(block_device): implement deluge_block_read/write on the RZA1 BSP"
```

---

### Task 3: In-tree Rust BSP — `deluge_block_read`/`deluge_block_write`

**Files:**
- Modify: `src/bsp/rust/src/sd.rs` (add real `deluge_block_read`/`deluge_block_write`, keep `disk_read_without_streaming_first`/`disk_write_without_streaming_first` untouched until Task 4)

**Interfaces:**
- Produces: same two functions as Tasks 1-2, real on the in-tree Rust BSP, `#[unsafe(no_mangle)] pub extern "C" fn`.

- [ ] **Step 1: Extend the top-of-file `use crate::sys::{...}` import block**

`sd.rs` currently imports (lines 20-23):

```rust
use crate::sys::{
    DelugeCardEvent, DelugeCardEvent_DELUGE_CARD_EVENT_EJECTED as CARD_EJECTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_INSERTED as CARD_INSERTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_NONE as CARD_NONE,
};
```

Change to also import `DelugeStatus` and the status constants this task needs (matching the existing `EnumName_VARIANT as ALIAS` bindgen convention):

```rust
use crate::sys::{
    DelugeCardEvent, DelugeCardEvent_DELUGE_CARD_EVENT_EJECTED as CARD_EJECTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_INSERTED as CARD_INSERTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_NONE as CARD_NONE,
    DelugeStatus, DelugeStatus_DELUGE_ERR_IO as DELUGE_ERR_IO,
    DelugeStatus_DELUGE_ERR_NODEV as DELUGE_ERR_NODEV,
    DelugeStatus_DELUGE_ERR_WRITE_PROTECTED as DELUGE_ERR_WRITE_PROTECTED,
    DelugeStatus_DELUGE_OK as DELUGE_OK,
};
```

If bindgen's generated constant names differ slightly from this guess (verify against `crate::sys`'s generated bindings — `cargo doc` or grepping the generated bindings file for `DelugeStatus_` is the fastest check), adjust the aliases to match; the shape (one `EnumType_VARIANT as ALIAS` per constant) is what must be preserved.

- [ ] **Step 2: Add the two functions after the existing `disk_write_without_streaming_first`**

Insert after line 211 (before the existing `disk_timerproc` function):

```rust
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_read(unit: u8, dst: *mut u8, sector: u32, count: u32) -> DelugeStatus {
    if unit != 0 || !sd::is_ready() {
        return DELUGE_ERR_NODEV;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `dst` holds `count` sectors.
    let out = unsafe { core::slice::from_raw_parts_mut(dst, len) };
    match block_on(sd::read_sectors(sector, count, out)) {
        Ok(()) => DELUGE_OK,
        Err(_) => DELUGE_ERR_IO,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_write(unit: u8, src: *const u8, sector: u32, count: u32) -> DelugeStatus {
    if unit != 0 || !sd::is_ready() {
        return DELUGE_ERR_NODEV;
    }
    if sd::is_write_protected() {
        return DELUGE_ERR_WRITE_PROTECTED;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `src` holds `count` sectors.
    let data = unsafe { core::slice::from_raw_parts(src, len) };
    match block_on(sd::write_sectors(sector, count, data)) {
        Ok(()) => DELUGE_OK,
        Err(e) => {
            log::warn!("deluge_block_write err {e:?} (sector={sector} count={count})");
            DELUGE_ERR_IO
        }
    }
}
```

- [ ] **Step 3: Verify the Rust BSP builds**

Run: `cargo build` (per this BSP's build process), then per `[[rust BSP C++ rebuild quirk]]` project convention, force `cmake --build ... --target deluge_app` if verifying the combined build, since `cargo build` alone doesn't track C++ source changes (not relevant here since this task is Rust-only, but worth confirming the crate itself builds clean: `cargo check` at minimum).
Expected: no compile errors; `deluge_block_read`/`deluge_block_write` are new exported symbols. No host-buildable verification applies to this BSP.

- [ ] **Step 4: Commit**

```bash
git add src/bsp/rust/src/sd.rs
git commit -m "feat(block_device): implement deluge_block_read/write on the in-tree Rust BSP"
```

---

### Task 4: Switch the FatFS diskio glue to `deluge_block_read`/`write`, retire the ad-hoc pair

**Files:**
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp:48-88`
- Modify: `src/bsp/host/host_platform.c` (delete the now-fully-unreferenced `disk_read_without_streaming_first`/`disk_write_without_streaming_first`)
- Modify: `src/RZA1/diskio.c` (same deletion)
- Modify: `src/bsp/rust/src/sd.rs` (same deletion)

**Interfaces:**
- Consumes: `deluge_block_read`/`deluge_block_write` (Tasks 1-3, all 3 BSPs).
- Produces: `disk_read`/`disk_write` (the FatFS diskio.h porting functions) now call `deluge_block_read`/`write` instead of the ad-hoc pair — this is the point where `TODO.md`'s "deluge-stream boundary" entry's `block_device.h` half is actually resolved.

- [ ] **Step 1: Rewrite `audio_file_manager.cpp`'s diskio glue block**

Replace this exact block (lines 48-88 today):

```cpp
extern "C" {
#include "fatfs/diskio.h"
#include "fatfs/ff.h"

DWORD get_fat_from_fs(                      /* 0xFFFFFFFF:Disk error, 1:Internal error, 2..0x7FFFFFFF:Cluster status */
                      FATFS* fs, DWORD clst /* Cluster number to get the value */
);

LBA_t clst2sect(           /* !=0:Sector number, 0:Failed (invalid cluster#) */
                FATFS* fs, /* Filesystem object */
                DWORD clst /* Cluster# to be converted */
);

DRESULT disk_read_without_streaming_first(BYTE pdrv, BYTE* buff, DWORD sector, UINT count);
DRESULT disk_write_without_streaming_first(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count);

extern uint8_t currentlyAccessingCard;
extern int32_t pendingGlobalMIDICommandNumClustersWritten;
extern int currentlySearchingForCluster;

// FatFs porting symbols. Service the audio cluster-streaming queue before every FatFs
// sector access (an app priority concern), then do the plain sector I/O. Inverts what
// used to be a HAL->app upcall (diskio.c calling loadAnyEnqueuedClustersRoutine): the
// streaming policy now lives in the app and calls *down* into the block device.
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	audioFileManager.loadAnyEnqueuedClusters(); // always ensure SD streaming is fulfilled first

	DRESULT result = disk_read_without_streaming_first(pdrv, buff, sector, count);

	if (currentlySearchingForCluster) {
		pendingGlobalMIDICommandNumClustersWritten++;
	}

	return result;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	audioFileManager.loadAnyEnqueuedClusters(); // always ensure SD streaming is fulfilled first
	return disk_write_without_streaming_first(pdrv, buff, sector, count);
}
}
```

with:

```cpp
extern "C" {
#include "fatfs/diskio.h"
#include "fatfs/ff.h"
#include "libdeluge/block_device.h"

DWORD get_fat_from_fs(                      /* 0xFFFFFFFF:Disk error, 1:Internal error, 2..0x7FFFFFFF:Cluster status */
                      FATFS* fs, DWORD clst /* Cluster number to get the value */
);

LBA_t clst2sect(           /* !=0:Sector number, 0:Failed (invalid cluster#) */
                FATFS* fs, /* Filesystem object */
                DWORD clst /* Cluster# to be converted */
);

extern uint8_t currentlyAccessingCard;
extern int32_t pendingGlobalMIDICommandNumClustersWritten;
extern int currentlySearchingForCluster;

// FatFs porting symbols. Service the audio cluster-streaming queue before every FatFs
// sector access (an app priority concern), then do the plain sector I/O via the
// libdeluge block-device boundary. Inverts what used to be a HAL->app upcall (diskio.c
// calling loadAnyEnqueuedClustersRoutine): the streaming policy lives in the app and
// calls *down* into the block device.
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	audioFileManager.loadAnyEnqueuedClusters(); // always ensure SD streaming is fulfilled first

	DelugeStatus status =
	    deluge_block_read(pdrv, reinterpret_cast<uint8_t*>(buff), static_cast<uint32_t>(sector), count);

	if (currentlySearchingForCluster) {
		pendingGlobalMIDICommandNumClustersWritten++;
	}

	return status == DELUGE_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	audioFileManager.loadAnyEnqueuedClusters(); // always ensure SD streaming is fulfilled first
	DelugeStatus status =
	    deluge_block_write(pdrv, reinterpret_cast<const uint8_t*>(buff), static_cast<uint32_t>(sector), count);
	return status == DELUGE_OK ? RES_OK : RES_ERROR;
}
}
```

Note `get_fat_from_fs`/`clst2sect`'s forward declarations stay — they're still used elsewhere in this file (`buildAudioFileFromCard`, the cold-path re-validation check) until Task 6.

- [ ] **Step 2: Delete the now-unreferenced `disk_read_without_streaming_first`/`disk_write_without_streaming_first` from all 3 BSPs**

In `src/bsp/host/host_platform.c`, delete the two functions added in Task 1's "keep untouched" note (originally lines 148-191 before Task 1's insert shifted line numbers — locate by function name, not line number).

In `src/RZA1/diskio.c`, delete the same two functions (originally lines 187-266).

In `src/bsp/rust/src/sd.rs`, delete the same two functions (originally lines 168-211), and remove `RES_OK`/`RES_ERROR`/`RES_NOTRDY`/`RES_WRPRT` constants (lines 30-33) **only if** nothing else in the file still references them — check first (`disk_ioctl` in the same file likely still uses `RES_OK`/`CTRL_SYNC` per the earlier grep; if so, leave the constants in place and only delete the two functions).

- [ ] **Step 3: Build host sim and run the golden-master sweep**

Run: the host-sim build (`./dbt build Debug --sim` or equivalent), then the golden-master runner (per `[[golden-master-harness]]`/`[[song-corpus-location]]` project convention: `export DELUGE_SONG_CORPUS=...` then `songs_run.py` or `param_record_run.py --binary`).
Expected: bit-exact match against the pre-existing baseline — this is a behavior-preserving relocation (host's `deluge_block_read`/`write` are byte-identical to the old `disk_read_without_streaming_first`/`write` bodies), so any diff here indicates a real regression, not an expected rebaseline.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/storage/audio/audio_file_manager.cpp src/bsp/host/host_platform.c src/RZA1/diskio.c src/bsp/rust/src/sd.rs
git commit -m "refactor(block_device): retire disk_*_without_streaming_first, route through deluge_block_read/write"
```

---

## Phase B: the `stream_io.h` boundary

### Task 5: `stream_io.h` + shared FatFS adapter (read side) + unit tests

**Files:**
- Create: `include/libdeluge/stream_io.h`
- Create: `src/fatfs/stream_io_internal.hpp`
- Create: `src/fatfs/stream_io.cpp`
- Create: `tests/spec/stream_io_spec.cpp`
- Modify: `tests/spec/CMakeLists.txt:21` (add `stream_io.cpp` to `deluge_spec`'s sources)
- Modify: `src/fatfs/CMakeLists.txt` (auto-generated — regenerate via `dbt buildgen`; if that command isn't available, manually add `stream_io.cpp` to the `add_library(fatfs STATIC ...)` list, matching `file_io.cpp`'s existing entry)

**Interfaces:**
- Produces: `deluge_stream_open`/`read_at`/`size`/`close`/`sector_of` (write-mode functions declared, return `DELUGE_ERR_UNSUPPORTED` until Task 7). Task 6 consumes `open`/`read_at`/`size`/`close`/`sector_of`.

- [ ] **Step 1: Write `include/libdeluge/stream_io.h`**

```c
/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

/// libdeluge/stream_io.h — real-time Sample audio streaming (playback read,
/// recording write).
///
/// Scoped specifically to `Sample` cluster streaming (`ClusterByteSource`,
/// `AudioFileManager::readClusterData`, `SampleRecorder`) -- not a general
/// file API. `WaveTable`/presets/songs stay on `file_io.h`. See
/// docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md.
///
/// `write_at`'s real contract is sequential append, not general random-access
/// write -- callers write whole clusters in increasing index order. `read_at`
/// reads exactly one cluster per call, from a cluster-aligned offset -- not a
/// general arbitrary-byte-range reader.
#ifndef LIBDELUGE_STREAM_IO_H
#define LIBDELUGE_STREAM_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// An open stream. Opaque; owned by the BSP implementation.
typedef struct DelugeStream DelugeStream;

typedef enum DelugeStreamMode {
	DELUGE_STREAM_READ,             ///< open an existing file; resolves full layout at open
	DELUGE_STREAM_WRITE_CREATE,     ///< create the file, truncating if it exists
	DELUGE_STREAM_WRITE_CREATE_NEW, ///< create the file; fails with DELUGE_ERR_EXISTS if it already exists
	DELUGE_STREAM_WRITE_APPEND,     ///< open an existing file for writing at its current size; no truncation
} DelugeStreamMode;

/// Open `path`. On success, `*out` is a handle the caller must eventually pass to
/// `deluge_stream_close`. [task]
DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out);

/// Read exactly `count` bytes at cluster-aligned `byte_offset` into `dst`.
/// `*out_read` is the number of bytes actually read. [task]
DelugeStatus deluge_stream_read_at(DelugeStream* stream, uint32_t byte_offset, void* dst, uint32_t count,
                                   uint32_t* out_read);

/// Append `count` bytes at `byte_offset` (must equal the stream's current end-of-file --
/// sequential append only). `*out_written` is the number of bytes actually written. [task]
DelugeStatus deluge_stream_write_at(DelugeStream* stream, uint32_t byte_offset, const void* src, uint32_t count,
                                    uint32_t* out_written);

/// Truncate (or, if smaller than the current position, no-op) the stream to `new_size` bytes. [task]
DelugeStatus deluge_stream_truncate(DelugeStream* stream, uint32_t new_size);

/// Total size of the stream, in bytes. [task]
DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size);

/// Close a stream opened with `deluge_stream_open`. `stream` is invalid after this call
/// regardless of the returned status. [task]
DelugeStatus deluge_stream_close(DelugeStream* stream);

/// Best-effort: the physical sector address backing cluster `cluster_index` (0-based).
/// In `DELUGE_STREAM_READ` mode, any already-resolved cluster index. In a write mode,
/// only the most recently `write_at`-completed cluster (returns `DELUGE_ERR_PARAM` for
/// any other index -- this boundary never keeps a full write-side layout table). Only
/// meaningful for sector-addressed backends (FatFS-family); a backend without sector
/// geometry (e.g. a future Linux/POSIX implementation) returns `DELUGE_ERR_UNSUPPORTED`.
/// Exists for two FatFS-specific, non-hot-path callers: `AudioFileManager`'s cold-path
/// "did the card's file change" identity re-validation on the read side, and
/// `SampleRecorder`'s per-cluster `sdAddress` bookkeeping on the write side -- the
/// real-time paths use `deluge_stream_read_at`/`write_at` exclusively. [task]
DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_STREAM_IO_H
```

- [ ] **Step 2: Write `src/fatfs/stream_io_internal.hpp`**

```cpp
#pragma once

#include "fatfs.hpp"
#include "libdeluge/stream_io.h"

namespace deluge::fatfs_adapter {

extern "C" {
DWORD get_fat_from_fs(FATFS* fs, DWORD clst);
LBA_t clst2sect(FATFS* fs, DWORD clst);
}

/// One resolved cluster's physical sector address.
struct StreamLayoutEntry {
	uint32_t sector = 0;
};

/// The opaque `DelugeStream` handle's real type. `layout` is a heap array of
/// `num_clusters` entries (READ mode only; nullptr in WRITE mode), resolved once
/// at open via the FAT chain walk (`resolve_read_layout`) so `read_at` never
/// re-walks the FAT.
struct StreamImpl {
	FatFS::File file;
	DelugeStreamMode mode;
	uint32_t cluster_size_bytes = 0;
	uint32_t num_clusters = 0;
	uint32_t file_size = 0;
	StreamLayoutEntry* layout = nullptr; // read mode only
	// Write modes only: the cluster index `write_at` most recently completed, or
	// UINT32_MAX if none yet. sector_of() only answers for this exact index -- see
	// stream_io.h's doc comment for why (no write-side layout table is kept).
	uint32_t last_written_cluster_index = 0xFFFFFFFFu;
};

/// Walks the FAT chain once for an already-open READ-mode `impl.file`, filling
/// `impl.layout`/`num_clusters`/`file_size`/`cluster_size_bytes`. Mirrors the
/// pre-boundary logic that used to live directly in
/// `AudioFileManager::buildAudioFileFromCard`, relocated here.
DelugeStatus resolve_read_layout(StreamImpl& impl);

} // namespace deluge::fatfs_adapter
```

- [ ] **Step 3: Write `src/fatfs/stream_io.cpp` (read side; write side stubbed)**

```cpp
#include "stream_io_internal.hpp"

#include "file_io_internal.hpp" // reuses deluge::fatfs_adapter::to_deluge_status(FatFS::Error)

#include <new>

namespace deluge::fatfs_adapter {

DelugeStatus resolve_read_layout(StreamImpl& impl) {
	FIL& fil = impl.file.inner();
	impl.file_size = static_cast<uint32_t>(fil.obj.objsize);
	impl.cluster_size_bytes = fil.obj.fs->csize * 512u;

	if (impl.file_size == 0 || impl.cluster_size_bytes == 0) {
		impl.num_clusters = 0;
		impl.layout = nullptr;
		return DELUGE_OK;
	}

	impl.num_clusters = (impl.file_size + impl.cluster_size_bytes - 1) / impl.cluster_size_bytes;
	impl.layout = new (std::nothrow) StreamLayoutEntry[impl.num_clusters];
	if (impl.layout == nullptr) {
		impl.num_clusters = 0;
		return DELUGE_ERR_NO_MEMORY;
	}

	uint32_t current_cluster = fil.obj.sclust;
	for (uint32_t i = 0; i < impl.num_clusters; i++) {
		impl.layout[i].sector = static_cast<uint32_t>(clst2sect(fil.obj.fs, current_cluster));
		if (i + 1 >= impl.num_clusters) {
			break;
		}
		current_cluster = get_fat_from_fs(fil.obj.fs, current_cluster);
		if (current_cluster == 0xFFFFFFFF || current_cluster < 2) {
			delete[] impl.layout;
			impl.layout = nullptr;
			impl.num_clusters = 0;
			return DELUGE_ERR_IO; // FAT chain shorter than the file's recorded size -- corrupted file
		}
	}
	return DELUGE_OK;
}

} // namespace deluge::fatfs_adapter

extern "C" {

DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out) {
	using namespace deluge::fatfs_adapter;

	if (mode != DELUGE_STREAM_READ) {
		return DELUGE_ERR_UNSUPPORTED; // Task 7 implements the write modes
	}

	auto opened = FatFS::File::open(path, FA_READ);
	if (!opened) {
		return to_deluge_status(opened.error());
	}

	auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
	if (impl == nullptr) {
		return DELUGE_ERR_NO_MEMORY;
	}

	DelugeStatus status = resolve_read_layout(*impl);
	if (status != DELUGE_OK) {
		delete impl;
		return status;
	}

	*out = reinterpret_cast<DelugeStream*>(impl);
	return DELUGE_OK;
}

DelugeStatus deluge_stream_read_at(DelugeStream* stream, uint32_t byte_offset, void* dst, uint32_t count,
                                   uint32_t* out_read) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_read = 0;

	if (impl->cluster_size_bytes == 0 || byte_offset % impl->cluster_size_bytes != 0
	    || count > impl->cluster_size_bytes) {
		return DELUGE_ERR_PARAM; // read_at reads exactly one cluster at a time, from a cluster-aligned offset
	}
	if (byte_offset + count > impl->file_size) {
		return DELUGE_ERR_PARAM;
	}

	uint32_t cluster_index = byte_offset / impl->cluster_size_bytes;
	if (cluster_index >= impl->num_clusters) {
		return DELUGE_ERR_PARAM;
	}

	uint32_t num_sectors = (count + 511u) / 512u;
	DelugeStatus status = deluge_block_read(deluge_block_sd_unit(), static_cast<uint8_t*>(dst),
	                                        impl->layout[cluster_index].sector, num_sectors);
	if (status != DELUGE_OK) {
		return status;
	}
	*out_read = count;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_write_at(DelugeStream* /*stream*/, uint32_t /*byte_offset*/, const void* /*src*/,
                                    uint32_t /*count*/, uint32_t* out_written) {
	*out_written = 0;
	return DELUGE_ERR_UNSUPPORTED; // Task 7
}

DelugeStatus deluge_stream_truncate(DelugeStream* /*stream*/, uint32_t /*new_size*/) {
	return DELUGE_ERR_UNSUPPORTED; // Task 7
}

DelugeStatus deluge_stream_size(DelugeStream* stream, uint32_t* out_size) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_size = impl->file_size;
	return DELUGE_OK;
}

DelugeStatus deluge_stream_close(DelugeStream* stream) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	delete[] impl->layout;
	auto result = impl->file.close();
	delete impl;
	if (!result) {
		return deluge::fatfs_adapter::to_deluge_status(result.error());
	}
	return DELUGE_OK;
}

DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	if (impl->mode != DELUGE_STREAM_READ || cluster_index >= impl->num_clusters) {
		return DELUGE_ERR_PARAM;
	}
	*out_sector = impl->layout[cluster_index].sector;
	return DELUGE_OK;
}

} // extern "C"
```

- [ ] **Step 4: Write `tests/spec/stream_io_spec.cpp`**

```cpp
// tests/spec/stream_io_spec.cpp
#include "fatfs/stream_io_internal.hpp"

#include "cppspec.hpp"

// clang-format off
describe stream_io("stream_io adapter", $ {
	it("returns DELUGE_ERR_UNSUPPORTED for DELUGE_STREAM_WRITE_CREATE (not yet implemented)", _ {
		DelugeStream* stream = nullptr;
		DelugeStatus status = deluge_stream_open("SAMPLES/TEST.WAV", DELUGE_STREAM_WRITE_CREATE, &stream);
		expect(status).to_equal(DELUGE_ERR_UNSUPPORTED);
	});

	it("returns DELUGE_ERR_UNSUPPORTED for DELUGE_STREAM_WRITE_CREATE_NEW (not yet implemented)", _ {
		DelugeStream* stream = nullptr;
		DelugeStatus status = deluge_stream_open("SAMPLES/TEST.WAV", DELUGE_STREAM_WRITE_CREATE_NEW, &stream);
		expect(status).to_equal(DELUGE_ERR_UNSUPPORTED);
	});

	it("resolve_read_layout on a zero-size file needs no FAT walk", _ {
		// open_by_locator is pure field construction (no I/O), same precedent as file_io_spec.cpp's
		// "open_by_locator constructs a File with exactly the given locator fields" test -- safe to call
		// against an unmounted fake FATFS because objsize=0 makes resolve_read_layout return before it
		// ever touches fs->csize or walks the FAT chain.
		FATFS fakeFs{};
		fakeFs.csize = 8; // must be non-zero, but resolve_read_layout must not read it for a 0-size file
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/0);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};

		DelugeStatus status = deluge::fatfs_adapter::resolve_read_layout(impl);
		expect(status).to_equal(DELUGE_OK);
		expect(impl.num_clusters).to_equal(0u);
		expect(impl.layout == nullptr).to_equal(true);
	});

	it("read_at rejects a non-cluster-aligned byte_offset without touching the block device", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/512);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.cluster_size_bytes = 512;
		impl.num_clusters = 1;
		impl.file_size = 512;
		impl.layout = layout;

		uint8_t dst[64];
		uint32_t out_read = 999;
		DelugeStatus status = deluge_stream_read_at(reinterpret_cast<DelugeStream*>(&impl), 7, dst, 64, &out_read);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		expect(out_read).to_equal(0u);
		impl.layout = nullptr; // don't let ~StreamImpl (none defined, but file.close() runs) touch the stack array
	});

	it("read_at rejects a count larger than one cluster", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/1024);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.cluster_size_bytes = 512;
		impl.num_clusters = 2;
		impl.file_size = 1024;
		impl.layout = layout;

		uint8_t dst[600];
		uint32_t out_read = 999;
		DelugeStatus status = deluge_stream_read_at(reinterpret_cast<DelugeStream*>(&impl), 0, dst, 600, &out_read);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		impl.layout = nullptr;
	});

	it("sector_of returns DELUGE_ERR_PARAM for an out-of-range cluster index", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/512);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.num_clusters = 1;
		impl.layout = layout;

		uint32_t sector = 0;
		DelugeStatus status = deluge_stream_sector_of(reinterpret_cast<DelugeStream*>(&impl), 5, &sector);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		impl.layout = nullptr;
	});

	it("sector_of returns the resolved sector for a valid cluster index", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[2] = {{.sector = 1000}, {.sector = 1008}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/1024);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.num_clusters = 2;
		impl.layout = layout;

		uint32_t sector = 0;
		DelugeStatus status = deluge_stream_sector_of(reinterpret_cast<DelugeStream*>(&impl), 1, &sector);
		expect(status).to_equal(DELUGE_OK);
		expect(sector).to_equal(1008u);
		impl.layout = nullptr;
	});
});

CPPSPEC_SPEC(stream_io)
```

Note the last five tests manually construct a `StreamImpl` (rather than going through `deluge_stream_open`, which would need a real mounted disk this test target doesn't have) and set `impl.layout = nullptr` before each `StreamImpl` goes out of scope, since `impl.layout` here points to a stack array, not a `new[]`-allocated one `deluge_stream_close`/`resolve_read_layout`'s error paths would `delete[]` — matching this file's own local variables' lifetime, not the production `deluge_stream_close` path (which isn't called by these tests at all).

- [ ] **Step 5: Register the new files in the test build**

In `tests/spec/CMakeLists.txt`, add `../../src/fatfs/stream_io.cpp` to the `deluge_spec` library's source list (line 21, immediately after `../../src/fatfs/file_io.cpp`). `stream_io_spec.cpp` needs no explicit registration — `create_specs_driver`'s `file(GLOB_RECURSE spec_sources ... *_spec.cpp)` picks it up automatically.

- [ ] **Step 6: Regenerate (or manually update) `src/fatfs/CMakeLists.txt` for the real firmware build**

Run: `dbt buildgen` (or this project's equivalent source-list regeneration command). If unavailable in the implementer's environment, manually add `stream_io.cpp` to the `add_library(fatfs STATIC ...)` list in `src/fatfs/CMakeLists.txt`, matching `file_io.cpp`'s existing entry exactly.

- [ ] **Step 7: Build and run the new tests**

Run: the `tests/spec` CppSpec target build + `ctest` (or `all_specs stream_io --verbose` directly, matching the `add_test` invocation in `tests/spec/CMakeLists.txt`).
Expected: all 7 new `stream_io` tests pass; existing `file_io` tests still pass (unaffected).

- [ ] **Step 8: Commit**

```bash
git add include/libdeluge/stream_io.h src/fatfs/stream_io_internal.hpp src/fatfs/stream_io.cpp \
        tests/spec/stream_io_spec.cpp tests/spec/CMakeLists.txt src/fatfs/CMakeLists.txt
git commit -m "feat(stream_io): add the stream_io.h boundary + shared FatFS read-side implementation"
```

---

### Task 6: Migrate `audio_file_manager.cpp`'s read path onto `stream_io.h`

**Files:**
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (`buildAudioFileFromCard`'s `SAMPLE` branch, `readClusterData`)
- Modify: `src/deluge/model/sample/sample.h` (add a `DelugeStream* readStream_` member)
- Modify: `src/deluge/model/sample/sample.cpp` (`~Sample()` closes it)

**Interfaces:**
- Consumes: `deluge_stream_open`/`read_at`/`sector_of`/`close` (Task 5).
- Produces: `Sample::readStream_` — a private member other tasks don't touch.

- [ ] **Step 1: Add `readStream_` to `Sample`**

In `src/deluge/model/sample/sample.h`, add near the other private/lifecycle members (alongside `resourceAssetId` at line 182):

```cpp
	// Opened once by AudioFileManager::buildAudioFileFromCard (DELUGE_STREAM_READ mode), used by
	// readClusterData for every cluster read thereafter; closed in ~Sample. nullptr for a Sample
	// that isn't backed by a stream_io.h read (e.g. one still being recorded).
	DelugeStream* readStream_ = nullptr;
```

Add `#include "libdeluge/stream_io.h"` to `sample.h`'s include block if not already present via a transitive include (check first; add directly if needed).

- [ ] **Step 2: Close it in `~Sample()`**

In `src/deluge/model/sample/sample.cpp`, at the top of `Sample::~Sample()` (before the existing `resourceAssetId` release block at line 221):

```cpp
Sample::~Sample() {
	if (readStream_ != nullptr) {
		deluge_stream_close(readStream_);
		readStream_ = nullptr;
	}

	// Retire our Asset first (frees any clusters the manager still has resident, via
	// ...
```

- [ ] **Step 3: Replace `buildAudioFileFromCard`'s raw FAT-walk block**

In `src/deluge/storage/audio/audio_file_manager.cpp`, replace this exact block (the `SAMPLE` branch's cluster-address loop, lines 804-822 today):

```cpp
		// Go directly to god-mode and store the address of each of the file's clusters.
		uint32_t currentClusterIndex = 0;
		uint32_t currentSDCluster = effectiveFilePointer.sclust; // First cluster, whose address we already got.
		while (true) {
			static_cast<Sample*>(audioFile)->clusters[currentClusterIndex].sdAddress =
			    clst2sect(&fileSystem, currentSDCluster);

			currentClusterIndex++;
			if (currentClusterIndex >= numClusters) {
				break;
			}
			currentSDCluster = get_fat_from_fs(&fileSystem, currentSDCluster);
			if (currentSDCluster == 0xFFFFFFFF || currentSDCluster < 2) {
				break;
			}
		}

		// The byte source streams the clusters; its destructor releases the held cluster's reason.
		ClusterByteSource source{static_cast<Sample&>(*audioFile), effectiveFilePointer.objsize};
		*error = audioFile->loadFile(source, makeWaveTableWorkAtAllCosts);
```

with:

```cpp
		// Open the stream_io.h boundary once for this Sample's lifetime; readClusterData (called
		// per-cluster during playback) reads through it. sdAddress stays populated too -- it feeds
		// AudioFileManager's cold-path "did the card's file change" re-validation check, a separate,
		// FatFS-specific concern outside the real-time read path.
		Sample* sampleFile = static_cast<Sample*>(audioFile);
		DelugeStream* stream = nullptr;
		DelugeStatus streamStatus = deluge_stream_open(filePath.c_str(), DELUGE_STREAM_READ, &stream);
		if (streamStatus != DELUGE_OK) {
			*error = Error::FILE_NOT_FOUND;
			destroyAudioFileObject(*audioFile);
			return nullptr;
		}
		sampleFile->readStream_ = stream;
		for (uint32_t i = 0; i < numClusters; i++) {
			uint32_t sector = 0;
			(void)deluge_stream_sector_of(stream, i, &sector); // best-effort; only meaningful on FatFS-family backends
			sampleFile->clusters[i].sdAddress = sector;
		}

		// The byte source streams the clusters; its destructor releases the held cluster's reason.
		ClusterByteSource source{*sampleFile, effectiveFilePointer.objsize};
		*error = audioFile->loadFile(source, makeWaveTableWorkAtAllCosts);
```

`effectiveFilePointer`/`resolveFilePointer`/`tryRegularPath` still resolve `filePath` and `effectiveFilePointer.objsize` upstream of this branch (unchanged — `objsize` is still needed for the 0-byte/too-big checks earlier in this function) but `effectiveFilePointer.sclust` is no longer read anywhere in this branch.

- [ ] **Step 4: Replace `readClusterData`'s raw sector read**

Replace this line (line ~985-986 today):

```cpp
	DRESULT result = disk_read_without_streaming_first(deluge_block_sd_unit(), (BYTE*)cluster.data,
	                                                   sample->clusters[cluster.clusterIndex].sdAddress, numSectors);
```

with:

```cpp
	uint32_t bytesRequested = static_cast<uint32_t>(numSectors) * 512u;
	uint32_t bytesRead = 0;
	DelugeStatus streamStatus = deluge_stream_read_at(
	    sample->readStream_, static_cast<uint32_t>(cluster.clusterIndex) << Cluster::size_magnitude,
	    cluster.data, bytesRequested, &bytesRead);
	DRESULT result = (streamStatus == DELUGE_OK) ? 0u : 1u;
```

The surrounding code (the `if (result != 0u) { goto getOutEarly; }` a few lines below, and everything after it) is untouched — `result`'s type/success-check contract (`0` = success) is preserved exactly, only how it's produced changes.

- [ ] **Step 5: Build host sim and run the golden-master sweep (read side)**

Run: host-sim build + golden-master runner, same invocation as Task 4 Step 3.
Expected: bit-exact match — this replaces the raw FAT-walk/`disk_read_without_streaming_first` call with `stream_io.h`'s equivalent, which does the identical work (same FAT-chain walk, same sector reads via `deluge_block_read`, already verified real in Task 4) relocated one layer down. `sample->clusters[0].sdAddress`-dependent cold-path behavior (line 254) is unaffected since it's still populated identically.

- [ ] **Step 6: Commit**

```bash
git add src/deluge/storage/audio/audio_file_manager.cpp src/deluge/model/sample/sample.h src/deluge/model/sample/sample.cpp
git commit -m "refactor(audio_file_manager): migrate SAMPLE read path onto stream_io.h"
```

---

### Task 7: Implement `stream_io.cpp`'s write side + unit tests

**Files:**
- Modify: `src/fatfs/stream_io.cpp` (implement `DELUGE_STREAM_WRITE_CREATE*` open modes, `write_at`, `truncate`)
- Modify: `tests/spec/stream_io_spec.cpp` (add write-side tests)

**Interfaces:**
- Consumes: nothing new (same `StreamImpl`, `deluge_block_write` from Task 1-3).
- Produces: `deluge_stream_open` (write modes), `deluge_stream_write_at`, `deluge_stream_truncate` — real. Task 8 consumes these.

- [ ] **Step 1: Implement write-mode open**

In `stream_io_internal.hpp`, extend `to_fatfs_mode`-equivalent logic inline (no need for a shared helper — `stream_io.h`'s two write modes map 1:1 to `file_io.h`'s, but this boundary's modes are a distinct C enum, so map directly):

Replace `deluge_stream_open`'s write-mode early-return in `stream_io.cpp` (from Task 5 Step 3):

```cpp
DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out) {
	using namespace deluge::fatfs_adapter;

	if (mode == DELUGE_STREAM_READ) {
		auto opened = FatFS::File::open(path, FA_READ);
		if (!opened) {
			return to_deluge_status(opened.error());
		}
		auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
		if (impl == nullptr) {
			return DELUGE_ERR_NO_MEMORY;
		}
		DelugeStatus status = resolve_read_layout(*impl);
		if (status != DELUGE_OK) {
			delete impl;
			return status;
		}
		*out = reinterpret_cast<DelugeStream*>(impl);
		return DELUGE_OK;
	}

	// DELUGE_STREAM_WRITE_CREATE / _CREATE_NEW / _APPEND
	FatFS::FileAccessMode fatfsMode = FA_WRITE;
	if (mode == DELUGE_STREAM_WRITE_CREATE) {
		fatfsMode |= FA_CREATE_ALWAYS;
	}
	else if (mode == DELUGE_STREAM_WRITE_CREATE_NEW) {
		fatfsMode |= FA_CREATE_NEW;
	}
	// DELUGE_STREAM_WRITE_APPEND: FA_WRITE alone -- opens an existing file at its
	// current size, no truncation. Used by SampleRecorder::finalizeRecordedFile to
	// reopen a file whose already-recorded audio (up to the eventual trim point) must
	// survive the reopen -- DELUGE_STREAM_WRITE_CREATE's truncate-on-open would destroy
	// it before truncateFileDownToSize ever runs.
	auto opened = FatFS::File::open(path, fatfsMode);
	if (!opened) {
		return to_deluge_status(opened.error());
	}
	auto* impl = new (std::nothrow) StreamImpl{std::move(opened.value()), mode};
	if (impl == nullptr) {
		return DELUGE_ERR_NO_MEMORY;
	}
	impl->cluster_size_bytes = impl->file.inner().obj.fs->csize * 512u;
	// WRITE_APPEND opens an existing file -- its real current size; WRITE_CREATE*
	// always starts a fresh, empty file.
	impl->file_size = (mode == DELUGE_STREAM_WRITE_APPEND) ? static_cast<uint32_t>(impl->file.inner().obj.objsize) : 0;
	impl->num_clusters = 0;
	impl->layout = nullptr; // write modes never populate the read-side layout table
	*out = reinterpret_cast<DelugeStream*>(impl);
	return DELUGE_OK;
}
```

- [ ] **Step 2: Implement `write_at`**

Replace the write_at stub from Task 5:

```cpp
DelugeStatus deluge_stream_write_at(DelugeStream* stream, uint32_t byte_offset, const void* src, uint32_t count,
                                    uint32_t* out_written) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	*out_written = 0;

	if (byte_offset != impl->file_size) {
		return DELUGE_ERR_PARAM; // sequential append only -- see stream_io.h's contract note
	}

	// FatFS owns cluster allocation -- a normal buffered write extends the file and allocates as
	// needed. sector_of() reads the resulting cluster's address on demand from this same `file`
	// afterward, matching SampleRecorder's pre-boundary logic (clst2sect off the FIL's live .clust).
	auto written = impl->file.write(std::span{const_cast<std::byte*>(static_cast<const std::byte*>(src)),
	                                          static_cast<size_t>(count)});
	if (!written || written->size() != count) {
		return DELUGE_ERR_IO;
	}

	impl->file_size += count;
	impl->last_written_cluster_index = byte_offset / impl->cluster_size_bytes;
	*out_written = count;
	return DELUGE_OK;
}
```

- [ ] **Step 3.5: Extend `sector_of` to cover write modes**

Replace `deluge_stream_sector_of` (written in Task 5 Step 3) with:

```cpp
DelugeStatus deluge_stream_sector_of(DelugeStream* stream, uint32_t cluster_index, uint32_t* out_sector) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	if (impl->mode == DELUGE_STREAM_READ) {
		if (cluster_index >= impl->num_clusters) {
			return DELUGE_ERR_PARAM;
		}
		*out_sector = impl->layout[cluster_index].sector;
		return DELUGE_OK;
	}
	// Write modes: only the cluster write_at() most recently completed -- no write-side
	// layout table is kept (see stream_io.h's doc comment).
	if (cluster_index != impl->last_written_cluster_index) {
		return DELUGE_ERR_PARAM;
	}
	FIL& fil = impl->file.inner();
	*out_sector = static_cast<uint32_t>(deluge::fatfs_adapter::clst2sect(fil.obj.fs, fil.obj.clust));
	return DELUGE_OK;
}
```

- [ ] **Step 3: Implement `truncate`**

```cpp
DelugeStatus deluge_stream_truncate(DelugeStream* stream, uint32_t new_size) {
	auto* impl = reinterpret_cast<deluge::fatfs_adapter::StreamImpl*>(stream);
	auto seeked = impl->file.lseek(new_size);
	if (!seeked) {
		return deluge::fatfs_adapter::to_deluge_status(seeked.error());
	}
	auto truncated = impl->file.truncate();
	if (!truncated) {
		return deluge::fatfs_adapter::to_deluge_status(truncated.error());
	}
	impl->file_size = new_size;
	return DELUGE_OK;
}
```

- [ ] **Step 4: Add write-side unit tests**

Append to `tests/spec/stream_io_spec.cpp`, inside the `describe stream_io(...)` block, before the closing `});`:

```cpp
	it("write_at rejects a byte_offset that doesn't match the current file size (append-only contract)", _ {
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/0);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_WRITE_CREATE};
		impl.file_size = 512; // pretend 512 bytes are already written

		uint8_t src[64] = {};
		uint32_t out_written = 999;
		DelugeStatus status =
		    deluge_stream_write_at(reinterpret_cast<DelugeStream*>(&impl), 0 /* wrong -- should be 512 */, src, 64,
		                           &out_written);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		expect(out_written).to_equal(0u);
	});
```

(A real successful `write_at`/`truncate` round-trip needs a mounted disk this test target doesn't have — same accepted gap as `file_io_spec.cpp`'s `createFile` note. Real write-side correctness is verified in Task 8 via the host sim, per the design doc §8's explicit note that the write side has no automated regression coverage today and this task doesn't newly promise one beyond this parameter-validation check.)

- [ ] **Step 5: Build and run tests**

Run: same as Task 5 Step 7.
Expected: all `stream_io` tests pass, including the new one.

- [ ] **Step 6: Commit**

```bash
git add src/fatfs/stream_io.cpp tests/spec/stream_io_spec.cpp
git commit -m "feat(stream_io): implement the write side (write_at/truncate)"
```

---

### Task 8: Migrate `sample_recorder.cpp` onto `stream_io.h`, delete `createFileRaw`

**Files:**
- Modify: `src/deluge/model/sample/sample_recorder.h:145` (`file` member type)
- Modify: `src/deluge/model/sample/sample_recorder.cpp` (`writeCluster`, `finalizeRecordedFile`/`truncateFileDownToSize`, the file-creation call site)
- Modify: `src/deluge/storage/storage_manager.h:365` (delete `createFileRaw` declaration)
- Modify: `src/deluge/storage/storage_manager.cpp:179-...` (delete `createFileRaw` definition)

**Interfaces:**
- Consumes: `deluge_stream_open` (write modes), `deluge_stream_write_at`, `deluge_stream_truncate`, `deluge_stream_close` (Task 7).
- Produces: nothing new — this is the terminal consumer for the write side.

- [ ] **Step 1: Change `SampleRecorder::file`'s type**

In `src/deluge/model/sample/sample_recorder.h:145`, replace:

```cpp
	std::optional<FatFS::File> file{};
```

with:

```cpp
	DelugeStream* file = nullptr;
```

Add `#include "libdeluge/stream_io.h"` to this header's include block if not already present (replacing whatever pulled in `FatFS::File` there, if that include becomes otherwise-unused — check before removing).

- [ ] **Step 2: Replace the file-creation call site**

In `sample_recorder.cpp` (the block around today's line 484, shown in the design doc's background section):

```cpp
			// Recording could finish or abort during this!
			auto created = StorageManager::createFileRaw(filePathCreated.c_str(), mayOverwrite);
			if (!created) {
				filePathCreated.clear();
				goto gotError;
			}
			else {
				this->file = created.value();
			}
```

becomes:

```cpp
			// Recording could finish or abort during this!
			DelugeStream* stream = nullptr;
			DelugeStatus streamStatus = deluge_stream_open(
			    filePathCreated.c_str(), mayOverwrite ? DELUGE_STREAM_WRITE_CREATE : DELUGE_STREAM_WRITE_CREATE_NEW,
			    &stream);
			if (streamStatus != DELUGE_OK) {
				filePathCreated.clear();
				goto gotError;
			}
			else {
				this->file = stream;
			}
```

- [ ] **Step 3: Replace `writeCluster`'s raw write**

Replace (line ~824-851 today):

```cpp
Error SampleRecorder::writeCluster(int32_t clusterIndex, size_t numBytes) {
	// D_PRINTLN("writeCluster");

	SampleCluster* sampleCluster = &sample->clusters[clusterIndex];

	auto written = file->write({(std::byte*)sampleCluster->cluster->data, numBytes});
	if (!written || numBytes != written.value()) {
		return Error::SD_CARD;
	}

	// MUST re-get this - while writing above, the audio routine is being called, and that could
	// allocate new SampleClusters and move them around!
	sampleCluster = &sample->clusters[clusterIndex];

	// Grab the SD address, for later
	sampleCluster->sdAddress = clst2sect(&fileSystem, file->inner().clust);

	// Now flushed to the card with its sdAddress recorded, this cluster is reconstructable like any
	// sample cluster (materialize re-reads it) — so clear dirty, letting the manager evict + reload
	// it under pressure. (Until now it was held dirty so the unflushed audio could not be evicted.)
	if (sampleCluster->cluster != nullptr) {
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_mark_dirty(mgr, sampleCluster->cluster, false);
		}
	}
	return Error::NONE;
}
```

with:

```cpp
Error SampleRecorder::writeCluster(int32_t clusterIndex, size_t numBytes) {
	SampleCluster* sampleCluster = &sample->clusters[clusterIndex];

	uint32_t byteOffset = static_cast<uint32_t>(clusterIndex) << Cluster::size_magnitude;
	uint32_t bytesWritten = 0;
	DelugeStatus status = deluge_stream_write_at(file, byteOffset, sampleCluster->cluster->data,
	                                             static_cast<uint32_t>(numBytes), &bytesWritten);
	if (status != DELUGE_OK || bytesWritten != numBytes) {
		return Error::SD_CARD;
	}

	// MUST re-get this - while writing above, the audio routine is being called, and that could
	// allocate new SampleClusters and move them around!
	sampleCluster = &sample->clusters[clusterIndex];

	uint32_t sector = 0;
	(void)deluge_stream_sector_of(file, static_cast<uint32_t>(clusterIndex), &sector);
	sampleCluster->sdAddress = sector;

	// Now flushed to the card with its sdAddress recorded, this cluster is reconstructable like any
	// sample cluster (materialize re-reads it) — so clear dirty, letting the manager evict + reload
	// it under pressure. (Until now it was held dirty so the unflushed audio could not be evicted.)
	if (sampleCluster->cluster != nullptr) {
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_mark_dirty(mgr, sampleCluster->cluster, false);
		}
	}
	return Error::NONE;
}
```

`deluge_stream_sector_of` returns the address of whichever cluster `write_at` most recently completed (Task 7 Step 3.5) — since `writeCluster` always calls `sector_of` immediately after the matching `write_at` for the same `clusterIndex`, this always matches.

- [ ] **Step 4: Replace `finalizeRecordedFile`'s reopen/truncate**

Replace (lines ~1528-1544 today):

```cpp
			if (action != MonitoringAction::NONE || capturedTooMuch) {

				deluge_file_invalidate_cache();
				auto opened = this->file->open(sample->filePath.c_str(), FA_WRITE);

				if (!opened) {
					return Error::SD_CARD;
				}
				this->file = opened.value();

				Error error = truncateFileDownToSize(dataLengthAfterAction + sample->audioDataStartPosBytes);
				if (error != Error::NONE) {
					return error;
				}

				auto closed = this->file->close();
				if (!closed) {
					return Error::SD_CARD;
				}
			}
```

with:

```cpp
			if (action != MonitoringAction::NONE || capturedTooMuch) {

				DelugeStatus reopenStatus =
				    deluge_stream_open(sample->filePath.c_str(), DELUGE_STREAM_WRITE_APPEND, &this->file);
				if (reopenStatus != DELUGE_OK) {
					return Error::SD_CARD;
				}

				Error error = truncateFileDownToSize(dataLengthAfterAction + sample->audioDataStartPosBytes);
				if (error != Error::NONE) {
					return error;
				}

				DelugeStatus closeStatus = deluge_stream_close(this->file);
				this->file = nullptr;
				if (closeStatus != DELUGE_OK) {
					return Error::SD_CARD;
				}
			}
```

This uses `DELUGE_STREAM_WRITE_APPEND` (Task 7 Step 1), not `WRITE_CREATE` — the file's already-recorded audio up to the eventual trim point must survive this reopen; `WRITE_CREATE`'s truncate-on-open would zero it before `truncateFileDownToSize`'s own `lseek`+`truncate` ever ran. This mirrors the original raw `FA_WRITE`-only reopen exactly (open at current size, no truncation), which is why `stream_io.h` needed a third mode instead of reusing the other two.

- [ ] **Step 5: Replace `truncateFileDownToSize`**

Replace (lines ~1563-1584 today):

```cpp
Error SampleRecorder::truncateFileDownToSize(uint32_t newFileSize) {

	// Update the Sample object to indicate the correct size. Do this before we risk errors below

	uint64_t numClustersAfterAction = ((newFileSize - 1) >> Cluster::size_magnitude) + 1;

	if (numClustersAfterAction < sample->clusters.size()) {
		sample->clusters.erase(sample->clusters.begin() + numClustersAfterAction, sample->clusters.end());
	}

	// Truncate file size
	auto seeked = file->lseek(newFileSize);
	if (!seeked) {
		return Error::SD_CARD;
	}
	auto truncated = file->truncate();
	if (!truncated) {
		return Error::SD_CARD;
	}

	return Error::NONE;
}
```

with:

```cpp
Error SampleRecorder::truncateFileDownToSize(uint32_t newFileSize) {

	// Update the Sample object to indicate the correct size. Do this before we risk errors below

	uint64_t numClustersAfterAction = ((newFileSize - 1) >> Cluster::size_magnitude) + 1;

	if (numClustersAfterAction < sample->clusters.size()) {
		sample->clusters.erase(sample->clusters.begin() + numClustersAfterAction, sample->clusters.end());
	}

	DelugeStatus status = deluge_stream_truncate(file, newFileSize);
	if (status != DELUGE_OK) {
		return Error::SD_CARD;
	}

	return Error::NONE;
}
```

- [ ] **Step 6: Delete `StorageManager::createFileRaw`**

In `src/deluge/storage/storage_manager.h:365`, delete the declaration (and its preceding doc comment, lines ~361-364).

In `src/deluge/storage/storage_manager.cpp`, delete the full `createFileRaw` definition (lines 179-...; find the matching closing brace — it follows the same create-with-folder-retry shape as `createFile`, confirm the end of the function before deleting).

- [ ] **Step 7: Search for any other `createFileRaw`/`FatFS::File`-via-`SampleRecorder` references**

Run: `grep -rn "createFileRaw" src/` — expect zero remaining matches after Steps 2 and 6. Run: `grep -n "FatFS::File\|\.inner()\|clst2sect\|fileSystem" src/deluge/model/sample/sample_recorder.cpp` — expect zero remaining matches (everything in this file should now go through `deluge_stream_*`).

- [ ] **Step 8: Build host sim; manually verify a record → load round-trip**

Run: host-sim build. Since there's no automated recording test (design doc §8's accepted gap, unchanged by this task), manually exercise the sim's recording path (or write a small scripted host-sim session if this project has one) to record a short sample, then load it back and confirm it plays / matches expected length. This is the real regression check for this task — treat any crash, silence, or corrupted playback as a blocking finding, not a pre-existing gap to defer.

- [ ] **Step 9: Commit**

```bash
git add src/deluge/model/sample/sample_recorder.h src/deluge/model/sample/sample_recorder.cpp \
        src/deluge/storage/storage_manager.h src/deluge/storage/storage_manager.cpp
git commit -m "refactor(sample_recorder): migrate write path onto stream_io.h, delete createFileRaw"
```

---

### Task 9: Documentation cleanup

**Files:**
- Modify: `docs/dev/target_architecture.md:270`
- Modify: `TODO.md` (delete the "deluge-stream boundary (future, unscoped)" entry)

**Interfaces:** none — doc-only.

- [ ] **Step 1: Correct `target_architecture.md`'s stale `Stealable` line**

Replace line 270:

```
**deluge-stream** — audio-context storage: cluster cache, sample streaming, SD read
scheduling, `Stealable` implementations. Split from `deluge-files` because the two halves have
opposite realtime contracts (blocking-allowed vs audio-safe), which today share a directory and
a god-object.
```

with:

```
**deluge-stream** — audio-context storage: cluster cache, sample streaming, SD read
scheduling, resource-manager `Source`/`materialize`/`on_evict` wiring (`Stealable` is fully
retired -- see `crates/deluge_resource`). Split from `deluge-files` because the two halves have
opposite realtime contracts (blocking-allowed vs audio-safe), which today share a directory and
a god-object. Implemented via `include/libdeluge/stream_io.h`
(docs/superpowers/specs/2026-07-15-deluge-stream-boundary-design.md).
```

- [ ] **Step 2: Remove the resolved `TODO.md` entry**

Delete the full "deluge-stream boundary (future, unscoped)" bullet (line 4 of `TODO.md` today, the one starting `- [] deluge-stream boundary (future, unscoped): browser.cpp, sample_browser.cpp, ...`).

- [ ] **Step 3: Commit**

```bash
git add docs/dev/target_architecture.md TODO.md
git commit -m "docs: correct deluge-stream's Stealable reference, resolve the TODO.md entry"
```

---

## Phase C: idiomatic C++ wrapper (added post-implementation)

Tasks 1-9 above landed and were reviewed clean, including a final whole-branch review. During
that review's follow-up, a design gap was identified: `stream_io.h` has real open/close handle
lifecycle (unlike `block_device.h`, which was the (wrong) precedent Section "Global Constraints"
originally compared it to) — the same shape `file_io.h` has, which already got a move-only RAII
C++ wrapper (`deluge::io::File`, `src/deluge/io/file.hpp`/`.cpp`) specifically to eliminate
manual-close bookkeeping. `Sample::readStream_` and `SampleRecorder::file` are both currently
raw `DelugeStream*` with manual `deluge_stream_close()` calls at every site — this task gives
them the same treatment `File` already has, restoring the `std::optional<...>`-held-RAII-object
idiom `SampleRecorder::file` had before Task 8 (it was `std::optional<FatFS::File>` before that
task, and only became a raw pointer because no wrapper existed yet to hold in the optional).

**This is a pure refactor — no behavior change.** Every call this task touches already goes
through the exact same `stream_io.h` C-ABI functions underneath; the wrapper is a thin,
zero-cost RAII layer over calls that already exist and already work. Verification should
confirm nothing changed, not explore new behavior.

### Task 10: `deluge::io::Stream` wrapper + migrate `Sample`/`SampleRecorder` onto it

**Files:**
- Create: `src/deluge/io/stream.hpp`
- Create: `src/deluge/io/stream.cpp`
- Modify: `src/deluge/model/sample/sample.h` (`readStream_` becomes `std::optional<deluge::io::Stream>`)
- Modify: `src/deluge/model/sample/sample.cpp` (`~Sample()`'s manual close removed — the optional's destructor handles it)
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (`buildAudioFileFromCard`, `readClusterData`, `cardReinserted` — all three call sites touching `readStream_`/raw `deluge_stream_*` calls)
- Modify: `src/deluge/model/sample/sample_recorder.h` (`file` becomes `std::optional<deluge::io::Stream>`)
- Modify: `src/deluge/model/sample/sample_recorder.cpp` (`cardRoutine`'s creation, `writeCluster`, `finalizeRecordedFile`, `alterFile`'s reopen, `truncateFileDownToSize`)

**Interfaces:**
- Consumes: `include/libdeluge/stream_io.h`'s C-ABI (Tasks 5/7, already complete); `deluge::io::Status`/`to_status`/`to_deluge_status` (already declared in `src/deluge/io/file.hpp`/`.cpp` — reuse, don't duplicate).
- Produces: `deluge::io::Stream` — move-only RAII wrapper, `std::expected<T, deluge::io::Status>` returns.

- [ ] **Step 1: Write `src/deluge/io/stream.hpp`**

```cpp
#pragma once

#include "io/file.hpp" // reuses deluge::io::Status / to_status / to_deluge_status
#include "libdeluge/stream_io.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace deluge::io {

class Stream {
public:
	Stream(Stream&) = delete;
	Stream(Stream&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	Stream& operator=(Stream&) = delete;
	Stream& operator=(Stream&& other) noexcept {
		if (this != &other) {
			if (handle_) {
				deluge_stream_close(handle_);
			}
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~Stream() {
		if (handle_) {
			deluge_stream_close(handle_);
		}
	}

	[[nodiscard]] static std::expected<Stream, Status> open(std::string_view path, DelugeStreamMode mode);
	std::expected<std::span<std::byte>, Status> read_at(uint32_t byte_offset, std::span<std::byte> buffer);
	std::expected<uint32_t, Status> write_at(uint32_t byte_offset, std::span<const std::byte> buffer);
	std::expected<void, Status> truncate(uint32_t new_size);
	std::expected<uint32_t, Status> size();
	std::expected<uint32_t, Status> sector_of(uint32_t cluster_index);
	std::expected<void, Status> close();

private:
	explicit Stream(DelugeStream* handle) : handle_(handle) {}
	DelugeStream* handle_ = nullptr;
};

} // namespace deluge::io
```

- [ ] **Step 2: Write `src/deluge/io/stream.cpp`**

```cpp
#include "io/stream.hpp"

namespace deluge::io {

std::expected<Stream, Status> Stream::open(std::string_view path, DelugeStreamMode mode) {
	DelugeStream* handle = nullptr;
	DelugeStatus status = deluge_stream_open(path.data(), mode, &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return Stream(handle);
}

std::expected<std::span<std::byte>, Status> Stream::read_at(uint32_t byte_offset, std::span<std::byte> buffer) {
	uint32_t out_read = 0;
	DelugeStatus status =
	    deluge_stream_read_at(handle_, byte_offset, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_read);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return buffer.subspan(0, out_read);
}

std::expected<uint32_t, Status> Stream::write_at(uint32_t byte_offset, std::span<const std::byte> buffer) {
	uint32_t out_written = 0;
	DelugeStatus status =
	    deluge_stream_write_at(handle_, byte_offset, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_written);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_written;
}

std::expected<void, Status> Stream::truncate(uint32_t new_size) {
	DelugeStatus status = deluge_stream_truncate(handle_, new_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<uint32_t, Status> Stream::size() {
	uint32_t out_size = 0;
	DelugeStatus status = deluge_stream_size(handle_, &out_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_size;
}

std::expected<uint32_t, Status> Stream::sector_of(uint32_t cluster_index) {
	uint32_t out_sector = 0;
	DelugeStatus status = deluge_stream_sector_of(handle_, cluster_index, &out_sector);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_sector;
}

std::expected<void, Status> Stream::close() {
	DelugeStatus status = deluge_stream_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

} // namespace deluge::io
```

- [ ] **Step 3: Register the new files in the build**

Add `stream.cpp` to wherever `file.cpp` is registered for the `src/deluge/io/` sources (check the relevant `CMakeLists.txt`/source-list — mirror `file.cpp`'s exact entry). Also add to `tests/spec/CMakeLists.txt`'s `deluge_spec` source list if `file.cpp` is listed there too (check first — it may not need to be, if no spec test exercises the wrapper directly; a real mounted disk would be needed to meaningfully test it beyond what `stream_io_spec.cpp` already covers at the C-ABI level, so a dedicated wrapper-level spec test is not required by this task unless a cheap, real-logic test is obviously available).

- [ ] **Step 4: Migrate `Sample`/`audio_file_manager.cpp`'s read side onto the wrapper**

In `sample.h`, change:
```cpp
	DelugeStream* readStream_ = nullptr;
```
to:
```cpp
	std::optional<deluge::io::Stream> readStream_;
```
(add `#include "io/stream.hpp"` and `#include <optional>` as needed; remove the now-unneeded raw `#include "libdeluge/stream_io.h"` if nothing else in this header needs it directly — check first).

In `sample.cpp`'s `~Sample()`, remove the manual close block:
```cpp
	if (readStream_ != nullptr) {
		deluge_stream_close(readStream_);
		readStream_ = nullptr;
	}
```
entirely — `std::optional<deluge::io::Stream>`'s destructor now handles this automatically (a disengaged optional does nothing; an engaged one destructs its `Stream`, which closes the handle).

In `audio_file_manager.cpp`:
- `buildAudioFileFromCard`: replace the raw `deluge_stream_open(...)` call + manual `sampleFile->readStream_ = stream` assignment with `sampleFile->readStream_ = deluge::io::Stream::open(filePath, DELUGE_STREAM_READ);` — but since `Stream::open` returns `std::expected<Stream, Status>` not a `Stream` directly, handle the error case explicitly (matching the existing early-return-on-failure shape at this call site) before assigning, e.g.:
  ```cpp
  auto opened = deluge::io::Stream::open(filePath, DELUGE_STREAM_READ);
  if (!opened) {
  	*error = Error::FILE_NOT_FOUND;
  	destroyAudioFileObject(*audioFile);
  	return nullptr;
  }
  sampleFile->readStream_ = std::move(opened.value());
  ```
  and update the per-cluster `sdAddress` population loop to call `sampleFile->readStream_->sector_of(i)` (handling its `expected` return the same best-effort way the current raw `deluge_stream_sector_of` call is handled — result ignored on failure, `sector` stays its default).
- `readClusterData`: replace the null-check-and-raw-call fallback logic with the wrapper's `expected`-returning `read_at`, keeping the exact same "readStream_ has no value → fall back to raw `deluge_block_read(sdAddress)`" branch structure Task 6 established (this fallback behavior doesn't change, only the non-fallback branch's mechanics do — `sample->readStream_->read_at(...)` instead of the raw C-ABI call).
- `cardReinserted`: replace the raw `deluge_stream_open`/`deluge_stream_sector_of`/`deluge_stream_close` sequence (added in the final-review fix, `66a3e0fca`) with `deluge::io::Stream::open(...)` + `.sector_of(0)`, relying on the `Stream`'s destructor for cleanup instead of an explicit `deluge_stream_close` call — preserve the exact same `markAsUnloadable()`/`continue` semantics on any failure.

- [ ] **Step 5: Migrate `SampleRecorder`/`sample_recorder.cpp`'s write side onto the wrapper**

In `sample_recorder.h`, change:
```cpp
	DelugeStream* file = nullptr;
```
to:
```cpp
	std::optional<deluge::io::Stream> file;
```
(update includes similarly to Step 4).

In `sample_recorder.cpp`:
- File creation (`cardRoutine`): `this->file = deluge::io::Stream::open(filePathCreated.c_str(), mayOverwrite ? DELUGE_STREAM_WRITE_CREATE : DELUGE_STREAM_WRITE_CREATE_NEW);`, handling the error case (currently `goto gotError`) the same way the existing code does when the `expected` doesn't hold a value.
- `writeCluster`: `file->write_at(byteOffset, ...)` then `file->sector_of(clusterIndex)`, same sequencing as today, `expected`-returning instead of `DelugeStatus`-returning.
- `finalizeRecordedFile`'s close sites: `file->close()` (or just let the `std::optional` be `.reset()`/reassigned — match whichever the surrounding code's control flow makes cleaner; if the code needs to explicitly observe close-failure as an error, call `.close()` and check the `expected`, don't just silently reset).
- `alterFile`'s reopen: `this->file = deluge::io::Stream::open(sample->filePath.c_str(), DELUGE_STREAM_WRITE_APPEND);` — a plain move-assignment over the existing (already-closed, per the current control flow) `std::optional`, matching `File`'s established reopen idiom.
- `truncateFileDownToSize`: `file->truncate(newFileSize)`.

- [ ] **Step 6: Build, verify, and confirm zero behavior change**

Run: host-sim build + `scripts/golden_mixdown.sh check` for `cordae` and `icoustic` (`NO_BUILD=1` if already built) — both must remain bit-exact, since this is a pure refactor. Run the full CppSpec/CTest suite. As an extra confidence check given this touches the exact same recording path Task 8 verified carefully, exercise a real recording via `deluge_render` (e.g. a DRUM-mode export) and confirm it's byte-identical to a `git stash`-based A/B against pre-Task-10 code, the same methodology Task 8 used. Also confirm via `grep -rn "DelugeStream\*\|deluge_stream_close\|deluge_stream_open" src/deluge/model/sample/sample.h src/deluge/model/sample/sample.cpp src/deluge/model/sample/sample_recorder.h src/deluge/model/sample/sample_recorder.cpp` that no raw `DelugeStream*` handle or raw C-ABI call remains in these 4 files (they should all go through `deluge::io::Stream` now) — `audio_file_manager.cpp` is expected to still call `deluge::io::Stream::open` (a static factory, not raw C-ABI) plus keep its existing fallback-path raw `deluge_block_read` call in `readClusterData` (Task 6's null-`readStream_` fallback, unrelated to this task, must survive unchanged).

- [ ] **Step 7: Commit**

```bash
git add src/deluge/io/stream.hpp src/deluge/io/stream.cpp \
        src/deluge/model/sample/sample.h src/deluge/model/sample/sample.cpp \
        src/deluge/storage/audio/audio_file_manager.cpp \
        src/deluge/model/sample/sample_recorder.h src/deluge/model/sample/sample_recorder.cpp
git commit -m "refactor(io): add deluge::io::Stream RAII wrapper, migrate Sample/SampleRecorder onto it"
```
