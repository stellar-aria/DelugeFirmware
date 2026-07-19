# Known Concurrency Bugs — preemptive-audio storage/streaming path

**Status:** PARTIALLY FIXED — **B2 + B3 FIXED** (2026-07-19, verified TSan-clean; see each below). **B1, B4, and B5 (new) remain OPEN.**
**Found by:** the host streaming-underrun harness (`src/bsp/rust/preemptive_race_tsan/` — ThreadSanitizer over the real `deluge_app` with a preemptive audio thread) and its deterministic timing lens (`src/bsp/rust/lens1_vt_sim/`). All found on host, **without hardware**.

## Scope / when these bite

Bugs **B1, B5** are data races on the audio-thread ↔ loader/fiber-thread hand-off in the **storage / cluster / resource-manager / scheduler** path (B2 + B3, on the **recorder** hand-off, are now fixed). They manifest **only when the audio task is genuinely preemptive** relative to the loader/fiber thread — i.e. on the **Embassy `InterruptExecutor` target** (see the `embassy-scheduler-migration` / `audio-callback-relocation` work). On the legacy **cooperative** BSP (audio runs cooperatively, never preempting mid-op) they cannot occur.

**⇒ The remaining open races must be fixed before the preemptive-audio architecture ships in combination with SD streaming + record-while-stream.** They are exactly the kind of narrow-window corruption that a device test tends to surface only as a rare, hard-to-repro glitch or crash — TSan makes them deterministic to find (but not to trigger in the field).

Reproduce: `cd src/bsp/rust/preemptive_race_tsan && ./run.sh` (needs a pre-packed `DELUGE_SD_IMAGE`; see `run.sh` header). Findings are catalogued in `preemptive_race_tsan/open_findings_races.txt`; the broad pre-existing cooperative-scheduling debt (UI/song-model/reverb/transport, out of scope here) is baselined in `known_patterns.txt`.

---

## B1 — `deluge_resource::Manager` chunk table: unsynchronized `Cell<ChunkSlot>` — HIGH

- **Race:** `Manager::release` (audio thread — via `Voice`/`SampleLowLevelReader` unassign → `cluster::remove_reason` → `release_lease`) vs `Manager::loader_next` (loader/fiber thread — picking the next queued cluster) on the **same `Cell<ChunkSlot>` slot**. `loader_enqueue`/`loader_remove`/`acquire`/`request`/`evict_lowest` plausibly touch the same table too — all are bare `Cell::get`/`Cell::set` with no synchronization.
- **TSan sites:** `core/src/cell.rs:555:18` (`Cell<ChunkSlot>::get`) vs `core/src/mem/mod.rs:970:49` (`mem::replace::<ChunkSlot>`).
- **Root cause:** `Manager` was written single-threaded (see its own `stat()` doc comment) but is reached from Rust-`unsafe` `extern "C"` entry points with **no lock**, so nothing enforces that assumption once C++ calls it from two real OS threads.
- **Impact:** non-atomic read-modify-write on lease counts / queued flags can silently **lose an update** → a lease undercounted → **premature eviction of a still-playing cluster** (audio glitch/wrong data), or a queued flag never cleared → the loader spins on a phantom queue entry.
- **Update (2026-07-19):** now that B2/B3 are fixed, B1 is the **only** remaining race the recorder participates in — `SampleRecorder::createNextCluster()` → `SampleStream::get_cluster(CLUSTER_DONT_LOAD)` → `deluge_resource_request` → `Manager::request` takes a manager lease on the audio thread, hitting this same `Cell<ChunkSlot>`. So B1 is reached from **both** playback and the recorder. Fixing B1 (manager synchronization / single-threaded-access guarantee) is the remaining prerequisite at the recorder allocation site.

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

## Caveat — harness scenario stability at high `BLOCKS`

Under the extended `BLOCKS=8000` record-while-stream scenario, 2 of 5 runs reported `scenario=FAILED` (the run did not reach a clean completion — likely the `timeout 300` cap and/or B1/B5 corruption under sustained preemption). This does not affect the B2/B3 verification (the passing runs exercised many `createNextCluster` grows with the races gone), but a shorter/tuned `BLOCKS` is advisable for a stable green run, and the failures are themselves circumstantial evidence that the remaining open races (B1/B5) can destabilize a long preemptive run.

---

## Note: a harness-instrument gap (found and already FIXED)

Not a production bug, recorded for context: the harness's own `loaded`-miss underrun instrument initially missed `SampleLowLevelReader::moveOnToNextCluster` — the *common* sustained-streaming miss site — so it under-reported (couldn't fire underrun at all at first). Fixed in `fb0dc5e3a` (added the guarded counter there). This is why the negative control (proving the instrument *can* detect underrun) matters.

---

*This record was produced from the streaming-underrun harness's findings. The fuller per-finding writeups + both racing stacks live in the harness's `open_findings_races.txt` (committed) and the (local) task reports. **B2 + B3 are fixed** (2026-07-19, recorder hand-off: stable-address `SegmentedVector` + release/acquire atomics); **B1, B4, B5 remain open** and are the prerequisites still owed before shipping preemptive audio with SD record-while-stream.*
