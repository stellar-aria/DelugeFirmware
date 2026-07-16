# Audio Stream — Phase 3: de-overload `Cluster` into `StreamedChunk` + `ComputedChunk`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Split the one overloaded `Cluster` class into two **independent** types — `StreamedChunk` (file-backed SAMPLE data) and `ComputedChunk` (the repitch SampleCache + the perc cache, structurally identical) — each carrying only its own fields, in idiomatic house-style C++23. No shared base / no inheritance (the resource manager treats every chunk as opaque `void*`; nothing needs a common vtable). Behavior-preserving.

**Architecture:** Migration is **alias-first + snake_case-first**: (1) drop dead enum values; (2) rename `Cluster`'s members + methods to house snake_case while it's still one type (golden-neutral, mechanical); (3) `using StreamedChunk = Cluster;` + retype the file-backed sample pointer sites; (4) `using ComputedChunk = Cluster;` + retype the cache/perc sites; (5) de-methodize the shared lease bookkeeping into free functions; (6) flip the aliases to two real distinct structs and delete `Cluster`. Each step compiles + golden-gates; the real struct-split becomes a localized diff.

**Naming (the house convention — this is a NEW type definition, so it applies fully):** the split structs' members and methods are **snake_case** (`cluster_index`, `resource_slot`, `data`, `type`, `resource_lease_asset_id()`, `convert_data_if_necessary()` …). Public members get no suffix (they're read across the streaming code); genuinely-private members get the `_` suffix. Types are CamelCase (`StreamedChunk`, `ComputedChunk`), enum constants UPPER_CASE. Matches the `.clang-tidy` we set up. (Field name→snake mapping: `clusterIndex`→`cluster_index`, `resourceSlot`→`resource_slot`, `numReasonsHeldBySampleRecorder`→`num_reasons_held_by_sample_recorder`, `extraBytesAtStartConverted`→`extra_bytes_at_start_converted`, `extraBytesAtEndConverted`→`extra_bytes_at_end_converted`, `firstThreeBytesPreDataConversion`→`first_three_bytes_pre_data_conversion` [optionally `unconverted_head`, matching Phase 2's `unconverted_head_out`], `unloadable`/`loaded`/`sample`/`data`/`dummy`/`type` already snake; methods `convertDataIfNecessary`→`convert_data_if_necessary`, `addReason`→`add_reason`, `leaseCount`→`lease_count`, `resourceLeaseAssetId`→`resource_lease_asset_id`, `setSize`→`set_size`, `destroy` fine.)

**Field split (from the current-state map):** **StreamedChunk** = `cluster_index`, `resource_slot`, `sample`, `loaded`, `unloadable`, `num_reasons_held_by_sample_recorder`, `extra_bytes_at_start_converted`, `extra_bytes_at_end_converted`, `first_three_bytes_pre_data_conversion[3]`, `dummy[]`+`data[]`, `convert_data_if_necessary()`, `resource_lease_asset_id()` (`= sample->resourceAssetId`). **ComputedChunk** = `type` (SAMPLE_CACHE / PERC_CACHE_FORWARDS / PERC_CACHE_REVERSED), `cluster_index`, `resource_slot`, owner (`sample` for perc; drop the never-read `sample_cache` field if `sampleCache->sample` is reachable), `dummy[]`+`data[]`, `resource_lease_asset_id()` (`type`-dispatch: SAMPLE_CACHE→NO_ASSET, PERC→`sample->percCacheAssetId[dir]`). Shared bookkeeping (`add_lease`/`release_lease`/`lease_count` over `resource_slot`) → free functions.

**Tech Stack:** C++23; the `deluge_resource` C ABI is UNCHANGED (opaque chunks, one pre-sized slab); golden-master gate (`scripts/golden_mixdown.sh`); the struct-split changes layout, so `padsweep` matters there.

## Global Constraints

- **Behavior-preserving.** Gate each task: `dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` bit-exact. `highsiderr` KNOWN-STALE. Run `scripts/golden_mixdown.sh padsweep` on the struct-split task (Task 6) and expect a re-baseline/ear-check if a fixture shifts deterministically (documented layout-sensitivity).
- **Two independent types, no base/inheritance.** Shared bits are `static` (`Cluster::size`/`size_magnitude`, keep accessible to both — a shared constant/namespace or duplicated statics) + free functions + the `data[]`/`dummy[]` over-alloc layout convention (documented once).
- **snake_case on the new/renamed identifiers**, per the house `.clang-tidy` convention. Sample's own legacy fields that these read (`sample->resourceAssetId`, `sample->percCacheAssetId`, `sample->clusters`) are NOT renamed here (out of scope — Sample is not being refactored).
- **No `deluge_resource` ABI change** — one slab, slot sized `max(sizeof(StreamedChunk), sizeof(ComputedChunk)) + Cluster::size` (the manager ignores the per-request size for `BACKING_SLAB`). Call sites pass `sizeof(<its type>) + <size>` (value ignored, kept honest).

---

## File Structure (touched)
- `src/deluge/storage/cluster/cluster.{h,cpp}` — the type defs (rename → aliases → real split), the shared free functions.
- `sample_cluster.{h,cpp}`, `sample.{h,cpp}`, `sample_cache.{h,cpp}`, `sample_recorder.cpp`, `sample_holder.{h,cpp}`, `sample_low_level_reader.{h,cpp}`, `voice/voice_sample.{h,cpp}`, `storage/audio/cluster_byte_source.{h,cpp}`, `dsp/timestretch/time_stretcher.{h,cpp}`, `storage/audio/audio_file_manager.{h,cpp}`, `gui/waveform/waveform_renderer.cpp`.
- `memory/general_memory_allocator.cpp` — slab slot sizing.

---

## Task 1: drop the dead `Type` enumerators (`GENERAL_MEMORY`, `OTHER`)

- [ ] **Step 1:** Grep-confirm `Type::GENERAL_MEMORY`/`Type::OTHER` have zero live uses; remove both from `Cluster::Type` (cluster.h). Fix any switch (should be none).
- [ ] **Step 2:** `dbt build Debug` clean; `./dbt test`; golden `check` + icoustic bit-exact.
- [ ] **Step 3:** Commit `chore(audio-stream): drop dead Cluster::Type::{GENERAL_MEMORY,OTHER}`.

---

## Task 2: rename `Cluster`'s members + methods to snake_case (house style)

Pure mechanical rename while `Cluster` is still one type — golden-neutral. This is the identifiers-only step that brings the struct to house style before the split, so the alias/split steps operate on snake_case names.

**Files:** `cluster.{h,cpp}` + EVERY access site (all files in File Structure).

- [ ] **Step 1:** Rename each member + method per the mapping in **Naming** above (`clusterIndex`→`cluster_index`, `resourceSlot`→`resource_slot`, `numReasonsHeldBySampleRecorder`→`num_reasons_held_by_sample_recorder`, `extraBytesAtStart/EndConverted`→snake, `firstThreeBytesPreDataConversion`→`first_three_bytes_pre_data_conversion`; methods `convertDataIfNecessary`/`addReason`/`leaseCount`/`resourceLeaseAssetId`/`setSize`→snake). Update every access site (grep each old name; e.g. `->clusterIndex`, `.cluster->resourceSlot`, `->addReason()`). `Cluster::size`/`size_magnitude` statics keep their names (already snake).
- [ ] **Step 2:** `dbt build Debug` clean; `./dbt test`; golden bit-exact (pure rename).
- [ ] **Step 3:** Commit `refactor(audio-stream): rename Cluster members/methods to snake_case (house style)`.

---

## Task 3: `StreamedChunk` alias + retype the file-backed sample sites

**Interfaces:** `using StreamedChunk = Cluster;` (cluster.h). Golden-neutral (same type).

- [ ] **Step 1:** Add `using StreamedChunk = Cluster;`.
- [ ] **Step 2:** Retype the SAMPLE-payload pointer members/locals/params to `StreamedChunk*`: `SampleCluster::cluster`; `SampleLowLevelReader::clusters[]` + hot reads; `SampleRecorder::currentRecordCluster` + recorder sites; `ClusterByteSource::currentCluster_`; `AudioFileManager::clusterBeingLoaded` + `readClusterData`/`loadCluster` `Cluster&` params; `TimeStretcher::clustersForPercLookahead` (misnamed source-audio); the `clusterMaterialize`/`clusterConstruct`/`clusterEvict` placement-new/casts; the reader/voice hot sites; `SampleHolder::claimClusterReasonsForMarker(Cluster** …)` (SAMPLE arrays → `StreamedChunk**`, confirm call sites).
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden bit-exact.
- [ ] **Step 4:** Commit `refactor(audio-stream): alias StreamedChunk; retype the file-backed sample sites`.

---

## Task 4: `ComputedChunk` alias + retype the cache/perc sites

**Interfaces:** `using ComputedChunk = Cluster;`. Golden-neutral.

- [ ] **Step 1:** Add `using ComputedChunk = Cluster;`.
- [ ] **Step 2:** Retype the cache/perc pointers to `ComputedChunk*`: `SampleCache::clusters[]` + its reads (incl. the `cacheCluster` locals in sample_low_level_reader.cpp/voice_sample.cpp); `Sample::percCacheClusters[2]` + perc reads; `TimeStretcher::percCacheClustersNearby[2]`; the `sampleCacheConstruct`/`Evict` + `percCacheConstruct`/`Evict` placement-new/casts.
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden bit-exact.
- [ ] **Step 4:** Commit `refactor(audio-stream): alias ComputedChunk; retype the cache/perc sites`.

---

## Task 5: de-methodize the shared lease bookkeeping (prepare for the split)

The lease ops shared across payloads can't stay methods on one class once split. Convert to free functions over `resource_slot` while still the single aliased `Cluster` (golden-neutral).

- [ ] **Step 1:** Free functions (snake_case) over `resource_slot`: `namespace deluge::cluster { void add_lease(uint32_t resource_slot); uint32_t lease_count(uint32_t resource_slot); }` (thin wrappers over `deluge_resource_add_lease`/`deluge_resource_lease_count_by_slot`). Repoint `Cluster::add_reason`/`lease_count` callers. `AudioFileManager::removeReasonFromCluster` → operate via the chunk pointer + slot so it works for either future type (a small `release_lease(DelugeResource*, void* chunk)` free function, or two thin overloads at Task 6).
- [ ] **Step 2:** Leave `resource_lease_asset_id` a `Cluster` method for now; its two future forms are noted in Architecture (Task 6 gives each type its own).
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; golden bit-exact.
- [ ] **Step 4:** Commit `refactor(audio-stream): lease bookkeeping as free functions over resource_slot`.

---

## Task 6: split the struct — two real independent types (the structural change)

- [ ] **Step 1:** In `cluster.h`, replace the two aliases with two real structs (snake_case members per Architecture): `StreamedChunk` (SAMPLE fields + `convert_data_if_necessary()` + `resource_lease_asset_id() = sample->resourceAssetId`; no `type`); `ComputedChunk` (`type` 3-values, `cluster_index`, `resource_slot`, owner `sample` [drop vestigial `sample_cache` if `sampleCache->sample` reachable], `dummy[]`/`data[]`, `resource_lease_asset_id()` type-switch; no SAMPLE-only fields). Keep the `dummy[]`+`data[]` over-alloc layout identical on both. `Cluster::size`/`size_magnitude` statics stay accessible to both.
- [ ] **Step 2:** Slab: `general_memory_allocator.cpp` (~:73) slot = `max(sizeof(StreamedChunk), sizeof(ComputedChunk)) + Cluster::size` (constexpr max). Each `request`/`acquire` call site passes `sizeof(<its type>) + <size>` (ignored, kept honest).
- [ ] **Step 3:** Callbacks placement-new the right type; `destroy`/`operator delete` → each type's slab-release (or a shared free `free_chunk(void*)`).
- [ ] **Step 4:** Fix the fallout (a now-split field touched on the wrong type won't compile) — should be few given Tasks 3-4 segregated the pointer types. The RT-reader files hold both types as separate locals; verify each accesses only its type's fields.
- [ ] **Step 5:** Verify: `dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact; **`scripts/golden_mixdown.sh padsweep`**. If a fixture shifts deterministically with 0 behavior change → documented layout-sensitivity; re-baseline/ear-check, don't assume a regression.
- [ ] **Step 6:** Commit `refactor(audio-stream): split Cluster into independent StreamedChunk + ComputedChunk`.

---

## Roadmap — after Phase 3

Phase 4 (`SampleStream`), Phase 5 (loader consolidation), follow-ons A (decouple ComputedChunk sizing from FAT `Cluster::size`) + B (`RecordingBacking`), plus the batched test-hygiene ticket, per the module design spec.
