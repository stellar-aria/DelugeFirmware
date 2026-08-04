# File-load worker-dispatch rung — design (B6 completion)

**Status:** IMPLEMENTED & software-complete (2026-07-22) — see `docs/dev/known-concurrency-bugs.md` B6 for the commit list and verification state. This doc is the design of record.
**Base:** `feat/rustfs-sp1b-cached-chain` (SP-stream-read Phase 1 landed: `Owner::on_owner()`/
`run_or_inline()`, sample-streaming + sample-browser B6 fixes).
**Closes:** the `AudioFileHolder::loadFile` half of **B6** (`docs/dev/known-concurrency-bugs.md`) — the
seven off-worker chains Phase 1's Task 4 investigation uncovered
(`.superpowers/sdd/task-4-report.md`).
**North-star:** `[[ui-storage-decoupling-northstar]]` — precise principle: *the UI never **blocks** on
storage*; a user-initiated load runs in the storage domain and publishes its result back.

## 1. Problem

Seven UI/model call chains reach `AudioFileManager::getAudioFileFromFilename` →
`buildAudioFileFromCard` → the synchronous `CLUSTER_LOAD_IMMEDIATELY` sites 6/7
(`wave_table.cpp:422`, `cluster_byte_source.cpp:51`) with **no storage-worker dispatch above them**.
Each blocks the executor on a cold-cache FatFS load and would trip the `sd.rs:386` single-owner audit
once the owner is up. Full inventory (from the Task 4 audit) in the B6 record. The SP-stream-read §6
site list under-counted B6; these were missed.

## 2. Architecture — no single choke-point; four proven patterns

**The single choke-point is impossible.** `getAudioFileFromFilename` returns `AudioFile*` *and* writes
`*error` synchronously — every caller needs both. A fire-and-forget dispatch there breaks all callers.
Dispatching at the choke point is only viable by restructuring every caller for async completion —
which is the same work as per-call-site. So dispatch is **per-interaction-site**, same conclusion
Phase 1 reached.

The seven chains collapse to **four interaction patterns, each with an in-tree precedent** — this rung
introduces no new mechanism:

| Pattern | Chains | Precedent |
|---|---|---|
| Latest-wins coalesced preview | preset-browser scroll (#10/#11), Slicer preview (#3) | `SampleBrowser::previewCoalescer_` + `runPreviewOp` |
| Context-menu confirm (always-`true` + close/finish-in-op) | ClearSong (#12) | Phase 1 Task 3 (Synth/Kit `acceptCurrentOption`) |
| Whole-gesture dispatch (provisional return) | Slicer doSlice (#4/#5), Drum Randomizer (#9) | Phase 1 Task 3 (`runClaimCurrentFileOp` thunk) |
| Background whole-op dispatch | Undo/Redo (#6) | `LoadSongUI::performLoad` via `Owner::run` in `slowRoutine` |

**Principle (interim vs end-state differs per site):** the UI still triggers the load, but it runs on
the worker (where blocking is safe). For most sites this is the interim step toward the north-star
(UI reads model, doesn't trigger). **The preset browser is the exception — it is the END-STATE** (§3):
preview *is* auditioning (press pads, hear the real sound), so the full load is intrinsic to the
interaction and cannot be designed away. Only its blocking is the bug; async-and-coalesced is the
permanent fix.

## 3. The preset browser (#10/#11) — the structural site

Port `SampleBrowser`'s proven preview architecture to `LoadInstrumentPresetUI`; the interaction is the
same shape (scroll-to-preview, commit-to-select).

- **Scroll (`currentFileChanged`):** build a load target (selected file + `loadingSynthToKitRow`), hand
  it to a new `loadCoalescer_` (`LatestWins<LoadTarget>`, the type SampleBrowser's `previewCoalescer_`
  uses); if newly in-flight, `Owner::run(&runLoadOp, this)`. `runLoadOp` runs
  `performLoad()`/`performLoadSynthToKit()` for the *current* target on the worker, stores
  `currentInstrumentLoadError`, renders on completion (audition pads / `displayError`), then
  `complete()` — re-dispatching if a newer scroll arrived.
- **Commit (`enterKeyPress`, SELECT_ENC):** today reads `currentInstrumentLoadError` synchronously right
  after a load to decide retry/displayError/continue. Under coalescing the scroll-load may still be in
  flight, so this becomes a dispatched commit op (Task 3's pattern): dispatch "commit the current
  selection" (loads if not already resident — `getAudioFileFromFilename` caches, so a scroll that loaded
  it is a hit), handle the error inside the op; `enterKeyPress` returns without a synchronous bool.

Two design points a naive port gets wrong:
1. **`UI_MODE_LOADING_BUT_ABORT_IF_SELECT_ENCODER_TURNED` maps onto `LatestWins`.** "Abort the load if
   the user scrolls again" *is* latest-wins — a new target supersedes the in-flight one. The coalescer
   preserves the abort-on-scroll intent; the ad-hoc mode flag largely dissolves into coalescer state.
2. **Preview here is load-to-ready, not auto-audition** — it loads the preset so its pads *can* sound
   when pressed; it does not auto-play. Simpler than SampleBrowser's preview (no audio to coalesce),
   just load + render-pads on completion.

`LoadInstrumentPresetUI` gains a coalescer field + a `runLoadOp`/commit-op pair, structurally cloned
from `previewCoalescer_`/`runPreviewOp`. This is the one file with real new structure — a port.

## 4. The other five chains

Applications of §2's patterns; per-site wrinkle noted.

- **Slicer preview (#3)** — coalesced-preview (mirror §3). `loadFile`'s result is discarded, but the
  next lines (`sendAuditionNote`) assume the audio is resident — so the load AND the audition-note move
  into the op together, not just the `loadFile` call.
- **Slicer doSlice (#4/#5)** — whole-gesture dispatch. `doSlice` loads then reads
  `audioFile`/`lengthInSamples` right after, and the manual-slice branch iterates the drums it created;
  the whole gesture (`doSlice` + that post-loop) moves into one op. No cross-boundary return value → no
  contract decision, just "move the whole thing in together."
- **Undo/Redo (#6)** — background whole-op dispatch, the cleanest. `PlaybackHandler::slowRoutine`
  already runs on the executor; nothing consumes `undo()`/`redo()`'s result. Wrap the whole
  pending-command handling in `run_or_inline`. `resumePlayback`'s dependency on the loaded sample is
  inside `revert()`, so it travels automatically.
- **Drum Randomizer (#9)** — whole-gesture dispatch, smaller blast radius (rare, feature-gated pad
  gesture). `padAction` checks the returned `ActionResult`; `drumName`/`beenEdited` are set right after
  the load; move the whole randomize step into the op and return a provisional `ActionResult` (same
  shape as Task 3's provisional `bool`).
- **ClearSong (#12)** — Task 3's context-menu pattern on a third `ContextMenu` subclass:
  `acceptCurrentOption` always returns `true`; the op does the load + `close()`-on-failure. Difference
  from Synth/Kit: ClearSong's success/failure UI teardown (`setUIForLoadedSong`) moves into the op.

No new mechanism — the provisional-`ActionResult` returns (#4/#5, #9) are the provisional-`bool` shape
from Task 3.

## 5. Verification

- **Audio is byte-identical** — these are UI-scheduling changes; the loads produce the same data on a
  different context. Golden is not the gate.
- **The B6 gate completes.** With all seven dispatched (plus Phase 1's), the `sd.rs:386` single-owner
  assert should stay silent across *every* UI load path — the whole B6 surface. This rung moves B6 from
  "partially fixed" to "fixed." Runtime silence is the on-device leg (Kate): build with
  `storage-owner-audit`, exercise the full UI (preset scroll, slicing, undo, clear-song, randomizer),
  require no assert.
- **Coalescer logic** rides on the existing `Coalescer`/`LatestWins` (`owner.h`, covered by
  `owner_spec`); the preset-browser port reuses it — build + that coverage, not new unit tests.
- **Per contract-change site** (preset commit, ClearSong, doSlice, drum randomizer): a written
  success/failure UX walk in each task's report (what closes/shows/commits on each outcome) — the check
  that caught the double-close risk in Phase 1 Task 3.
- **Builds:** `./dbt build Debug` (cooperative) + `cargo device` (Embassy) clean per task. `./dbt` is a
  repo-root script, not on PATH.

## 6. Sequencing

No cross-chain dependencies; coalescer infra already exists (Phase 1's `run_or_inline`/`on_owner`).
Order by value + template-reuse:

1. **Preset browser (#10/#11)** — highest traffic, most structure (the port); reference for #3.
2. **Slicer preview (#3)** — mirrors #1's coalesced-preview.
3. **ClearSong (#12)** — Task 3 pattern, quick, independent.
4. **Slicer doSlice (#4/#5)** — whole-gesture.
5. **Drum Randomizer (#9)** — whole-gesture, smallest.
6. **Undo/Redo (#6)** — background, cleanest, independent.
7. **B6 completion gate** — audit build + full-UI exercise (device, Kate).

## 7. Out of scope

- Eliminating UI-triggered loads (the deeper north-star) — for the preset browser this is explicitly
  NOT the goal (§2: the load is intrinsic to preview). For others (e.g. a preset *index* the UI reads
  instantly) it is a later, separate concern.
- The pitch-at-load model-ownership rung (deferred separately).
- Any audio/DSP change — this rung is UI scheduling only.
