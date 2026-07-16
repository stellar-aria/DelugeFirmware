# Audio Stream — Phase 3: de-overload `Cluster` into `StreamedChunk` + `ComputedChunk`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Split the one overloaded `Cluster` class into two **independent** types — `StreamedChunk` (file-backed SAMPLE data) and `ComputedChunk` (the repitch SampleCache + the perc cache, structurally identical) — so each carries only its own fields. No shared base / no inheritance (the resource manager treats every chunk as opaque `void*`; nothing needs a common vtable). Behavior-preserving.

**Architecture:** Migration is **alias-first**: introduce `using StreamedChunk = Cluster;` / `using ComputedChunk = Cluster;`, retype every pointer site to the right alias (mechanical, golden-neutral since both alias `Cluster`), de-methodize the lease bookkeeping shared by both, THEN flip the aliases to two real distinct structs. Each step compiles and golden-gates; the real struct-split becomes a localized diff (its fallout is only where a now-split field is touched — which the retyping has already segregated). Field split (from the current-state map): **StreamedChunk** = `clusterIndex`, `resourceSlot`, `sample`, `loaded`, `unloadable`, `numReasonsHeldBySampleRecorder`, `extraBytesAtStart/EndConverted`, `firstThreeBytesPreDataConversion[3]`, `dummy[]`+`data[]`, `convertDataIfNecessary()`. **ComputedChunk** = `type` (SAMPLE_CACHE / PERC_CACHE_FORWARDS / PERC_CACHE_REVERSED), `clusterIndex`, `resourceSlot`, owner (`sample` for perc; drop the never-read `sampleCache` field if `sampleCache->sample` is reachable), `dummy[]`+`data[]`. Shared bookkeeping (`add_reason`/`remove_reason`/`lease_count` over `resourceSlot`) becomes free functions; `resourceLeaseAssetId` becomes type-specific (StreamedChunk → `sample->resourceAssetId`; ComputedChunk → `type`-dispatch: SAMPLE_CACHE→NO_ASSET, PERC→`sample->percCacheAssetId[dir]`).

**Tech Stack:** C++23; the `deluge_resource` C ABI is UNCHANGED (opaque chunks, one pre-sized slab); golden-master gate (`scripts/golden_mixdown.sh`) — this phase changes struct layout, so the padsweep layout-invariance guard matters.

## Global Constraints

- **Behavior-preserving.** Gate each task: `dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` bit-exact. `highsiderr` KNOWN-STALE. **Struct-size changes shift heap addresses** — run `scripts/golden_mixdown.sh padsweep` on the struct-split task (Task 5) and expect to re-baseline / ear-check if a fixture shifts deterministically (per the documented layout-sensitivity history).
- **Two independent types, no base/inheritance.** Shared bits are `static` (`Cluster::size`/`size_magnitude` → keep as statics, hoist to a shared location or duplicate) + free functions + the `data[]`/`dummy[]` layout convention (documented once).
- **No `deluge_resource` ABI change** — one slab, slot sized `max(sizeof(StreamedChunk), sizeof(ComputedChunk)) + Cluster::size` (the manager ignores the per-request size for `BACKING_SLAB`). Every `request`/`acquire` call site passes `sizeof(<its type>) + Cluster::size` (value ignored, but keep it honest).
- snake_case for NEW identifiers; idiomatic C++23. Legacy field names on the split structs may keep their current names (renaming is out of scope).

---

## File Structure (touched)
- `src/deluge/storage/cluster/cluster.{h,cpp}` — the type defs (aliases → real split), the shared free functions.
- `src/deluge/model/sample/sample_cluster.{h,cpp}`, `sample.{h,cpp}`, `sample_cache.{h,cpp}`, `sample_recorder.cpp`, `sample_holder.{h,cpp}`, `sample_low_level_reader.{h,cpp}` — pointer retypes + field access.
- `src/deluge/model/voice/voice_sample.{h,cpp}`, `src/deluge/storage/audio/cluster_byte_source.{h,cpp}`, `src/deluge/dsp/timestretch/time_stretcher.{h,cpp}`, `src/deluge/storage/audio/audio_file_manager.{h,cpp}`, `src/deluge/gui/waveform/waveform_renderer.cpp`.
- `src/deluge/memory/general_memory_allocator.cpp` — slab slot sizing.

---

## Task 1: drop the dead `Type` enumerators (`GENERAL_MEMORY`, `OTHER`)

**Files:** `cluster.h` (+ any switch that lists them).

- [ ] **Step 1:** Grep-confirm `Type::GENERAL_MEMORY` and `Type::OTHER` have zero live uses (only the enum declaration). Remove both enumerators from `Cluster::Type` (cluster.h). Fix any `switch` that enumerated them (there should be none needing a case).
- [ ] **Step 2:** `dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact (removing dead enum values is behavior-neutral).
- [ ] **Step 3:** Commit `chore(audio-stream): drop dead Cluster::Type::{GENERAL_MEMORY,OTHER}`.

---

## Task 2: `StreamedChunk` alias + retype the SAMPLE (streamed) sites

**Files:** `cluster.h` (alias), `sample_cluster.{h,cpp}`, `sample.{h,cpp}` (the SAMPLE asset callbacks), `sample_recorder.cpp`, `cluster_byte_source.{h,cpp}`, `sample_low_level_reader.{h,cpp}`, `voice_sample.{h,cpp}`, `audio_file_manager.{h,cpp}`, `time_stretcher.{h,cpp}` (the misnamed source-audio lookahead), `sample_holder.{h,cpp}`, `waveform_renderer.cpp`.

**Interfaces:** Produces `using StreamedChunk = Cluster;` in `cluster.h`. Golden-neutral (same type).

- [ ] **Step 1:** Add `using StreamedChunk = Cluster;` to `cluster.h`.
- [ ] **Step 2:** Retype the SAMPLE-payload pointer members + locals + params to `StreamedChunk*` (they still ARE `Cluster` via the alias, so this compiles and is behavior-identical):
  - `SampleCluster::cluster` (sample_cluster.h:57); `Sample::clusters` element access.
  - `SampleLowLevelReader::clusters` (sample_low_level_reader.h:106) + its hot read sites.
  - `SampleRecorder::currentRecordCluster` (sample_recorder.h:92) + all recorder sites.
  - `ClusterByteSource::currentCluster_` (cluster_byte_source.h:51).
  - `AudioFileManager::clusterBeingLoaded` (audio_file_manager.h:129); `readClusterData`/`loadCluster`'s `Cluster&` params (SAMPLE-only).
  - `TimeStretcher::clustersForPercLookahead` (time_stretcher.h:96 — despite the name, SAMPLE-cluster lookahead).
  - `Sample::clusterMaterialize`/`clusterConstruct`/`clusterEvict` (sample.cpp:117-160) placement-new + `Cluster*` casts → `StreamedChunk`.
  - Voice/reader hot read sites in `voice_sample.cpp`.
  - `SampleHolder::claimClusterReasonsForMarker(Cluster** ...)` — this is invoked with SAMPLE arrays → `StreamedChunk**` (confirm call sites).
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden `check` + icoustic bit-exact (alias = identical).
- [ ] **Step 4:** Commit `refactor(audio-stream): alias StreamedChunk; retype the file-backed sample sites`.

---

## Task 3: `ComputedChunk` alias + retype the cache/perc sites

**Files:** `cluster.h` (alias), `sample_cache.{h,cpp}`, `sample.{h,cpp}` (perc), `time_stretcher.{h,cpp}` (the perc-nearby ring), `sample_low_level_reader.cpp`/`voice_sample.cpp` (the `cacheCluster` locals).

**Interfaces:** Produces `using ComputedChunk = Cluster;`. Golden-neutral.

- [ ] **Step 1:** Add `using ComputedChunk = Cluster;` to `cluster.h`.
- [ ] **Step 2:** Retype the cache/perc pointer members + locals to `ComputedChunk*`:
  - `SampleCache::clusters[]` (sample_cache.h:60) + its reads (sample_cache.cpp, and the `cacheCluster` locals in sample_low_level_reader.cpp:709-936 / voice_sample.cpp).
  - `Sample::percCacheClusters[2]` (sample.h:146) + all perc reads (sample.cpp).
  - `TimeStretcher::percCacheClustersNearby[2]` (time_stretcher.h:98).
  - The perc/cache construct/evict callbacks (`sampleCacheConstruct`/`sampleCacheEvict`, `percCacheConstruct`/`percCacheEvict`) placement-new + casts → `ComputedChunk`.
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden bit-exact.
- [ ] **Step 4:** Commit `refactor(audio-stream): alias ComputedChunk; retype the cache/perc sites`.

---

## Task 4: de-methodize the shared lease bookkeeping (prepare for the split)

The lease ops (`addReason`, `leaseCount`, `AudioFileManager::removeReasonFromCluster`) and `resourceLeaseAssetId` are currently `Cluster` methods shared across payloads. With two independent types they can't stay methods on one class. Convert them to a form both future types can use — while everything is still the single aliased `Cluster` (golden-neutral).

**Files:** `cluster.{h,cpp}`, `audio_file_manager.{h,cpp}`, callers.

- [ ] **Step 1:** Introduce free functions over `resourceSlot` (both future types carry it): e.g. `namespace deluge::cluster { void add_lease(uint32_t resource_slot); uint32_t lease_count(uint32_t resource_slot); }` (thin wrappers over `deluge_resource_add_lease` / `deluge_resource_lease_count_by_slot`). Repoint `Cluster::addReason`/`leaseCount` callers at them (or keep the methods delegating for now). `AudioFileManager::removeReasonFromCluster(Cluster&, ...)` → operate via the chunk pointer + slot so it works for either future type (a small free function `release_lease(DelugeResource*, void* chunk)` or two thin overloads later).
- [ ] **Step 2:** `resourceLeaseAssetId` — leave it a `Cluster` method for now BUT confirm its two future forms: StreamedChunk → `sample->resourceAssetId`; ComputedChunk → the `type`-switch (SAMPLE_CACHE→NO_ASSET, PERC_*→`sample->percCacheAssetId[reversed]`). (The split in Task 5 gives each type its own.)
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden bit-exact (pure refactor of shared ops).
- [ ] **Step 4:** Commit `refactor(audio-stream): lease bookkeeping as free functions over resource_slot`.

---

## Task 5: split the struct — two real independent types (the structural change)

Flip the aliases to two distinct structs; delete `Cluster`; size the slab to the max; give each type its own fields + `resourceLeaseAssetId`; placement-new the right type in each callback.

**Files:** `cluster.{h,cpp}`, `general_memory_allocator.cpp`, the asset callbacks in `sample.cpp`/`sample_cache.cpp`, and any fallout site.

- [ ] **Step 1:** In `cluster.h`, replace the two aliases with two real structs:
  - `StreamedChunk` — the SAMPLE fields (§Architecture), its own `convertDataIfNecessary()` wrapper + `resourceLeaseAssetId()` (`= sample->resourceAssetId`). No `type` (single kind).
  - `ComputedChunk` — `type` (3 values), `clusterIndex`, `resourceSlot`, owner (`sample`; drop the vestigial `sampleCache` field if `sampleCache->sample` is reachable — else keep it), `dummy[]`/`data[]`, its own `resourceLeaseAssetId()` (`type`-switch). No SAMPLE-only fields.
  - Keep the `dummy[]`+`data[]` over-alloc layout identical on both (the `&data[pos]-4+byteDepth` back-peek idiom + the `Cluster::size` overhang rely on it). `Cluster::size`/`size_magnitude` statics: keep accessible to both (a shared `namespace` constant or duplicate).
- [ ] **Step 2:** Slab: in `general_memory_allocator.cpp` (~:73), slot = `max(sizeof(StreamedChunk), sizeof(ComputedChunk)) + Cluster::size` (use a `constexpr max`). Each `deluge_resource_request`/`acquire` call site passes `sizeof(<its type>) + <size>` (value ignored by the manager, keep honest).
- [ ] **Step 3:** Callbacks: `clusterMaterialize`/`clusterConstruct`/`clusterEvict` placement-new `StreamedChunk`; `sampleCacheConstruct`/`Evict` + `percCacheConstruct`/`Evict` placement-new `ComputedChunk`. `destroy()`/`operator delete` (currently on `Cluster`) — give each type the slab-release form (or a shared free `free_chunk(void*)`).
- [ ] **Step 4:** Fix the fallout — any site that now touches a field on the wrong type won't compile. These should be few (Tasks 2-3 segregated the pointer types); resolve each by confirming it's genuinely the right payload. The RT-reader files hold both types as separate locals — verify each accesses only its type's fields.
- [ ] **Step 5:** Verify: `dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact; **`scripts/golden_mixdown.sh padsweep`** (layout-invariance — the struct sizes changed). If a fixture shifts deterministically with 0 behavior change, that's the documented layout-sensitivity — re-baseline / ear-check per the prior pattern, don't assume a regression.
- [ ] **Step 6:** Commit `refactor(audio-stream): split Cluster into independent StreamedChunk + ComputedChunk`.

---

## Roadmap — after Phase 3

Phase 4 (`SampleStream` — owns the residency table + `getCluster` + the `ReadSource` per Phase-1's deferred alloc note), Phase 5 (loader consolidation), then follow-ons A (decouple ComputedChunk sizing from FAT `Cluster::size`) + B (`RecordingBacking`), plus the batched test-hygiene ticket, per the module design spec.
