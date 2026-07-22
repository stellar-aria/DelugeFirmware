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
