# SD / file-I/O migration roadmap — from here to the Rust-native FS

**Status:** approved roadmap (strategy/decomposition). Each rung below gets its own spec → plan →
implementation cycle; this doc is the sequence, the gates, and the dependencies.
**Base:** `feat/rustfs-sp1b-cached-chain` (B6/B7 software-complete; next+4460 merged).
**North-star:** `docs/superpowers/specs/2026-07-20-async-storage-end-architecture-design.md` §5 (the
SP0–SP6 ladder) and `[[async-storage-end-architecture]]`. This roadmap is that ladder re-planned from the
current mid-point, with the verification model settled.

## 1. Where we are

- **B6** (off-worker cold loads) and **B7** (efatfs SD_BUS bypass): **software-complete**, device runtime
  gate pending. These were the concurrency *prerequisites* to safely migrating the read path.
- **The sync read path is still on C FatFS** (`read_cluster_data` → `StreamReadSource` →
  `deluge_stream_read_at`). `efatfs_streaming` is default-off; `resolve_read_layout` (the cluster→sector
  map) is still present. The efatfs read migration — SP-stream-read's original goal — is **unblocked but
  not done**.
- `fs_differential` exists (efatfs ≡ C FatFS byte-exact on real FAT16/32 images; found two real crate
  bugs). The **lens harnesses** exist on this branch: `lens1_vt_sim` (deterministic underrun-margin) and
  `preemptive_race_tsan` (Lens 2 — real app under TSan with a preemptive audio thread; found B1/B2/B3).
- Open, off-spine: **B5** (rung-5 priority-queue race), **B8** (pre-existing AudioClip-delete-during-revert
  UAF). Deferred UI rungs: pitch-at-load, the UI↔storage north-star (`[[ui-storage-decoupling-northstar]]`).
- **P1** (preemptive audio, `InterruptExecutor`) is code-complete, **not yet hardware-proven** — it gates
  SP-delete only.

## 2. Verification model (settled) — harness-first, device as periodic confidence

**Golden cannot bit-exact-gate a FAT-layer change** under the SDK-layer/passthrough end-state (correct
bytes → identical audio regardless of FS). So the FS migration is **not** gated on golden, and we do
**not** build FAT-on-host for golden. Each rung gates on host harnesses:

- **`fs_differential`** — byte-exact FS correctness (efatfs ≡ C FatFS on real images).
- **Lens 1 (`lens1_vt_sim`)** — no-underrun margin as a function of modeled SD latency.
- **Lens 2 (`preemptive_race_tsan`)** — no new concurrency races (real app, preemptive audio, TSan).
- **Power-loss fault-injection** (new for R3) — simulated power-cut at each recorder writeback point,
  inspect FS state.

**Device shrinks to a periodic confidence pass** (final latency feel, real-SD-throughput sanity) — NOT a
per-rung blocker. This is realizable because Lens 1/2 already model the two things device was really for
(underrun, preemptive races), and they run on **`host_app`** (the Rust BSP linked with the C++ app), which
**can link efatfs** — unlike the C-host-BSP sim. Enabling that is R0.

**Cadence: software-first.** Build R0, then R1→R3 run ahead gated entirely on host harnesses; hardware is
an occasional confidence pass, not a gate between rungs. (Decision: Kate, 2026-07-22.)

## 3. The rungs

### R0 — Harness enablement (the "obviate device gates" foundation) — buildable NOW
- Enable efatfs in `host_app` (it links the Rust BSP; `efatfs_fs.rs` is present — turn on the
  `efatfs_streaming` path in the host_app build).
- Point **Lens 1** (margin) and **Lens 2** (TSan) at the efatfs read path.
- Extend `fs_differential` with the **streaming access-pattern differential** (cluster-aligned reads in
  playback order incl. loop points, efatfs vs C FatFS).
- **Exit:** every subsequent efatfs rung can be gated on host — byte-exact + no-underrun + no-new-races —
  without hardware.
- **No device gate of its own** (it's harness infrastructure). Independent of the B6/B7 device gate.

### R1 — efatfs read-path migration (finishes SP-stream-read)
- Route `read_cluster_data` through efatfs (`block_on_fiber` — safe now that the loads are on-worker,
  B6). Retire `resolve_read_layout` + the `sdAddress` map (the "nothing above the port knows about
  sectors/clusters" exit criterion). Flip `efatfs_streaming` default-on.
- **Gate:** `fs_differential` + R0's streaming differential + Lens 1 (margin no-regression vs the SP1a
  1.03x baseline) + Lens 2 (no new races).
- **Depends on:** R0 (for the gate). Does **not** need the B6/B7 device gate to *build* — but the default
  flip should not ship to hardware until B6/B7 are device-confirmed (they underpin it).

### R2 — SP-fileio: re-back `file_io.h` / `stream_io.h` onto efatfs
- Flip task-context byte-I/O + directory enumeration from C FatFS to efatfs, under the port boundary.
  C++ above the port unchanged. The fiber persists as the blocking context (retired later, at R4).
- **Gate:** `fs_differential` + Lens 2 (task-context ops under preemptive audio).
- **Depends on:** R1 (streaming read already off C FatFS).

### R3 — SP-recorder: recorder read + write/finalize onto efatfs
- Contiguous-preallocation write fast path; async recorder-writeback; **power-loss safety**.
- **Gate:** `fs_differential` + Lens 2 + the **power-loss fault-injection harness** (new). This rung's
  device confidence pass (real pull-the-plug) is worth doing even under software-first, because power-loss
  is the highest-stakes device-only behavior — but the *gate* is the fault-injection harness.
- **Depends on:** R2.

### R4 — SP-delete: delete C FatFS, retire the fiber
- Once nothing calls `ff.c`: delete chan-fs, retire the worker fiber, move task-context storage onto the
  plain blocking worker context.
- **Gate:** `fs_differential` (now efatfs-only, regression net) + Lens 2 + **P1 hardware-proven**.
- **Depends on:** R1–R3 **and P1** (preemptive audio on hardware — the one hard hardware dependency in the
  whole roadmap).

## 4. Off-spine / parallel work

- **`fs_differential` streaming differential** — folded into R0 (it's R0's third deliverable).
- **B5** (rung-5 priority-queue `Q_COUNT`/ring race) and **B8** (AudioClip-delete-during-revert UAF) —
  independent HIGH bugs, own tickets, fixable anytime (Lens 2 is the harness for both).
- **Deferred UI rungs** — pitch-at-load (compute derived sample data at load, store on model) and the
  UI↔storage north-star. Model-ownership work, orthogonal to the FS-backing swap; sequence later.

## 5. Critical path & dependency summary

```
R0 (harness enablement) ──► R1 (read migration) ──► R2 (SP-fileio) ──► R3 (SP-recorder) ──► R4 (SP-delete)
   [no device gate]           gate: fs_diff +          gate: fs_diff +     gate: fs_diff +      gate: fs_diff +
                              streaming-diff +          Lens 2               Lens 2 +             Lens 2 +
                              Lens 1 + Lens 2                                power-loss harness   ★ P1 (HW)

B6/B7 device gate (Kate) ──► confirms the concurrency foundation R1 ships on; batched, not a per-rung blocker.
Device confidence pass ──► periodic (latency feel, real-SD throughput); NOT between every rung.
Off-spine: B5, B8 (own tickets) · pitch-at-load, UI north-star (deferred).
```

**Immediate next:** R0. It's buildable now, has no device dependency, and is what makes the entire
software-first cadence real — turning "device gate" into "host harness run" for R1–R3.

## 6. Out of scope

- FAT-on-host for golden bit-exactness (§2 — wrong under the passthrough end-state).
- exFAT (FAT16/32 + LFN only, per the end-architecture decision).
- The SDK-layer / passthrough sim end-state itself (a later, separate concern; this roadmap gets the
  device path onto the Rust FS, which is the precondition).
- SP5/SP6 (position-driven prefetch; convert/stitch → Rust) — optional north-star tail, after R4.
