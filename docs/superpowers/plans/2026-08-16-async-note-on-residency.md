# Async Note-On Residency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A sample note-on never fails because its first cluster has not loaded *yet*, and the preview stops blocking the audio path for ~38 ms.

**Architecture:** Propagate the region port's tri-state (`READY`/`LOADING`/`UNAVAILABLE`) up to the note-on decision instead of collapsing it to `bool`, then treat `LOADING` as "admit the voice and start it late" via the late-start machinery that already exists. With that in place, the preview switches from the synchronous `DELUGE_LOAD_NOW` warm fill to the non-blocking `DELUGE_LOAD_ENQUEUE`.

**Tech Stack:** C++23 (portable app), Rust (region port / reservation crates), CppSpec (`./dbt test`), `./dbt harness run <name>` for the differential + golden gates, `./dbt rust --release` + `tools/deluge_jlink.py` for device verification.

**Spec:** `docs/superpowers/specs/2026-08-15-async-note-on-residency-design.md` — read §4a and §4b first; they are the measured findings that this plan implements, and they supersede §4.

## Global Constraints

- The `READY` path must stay **byte-identical**. Gates: `./dbt harness run region-source-differential`, `region-fill-differential`, `region-read-differential`, `golden-embassy-diff`.
- **Nothing may block.** No new `block_on_fiber`, no synchronous card read on a note-on path. A wait on a storage-tier task is progress-class and illegal until `STREAM_EXEC` (R5a Phase 1) — see `docs/superpowers/specs/2026-08-08-storage-execution-model-end-state-design.md`.
- **`DelugeRegionState` must not be returned directly from C++ functions.** It is a plain C enum with `DELUGE_REGION_READY = 1`, `LOADING = 2`, `UNAVAILABLE = 3`, so `if (!result)` compiles and is silently wrong at every site. Use the scoped `RegionOutcome` from Task 1.
- AudioClips keep today's behaviour: `pendingSamplesLate` is unused for them (`voice_sample.h:87`).
- Device builds: `./dbt rust --release` (GCC tree). `./dbt rust` alone builds the dev profile, whose linker layout inverts the SRAM heap — unusable.
- Commit at each task boundary. Never `git commit --amend` (the clang-format/rustfmt hooks reformat and fail the commit; recover with a fresh commit). No `Co-Authored-By` trailers.

---

### Task 1: A scoped outcome type, and `assignClusters` returns it

**Files:**
- Modify: `src/deluge/model/sample/sample_low_level_reader.h` (add the enum; change `assignClusters`)
- Modify: `src/deluge/model/sample/sample_low_level_reader.cpp:370-440` (`assignClusters` body + its 3 internal callers)

**Interfaces:**
- Consumes: `DelugeRegionState` / `deluge_sample_region_acquire_ex` from `include/libdeluge/sample_source.h`
- Produces: `enum class RegionOutcome : uint8_t { Ready, Loading, Unavailable };` and `RegionOutcome SampleLowLevelReader::assignClusters(SamplePlaybackGuide*, Sample*, int32_t clusterIndex, int32_t priorityRating);`

- [ ] **Step 1: Add the scoped type to the header**

In `sample_low_level_reader.h`, above the class:

```cpp
/// The region port's tri-state, as a SCOPED enum.
///
/// Deliberately not `DelugeRegionState` itself: that is a plain C enum whose `DELUGE_REGION_READY`
/// is 1, so `if (!state)` compiles at every call site and is silently wrong (`!UNAVAILABLE` is
/// `false`). A scoped enum makes the compiler find every site that must be updated.
enum class RegionOutcome : uint8_t {
	Ready,       ///< The region is resident and pinned; `region_` is set.
	Loading,     ///< Reserved and enqueued, not yet filled. Retry later; do NOT treat as failure.
	Unavailable, ///< Out of range, or the port could not reserve at all. A genuine failure.
};

/// Map the C-ABI tri-state onto RegionOutcome. Any unknown value is treated as Unavailable, the
/// conservative choice: a caller that drops a voice on an unrecognised state is safe, one that
/// waits forever is not.
constexpr RegionOutcome regionOutcomeFrom(DelugeRegionState state) {
	switch (state) {
	case DELUGE_REGION_READY:
		return RegionOutcome::Ready;
	case DELUGE_REGION_LOADING:
		return RegionOutcome::Loading;
	default:
		return RegionOutcome::Unavailable;
	}
}
```

Change the declaration (currently `bool assignClusters(...)` at line 179):

```cpp
	RegionOutcome assignClusters(SamplePlaybackGuide* guide, Sample* sample, int32_t clusterIndex,
	                             int32_t priorityRating);
```

- [ ] **Step 2: Build to see the compiler list every call site**

Run: `./dbt rust --release 2>&1 | grep -E "error:" | head -20`
Expected: FAIL, listing each site that uses `assignClusters`'s result as a `bool`. That list is the work for Step 3 — the type is doing its job.

- [ ] **Step 3: Convert the body and its internal callers**

In `assignClusters` (`sample_low_level_reader.cpp`), replace the two `return false` paths and the final `return true`:

```cpp
	if (source_ == nullptr) {
		D_PRINTLN("assignClusters fail: source_ null (source pool exhausted)");
		return RegionOutcome::Unavailable; // No cursor to load through — this can never become ready.
	}

	DelugeSampleRegion region;
	RegionOutcome outcome = regionOutcomeFrom(deluge_sample_region_acquire_ex(
	    source_, static_cast<uint32_t>(clusterIndex), guide->playDirection,
	    static_cast<uint32_t>(priorityRating), &region));
	if (outcome != RegionOutcome::Ready) {
		return outcome; // Loading is NOT a failure — the caller decides whether to wait.
	}
	// ... unchanged retain/region_ assignment ...
	return RegionOutcome::Ready;
```

Update its three internal callers to preserve today's behaviour exactly by testing against `Ready`:
- `setupClustersForPlayFromByte` (line ~358): `if (assignClusters(...) != RegionOutcome::Ready) { ... return false; }` — this function still returns `bool` until Task 2.
- `moveOnToNextCluster` (line ~454): same shape.
- `reassessReassessmentLocation` (line ~164 via the final-cluster acquire): unchanged, it calls the boolean `deluge_sample_region_acquire` directly.

- [ ] **Step 4: Build clean and gate the READY path**

Run: `./dbt rust --release 2>&1 | grep -cE "error:"`
Expected: `0`

Run: `./dbt harness run region-source-differential && ./dbt harness run region-fill-differential`
Expected: both pass — this task is behaviour-preserving, so any diff is a bug in it.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/model/sample/sample_low_level_reader.h src/deluge/model/sample/sample_low_level_reader.cpp
git commit -m "refactor(sample): give assignClusters a scoped region outcome"
```

---

### Task 2: Propagate the outcome to the note-on decision

**Files:**
- Modify: `src/deluge/model/sample/sample_low_level_reader.h:66,81` (two signatures)
- Modify: `src/deluge/model/sample/sample_low_level_reader.cpp` (`setupClustersForPlayFromByte`, `setupClusersForInitialPlay`)
- Modify call sites: `src/deluge/model/voice/voice_sample.cpp:256`, `src/deluge/dsp/timestretch/time_stretcher.cpp:1006`, `src/deluge/model/clip/audio_clip.cpp:428`, `src/deluge/model/voice/voice_unison_part_source.cpp:59`

**Interfaces:**
- Consumes: `RegionOutcome`, `assignClusters` (Task 1)
- Produces:
  - `RegionOutcome SampleLowLevelReader::setupClustersForPlayFromByte(SamplePlaybackGuide*, Sample*, int32_t startPlaybackAtByte, int32_t priorityRating);`
  - `RegionOutcome SampleLowLevelReader::setupClusersForInitialPlay(SamplePlaybackGuide*, Sample*, int32_t byteOvershoot = 0, bool justLooped = false, int32_t priorityRating = 1);`

- [ ] **Step 1: Change both signatures and their returns**

Both functions currently return `bool`. Every `return false` that came from a *range* rejection stays `Unavailable`; the one that came from `assignClusters` forwards its outcome:

```cpp
RegionOutcome SampleLowLevelReader::setupClustersForPlayFromByte(SamplePlaybackGuide* guide, Sample* sample,
                                                                 int32_t startPlaybackAtByte,
                                                                 int32_t priorityRating) {
	if (startPlaybackAtByte < sample->audioDataStartPosBytes
	    || startPlaybackAtByte >= sample->audioDataStartPosBytes + sample->audioDataLengthBytes) {
		return RegionOutcome::Unavailable; // Out of range — no amount of waiting fixes this.
	}
	int32_t clusterIndex = startPlaybackAtByte >> Cluster::size_magnitude;
	RegionOutcome outcome = assignClusters(guide, sample, clusterIndex, priorityRating);
	if (outcome != RegionOutcome::Ready) {
		return outcome;
	}
	setupForPlayPosMovedIntoNewCluster(guide, sample, reinterpret_cast<char*>(region_.payload_base),
	                                   startPlaybackAtByte & (Cluster::size - 1), sample->byteDepth);
	return RegionOutcome::Ready;
}
```

`setupClusersForInitialPlay` forwards `setupClustersForPlayFromByte`'s outcome unchanged in place of its `bool`.

- [ ] **Step 2: Build to enumerate the external call sites**

Run: `./dbt rust --release 2>&1 | grep -E "error:" | head`
Expected: FAIL at the four call sites listed under **Files**.

- [ ] **Step 3: Update the three call sites that keep today's behaviour**

These three must NOT gain deferral — only Task 3's site does.

`voice_sample.cpp:256` (the `goodToGo` re-acquire):
```cpp
			if (setupClustersForPlayFromByte(voiceSource, sample, static_cast<int32_t>(startAtByte),
			                                 static_cast<int32_t>(0xFFFFFFFFU))
			    != RegionOutcome::Ready) {
				goto waitForResidency;
			}
```

`time_stretcher.cpp:1006`:
```cpp
	bool success = voiceSample->setupClustersForPlayFromByte(guide, sample, newHeadBytePos, priorityRating)
	               == RegionOutcome::Ready;
```

`audio_clip.cpp:428` — AudioClips have no `pendingSamplesLate`, so anything but `Ready` stays a failure:
```cpp
				    voiceSample->setupClusersForInitialPlay(&guide, ((Sample*)sampleHolder.audioFile), 0, false, 1)
				        == RegionOutcome::Ready;
```

- [ ] **Step 4: Update `voice_unison_part_source.cpp:59` to compile, still without deferral**

```cpp
		return voiceSample->setupClusersForInitialPlay(guide, (Sample*)guide->audioFileHolder->audioFile, 0, false, 1)
		       == RegionOutcome::Ready;
```

Deferral arrives in Task 3; keeping this task purely mechanical is what makes the golden gates meaningful.

- [ ] **Step 5: Build clean and prove byte-identity**

Run: `./dbt rust --release 2>&1 | grep -cE "error:"`
Expected: `0`

Run: `./dbt harness run region-source-differential && ./dbt harness run region-read-differential && ./dbt harness run golden-embassy-diff`
Expected: all pass. This task changed no behaviour, so a golden diff here means a call site was converted wrongly.

- [ ] **Step 6: Commit**

```bash
git add src/deluge/model/sample src/deluge/model/voice src/deluge/dsp/timestretch src/deluge/model/clip
git commit -m "refactor(sample): propagate the region outcome to the note-on decision"
```

---

### Task 3: Defer instead of dropping the voice

**Files:**
- Modify: `src/deluge/model/voice/voice_unison_part_source.cpp:31-60`
- Test: `tests/spec_sample_reader/` (new file `region_outcome_spec.cpp`)

**Interfaces:**
- Consumes: `RegionOutcome`, `setupClusersForInitialPlay` (Task 2), `VoiceSample::pendingSamplesLate` (public, `voice_sample.h:87`)
- Produces: no new API — a behaviour change at one call site.

- [ ] **Step 1: Write the failing spec for the outcome mapping**

The full voice path needs a `Voice`/`Sound`/`ModelStack` and is not host-constructible, so this spec pins the piece that IS unit-testable and that the bug turned on: the mapping, including the trap that `DELUGE_REGION_READY == 1` makes a boolean test wrong.

Create `tests/spec_sample_reader/region_outcome_spec.cpp`:

```cpp
// Pins RegionOutcome's mapping, including the reason it exists: DelugeRegionState is a plain C enum
// whose READY is 1, so a boolean test on it silently inverts for UNAVAILABLE (== 3, truthy).
#include "model/sample/sample_low_level_reader.h"

#include "cppspec.hpp"

describe region_outcome_spec("RegionOutcome", $ {
	it("maps each port state to its own outcome", _ {
		expect(regionOutcomeFrom(DELUGE_REGION_READY)).to_equal(RegionOutcome::Ready);
		expect(regionOutcomeFrom(DELUGE_REGION_LOADING)).to_equal(RegionOutcome::Loading);
		expect(regionOutcomeFrom(DELUGE_REGION_UNAVAILABLE)).to_equal(RegionOutcome::Unavailable);
	});

	it("treats an unrecognised state as Unavailable, never as ready-or-waitable", _ {
		expect(regionOutcomeFrom(static_cast<DelugeRegionState>(99))).to_equal(RegionOutcome::Unavailable);
	});

	it("does not share the C enum's truthiness trap", _ {
		// The bug this type prevents: `!DELUGE_REGION_UNAVAILABLE` is false, so a caller testing the
		// raw state as a bool treats an unloadable region as success.
		expect(!static_cast<int>(DELUGE_REGION_UNAVAILABLE)).to_be_false();
		expect(RegionOutcome::Unavailable != RegionOutcome::Ready).to_be_true();
	});
});
```

Register it in `tests/CMakeLists.txt` alongside the other `spec_sample_reader` sources.

- [ ] **Step 2: Run it to see it fail**

Run: `./dbt test 2>&1 | tail -20`
Expected: FAIL — `region_outcome_spec.cpp` does not compile until Task 1's header exists in this build, or the spec binary is not registered.

- [ ] **Step 3: Make it pass**

Task 1 already added `regionOutcomeFrom`. If the spec fails only on registration, fix the CMake entry; add no production code for this step.

Run: `./dbt test 2>&1 | tail -5`
Expected: PASS

- [ ] **Step 4: Implement defer-not-drop**

Replace `voice_unison_part_source.cpp:59`:

```cpp
		RegionOutcome outcome =
		    voiceSample->setupClusersForInitialPlay(guide, (Sample*)guide->audioFileHolder->audioFile, 0, false, 1);
		switch (outcome) {
		case RegionOutcome::Ready:
			return true; // Sounds now — unchanged.
		case RegionOutcome::Loading:
			// The chunk is reserved and its fill is in flight; it is not loaded YET. Admit the voice
			// and let the late-start machinery start it as soon as the data lands: a non-zero
			// pendingSamplesLate makes voice.cpp's tryToStartMidNote fire on the next render, which
			// calls attemptLateSampleStart -> SUCCESS starts the note, WAIT retries, FAILURE
			// unassigns. 1 sample is inaudible, and each WAIT advances the position in real time, so
			// the note sounds from where it would have been.
			//
			// Dropping the voice here instead is the old behaviour, and it is what made a
			// still-loading first cluster fire a false E199.
			voiceSample->pendingSamplesLate = 1;
			return true;
		case RegionOutcome::Unavailable:
			return false; // Genuinely unplayable — unchanged.
		}
		return false; // Unreachable; keeps the compiler happy about the enum switch.
```

- [ ] **Step 5: Verify no regression on the ready path**

Run: `./dbt test && ./dbt harness run region-source-differential && ./dbt harness run golden-embassy-diff`
Expected: all pass. Deferral only changes what happens on `LOADING`, which the goldens never hit (their fixtures are resident).

- [ ] **Step 6: Commit**

```bash
git add src/deluge/model/voice/voice_unison_part_source.cpp tests/
git commit -m "fix(voice): defer a note whose first cluster is still loading, instead of dropping it"
```

---

### Task 4: Stop the preview blocking the audio path

**Files:**
- Modify: `src/deluge/processing/engines/audio_engine.cpp:1437` (`previewSample`'s `loadFile` call)

**Interfaces:**
- Consumes: Task 3's deferral (this task is unsafe without it — `ENQUEUE` guarantees the first note-on sees `LOADING`)
- Produces: no API change.

- [ ] **Step 1: Switch the preview's load instruction**

```cpp
	// CLUSTER_ENQUEUE, not CLUSTER_LOAD_IMMEDIATELY: the immediate mode reads the covered clusters
	// from the card SYNCHRONOUSLY, measured at 38.2-40.1 ms on device. That stalls the audio path
	// (a 9.8 ms late render, plus four more in the same second), which inflates the engine's
	// measured render time, which makes it declare an overload and force-cull a voice -- the very
	// preview voice the fill was warming. That is the preview-truncation bug; see the
	// 2026-08-15 async-note-on-residency design doc, §4b and the truncation memory note.
	//
	// Enqueueing never blocks. The note-on then sees LOADING and defers (VoiceUnisonPartSource),
	// starting a few ms late instead of either stalling audio or dropping the voice.
	Error error = range->sampleHolder.loadFile(false, true, true, CLUSTER_ENQUEUE);
```

- [ ] **Step 2: Build and flash**

```bash
./dbt rust --release
tools/deluge_jlink.py src/bsp/rust/target/armv7a-none-eabihf/release/deluge-rust \
    --timeout 200 --until 'going into main loop' --fail-on 'panicked at'
```
Expected: boots to `going into main loop`.

- [ ] **Step 3: Verify on hardware — the three symptoms together**

Attach and capture while previewing several not-yet-cached samples:
```bash
tools/deluge_jlink.py src/bsp/rust/target/armv7a-none-eabihf/release/deluge-rust \
    --no-load --timeout 300 --max-lines 8000 > /tmp/cap.log 2>&1
```
Expected in `/tmp/cap.log`:
- `reserve:` lines report **well under 1 ms** (was 38.2-40.1 ms).
- **No** `force-culled` line follows a preview.
- **No** `assignClusters fail` line, and no `fault COMMIT` (E199).
- The preview plays to its full length by ear.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/processing/engines/audio_engine.cpp
git commit -m "fix(preview): enqueue the preview's clusters instead of reading them synchronously"
```

---

### Task 5: Fix the E199 assertion itself — RESOLVED 2026-08-16, scope changed

**Outcome:** Step 1's investigation found the remover, and it is a cross-tier race rather than
anything local to this call: the audio render preempts the fiber-tier note-on and erases the voice
from `voices_` while `Sound::noteOn` still holds a reference to that element. The full write-up,
including two further defects the same race causes (vector reallocation under an in-flight
audio-tier iterator, and ABA on pool-recycled `Voice` addresses), is
`docs/superpowers/specs/2026-08-16-voices-cross-tier-race.md`.

Steps 2-3 as written are therefore **not implementable as scoped**: the invariant `checkVoiceExists`
asserts cannot hold at that site, so "fix the cause without touching the assertion" is a
contradiction there. Fixing the cause properly means mutual exclusion on `voices_`, which is a design
with its own testing story and belongs with the storage-execution-model tiering.

**Done instead (by decision, 2026-08-16):** the narrow, safe part — `previewSample` now sets
`bypassCulling = true` **before** its `Sound::noteOn` instead of after, so the preview voice is no
longer cull-eligible throughout its own note-on. `checkVoiceExists` is untouched. Commit
`e58f2141a`. Tasks 3 and 4 independently stop `noteOn` returning false for the preview, so E199 is
unreachable in practice.

**Left open:** the three defects in the race spec. Not scheduled.

<details>
<summary>Original Task 5 text, kept for the record</summary>

#### Task 5: Fix the E199 assertion itself

**Files:**
- Modify: `src/deluge/processing/sound/sound.cpp:1665-1680` (only after the investigation below)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: no API change.

The assertion is wrong independently of everything above: by the time `checkVoiceExists(voice, "E199")` runs, the voice is not in `voices_`, so E199 misreports whatever removed it. `acquireVoice()` does `voices_.push_back` before `noteOn` (`sound.cpp:4855`) and the reuse path keeps its voice in the list (`sound.cpp:1627-1631`), so **what removes it is not established.**

- [ ] **Step 1: Find the remover — do not guess**

Read every path between `acquireVoice()` and the `checkVoiceExists` call that can erase from `voices_`, including anything `voice->noteOn` itself calls. Candidates to rule in or out explicitly: `freeActiveVoice`, `unassignAllVoices`, culling (`AudioEngine::cullVoices` → `terminateOneVoice`) running re-entrantly during a note-on.

Culling is the leading candidate on the evidence: the device logs show `force-culled 1 voice` interleaved with the two failing `assignClusters` attempts, and culling removes voices from `voices_`.

Write what you find into the task's report before changing any code.

- [ ] **Step 2: Write a failing spec for the established cause**

Only writable once Step 1 names the remover. If the cause is re-entrant culling, the spec asserts that a voice culled during its own note-on does not reach `checkVoiceExists`.

- [ ] **Step 3: Fix the cause, not the assertion**

Do NOT soften or delete `checkVoiceExists`. It is a real invariant; the bug is that something violates it silently.

- [ ] **Step 4: Verify**

Run: `./dbt test && ./dbt harness run golden-embassy-diff`
Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/processing/sound/sound.cpp tests/
git commit -m "fix(sound): stop E199 firing on a voice something else already removed"
```

</details>

---

### Task 6: Retire the investigation scaffolding

**Files:**
- Modify: `src/deluge/model/sample/sample_holder.cpp` (timing + `reserve:` log), `src/deluge/model/sample/sample_holder_for_voice.cpp` (close logs), `src/deluge/model/sample/sample_low_level_reader.cpp` (the manager-probe block in `assignClusters`), `src/deluge/processing/engines/audio_engine.cpp` (`preview: loadFile` log)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing removed from the C-ABI — see Step 1.

- [ ] **Step 1: Keep the accessors, drop the noise**

**Keep** `deluge_sample_reserve_asset`, `_covered_count`, `_leased_count`, `_covered_index` and their header docs. They are what made an invisible failure checkable, and `_asset` is what the Task-in-`21c50dc2f` fix is tested against.

**Remove** the per-call `D_PRINTLN`s added during the investigation: the `reserve:` line and its `getSystemTime()` timing in `claimClusterReasonsForMarker`, the `reserve CLOSE` lines, the `preview: loadFile` line, and the `peek`/`slot_of`/`lease_count_by_slot` probe block inside `assignClusters`'s failure path. They log on hot-ish paths and their job is done.

**Keep** the plain one-line `assignClusters` failure log, reduced to the outcome and cluster index — it is still the first thing anyone wants when a note fails.

- [ ] **Step 2: Build, test, and confirm the logs are gone**

```bash
./dbt rust --release
strings src/bsp/rust/target/armv7a-none-eabihf/release/deluge-rust | grep -cE "reserve: |reserve CLOSE|preview: loadFile"
```
Expected: `0`

Run: `./dbt test`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add src/deluge
git commit -m "chore(sample): retire the residency investigation logging, keep the accessors"
```

---

## Verification before calling this done

- [ ] `./dbt test` passes.
- [ ] `./dbt harness run region-source-differential`, `region-fill-differential`, `region-read-differential`, `golden-embassy-diff` all pass.
- [ ] `cd crates/deluge_sample_reader && cargo test` passes (43 tests, including `move_to_a_different_asset_rebinds_and_releases_the_old_pins`).
- [ ] On device: previewing several uncached samples in rapid succession produces no `force-culled` after a preview, no `assignClusters fail`, no E199, `reserve:` well under 1 ms, and full-length preview playback by ear.
- [ ] On device: ordinary polyphonic playback under load is unchanged by ear — Task 3 makes a deferred voice hold its slot where it used to be dropped, which is the one behaviour most likely to surprise (spec §8).

## Out of scope

- **The ~1.7 MB/s read rate.** 64 KB taking 38 ms is slow for this card path and suggests something inefficient under `fill_now`. Task 4 stops the preview *waiting* on it, but does not make it faster. Separate investigation.
- Layer 3 / `STREAM_EXEC` promotion (R5a Phase 1) — its own ladder. Until it lands, a deferred voice's convergence depends on the Embassy executor scheduling the fill task, which it does today but is not guaranteed under audio-tier load (spec §8).
- AudioClip deferral (spec §5.3).
- Migrating other `claimClusterReasons` callers to direct reservation use.
