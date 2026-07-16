# Audio Stream Module — Phase 5: consolidate the loader + retire AudioFileManager streaming — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the per-cluster reconstruction and the loader pump into the `deluge::audio::stream` module, and retire the streaming methods (and the `clusterBeingLoaded` sentinel) from `AudioFileManager`, leaving it owning only identity/library concerns — the final core step of the audio-stream module refactor (design-spec §7 / §8 step 4).

**Architecture:** `readClusterData` becomes `SampleStream::read_cluster_data` (per-`Sample` reconstruction — it already reaches `sample->stream()` for the read source and the boundary-stitch neighbour edges). A new module-level `loader` owns the queue pump (today's `AudioFileManager::loadAnyEnqueuedClusters`), driving `read_cluster_data` across all streams over the existing `deluge_resource_loader_*` C ABI. The lease-drop `removeReasonFromCluster` moves to `deluge::cluster::remove_reason` alongside the other lease free functions. The legacy `loadCluster` + `clusterBeingLoaded` re-entrancy sentinel are retired.

**Tech Stack:** C++23 in-tree; `deluge::io::Stream` I/O; the `deluge_resource` C ABI (`loader_next`/`loader_enqueue`/`loader_remove`/`loader_has_lowest`/`mark_ready`); the already-pure reconstruction core (`convert_data_if_necessary`, `stitch_boundaries`); CppSpec + golden sweep + `./dbt build Debug`.

## Global Constraints

- **NOT strictly bit-exact-gated (§8).** Steps that only relocate code (Tasks 1–3) should stay cordae + icoustic bit-exact and must be treated as regressions if they diverge. **Task 4 (retiring `loadCluster` + the sentinel) is behaviour-touching and ear-check + hardware gated** — run the goldens as a regression signal, but a *documented, understood* divergence there is resolved by ear-check, not blocked. `highsiderr` is known-stale — verify via git-stash A/B, never block.
- **RT contract preserved (§7).** The render thread never does I/O; the pump keeps the current cooperative model (no background thread introduced). The pump's re-entrancy behaviour under the cooperative convert-yield must be preserved or deliberately, understandably changed (Task 4).
- **House style (`.clang-tidy`):** NamespaceCase `lower_case`, Class/Struct `CamelCase`, members `lower_case` + private `_`, Function/Method `lower_case`; new identifiers snake_case; idiomatic C++23. Doxygen per the house `///` + `@`-style convention.
- **The `deluge_resource` C ABI is the seam** — the module `loader` pumps it directly; no new C ABI introduced.
- Commit prefix `refactor(audio-stream):`. clang-format/ruff pre-commit reformat → recover with a FRESH `git commit`, never `--amend`.
- **Hardware ear-checks owed before merge** (Kate's gate, don't nag): streaming-under-load (looping non-resident sample), recording round-trip, offline/headless render (the `StemExport::renderWait` pump path), memory-pressure eviction, non-native-format (24-bit) load.

## Design decisions (from the spec + the Phase-5 surface map — FLAGGED for review; a reviewer/human may veto any)

- **DD1 — `readClusterData` home.** → `SampleStream::read_cluster_data(StreamedChunk&, int32_t min_reasons_after)`. It is per-`Sample` reconstruction and already reaches `sample->stream()` for `make_read_source()` and `chunk_at(idx±1)` (neighbour edges). Its 4 callers: the two `SampleStream` self-callers (`cluster_materialize`, `get_cluster`) become direct calls; the two `AudioFileManager`/loader callers go through `sample->stream()`.
- **DD2 — the `loader` shape.** Free functions in `namespace deluge::audio::stream` in a new `loader.{h,cpp}` — `void pump(int32_t max_num = 128, bool may_process_user_actions = false);` and `bool has_lowest_priority_queued();`. Not a class: the pump is a stateless global over the single resource manager (the only "state" — `REPORT_AWAY_TIME` timers — is compile-disabled). It reads card-readiness from `AudioFileManager` via a minimal accessor (DD5).
- **DD3 — `removeReasonFromCluster` home.** → `deluge::cluster::remove_reason(StreamedChunk&, char const* error_code)` + a `ComputedChunk&` overload, next to `add_lease`/`release_lease`/`lease_count`/`free_chunk` in `cluster.{h,cpp}` (it *is* lease bookkeeping — the ALPHA freeze-check + `release_lease`). The `deletingSong` param is unused today (`(void)`); drop it unless a caller relies on it (map shows none do — verify). Retire the `AudioFileManager` overloads + the `removeReasonFromChunkImpl` file-static.
- **DD4 — retire `loadCluster` + `clusterBeingLoaded` + `minNumReasonsForClusterBeingLoaded`** as an isolated, ear-check-gated task. The pump already routes manager-owned clusters straight to `read_cluster_data`; every `Sample` is manager-owned (`ensure_resource_asset` has no legacy fallback), so the `else → loadCluster` branch is expected-dead — verify, then collapse the pump to always `read_cluster_data` and delete `loadCluster` + the sentinel. Rework the ALPHA-only bug-checks that read `clusterBeingLoaded` to discount an in-flight lease (`~SampleCluster` sample_cluster.cpp:30; `sample.cpp:1846,1860`). `minNumReasonsForClusterBeingLoaded` is write-only → deletes trivially.
- **DD5 — card state stays on `AudioFileManager`.** The pump's card guards (`currentlyAccessingCard`, `cardEjected`, `cardDisabled`, `StorageManager::checkSDInitialized()`) read AFM/storage lifecycle state that is out of scope to move. Add one minimal accessor (e.g. `bool AudioFileManager::cardReadyForClusterLoad() const`) that the loader calls; the `allowSomeUserActionsEvenWhenInCardRoutine` toggle around the per-cluster load stays as the loader sets/clears it (it's global `extern` state, not AFM-private).
- **DD6 — decomposition (5 tasks).** T1 move reconstruction → T2 move lease-drop → T3 introduce loader + move the pump (preserving `loadCluster`/sentinel) → T4 retire `loadCluster`/sentinel (ear-check) → T5 cleanup + boundary confirmation. T4 is isolated so the one behaviour-sensitive change is a single reviewable, ear-checkable commit.

---

## File Structure

- **Modify:** `src/deluge/storage/audio/stream/sample_stream.{h,cpp}` — gains `read_cluster_data` (from AFM); its self-callers become direct.
- **Create:** `src/deluge/storage/audio/stream/loader.{h,cpp}` — `deluge::audio::stream::loader::{pump, has_lowest_priority_queued}` (from AFM's `loadAnyEnqueuedClusters` + `loadingQueueHasAnyLowestPriorityElements`).
- **Modify:** `src/deluge/storage/cluster/cluster.{h,cpp}` — gains `deluge::cluster::remove_reason` overloads (from AFM).
- **Modify:** `src/deluge/storage/audio/audio_file_manager.{h,cpp}` — sheds `readClusterData`, `loadCluster`, `loadAnyEnqueuedClusters`, `removeReasonFromCluster`(+impl), `loadingQueueHasAnyLowestPriorityElements`, `clusterBeingLoaded`, `minNumReasonsForClusterBeingLoaded`; gains the minimal `cardReadyForClusterLoad` accessor (DD5); keeps identity/library + the `disk_read`/`disk_write` hooks (which now call the module loader).
- **Modify (repoint call sites — exact lists in the Phase-5 surface map):** the ~13 pump callers (`disk_read`/`disk_write`, `deluge.cpp:540`, `audio_engine.cpp:388,1119`, `stem_export.cpp:220`, `browser.cpp`, `sample_browser.cpp`, `load_song_ui.cpp`, `instrument_clip_view.cpp`, AFM-internal `getUnusedAudioRecordingFilePath`); the 55 `removeReasonFromCluster` callers; the 1 `loadingQueueHasAnyLowestPriorityElements` caller (`load_song_ui.cpp:478`); the ALPHA `clusterBeingLoaded` readers (Task 4).

---

## Task 1: move `readClusterData` → `SampleStream::read_cluster_data`

Relocate the per-cluster reconstruction onto `SampleStream` (behaviour-preserving; bit-exact expected).

**Files:**
- Modify: `sample_stream.{h,cpp}` — add `bool read_cluster_data(StreamedChunk& cluster, int32_t min_reasons_after);`, body moved verbatim from `audio_file_manager.cpp:927-1068`. Inside `SampleStream` it already has `sample_`, `make_read_source()`, `chunk_at()` — rebase `sample->stream().make_read_source()`→`make_read_source()`, `sample->stream().chunk_at(...)`→`chunk_at(...)`, `cluster.sample`→still `cluster.sample` (the chunk carries its own back-pointer; the neighbour lookups use `chunk_at` on THIS stream since the cluster belongs to this sample). Keep the geometry, ALPHA checks, convert, `stitch_boundaries`, `mark_ready` steps identical.
- Modify: `sample_stream.cpp` — `cluster_materialize` (`:45`) and `get_cluster` (`:209`) call `read_cluster_data(...)` directly instead of `audioFileManager.readClusterData(...)`.
- Modify: `audio_file_manager.cpp` — the two AFM callers that survive this task (`loadCluster:903`, `loadAnyEnqueuedClusters:1179`) call `cluster.sample->stream().read_cluster_data(cluster, ...)`. Delete `AudioFileManager::readClusterData` (decl `.h:95`, def `.cpp:927-1068`).

**Interfaces:**
- Produces: `SampleStream::read_cluster_data`. Consumes: existing `make_read_source`/`chunk_at`, `stitch_boundaries`, `convert_data_if_necessary`.

- [ ] **Step 1:** Add `read_cluster_data` to `SampleStream` (body moved + rebased); add the `///`-Doxygen brief (per house style — `@brief`, `@param`, `@return`).
- [ ] **Step 2:** Repoint the 2 self-callers + the 2 AFM callers; delete `AudioFileManager::readClusterData`.
- [ ] **Step 3:** `./dbt build Debug` clean; `./dbt test` (20/20); `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` (bit-exact); `padsweep`.
- [ ] **Step 4:** Commit `refactor(audio-stream): move readClusterData onto SampleStream::read_cluster_data`.

## Task 2: move `removeReasonFromCluster` → `deluge::cluster::remove_reason`

Relocate the lease-drop to the lease-bookkeeping home; repoint all 55 call sites (bit-exact expected).

**Files:**
- Modify: `cluster.{h,cpp}` — add to `namespace deluge::cluster`: `void remove_reason(StreamedChunk& chunk, char const* error_code);` + `void remove_reason(ComputedChunk& chunk, char const* error_code);`, each forwarding to a shared `remove_reason_impl(void* chunk, uint32_t resource_slot, char const* error_code)` (body = the current `removeReasonFromChunkImpl`: ALPHA `lease_count(resource_slot) == 0` → `FREEZE_WITH_ERROR(error_code)`, then `release_lease(chunk)`).
- Modify: `audio_file_manager.{h,cpp}` — delete the two `removeReasonFromCluster` overloads (`.h:100-101`, `.cpp:1233-1241`) + `removeReasonFromChunkImpl` (`.cpp:1226-1231`).
- Modify (repoint all 55 sites, per the map §5): `audio_file_manager.cpp:906` (→ within `loadCluster`, still present this task); `time_stretcher.cpp:182,191,1119`; `waveform_renderer.cpp:445,552,554`; `sample_holder.cpp:70,240`; `sample_holder_for_voice.cpp:48,58,107`; `sample_low_level_reader.cpp:34,339,1215`; `sample_recorder.cpp` (15 sites); `sample.cpp:1422,1456,1509,1511,1749`; `cluster_byte_source.cpp:29,50`; `wave_table.cpp:420,768`. Transform `audioFileManager.removeReasonFromCluster(X, "code")` → `deluge::cluster::remove_reason(X, "code")` (drop the unused `deletingSong` third arg — verify no caller passes a non-default value).

**Interfaces:**
- Consumes: `deluge::cluster::{lease_count,release_lease}` (already present). Produces: `deluge::cluster::remove_reason`.

- [ ] **Step 1:** Add the `remove_reason` overloads + impl to `deluge::cluster` (with Doxygen). Confirm `cluster.h` is already included at every call site (they all use `Cluster`/chunk types — spot-check; add includes only if genuinely missing).
- [ ] **Step 2:** Repoint all 55 sites; delete the AFM overloads + impl. (The compiler catches a miss once the AFM members are gone.)
- [ ] **Step 3:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep`.
- [ ] **Step 4:** Commit `refactor(audio-stream): move removeReasonFromCluster to deluge::cluster::remove_reason`.

## Task 3: introduce the module `loader`; move the pump

Create `deluge::audio::stream::loader` and move `loadAnyEnqueuedClusters` + `loadingQueueHasAnyLowestPriorityElements` into it, repointing all callers. Preserve `loadCluster` + the `clusterBeingLoaded` sentinel (Task 4 retires them) — the pump keeps both branches this task.

**Files:**
- Create: `loader.{h,cpp}` — `namespace deluge::audio::stream::loader`: `void pump(int32_t max_num = 128, bool may_process_user_actions = false);` (body = `loadAnyEnqueuedClusters`, `audio_file_manager.cpp:1103-1218`, rebased); `bool has_lowest_priority_queued();` (= `deluge_resource_loader_has_lowest(...)`). The pump's per-cluster branch this task: manager-owned → `cluster->sample->stream().read_cluster_data(*cluster, 0)`; else → still `audioFileManager.loadCluster(*cluster)` (temporary reach-back, removed in Task 4). Card guards via `audioFileManager.cardReadyForClusterLoad()` (DD5).
- Modify: `audio_file_manager.{h,cpp}` — add `bool cardReadyForClusterLoad() const` (wraps `!currentlyAccessingCard && !cardEjected && !cardDisabled && StorageManager::checkSDInitialized()`, matching the pump's current early-outs). Delete `loadAnyEnqueuedClusters` (`.h:96`, `.cpp:1103-1218`) + `loadingQueueHasAnyLowestPriorityElements` (`.h:110`, `.cpp:1243-1245`). Keep `loadCluster` + the sentinel.
- Modify: `audio_file_manager.cpp` — the `disk_read`/`disk_write` hooks (`:67-85`) call `deluge::audio::stream::loader::pump()`.
- Modify (repoint the other pump callers, map §3/§7): `deluge.cpp:540`; `audio_engine.cpp:388,1119`; `stem_export.cpp:220`; `browser.cpp:269`; `sample_browser.cpp:1228`; `load_song_ui.cpp:529`; `instrument_clip_view.cpp:2119`; AFM-internal `getUnusedAudioRecordingFilePath:328`. And the 1 `has_lowest` caller: `load_song_ui.cpp:478` → `deluge::audio::stream::loader::has_lowest_priority_queued()`.

**Interfaces:**
- Consumes: `SampleStream::read_cluster_data` (T1), `AudioFileManager::{loadCluster,cardReadyForClusterLoad}`, the `deluge_resource_loader_*` ABI. Produces: `loader::{pump,has_lowest_priority_queued}`.

- [ ] **Step 1:** Add `cardReadyForClusterLoad()` to AFM. Create `loader.{h,cpp}` with `pump` (body rebased, both branches preserved) + `has_lowest_priority_queued`. Doxygen the module + functions.
- [ ] **Step 2:** Repoint the ~13 pump callers + the 1 has-lowest caller; delete AFM's `loadAnyEnqueuedClusters` + `loadingQueueHasAnyLowestPriorityElements`.
- [ ] **Step 3:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep`. **Ear-check note:** the pump drives all SD streaming — after goldens, a streaming-under-load + offline-render (StemExport) ear-check is owed before merge (§9).
- [ ] **Step 4:** Commit `refactor(audio-stream): move the loader pump into deluge::audio::stream::loader`.

## Task 4: retire `loadCluster` + the `clusterBeingLoaded` sentinel — EAR-CHECK GATED

The behaviour-touching task: prove the legacy `loadCluster` branch dead, collapse the pump, delete `loadCluster` + the sentinel, and rework the ALPHA in-flight-lease bug-checks. Per §8 this is ear-check + hardware gated, not bit-exact-gated.

**Files:**
- Modify: `loader.cpp` — collapse the pump's per-cluster branch to always `cluster->sample->stream().read_cluster_data(*cluster, 0)` (the `else → loadCluster` arm removed, once verified dead).
- Modify: `audio_file_manager.{h,cpp}` — delete `loadCluster` (`.h:91`, `.cpp:867-920`), `clusterBeingLoaded` (`.h:133`), `minNumReasonsForClusterBeingLoaded` (`.h:134`) + its `init()` reset (`.cpp:148`).
- Modify (rework the ALPHA `clusterBeingLoaded` readers, map §6): `sample_cluster.cpp:30` (`~SampleCluster` discounts one lease if `== clusterBeingLoaded`); `sample.cpp:1846,1860` (reason-count dump/check + the "(loading)" printout). With the sentinel gone and the pump doing `read_cluster_data` directly (already leased via `request`, no separate load-lease), rework these checks so they neither over- nor under-count — analyze the new invariant and adjust (or remove the discount if it's no longer meaningful).

- [ ] **Step 1:** **Verify the dead branch.** Confirm every cluster reaching the pump has `sample != nullptr && sample->stream().resource_asset_id() != NO_ASSET` (every Sample is manager-owned via `ensure_resource_asset`, no legacy fallback). Document the reasoning; if a live path can still enqueue a non-manager cluster, STOP and escalate — the retirement premise fails.
- [ ] **Step 2:** Collapse the pump branch; delete `loadCluster` + the sentinel + its reset. Rework the ALPHA `clusterBeingLoaded` bug-checks (Step-1 invariant tells you the correct in-flight-lease accounting).
- [ ] **Step 3:** `./dbt build Debug` clean; `./dbt test`; `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` + `padsweep`. **These are a regression signal, not a hard gate** — if a fixture diverges, analyze whether it's the intended re-entrancy/accounting change (ear-check-resolve) or an unintended regression (fix). Document the outcome. **Ear-check owed:** streaming-under-load, recording, offline render — the sentinel guarded re-entrancy during the cooperative convert-yield; confirm on hardware that removing it doesn't cause dropouts/corruption under load.
- [ ] **Step 4:** Commit `refactor(audio-stream): retire loadCluster + the clusterBeingLoaded sentinel`.

## Task 5: cleanup + confirm the AudioFileManager boundary

Confirm `AudioFileManager` is now identity/library only; tidy includes and dead references.

**Files:**
- Modify: `audio_file_manager.{h,cpp}` — remove now-unused includes/forward-decls left by the moved methods; confirm the remaining public surface is the identity/library set (map §9). Confirm `StreamedChunk`/`ComputedChunk`/streaming headers are only included where still needed.
- Modify: any file left with a stale include of `audio_file_manager.h` that only needed the moved streaming methods → repoint to `loader.h`/`sample_stream.h`/`cluster.h` (the compiler + a grep for `audioFileManager.` residue guide this).

- [ ] **Step 1:** Grep the tree for residual `audioFileManager.readClusterData`/`loadCluster`/`loadAnyEnqueuedClusters`/`removeReasonFromCluster`/`loadingQueueHasAnyLowestPriorityElements`/`clusterBeingLoaded` — expect ZERO. Tidy includes.
- [ ] **Step 2:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) + `padsweep`.
- [ ] **Step 3:** Commit `refactor(audio-stream): confirm AudioFileManager identity/library boundary; tidy includes`.

---

## Verification (whole increment)

- Per-task: `./dbt build Debug` + `./dbt test` (20/20) + goldens + `padsweep`. Tasks 1–3 + 5 target bit-exact; **Task 4 is ear-check-gated** (goldens are a signal).
- The reconstruction-core specs (`tests/spec_audio_stream/`, `tests/qemu/spec/`) stay green — the core is untouched; only its invocation relocates.
- **NEEDS-HARDWARE before merge** (§9, Kate's gate — don't nag): streaming-under-load (looping non-resident sample), recording round-trip, offline/headless render (`StemExport` pump path), memory-pressure eviction, non-native (24-bit) load. Task 4's re-entrancy change is the highest-value hardware check.

## After Phase 5

The design-spec §8 migration is complete. Remaining: follow-on A (decouple `ComputedChunk` sizing from FAT `Cluster::size`), follow-on B (`RecordingBacking`: `SampleStream` owns the write stream + physical-address bookkeeping, completing the `ReadSource` symmetry), the deferred Phase-3 dead-code cleanup (`resource_lease_asset_id`, `ComputedChunk::sampleCache`), and the batched test-hygiene ticket.

## Self-Review notes (author)

- Spec coverage: §7 (loader pump keeps cooperative model, RT contract) → T3/T4; §8 step 4 (retire the named AFM methods + sentinel) → T1–T5. The "reconstruction core stays pure" property is preserved (only the invocation moves).
- Open risks to surface at review: (a) **Task 4's sentinel retirement** is the real behaviour change — the `clusterBeingLoaded` guard prevents re-entering the pump/`loadCluster` during the cooperative convert-yield; the manager-owned fast path already runs without it, so retirement should be safe, but this is the ear-check crux. (b) The ALPHA in-flight-lease accounting rework (Step 4.2) needs the exact new invariant — if unclear, keep the checks conservative or remove them with a note rather than guess. (c) DD5's `cardReadyForClusterLoad` must reproduce the pump's early-outs *exactly*, including the `performActionsAndGetOut` `slowRoutine()` path — confirm the card-down behaviour (not just the boolean) is preserved when the loader moves. (d) `disk_read`/`disk_write` are FatFS link-time symbols that must stay `extern "C"` in `audio_file_manager.cpp` (or move with a matching contract) — keep them where FatFs finds them; only their *body* calls the module loader.
