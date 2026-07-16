# Audio Stream Module — Phase 4: introduce `SampleStream` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce a per-`Sample` `SampleStream` object that owns the Sample's cluster residency table, its open `deluge::io::Stream` read handle, the resource-manager Asset + its materialize/construct/evict callbacks, the `getCluster` dispatch, and the `ReadSource` selection — pulling that streaming orchestration off `Sample`/`SampleCluster` so consumers (SampleHolder, the RT reader, the recorder, TimeStretcher, waveform, header-parse) lease/read *through* it.

**Architecture:** This is design-spec §8 **step 3** (`docs/superpowers/specs/2026-07-15-audio-stream-module-design.md` §4/§6/§7). Phases 0–2d extracted the pure reconstruction core; Phase 3 split `Cluster` into `StreamedChunk` + `ComputedChunk`. `SampleStream` is the Sample-side orchestrator that will become the single object a future Rust port replaces behind the existing `deluge_resource` callback seam. `SampleCluster` stays as the passive residency-table *entry* (sdAddress + resident-chunk pointer + waveform min/max cache); only the *orchestration* moves onto `SampleStream`.

**Tech Stack:** C++23 in-tree; `deluge::io::Stream` for I/O (C ABI `stream_io.h` is the boundary beneath, never called directly); `deluge_resource` C ABI for residency (Source-callback + resident-chunk-pointer seam); `deluge::fast_vector` for the table; CppSpec + golden-master sweep (`scripts/golden_mixdown.sh`) + `dbt build Debug`.

## Global Constraints

- **Behavior-preserving / bit-exact expected.** Per §8, step 3 is a structural move — cordae + icoustic goldens must stay bit-exact at every task (the sim/golden build zeroes slab slots on acquire, so struct-size changes don't perturb renders). `highsiderr` is known-stale — verify via git-stash A/B, never block on it. Run `scripts/golden_mixdown.sh padsweep` on any task that changes a struct's size.
- **House style (`.clang-tidy`):** NamespaceCase `lower_case`, Class/Struct/Enum `CamelCase`, EnumConstant `UPPER_CASE`, members `lower_case` with private `_` suffix, Function/Method `lower_case`. All NEW identifiers snake_case. New code is idiomatic modern C++23, not a copy of the legacy style.
- **No new C ABI in the middle.** `SampleStream` consumes `deluge::io::Stream` (C++) + `deluge_resource` (C ABI, called directly for residency) — no new call boundary introduced.
- **RT contract preserved exactly.** The render thread never does I/O. The RT reader keeps its own local `clusters[]` lookahead array and reads resident bytes by pointer; only the boundary-crossing refill points route through `SampleStream`. Do not add per-sample indirection.
- **FAT-cluster-sized, DMA-aligned transfers unchanged** (physical model preserved).
- **Recorder path is behavior-sensitive** (§7, §10). Carry `CLUSTER_DONT_LOAD` / dirty-pin / `numReasonsHeldBySampleRecorder` / raw-block read-back faithfully; ear-check the recorder slices.
- Commit message prefix: `refactor(audio-stream): …`. Recover a clang-format pre-commit reformat with a FRESH `git commit`, never `--amend`.

## Design decisions (made from the spec + the migration map — FLAGGED for review; a reviewer/human may veto any before execution)

- **DD1 — Ownership.** `SampleStream` is an owned member of `Sample` (`Sample::stream_`), holding a `Sample& sample_` back-reference (it needs Sample geometry: `audioDataStartPosBytes`, `rawDataFormat`, `getFirstClusterIndexWith*AudioData`, `unloadable`). Home: `src/deluge/storage/audio/stream/sample_stream.{h,cpp}`, namespace `deluge::audio::stream` (the module home per §4; keeps the pure core dir and the orchestrator together while staying out of `AudioFileManager`).
- **DD2 — `SampleCluster` stays the table entry.** `SampleStream` owns `deluge::fast_vector<SampleCluster> table_`. `SampleCluster` keeps its per-entry data (`sdAddress`, `StreamedChunk* cluster`, `minValue`/`maxValue`/`investigatedWholeLength` waveform cache). The `getCluster` dispatch + `ensureNoReason` move OFF `SampleCluster` ONTO `SampleStream`; `SampleCluster` becomes a passive POD-ish entry.
- **DD3 — `SampleStream` public surface** (the accessors the ~30 consumer sites migrate onto):
  - `StreamedChunk* get_cluster(uint32_t index, int32_t load_instruction = CLUSTER_ENQUEUE, uint32_t priority_rating = 0xFFFFFFFF, Error* error = nullptr);` — the dispatch (was `SampleCluster::getCluster`).
  - `StreamedChunk* chunk_at(uint32_t index) const;` — resident chunk pointer, no lease (for stitch neighbor edges, `fillPercCache`, crossfade sampling, ALPHA bug-checks).
  - `uint32_t sd_address_at(uint32_t index) const;` and `void set_sd_address_at(uint32_t index, uint32_t sector);` — recorder + `BlockReadSource` + afm load-time population.
  - `SampleCluster& entry(uint32_t index);` / `const SampleCluster& entry(uint32_t index) const;` — heavy per-entry recorder/waveform access (waveform min/max, re-fetch-after-write).
  - `size_t num_clusters() const;` `void resize(size_t n);` `void erase_from(size_t index);` — recorder table growth/shrink + init.
  - `uint32_t ensure_resource_asset();` — moves off `Sample`; owns `resource_asset_id_` + defines the Asset with the callbacks.
  - `deluge::audio::stream::ReadSource make_read_source() const;` (or an internal read entry `read(index, span)`) — internalizes the `StreamReadSource`-vs-`BlockReadSource` selection (afm:986).
  - stream lifecycle: `void open_read_stream(...)`, `bool has_read_stream() const`, the `std::optional<deluge::io::Stream>` moves inside.
- **DD4 — Decomposition (4 tasks, each independently gated bit-exact).** T1 stream/asset/ReadSource half → T2 `getCluster` dispatch → T3 the table + accessors → T4 SampleHolder + RT reader. Recorder edits are distributed across T2 (its `getCluster` calls) and T3 (its `entry`/`sd_address`/`resize`); each recorder-touching task carries an explicit ear-check note. `std::expected<StreamedChunk*, Error>` return (flagged at `sample_cluster.h:49`) is OUT OF SCOPE — keep the `Error*` out-param, behavior-preserving.

---

## File Structure

- **Create:** `src/deluge/storage/audio/stream/sample_stream.{h,cpp}` — `deluge::audio::stream::SampleStream`. Owns the residency table, read stream, Asset id + callbacks, `getCluster` dispatch, `ReadSource` selection.
- **Modify:** `src/deluge/model/sample/sample.{h,cpp}` — hold `SampleStream stream_` (DD1); relocate `clusters`/`readStream_`/`resourceAssetId`/asset-callbacks/`ensureResourceAsset` into `SampleStream` incrementally; `Sample::stream()` accessor.
- **Modify:** `src/deluge/model/sample/sample_cluster.{h,cpp}` — strip the `getCluster`/`ensureNoReason` dispatch (moves to `SampleStream`); keep it a passive entry.
- **Modify (consumers, per the migration map):** `sample_low_level_reader.cpp`, `voice_sample.cpp`, `time_stretcher.cpp`, `sample_holder.cpp`, `sample_holder_for_voice.cpp`, `waveform_renderer.cpp`, `sample_recorder.cpp`, `wave_table.cpp`, `storage/audio/cluster_byte_source.cpp`, `storage/audio/stream/read_source.cpp`, `storage/audio/audio_file_manager.cpp` (readClusterData stitch edges + sdAddress population), `sample.cpp` (self-uses: pitch detection, `fillPercCache`, `getAveragesForCrossfade`, `markAsUnloadable`, `convertDataOnAnyClustersIfNecessary`).

---

## Task 1: `SampleStream` skeleton — own the read stream, the Asset + callbacks, and `ReadSource` selection

Introduce the class and move the STREAM / RESIDENCY-DEFINITION half onto it (the pieces with the fewest external references). The `clusters` table stays on `Sample` this task (callbacks and dispatch still reach `sample.clusters[...]`); only `readStream_`, `resourceAssetId`, `ensureResourceAsset`, the three asset callbacks, and `make_read_source` selection relocate.

**Files:**
- Create: `src/deluge/storage/audio/stream/sample_stream.{h,cpp}`
- Modify: `src/deluge/model/sample/sample.{h,cpp}` (add `SampleStream stream_;` + `SampleStream& stream()`; move `readStream_`:169, `resourceAssetId`:163, `ensureResourceAsset`:187-213, `clusterMaterialize`/`clusterConstruct`/`clusterEvict`:118-159 onto `SampleStream`; `~Sample`:215-237 releases the asset via `stream_`)
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (`buildAudioFileFromCard`:808-825 opens the stream via `sample->stream().open_read_stream(...)` + populates `sd_address` through the stream; `readClusterData`:986 `make_read_source(*sample)` → `sample->stream().make_read_source()`)
- Modify: `src/deluge/storage/audio/stream/read_source.{h,cpp}` (`make_read_source(Sample&)` selection logic moves into `SampleStream::make_read_source()`; `BlockReadSource` reads `sample.stream().sd_address_at(i)` — but since the table is still on Sample this task, it may temporarily read `sample.clusters[i].sdAddress` — keep bit-exact)

**Interfaces:**
- Produces: `class SampleStream { explicit SampleStream(Sample& sample); uint32_t ensure_resource_asset(); deluge::audio::stream::ReadSource make_read_source() const; void open_read_stream(...); bool has_read_stream() const; uint32_t resource_asset_id() const; /* asset callbacks as private statics */ };` and `Sample::stream() -> SampleStream&`.

- [ ] **Step 1:** Write a CppSpec (or extend `tests/spec_audio_stream/`) asserting `SampleStream::make_read_source()` returns a `StreamReadSource` when a read stream is open and a `BlockReadSource` otherwise (feed a stub Sample/stream) — the selection seam the afm comment names. Run it; verify it fails (no `SampleStream` yet).
- [ ] **Step 2:** Create `sample_stream.{h,cpp}` with the ctor(`Sample&`), `resource_asset_id_`, `readStream_` (`std::optional<deluge::io::Stream>`), `ensure_resource_asset()` (moved verbatim from `Sample::ensureResourceAsset`, now `deluge_resource_define_asset` with the callbacks + `set_construct`), the three asset callbacks as private statics (bodies moved verbatim; they still write `sample_.clusters[index].cluster` — table on Sample this task), `make_read_source()` (the `read_source.cpp:30-36` selection), `open_read_stream`/`has_read_stream`.
- [ ] **Step 3:** In `Sample`, add `deluge::audio::stream::SampleStream stream_{*this};` + `SampleStream& stream() { return stream_; }`. Remove `readStream_`/`resourceAssetId`/`ensureResourceAsset`/the callbacks from `Sample`; repoint `Sample`'s own uses (`~Sample`, `applyProjectReference`, `markAsUnloadable`) to `stream_`. Repoint afm `buildAudioFileFromCard` + `readClusterData` selection. Delete the now-empty `make_read_source(Sample&)` free function (or make it forward) — prefer moving its body into `SampleStream`.
- [ ] **Step 4:** Run the spec (PASS); `dbt build Debug` clean; `./dbt test` 20/20; `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` bit-exact; `padsweep` (Sample struct size changes — expect layout-invariant, re-baseline only if a fixture shifts deterministically with no behavior change).
- [ ] **Step 5:** Commit `refactor(audio-stream): introduce SampleStream; move read-stream + Asset + ReadSource selection onto it`.

## Task 2: move the `getCluster` dispatch onto `SampleStream`

`SampleCluster::getCluster(Sample*, index, …)` → `SampleStream::get_cluster(index, …)` (the stream knows its Sample). Repoint all ~20 callers (see the migration map §4). `SampleCluster` keeps being the entry; the table stays on `Sample` this task (`get_cluster` reaches `sample_.clusters[index]` internally).

**Files:**
- Modify: `sample_stream.{h,cpp}` (add `StreamedChunk* get_cluster(uint32_t index, int32_t load_instruction, uint32_t priority_rating, Error* error)`; body = `sample_cluster.cpp:63-146` verbatim, `sample->` → `sample_.`, `this->cluster` → `sample_.clusters[index].cluster`; also move `ensureNoReason`)
- Modify: `sample_cluster.{h,cpp}` (delete `getCluster`:50-51/63-146 + `ensureNoReason`:52/50-59)
- Modify (repoint callers — exact sites from map §4): `sample.cpp:1474,1500`; `sample_low_level_reader.cpp:304,380`; `voice_sample.cpp:203,847`; `time_stretcher.cpp:1142`; `waveform_renderer.cpp:407,433`; `sample_holder.cpp:220`; `cluster_byte_source.cpp:53`; `sample_recorder.cpp:141,804,943,1262,1279,1297,1446,1494`; `wave_table.cpp:423`. Transform `X->clusters[i].getCluster(X, i, INSTR, prio, &err)` → `X->stream().get_cluster(i, INSTR, prio, &err)`.

**Interfaces:**
- Consumes: `Sample::stream()` (Task 1). Produces: `SampleStream::get_cluster(...)`.

- [ ] **Step 1:** Move `get_cluster` + `ensureNoReason` onto `SampleStream` (bodies verbatim, rebased on `sample_.clusters[index]`). Delete from `SampleCluster`.
- [ ] **Step 2:** Repoint every caller in the list above (mechanical; the compiler catches a miss since `SampleCluster::getCluster` no longer exists).
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact. **Ear-check note:** this task touches the recorder's `getCluster` calls (`sample_recorder.cpp` sites) — after goldens, do a recording round-trip ear-check before merge (§9 NEEDS-HARDWARE for recording).
- [ ] **Step 4:** Commit `refactor(audio-stream): move getCluster dispatch onto SampleStream`.

## Task 3: move the residency table into `SampleStream` + expose accessors

Move `clusters` (`fast_vector<SampleCluster>`) off `Sample` into `SampleStream::table_`; add the `chunk_at`/`entry`/`sd_address_at`/`num_clusters`/`resize`/`erase_from` accessors (DD3); repoint every raw `sample->clusters[...]` site (map §3).

**Files:**
- Modify: `sample_stream.{h,cpp}` (own `deluge::fast_vector<SampleCluster> table_;`; add `chunk_at`, `entry` (×const), `sd_address_at`/`set_sd_address_at`, `num_clusters`, `resize`, `erase_from`; rebase `get_cluster`/callbacks/`ensureNoReason` onto `table_`)
- Modify: `sample.{h,cpp}` (remove `clusters`:157; `initialize`:102 `resize` → `stream_.resize`; self-uses at `sample.cpp:715,937,1151-1152,1821,1914-1954` → `stream_.chunk_at(...)`/`num_clusters()`)
- Modify (raw-access repoints, exact sites from map §3): `sample_low_level_reader.cpp:108` (`chunk_at`); `time_stretcher.cpp:727` (`chunk_at`); `waveform_renderer.cpp:241,382,432-433` (`num_clusters`/`entry`); `sample_recorder.cpp:73,77,96,620,634,802,873,885,937,1218,1246,1383,1555,1624-1625` (`entry`/`sd_address`/`chunk_at`/`num_clusters`/`resize`/`erase_from`); `cluster_byte_source.cpp` (via `get_cluster` already); `read_source.cpp:23` (`BlockReadSource` → `sample_.stream().sd_address_at(i)`); `audio_file_manager.cpp:250,824,924,1039,1050-1051` (sdAddress population + stitch neighbor-edge `chunk_at(idx±1)` + `num_clusters` bound)

**Interfaces:**
- Consumes: `SampleStream` (Tasks 1–2). Produces: the table accessors (DD3).

- [ ] **Step 1:** Add a CppSpec for the stitch neighbor-edge path exercising `chunk_at(index±1)` returning the correct resident/null pointer (mock table), guarding the §6 boundary-conversion coupling. Run; verify it fails.
- [ ] **Step 2:** Move `table_` into `SampleStream`; add the accessors; rebase `get_cluster`/callbacks/`ensureNoReason` onto `table_`.
- [ ] **Step 3:** Repoint every raw site in the list (the compiler catches misses — `Sample::clusters` no longer exists). Preserve the recorder's re-fetch-after-write pattern (`sample_recorder.cpp:873,885` re-takes `entry` after a write because the audio routine may reallocate the table — keep that reacquire through `entry()`).
- [ ] **Step 4:** Spec PASS; `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep` (Sample loses the vector member — size changes; expect layout-invariant). **Ear-check note:** heavy recorder-table changes (grow/shrink/sdAddress) — recording round-trip + `alterFile` ear-check before merge.
- [ ] **Step 5:** Commit `refactor(audio-stream): move the residency table into SampleStream + accessors`.

## Task 4: `SampleHolder` + RT reader lease/refill through `SampleStream`

Repoint the always-resident head/loop leasing and the RT reader's boundary-crossing refill onto `SampleStream` accessors. Behavior-preserving; the reader keeps its own local `clusters[]` array (hot loop untouched — map §6).

**Files:**
- Modify: `sample_holder.cpp:218,220` (`((Sample*)audioFile)->clusters[clusterIndex]` → `((Sample*)audioFile)->stream()`, `get_cluster(clusterIndex, …)`); `sample_holder_for_voice.cpp:96` (`->clusters.size()` → `->stream().num_clusters()`)
- Modify: `sample_low_level_reader.cpp:304,380` already repointed in Task 2 (getCluster); confirm the refill points read via `stream()`; `:108` raw peek via `chunk_at` (Task 3). Verify no per-sample-loop site now touches `stream()`.
- Modify: `voice_sample.cpp:203,847` already repointed in Task 2; confirm.

**Interfaces:**
- Consumes: `SampleStream::get_cluster`/`num_clusters`/`chunk_at` (Tasks 2–3).

- [ ] **Step 1:** Repoint the SampleHolder direct-index leasing (`claimClusterReasonsForMarker`:218-220, the `clusters.size() <= 4` short-sample fallback:96) onto `stream()`.
- [ ] **Step 2:** Audit the RT reader + VoiceSample: confirm every `Sample::clusters` touch now goes through `stream()` and lives only at boundary-crossing/refill points (`assignClusters`, `moveOnToNextCluster`, `attemptLateSampleStart`, cache-resync, the `:108` peek) — NOT in the per-sample inner loop. Add a code comment at the refill sites noting the one-hop-per-boundary cost is intentional.
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact. **Ear-check note:** streaming-under-load + loop-start leasing behavior — play a looping, streamed (non-fully-resident) sample and confirm no dropouts (§9 NEEDS-HARDWARE: streaming-under-load).
- [ ] **Step 4:** Commit `refactor(audio-stream): SampleHolder + RT reader lease through SampleStream`.

---

## Verification (whole increment)

- Per-task: `dbt build Debug` + `./dbt test` (20/20) + cordae/icoustic bit-exact; `padsweep` on the struct-size-changing tasks (T1, T3).
- The reconstruction-core specs (`tests/spec_audio_stream/`, `tests/qemu/spec/`) stay green — `SampleStream` only relocates the orchestration around the already-pure core.
- **NEEDS-HARDWARE before merge** (§9): recording round-trip + `alterFile`, memory-pressure eviction, streaming-under-load (looping non-resident sample), non-native-format (24-bit) sample load. Hardware gating is the user's call — do not treat it as a blocking next step.

## After Phase 4

Phase 5 (design-spec §8 step 4): consolidate the `loader` pump into the module + retire the `AudioFileManager` streaming methods (`loadCluster`, `readClusterData`, `loadAnyEnqueuedClusters`, `removeReasonFromCluster`, `loadingQueueHasAnyLowestPriorityElements`, the `clusterBeingLoaded` sentinel). Then follow-ons A (decouple `ComputedChunk` sizing from FAT `Cluster::size`) + B (`RecordingBacking`: `SampleStream` owns the write stream + physical-address bookkeeping, completing the `ReadSource` symmetry), plus the deferred Phase-3 dead-code cleanup (`resource_lease_asset_id()`, `ComputedChunk::sampleCache`) and the batched test-hygiene ticket.

## Self-Review notes (author)

- Spec coverage: §4 (SampleStream shape) → all 4 tasks; §6 (ReadSource selection ownership) → T1; §7 (getCluster dispatch, RT contract) → T2/T4; §8 step 3 → the whole plan. §6 stitch coupling is guarded by the T3 `chunk_at` spec.
- Open risks to surface at review: (a) DD1 places `SampleStream` in `storage/audio/stream/` while it holds a `Sample&` — a slight layering inversion vs. the pure core in the same dir; alternative is `model/sample/`. (b) The recorder is spread across T2+T3; if its behavior-sensitivity warrants isolation, split a dedicated recorder-migration task. (c) T3 is the largest blast radius — could split Sample-self-uses vs. external consumers if a reviewer finds it too big for one gate.
