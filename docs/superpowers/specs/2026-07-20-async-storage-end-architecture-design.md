# Async storage & SD loading — end-state architecture

**Status:** north-star architecture design. Integrating end-state for the *whole* SD/file-I/O stack,
plus a sequenced decomposition into shippable sub-projects. Nothing new implemented by this doc — it
defines the target the in-flight work converges on. NEEDS-HARDWARE gates called out per sub-project.

**Base branch:** the frontier lives on `feat/async-sd-owner-substrate` (all 5 ladder rungs
software-complete; the Rust async streaming-loader beachhead merged + default-on, `e73335eaf`). This
design assumes that branch (or a successor off `next`) as the starting point.

**Committed constraint (Kate, 2026-07-20):** the **Embassy/Rust backend is the only backend we
keep**. The dual-backing tax — the fiber `pump()` fallback maintained for other BSPs and the
host-cooperative build — is deliberately dropped. That single decision is what turns "relocate the
fiber" into "delete the fiber," and what makes a full Rust-native filesystem the end-state rather than
a permanent C-FatFS island.

**Committed decisions captured here:**
- **Filesystem ownership:** full **Rust-native filesystem**. C FatFS (`ff.c` / chan-fs) is deleted.
- **FS scope:** **FAT16/32 + LFN only. No exFAT.** This is what makes a Rust-native FS tractable.
- **FS implementation:** **adopt + harden `embedded-fatfs`** (fork + pin), not bespoke, not as-is.
- **SRAM-residency perf work:** a **named parallel track**, meeting the async stack only at the
  resource-manager backing-selection seam. Architecturally separate; see §6.
- **Flash settings (`flash_storage`):** **out of scope** — SPI/NVM flash, not the SD FAT volume.

---

## 1. The end-state in one picture — two domains, one Rust storage service

The whole stack collapses into **two execution domains** with a lock-free boundary between them.

```
┌──────────────────── AUDIO DOMAIN (preemptive, real-time, NEVER blocks) ────────────────────┐
│  Voice/render reads RAM chunks only · publishes play-cursors + prefetch reqs (lock-free)    │
│  consumes readiness flags · ZERO FatFS, ZERO blocking I/O                                    │
└───────────────────────────────────────┬─────────────────────────────────────────────────────┘
                  Published<T> / SpscRing  ·  resource-manager readiness (B1-synchronized)
┌───────────────────────────────────────┴─────────────────────────────────────────────────────┐
│                         STORAGE DOMAIN  (Rust Embassy async tasks)                             │
│  ┌─ streaming-fill task ─┐  ┌─ recorder-writeback task ─┐  ┌─ task-context file-ops ─┐        │
│  │  drains loader queue  │  │  drains filled-cluster ring│  │  load/save/browse/enum  │        │
│  └───────────┬───────────┘  └────────────┬──────────────┘  └────────────┬────────────┘        │
│              └─────────────── Rust FS (embedded-fatfs fork, FAT16/32+LFN, async) ───┘          │
│                    resource mgr / loader / alloc (deluge_resource, deluge_alloc)               │
│                    async block device (sd.rs, IRQ-woken, peripheral async-mutex)              │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
                       ▲ C ABI seam: file_io.h / stream_io.h / streaming_fill.h / deluge_resource.h
┌──────────────────────┴─────────────────────────────────────────────────────────────────────────┐
│  C++ APP — synchronous load/save/browse code runs on a plain BLOCKING worker context             │
│  (blocking on Rust storage completions is fine — it is OFF the audio path)                        │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### The load-bearing claims

1. **The worker fiber is fully retired.** Its sole reason to exist is to let *synchronous* C++ FatFS
   calls cooperatively yield without deadlocking the non-reentrant FatFS. Once Rust owns the FS
   async-natively there is no C FatFS to protect and nothing to yield around. C++ becomes a *client*
   of the Rust storage service.

2. **Blocking replaces cooperative pumping — and that is the win.** Deleting the fiber (not merely
   relocating it) is possible because **preemptive audio** is in place: audio runs on the cortex-ar
   `InterruptExecutor` and preempts everything else. So the C++ task-context storage code can simply
   **block** on a Rust completion — no `pump()`, no `allowSomeUserActions`, no cooperative glue. This
   end-state therefore *depends on* and *finishes* the preemptive-audio and cooperative-glue-retirement
   efforts (see §5, precondition P1).

3. **C FatFS is deleted entirely.** The Rust `embedded-fatfs` fork owns mount, path resolution,
   directory enumeration, create/extend/delete, FAT-chain management, and LFN. FAT16/32 only.

4. **The C++ code above `file_io.h` / `stream_io.h` does not change.** Those port boundaries already
   exist (the file-io and stream-io migrations are complete). The Rust FS plugs in *underneath* them —
   the `deluge::io::File` / `Stream` implementations get re-backed from C FatFS to the Rust FS over the
   C ABI. The Serializer / XML / preset machinery is untouched.

5. **The cluster→sector-map trick retires.** The shipped beachhead used a C++-provided
   `cluster_index → sector` map to keep FatFS C-side and open-time-only. Once the FS is Rust-native the
   loader just opens the file and uses a **sequential-read fast path** (the open file's FAT chain is
   cached, so a streaming read is a chain-indexed sector read with no per-cluster FAT walk) — the same
   cost as raw sectors, but with real FS semantics.

---

## 2. Where we are starting from (the frontier)

- **Incremental ladder** (`feat/async-sd-owner-substrate`): the worker fiber *is* the single storage
  owner; the `block_on_fiber` yield-flip landed; two-level priority owner queue + cooperative recorder
  yield. Software-complete; only the hardware ear-check remains.
- **Rust async streaming loader** (merged, default-on): the streaming cluster-fill *drain* runs on a
  Rust Embassy async task that `await`s raw sector reads via the C++-provided cluster→sector map and
  FFI-calls the existing C++ convert/stitch. It deliberately **dodged** the FatFS question (FatFS stays
  C-side, open-time-only). This design *answers* that question.
- **Recorder / preview / browser** still run on the C++ fiber.
- **Port boundaries already in place:** `file_io.h`, `stream_io.h`, `block_device.h`,
  `streaming_fill.h`, `deluge_resource.h`. The Rust FS plugs in beneath the first two.

The migration in §5 is the bridge from this frontier to §1.

---

## 3. The two execution domains, precisely

**Audio domain** — preemptive, real-time, never blocks on I/O. Render reads resource-manager RAM
chunks, publishes play-cursors and prefetch requests through the lock-free spine
(`Published<T>` / `SpscRing`), and consumes readiness flags (B1-synchronized in the resource manager).
It touches no FatFS and never blocks.

**Storage domain** — Rust Embassy async tasks own all SD I/O: the streaming-fill task, the
recorder-writeback task, and the task-context file-ops surface. Beneath them sits the Rust FS, the
resource manager / loader / allocator, and the async block device. The C++ synchronous load/save/browse
code runs on a **blocking worker context** that blocks on Rust storage completions — safe precisely
because it is off the audio path (claim 2).

The boundary between the domains is lock-free in both directions: audio→storage via published cursors /
prefetch requests, storage→audio via chunk-readiness. No lock is ever held across the domain boundary.

---

## 4. Per-path end-states

### 4.1 Streaming loader (audio-context) — closest to done
Render never touches SD; it reads resource-manager RAM chunks and publishes play-cursors. The
streaming-fill task (already live) drains the loader queue and, in the end-state, reads through the
**Rust FS sequential fast path** instead of the C++-supplied cluster→sector map. Convert/stitch run
**once per cluster fill** (not per sample), so their C↔Rust FFI cost is negligible; they stay pure C++
over byte spans via FFI by default, with an **optional** Rust port (§5, SP6) as the "last mile to zero
FFI." Prefetch depth/policy is the other open sub-choice (§5, SP5). SP5 + SP6 together make the
streaming-fill task 100% Rust — both optional, neither on the critical path.

### 4.2 Recorder (audio-context) — read-back + write/finalize
The audio path fills cluster buffers into a lock-free ring; a **recorder-writeback task** drains the
ring and writes via the Rust FS. This is why the fork needs a **contiguous-preallocation fast path**:
the recorder preallocates a contiguous run at record-start and streams raw cluster writes into it, with
FAT / directory-entry metadata flushed only at boundaries and at finalize — matching today's behavior,
not per-cluster metadata churn. **Power-loss safety is the headline hardening requirement here**: a
recording killed mid-stream must not corrupt the card and should leave a recoverable file. Read-back
(`BlockReadSource`) becomes ordinary Rust-FS reads.

### 4.3 Task-context: preset/song load-save, browser enumeration, sample/DX browse
These flip *underneath* the existing `file_io.h` / `stream_io.h` port boundary — the
`deluge::io::File` / `Stream` implementations are re-backed from C FatFS to the Rust FS over the C ABI.
The Serializer / XML / preset code above the boundary is untouched. Execution: the synchronous C++
load/save code runs on the **blocking worker context** and blocks on Rust-FS completions. Directory
enumeration for the browser becomes a Rust-FS `read_dir` async call surfaced through the same boundary.
Largest *surface*, lowest *architectural* risk — the seam already exists and the behavior is
latency-tolerant.

### 4.4 Flash settings (`flash_storage`) — out of scope
Device settings live in **SPI / NVM flash, not the SD FAT volume** — a different medium that does not
ride the Rust FS. Cross-referenced only.

---

## 5. Migration decomposition

### Preconditions (in flight, not owned by this plan)
- **P1 — Preemptive audio integrated + hardware-proven.** The blocking worker context (§1 claim 2) is
  only safe once audio preempts. Code-complete on the Embassy scheduler migration; **awaiting its
  hardware gate.** SP-delete cannot land until P1 is real on-device.
- **P2 — Allocator SRAM second-pool.** Needed only by the SRAM-residency track (§6), not by the async
  spine. Gated/deferred per the allocator redesign.

### Sub-projects (each golden-bit-exact and independently shippable)

**SP0 — Rust-FS spike + differential harness.** *De-risks everything; do first.*
Fork / pin `embedded-fatfs`, mount real Deluge card images in the host Embassy harness. Build the
**differential test harness**: identical op sequences (open / read / enumerate / create / extend /
write / delete) driven through C FatFS and the Rust FS on the same card image, asserting byte-for-byte
data + metadata equivalence, across LFN and both FAT16 *and* FAT32 volumes. Benchmark sequential-read
and contiguous-write throughput vs FatFS **on-device**.
*Exit gate:* differential-clean + throughput parity. If it fails, the "adopt embedded-fatfs" decision
reopens before any integration cost is sunk.

**SP-bridge — Device block-device bridge; async block device foundation.** Stand up the async block
device abstraction (`sd.rs`, IRQ-woken, peripheral async-mutex) in the BSP, mounting real Deluge card
images over the Rust FS in the host Embassy harness. This is foundational work — no live audio path
impact, purely a validation rung. Unblocks SP-stream-read and provides the I/O boundary for all FS
validation work.

**SP-stream-read — Rust FS behind the block seam; streaming read path.** Stand up the FS in the BSP over `sd.rs`;
route the streaming-fill task through its sequential fast path, **retiring the cluster→sector-map
trick**. Audio-only surface. Golden bit-exact + Lens-1/Lens-2 streaming harnesses.

**SP-fileio — Re-back `file_io.h` / `stream_io.h` onto the Rust FS.** Flip task-context byte-I/O + directory
enumeration from C FatFS to Rust FS *under the port boundary*. C++ above unchanged. **The fiber still
exists during this SP** as the blocking context — flip the *backing* here, retire the fiber later
(SP-delete), keeping the two changes separate. Largest surface, lowest architectural risk.

**SP-recorder — Recorder read + write/finalize onto the Rust FS.** Contiguous-preallocation write fast path;
async recorder-writeback task; **power-loss-safety validation** (pull-the-plug matrix on-device,
fault-injected in the harness).

**SP-delete — Delete C FatFS + retire the fiber.** Once nothing calls `ff.c`: delete chan-fs, retire the
fiber, move task-context storage onto the plain blocking worker context, sweep the last cooperative
glue. **Requires P1.** This is the moment the two-domain end-state (§1) becomes real.

**SP5 — Position-driven prefetch (optional north-star tail).** Move streaming prefetch policy fully
into the Rust loader task (derive prefetch from published play-cursors; retire the C++ enqueue API).
**YAGNI checkpoint:** if enqueue-driven prefetch measures fine after SP-stream-read, SP5 may be *deliberately not
done* — the architecture is already "there" without it. Optional endpoint, not mandatory.

**SP6 — Port convert/stitch to Rust (optional, last mile to zero FFI).** Reimplement
`convert_cluster_data` / `stitch_boundaries` in Rust so the streaming-fill task carries **no FFI at
all**. *Not a performance change* — these run once per cluster fill, so the FFI cost is already noise;
the payoff is a fully-native fill task (pairs with SP5). **The cost is the gate:** these are SIMD DSP,
and the golden harness is bit-exact, so the port must reproduce exact ARM-NEON *and* x86 results.
Mandatory **dual-arch bit-exact spec** (the dsp-next / golden-masters methodology the project already
uses) before the FFI path is retired. Pure downside if it ever drifts a fixture — hence deferred and
optional. Do it only when the core migration (SP0–SP-delete) is proven.

### Critical-path ordering
`SP0 → SP-bridge → SP-stream-read → SP-fileio → SP-recorder → SP-delete`. `SP5` and `SP6` are **both optional** after SP-stream-read/SP-delete respectively and
together make the fill task 100% Rust. `SP-R` (§6) free-floats after P2. SP-delete additionally waits on P1.
If P1 slips, SP0–SP-recorder still land (the fiber persists as the blocking context); only fiber-deletion waits
— the plan degrades gracefully.

**Note on file-naming drift:** On-disk plan and spec files were created with the original SP0–SP4 numbering
before the device-bridge rung was inserted. The filename-to-rung mapping is: files labeled "sp1-device-bridge"
correspond to **SP-bridge**; "sp1a-streaming-read" and "sp1b-streaming-read" files correspond to
**SP-stream-read**; filenames "sp2-*" map to **SP-fileio**; "sp3-*" to **SP-recorder**; "sp4-*" to
**SP-delete**. This numbering drift reflects the order in which rungs were actually built.

---

## 6. Parallel track — SRAM residency (SP-R)

Diagnosis (`docs/dev/sram_residency_streaming_perf.md`): the real streaming-perf ceiling is the
**single 16-bit SDRAM bus**, a hardware/placement concern orthogonal to async-ness. Concurrent
scattered read cursors are close to the SDRAM's worst-case (row/bank thrashing); moving hot data to the
multi-ported, refresh-free on-chip SRAM is the fix.

The async architecture exposes exactly one seam this plugs into: the **resource-manager per-asset
backing selection** (`DELUGE_RESOURCE_BACKING_SLAB` today; end-state adds an SRAM slab/pool). Tier-1
whole-small-samples and Tier-2 active-voice heads select the SRAM backing; streamed tails stay SDRAM.
The async loader is **tier-unaware** — it fills whatever buffer the manager hands it. Residency is a
placement decision made at asset-registration / voice-activity time, entirely separate from fill
scheduling.

Dependency: the allocator SRAM second-pool (P2). Can run alongside SP1–SP5.

---

## 7. Verification & risks

### Verification spine (reused at every SP)
- **Golden bit-exact** audio equivalence at every commit — the non-negotiable gate.
- **Differential FS harness (SP0)** — C FatFS vs Rust FS on identical card images; the go/no-go
  evidence for the entire adoption.
- **Host-native Embassy harness + Lens-1/Lens-2** — deterministic streaming-margin curve (Lens 1) and
  preemptive-race TSan (Lens 2); proves no-underrun and race-freedom off-hardware.
- **Fault-injection** — power-loss matrix for SP3, driven in-harness on the block device and confirmed
  on-device.
- **Hardware ear-checks** at SP boundaries (Kate's gate): no-underrun-while-recording, UI/MIDI
  responsiveness, bounded read latency.

### Top risks (ranked)
1. **embedded-fatfs throughput / hardening shortfall** (highest). Mitigated by SP0-as-go/no-go *before*
   integration cost; contiguous-write and sequential-read fast paths are explicit fork requirements.
2. **Power-loss corruption on the write path** (SP3) — a class C FatFS handled implicitly. Explicit
   fault-injection gate; contiguous-preallocation limits the metadata-inconsistency window.
3. **SP4 depends on P1** (preemptive audio's hardware gate). Degrades gracefully — SP0–SP3 land without
   it.
4. **LFN / FAT16 corner cases vs user cards in the wild** — caught by the differential harness across
   both FAT variants, but real-card variety is the long tail; on-device soak recommended.
5. **Convert/stitch bit-exactness** — kept as a C++-FFI call by default (once-per-cluster, negligible
   cost); the optional SP6 Rust port must reproduce exact ARM-NEON + x86 SIMD results or it drifts a
   golden fixture. Deferred and gated on a dual-arch bit-exact spec for exactly this reason.

---

## 8. Relationship to the broader north star

This is the storage-stack instance of the target-architecture thesis (`docs/dev/target_architecture.md`):
decompose the monolith into components behind stable C-ABI seams, with **Rust owning HAL + BSP** and the
C++ app as a client. It completes async-SD design §8 ("Rust async filesystem below the seam"), finishes
the cooperative-glue retirement, and consumes the preemptive-audio migration. The synth stays C++
(CLAP at L3); storage goes fully Rust below the `file_io.h` / `stream_io.h` seam.
