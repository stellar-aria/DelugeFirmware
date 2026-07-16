# fatfs_stress: FF_FS_REENTRANT concurrent-access verdict

## Config tested

Real `src/fatfs/ff.c` + `ffunicode.c` (byte-identical, staged unmodified),
built host-native under ThreadSanitizer against a harness `ffconf.h`
(`tests/fatfs_stress/ffconf.h`) that differs from firmware's
`src/fatfs/ffconf.h` in exactly three settings:

| Setting            | Firmware | Harness | Why |
|---------------------|----------|---------|-----|
| `FF_FS_REENTRANT`   | 0        | 1       | the Phase-2b change under test |
| `FF_USE_MKFS`       | 0        | 1       | test setup only (need `f_mkfs` for a fresh RAM volume) |
| `FF_USE_LFN`        | 1        | 2       | **forced**, not discretionary: `ff.c` hard-`#error`s on `FF_FS_REENTRANT=1` + `FF_USE_LFN==1` ("Static LFN work area cannot be used in thread-safe configuration"). Mode 1 is a single BSS buffer shared by every call -- ChaN's own doc calls it "Always NOT thread-safe" -- so `FF_FS_REENTRANT=1` is literally unbuildable against firmware's current LFN setting. Mode 2 moves the LFN work buffer onto the per-call stack (~512 B when needed), which has no cross-call shared state and is itself reentrancy-safe. This is a real finding for the Phase-2b design, not a harness artifact: **firmware cannot flip `FF_FS_REENTRANT` on without also moving off `FF_USE_LFN=1`.** |

All other reentrancy-relevant settings (`FF_VOLUMES=1`, `FF_FS_LOCK=0`,
`FF_FS_TINY=0`, `FF_FS_NORTC=0`, sector sizes) match firmware exactly.

Sync primitives: `ff_req_grant`/`ff_rel_grant` backed by a single
`std::mutex` per volume (`tests/fatfs_stress/ff_sync.cpp`) -- the host proxy
for the eventual Embassy volume mutex. `disk_read`/`disk_write` are plain,
unlocked accesses into an in-process RAM buffer
(`tests/fatfs_stress/ram_diskio.cpp`): correct only if FatFS's own grant
actually serializes every caller before reaching diskio, which is exactly
the property this harness is stress-testing.

Build: `-fsanitize=thread -g -O1 -m64` (TSan requires 64-bit; overridden
from the tree-wide `-m32`). Verified this is a real instrumented binary, not
a no-op: `nm -D` shows `__tsan_*` symbols and the binary links
`libtsan.so.2`; a throwaway negative-control program with a deliberate
unsynchronized `int` race (`for(...) counter++` from 4 threads, same
compiler/flags) was confirmed to trigger a TSan `WARNING: ThreadSanitizer:
data race` report in this exact environment, so a clean run below means "no
race found," not "TSan didn't run."

## Workload

- **Step 1 -- distinct-file write/read-back workers:** 8 threads x 500
  iterations. Thread `t`, iteration `i` writes a deterministic
  pattern (seeded by `(t,i)`, length uniform in [1, 8192] bytes) to its OWN
  file `/w<t>_<i>.bin` (`FA_CREATE_ALWAYS|FA_WRITE`), closes, reopens
  `FA_READ`, reads back, asserts byte-for-byte equality, unlinks. 4000 total
  create/write/close/reopen/read/verify/unlink cycles, each thread touching
  only its own paths and its own `FIL`s -- this is what exercises the
  *volume* structures (FAT allocation, directory entries) under real
  concurrent contention on the grant, which is exactly the thing
  `FF_FS_REENTRANT` is supposed to serialize.
- **Step 2 -- shared-path metadata contention workers:** 16 files
  pre-created (`/shared_0.bin` .. `/shared_15.bin`, each with its own
  deterministic pattern) before the concurrent phase starts and left
  untouched (read-only) until final cleanup. 6 threads x 300 iterations,
  each iteration picking a random shared file and a random op: `f_stat` +
  verify `fsize`, or `f_open(FA_READ)` + `f_read` + verify contents +
  `f_close`, or `f_opendir("/")` + `f_readdir` traversal to completion.
  These run *concurrently* with the Step 1 threads (same thread pool,
  joined together), so directory reads race against directory
  creates/deletes from Step 1 on the same root directory -- the concurrent
  traversal case. (Note: since `FF_USE_LFN=2` is forced by
  `FF_FS_REENTRANT=1`, the LFN work buffer is a per-call stack buffer, not a
  shared static, so this workload does not target that specific
  static-buffer race; its value is validating that the grant covers the
  FAT/directory structures and traversal under concurrent access.)
- **Step 3 -- watchdog:** a detached thread sleeping 60 s that hard-exits
  (`std::_Exit(2)`, printing `DEADLOCK/HANG`) if the worker joins haven't
  completed in that window. Self-tested independently: with the budget
  temporarily lowered to 3 s and one worker thread made to sleep 600 s, the
  watchdog fired at ~3.0 s wall-clock, printed the expected message, and the
  process exited with code 2 -- confirming the guard actually works, not
  just that it compiles. (This was a throwaway edit for the self-test only;
  reverted before the real run.)
- **Verdict gate:** exit 0 only if zero integrity mismatches, zero
  `FRESULT != FR_OK`, and all threads joined before the watchdog timeout.

Total concurrent thread count: 14 (8 write/read + 6 metadata), all racing on
one `FATFS` volume backed by one 8 MiB RAM disk.

## Outcome: TSan CLEAN + integrity PASS

Ran the instrumented binary directly (not just via `ctest`, to see the full
TSan stream) **10 times** in a row. All 10 runs: exit code 0, zero `WARNING:
ThreadSanitizer` reports, zero integrity mismatches, zero `FRESULT` errors.
Representative run (~1.2-1.3 s wall clock):

```
PASS: single-thread smoke (mount/mkfs/write/close/reopen/read/unlink)
8 write/read threads x 500 iters, 16 shared files x 300 metadata-thread iters, 0 mismatches, 0 FRESULT errors
PASS: fatfs_stress (single-thread smoke + concurrent distinct-file + shared-metadata workload)
```

`ctest --test-dir build-tests -R fatfs_stress --output-on-failure`:

```
1/1 Test #21: fatfs_stress .....................   Passed    1.2x sec
100% tests passed out of 1
```

No TSan warning of any kind was observed across any run (not just the
data-race report -- no lock-order, no thread-leak, no other diagnostic).

**Conclusion:** under this workload (14 threads, ~5800 total file
operations, real concurrent volume access via `f_open`/`f_write`/`f_read`/
`f_close`/`f_unlink`/`f_stat`/`f_opendir`/`f_readdir`), the real `ff.c` built
with `FF_FS_REENTRANT=1` (and the `FF_USE_LFN=2` change it forces) is
race-free under ThreadSanitizer, backed by a single `std::mutex`-based
volume grant. **Phase 2b's core reentrancy risk -- whether FatFS's own grant
actually serializes concurrent volume access correctly -- is retired without
hardware**, subject to the scope notes below.

### What CLEAN mechanically certifies

In `ff.c`, `lock_fs()`/`validate()` bracket essentially the *entire body* of
every top-level API call the workload uses (`f_open` via `find_volume`,
`f_read`/`f_write`/`f_close`/`f_stat`/etc. via `validate()`, each paired with
`LEAVE_FF`->`unlock_fs`). So FatFS's internals never actually execute
concurrently with each other under the grant -- the "concurrency" under test
is thread-level *API-call contention*, not internal execution overlap. A CLEAN
verdict therefore certifies precisely: **the vendored `ff.c`'s lock-macro
placement has no gaps that leak shared-state access outside the grant, for the
codepaths this workload exercises.** That is exactly Phase 2b's dependency (the
`FF_FS_REENTRANT` grant macros have no coverage gaps). It does *not* certify --
and structurally cannot exercise -- FatFS supporting genuine internal
parallelism. This matters for the async-SD/Phase-7 follow-on: there, diskio
calls happen *inside* that locked region, and yielding mid-transfer changes
this calculus entirely (it lets another caller enter while the grant is held by
a suspended one) -- so the CLEAN verdict here must not be read as covering that.

## Scope notes (what this does NOT cover)

- Not a proof of absence of races in general -- it is one (large, seeded,
  deterministic) concurrent schedule under one TSan run. TSan detects races
  it actually observes on the executed interleaving; it does not exhaustively
  enumerate schedules. Confidence comes from repetition (10/10 clean) and
  from the workload's syscall-heavy, small-critical-section shape stressing
  many interleavings per run, not from formal coverage.
- Does not validate the real Embassy `ff_req_grant`/`ff_rel_grant`
  implementation -- only that FatFS's own reentrancy story is sound *given*
  a correct binary mutex-style grant. The Embassy-specific lock/unlock stub
  is validated when 2b actually lands.
- Does not exercise the shared-`FIL`-across-threads case -- deliberately out
  of scope per the correctness bound (`FF_FS_REENTRANT` protects the volume,
  not an individual `FIL`; two threads sharing one `FIL` is undefined even
  with the grant held, and firmware does not do this).
- Uses a synchronous, always-available RAM diskio (no yield-mid-transfer
  modeling). The async-SD yield-mid-op scenario (Phase 7) is a documented
  follow-on: swap `ram_diskio.cpp`'s `disk_read`/`disk_write` for versions
  that sleep/yield mid-transfer.
- `FF_USE_LFN=1` -> `2` is flagged as a real Phase-2b design finding (not
  just a harness accommodation): firmware's current `FF_USE_LFN=1` is
  incompatible with `FF_FS_REENTRANT=1` at compile time.
