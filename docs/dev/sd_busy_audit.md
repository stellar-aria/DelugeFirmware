# `sd_busy()` / `deluge_storage_fs_busy()` consumer audit

## Why this document exists

`deluge::sync::sd_busy()` used to be backed by `currentlyAccessingCard`, a flag with no writer since
the legacy diskio was deleted — so it was permanently `false`, and every one of its call sites had a
branch that had **never executed**, on host or on Embassy, since the flag went dead. This plan retargets
`sd_busy()` onto `deluge_storage_fs_busy()`, a libdeluge C-ABI seam that the Rust BSPs back with the real
efatfs FS-mutex state (`Mutex::try_lock().is_err()`). On Embassy that answer is now genuinely `true` for
the duration of one filesystem operation.

That flip activates nine dormant deferral/assertion branches at once. This document exercises the one
whose correctness the whole plan turns on (does the waveform overview scan still function, or did fixing
the deadlock just trade it for a silently dead feature?), then walks the other eight and classifies each.

**Scope note, stated once up front:** `deluge_storage_fs_busy()` is hardwired `false` on the
cooperative/C-host BSP (`task_scheduler_c_api.cpp`) — FatFS runs inline on the caller there, so there is
no FS mutex to report on. Every site below therefore behaves **exactly as it did before this change** on
that BSP. Everything in this document — the activation, the risk, the classifications — is Embassy-only.

## Step 0: does the overview scan still function?

### Why the existing gate cannot answer this

Task 3 fixed the deadlock by making `WaveformRenderer::advanceOverviewScan` defer (`return true`)
instead of issuing a synchronous read when `sd_busy()` is true. Task 3's own Lens 1 gate
(`cordae` fixture) went from `WEDGED` to `exit=0` with an unchanged `LENS1_RESULT` line. That is real
evidence the deadlock is gone, but it is **not** evidence the scan still works, because every field of
`LENS1_RESULT` is structurally blind to it:

- `scenario.rs:239-240` sets `cluster_reads`/`recorder_writes` from
  `sd::stats::on_fiber_reads()`/`on_fiber_writes()`.
- `sd.rs:548-558` (`note_read`/`note_write`) increments those counters **only if `on_fiber`** is true.
- The overview scan runs off-fiber by definition — that off-fiber-ness is the entire premise of the
  original deadlock — so any read it issues increments nothing these counters can see, whether the scan
  runs perfectly, stalls, or never runs at all.
- `blocks_rendered` is `audio_host::drive_count()`, a sim constant regardless.
- `underrun_wait`/`underrun_unassign` are audio-path counters, orthogonal to the scan.

So "deadlock fixed" and "feature silently dead" would produce byte-identical `LENS1_RESULT` output. The
structural argument (the guard's `return true` returns before `overviewScanAllDone = true` is reached,
and `overviewScanNextCluster` is untouched on that path, so the same cluster is simply retried with no
lost place) says deferral is lossless — but that is reading the code, not evidence it actually runs.

### The probe

Picked the brief's combined option: a temporary counter **and** a live check of
`sample->overviewCacheEntry(clusterIndex).investigated` at the moment of each successful
`investigateWholeCluster` call, printed to stderr. Added directly after the existing
`if (!investigateWholeCluster(sample, clusterIndex)) { return true; }` check in
`WaveformRenderer::advanceOverviewScan` (`src/deluge/gui/waveform/waveform_renderer.cpp`):

```cpp
// TEMPORARY (Task 6 Step 0 probe) -- remove before commit.
static uint32_t probeOverviewScanHits = 0;
std::fprintf(stderr, "PROBE_OVERVIEW_SCAN_HIT cluster=%d investigated=%d count=%u\n", clusterIndex,
             (int)sample->overviewCacheEntry(clusterIndex).investigated, ++probeOverviewScanHits);
```

(plus a temporary `#include <cstdio>`.) Rebuilt the full target list —

```
ninja -C build-embassy-hostapp deluge_app NE10 eyalroz_printf deluge_dsp deluge_scheduler deluge_foundation deluge_midi
```

— then rebuilt and ran the Lens 1 harness (`src/bsp/rust/lens1_vt_sim`) under a hard timeout, `stdout`
and `stderr` combined so the probe lines and the `LENS1_RESULT` line land in the same capture:

```
timeout 120 env LENS1_FIXTURE=cordae LENS1_BLOCKS=500 LENS1_WALL_TIMEOUT_S=45 \
  LENS1_VIRTUAL_BUDGET_MS=600000 ./target/debug/lens1-vt-sim > probe_run.log 2>&1
echo "exit=$?"
```

### Verbatim output

```
exit=0
```

First 5 and last 6 lines of `probe_run.log` (172 `PROBE_OVERVIEW_SCAN_HIT` lines total, all landing
before the final `LENS1_RESULT` line):

```
PROBE_OVERVIEW_SCAN_HIT cluster=0 investigated=1 count=1
PROBE_OVERVIEW_SCAN_HIT cluster=0 investigated=1 count=2
PROBE_OVERVIEW_SCAN_HIT cluster=0 investigated=1 count=3
PROBE_OVERVIEW_SCAN_HIT cluster=0 investigated=1 count=4
PROBE_OVERVIEW_SCAN_HIT cluster=0 investigated=1 count=5
...
PROBE_OVERVIEW_SCAN_HIT cluster=14 investigated=1 count=168
PROBE_OVERVIEW_SCAN_HIT cluster=14 investigated=1 count=169
PROBE_OVERVIEW_SCAN_HIT cluster=14 investigated=1 count=170
PROBE_OVERVIEW_SCAN_HIT cluster=14 investigated=1 count=171
PROBE_OVERVIEW_SCAN_HIT cluster=14 investigated=1 count=172
LENS1_RESULT blocks_rendered=0 cluster_reads=27 recorder_writes=144 underrun_wait=0 underrun_unassign=0
```

Distribution by `clusterIndex` (172 hits total, monotonically non-increasing as the index rises):

```
    19 cluster=0
    19 cluster=1
    16 cluster=2
    14 cluster=3
    13 cluster=4
    11 cluster=5
    11 cluster=6
    11 cluster=7
    10 cluster=8
     8 cluster=9
     8 cluster=10
     8 cluster=11
     8 cluster=12
     8 cluster=13
     8 cluster=14
```

Every single hit printed `investigated=1` — zero `investigated=0` lines (checked with
`grep -c "investigated=0"`, returned `0`).

### What this proves

- The scan **ran**, off-fiber, 172 separate times over the course of one Lens 1 run, and every one of
  those 172 calls **succeeded** — `investigateWholeCluster` returned `true` and the cache entry it wrote
  was observed `investigated == true` immediately afterward. This is the strongest available signal
  short of instrumenting the byte contents of the overview cache itself: it shows the cache entry the
  scan is responsible for populating actually flipped, at the moment the scan flipped it, for the exact
  cluster the scan was working on.
- The scan made **forward progress across many distinct clusters** (0 through 14) rather than getting
  stuck retrying one index. The monotonic decrease in hit-count as the cluster index rises is the
  signature of several audio files of different lengths being round-robined by
  `AudioFileManager::backgroundWaveformOverviewScan` — shorter files finish (drop out of the "still has
  work" rotation) before longer ones, which is exactly the shape you'd expect from `kOverviewScanClustersPerCall
  = 1` scanning ~19 files of varying length one cluster at a time. That shape is evidence of a
  functioning round-robin, not an artifact of a stuck cursor (a stuck cursor would show one cluster index
  with a runaway count and nothing after it).
- Zero `investigated=0` observations rules out the failure mode where the guard's deferral path is
  somehow taken *after* a call that looked successful but didn't actually persist the flag.
- This run took the synchronous `fill_now` branch, not the `async_streaming_loader` degrade
  (`streaming_loader.rs:172-181`) — `lens1_vt_sim` leaves that feature off, and a scan that never filled
  anything would show either zero probe lines or (if the guard were the culprit) probe lines that never
  advance past `cluster=0`. Neither happened, so the pre-existing device-side degrade is confirmed **not**
  live in this harness and did not confound this result, as the brief anticipated.
- Combined with the structural argument from Task 3 (the guard's `return true` is taken *before*
  `investigateWholeCluster` is called, so a deferral never touches `overviewScanNextCluster` and never
  produces a false `investigated=0`), this closes the loop: the code-reading argument said deferral is
  lossless, and this run is direct execution evidence that, deferral or not, the scan completes real work
  and keeps making progress across the whole fixture.

### What this does NOT prove

- It does not prove the scan reaches full completion (`overviewScanAllDone = true`) for every sample in
  the fixture — the run only needed to reach the target block count, not necessarily scan every cluster
  of every file to exhaustion. 172 successful investigations across ~19 files reaching at least cluster 14
  is strong partial-completion evidence, not a proof of exhaustive completion.
- It does not exercise the `deluge::sync::sd_busy() == true` deferral branch itself with direct evidence
  of a *retry* (i.e., a probe line for the same cluster appearing twice because a deferral happened in
  between) — this run's `sd_busy()` windows apparently didn't coincide with an in-progress scan attempt at
  a granularity this probe would catch as a repeat. That branch's correctness rests on the structural
  argument (untouched cursor) rather than on execution evidence from this specific run.
- It says nothing about hardware timing, real SD-card latency, or the on-device experience — this is a
  virtual-time host harness.

### Headline finding

**The scan still functions.** This is not a regression. 172 successful, cache-flipping investigations
across 15 distinct cluster indices and roughly 19 audio files were observed in one run, ruling out the
"deadlock fixed by starving the feature" failure mode this task exists to check for.

### Instrumentation removed

The temporary counter, the `fprintf` probe line, and the temporary `#include <cstdio>` were all removed
from `src/deluge/gui/waveform/waveform_renderer.cpp` before rebuilding and committing. `git diff` against
that file shows no residual change from this task (confirmed with `git status --short` after reverting,
and by rebuilding `deluge_app`/`deluge_loadcheck` clean afterward).

## Step 1–2: the nine sites

**Summary, as shipped by this task: 7 IMPROVED / 2 NEUTRAL / 0 REGRESSION.**

(Sites 1–6 and 8 are IMPROVED; sites 7 and 9 are NEUTRAL — site 7 because its code path is compiled out
by default, site 9 because Step 3 downgraded what would otherwise have been a regression, discussed
next.)

That tally already accounts for Step 3's fix. Stated the other way, because it is the more important
fact: **as activated by the flip, before this task's Step 3 judgement, one of the nine
(`save_song_ui.cpp:124`) would have been a REGRESSION** — a false `FREEZE_WITH_ERROR` on legitimate,
harmless concurrency, worse than the eternal no-op it replaced. This task's job was exactly to catch that
before it shipped, per Step 3 below, rather than let the flip alone decide it. With that fix applied, the
final, shipped tally has zero regressions.

No site's deferral/assertion branch had ever executed, on host or Embassy, before this change — every
one of the nine reads below the fold is being exercised for the first time by this plan.

---

### 1. `waveform_renderer.cpp:691` — the overview-scan guard (already migrated, Task 3)

**What it does when busy:** `advanceOverviewScan` returns `true` before issuing a synchronous cluster
load, deferring that cluster to the next call instead of racing the storage worker for the FS mutex.

**Classification: IMPROVED.** This is the deadlock fix itself. Before, the branch never fired (dead
flag), so the scan's synchronous off-owner read could — and, per the bug report that started this whole
plan, did — block on the efatfs FS mutex while a suspended worker fiber held it, with no task able to
ever resume that fiber. Now the branch fires correctly and defers instead of deadlocking. Step 0 above
independently confirms the deferred scan still gets its work done.

Migrated by commit `41745107e` (Task 3); not touched by this task beyond the temporary Step 0 probe,
which was reverted before commit.

### 2. `resource_checker.h:38` — the scheduler `RESOURCE_SD` ceiling (already migrated, Task 4)

**What it does when busy:** `ResourceChecker::checkResources()` treats `RESOURCE_SD` as locked, holding
off admission of any task that declares that resource.

**Classification: IMPROVED.** This priority-ceiling exists specifically to stop a task from locking one
resource and then yielding while it waits for another — a real hazard shape. Before, its input
(`currentlyAccessingCard`, then `deluge_storage_fs_busy()` before this whole flip) never went `true`, so
the ceiling was a permanent no-op: any task declaring `RESOURCE_SD` was always admitted regardless of
real FS state. Now it can genuinely hold off admission for the brief window a real FS operation is in
flight, which is exactly the protection the ceiling was written to provide. Per the brief's own framing
this is the widest-blast-radius site in the plan (task admission, not one call-site deferral) and has no
`isSDRoutineActive()`-style softening companion; Task 4's report flagged that explicitly and did not
narrow it, deferring the call to review. Nothing in this task's investigation surfaces a reason to
downgrade that IMPROVED classification to NEUTRAL or REGRESSION — "busy" is still bounded to one FS
operation's duration, so the ceiling's new hold-offs are brief by the same architectural guarantee that
bounds every other site here.

Migrated by commits `1c3295df3`/`a6447337b` (Task 4); not touched by this task.

### 3. `midi_engine.cpp:502-504` — defer SysEx/USB-MIDI handling

**What it does when busy:** `MidiEngine::checkIncomingUsbMidi()` returns immediately, skipping
`check_incoming_usb()` and the per-cable device servicing loop for this call, retried on the next tick.
The existing comment already calls this "a hack to avoid SysEx handlers clashing with other sd-card
activity" — i.e. this check was *written* to do exactly what it can now actually do.

**Classification: IMPROVED.** Before, USB-MIDI/SysEx servicing ran on every tick regardless of real SD
activity, so it could clash with concurrent card access — the precise hazard the comment names. Now it
genuinely defers during the brief window a real FS operation is in flight, and resumes on the very next
tick (this function is called from the main poll loop at high frequency, so a one-tick defer is not a
perceptible MIDI-timing hit). No counter-indication found that deferring USB-MIDI service for a single
FS-operation-length window causes data loss — the USB stack buffers received bytes at a lower layer
(`deluge_midi_service`), so a skipped poll doesn't drop input, only delays draining it by one tick.

### 4. `playback_handler.cpp:191` — defer a pending global MIDI undo/redo command

**What it does when busy:** `PlaybackHandler::slowRoutine()`'s guard is `pendingGlobalMIDICommand != NONE
&& !sd_busy()`; when busy, the whole body is skipped and `pendingGlobalMIDICommand` is left untouched
(it's only cleared inside the guarded body), so the pending command is retried, unlost, on the next tick.

**Classification: IMPROVED.** The dispatched op (`undo()`/`redo()`) can itself load a sample and, on
Embassy, can outlive the call that dispatched it onto the storage worker (documented in the surrounding
comment, referencing `known-concurrency-bugs.md` B6). Deferring dispatch while a real FS operation is
already in flight avoids adding a second concurrent FS consumer during that same brief window, which is
strictly safer than dispatching unconditionally as before. The retry is lossless by construction — the
flag is cleared only on the taken branch, matching the same "lossless deferral" shape verified for the
waveform guard in Step 0.

### 5. `audio_recorder.cpp:174` — defer recorder `slowRoutine` work

**What it does when busy:** `AudioRecorder::slowRoutine()`'s guard is `isSDRoutineActive() ||
sd_busy()`; when either is true, the whole routine (including the check that calls `finishRecording()`,
which frees the `SampleRecorder`) is skipped for this tick.

**Classification: IMPROVED, softened.** This is one of the two sites the brief calls out as already
paired with `isSDRoutineActive()`. The comment explains the underlying hazard precisely:
`finishRecording()` frees the recorder, and `discardRecorder()` (site 8 below) forbids doing that from
inside the SD card routine because the recorder may be suspended part-way through its own
`cardRoutine()` — freeing it there leaves that routine running on freed memory. `isSDRoutineActive()`
was already a real, live signal before this change (unrelated to the dead `currentlyAccessingCard`
flag), so this site already had partial protection. Adding a genuine `sd_busy()` extends coverage to the
FS-mutex-held window specifically, which `isSDRoutineActive()` does not by itself guarantee overlaps
with. Since the two signals track different mechanisms (a cooperative-routine flag vs. the efatfs
FS-mutex state) rather than being redundant, the real `sd_busy()` half is additive protection against the
same documented hazard, not a new hazard of its own.

### 6. `smsysex.cpp:909` — defer dispatching the front SysEx op

**What it does when busy:** `smSysex::handleNextSysEx()` returns before setting `g_sysex_op_in_flight`
and before dispatching `runSysexOp` onto the storage owner; the queued `SysExQ` entry is untouched, so
it's retried, unlost, on the next tick.

**Classification: IMPROVED.** Before, the front SysEx request was dispatched onto the storage owner
unconditionally. Now dispatch is briefly held back while the FS mutex is genuinely held elsewhere, adding
a small amount of backpressure before a FatFS-touching op is queued during a window when FatFS is
already busy. The `deluge::storage::Owner::run` dispatch mechanism would eventually serialize this either
way, so the marginal safety benefit here is smaller than at the recorder/overview-scan sites — but there
is no plausible starvation risk given "busy" is bounded to one brief FS operation, and the in-flight
guard/queue-front state is left completely untouched on the deferred path, so nothing is lost.

### 7. `sample_marker_editor.cpp:902` — throttle a debug-only marker-randomization routine

**What it does when busy:** `SampleMarkerEditor::graphicsRoutine()`'s `if (!sd_busy() && ...)` guards a
block that randomly nudges a sample's loop-start/end markers, purely for manual loop-point stress
testing.

**Classification: NEUTRAL.** This entire block is compiled only under `#ifdef TEST_SAMPLE_LOOP_POINTS`
(`src/deluge/gui/ui/sample_marker_editor.cpp:901`), and that macro is commented out by default
(`src/definitions_cxx.hpp:34`: `// #define TEST_SAMPLE_LOOP_POINTS 1`). In every build this repo actually
produces — including the Embassy builds this whole plan is about — this code does not compile in at all,
so the newly-real `sd_busy()` signal has no observable effect anywhere except for a developer who locally
uncomments that macro to do manual loop-point stress testing, in which case a real signal is strictly more
correct than a permanently-false one for the exact reason the guard exists (throttle the randomizer away
from real card activity). NEUTRAL for the shipped product; a minor, harmless correctness improvement for
the one developer workflow that ever compiles this code.

### 8. `audio_engine.cpp:1666` — `discardRecorder()`'s `ALPHA_OR_BETA` assertion

**What it does when busy:** `FREEZE_WITH_ERROR("E251")` if `isSDRoutineActive() || sd_busy()`.

**Classification: IMPROVED.** Full reasoning in Step 3 below — kept unchanged. This assertion now does
exactly what its own comment says it exists to do: catch a real, documented double-free hazard
(freeing a `SampleRecorder` while its own `cardRoutine()` may be suspended mid-transfer) on a call path
(`AudioRecorder::process()`) that has no other guard. Before this flip, the `sd_busy()` half of its
condition was dead, so this assertion was only ever as strong as `isSDRoutineActive()` alone; now it is
as strong as intended.

### 9. `save_song_ui.cpp:124` — `performSave()`'s `ALPHA_OR_BETA` assertion

**What it does when busy (as activated by the flip, before this task's fix):**
`FREEZE_WITH_ERROR("E316")` if `sd_busy()`.

**Classification: REGRESSION as activated, fixed to NEUTRAL in this task.** Full reasoning in Step 3
below. Unlike site 8, no documented hazard justifies halting here — `performSave()`'s own filesystem
calls simply queue behind whatever briefly holds the FS mutex, exactly like any other caller. Left as a
hard freeze, this assertion would turn an ordinary, harmless scheduling coincidence (a user pressing Save
while an unrelated background op like the overview scan briefly holds the mutex) into a user-visible
device freeze on every beta build — worse than the eternal no-op it replaced. Downgraded to a diagnostic
log in this task, which makes it behaviour-preserving (NEUTRAL) while keeping the information.

---

## Step 3: the two `ALPHA_OR_BETA` assertions, judged individually

Both assert *not busy* and have been trivially satisfied since the day `currentlyAccessingCard` went
dead. With a real signal, both *can* now fire — the question for each is whether that firing represents a
genuine invariant violation worth halting a beta build over, or legitimate concurrency that the assertion
was never entitled to rule out.

### `audio_engine.cpp:1666` (`discardRecorder`) — **KEEP**

```cpp
void discardRecorder(SampleRecorder* recorder) {
	if (ALPHA_OR_BETA_VERSION && (isSDRoutineActive() || deluge::sync::sd_busy())) {
		FREEZE_WITH_ERROR("E251");
	}
	...
```

The surrounding comment states the hazard explicitly and specifically: `finishRecording()` frees the
`SampleRecorder`, and freeing it while the recorder's own `cardRoutine()` is suspended part-way through
leaves that routine running on freed memory, "then freeing it a second time" — a double free, which
"surfaces as M123 from the allocator, a long way from here." The comment says the rule is enforced here,
rather than left for every caller to honour individually, *because* the consequence is real memory
corruption at a distant, hard-to-diagnose site.

Two things push this toward KEEP rather than downgrade or narrow:

1. **This is a real, distant, hard-to-diagnose memory-corruption bug if the invariant is violated**, not
   a scheduling nicety. A false FREEZE (halting when nothing would actually have gone wrong) is a strictly
   better failure mode than a missed real violation (silent heap corruption that surfaces later, possibly
   nowhere near this call site).
2. **`isSDRoutineActive()` already made this assertion partially live before this whole plan.** It has
   already been a real, reachable check, on real signal, independent of the dead-flag bug this plan fixes.
   Adding `sd_busy()` extends the same already-accepted invariant to also cover the FS-mutex-held window,
   rather than introducing a brand new failure mode into a check that was previously always benign.
3. **There is a second call path that bypasses the one softening guard that exists.**
   `AudioRecorder::slowRoutine()` (site 5 above) checks `isSDRoutineActive() || sd_busy()` before calling
   `finishRecording()` → `discardRecorder()`, but `AudioRecorder::process()` (the legacy
   non-`USE_TASK_MANAGER` main loop) calls `finishRecording()` directly, with no such guard. This
   assertion is the *only* protection on that second path — downgrading it to a log would remove the only
   enforcement of a documented double-free hazard on a live call path, not just add a false positive.

**Kept unchanged.** No code edit made to this site.

### `save_song_ui.cpp:124` (`performSave`) — **DOWNGRADED to a diagnostic log**

```cpp
if (ALPHA_OR_BETA_VERSION && deluge::sync::sd_busy()) {
	FREEZE_WITH_ERROR("E316");
}
```

Unlike site 8, there is no comment here, and reading the surrounding code finds no memory-safety or
data-corruption rationale — `performSave()` is a UI-triggered entry point (context menus, the Save
button) that goes on to do its own filesystem work (`StorageManager::fileExists`, sample renames, the
save itself), all of which routes through the same single-owner FS serialization as everything else in
this plan. There is nothing about calling `performSave()` while some unrelated background operation
briefly holds the FS mutex that is unsafe: `performSave`'s own FS calls will simply queue behind whatever
currently holds the mutex, exactly as they would for any other caller.

The most plausible original intent is a sanity check written when the dead flag was believed to
correctly track "exclusive SD access is in progress" — a state that, under the old purely-cooperative,
single-thread-does-everything model, arguably never should have coincided with a user pressing Save. On
Embassy that assumption is simply no longer true: background FS work (the overview scan, sample loads for
playback, MIDI-driven undo/redo dispatch, SysEx handling — sites 1, 4, 6 in this same document) can now
legitimately be mid-operation, for a brief window, at the exact moment a user presses Save. Step 0's own
probe run shows the overview scan alone issuing FS work well over a hundred times across one run — the
kind of frequency that makes a coincidental Save-while-busy a real, unremarkable possibility rather than
an edge case.

Firing `FREEZE_WITH_ERROR` here would turn a normal scheduling coincidence into a user-visible device
freeze on every beta build, for behaviour that was never dangerous. That is exactly the "worse than the
no-op" bar the brief describes for REGRESSION-shaped consumer sites, so this one is fixed rather than left
to fire in the field. Downgraded to a diagnostic `D_PRINTLN` that preserves the `ALPHA_OR_BETA_VERSION`
gate (so it costs nothing on release builds) and keeps a record of the event for anyone debugging a
report of a "slow save," without halting the device:

```cpp
// Diagnostic only, not a hard invariant (see docs/dev/sd_busy_audit.md, save_song_ui.cpp:124):
// this used to FREEZE_WITH_ERROR("E316") here, back when sd_busy() was permanently false and the
// check could never fire. Now that it reports a real, brief per-operation FS-mutex state, a user
// pressing Save while some unrelated background op (e.g. the waveform overview scan) is mid-op is
// legitimate, benign concurrency, not corruption -- performSave's own filesystem calls simply queue
// behind it via the usual single-owner serialization. Log it rather than crash a beta build over a
// normal scheduling coincidence.
if (ALPHA_OR_BETA_VERSION && deluge::sync::sd_busy()) {
	D_PRINTLN("performSave: sd_busy() was true on entry (see sd_busy_audit.md)");
}
```

### Why one and not the other

This is a deliberate asymmetry, not an oversight: site 8 protects a *documented, distant,
hard-to-diagnose memory-corruption hazard* on a call path that has no other guard, where a false halt is
the safe failure mode. Site 9 protects *no documented hazard at all*, on a call path where the
"violation" is provably benign (the operation just queues), where a false halt is a real, avoidable
regression. Reasoning about them independently rather than applying one rule to both is exactly what
produced the different outcomes — treating "assert not busy" as one interchangeable pattern across both
sites would have missed the difference between "this could corrupt memory" and "this is provably safe to
proceed."

## Step 3b: stale doc reference

`docs/dev/target_architecture.md:66` named `currentlyAccessingCard` (deleted in this plan's Task 5) as a
still-existing "hand-placed guard" alongside `audioRoutineLocked`. Updated to name the real, current
mechanism, `deluge::sync::sd_busy()`. The surrounding architectural point (concurrency safety in this
codebase rests on ad-hoc, hand-placed guards rather than a declared contract) is unchanged — only the
example needed correcting.

## Concerns

1. The Step 0 probe's 172 hits confirm the scan runs and makes progress in the one fixture (`cordae`)
   this plan's harness exercises. It is not a proof for every possible sample/fixture combination, and it
   does not confirm the scan reaches full completion (`overviewScanAllDone`) — only that it makes
   substantial, correctness-preserving progress within the run's virtual-time budget.
2. `resource_checker.h`'s `RESOURCE_SD` ceiling (site 2) remains the widest-blast-radius site in this
   plan, per Task 4's own concern, with no softening companion check. This audit did not find a reason to
   narrow it, but it is worth an on-device ear/behaviour check alongside the rest of this plan's pending
   hardware smoke test, since it is the one site that changes task admission rather than a single
   call-site's own behaviour.
3. All classifications and both Step 3 decisions rest on reading the code plus the one Lens 1 fixture's
   execution evidence; none of this has run on real Embassy hardware yet, consistent with the rest of this
   plan.
