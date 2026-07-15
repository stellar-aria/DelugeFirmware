# Audio Stream Module — Design

**Date:** 2026-07-15
**Status:** Approved (design); implementation not started
**Scope:** Extract the SD-card sample streaming / chunk data path into a self-contained,
de-overloaded module. De-overload the `Cluster` class. Express the loader on the existing
`stream_io.h` and `deluge_resource` seams. **Not** the AudioFile identity/library layer, and
**not** a replacement of the residency/eviction engine (already done — see below).

---

## 1. Context & what already exists

The Deluge streams SD-card samples in FAT-cluster-sized chunks (one cluster = one physically
contiguous region on the card = one DMA transfer). That physical model is correct and performant
and **is not changing.** What is legacy is the *shape* of the code expressing it.

Critically, the residency/eviction/loader-queue **mechanism** was already migrated to a Rust crate
weeks ago and is done (NEEDS-HARDWARE only):

- `crates/deluge_resource/` — a bounded cache of reconstructable *chunks* with leases, soft-refs,
  cost/size-aware eviction, an async load ABI, and a loader queue. It **replaced** `CacheManager` /
  `Stealable` / `numReasonsToBeLoaded` / `getAppropriateQueue` (all deleted). C ABI in
  `include/libdeluge/deluge_resource.h`, ~25 `deluge_resource_*` entry points.
- `include/libdeluge/stream_io.h` + `deluge::io::Stream` (`src/deluge/io/stream.{hpp,cpp}`) — a
  byte-offset-addressed RAII stream over an opaque handle (`open`/`read_at`/`write_at`/`truncate`/
  `size`/`sector_of`/`close`), with one shared FatFS-family backend (`src/fatfs/stream_io.cpp`).
  Raw sector/DMA reads happen there, below the app.

So this subsystem is **already split across a C ABI**: `deluge_resource` (Rust) owns residency; the
C++ side owns only *reconstruction* (`readClusterData`: read → convert → stitch) and *dispatch*
(`getCluster`'s load-instruction switch); and the Rust manager already calls back *into* the C++
reconstruction through a defined callback seam (`clusterMaterialize`/`construct`/`evict`).

### What's still "the cluster-caching model"

- **`Cluster`** (`src/deluge/storage/cluster/cluster.{h,cpp}`) is overloaded three ways: streamed
  sample data (`SAMPLE`), the repitch cache (`SAMPLE_CACHE`), and the perc/time-stretch analysis
  cache (`PERC_CACHE_FORWARDS`/`_REVERSED`). Its own header comment calls this "largely legacy";
  the original justification (uniform RAM size so "stealing" always frees the right amount) is now
  dead — the resource-manager slab already gives uniform backing and `Stealable` is gone.
- The streaming orchestration is smeared across `Sample`, `SampleCluster`, and the
  `AudioFileManager` god-object (which also owns identity/library).

---

## 2. Goal & non-negotiables

**Goal:** a self-contained C++23 streaming module with the three payloads de-overloaded, the loader
expressed on the existing seams, and the orchestration pulled out of `AudioFileManager` — shaped so
that the *far* end-goal (the whole audio-file management/streaming/caching subsystem living in Rust
behind a "give me this file" API) becomes a mechanical port rather than a redesign. Bit-exact goldens
are **not** the gate; correctness is ear-check + hardware, leaving room for caching/performance
improvement.

**Language / boundary decision (settled):** idiomatic C++23, in-tree. Reuse the *existing* C ABIs as
the module's seams — `stream_io.h` below, the `deluge_resource` Source-callback + resident-chunk-
pointer contract above. **Do not** hand-roll a new C ABI in the middle. Rationale: a C ABI is a
commitment that must sit on stable ground; the durable seams are the bottom (`stream_io.h`) and the
top-ish (`deluge_resource`), while the middle (the de-overloaded cache types, `getCluster` dispatch,
and above all the RT reader that reads `clusters[0]->data` directly per-sample) is still churning and
must not be crossed by a call boundary. Writing the reconstruction + dispatch as *pure, POD-in/out*
units keeps the eventual Rust reimplementation a mechanical swap behind the callback seam that
already exists.

**Non-negotiables carried into every step:**
- C++23 in-tree; seamed on `stream_io.h` + `deluge_resource`; no new C ABI.
- Reconstruction + dispatch as pure, dependency-light, POD-shaped units (the Rust-port target).
- FAT-cluster-sized, DMA-aligned transfers preserved (physical model unchanged).
- RT reader keeps reading resident bytes by pointer; the render thread never does I/O.

---

## 3. Approach (chosen: "Two concepts, one seam")

The real cleavage is **file-backed** vs. **computed-in-RAM**, not three peers. `Cluster` splits into
two chunk types; the two caches share one type.

Alternatives considered and rejected: (B) three sibling chunk types + a thin loader — more faithful
but keeps repitch/perc duplicated and yields a messier Rust boundary; (C) a C++ `ChunkSource`
abstraction + generic streaming service — double-abstraction, because `deluge_resource` *already is*
the Source registry (materialize/construct/evict/cost), and it leans on virtuals right where the core
must stay pure.

---

## 4. Module shape & boundaries

New module `deluge::audio::stream`, home `src/deluge/storage/audio/stream/`. Pieces:

- **`StreamedChunk`, `ComputedChunk`** — the two de-overloaded chunk types (§5).
- **`reconstruct` core** — the pure read → convert → stitch unit (§6). The Rust-port target.
- **`SampleStream`** — per-`Sample` object owning that Sample's residency table + open
  `deluge::io::Stream` + `getCluster` dispatch (§7). Replaces the `fast_vector<SampleCluster>` +
  stream plumbing that lives directly on `Sample` today.
- **`loader`** — the module-level queue pump (today's `AudioFileManager::loadAnyEnqueuedClusters` +
  the `disk_read`/`disk_write` drain hooks), driving reconstruction across all streams.

**What stays put:**
- `AudioFileManager` keeps identity/library only: `getAudioFileFromFilename`, dedup, recording-path
  allocation, alt-dir search, `adoptAudioFileObject`/`destroyAudioFileObject`, the `audioFiles` table.
- `SampleHolder`'s "first two clusters + loop-start permanently resident" leasing
  (`claimClusterReasons`/`claimClusterReasonsForMarker`, `kNumClustersLoadedAhead = 2`) stays — it
  just leases through `SampleStream` instead of reaching into `Sample::clusters`.
- The RT reader (`SampleLowLevelReader::changeClusterIfNecessary`/`moveOnToNextCluster`,
  `VoiceSample`) is structurally untouched — still reads resident bytes by pointer, still enqueues
  the next-next cluster on boundary crossing.

---

## 5. The two chunk types

**`StreamedChunk`** (was `Cluster::Type::SAMPLE`). Carries only streamed-relevant state:
`resourceSlot`, `clusterIndex`, the data view, `loaded`, the boundary-conversion trio
(`extraBytesAtStartConverted`, `extraBytesAtEndConverted`, `firstThreeBytesPreDataConversion`),
`unloadable`, `numReasonsHeldBySampleRecorder`. The SAMPLE-only fields that ride dead on every cache
cluster today are gone from the computed path.

**`ComputedChunk`** (was `SAMPLE_CACHE` **and** `PERC_CACHE_*`). Just `{resourceSlot, clusterIndex}`
+ data view. It serves both caches because their chunk *structs* are structurally identical; every
difference is expressed elsewhere:

| Difference | Repitch cache | Perc cache | Where it lives (not on the chunk) |
|---|---|---|---|
| eviction order | evict-tail-first | arbitrary (middle-of-list) | `deluge_resource` per-asset flag |
| leasing | unleased (resident-but-evictable from birth) | leased-while-nearby (TimeStretcher 2-slot LRU) | owner behavior |
| byte meaning | 3-byte PCM (`kCacheByteDepth`) | 1 byte / 128 samples | asset `ctx` |
| direction | — | per-direction (`reversed`) | asset `ctx` (Sample* + `reversed`) |

The chunk carries **no owner backpointer**: `on_evict` receives the asset `ctx` from the manager
(SampleCache\*, or Sample\* + `reversed`), which is enough to run `onCacheEvict` /
`percCacheClusterStolen`.

**Backing:** both types keep coming from the one uniform slab (slot = `max(header) + Cluster::size`),
so residency/eviction is unchanged and the "steal one → free the right amount" property holds. The
flexible-array-member trick (`data` declared small, real allocation appends `Cluster::size +
CACHE_LINE_SIZE`) is preserved. The dead `GENERAL_MEMORY` / `OTHER` enumerators are dropped.

---

## 6. The reconstruction core (the crux, the Rust-port target)

`readClusterData`'s body (`audio_file_manager.cpp:939-1240`) becomes a **pure function**, POD in/out,
with no reach into C++ object graphs:

- **In:** a read source (the `deluge::io::Stream` handle, or the raw-block fallback via
  `deluge_block_read` against `SampleCluster::sdAddress` for not-yet-streamed recordings); the byte
  offset + length for this cluster; the destination span; a small **format descriptor** POD
  (`rawDataFormat`, byte depth, channel count, `audioDataStartPosBytes`, total length); and — for the
  stitch — the **edge spans of the neighbor chunks** plus their current boundary flags.
- **Out:** converted bytes written into the destination, plus the **boundary-state deltas** (which
  `extraBytesAt*Converted` to set, `firstThreeBytesPreDataConversion` to stash).

**Why this is the delicate part.** For non-native formats a multi-byte sample frame straddles the FAT
boundary (e.g. 3-byte 24-bit frames don't divide 32768 evenly), so the last bytes of one cluster are
the first part of a frame whose remainder lives in the next cluster (not yet loaded when this one is
converted), and conversion is destructive/in-place. Today the stitch (audio_file_manager.cpp:
1048-1227) reaches `sample->clusters[index±1].cluster->data` directly and tracks
`extraBytesAt*Converted` / `firstThreeBytesPreDataConversion` to avoid double-converting a straddling
frame and to recover pre-conversion raw bytes for the wrong-endian-24 case.

To keep the core pure: **`SampleStream` gathers the neighbor edge spans and passes them in**; the core
does the byte math and returns the flag deltas; `SampleStream` applies them to the neighbors.
Native-format samples (the common case) skip the convert/stitch path entirely. This isolation is
exactly what makes the later Rust reimplementation a mechanical swap behind the existing
`clusterMaterialize` callback. The computed caches have **no** such coupling — they're written
forward-only at a moving cursor and never independently loaded out of order.

---

## 7. Orchestration & RT contract

**`getCluster` dispatch** moves onto `SampleStream`, behavior-preserving (today at
`sample_cluster.cpp:62-145`):
- `CLUSTER_DONT_LOAD` → `deluge_resource_request` (construct, no I/O) + `deluge_resource_mark_dirty`
  (pin unevictable until flushed) — the recording/convert target, non-reconstructable.
- `CLUSTER_ENQUEUE` → `deluge_resource_request` (no I/O) + `deluge_resource_loader_enqueue`. Returns
  immediately; caller may get an unloaded chunk.
- `CLUSTER_LOAD_IMMEDIATELY[_OR_ENQUEUE]` → `deluge_resource_acquire` (may materialize
  synchronously); force-read a prefetch-constructed hit via the reconstruction core +
  `deluge_resource_loader_remove`; on failure fall back to enqueue or null.

**The loader pump** keeps the current cooperative model (today `loadAnyEnqueuedClusters`,
`audio_file_manager.cpp:1275-1399`): drained from the `disk_read`/`disk_write` shims
(`audio_file_manager.cpp:64-83`) and audio-engine servicing, popping `deluge_resource_loader_next`,
running the reconstruction core, `deluge_resource_mark_ready`. **No background thread is introduced.**

**RT contract preserved exactly.** The render thread never does I/O: every RT `getCluster` is
`CLUSTER_ENQUEUE`, and not-ready → fail-fast to silence / late-start retry
(`VoiceSample::attemptLateSampleStart`), never block. `moveOnToNextCluster` still drops the trailing
cluster's lease, slides the 2-wide window, and enqueues the next-next cluster as the playhead crosses
a boundary.

**Recorder / latched-legacy edge.** Recording samples (`CLUSTER_DONT_LOAD`, non-reconstructable, raw-
block-read fallback, the separate `numReasonsHeldBySampleRecorder` hold layered on top of the manager
lease) are carried through faithfully on `StreamedChunk` as a first-class case, not an afterthought —
this path is behavior-sensitive.

---

## 8. Migration sequence (incremental, each independently gated)

1. **Module skeleton + extract the pure reconstruction core** out of `readClusterData`. Pure code-
   move — golden bit-exact expected.
2. **Split `ComputedChunk` out of `Cluster`**; repoint `SampleCache` + perc onto it; `Cluster` →
   `StreamedChunk` sheds cache concerns. Structural — golden bit-exact expected.
3. **Introduce `SampleStream`**; migrate the residency table + `getCluster` + stream handle off
   `Sample`/`SampleCluster` onto it; `SampleHolder` and the RT reader lease through it. Structural.
4. **Consolidate the `loader` pump** into the module; retire the `AudioFileManager` streaming methods
   (`loadCluster`, `readClusterData`, `loadAnyEnqueuedClusters`, `removeReasonFromCluster`,
   `loadingQueueHasAnyLowestPriorityElements`, the `clusterBeingLoaded` sentinel state).

Steps 1–3 should stay bit-exact (pure moves); step 4 and any conversion-path reshaping are ear-check
+ hardware gated. Each step builds firmware (`dbt build Debug`) + sim and runs the golden sweep.

---

## 9. Testing & verification

- **Golden-master sweep** (cordae / highsiderr / icoustic) at each step for the structural moves;
  full-heap bit-exact is the gate for steps 1–3.
- **Dual-arch CppSpec unit specs for the reconstruction core specifically** (x86 SIMDe + ARM/qemu) —
  format-conversion + boundary-stitch is pure logic that wants a dedicated regression net, and those
  specs *are* the Rust-port contract (the Rust impl must satisfy the same specs).
- **Real render + ear-check** as the true gate for the behavior-touching steps (step 4, conversion
  reshaping), per the "not bit-exact-gated" steer and the "real execution catches what review misses"
  lesson.
- **NEEDS-HARDWARE** before merge: recording, memory-pressure eviction, streaming-under-load,
  non-native-format samples.

---

## 10. Risks

- **Primary — the boundary-conversion stitch (§6).** The only genuinely-coupled logic in the class.
  Isolating it into POD-in/out is the delicate move; non-native-format fixtures are the ones to watch.
  `highsiderr`'s documented layout-sensitivity means a small PCM divergence there is ear-check-
  resolved, not necessarily bit-exact.
- **Recorder path** (§7) is behavior-sensitive; keep it first-class through the migration.
- **Slab-struct byte-stability.** Per prior experience, changing manager/chunk struct sizes shifts
  heap addresses and can flip goldens deterministically with zero eviction changes. Keep chunk
  headers byte-stable across steps, or expect (and re-baseline) spurious golden shifts.

---

## 11. Documented later step (explicitly out of this increment)

**Decouple `ComputedChunk` sizing from the FAT-derived `Cluster::size`.** The repitch and perc caches
have no FAT relationship, yet today they inherit whatever cluster size the *card* dictates (a single
mutable static `Cluster::size`, set at boot). Sizing computed chunks independently — leaning on the
resource manager's already-size-aware eviction — is a genuine memory/perf improvement. It changes
slab layout and carries its own golden/hardware risk, so it lands as a **separate step on top** of
this increment, not bundled into the core refactor. Recorded here as a named follow-on.

---

## Appendix — key current-code anchors

- `src/deluge/storage/cluster/cluster.{h,cpp}` — the overloaded `Cluster`.
- `src/deluge/model/sample/sample_cluster.{h,cpp}` — residency-table entry + `getCluster` dispatch.
- `src/deluge/model/sample/sample.{h,cpp}` — cluster vector, the resource-manager Asset
  definition/materialize/construct/evict callbacks (`sample.cpp:95-292`), `fillPercCache`
  (`385-865`), `percCacheClusterStolen` (`1036-1140`).
- `src/deluge/model/sample/sample_cache.{h,cpp}` — the repitch cache.
- `src/deluge/dsp/timestretch/time_stretcher.cpp` — perc lease-while-nearby (`~150-195`, `1106-1230`).
- `src/deluge/storage/audio/audio_file_manager.{h,cpp}` — `readClusterData` + boundary stitch
  (`939-1240`), `loadAnyEnqueuedClusters` (`1275-1399`), the `disk_read`/`disk_write` hooks (`64-83`).
- `src/deluge/model/sample/sample_low_level_reader.{h,cpp}`, `src/deluge/model/voice/voice_sample.{h,cpp}`
  — the RT read / boundary-crossing state machine.
- `src/deluge/model/sample/sample_holder{,_for_voice}.{h,cpp}` — the always-resident head/loop leases.
- `src/deluge/io/stream.{hpp,cpp}`, `include/libdeluge/stream_io.h` — the stream seam.
- `include/libdeluge/deluge_resource.h`, `crates/deluge_resource/src/manager.rs` — the residency
  engine + its Source-callback seam.
- `src/deluge/memory/general_memory_allocator.cpp` (`~55-104`) — manager/slab bring-up, slot sizing.
