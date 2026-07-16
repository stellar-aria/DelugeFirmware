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
- **DD4 — Decomposition (5 tasks, each independently gated bit-exact; COEXISTENCE migration).** Reviewed with the human: the recorder gets a **dedicated task** (it is the heaviest, most behavior-sensitive consumer), and the residency-table migration stays coherent (introduced once, internalized once — not split self-vs-external). To isolate the recorder into its own commit while keeping *every intermediate build green*, the `SampleStream` API is introduced **additively**: `Sample::clusters` stays public and `SampleCluster::getCluster` becomes a thin forwarder during migration, consumers move group-by-group, and a final cleanup task internalizes the table. Order: **T1** stream/Asset/ReadSource half → **T2** add the `SampleStream` API (get_cluster + table accessors, coexisting) + migrate the bulk non-recorder/non-reader consumers → **T3** dedicated recorder migration → **T4** SampleHolder + RT reader → **T5** cleanup (physically internalize `table_`, delete `Sample::clusters` + the `SampleCluster::getCluster` forwarder + `ensureNoReason`). `std::expected<StreamedChunk*, Error>` return (flagged at `sample_cluster.h:49`) is OUT OF SCOPE — keep the `Error*` out-param, behavior-preserving.

---

## File Structure

- **Create (T1):** `src/deluge/storage/audio/stream/sample_stream.{h,cpp}` — `deluge::audio::stream::SampleStream`. Ends up owning (by T5) the residency table, read stream, Asset id + callbacks, `get_cluster` dispatch, `ReadSource` selection.
- **Modify:** `src/deluge/model/sample/sample.{h,cpp}` — hold `SampleStream stream_` + `Sample::stream()` (T1); relocate `readStream_`/`resourceAssetId`/asset-callbacks/`ensureResourceAsset` into `SampleStream` (T1); `clusters` stays public through the migration and is internalized in T5.
- **Modify:** `src/deluge/model/sample/sample_cluster.{h,cpp}` — `getCluster` becomes a forwarder (T2), deleted in T5; `SampleCluster` ends a passive entry (`sdAddress`, `cluster`, waveform min/max).
- **Modify (consumers, per the migration map — grouped by task):** T2 bulk (`sample.cpp` self-uses, `time_stretcher.cpp`, `waveform_renderer.cpp`, `wave_table.cpp`, `cluster_byte_source.cpp`, `read_source.cpp`, `audio_file_manager.cpp` stitch edges + sdAddress); T3 (`sample_recorder.cpp`); T4 (`sample_holder.cpp`, `sample_holder_for_voice.cpp`, `sample_low_level_reader.cpp`, `voice_sample.cpp`).

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

## Task 2: add the `SampleStream` API (coexisting) + migrate the bulk consumers

Introduce the full consumer surface — `get_cluster` + the table accessors (DD3) — **additively**, over the still-public `Sample::clusters`. Turn `SampleCluster::getCluster` into a thin forwarder so un-migrated callers keep compiling. Migrate every consumer EXCEPT the recorder (Task 3) and SampleHolder + RT reader (Task 4).

**Files:**
- Modify: `sample_stream.{h,cpp}` (add `StreamedChunk* get_cluster(uint32_t index, int32_t load_instruction = CLUSTER_ENQUEUE, uint32_t priority_rating = 0xFFFFFFFF, Error* error = nullptr)` — body = `sample_cluster.cpp:63-146` verbatim, `sample->` → `sample_.`, `this->cluster` → `sample_.clusters[index].cluster`; add `chunk_at`, `entry` (×const), `sd_address_at`/`set_sd_address_at`, `num_clusters`, `resize`, `erase_from`, `ensure_no_reason` — all over `sample_.clusters` this task; the accessors are one-liners into `sample_.clusters[index]`)
- Modify: `sample_cluster.{h,cpp}` (`SampleCluster::getCluster` body → `return sample->stream().get_cluster(clusterIndex, loadInstruction, priorityRating, error);` — a thin forwarder kept only for the not-yet-migrated Task-3/4 callers; `Sample::clusters` stays public)
- Modify (repoint the bulk non-recorder / non-holder / non-reader consumers): `sample.cpp` self-uses (`1474,1500` get_cluster; `715,937` `chunk_at`; `1151-1152` `num_clusters`; `1821,1914-1954` `chunk_at`; `102` `resize`); `time_stretcher.cpp` (`1142` get_cluster; `727` `chunk_at`); `waveform_renderer.cpp` (`407,433` get_cluster; `241` `num_clusters`; `382,432` `entry`); `wave_table.cpp:423` get_cluster; `cluster_byte_source.cpp:53` get_cluster; `read_source.cpp:23` (`BlockReadSource` → `sample_.stream().sd_address_at(i)`); `audio_file_manager.cpp` (`250,824` sdAddress via `sd_address_at`/`set_sd_address_at`; `924` consistency check via `chunk_at`; `1039,1050-1051` stitch neighbor-edge `chunk_at(idx±1)` + `num_clusters` bound)

**Interfaces:**
- Consumes: `Sample::stream()` (Task 1). Produces: `SampleStream::get_cluster` + all table accessors (DD3), consumed by Tasks 3–4.

- [ ] **Step 1:** Add a CppSpec (extend `tests/spec_audio_stream/`) for the stitch neighbor-edge path: `chunk_at(index±1)` returns the correct resident/null pointer over a mock table — guards the §6 boundary-conversion coupling. Run; verify it fails (no accessor yet).
- [ ] **Step 2:** Add `get_cluster` + the accessors + `ensure_no_reason` to `SampleStream` (bodies over `sample_.clusters`). Make `SampleCluster::getCluster` forward to `stream().get_cluster`.
- [ ] **Step 3:** Repoint the bulk-consumer sites in the list above. (Un-migrated recorder + holder + reader keep working via the forwarder / still-public `clusters`.)
- [ ] **Step 4:** Spec PASS; `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep` (Sample gains the `SampleStream` member — size changes; expect layout-invariant, re-baseline only on a deterministic shift with no behavior change).
- [ ] **Step 5:** Commit `refactor(audio-stream): add the SampleStream API; migrate the bulk consumers`.

## Task 3: dedicated recorder migration

Migrate every `sample_recorder.cpp` site onto the `SampleStream` API in one isolated, ear-checkable commit — the recorder is the heaviest and most behavior-sensitive consumer (§7, §10). Coexistence keeps this the only file changed.

**Files:**
- Modify (all recorder sites — map §3/§4): `sample_recorder.cpp` — `get_cluster`: `141,804,943,1262,1279,1297,1446,1494`; raw/entry access: `73,77,96,620,634,802,873,885,937,1218,1246,1383,1555,1624-1625` → `entry(i)` (waveform/`.cluster` peeks + `.sdAddress` writes), `sd_address_at`/`set_sd_address_at` (`1383,1555` disk-write sector target), `chunk_at(i)` (no-lease `.cluster` reads), `num_clusters()`/`resize()`/`erase_from()` (`937,1624-1625` table grow/shrink).

**Interfaces:**
- Consumes: the full `SampleStream` API (Task 2).

- [ ] **Step 1:** Transform each `sample->clusters[i].getCluster(sample, i, INSTR, prio, &err)` → `sample->stream().get_cluster(i, INSTR, prio, &err)`, and each raw `sample->clusters[i].<field>` → the matching accessor (`entry(i).<field>` for writes/waveform, `chunk_at(i)` for no-lease chunk reads, `sd_address_at`/`set_sd_address_at`, `num_clusters`/`resize`/`erase_from`).
- [ ] **Step 2:** Preserve the re-fetch-after-write pattern (`sample_recorder.cpp:873,885` re-takes the entry after a write because the audio routine may reallocate the table) — keep that reacquire, now through `entry(i)`. Carry `CLUSTER_DONT_LOAD` / dirty-pin / `numReasonsHeldBySampleRecorder` faithfully.
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact. **Ear-check note:** recording round-trip + `alterFile` reprocessing — §9 NEEDS-HARDWARE (recording); flag for the human's hardware pass before merge.
- [ ] **Step 4:** Commit `refactor(audio-stream): migrate SampleRecorder onto the SampleStream API`.

## Task 4: `SampleHolder` + RT reader lease/refill through `SampleStream`

Repoint the always-resident head/loop leasing and the RT reader's boundary-crossing refill onto the `SampleStream` API. Behavior-preserving; the reader keeps its own local `clusters[]` array (hot loop untouched — map §6).

**Files:**
- Modify: `sample_holder.cpp:218,220` (`((Sample*)audioFile)->clusters[clusterIndex].getCluster(...)` → `((Sample*)audioFile)->stream().get_cluster(clusterIndex, …)`); `sample_holder_for_voice.cpp:96` (`->clusters.size()` → `->stream().num_clusters()`)
- Modify: `sample_low_level_reader.cpp:304,380` (refill get_cluster → `stream()`); `:108` raw peek → `stream().chunk_at(...)`
- Modify: `voice_sample.cpp:203,847` (get_cluster → `stream()`)

**Interfaces:**
- Consumes: `SampleStream::get_cluster`/`num_clusters`/`chunk_at` (Task 2).

- [ ] **Step 1:** Repoint the SampleHolder direct-index leasing (`claimClusterReasonsForMarker`:218-220, the `clusters.size() <= 4` short-sample fallback:96) onto `stream()`.
- [ ] **Step 2:** Repoint the RT reader + VoiceSample refill points; confirm every `Sample::clusters` touch now goes through `stream()` and lives only at boundary-crossing/refill sites (`assignClusters`, `moveOnToNextCluster`, `attemptLateSampleStart`, cache-resync, the `:108` peek) — NOT in the per-sample inner loop (which uses the reader's own local array). Add a code comment at the refill sites noting the one-hop-per-boundary cost is intentional.
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact. **Ear-check note:** streaming-under-load + loop-start leasing — play a looping, streamed (non-fully-resident) sample and confirm no dropouts (§9 NEEDS-HARDWARE: streaming-under-load).
- [ ] **Step 4:** Commit `refactor(audio-stream): SampleHolder + RT reader lease through SampleStream`.

## Task 5: cleanup — internalize the table; delete the migration shims

Every consumer now goes through `stream()`. Physically move the residency table into `SampleStream` and delete the coexistence shims. Contained: only `SampleStream` (and `Sample`'s own construction/destruction) still names the table.

**Files:**
- Modify: `sample_stream.{h,cpp}` (own `deluge::fast_vector<SampleCluster> table_;`; repoint the accessors + `get_cluster` + the asset callbacks + `ensure_no_reason` from `sample_.clusters` → `table_`)
- Modify: `sample.{h,cpp}` (delete `clusters`:157; `initialize`:102 already routes through `stream_.resize` from Task 2 — confirm; `~Sample`/`markAsUnloadable`/`convertDataOnAnyClustersIfNecessary` reach the table only via `stream_` now)
- Modify: `sample_cluster.{h,cpp}` (delete the `SampleCluster::getCluster` forwarder:50-51 + `ensureNoReason`:52 now that `SampleStream` owns them; `SampleCluster` is a passive entry — `sdAddress`, `cluster`, waveform min/max)

**Interfaces:**
- Consumes: everything from Tasks 1–4. Produces: the final shape — `SampleStream` owns `table_`; `SampleCluster` is a passive entry.

- [ ] **Step 1:** Move `table_` into `SampleStream`; flip every internal `sample_.clusters` reference to `table_`.
- [ ] **Step 2:** Delete `Sample::clusters` and the `SampleCluster::getCluster` forwarder + `ensureNoReason`. Build — the compiler confirms nothing outside `SampleStream`/`Sample`-construction still names the table (any hit is a missed Task-2/3/4 migration; fix by routing through `stream()`).
- [ ] **Step 3:** `dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep` (Sample/SampleCluster sizes settle — expect layout-invariant).
- [ ] **Step 4:** Commit `refactor(audio-stream): internalize the residency table into SampleStream; drop migration shims`.

---

## Verification (whole increment)

- Per-task: `dbt build Debug` + `./dbt test` (20/20) + cordae/icoustic bit-exact; `padsweep` on the struct-size-changing tasks (T2: Sample gains the `SampleStream` member; T5: table internalized, `SampleCluster`/`Sample` sizes settle).
- The reconstruction-core specs (`tests/spec_audio_stream/`, `tests/qemu/spec/`) stay green — `SampleStream` only relocates the orchestration around the already-pure core.
- Coexistence keeps EVERY intermediate commit build-green and golden-bit-exact; the final shape isn't reached until T5.
- **NEEDS-HARDWARE before merge** (§9): recording round-trip + `alterFile` (T3), memory-pressure eviction, streaming-under-load (looping non-resident sample, T4), non-native-format (24-bit) sample load. Hardware gating is the user's call — do not treat it as a blocking next step.

## After Phase 4

Phase 5 (design-spec §8 step 4): consolidate the `loader` pump into the module + retire the `AudioFileManager` streaming methods (`loadCluster`, `readClusterData`, `loadAnyEnqueuedClusters`, `removeReasonFromCluster`, `loadingQueueHasAnyLowestPriorityElements`, the `clusterBeingLoaded` sentinel). Then follow-ons A (decouple `ComputedChunk` sizing from FAT `Cluster::size`) + B (`RecordingBacking`: `SampleStream` owns the write stream + physical-address bookkeeping, completing the `ReadSource` symmetry), plus the deferred Phase-3 dead-code cleanup (`resource_lease_asset_id()`, `ComputedChunk::sampleCache`) and the batched test-hygiene ticket.

## Self-Review notes (author)

- Spec coverage: §4 (SampleStream shape) → all 5 tasks; §6 (ReadSource selection ownership) → T1; §7 (getCluster dispatch, RT contract) → T2/T4; §8 step 3 → the whole plan. §6 stitch coupling is guarded by the T2 `chunk_at` spec.
- **Decisions settled with the human (2026-07-16 review):** DD1 home = `storage/audio/stream/` (spec §4). Recorder = its own dedicated task (T3). Residency-table migration kept coherent (introduced once in T2, internalized once in T5 — not split self-vs-external). Coexistence adopted to satisfy dedicated-recorder + all-green-builds simultaneously.
- Residual open item for the second review: whether the T2 bulk-consumer group is itself too large (it carries the stitch/afm + timestretch + waveform + wavetable + header-parse repoints in one commit) — could split the afm stitch-edge repoint (the §6/§10 risk area) into its own gate if desired.
