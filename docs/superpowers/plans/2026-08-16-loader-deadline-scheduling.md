# Deadline-Ordered Streaming Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Order the streaming loader queue by when each chunk is actually needed, so a voice about to run dry is served before a more "important" voice with 180 ms of slack.

**Architecture:** The loader queue's `priority` field is reinterpreted as an **absolute deadline** — the `AudioEngine::audioSampleTimer` value by which the chunk must be resident. Lower already means more urgent to `Manager::loader_next`, so the queue mechanism is unchanged; only the number supplied changes, plus a wrap-safe comparison. `SampleLowLevelReader` computes the deadline from bytes remaining in the current cluster and the phase increment it last rendered with.

**Tech Stack:** C++23 (portable app), Rust (`deluge_resource`, `deluge_sample_source`), CppSpec (`./dbt test`), `./dbt harness run <name>` for differential + golden gates, `./dbt rust --release` + `tools/deluge_jlink.py` for device verification.

**Spec:** `docs/superpowers/specs/2026-08-16-loader-deadline-scheduling-design.md`

## Global Constraints

- **`u32::MAX` (`0xFFFFFFFF`) means "speculative, no deadline" and MUST keep sorting last.** `Manager::loader_has_lowest` tests `queue_priority == u32::MAX` exactly, and `load_song_ui.cpp:481` waits on that predicate to know speculative prefetching has drained. A computed deadline must never equal it — see Task 1's saturation rule.
- **`0` means "needed immediately."** `BLOCKING_FILL_PRIORITY` in `streaming_loader.rs` is already 0.
- **`audioSampleTimer` must NOT be widened to 64 bits.** 168 uses across ~30 files, 33 of which rely on the `(int32_t)(a - b)` wrap-safe idiom that widening breaks; and `audioSampleTimer += numSamples` runs in the audio ISR while other contexts read it, so a non-atomic 64-bit read would introduce a tearing race.
- **Deadlines must stay within half the `u32` range of "now"** or the signed-difference comparison in Task 4 inverts. Task 1 caps the offset at `0x7FFFFFFF`.
- **Pure EDF.** Voice importance (`getPriorityRating`) no longer influences load order at all. It continues to drive culling, which is unchanged by this plan.
- **The READY path must stay byte-identical.** Gates: `./dbt harness run region-source-differential`, `region-fill-differential`, `region-read-differential`, `golden-embassy-diff`.
- Device builds: `./dbt rust --release` (GCC tree). `./dbt rust` alone builds the dev profile, whose linker layout inverts the SRAM heap — unusable.
- Commit at each task boundary. Never `git commit --amend` (the clang-format/rustfmt hooks reformat and fail the commit; recover with a fresh commit). No `Co-Authored-By` trailers.

---

### Task 1: The deadline arithmetic, as a testable pure function

**Files:**
- Modify: `src/deluge/model/sample/sample_low_level_reader.h`
- Modify: `src/deluge/model/sample/sample_low_level_reader.cpp`
- Test: `tests/spec_sample_reader/loader_deadline_spec.cpp` (create)

**Interfaces:**
- Consumes: `kMaxSampleValue` (`definitions_cxx.hpp:1023`, `1 << 24`, meaning 1:1 playback).
- Produces:
  - `constexpr uint32_t deadlineFromFramesLeft(uint32_t nowFrames, uint32_t sourceFramesLeft, int32_t phaseIncrement);` — free function in `sample_low_level_reader.h`.
  - `int32_t SampleLowLevelReader::lastPhaseIncrement_` — protected member, defaults to `kMaxSampleValue`.

The arithmetic is a free function taking plain integers, deliberately: a member function would need a live `Sample` and `SamplePlaybackGuide` to test, and neither is host-constructible in `tests/spec_sample_reader`. Task 2 wires it to real playback state.

- [ ] **Step 1: Write the failing spec**

Create `tests/spec_sample_reader/loader_deadline_spec.cpp`:

```cpp
// tests/spec_sample_reader/loader_deadline_spec.cpp
//
// The loader queue orders by absolute deadline (see the 2026-08-16 deadline-scheduling design).
// This pins the arithmetic that converts "source frames left in this cluster" into that deadline,
// including the two values that would silently invert urgency if produced by accident: 0xFFFFFFFF
// (which means "speculative, least urgent") and an offset beyond half the u32 range (which would
// invert Task 4's signed-difference comparison).
#include "model/sample/sample_low_level_reader.h"

#include "cppspec.hpp"

describe loader_deadline("deadlineFromFramesLeft", $ {
	it("puts the deadline one output frame per source frame ahead at 1:1", _{
		expect(deadlineFromFramesLeft(1000, 500, kMaxSampleValue)).to_equal(1500u);
	});

	it("brings the deadline nearer when playback is faster than 1:1", _{
		// Double speed consumes 500 source frames in 250 output frames.
		expect(deadlineFromFramesLeft(1000, 500, kMaxSampleValue * 2)).to_equal(1250u);
	});

	it("pushes the deadline further out when playback is slower than 1:1", _{
		expect(deadlineFromFramesLeft(1000, 500, kMaxSampleValue / 2)).to_equal(2000u);
	});

	it("treats a non-positive phase increment as 1:1 rather than dividing by zero", _{
		expect(deadlineFromFramesLeft(1000, 500, 0)).to_equal(1500u);
		expect(deadlineFromFramesLeft(1000, 500, -1)).to_equal(1500u);
	});

	it("never returns the speculative sentinel", _{
		// 0xFFFFFFFF means "no deadline, least urgent". A computed deadline that landed on it
		// would make the MOST distant chunk sort behind every speculative prefetch.
		expect(deadlineFromFramesLeft(0xFFFFFFFEu, 1, kMaxSampleValue) != 0xFFFFFFFFu).to_be_true();
	});

	it("caps the offset at half the u32 range so the wrap-safe compare stays valid", _{
		// A very slow phase increment would otherwise put the deadline more than 2^31 ahead, which
		// Task 4's signed-difference comparison reads as being in the PAST.
		uint32_t d = deadlineFromFramesLeft(0, 0xFFFFFFu, 1);
		expect(d <= 0x7FFFFFFFu).to_be_true();
	});

	it("wraps rather than saturating, because deadlines are absolute timer values", _{
		// Near the timer wrap the deadline legitimately lands at a small number; Task 4's compare
		// is what makes that order correctly.
		expect(deadlineFromFramesLeft(0xFFFFFF00u, 0x200, kMaxSampleValue)).to_equal(0x100u);
	});
});

CPPSPEC_SPEC(loader_deadline)
```

The spec driver in `tests/spec_sample_reader/CMakeLists.txt` globs `*_spec.cpp`, so no registration step is needed.

- [ ] **Step 2: Run it to verify it fails**

Run: `./dbt test 2>&1 | grep -E "loader_deadline|tests passed|tests failed"`
Expected: FAIL — `deadlineFromFramesLeft` is not declared.

- [ ] **Step 3: Implement the arithmetic**

In `sample_low_level_reader.h`, directly below the `regionOutcomeFrom` definition:

```cpp
/// @brief The offset cap for a computed deadline, in output frames.
///
/// Deadlines are absolute `AudioEngine::audioSampleTimer` values compared with a signed difference,
/// which is only valid while the two values are within half the `uint32_t` range of each other. A
/// very slow phase increment could otherwise place a deadline further ahead than that, and it would
/// then compare as being in the PAST — maximally urgent, exactly backwards.
constexpr uint32_t kMaxDeadlineOffsetFrames = 0x7FFFFFFFu;

/// @brief Convert "source frames left in the current cluster" into an absolute loader deadline.
///
/// @param nowFrames        The current `AudioEngine::audioSampleTimer` value.
/// @param sourceFramesLeft Frames of source audio remaining before the next cluster is needed.
/// @param phaseIncrement   Q24 playback rate; `kMaxSampleValue` (1 << 24) is 1:1. Values <= 0 are
///                         treated as 1:1 rather than dividing by zero.
/// @return The absolute timer value by which the next cluster must be resident. Never
///         `0xFFFFFFFF`, which the loader reserves to mean "speculative, no deadline".
constexpr uint32_t deadlineFromFramesLeft(uint32_t nowFrames, uint32_t sourceFramesLeft,
                                          int32_t phaseIncrement) {
	uint32_t increment = (phaseIncrement > 0) ? static_cast<uint32_t>(phaseIncrement)
	                                          : static_cast<uint32_t>(kMaxSampleValue);
	uint64_t outputFrames =
	    (static_cast<uint64_t>(sourceFramesLeft) * static_cast<uint64_t>(kMaxSampleValue)) / increment;
	if (outputFrames > kMaxDeadlineOffsetFrames) {
		outputFrames = kMaxDeadlineOffsetFrames;
	}
	// Deliberately wraps: the deadline is an absolute timer value, and the loader's comparison is
	// wrap-safe. Only the reserved sentinel is excluded.
	uint32_t deadline = nowFrames + static_cast<uint32_t>(outputFrames);
	return (deadline == 0xFFFFFFFFu) ? 0xFFFFFFFEu : deadline;
}
```

Add the cached rate as a protected member of `SampleLowLevelReader`, beside `interpolationBufferSizeLastTime`:

```cpp
	/// @brief The Q24 phase increment this reader last rendered with, for the loader deadline.
	///
	/// Cached rather than threaded down: `moveOnToNextCluster` and `changeClusterIfNecessary` do not
	/// take a phase increment (only `considerUpcomingWindow` does), and `considerUpcomingWindow` is
	/// the caller that leads to the cluster advance — so updating it there means the advance uses a
	/// current rate, not a stale one.
	int32_t lastPhaseIncrement_ = kMaxSampleValue;
```

- [ ] **Step 4: Run the spec to verify it passes**

Run: `./dbt test 2>&1 | grep -E "loader_deadline|tests passed|tests failed"`
Expected: PASS, and the overall suite still reports 0 failures.

- [ ] **Step 5: Verify the spec is not vacuous**

Temporarily change the implementation's `return (deadline == 0xFFFFFFFFu) ? 0xFFFFFFFEu : deadline;` to `return deadline;` and re-run.
Expected: the "never returns the speculative sentinel" example FAILS. Restore the line and confirm the suite is green again.

- [ ] **Step 6: Commit**

```bash
git add src/deluge/model/sample/sample_low_level_reader.h src/deluge/model/sample/sample_low_level_reader.cpp tests/spec_sample_reader/loader_deadline_spec.cpp
git commit -m "feat(sample): add the loader deadline arithmetic"
```

---

### Task 2: Feed real deadlines from the reader's acquire sites

**Files:**
- Modify: `src/deluge/model/sample/sample_low_level_reader.cpp` (`considerUpcomingWindow`, `assignClusters`, `moveOnToNextCluster`, `reassessReassessmentLocation`)
- Modify: `include/libdeluge/sample_source.h` (the `priority` parameter docs)

**Interfaces:**
- Consumes: `deadlineFromFramesLeft`, `lastPhaseIncrement_`, `kMaxDeadlineOffsetFrames` (Task 1).
- Produces: `uint32_t SampleLowLevelReader::deadlineForNextCluster(SamplePlaybackGuide* guide, Sample* sample) const;` — private member.

- [ ] **Step 1: Cache the phase increment where playback provides it**

At the top of `considerUpcomingWindow` (`sample_low_level_reader.cpp:688`), immediately after the existing `FREEZE_WITH_ERROR("E228")` guard:

```cpp
	// Record the rate for the loader deadline. This function is what leads to
	// changeClusterIfNecessary -> moveOnToNextCluster, so the advance that follows uses a current
	// rate rather than one from the previous render.
	lastPhaseIncrement_ = phaseIncrement;
```

- [ ] **Step 2: Add the member that gathers the inputs**

In `sample_low_level_reader.cpp`, above `assignClusters`:

```cpp
uint32_t SampleLowLevelReader::deadlineForNextCluster(SamplePlaybackGuide* guide, Sample* sample) const {
	// No residency to reason from (a note-on, or a reader that just lost its region): the data is
	// wanted for the render in progress, so it is maximally urgent.
	if (!hasCurrentRegion() || currentPlayPos == nullptr) {
		return 0;
	}
	int32_t bytePosWithinCluster = currentPlayPos - reinterpret_cast<char*>(region_.payload_base);
	// Reverse playback consumes toward the cluster's start, so the distance to the boundary is the
	// position itself rather than the remainder.
	int32_t bytesLeft = (guide->playDirection >= 0) ? (Cluster::size - bytePosWithinCluster)
	                                                : bytePosWithinCluster;
	if (bytesLeft < 0) {
		bytesLeft = 0; // already past the boundary — needed now
	}
	int32_t bytesPerFrame = sample->numChannels * sample->byteDepth;
	if (bytesPerFrame <= 0) {
		return 0; // malformed geometry; treat as urgent rather than dividing by zero
	}
	return deadlineFromFramesLeft(AudioEngine::audioSampleTimer,
	                              static_cast<uint32_t>(bytesLeft / bytesPerFrame), lastPhaseIncrement_);
}
```

Declare it in `sample_low_level_reader.h` beside `assignClusters`:

```cpp
	/// @brief The absolute loader deadline for the cluster after the current one — the
	///        `AudioEngine::audioSampleTimer` value by which it must be resident.
	[[nodiscard]] uint32_t deadlineForNextCluster(SamplePlaybackGuide* guide, Sample* sample) const;
```

Add the include for `audioSampleTimer` at the top of `sample_low_level_reader.cpp` if absent:

```cpp
#include "processing/engines/audio_engine.h" // audioSampleTimer (loader deadlines)
```

- [ ] **Step 3: Replace priorityRating at the three acquire sites**

`assignClusters` (`sample_low_level_reader.cpp:403`) — the note-on / reassess acquire:

```cpp
	DelugeRegionState state =
	    deluge_sample_region_acquire_ex(source_, static_cast<uint32_t>(clusterIndex), guide->playDirection,
	                                    deadlineForNextCluster(guide, sample), &region);
```

`moveOnToNextCluster` (`sample_low_level_reader.cpp:486`) — the streaming advance, the site the underruns come from:

```cpp
	if (!deluge_sample_region_acquire(source_, static_cast<uint32_t>(newClusterIndex), guide->playDirection,
	                                  deadlineForNextCluster(guide, sample), &region)) {
```

`reassessReassessmentLocation` (`sample_low_level_reader.cpp:167`) — the final-cluster acquire, currently a hardcoded `1`:

```cpp
		if (!deluge_sample_region_acquire(source_, static_cast<uint32_t>(finalClusterIndex), guide->playDirection,
		                                  deadlineForNextCluster(guide, sample), &finalRegion)) {
```

`priorityRating` remains a parameter on these functions and is now unused by the loader. Leave the signatures alone — removing the parameter touches a dozen call sites across voice, timestretch, and cache code and belongs in its own change. Add this comment above `assignClusters`'s signature so the next reader is not misled:

```cpp
// NOTE: `priorityRating` no longer reaches the loader queue — ordering is by deadline
// (deadlineForNextCluster) as of the 2026-08-16 deadline-scheduling design. The parameter is still
// threaded through from Voice::getPriorityRating() and is retained only to avoid a wide signature
// change; removing it is a separate cleanup.
```

- [ ] **Step 4: Update the C ABI documentation**

In `include/libdeluge/sample_source.h`, replace both occurrences of the `@param priority` line (lines ~113 and ~126):

```c
/// @param priority  Absolute loader deadline: the AudioEngine::audioSampleTimer value by which the
///                  chunk must be resident. Lower is more urgent. 0 == needed immediately;
///                  0xFFFFFFFF == speculative, no deadline (sorts last, and is what
///                  deluge_resource_loader_has_lowest tests for). Used only when the fill is
///                  enqueued.
```

- [ ] **Step 5: Build and gate**

Run: `./dbt rust --release 2>&1 | grep -cE "error"`
Expected: `0`

Run: `./dbt test`
Expected: PASS

Run: `./dbt harness run region-source-differential && ./dbt harness run region-fill-differential && ./dbt harness run region-read-differential && ./dbt harness run golden-embassy-diff`
Expected: all pass, `golden-embassy-diff` reporting `PASS — cordae MIXDOWN matches golden`. Load ORDER changes but loaded CONTENT does not, so a golden diff here means something other than ordering changed.

- [ ] **Step 6: Commit**

```bash
git add src/deluge/model/sample include/libdeluge/sample_source.h
git commit -m "feat(sample): order streaming loads by deadline instead of voice priority"
```

---

### Task 3: A waiting voice's data is needed now, not last

**Files:**
- Modify: `src/deluge/model/voice/voice_sample.cpp:215` (`attemptLateSampleStart`)
- Modify: `src/deluge/model/voice/voice_sample.cpp:916` (the cache-resync acquire)

**Interfaces:**
- Consumes: the deadline semantics established in Task 2. No new API.

Both sites currently pass `0xFFFFFFFF`, which under the new encoding means *speculative, least urgent*. That is backwards for both: `attemptLateSampleStart` is a voice waiting to begin (and, since the async note-on work, a voice we deliberately deferred rather than dropped), and the resync is feeding the render in progress. Left unchanged, a deferred note's own cluster would be served behind every speculative prefetch in the system.

- [ ] **Step 1: Make the late-start acquire urgent**

Replace the acquire at `voice_sample.cpp:215` and its preceding comment's last sentence:

```cpp
	// Deadline 0 == needed immediately: this voice is already waiting to start (the note-on deferred
	// rather than dropped when its first cluster was still loading), so its cluster is the most
	// urgent work in the system. It previously passed 0xFFFFFFFF, which under deadline ordering is
	// "speculative" -- it would have queued a waiting voice behind every prefetch.
	DelugeSampleRegion region{};
	DelugeRegionState state0 = deluge_sample_region_acquire_ex(source_, startAtClusterIndex, voiceSource->playDirection,
	                                                           0u, &region);
```

- [ ] **Step 2: Make the cache-resync acquire urgent**

Replace the acquire at `voice_sample.cpp:916` and the two comment lines above it:

```cpp
					// Deadline 0 == needed immediately: this resync feeds the render in progress.
					// (Previously 0xFFFFFFFF to keep the resync out of the caller's priority
					// ordering; under deadline ordering that value means "speculative", which is the
					// opposite of what this needs.)
					DelugeSampleRegion region;
					if (deluge_sample_region_acquire_ex(source_, static_cast<uint32_t>(uncachedClusterIndex),
					                                    static_cast<int8_t>(playDirection), 0u, &region)
					    == DELUGE_REGION_READY) {
```

- [ ] **Step 3: Build and gate**

Run: `./dbt rust --release 2>&1 | grep -cE "error"`
Expected: `0`

Run: `./dbt test && ./dbt harness run golden-embassy-diff`
Expected: both pass.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/model/voice/voice_sample.cpp
git commit -m "fix(voice): a waiting voice's cluster is urgent, not speculative"
```

---

### Task 4: Wrap-safe ordering in the loader queue

**Files:**
- Modify: `crates/deluge_resource/src/manager.rs` (`loader_next`)
- Modify: `crates/deluge_resource/include/deluge_resource.h` (`loader_enqueue` docs)
- Test: `crates/deluge_resource/src/lib.rs` (tests module)

**Interfaces:**
- Consumes: nothing from earlier tasks (the manager does not know where deadlines come from).
- Produces: no API change.

Deliberately AFTER Tasks 2-3. A signed-difference comparison assumes the values are points on a timeline within half the range of each other; `getPriorityRating()` values are spread across the whole `u32` range, so making this change first would scramble ordering while the old values were still in flight.

- [ ] **Step 1: Write the failing tests**

Add to the `tests` module in `crates/deluge_resource/src/lib.rs`:

```rust
    /// Deadlines are absolute timer values that wrap, so ordering must use a signed difference
    /// rather than `<`. Without it, the instant the sample timer wraps, a deadline of 5 sorts as
    /// less urgent than one of 0xFFFFFF00 — i.e. every just-past-the-wrap chunk goes to the back.
    #[test]
    fn loader_serves_the_earliest_deadline_across_the_timer_wrap() {
        let (buf, m, a, _h) = indexed_mgr(64, 2 * 1024 * 1024);
        let early = unsafe { deluge_resource_request(m, a, 0, 4096) };
        let late = unsafe { deluge_resource_request(m, a, 1, 4096) };
        assert!(!early.is_null() && !late.is_null());

        // `late` is just BEFORE the wrap, `early` just after it: `early` is the sooner deadline.
        unsafe { deluge_resource_loader_enqueue(m, deluge_resource_slot_of(m, late), 0xFFFF_FF00) };
        unsafe { deluge_resource_loader_enqueue(m, deluge_resource_slot_of(m, early), 0x0000_0005) };

        assert_eq!(
            unsafe { deluge_resource_loader_next(m) },
            early,
            "the chunk just past the wrap is the earlier deadline and must be served first"
        );
        unsafe { deluge_resource_release(m, early) };
        unsafe { deluge_resource_release(m, late) };
        let _ = buf;
    }

    /// The speculative sentinel must sort last even against a deadline that is numerically larger
    /// in wrapped terms — `load_song_ui` waits on `loader_has_lowest`, which tests for it exactly.
    #[test]
    fn the_speculative_sentinel_still_sorts_last() {
        let (buf, m, a, _h) = indexed_mgr(64, 2 * 1024 * 1024);
        let real = unsafe { deluge_resource_request(m, a, 0, 4096) };
        let spec = unsafe { deluge_resource_request(m, a, 1, 4096) };
        assert!(!real.is_null() && !spec.is_null());

        unsafe { deluge_resource_loader_enqueue(m, deluge_resource_slot_of(m, spec), u32::MAX) };
        unsafe { deluge_resource_loader_enqueue(m, deluge_resource_slot_of(m, real), 0xFFFF_FF00) };

        assert_eq!(
            unsafe { deluge_resource_loader_next(m) },
            real,
            "a real deadline must beat the speculative sentinel"
        );
        unsafe { deluge_resource_release(m, real) };
        unsafe { deluge_resource_release(m, spec) };
        let _ = buf;
    }
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd crates/deluge_resource && cargo test --quiet 2>&1 | grep -E "test result|panicked"`
Expected: FAIL on `loader_serves_the_earliest_deadline_across_the_timer_wrap` — the plain `<` picks `late`.

- [ ] **Step 3: Implement the wrap-safe comparison**

Replace the comparison in `Manager::loader_next` (`manager.rs:789`):

```rust
            // Deadline ordering, wrap-safe. `queue_priority` is an absolute AudioEngine sample-timer
            // value, so a plain `<` inverts across the 32-bit wrap: a deadline of 5 would sort behind
            // one of 0xFFFFFF00 even though it is sooner. The signed difference is the same idiom the
            // C++ side uses in 33 places, e.g. `(int32_t)(audioSampleTimer - pressTime) < x`.
            //
            // u32::MAX is the speculative sentinel, not a timeline point, so it is excluded from the
            // signed comparison and always loses to a real deadline.
            let better = match (best, s.queue_priority) {
                (None, _) => true,
                (Some(_), u32::MAX) => false,
                _ if best_pri == u32::MAX => true,
                _ => (s.queue_priority.wrapping_sub(best_pri) as i32) < 0,
            };
            if better {
                best = Some(i);
                best_pri = s.queue_priority;
            }
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd crates/deluge_resource && cargo test --quiet 2>&1 | grep -E "test result"`
Expected: all pass.

- [ ] **Step 5: Verify the tests are not vacuous**

Temporarily revert the comparison to `s.queue_priority < best_pri` and re-run.
Expected: `loader_serves_the_earliest_deadline_across_the_timer_wrap` FAILS. Restore and confirm green.

- [ ] **Step 6: Update the queue's documentation**

In `crates/deluge_resource/include/deluge_resource.h`, replace the `deluge_resource_loader_enqueue` doc comment:

```c
/// Enqueue the chunk at `slot` for loading at `priority` (lower = more urgent; re-enqueue just
/// updates it). `priority` is an ABSOLUTE DEADLINE: the AudioEngine::audioSampleTimer value by
/// which the chunk must be resident. Ordering is wrap-safe (signed difference), so a deadline just
/// past the 32-bit wrap correctly beats one just before it. 0 == needed immediately; 0xFFFFFFFF ==
/// speculative, no deadline, always served last (and is what deluge_resource_loader_has_lowest
/// tests for).
```

Mirror the same wording onto `Manager::loader_enqueue`'s Rust doc comment in `manager.rs`.

- [ ] **Step 7: Build, gate, commit**

Run: `./dbt rust --release 2>&1 | grep -cE "error"` → `0`
Run: `./dbt test && ./dbt harness run golden-embassy-diff` → both pass

```bash
git add crates/deluge_resource
git commit -m "feat(resource): order the loader queue by wrap-safe deadline"
```

---

### Task 5: Verify on hardware against the measured baseline

**Files:** none modified — this task is measurement.

**Interfaces:** consumes the whole change.

The pre-change baseline, captured 2026-08-16 on the dual-index build: **5** `late or reached end of waveform` lines and **1** `force-culled` in a ~40 s playback window.

- [ ] **Step 1: Flash**

```bash
./dbt rust --release
tools/deluge_jlink.py src/bsp/rust/target/armv7a-none-eabihf/release/deluge-rust \
    --timeout 200 --until 'going into main loop' --fail-on 'panicked at'
```
Expected: boots to `going into main loop`.

- [ ] **Step 2: Capture the same workload**

```bash
tools/deluge_jlink.py src/bsp/rust/target/armv7a-none-eabihf/release/deluge-rust \
    --no-load --timeout 600 --max-lines 20000 > /tmp/cap-edf.log 2>&1 &
```

Play the same song for a comparable duration, then:

```bash
grep -c "late or reached end of waveform" /tmp/cap-edf.log
grep -c "force-culled" /tmp/cap-edf.log
```

- [ ] **Step 3: Judge the result**

Success is the underrun count falling toward zero **without** a rise in `force-culled`. A rise in culls would mean the loader is now consuming CPU rather than scheduling better, and the change should be reconsidered rather than kept.

Record the numbers in the spec's Testing section, replacing the baseline line with a before/after pair. Do not delete the baseline — a future change needs it.

- [ ] **Step 4: Commit the recorded result**

```bash
git add docs/superpowers/specs/2026-08-16-loader-deadline-scheduling-design.md
git commit -m "docs(spec): record the measured deadline-scheduling result"
```

---

## Verification before calling this done

- [ ] `./dbt test` passes.
- [ ] `cd crates/deluge_resource && cargo test` passes.
- [ ] `region-source-differential`, `region-fill-differential`, `region-read-differential`, `golden-embassy-diff` all pass.
- [ ] On device: `late or reached end of waveform` count is lower than the baseline of 5, with `force-culled` no higher than 1.
- [ ] Loading a song still leaves the load screen promptly — `load_song_ui.cpp:481` waits on `deluge_resource_loader_has_lowest`, which depends on `u32::MAX` still being reachable and still sorting last.

## Out of scope

- **Underrun recovery.** `moveOnToNextCluster` still terminates the voice when a cluster genuinely is not ready. Stalling instead of dropping is the right handler and is tracked separately; this plan reduces how often it is reached, it does not change what happens then.
- **Removing the now-unused `priorityRating` parameter** from the reader's signatures.
- **Time-stretched voices' true consumption rate** — they use the cached phase increment, which is not their real rate. See the spec's out-of-scope section.
- **Lookahead depth and admission control** — the spec explains why both are separate.
