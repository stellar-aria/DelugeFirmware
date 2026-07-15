# `deluge-stream` boundary — Design

**Date:** 2026-07-15
**Status:** Design (approved in brainstorming) → plan next
**Base:** `next`

## 1. Goal

Give real-time `Sample` audio streaming (playback read, recording write) a real
`libdeluge` boundary — `include/libdeluge/stream_io.h` — so portable app code
(`audio_file_manager.cpp`, `sample_recorder.cpp`) stops reaching directly into
vendored FatFS internals (`get_fat_from_fs`, `clst2sect`, raw `FatFS::File`)
and stops calling an ad-hoc, non-boundary `extern "C"` pair
(`disk_read_without_streaming_first`/`disk_write_without_streaming_first`)
declared straight in app code. This is the gap named but not designed in
`docs/dev/target_architecture.md` §3.2 ("`deluge-stream` — audio-context
storage: cluster cache, sample streaming, SD read scheduling... split from
`deluge-files` because the two halves have opposite realtime contracts") and
tracked in `TODO.md`'s "deluge-stream boundary (future, unscoped)" entry,
found while implementing Tiers 2 and 3 of the `file_io.h` migration.

This closes the last piece of raw-FatFS-touching app code left after that
migration (which deliberately excluded this path — see §3).

## 2. Non-goals

- **Not the resource manager.** `crates/deluge_resource`'s async
  request/acquire/loader-queue ABI (`deluge_resource_request`/`_acquire`/
  `_loader_enqueue`/`_loader_next`/`_mark_ready`) is already the real
  cross-BSP `deluge-stream` *contract* — complete, unit-tested, and already
  fully consumed by `SampleCluster::getCluster` and
  `AudioFileManager::loadAnyEnqueuedClusters`. This doc adds nothing there. It
  only replaces what sits *underneath* it: `clusterMaterialize`
  (`sample.cpp:117`, the manager's synchronous-miss `materialize` callback)
  calls `AudioFileManager::readClusterData`, which is where the raw FatFS
  reach-in actually happens.
- **Not `WaveTable`, presets, songs, or anything else on `file_io.h`.** Those
  are correctly scoped to the portable file-path boundary already
  (`buildAudioFileFromCard`'s `WaveTable` branch already uses
  `deluge::io::File::open`, unchanged by this doc). This is specifically the
  `Sample` cluster-streaming path (`ClusterByteSource`, `readClusterData`,
  `SampleRecorder::writeCluster`/`finalizeRecordedFile`).
- **Not designing the Linux BSP's storage backend.** A Linux BSP (branch
  `linux-bsp`, commit `e417a745e`, doc
  `docs/superpowers/specs/2026-07-14-linux-deluge-bsp-design.md` §14) already
  has an approved architecture for this: sample-cluster streaming there is an
  **io_uring loader polled once per `deluge_app_tick()`**, submitting
  `deluge_resource_loader_next()`'s queue and calling
  `deluge_resource_mark_ready()` on completion — a *sibling* driver loop
  around the same resource-manager seam, using `io_uring`/`pread`-style
  byte-offset reads against an already-open native fd. It has no FAT/sector
  geometry and doesn't touch `block_device.h`. It's cited here so this doc
  doesn't need to (and shouldn't try to) design for `io_uring`; its
  synchronous fallback (see §4) is expected to implement this same
  `stream_io.h` boundary directly over `pread`/`pwrite`, analogous to how
  Linux's planned `file_io.h` implementation skips FatFS entirely.
- **Not retiring `browser.cpp`/`sample_browser.cpp`'s `FilePointer`/
  `staticDIR`/`staticFNO`.** This work removes `audio_file_manager.cpp`'s
  `FilePointer` usage (one of three blocking call sites named in the
  `file_io.h` Tier 2 design doc §6), which unblocks that retirement, but
  doesn't do it — left as a follow-up.

## 3. Background: what's actually there today

**Read side** (`audio_file_manager.cpp`). `buildAudioFileFromCard`'s `SAMPLE`
branch (lines ~804-819) walks the FAT chain itself, once, using FatFS-internal
functions declared straight in this file (`get_fat_from_fs`, `clst2sect` —
defined in the vendored, single shared `src/fatfs/ff.c`, not BSP-specific),
seeded from `effectiveFilePointer.sclust` (obtained by a prior `resolveFilePointer`/
`tryRegularPath` open). This builds a per-cluster `sdAddress` table on the
`Sample` object. `readClusterData` (`audio_file_manager.cpp:936`, already the
resource manager's `materialize` Source — see §2) then reads via
`disk_read_without_streaming_first(deluge_block_sd_unit(), ...)` — an
`extern "C"` pair declared ad hoc in this file (not in any `libdeluge` header)
and implemented separately in all three BSPs today (RZA1 `src/RZA1/diskio.c`,
host `src/bsp/host/host_platform.c`, Rust `src/bsp/rust/src/sd.rs`). It's
already parametrized by `block_device.h`'s `deluge_block_sd_unit()`, i.e.
half-wired to the real boundary already.

**Write side** (`sample_recorder.cpp`). `writeCluster` (line ~824) computes
`sdAddress = clst2sect(&fileSystem, file->inner().clust)` against a raw
`FatFS::File` obtained from `StorageManager::createFileRaw` (a sibling of the
portable `createFile()`, quarantined during the `file_io.h` Tier 3 migration
specifically because this consumer needed raw FAT/cluster access that
`deluge::io::File` doesn't and shouldn't expose), then writes via
`disk_write(0, ...)` — note this hardcodes unit `0` instead of calling
`deluge_block_sd_unit()`, and uses the *with-streaming* `disk_write` rather
than the `_without_streaming_first` variant the read side uses; both look like
latent inconsistencies this work fixes as a side effect, not the point of it.
Finalization (`finalizeRecordedFile`/`truncateFileDownToSize`, lines
~1529-1584) reopens the file raw with `FA_WRITE` only (a flag
`DelugeFileOpenMode` has no equivalent for) and calls raw `lseek`/`truncate`
on the `FatFS::File`.

**`block_device.h` already exists but is unfinished.** It's the designed
sibling to `file_io.h` for raw sector access, but only
`deluge_block_sd_unit`/`deluge_block_poll_card_event` are implemented on real
hardware and the Rust BSP; `deluge_block_read`/`write`/`init`/`ready`/
`sector_count`/`sector_size`/`sync` are stub-only everywhere (the host BSP's
stub literally returns `DELUGE_ERR_NODEV`). `src/bsp/rza1/block_device.c`'s
own comment says the sector operations are "added if/when the application's
storage layer moves off the FatFs diskio interface directly" — this doc is
that trigger.

**The resource manager needs no changes.** `Source.materialize`/`on_evict`
(`crates/deluge_resource/src/manager.rs`) are already fully storage-agnostic
(`(ctx, owner, index, dest, len) -> bool`); `readClusterData` already plugs in
as-is. `Stealable` is fully retired from the codebase (confirmed: zero
matches for `class Stealable`/`: public Stealable`) — `target_architecture.md`
§3.2's description of `deluge-stream` owning "`Stealable` implementations" is
now stale and should be corrected to describe the resource-manager
`Source`/`materialize`/`on_evict` wiring instead, as a documentation
fast-follow.

## 4. The boundary: `include/libdeluge/stream_io.h`

Same conventions as `file_io.h`/`block_device.h`: opaque handle, `[task]`
context, `DelugeStatus` returns (reusing the error vocabulary `file_io.h`
already added to `types.h` — `DELUGE_ERR_NOT_FOUND`/`_EXISTS`/`_NO_SPACE`/
`_NO_FILESYSTEM`/`_WRITE_PROTECTED` — no new codes needed), plain C,
extern "C". Scoped to `Sample` streaming only — not a general file API.

```c
typedef struct DelugeStream DelugeStream;   // opaque handle

typedef enum DelugeStreamMode {
    DELUGE_STREAM_READ,               // open an existing file; resolves full layout at open
    DELUGE_STREAM_WRITE_CREATE,       // create the file, truncating if it exists
    DELUGE_STREAM_WRITE_CREATE_NEW,   // create the file; fails with DELUGE_ERR_EXISTS if it already exists
} DelugeStreamMode;

DelugeStatus deluge_stream_open(const char* path, DelugeStreamMode mode, DelugeStream** out);           // [task]
DelugeStatus deluge_stream_read_at(DelugeStream*, uint32_t byte_offset, void* dst,
                                    uint32_t count, uint32_t* out_read);                                // [task]
DelugeStatus deluge_stream_write_at(DelugeStream*, uint32_t byte_offset, const void* src,
                                     uint32_t count, uint32_t* out_written);                             // [task]
DelugeStatus deluge_stream_truncate(DelugeStream*, uint32_t new_size);                                  // [task]
DelugeStatus deluge_stream_size(DelugeStream*, uint32_t* out_size);                                     // [task]
DelugeStatus deluge_stream_close(DelugeStream*);                                                        // [task]
```

`DELUGE_STREAM_WRITE_CREATE`/`_CREATE_NEW` mirror `file_io.h`'s
`DELUGE_FILE_WRITE_CREATE`/`_CREATE_NEW` exactly (added there during Tier 3
for the same "mayOverwrite" need) — `SampleRecorder`'s existing
`mayOverwrite` bool maps directly to which mode it opens with.

**`write_at`'s real contract is sequential append, not general random-access
write** — `SampleRecorder` only ever writes whole clusters in increasing
index order, never seeks backward except via `truncate` at the very end. The
`byte_offset` parameter is kept (matching `read_at`'s shape, and so a
BSP implementation can assert it equals the current end-of-file as a
caller-bug check) but the boundary is not designed or tested for arbitrary
offsets. Documented here so a future reader doesn't read more generality into
the signature than exists.

**No `cluster_size` parameter anywhere.** The FatFS adapter already knows the
mounted volume's cluster size (`fileSystem.csize`); callers just deal in plain
byte offsets/counts, same as `file_io.h`.

## 5. Per-BSP implementation strategy

- **`bsp/rza1`, `bsp/host`, in-tree Rust BSP:** **one shared implementation**,
  `src/fatfs/stream_io.cpp` (new file, alongside the existing `file_io.cpp`),
  built once against the single vendored `ff.c` these three BSPs already
  share — not three separate reimplementations. `deluge_stream_open` in
  `DELUGE_STREAM_READ` mode does the FAT-chain walk once (today's
  `get_fat_from_fs`/`clst2sect` logic, relocated verbatim, not reinvented)
  and caches the resolved cluster→sector table in the opaque handle;
  `read_at` computes which cluster(s) a byte range spans and calls the
  now-finished `deluge_block_read` directly — skipping FatFS's buffered
  `f_read` for data, exactly as `readClusterData` does today, just relocated
  to where FatFS knowledge is supposed to live. `write_at` in
  `DELUGE_STREAM_WRITE_CREATE*` mode still calls `ff.c`'s normal `f_write`/
  `f_lseek` to trigger new-cluster allocation (FatFS owns the free-space
  bitmap; no reason to reimplement that), discovers the resulting address via
  `clst2sect`, then does the actual data write raw via `deluge_block_write`
  — same split strategy as today, relocated. `truncate` absorbs the raw
  `FA_WRITE`-reopen/`lseek`/`truncate` dance.
- **Linux BSP (future, not this doc):** implements `stream_io.h` directly
  over `pread`/`pwrite` against a plain fd opened once at `deluge_stream_open`
  — no cluster/sector concept, no FatFS, no `block_device.h` involvement.
  Simpler than the FatFS-family implementation by construction, since the
  kernel's own page cache already makes offset-based reads fast. This is the
  BSP-side implementation `clusterMaterialize`'s synchronous-miss fallback
  needs to keep working even when the primary path is the io_uring loader
  (§2) — out of scope to build here, noted so the contract is right.

## 6. `block_device.h` completion

`deluge_block_read`/`deluge_block_write` become real on RZA1, host, and the
in-tree Rust BSP — in practice this is closer to a rename-and-consolidate
than new code, since `disk_read_without_streaming_first`/
`disk_write_without_streaming_first` already do this work in all three
places today. Once done:

- `disk_read_without_streaming_first`/`disk_write_without_streaming_first`
  (the ad-hoc `extern "C"` pair declared in `audio_file_manager.cpp`) are
  deleted; their logic moves inside each BSP's `deluge_block_read`/`write`.
- FatFS's own diskio port (`disk_read`/`disk_write` — the "service the
  audio-streaming queue before every sector access" policy wrapper, which
  stays exactly where it is, `audio_file_manager.cpp:72-87`) now calls
  `deluge_block_read`/`write` instead of the ad-hoc pair. This wrapper is
  unaffected otherwise — it's genuinely app-level policy, not part of this
  boundary.
- `sample_recorder.cpp`'s hardcoded unit-`0` / wrong-streaming-variant bug
  (§3) is fixed as part of this consolidation, not as a separate patch.

## 7. Migration scope

| file | changes |
|---|---|
| `include/libdeluge/stream_io.h` | new boundary header (§4) |
| `src/fatfs/stream_io.cpp` | new shared FatFS-family implementation (§5) |
| `src/bsp/rza1`, `src/bsp/host`, `src/bsp/rust` | finish `deluge_block_read`/`write` (§6); delete `disk_read_without_streaming_first`/`_write_...` |
| `src/deluge/storage/audio/audio_file_manager.cpp` | `buildAudioFileFromCard`'s `SAMPLE` branch + `readClusterData` move to `deluge_stream_open`/`read_at`; raw `get_fat_from_fs`/`clst2sect` calls removed; `FilePointer` drops out of this path |
| `src/deluge/model/sample/sample_recorder.cpp` | `writeCluster`/`finalizeRecordedFile`/`truncateFileDownToSize` move to `deluge_stream_write_at`/`truncate`; raw `FatFS::File`/`clst2sect`/`FA_WRITE` reopen removed |
| `src/deluge/storage/storage_manager.{h,cpp}` | `createFileRaw` deleted (its one caller, `sample_recorder.cpp`, no longer needs raw `FatFS::File`) |
| `docs/dev/target_architecture.md` §3.2 | correct the stale "`Stealable` implementations" line (§3) |
| `TODO.md` | remove the "deluge-stream boundary (future, unscoped)" entry this doc resolves |

Read (`audio_file_manager.cpp`) and write (`sample_recorder.cpp`) are
independent call chains — unlike `file_io.h`'s Tier 3, there's no single task
that has to cover both at once; each can land as its own small, independently
shippable step (see §8 phasing, deferred to the implementation plan).

## 8. Testing strategy

- **Host-buildable unit tests** for the new boundary's FatFS-family adapter:
  open/read_at/write_at/truncate round-tripped against the host's existing
  FatFS-backed test disk, same shape as `file_io.h`'s adapter tests.
- **Golden-master regression covers the read side, not the write side.** The
  existing golden-master harness's 296 real factory/community presets load
  real sample files through `buildAudioFileFromCard`/`ClusterByteSource`, so
  it's a strong, cheap bit-exactness signal for the Tier 2 (read) relocation
  — confirmed no test currently references `SampleRecorder`/
  `ClusterByteSource` directly (`tests/` grep), so this harness is the real
  regression net for that side, not a supplementary one. **The write side has
  no automated regression coverage today** — no recording-specific tests
  exist in `tests/` at all; this is a pre-existing gap, not one this doc
  introduces, in the same vein as `file_io.h` Tier 3's accepted
  `storage_manager_spec.cpp` gap. Worth adding a basic record →
  read-back-and-compare round-trip test as part of the Tier 3 (write)
  implementation task, but that's a recommendation for the plan, not a
  precondition for this design.
- **Hardware gate**, unpushed here per standing project practice: real
  hardware timing (RZA1's SD streaming keeping up with playback/recording
  under the relocated call sequence) is the actual residual risk, verified
  the same way every other storage-layer change in this codebase is.
- **No test obligation for the Linux backend** — out of scope (§2).

## 9. Relationship to other in-flight docs

- Resolves `TODO.md`'s "deluge-stream boundary (future, unscoped)" entry,
  found during `file_io.h` Tier 2 and Tier 3 (see
  `docs/superpowers/specs/2026-07-14-file-io-migration-tier2-locator-cache-design.md`
  §6 and `docs/superpowers/specs/2026-07-14-file-io-migration-tier3-filereader-filewriter-design.md`).
- Sibling to the Linux BSP's io_uring cluster loader
  (`docs/superpowers/specs/2026-07-14-linux-deluge-bsp-design.md` §14,
  branch `linux-bsp`, commit `e417a745e`) — both are backend implementations
  of the same resource-manager loader-queue seam; neither doc needs to design
  for the other beyond this cross-reference (§2, §5).
- Advances `docs/dev/target_architecture.md` §3.2's `deluge-stream` entry and
  §11 step 8 ("`deluge-alloc` + `deluge-stream` split out of `storage/` +
  `memory/`") without committing to the rest of that step.

## 10. Open questions

1. Whether `deluge_stream_write_at`'s `byte_offset` parameter should be
   dropped entirely (given §4's sequential-append-only contract) in favor of
   an implicit "append `count` bytes" call, or kept as an explicit
   caller-bug assertion — decide during implementation, not guessed here.
2. Whether FatFS's `f_expand()` (contiguous/non-contiguous pre-allocation)
   is a cleaner way for `write_at` to trigger new-cluster allocation than
   today's approach of relying on a normal `f_write` to extend the file —
   an implementation-detail optimization, not a boundary-contract concern.
3. `docs/dev/target_architecture.md` §3.2's stale `Stealable` line (§3) —
   confirm the correction lands as part of this work or a quick follow-up.
