# R0 — harness enablement: efatfs on host, gated by the lenses — design

**Status:** approved design, ready for an implementation plan. Nothing implemented.
**Base:** `feat/rustfs-sp1b-cached-chain`.
**Roadmap:** `docs/dev/rustfs_migration_roadmap.md` §3 R0 — the foundation that turns "device gate" into
"host harness run" for R1–R3.
**Verification model:** harness-first (roadmap §2). R0 builds the harness coverage the later rungs gate on.

## 1. Why R0 is a rung, not a flag flip

Validated by source (2026-07-22): `--features efatfs_streaming` on a `host_app` build compiles **nothing
new**. Every efatfs file is `#![cfg(target_os = "none")]` (`efatfs_fs.rs:21`, `fat_block_device.rs:22`,
the `efatfs_core` module decl `main.rs:109`, the `ProdOps::read` efatfs branch `streaming_loader.rs:310`),
and the whole storage crate stack (`embedded-fatfs`, block-device adapters, `aligned`) sits in
`[target.'cfg(target_os = "none")'.dependencies]` (`Cargo.toml:52-76`) — so on `host_app`'s x86-64 triple
those crates are not even in the dependency graph.

The assets that make it *bounded*: `efatfs_core` is storage-generic and already compiles host-side
(`fs_differential` proves it); `crate::sd` is dual-target with a file-backed host image; the block-device
adapter stack runs on host in `fs_differential`.

## 2. Decomposition

**R0a — streaming access-pattern differential** (in `fs_differential`; NO `host_app`; independent,
low-risk, buildable now). **R0b — host efatfs shim + lens integration** (the sub-project). R0a first: it
de-risks the schedule and is the byte-exact half of R1's gate regardless of R0b's outcome.

## 3. R0a — streaming access-pattern differential

A new `fs_differential` test that drives the **audio read access pattern** — cluster-aligned reads in
playback order, including loop-point backward seeks — through efatfs vs C FatFS on the same image, and
asserts byte-identical. `fs_differential` already mounts both filesystems over one RAM/file image
(`efatfs.rs`, `fatfs_c.rs`), so this reuses that machinery; it is a new op-sequence, not new
infrastructure.

- **Non-vacuity:** the test must be shown able to FAIL (fault-inject a wrong offset / a corrupted cluster),
  per the project's "real-execution catches what review misses" mandate.
- **Gate role:** the byte-exact half of R1's gate — proves the efatfs read serves the *streaming pattern*
  identically, complementing the existing whole-file/interleave differentials.
- **Env/run:** `SP0_FAT32`/`SP0_FAT16`, `--test-threads=1` (2.5 GB images OOM in parallel).

## 4. R0b — host efatfs shim + lens integration (isolated shim)

**Decision (Kate, 2026-07-22): isolated host shim, NOT relaxing the device cfg gates.** These harnesses
exist to GATE the device build; destabilizing device-critical files to enable a host harness is
self-defeating. The shim keeps the risk in test infrastructure.

### 4.1 The shim

A host-only module (`#[cfg(feature = "host_app")]`), a host counterpart to `efatfs_fs.rs` that **reuses
the storage-generic `efatfs_core` unchanged** — so the harness measures the *real* read logic, not a
parallel reimplementation. It provides:
- a **file-backed block device** — reuse `fs_differential`'s `FileBlockDevice` → `BufStream` →
  `embedded-fatfs` stack, pointed at the harness's SD image (the same image the C-FatFS diskio reads);
- `mount()` + a `HandleTable` over `efatfs_core` (the same split-primitive lock discipline; host is
  single-threaded so `read_at_owned` is available);
- the C-ABI `deluge_efatfs_open`/`deluge_efatfs_close` the C++ app calls (host versions), and the Rust
  `read_at` that `ProdOps::read` calls.

### 4.2 Injection — additive cfgs only, device path byte-unchanged

The only edits outside the shim are cfg wideners; no device logic changes:
- `ProdOps::read`'s efatfs branch (`streaming_loader.rs:310`) and `deluge_streaming_efatfs_active()`
  (`streaming_loader.rs:80`) widen from `all(target_os="none", efatfs_streaming)` to also accept
  `all(feature="host_app", feature="efatfs_streaming")`, calling the shim's `read_at`/returning true.
- The host boot paths — `host_app_task` and Lens 1's `boot_task` (`lens1_vt_sim/src/main.rs:176-199`) —
  gain the arena/allocator init + `mount()` call (the device mount lives in `app_task`, which is
  `target_os="none"`, so it is not reached on host).
- Enable `efatfs_streaming` in the two harness build invocations (`preemptive_race_tsan/run.sh`, Lens 1's
  Cargo features + the storage deps for the host build).

The device build sees only `any(...)`-widened cfgs whose device arm is unchanged → byte-identical device
image (verified in the gate, §5).

### 4.3 The one load-bearing risk

The shim's `mount()` runs `block_on` on the host boot path, which collides with the documented off-fiber
`block_on` livelock the lenses already work around for the C-FatFS mount
(`lens1_vt_sim/src/main.rs:18-54`, `sim_latency::set_off_fiber_instant`). The design **reuses that same
workaround** for the efatfs mount. If it does not extend cleanly, that surfaces at the first R0b build —
not later — and R0a is unaffected. Secondary, lower risks: the global allocator interaction
(`embedded-fatfs` `alloc` vs the device's `fs_alloc` arena — host uses the std heap), and running C FatFS
and a Rust efatfs mount over one image (avoid by not driving both in one harness process; the harness
measures efatfs *only*, flag-on — the differential baseline is R0a's separate `fs_differential` run).

## 5. What R0 proves, and R0's own gate

R0 turns R1's gate into a host harness run:
- **R0a** → efatfs reads the audio access pattern byte-identically to C FatFS (byte-exact half of R1).
- **R0b + Lens 1** → the efatfs read path's underrun margin, on host (comparable to the C-FatFS baseline /
  the SP1a 1.03x). Timing half.
- **R0b + Lens 2** → the efatfs read path under preemptive TSan — no new races beyond the catalogued
  baseline. Concurrency half.

**R0's own completion gate:**
- R0a green AND proven non-vacuous (fault-injection).
- Lens 1 runs with efatfs and produces a margin number.
- Lens 2 runs efatfs under TSan; races catalogued, no NEW efatfs-read races beyond
  `preemptive_race_tsan`'s known baseline (`known_patterns.txt`/`open_findings_races.txt`).
- **Device build stays green** — `./dbt build Debug` + `cargo device`; the cfg changes are additive, and a
  device build confirms the device image is byte-unchanged. Non-negotiable: the harness must not perturb
  the thing it gates.

## 6. Scope boundaries / non-goals

- R0 does **not** flip `efatfs_streaming` default-on for the device — only enables it in the *harness*
  builds. The device default flip is **R1**.
- R0 does **not** touch the device read path — additive cfgs only; the shim is host-only.
- R0 does **not** build FAT-on-host for the *sim* (`deluge_host`, C host BSP) — only for `host_app` (the
  harness vehicle). The sim stays C FatFS, consistent with the SDK-layer/passthrough end-state.
- The shim is **test infrastructure** — reusable across R1–R3's gating, not a device artifact.

## Appendix A — mount() spike result

**Task 2 (R0, THROWAWAY spike), 2026-07-22.** Empirically resolves §4.3's one load-bearing risk before
the shim is built.

**(a) Does the host efatfs mount livelock on the lens boot path? YES, confirmed by reproduction.**

`fs_differential`'s `EFatFs::mount()` (`src/bsp/rust/fs_differential/src/efatfs.rs:75-79`) already does
`block_on(FileSystem::new(...))` successfully on host — but its `FileBlockDevice::read`
(`src/bsp/rust/fs_differential/src/block_dev.rs:26-34`) calls `RamDisk::read_at` directly: an `async fn`
with no internal `.await` at all, so it resolves on the very first poll. That is a fundamentally different
code path from the lens boot mount, which goes through `sim_latency::modeled_read` — a *genuine* pend on
a separately-spawned `pump()` task's `Timer::after(latency).await`
(`src/bsp/rust/src/sd.rs:872-879`). `fs_differential`'s mount working on host proves nothing about the
lens boot path; it only proves `block_on` is safe over a future that never actually suspends.

Reproduced directly (spike code, since reverted — see below): a throwaway block device
(`SpikeBlockDevice`) was added to `lens1_vt_sim`, routing every `embedded-fatfs` block read through the
SAME `deluge_block_read` C-ABI dispatch FatFS's diskio uses (`src/bsp/rust/src/sd.rs:614-671`: the
`on_fiber` / `off_fiber_instant()` / else three-way branch), over a real FAT16 image reached via
`DELUGE_SD_IMAGE` (i.e. the modeled-latency SD path, not the instant `RamDisk`). Called
`block_on(FileSystem::new(storage, FsOptions::new()))` from `boot_task`
(`src/bsp/rust/lens1_vt_sim/src/main.rs`'s `boot_task`), off-fiber, before `deluge_app_init`/the worker
fiber exists — the exact position and call shape the C-FatFS boot mount uses.

With `sd::sim_latency::set_off_fiber_instant` forced to `false` (negative control — the workaround
disabled): `cargo run` under `timeout 12`, `RUST_LOG=info`, exits **124** (timeout-killed). The last log
line is `SPIKE: about to block_on(FileSystem::new(...))`; `SPIKE: mount returned` never prints. This is
the exact livelock mechanism `lens1_vt_sim/src/main.rs:18-54` documents: `block_on`'s tight busy-spin
poll loop occupies the one OS thread's stack from inside `executor.poll()`'s call to `boot_task`, so
`sim_latency::pump()`'s `Timer::after(...).await` (a genuine spawned-task future, needing the executor to
poll it) can never be polled — nothing can make virtual-clock-independent forward progress. Hangs
real (wall-clock) forever, confirmed by the `timeout` kill.

**(b) Does `set_off_fiber_instant` fix it? YES.**

Same reproduction, `sd::sim_latency::set_off_fiber_instant(true)` (the existing, already-shipped
workaround, set once at boot before any task spawns — `lens1_vt_sim/src/main.rs:318`, matching how it's
already applied for the C-FatFS mount): the mount completes in **55µs**, and a follow-up root-directory
read (the "one cluster read", driving the block device again post-mount) completes in **23µs**. Full
`RUST_LOG=info` run, no timeout needed, clean exit 0. The flag routes `deluge_block_read`'s off-fiber
branch straight to the real synchronous `sd::read_sectors` (`sd.rs:648-654`), skipping
`sim_latency::modeled_read`/`pump`/`Timer` entirely — there is nothing left for the busy-spin to wait on.

**(c) Mount strategy the shim's `mount()` must use.**

The shim's host block device must NOT be a bespoke reimplementation of the read dispatch (e.g. calling
`sim_latency::modeled_read` directly, or reading the file unconditionally) — it must go through the SAME
`on_fiber()` / `off_fiber_instant()` / else three-way dispatch `deluge_block_read`
(`src/bsp/rust/src/sd.rs:614-671`) already implements, so it inherits the workaround automatically rather
than needing its own copy of the flag-check logic. Concretely, for R0b:

- **Simplest, and recommended:** the shim's block-device `read`/`write` should call the existing
  `deluge_block_read`/`deluge_block_write` C-ABI functions directly (they're plain `pub extern "C" fn`s
  in the same crate, callable as ordinary Rust functions, not just via FFI) instead of reimplementing SD
  access. This is a **zero-new-risk** reuse: one code path serves both C-FatFS's diskio and efatfs's
  block device, so there's exactly one place the off-fiber dispatch/workaround lives, and it can never
  drift between the two mounts.
  - This also means the shim's boot mount does not need any DEVICE-side change — the dispatch already
    exists in shared `sd.rs`, unconditionally, for any host build.
- The boot-path call order stays exactly what §4.2 already specifies: `set_off_fiber_instant(true)` is
  already set once, globally, before any task spawns (`main.rs:318`, ahead of `boot_task`/`deluge_app_init`)
  — the efatfs `mount()` call added to `host_app_task`/Lens 1's `boot_task` needs NO additional wrapping,
  because it inherits the same global flag through the shared dispatch (previous bullet). No "wrap and
  restore" step is needed at the mount call site itself; the existing boot-time-global flag already covers
  it, exactly as designed in §4.2/§4.3.
- One caveat carried forward, not newly discovered: `set_off_fiber_instant` only affects the OFF-fiber
  branch. Any efatfs access issued from ON the storage-owner fiber (post-boot, e.g. R1's later on-fiber
  reads) is unaffected and continues to model latency normally via `block_on_fiber` — by design, this is
  correct and is NOT part of this risk (see `sd.rs:826-830`'s doc comment).

**Verification performed:** `cargo build --bin lens1-vt-sim` (debug) in `src/bsp/rust/lens1_vt_sim`;
runs via the built binary directly (`./target/debug/lens1-vt-sim`) with `DELUGE_SD_IMAGE` pointed at a
throwaway 16 MiB FAT16 image (`mkfs.vfat -F16`, created in the session scratchpad, not committed) and
`SPIKE_EFATFS_MOUNT=1` (+ `SPIKE_OFF_FIBER_INSTANT=0`/`1` for the negative/positive control), each under
`timeout 12`. All spike code (temporary `spike_efatfs.rs`, the `main.rs`/`Cargo.toml` hooks, and the
`Cargo.lock` deps it pulled in) has been reverted; only this appendix and the throwaway FAT image (outside
the repo, in the session scratchpad) remain.

**Status: no DECISION_NEEDED.** The risk is real (reproduced) but the documented workaround extends
cleanly with no new mechanism — §4.2's plan stands unchanged.
