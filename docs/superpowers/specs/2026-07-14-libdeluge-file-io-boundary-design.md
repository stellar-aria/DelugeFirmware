# `libdeluge` file-I/O boundary — Design

**Date:** 2026-07-14
**Status:** Design (approved in brainstorming) → plan next
**Branch:** `feat/libdeluge-file-io`, based off `next`

## 1. Goal

Give the application a real `libdeluge` boundary for file-level storage access —
`include/libdeluge/file_io.h` — so portable app code (`storage_manager.cpp`,
`audio_file_manager.cpp`, the browser, the companion SysEx protocol, and
friends) stops calling the vendored FatFS library (`f_open`/`f_read`/`FRESULT`/
`FIL`/attribute bits) directly. FatFS becomes a BSP-internal implementation
detail — something `bsp/rza1`, `bsp/host`, and the Embassy BSP use to
*implement* `file_io.h` — instead of something the app links against.

This is not a new idea invented for portability's own sake: `docs/dev/
target_architecture.md` §3.3 already says `storage/` should dissolve into
`files`/`stream`/`xml`, and §11 step 9 already calls for "serialization
binding moves off model member functions; `storageManager` global dissolves."
The direct FatFS calls in app code are incomplete separation, not a deliberate
design — every *other* hardware capability already has a boundary header in
this shape (`audio_io.h`, `display.h`, `control_surface.h`, `block_device.h`,
`storage_wait.h`). File access is the one gap.

**Immediate driver:** a planned Linux BSP (`src/bsp/linux/`, spec at
`docs/superpowers/specs/2026-07-14-linux-deluge-bsp-design.md` on
`deluge-project-param`) needs to back Deluge's storage with the SD card's
*already-mounted* native filesystem, not a private FatFS-formatted image —
deluge-linux has no spare partition and no image-file convention; the whole
card is one plain FAT volume the Linux kernel already owns. Running FatFS
again inside a file *on top of* that would work, but running two independent
filesystem implementations against the same physical card invites corruption
and forecloses the nicer outcome: users drag-and-drop songs onto the card from
any computer. A native-POSIX BSP implementation of `file_io.h` gets that for
free. But the boundary itself is not Linux-specific — it benefits every BSP by
finally giving the app a real, swappable file-access contract instead of a
vendored library's own API leaking through.

## 2. Non-goals

- **Not the full `deluge-files`/`deluge-xml` extraction** from
  `target_architecture.md` §3 (the model⇄XML binding layer, the settings
  store, favourites, etc.). This is scoped to the file-*access primitives*
  only — open/read/write/seek/stat/directory-iteration/mkdir/unlink/rename.
  The larger library split can build on this later; it isn't required first.
- **Not a new filesystem feature.** No new capability the app doesn't already
  have today (no symlinks, no permissions, no extended attributes).
- **Not touching the sample-cluster streaming path.** That's the
  `deluge_resource` manager's async request/ready/`try_acquire` ABI
  (`crates/deluge_resource`), which already has its own I/O seam ("the seam
  for a future embassy storage task") separate from ad-hoc file access. Out of
  scope here; the Linux BSP's loader-thread/io_uring design for that path is
  a separate, already-sketched piece of work.
- **Not implementing the Linux BSP.** This doc only adds the boundary +
  migrates existing call sites; a native-POSIX implementation lands with the
  Linux BSP work itself.

## 3. Background: what's actually there today

A prior audit (this branch's brainstorming session) surveyed every FatFS call
site in the app:

- **~65 call sites across ~10 substantive files** (`storage_manager.cpp`,
  `smsysex.cpp`, `browser.cpp`, `sample_browser.cpp`,
  `instrument_clip_view.cpp`, `performance_view.cpp`, `midi_follow.cpp`,
  `midi_device_manager.cpp`, `runtime_feature_settings.cpp`,
  `stem_export.cpp`, plus `util/functions.cpp`'s translator); another ~12
  files merely `#include` a FatFS header without calling into it.
- Call counts: `f_open`×10, `f_read`×10, `f_close`×11, `f_write`×4,
  `f_mkdir`×14, `f_unlink`×11, `f_rename`×8, `f_opendir/f_readdir/
  f_closedir`×3/3/2, `f_lseek`×3, `f_stat`×1, `f_size`×3. **Zero** calls to
  `f_mount`, `f_getfree`, `f_truncate`, `f_sync`, `f_tell`, `f_eof` — none of
  those need to exist in the new boundary.
- `f_open/f_read/f_write/f_close` are used exactly like POSIX
  `open/read/write/close` — no FatFS-specific flag combinations found beyond
  `FA_READ` / `FA_WRITE|FA_CREATE_ALWAYS`.
- `f_lseek`'s 3 sites are all plain absolute-offset seeks — no fast-seek/CLMT
  tricks.
- `f_size()` (a macro reading `fp->obj.objsize`) has 3 sites, trivially an
  open-handle size query.
- The `AM_DIR` attribute bit is read directly at 3 sites, always to
  distinguish files from directories while iterating a listing.
- **No 8.3/LFN handling, no cluster/CLMT fast-seek, no FatFS drive-number path
  syntax (`"0:/"`) anywhere in the app.** Paths already look like plain
  relative paths.
- Error handling already funnels through one translator,
  `fresultToDelugeErrorCode(FRESULT)` (`util/functions.cpp:2135`), covering 7
  named cases (`FR_OK`, `FR_NO_FILESYSTEM`, `FR_NO_FILE`, `FR_NO_PATH`,
  `FR_WRITE_PROTECTED`, `FR_NOT_ENOUGH_CORE`, `FR_EXIST`) into the app's
  `Error` enum (`Error::NONE`, `SD_CARD_NO_FILESYSTEM`, `FILE_NOT_FOUND`,
  `FOLDER_DOESNT_EXIST`, `WRITE_PROTECTED`, `INSUFFICIENT_RAM`,
  `FILE_ALREADY_EXISTS`, default `SD_CARD`). Five call sites branch on raw
  `FR_EXIST`/`FR_NO_PATH` directly for control flow (all the same two idioms:
  "mkdir succeeded or the folder was already there," and "auto-create parent
  dirs on `FR_NO_PATH`").

**Verdict: the coupling is real but shallow.** Every FatFS behavior the app
depends on reduces to open/read/write/seek/close, directory iteration with an
is-directory bit, three mkdir/unlink/rename ops, and one 7-code error
vocabulary — all trivially POSIX-shaped. This is a bounded migration, not a
compatibility project.

## 4. The boundary: `include/libdeluge/file_io.h`

Same conventions as `block_device.h`/`storage_wait.h`: opaque handles,
`[task]`-context, `DelugeStatus` returns, plain C, extern "C". Paths are plain
forward-slash strings rooted at the storage volume — no drive prefix (matches
what the app already does; FatFS's own drive-number syntax was never actually
used).

```c
typedef struct DelugeFile DelugeFile;   // opaque handle
typedef struct DelugeDir DelugeDir;     // opaque handle

#define DELUGE_MAX_FILENAME 256   // FAT LFN max (255) + NUL

typedef enum DelugeFileOpenMode {
    DELUGE_FILE_READ,
    DELUGE_FILE_WRITE_CREATE,   // create, truncate if it exists
} DelugeFileOpenMode;

DelugeStatus deluge_file_open(const char* path, DelugeFileOpenMode mode, DelugeFile** out);   // [task]
DelugeStatus deluge_file_read(DelugeFile*, void* dst, uint32_t count, uint32_t* out_read);      // [task]
DelugeStatus deluge_file_write(DelugeFile*, const void* src, uint32_t count, uint32_t* out_written); // [task]
DelugeStatus deluge_file_seek(DelugeFile*, uint32_t offset);    // absolute only
DelugeStatus deluge_file_size(DelugeFile*, uint32_t* out_size);
DelugeStatus deluge_file_close(DelugeFile*);

typedef struct DelugeDirEntry {
    char name[DELUGE_MAX_FILENAME];
    bool is_directory;
} DelugeDirEntry;

DelugeStatus deluge_dir_open(const char* path, DelugeDir** out);
DelugeStatus deluge_dir_read(DelugeDir*, DelugeDirEntry* out, bool* out_has_entry); // has_entry=false + OK = end of dir
DelugeStatus deluge_dir_close(DelugeDir*);

DelugeStatus deluge_file_mkdir(const char* path);   // DELUGE_ERR_EXISTS if already there
DelugeStatus deluge_file_unlink(const char* path);
DelugeStatus deluge_file_rename(const char* old_path, const char* new_path);
```

**Open item, to confirm during implementation, not guessed here:** whether
`DELUGE_FILE_READ`/`DELUGE_FILE_WRITE_CREATE` are the *only* two modes the 10
`f_open` call sites need, or whether any site combines read+write on one
handle. Verify against the actual `f_open` flag arguments at each site before
finalizing the enum.

## 5. Error mapping

`types.h`'s `DelugeStatus` gains five codes, general-purpose (not FAT-specific
in name or meaning) so they're usable by any future boundary call, not just
file I/O:

```c
DELUGE_ERR_NOT_FOUND,        // path doesn't exist
DELUGE_ERR_EXISTS,           // path already exists (e.g. mkdir target)
DELUGE_ERR_NO_SPACE,         // storage full / allocation failed
DELUGE_ERR_NO_FILESYSTEM,    // media present but has no valid filesystem
DELUGE_ERR_WRITE_PROTECTED,  // media is read-only / locked
```

The app-side translator (today `fresultToDelugeErrorCode(FRESULT)`) becomes a
`DelugeStatus → Error` mapping instead of `FRESULT → Error` — same shape, one
layer up. It still resolves `DELUGE_ERR_NOT_FOUND` to either
`Error::FILE_NOT_FOUND` or `Error::FOLDER_DOESNT_EXIST` based on which boundary
call failed (`deluge_file_open` vs `deluge_dir_open`) — the boundary doesn't
need to carry that distinction, the call site already knows which it asked
for. The five call sites that branch directly on `FR_EXIST`/`FR_NO_PATH` move
to branching on `DELUGE_ERR_EXISTS`/`DELUGE_ERR_NOT_FOUND` — same two idioms,
same call sites, new enum.

## 6. Per-BSP implementation strategy

- **`bsp/rza1`, `bsp/host`, Embassy BSP:** implement `file_io.h` as a thin
  forward to the *existing* FatFS + `block_device.h` stack, now BSP-internal.
  `FRESULT`→`DelugeStatus` translation moves here (was app-side). This is a
  **behavior-preserving refactor** for these three BSPs — same FatFS, same
  block device, just called from one layer lower than before. No golden-render
  or timing risk; it's a mechanical call-site move plus a new thin adapter
  file per BSP.
- **Linux BSP (future):** implements `file_io.h` directly over POSIX
  (`open`/`read`/`write`/`lseek`/`fstat`/`opendir`/`readdir`/`mkdir`/`unlink`/
  `rename`) against the natively-mounted `/sd`. No FatFS anywhere in this BSP.
  `errno`→`DelugeStatus` translation lives here. This BSP also owns bridging
  these (now-POSIX) blocking calls into the app's cooperative scheduler via
  `storage_wait.h`'s `yieldingRoutineForSD` — a worker thread per the
  storage-concurrency design already sketched for that BSP. Out of scope for
  *this* doc; noted here only so the dependency is visible.

  **Implementation-detail note (not a boundary-contract concern):** the
  Linux (and possibly `bsp/host`) adapter's *internal* implementation of
  `file_io.h` is a reasonable place to use `std::filesystem`
  (`directory_iterator`, `path`, `file_size`, `create_directory`, `remove`,
  `rename`, all via the non-throwing `std::error_code&` overloads so no
  exception crosses the C-ABI boundary — matching `target_architecture.md`
  §5.2) instead of hand-rolled POSIX calls. It maps closely onto the function
  list in §4 and fits this project's preference for idiomatic modern C++ in
  new code. This is purely an adapter-internal style choice — it doesn't
  change the boundary shape, the error mapping, or the concurrency design
  (§6 above still applies: `std::filesystem` calls block on the same
  underlying syscalls, so the worker-thread/yield bridge is unaffected).
  **Verify before committing to it:** deluge-linux's musl cross toolchain
  (`arm-linux-g++` + static `libstdc++` per the Linux BSP spec's build
  section) needs a working `<filesystem>` — musl/embedded libstdc++ builds
  have historically had gaps here (missing symbols, needing `-lstdc++fs` on
  older GCC). Not yet checked against the actual toolchain.

## 7. Migration scope

The ~65 call sites move from FatFS's own API to `file_io.h`. Concretely, per
file:

| file | FatFS calls to migrate |
|---|---|
| `storage/storage_manager.cpp` | open/read/write/close/seek/size, mkdir/rename error branches |
| `storage/smsysex.cpp` | open/read/close/size, mkdir/rename error branches |
| `gui/browser.cpp` | opendir/readdir/closedir, `AM_DIR`, mkdir/rename branches |
| `gui/views/sample_browser.cpp` | `AM_DIR` |
| `gui/views/instrument_clip_view.cpp` | `AM_DIR` |
| `gui/views/performance_view.cpp` | mkdir `FR_EXIST` branch |
| `io/midi/midi_follow.cpp` | mkdir `FR_EXIST` branch |
| `io/midi/midi_device_manager.cpp` | mkdir `FR_EXIST` branch |
| `storage/flash_storage/runtime_feature_settings.cpp` | mkdir `FR_EXIST` branch |
| `processing/stem_export.cpp` | mkdir/rename branches |
| `util/functions.cpp` | the translator itself: `fresultToDelugeErrorCode` → a `DelugeStatus`-based version |

Each site keeps its existing control flow; only the function names, handle
types, and error-code identifiers change. No behavior change is intended on
rza1/host/Embassy — this is the kind of mechanical, git-mv-shaped step the
project has done before for other boundaries (`control_surface.h`,
`midi_io.h`).

## 8. Testing strategy

- **Host-buildable unit tests** for the new boundary's rza1/host adapter
  (thin forward — assert `DelugeStatus` codes map correctly, round-trip a
  read/write/seek against the existing host FatFS backing) and for the error
  translator (`DelugeStatus`→`Error`, table-driven, mirrors the existing
  `FRESULT`→`Error` test shape if one exists, or is new).
- **Golden-render regression**: since rza1/host/Embassy behavior doesn't
  change, the existing golden-master harness should stay bit-exact through
  this migration — a strong, cheap correctness signal for a mechanical
  refactor.
- **No new hardware-verify step for this doc** — the actual hardware risk
  (Linux's POSIX-backed implementation, its concurrency bridging) lands with
  the Linux BSP work and is verified there.

## 9. Relationship to other in-flight docs

- Supersedes the "FatFS-in-a-file-image" and "FatFS-API-compatible shim"
  ideas floated earlier for the Linux BSP's `block_device.h` — this boundary
  replaces the need for either; Linux never touches FatFS or
  `block_device.h` at all.
- Prerequisite for the Linux BSP's storage work
  (`docs/superpowers/specs/2026-07-14-linux-deluge-bsp-design.md`, §2/§13
  storage fast-follow) — that spec should be amended to reference this
  boundary instead of `block_device.h`/`flash.h` once this lands.
- Advances `docs/dev/target_architecture.md` §11 step 9 ("`deluge-files`:
  serialization binding moves off model member functions") without
  committing to the rest of that step; worth a forward-reference from that
  doc once this merges.

## 10. Open questions

1. `deluge_file_open`'s exact mode set (§4) — confirm against real call sites
   during implementation.
2. Should `deluge_file_mkdir` recursively create parent directories, or keep
   today's explicit "caller checks `FOLDER_DOESNT_EXIST`(`DELUGE_ERR_NOT_FOUND`)
   and creates parents itself" pattern? Current app code does the latter (5
   call sites); default to preserving that unless it's clearly worth
   simplifying during migration.
3. Does any call site need `DELUGE_FILE_READ` **and** write combined, or
   append-mode writes? Survey found none, but wasn't exhaustively checking
   flag combinations — verify.
