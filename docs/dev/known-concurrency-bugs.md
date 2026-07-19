# Known Concurrency Bugs — preemptive-audio storage/streaming path

**Status:** OPEN (detectors exist; fixes are separate work)
**Found by:** the host streaming-underrun harness (`src/bsp/rust/lens2_tsan/` — ThreadSanitizer over the real `deluge_app` with a preemptive audio thread) and its deterministic timing lens (`src/bsp/rust/lens1_vt_sim/`). All found on host, **without hardware**.

## Scope / when these bite

Bugs **B1–B3** are data races on the audio-thread ↔ loader/fiber-thread hand-off in the **storage / cluster / recorder / resource-manager** path. They manifest **only when the audio task is genuinely preemptive** relative to the loader/fiber thread — i.e. on the **Embassy `InterruptExecutor` target** (see the `embassy-scheduler-migration` / `audio-callback-relocation` work). On the legacy **cooperative** BSP (audio runs cooperatively, never preempting mid-op) they cannot occur.

**⇒ These must be fixed before the preemptive-audio architecture ships in combination with SD streaming + record-while-stream.** They are exactly the kind of narrow-window corruption that a device test tends to surface only as a rare, hard-to-repro glitch or crash — TSan makes them deterministic to find (but not to trigger in the field).

Reproduce: `cd src/bsp/rust/lens2_tsan && ./run.sh` (needs a pre-packed `DELUGE_SD_IMAGE`; see `run.sh` header). Findings are catalogued in `lens2_tsan/open_findings_races.txt`; the broad pre-existing cooperative-scheduling debt (UI/song-model/reverb/transport, out of scope here) is baselined in `known_patterns.txt`.

---

## B1 — `deluge_resource::Manager` chunk table: unsynchronized `Cell<ChunkSlot>` — HIGH

- **Race:** `Manager::release` (audio thread — via `Voice`/`SampleLowLevelReader` unassign → `cluster::remove_reason` → `release_lease`) vs `Manager::loader_next` (loader/fiber thread — picking the next queued cluster) on the **same `Cell<ChunkSlot>` slot**. `loader_enqueue`/`loader_remove`/`acquire`/`request`/`evict_lowest` plausibly touch the same table too — all are bare `Cell::get`/`Cell::set` with no synchronization.
- **TSan sites:** `core/src/cell.rs:555:18` (`Cell<ChunkSlot>::get`) vs `core/src/mem/mod.rs:970:49` (`mem::replace::<ChunkSlot>`).
- **Root cause:** `Manager` was written single-threaded (see its own `stat()` doc comment) but is reached from Rust-`unsafe` `extern "C"` entry points with **no lock**, so nothing enforces that assumption once C++ calls it from two real OS threads.
- **Impact:** non-atomic read-modify-write on lease counts / queued flags can silently **lose an update** → a lease undercounted → **premature eviction of a still-playing cluster** (audio glitch/wrong data), or a queued flag never cleared → the loader spins on a phantom queue entry.

## B2 — `SampleRecorder`/`SampleStream` record-cluster state: `std::vector` realloc UAF — HIGH (worst)

- **Race:** `SampleRecorder::createNextCluster()` (audio thread, real-time capture — increments `currentRecordClusterIndex` and **grows** `SampleStream::table_`, a `std::vector<SampleCluster>`) vs `SampleRecorder::cardRoutine()` → `writeOneCompletedCluster()` → `SampleStream::chunk_at()` (loader/fiber thread, the card-write drain) on the **same `currentRecordClusterIndex` field and the same `table_` vector**.
- **TSan sites:** `sample_recorder.cpp:555:36` (`cardRoutine()`) & `sample_recorder.cpp:914:27` (`createNextCluster()`) & `sample_stream.cpp:380:9` (`SampleStream::chunk_at()`).
- **Impact:** if `table_` **reallocates** (moves its backing storage) on the audio thread at the moment the fiber thread is mid-index into the OLD backing array, that is a genuine **use-after-free / OOB read** — a real (narrow-window) crash/corruption hazard during sustained recording, not merely a torn value.

## B3 — record payload buffer: audio `copy_n` vs fiber `write(2)` — HIGH

- **Race:** the audio thread's `std::copy_n` (feeding captured samples into the in-progress record cluster's buffer, via `__tsan_memmove`) vs the fiber thread's real `write(2)` syscall (`deluge_bsp::sd::host::write_sectors`) writing that **same buffer** out to disk — the fiber can read a torn/half-written buffer if the audio copy lands mid-write. Both race on the `HOST_SDRAM` region.
- **TSan sites:** generic (`in write`, `in __tsan_memmove`) — ambiguous alone; **confirmed by manual trace** to the SampleRecorder hand-off. Re-verify the paired stack before treating a future match as this same finding.
- **Root cause:** **same as B2** — `SampleRecorder`'s cluster hand-off from audio-thread producer to fiber-thread consumer has no synchronization. B2 and B3 should be fixed together (a producer/consumer sync on the recorder cluster hand-off).

---

## B4 — rung-5 priority queue: NORMAL-job starvation (no starvation-freedom guarantee) — MEDIUM

- **Where:** `src/bsp/rust/src/fiber.rs` `dequeue()` — strict HIGH-before-NORMAL priority (added in the async-SD rung-5 flip).
- **Symptom (demonstrated deterministically by Lens 1's zero-jitter clock):** `loader::request_pump`'s periodic HIGH-priority dispatch re-arms itself (~100–200 µs) strictly before any fallback, so with no timing jitter a HIGH job is *always* in the ring at every `dequeue`, and strict HIGH-before-NORMAL **starves every NORMAL job forever** — `LoadSongUI::performLoad`'s dispatched job never ran once.
- **On device:** real-hardware timing jitter opens a gap between `request_pump`'s completion and its re-arm during which a pending NORMAL job wins, so *total* starvation is practically unreachable — **but the policy has no starvation-freedom guarantee.** This is the "fairness edge" flagged in the rung-5 design.
- **Decision owed:** whether to ship a conservative **priority-aging backstop** in the production `dequeue` (bounded fairness — force a NORMAL pick after N consecutive HIGH picks *while a NORMAL waits*), as a belt-and-suspenders against NORMAL delay under sustained streaming. A Lens-1-only aging knob (`fiber::HIGH_PRIORITY_FAIRNESS_BOUND`, **=0/inert in production**) exists as the pattern. Low-urgency; its own small change.

---

## Note: a harness-instrument gap (found and already FIXED)

Not a production bug, recorded for context: the harness's own `loaded`-miss underrun instrument initially missed `SampleLowLevelReader::moveOnToNextCluster` — the *common* sustained-streaming miss site — so it under-reported (couldn't fire underrun at all at first). Fixed in `fb0dc5e3a` (added the guarded counter there). This is why the negative control (proving the instrument *can* detect underrun) matters.

---

*This record was produced from the streaming-underrun harness's findings. The fuller per-finding writeups + both racing stacks live in the harness's `open_findings_races.txt` (committed) and the (local) task reports. Fixing B1–B3 is prerequisite to shipping preemptive audio with SD record-while-stream.*
