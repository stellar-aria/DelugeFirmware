# SD / file-I/O migration roadmap — from here to the Rust-native FS

**Status:** approved roadmap (strategy/decomposition). Each rung below gets its own spec → plan →
implementation cycle; this doc is the sequence, the gates, and the dependencies.
**Base:** `feat/rustfs-sp1b-cached-chain` (B6/B7 software-complete; next+4460 merged).
**North-star:** `docs/superpowers/specs/2026-07-20-async-storage-end-architecture-design.md` §5 (the
SP0–SP6 ladder) and `[[async-storage-end-architecture]]`. This roadmap is that ladder re-planned from the
current mid-point, with the verification model settled.

> **⟶ Superseding direction (Kate, 2026-07-23): the region boundary.**
> `docs/superpowers/specs/2026-07-23-region-boundary-streaming-relocation-design.md` raises the app-facing
> boundary **above sample streaming** and relocates the *act of streaming* (clusters, prefetch,
> cache/eviction, async SD, FAT chain-cache, `convert`/`stitch`) **down into the Rust platform layer**
> behind a high-level region port (`deluge_sample_source_*`). This **supersedes the streaming half** of
> the rungs below: the sector-vs-handle dual C-ABI, the `deluge_streaming_efatfs_active()` selector, and
> `StreamedChunk`-as-an-app-type all become Rust-internal. Consequently the remaining rungs **shrink to
> the task-context path**: R3.5 (host passthrough) and R4 (delete C-FatFS) become plain path/handle work
> with no streaming C-ABI to mirror, and R5 (fiber retirement) keeps only the task-context reason. The
> region arc has its own SR1→SR3 ladder; execute it (or interleave) ahead of the task-context residual.
> The rung descriptions below stand as the task-context plan; read them through this lens.

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

### R0 — Harness enablement — ✅ DONE (2026-07-22)

**Outcome:** R0a (byte-exact streaming differential) + Lens 2 (preemptive-TSan, 0 new races) gate the efatfs read path on host via an isolated shim reusing `efatfs_core`; device build byte-unchanged. **Margin = COMPOSED PROXY** (Lens 1 C-FatFS baseline + fs_differential efatfs overhead 1.03×/1.15× + on-device SP1 parity 3.83 vs 3.84 MB/s) — Lens 1's virtual clock mismodels efatfs single-sector reads (~64× more modeled reads → deterministic wedge; a HARNESS limitation, device showed parity). Fix b40fef579 (shim awaits locked_read_sectors, mirrors device) killed the original sync-block_on deadlock but exposed the deeper single-sector mismatch; chasing it further judged not worth it vs proxy+device parity (Kate). Lens-1-efatfs stays opt-in.

### R0 (original design) — Harness enablement
- Enable efatfs in `host_app` (it links the Rust BSP; `efatfs_fs.rs` is present — turn on the
  `efatfs_streaming` path in the host_app build).
- Point **Lens 1** (margin) and **Lens 2** (TSan) at the efatfs read path.
- Extend `fs_differential` with the **streaming access-pattern differential** (cluster-aligned reads in
  playback order incl. loop points, efatfs vs C FatFS).
- **Exit:** every subsequent efatfs rung can be gated on host — byte-exact + no-underrun + no-new-races —
  without hardware.
- **No device gate of its own** (it's harness infrastructure). Independent of the B6/B7 device gate.

### R1 — efatfs read-path migration (finishes SP-stream-read) — ✅ DONE (2026-07-22)

**Outcome:** streaming sample read is now efatfs-only (sync `read_cluster_data` via new `deluge_efatfs_read_at` FFI + `EfatfsReadSource`; async `ProdOps` efatfs arm the only arm). C-FatFS streaming map (a) — `resolve_read_layout`/`StreamImpl::layout`/`deluge_stream_read_at`/`Stream::read_at`/`StreamReadSource`/`read_stream_`/the seeding loop — **deleted**; selector is `efatfs_handle_ != 0`; `efatfs_streaming` default-**on**; descriptor `.sector` removed (FFI layout guards updated lockstep). R1/R3 boundary held: `sdAddress`/`sd_address_at`/`BlockReadSource`/recorder/`Stream::write_at`+`sector_of` preserved (`sector_of` rewritten to walk the FAT on demand). Regression fix: `cardReinserted()` tolerates a missing (0) sdAddress baseline for efatfs samples.

**KEY BASIS (from the final whole-branch review):** the full C-FatFS deletion is valid **only because the legacy C/C++ RZA1 BSP is now committed for retirement** (Kate, 2026-07-22; see `[[legacy-bsp-retirement-committed]]`) — efatfs is Rust-BSP-only (`deluge-bsp-rust`), and `dbt build Debug` links the RZA1 BSP which has **no** efatfs. So on RZA1 the streaming read is **retired, not migrated**: `deluge_efatfs_open` is the weak no-op → `open_read_stream` fails → streamed samples don't load. This is accepted (RZA1 is being retired); it still LINKS (silent runtime break, not a compile break). Full legacy-BSP retirement is a separate, later, hardware-gated milestone. The verification target for R1 is the **Rust BSP** host harnesses, not RZA1 runtime.

**Two final-review findings, both resolved:** (C1) the RZA1 breakage above — resolved by the BSP-retirement commitment (deletion kept). (C2) efatfs last-cluster sector-rounded read extended past logical EOF and failed (`efatfs_core::fill` returned false on `Ok(0)`) → **fixed** to zero-pad the tail past EOF and return success (matches the retired raw-sector read's tolerance; the past-`audioDataEnd` bytes are unused cluster padding), commit `bfd24e5143`, with a fail→pass over-EOF differential test.

**Gates (Rust BSP / host):** fs_differential **10/10** byte-exact (incl. the new over-EOF test) + Lens 1 proxy (1.03×/1.15×, R0 band) + Rust-BSP/host + `dbt build Debug` link + Lens 2 `scenario=PASSED open_findings=0`; UNCATALOGUED TSan findings full-stack-attributed **0/21 to R1's read surface** (pre-existing TLSF-allocator + playback-transport classes), cataloged with provenance. Base `b40fef579` → head `7e68c8632` (T1 `f0fdb2ddc` · T2 `b5c89191f` · T3 `facd74755`+`818a34192` · T4 `865966036`+`99bdf8a0a` · catalog `5ba74a630` · C2 `bfd24e5143` · comments `7e68c8632`).

### R1 (original design) — efatfs read-path migration
- Route `read_cluster_data` through efatfs (`block_on_fiber` — safe now that the loads are on-worker,
  B6). Retire `resolve_read_layout` + the `sdAddress` map (the "nothing above the port knows about
  sectors/clusters" exit criterion). Flip `efatfs_streaming` default-on.
- **Gate:** `fs_differential` + R0's streaming differential + Lens 1 (margin no-regression vs the SP1a
  1.03x baseline) + Lens 2 (no new races).
- **Depends on:** R0 (for the gate). Does **not** need the B6/B7 device gate to *build* — but the default
  flip should not ship to hardware until B6/B7 are device-confirmed (they underpin it).

### R2 — SP-fileio: re-back `file_io.h` onto efatfs — ✅ DONE (2026-07-22)

**Outcome:** the `deluge::io::File`/`Directory` port routes task-context file I/O + directory
enumeration to efatfs when active (runtime **selector, not replace** — C-FatFS `file_io.cpp` kept,
deleted at R4). New `efatfs_core` primitives (write/read_exact-EOF-honest/size/truncate/create/unlink/
rename/mkdir/set_time/readdir); 16 file/dir C-ABI mirrored device+host, `block_on_fiber`-bridged;
`File::read`→EOF-honest `read_exact`. The **browser + `fileExists` migrated onto the port**, files
identified by **PATH** (the UI-held locator was rejected mid-flight — `[[ui-file-identity-paths]]`; its
machinery was removed). **Coherency (§3.4) intentionally NOT built** — R2 is internally coherent; the
only cross-FS gap is the recorder, which R3 dissolves. Full whole-branch review: Ready-to-merge, 0
Critical/Important. **Gates:** fs_differential 17/17 (write/path-ops/readdir set-equivalence vs C-FatFS)
+ whole host suite + Lens 2 `scenario=PASSED UNCATALOGUED=0` (no new races) + `deluge_app` staticlib
links. efatfs is now the live default **save** path on the Rust BSP (covered by `closeAfterWriting`'s
reopen-and-verify). Device runtime confirmation on the Rust BSP owed (browser render, save/load on real
SD). Base `1896f1bbc` → head `87cefd1aa`.

### R2 (original design)
- Flip task-context byte-I/O + directory enumeration from C FatFS to efatfs, under the port boundary.
  C++ above the port unchanged. The fiber persists as the blocking context (retired later, at R4).
- **Gate:** `fs_differential` + Lens 2 (task-context ops under preemptive audio).
- **Depends on:** R1 (streaming read already off C FatFS).

### R3 — SP-recorder: recorder onto efatfs — ✅ DONE (2026-07-23), migrate-only

**Outcome:** the sample recorder — the last C-FatFS consumer — is fully on efatfs (on the efatfs path).
Its write path holds a **persistent efatfs write context** across a recording (write-many-no-flush,
flush-at-finalize; crate `File::detach()` + name-bytes reattach for dirty contexts); the finalize
header-patch and `alterFile` in-place rewrites became efatfs **positional writes**; the mid-write
read-back (`RecordingReadSource`) reads through the recorder's own write context (in-memory size = the
written extent while the on-disk dir entry is stale). This **retired `sdAddress`/`sd_address_at`/
`sector_of`/`BlockReadSource` + the `fileSystem.database/csize` geometry coupling** and **closed the R2
dual-FS window** (everything is now one efatfs mount). Full whole-branch review: Ready-to-merge; one
Important corruption window (abort-path deferred flush) found + fixed.

**Scope correction (Kate, 2026-07-23):** the roadmap's "contiguous-preallocation fast path" was obsolete
(the recorder never used `f_expand`); "power-loss safety + fault-injection harness" is a NEW capability
the recorder never had (no periodic sync today) — both **deferred to their own rungs**. R3 is a
behavior-preserving **migrate-only** rung. **Gate:** `fs_differential` 21/21 + whole host suite + Lens 2
5/5 `scenario=PASSED recorder_writes>0 UNCATALOGUED=0` + `deluge_app` staticlib links. No power-loss
harness (deferred). Base `87cefd1aa` → head `ec300d4c9`.

**Deferred to R4 / follow-up:** C-FatFS `deluge_stream_*/file_*/dir_*` backings (dead on Rust BSP, R4
deletes); the recorder abort `f_unlink`/`invalidate_cache` (still C-FatFS, R4); the pre-existing latent
SD-card-reinsert "samples unloadable under efatfs" bug (needs an efatfs read-mode sector-resolution
mechanism); contiguous preallocation + power-loss safety (own rungs).

- (original) Contiguous-preallocation write fast path; async recorder-writeback; **power-loss safety**.
- **Gate:** `fs_differential` + Lens 2 + the **power-loss fault-injection harness** (new; deferred — R3 is migrate-only).
- **Depends on:** R2.

### R4 — SP-delete: delete C FatFS, retire the fiber
> Reframed by the region boundary (see the banner at the top): the streaming C-ABI it once had to handle
> is gone (Rust-internal behind the region port), so R4 is now **pure task-context** deletion. Reshaped by
> `2026-07-23-r4-delete-cfatfs-design.md`.
> **Re-scoped 2026-08-04:** after U4b retired the C-host streaming targets and stood up a partial POSIX
> passthrough, **R3.5 is folded into R4 as Phase 0** (no longer a standalone rung). Base is now the merge of
> `feat/region-port-sr1`; R4 runs **after** that merge (Kate's sequencing). See R4 §0.
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
- **Note-on deadline elimination (deferred into the region arc)** — research
  `docs/superpowers/specs/2026-07-24-deadline-elimination-research.md`; decision recorded in the
  region-boundary design §4.1. The I/O half (attack-cluster residency / "warm hint") is a residency
  optimization that belongs **below the port in Rust at SR2**, not built in C++ now — attack-pinning
  already exists in C++ (`clustersForStart`) and moves with residency. Above-the-port / orthogonal pieces
  (turn the silent note-on voice-DROP into the graceful defer; cost-aware voice admission + bounding the
  periodic time-stretch `hopEnd` spike) are tracked, low-urgency, schedule on measured evidence.

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
