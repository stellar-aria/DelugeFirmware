# Known Concurrency Bugs — preemptive-audio storage/streaming path

**Status:** PARTIALLY FIXED — **B1 + B2 + B3 + B7 FIXED** (B1/B2/B3 2026-07-19; B1 = the `Manager` chunk-table `Cell<ChunkSlot>` race, synchronized via asymmetric fiber-side critical sections; B2/B3 = the recorder hand-off races. B7 2026-07-21 = the efatfs block device's `SD_BUS` bypass. See each below.). **B4, B5 and B6 remain OPEN**, as do additional pre-existing **streaming-path** races the harness surfaces nondeterministically (see "Additional surfaced races"). A run's `UNCATALOGUED=0` is therefore not guaranteed; the specific, reproducible guarantee is that the recorder B2/B3 races are gone and the B1 `Cell<ChunkSlot>`/`mem::replace`/`loader_next` signatures are synchronized at the source.
**Found by:** the host streaming-underrun harness (`src/bsp/rust/preemptive_race_tsan/` — ThreadSanitizer over the real `deluge_app` with a preemptive audio thread) and its deterministic timing lens (`src/bsp/rust/lens1_vt_sim/`). All found on host, **without hardware**.

## Scope / when these bite

Bug **B5** (plus the pre-existing streaming-path races) is a data race on the audio-thread ↔ loader/fiber-thread hand-off in the **storage / cluster / resource-manager / scheduler** path (B1, on the resource-manager chunk table, and B2 + B3, on the **recorder** hand-off, are now fixed). They manifest **only when the audio task is genuinely preemptive** relative to the loader/fiber thread — i.e. on the **Embassy `InterruptExecutor` target** (see the `embassy-scheduler-migration` / `audio-callback-relocation` work). On the legacy **cooperative** BSP (audio runs cooperatively, never preempting mid-op) they cannot occur.

**⇒ The remaining open races must be fixed before the preemptive-audio architecture ships in combination with SD streaming + record-while-stream.** They are exactly the kind of narrow-window corruption that a device test tends to surface only as a rare, hard-to-repro glitch or crash — TSan makes them deterministic to find (but not to trigger in the field).

Reproduce: `cd src/bsp/rust/preemptive_race_tsan && ./run.sh` (needs a pre-packed `DELUGE_SD_IMAGE`; see `run.sh` header). Findings are catalogued in `preemptive_race_tsan/open_findings_races.txt`; the broad pre-existing cooperative-scheduling debt (UI/song-model/reverb/transport, out of scope here) is baselined in `known_patterns.txt`.

---

## B1 — `deluge_resource::Manager` chunk table: unsynchronized `Cell<ChunkSlot>` — HIGH — **FIXED (2026-07-19)**

- **Fix:** asymmetric fiber-side critical sections. Every `Manager` access to the shared `Cell<ChunkSlot>` state now routes through `crates/deluge_resource/src/sync.rs`'s `Masked` guard + `m_get`/`m_set`/`m_rmw`, gated on `!deluge_in_interrupt()`: the **fiber** masks a minimal O(1) window per single-cell RMW (bump/stat/loader-enqueue/loader-remove/release/add-lease/set-dirty/mark-ready/touch/loader_next winner-commit/protect set-restore); the **audio ISR** skips the mask (it can never be preempted by the fiber, so its RMWs are already atomic w.r.t. it). The **host** critical section (`src/bsp/rust/src/services.rs`) was made per-thread cross-thread-correct — its depth+token are now thread-local, so each OS thread acquires the global `critical_section` mutex on its own outermost enter (the process-global `CS_DEPTH` gate previously let a second thread skip `acquire` and provided no cross-thread exclusion — the guard's prerequisite). **Eviction scans run unmasked and commit under a re-validated masked window** (`evict_slot`/`evict_lowest`): no mask is held across an O(n) scan, `alloc_backing`, or an `on_evict` callback. `release_asset` remains **unguarded** (owner-teardown path, out of scope — see §4).
- **Verified (this session):** unit + proptest suite green (`cargo test -p deluge_resource`, 32 tests incl. `concurrent_lease_churn` + the `churn_never_violates_invariants` proptest); the crate's unit-level ThreadSanitizer coverage on `concurrent_lease_churn` reports **0 data races**, closing the `acquire`-vs-`release` `Cell<ChunkSlot>` race at the unit level; golden mixdown **bit-exact** against baseline `eae09f094e71888a…` (Tasks 1-6 changed no C++ source — confirmed `git diff` touches only `crates/deluge_resource/` + `services.rs`); firmware **green** (`./dbt build Debug`, `deluge.elf` linked, the `deluge_resource` device-path guard compiled into the image) and the `deluge_resource` device-target build green (`cargo check -p deluge_resource --target armv7a-none-eabihf`). **NOT run to completion this session:** the full `preemptive_race_tsan` end-to-end harness (its stale TSan tree needed a fresh rebuild that is expensive on this memory-pressured host and was not completed here) — the end-to-end confirmation that the B1 `Cell<ChunkSlot>` / `mem::replace` / `loader_next` SUMMARY signatures no longer fire under sustained preemptive playback+record **remains outstanding**.

- **Race (historical):** `Manager::release` (audio thread — via `Voice`/`SampleLowLevelReader` unassign → `cluster::remove_reason` → `release_lease`) vs `Manager::loader_next` (loader/fiber thread — picking the next queued cluster) on the **same `Cell<ChunkSlot>` slot**. `loader_enqueue`/`loader_remove`/`acquire`/`request`/`evict_lowest` plausibly touch the same table too — all were bare `Cell::get`/`Cell::set` with no synchronization.
- **TSan sites:** `core/src/cell.rs:555:18` (`Cell<ChunkSlot>::get`) vs `core/src/mem/mod.rs:970:49` (`mem::replace::<ChunkSlot>`).
- **Root cause:** `Manager` was written single-threaded (see its own `stat()` doc comment) but is reached from Rust-`unsafe` `extern "C"` entry points with **no lock**, so nothing enforces that assumption once C++ calls it from two real OS threads.
- **Impact:** non-atomic read-modify-write on lease counts / queued flags can silently **lose an update** → a lease undercounted → **premature eviction of a still-playing cluster** (audio glitch/wrong data), or a queued flag never cleared → the loader spins on a phantom queue entry.
- **Recorder reachability (resolved by this fix):** with B2/B3 fixed, `SampleRecorder::createNextCluster()` → `SampleStream::get_cluster(CLUSTER_DONT_LOAD)` → `deluge_resource_request` → `Manager::request` was found to also take a manager lease on the audio thread, hitting this same `Cell<ChunkSlot>` — so B1 was reached from **both** playback and the recorder. The **Fix** above (asymmetric fiber-side critical sections) covers this call path too, so the recorder's allocation site is synchronized along with the rest of `Manager`.

## B2 — `SampleRecorder`/`SampleStream` record-cluster state: `std::vector` realloc UAF — HIGH (worst) — **FIXED (2026-07-19)**

- **Race (was):** `SampleRecorder::createNextCluster()` (audio thread, real-time capture — incremented `currentRecordClusterIndex` and **grew** `SampleStream::table_`, a `std::vector<SampleCluster>`) vs `SampleRecorder::cardRoutine()` → `writeOneCompletedCluster()` → `SampleStream::chunk_at()` (loader/fiber thread) on the **same `currentRecordClusterIndex` field and the same `table_` vector**. If `table_` reallocated on the audio thread while the fiber was mid-index into the old backing array → **use-after-free / OOB read**.
- **Fix:** `SampleStream::table_` is now a **stable-address `deluge::SegmentedVector`** (`src/deluge/util/segmented_vector.h`) whose elements never move on growth, so a grow can no longer move storage under a concurrent reader. The `currentRecordClusterIndex` hand-off is a **release/acquire `std::atomic`** (producer completion store ↔ fiber consumer loop-bound load), with the finalize ownership-flip carried by an atomic `status`.
- **Verified:** the `preemptive_race_tsan` harness under sustained record-while-stream reports **no** SUMMARY race on `createNextCluster` / `cardRoutine` / `writeCluster` / `chunk_at` / `entry`.

## B3 — record payload buffer: audio `copy_n` vs fiber `write(2)` — HIGH — **FIXED (2026-07-19)**

- **Race (was):** the audio thread's `std::copy_n` into the in-progress record cluster's buffer vs the fiber's `write(2)` (`deluge_bsp::sd::host::write_sectors`) of that **same buffer** — a torn/half-written buffer if the copy landed mid-write.
- **Root cause / fix:** **same as B2** — the recorder producer/consumer hand-off had no synchronization. The B2 release/acquire completion edge now orders the completed cluster's payload writes happens-before the fiber's write of that cluster (the fiber only drains clusters `< currentRecordClusterIndex`, which it acquires), so the fiber never reads a buffer the audio thread is still filling.
- **Verified:** no recorder-payload torn-write SUMMARY (`write` / `__tsan_memmove` traced to the recorder) appears in the harness. (Note: a bare `__tsan_memset`/`__tsan_memmove` SUMMARY still appears but traces to **B1**'s `Cell<ChunkSlot>` `mem::replace`, not the recorder — see `open_findings_races.txt`.)

---

## B4 — rung-5 priority queue: NORMAL-job starvation (no starvation-freedom guarantee) — MEDIUM

- **Where:** `src/bsp/rust/src/fiber.rs` `dequeue()` — strict HIGH-before-NORMAL priority (added in the async-SD rung-5 flip).
- **Symptom (demonstrated deterministically by Lens 1's zero-jitter clock):** `loader::request_pump`'s periodic HIGH-priority dispatch re-arms itself (~100–200 µs) strictly before any fallback, so with no timing jitter a HIGH job is *always* in the ring at every `dequeue`, and strict HIGH-before-NORMAL **starves every NORMAL job forever** — `LoadSongUI::performLoad`'s dispatched job never ran once.
- **On device:** real-hardware timing jitter opens a gap between `request_pump`'s completion and its re-arm during which a pending NORMAL job wins, so *total* starvation is practically unreachable — **but the policy has no starvation-freedom guarantee.** This is the "fairness edge" flagged in the rung-5 design.
- **Decision owed:** whether to ship a conservative **priority-aging backstop** in the production `dequeue` (bounded fairness — force a NORMAL pick after N consecutive HIGH picks *while a NORMAL waits*), as a belt-and-suspenders against NORMAL delay under sustained streaming. A Lens-1-only aging knob (`fiber::HIGH_PRIORITY_FAIRNESS_BOUND`, **=0/inert in production**) exists as the pattern. Low-urgency; its own small change.

## B5 — rung-5 priority queue: unsynchronized `Q_COUNT` / ring links — HIGH — **OPEN (new, 2026-07-19)**

- **Race:** `deluge_rust::fiber::enqueue` writes `deluge_rust::fiber::Q_COUNT` (and the two-level priority ring links) on the **audio thread** — via `loader::request_pump` → `Coalescer::request` → `deluge::storage::Owner::run_priority` → `deluge_worker_run_priority` → `fiber::enqueue` (a HIGH-priority loader-fill dispatch issued from the audio render path) — while the **fiber (owner) thread** reads `Q_COUNT` in `deluge_rust::fiber::queue_nonempty` / `dequeue`.
- **TSan site:** `src/bsp/rust/src/fiber.rs:470` (`enqueue`, write of `Q_COUNT`) vs `fiber.rs:662` (`queue_nonempty`, read). Global `deluge_rust::fiber::Q_COUNT`.
- **Root cause:** the **rung-5 two-level priority queue** (async-SD staging ladder) has no synchronization on its counters/links between an audio-thread HIGH enqueue and the fiber's queue check — same single-threaded-by-assumption class as B1. **Not introduced by the B2/B3 recorder fix** (which touched none of `fiber.rs` / `owner.cpp` / `loader.cpp`); surfaced here under preemptive audio + sustained streaming. Belongs to the async-SD ladder / preemptive-scheduler work, not the recorder.

## Additional surfaced races — preemptive **streaming/playback** path (pre-existing, nondeterministic, out of scope)

The harness intermittently surfaces a broader set of preemptive-audio races on the **streaming/playback + loader + resource-manager + allocator** path — e.g. `deluge::audio::stream::loader::reconstruct_one`, `SampleStream::read_cluster_data`, `stitch_boundaries`, `StreamedChunk::payload()`, `deluge_resource_loader_next` (B1's Manager, at a different SUMMARY frame), and `deluge_alloc::tlsf::Tlsf::remove_free_block`. These are the audio render thread reading/loading a cluster while the loader fiber reconstructs it (and the shared allocator/manager underneath) — the **same single-threaded-by-assumption class as B1/B5**, applied to the *playback* side rather than the recorder.

They are **not introduced by this branch** (which touches only `sample_recorder.*`, `sample_stream.*` [container swap + `reserve`], and the new `segmented_vector.h` — none of `loader.cpp` / `stitch.cpp` / `cluster.h` / `manager.rs` / `tlsf.rs`), and they fire **nondeterministically**: e.g. one `BLOCKS=2000` run reported 7 uncatalogued while the next two reported 0. Consequently a run's `UNCATALOGUED=0` is **not guaranteed** and should not be read as "the storage path is race-free." The reliable, reproducible guarantee this branch establishes is narrower and specific: the **recorder hand-off races (B2/B3) never appear in any run** post-fix. The streaming-path races are catalogued neither in `known_patterns.txt` nor `open_findings_races.txt` (the catalog is incomplete for this surface) and remain **open, out of scope** — prerequisites, alongside B1/B4/B5, for the broader "make the whole storage path preemption-safe" work.

## Caveat — harness scenario stability at high `BLOCKS`

Under the extended `BLOCKS=8000` record-while-stream scenario, 2 of 5 runs reported `scenario=FAILED` (the run did not reach a clean completion — likely the `timeout 300` cap and/or B1/B5 corruption under sustained preemption). This does not affect the B2/B3 verification (the passing runs exercised many `createNextCluster` grows with the races gone, up to 294 completions — past the `SegmentedVector`'s 256-entry segment boundary — in a follow-up `BLOCKS=12000` repro that passed clean), but a shorter/tuned `BLOCKS` is advisable for a stable green run, and the failures are themselves circumstantial evidence that the remaining open races (B1/B5) can destabilize a long preemptive run.

**Re-verification note (2026-07-19, follow-up):** re-ran the `BLOCKS=8000` scenario directly (outside `run.sh`) under `gdb` with `TSAN_OPTIONS=handle_segv=0` to capture a full native backtrace of one of these crashes (the batch runs' own TSan-handled SEGV report was truncated — TSan's own signal handler produced no backtrace, only "SEGV on unknown address... caused by a WRITE"). The captured crash is **inside ThreadSanitizer's own runtime** (`__tsan::TraceSwitchPartImpl` / `__tsan::TraceRestartMemoryAccess`), triggered while instrumenting a `Cell<ChunkSlot>::get` access in `Manager::loader_next` (B1's hot read path, called every fiber poll) — not a wild-pointer write reachable from application code. This is consistent with a **ThreadSanitizer trace-buffer/shadow-memory limitation under very long, high-event-count runs combined with host memory pressure** (this host was running with ~20/30GB swap in use at the time) rather than new applicaton-level memory corruption; it does not, by itself, confirm the "B1/B5 corruption" hypothesis above, though it also does not rule it out (TSan's instrumentation could itself be destabilized by prior corruption). Reproduced at a rate of roughly 2-3 crashes per 7 attempts at `BLOCKS=8000` on this host; not reproduced at `BLOCKS=12000` in a single follow-up run, consistent with a rare/environmental trigger rather than a deterministic function of block count or table-segment growth. Treat as a harness/tooling robustness note (mind memory headroom for large `BLOCKS`×`NUM_RUNS` sweeps on memory-constrained hosts), not a new confirmed application bug.

---

## Note: a harness-instrument gap (found and already FIXED)

Not a production bug, recorded for context: the harness's own `loaded`-miss underrun instrument initially missed `SampleLowLevelReader::moveOnToNextCluster` — the *common* sustained-streaming miss site — so it under-reported (couldn't fire underrun at all at first). Fixed in `fb0dc5e3a` (added the guarded counter there). This is why the negative control (proving the instrument *can* detect underrun) matters.

---

*This record was produced from the streaming-underrun harness's findings. The fuller per-finding writeups + both racing stacks live in the harness's `open_findings_races.txt` (committed) and the (local) task reports. **B2 + B3 are fixed** (2026-07-19, recorder hand-off: stable-address `SegmentedVector` + release/acquire atomics); **B1, B4, B5 remain open** and are the prerequisites still owed before shipping preemptive audio with SD record-while-stream.*

## B6 — `CLUSTER_LOAD_IMMEDIATELY` performs card transfers off the storage owner — HIGH — **OPEN (new, 2026-07-21)**

- **Where:** `SampleStream::get_cluster` (`src/deluge/storage/audio/stream/sample_stream.cpp:316-346`) →
  `deluge_resource_acquire` → `cluster_materialize` (`:42-58`) → `read_cluster_data` (`:180-254`) →
  `StreamReadSource` → `deluge_stream_read_at` → `deluge_block_read`. **This whole chain runs
  synchronously in whatever context called `get_cluster`, with no fiber dispatch anywhere in it.**
- **Contrast with the path that gets it right:** `loader::request_pump` (`loader.cpp:169-192`) is
  fiber-aware by construction — if not already `deluge_storage_on_owner()` it dispatches via
  `Coalescer`/`Owner::run_priority`, otherwise it runs inline. `CLUSTER_LOAD_IMMEDIATELY` bypasses that
  machinery entirely.
- **Off-fiber callers found** (traced 2026-07-21; none wrapped in `deluge::storage::Owner::run`):
  `waveform_renderer.cpp:404,433` (UI waveform redraw — slicer, marker editor, audio-clip view, browser,
  instrument-clip view); `sample.cpp:1395,1420` via `sample_browser.cpp:1369,1578,1589`
  (`loadAllSamplesInFolder`, bulk import); `sample_holder_for_voice.cpp:139` via
  `sample_browser.cpp:1025,1799` (`claimCurrentFile`); `sample_marker_editor.cpp:200` (marker drag);
  `audio_clip.cpp:1396` (`shiftHorizontally`).
  **Reached from BOTH contexts** (the dangerous case): `wave_table.cpp:422` and
  `cluster_byte_source.cpp:51`, both inside the general `AudioFile::load` /`buildAudioFileFromCard`
  pipeline, which is entered on-fiber for song load/preview and off-fiber for the UI paths above.
- **Confirmed NOT affected:** the RT audio render path never loads synchronously —
  `sample_low_level_reader.cpp:310,400`, `voice_sample.cpp:207,868`, `time_stretcher.cpp:1141` all use
  `CLUSTER_ENQUEUE`. `audio_engine.cpp:1391` (`previewSample`) *is* on-fiber: its only caller is
  `sample_browser.cpp:596`, dispatched via `Owner::run` at `:551/564`.
- **Why it goes unnoticed:** `src/bsp/rust/src/sd.rs:386-390` carries exactly the assert that would catch
  this — `debug_assert!(on_fiber() || !worker_started())`, "FatFS card transfer off the storage owner
  after the owner started — single-owner discipline violated at runtime" — but it is
  `#[cfg(feature = "storage-owner-audit")]`, and that feature is **not** in `default`
  (`src/bsp/rust/Cargo.toml:117` = `["rtt", "async_streaming_loader"]`). Meanwhile the transfer still
  *works*: `sd.rs:398` falls back to a parking `block_on` when off-fiber. That fallback is documented as
  being for the **boot mount**, valid only "before the owner is up".
- **Impact:** C FatFS is **not re-entrant**, which is the entire reason the single-owner discipline
  exists. A UI-thread synchronous card transfer concurrent with a fiber-side FatFS op is the corruption
  case the ladder was built to prevent. Also a latency hazard: these parks block the UI thread for a
  full SD transfer.
- **Reachability caveat (do not overstate):** the off-fiber call sites are *proven*; an actual concurrent
  FatFS entry has **not** been demonstrated. The bug is that the discipline is unenforced on this path,
  not that a specific corruption has been observed. A first repro step is to enable
  `storage-owner-audit` and exercise the waveform renderer or marker editor.
- **Fix (designed, not implemented):** give `get_cluster`'s synchronous-acquire path the same on-fiber
  dispatch `request_pump` already has — one location, structural, so the property holds for callers that
  do not know about it. Tracked as the prerequisite step of SP-stream-read
  (`docs/dev/rustfs_sp_stream_read_completion_design.md` §6); that rung **requires** it, because once the
  cluster→sector map is deleted there is no non-fiber fallback left.

## B7 — efatfs block device bypasses `SD_BUS` arbitration — HIGH — **FIXED (2026-07-21)**

- **Fix:** `SdBlockDevice::read`/`write` now call `crate::sd::locked_read_sectors`/`locked_write_sectors`,
  taking `SD_BUS` for the whole transfer exactly as the C-FatFS path does. Lock order is FS → SD_BUS
  (this impl runs under the efatfs FS mutex); nothing takes SD_BUS then FS, so no inversion. A comment
  guard above the `impl` records why the raw driver calls must never come back.
  **Still NEEDS-HARDWARE:** the interleaving was inferred from source, and the fix is verified by build +
  inspection only — there is no host test, since `fat_block_device.rs` is `cfg(target_os = "none")`.
- **Where:** `src/bsp/rust/src/fat_block_device.rs:64,78` — `SdBlockDevice::read`/`write` call
  `deluge_bsp::sd::read_sectors`/`write_sectors` **directly**, not `crate::sd::locked_read_sectors`
  /`locked_write_sectors`. The efatfs path therefore **never acquires `SD_BUS`**.
- **Why `SD_BUS` exists** (`src/bsp/rust/src/sd.rs:51-54`): `block_on_fiber` yields the executor
  *mid-DMA*, so without arbitration two contexts can drive the single SDHI controller concurrently. The
  C-FatFS path holds `SD_BUS` across its awaits (`sd.rs:197`, `:397`).
- **Impact:** a fiber-side C-FatFS transfer and an efatfs transfer can **interleave on the same SDHI
  controller**. Corrupt reads, corrupt writes, or a wedged controller. Note the two filesystems coexist by
  design during the migration, so this is reachable whenever both are in use.
- **Status:** live **only when `efatfs_streaming` is enabled** (not a default feature,
  `src/bsp/rust/Cargo.toml:117`). It was recorded as an SP1 carry-forward ("SdBlockDevice must go through
  SD_BUS arbitration (currently bypasses)") and never actioned.
- **⚠️ BLOCKED the SP-stream-read flag flip** — now unblocked by the fix above. `efatfs_streaming` must
  not have become a default feature until this was fixed, because the flip is precisely what makes it
  reachable in shipped builds.
- **Found by:** source analysis while verifying whether efatfs self-serialization removes the need for
  fiber dispatch (it does not — see B6 and
  `docs/dev/rustfs_sp_stream_read_completion_design.md` §6). Inferred from source; not observed on
  hardware.
