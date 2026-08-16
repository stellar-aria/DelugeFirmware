# `Sound::voices_` is mutated across tiers without exclusion

**Status:** diagnosed, not fixed. No design chosen yet. A device data abort whose signature fits
this race was seen on 2026-08-16 while browsing samples — see the section below, including why the
one measurement taken does NOT confirm it.
**Found:** 2026-08-16, while establishing what removes the voice that E199 asserts on
(`docs/superpowers/plans/2026-08-16-async-note-on-residency.md`, Task 5).
**Related:** `2026-08-08-storage-execution-model-end-state-design.md` (tier rules),
`2026-08-15-async-note-on-residency-design.md` (the E199 that surfaced this).

## Summary

`Sound::voices_` is a `deluge::fast_vector<ActiveVoice>` that both the **audio tier** and the
**fiber/interaction tier** read, iterate, and structurally modify, with no mutual exclusion between
them. The audio render runs on a preemptive `InterruptExecutor` and can therefore interrupt a
fiber-tier note-on at any instruction. Every consequence below follows from that one fact.

This is not preview-specific. It is reachable from any note-on.

## How it was established

`ActiveVoice` is `std::unique_ptr<Voice, decltype(&recycle)>` (`src/deluge/memory/object_pool.h:91`).
`Sound::checkVoiceExists` (`sound.cpp:4979`) does:

```cpp
if (std::ranges::find(voices_, voice) == voices_.end()) { FREEZE_WITH_ERROR(error); }
```

where `voice` is a **reference to an element of `voices_`**. That is a self-comparison, so it can
only fail if the reference no longer aliases a live element. E199 firing on hardware is therefore
proof that `voices_` shrank, was cleared, or reallocated between `acquireVoice()` and the check.

Every sequential path between those two points was checked and none can do it:

- `acquireVoice` (`sound.cpp:4846`) takes `&voices_.back()` **after** its single `push_back`, so the
  reallocation has already happened.
- The non-POLY reuse loop (`sound.cpp:1604-1638`) only erases elements at indices *after*
  `voiceToReuse`'s, which neither a vector shift nor a swap-and-pop can disturb.
- `Voice::unassignStuff` does not touch `voices_`. `Voice::noteOn` returns false only at
  `voice.cpp:308`, and reaches no `voices_` mutator — `AudioEngine::solicitVoiceSample` returns
  `nullptr` on pool exhaustion rather than culling.

So the mutation is necessarily concurrent, and there is exactly one concurrent mutator:
`AudioEngine::cullVoices` → `terminateOneVoice` → `voice->sound.freeActiveVoice(voice)` with the
default `erase = true` → `std::erase(voices_, voice)` (`audio_engine.cpp:345`, `sound.cpp:4874`).

## The three defects, worst first

### 1. Vector reallocation under an in-flight audio-tier iterator

`AudioEngine::terminateOneVoice` (`audio_engine.cpp:320`) iterates every Sound's list:

```cpp
auto all_voices = sounds | std::views::transform(&Sound::voices) | std::views::join;
for (const auto& voice : all_voices) { ... }
```

If a fiber-tier `acquireVoice()` does `voices_.push_back(...)` and reallocates while that loop is in
flight, the loop walks freed memory. `std::erase` from the fiber side corrupts it the same way.
This is memory corruption, not a wrong answer, and it is the reason this document exists rather
than a one-line patch.

### 2. A raw reference held across a preemptible call

`Sound::noteOnPostArpeggiator` (`sound.cpp:1645-1677`) binds `const ActiveVoice& voice` to a slot in
`voices_`, then calls `voice->noteOn(...)`, then uses `voice` again for both the success-path
envelope resume and the failure-path free. The audio tier can erase that element during the call.
On the failure path this is what fires E199; on the success path it silently calls
`resumeAttack` on a recycled `Voice`.

### 3. ABA on pool-recycled `Voice` addresses

`ActiveVoice` comes from an `ObjectPool`, so a culled voice's `Voice` object returns to the pool and
can be re-acquired — possibly by a **different** `Sound` — before the interrupted note-on resumes.
Any fix that re-identifies the voice by raw pointer after the fact (the obvious repair for defect 2)
will therefore sometimes match a different, live voice at the same address and operate on someone
else's note. Identity here needs a generation stamp, not an address.

## What has been done so far (and what it does not do)

`previewSample` now sets `bypassCulling = true` **before** its `Sound::noteOn` rather than after
(`audio_engine.cpp`). Previously the preview voice was cull-eligible for the entire duration of its
own note-on — which, before the `CLUSTER_ENQUEUE` change, included a ~38 ms synchronous fill.

That narrows one window. It does **not** fix any of the three defects: `bypassCulling` is cleared
every render (`audio_engine.cpp:661`), it is a single global rather than a per-Sound guard, and it
does nothing for note-ons that do not go through `previewSample`.

## A device fault that FITS this race — but the one measurement did not support it (2026-08-16)

While browsing sample folders on `next`, the device took a data abort:

```
DABT  PC=201D9ECA  DFAR=7F008084  DFSR=000000F8
```

`PC` resolves to `Patcher::performPatching` (`patcher.cpp:67`), whose first statement is

```cpp
PatchCableSet& patch_cable_set = *param_manager.getPatchCableSet();
```

`DFSR=0xF8` decodes to FS=0x8, a **synchronous external abort**, and `DFAR=0x7F008084` is unmapped.
So the audio render dereferenced a wild `ParamManager`/patch-cable-set while patching a voice — which
is what defect 2 above predicts, and the browser exercises it on every detent:
`previewSample()` → `stopAnyPreviewing()` → `Sound::killAllVoices()` → `voices_.clear()`, all on the
storage owner while the audio ISR may be mid-render.

### Why this is recorded as UNSUPPORTED rather than confirmed

A detector was added (a flag set while the audio tier iterates `voices_`, checked in `killAllVoices`
and `freeActiveVoice`) and ~25 s of sample-folder scrolling produced **zero overlap hits** — despite
the log showing the browser actively previewing (46 `assignClusters fail: acquire LOADING`, 16
soft-culls), so the mutators certainly ran dozens of times. At the audio render's ~35% duty cycle,
dozens of mutations should have yielded roughly ten hits.

That measurement is **inconclusive, not exculpatory**, because it lacked a denominator: it counted
overlaps but not mutations, and not renders that actually reached the voice loop. With no song
playing, `Sound::render` returns early on an empty `voices_`, so the window may simply not have
existed during the test — while the fault itself *requires* a voice being patched, i.e. a state the
test may never have entered.

**To settle it:** count mutations and voice-reaching renders alongside overlaps, and soak with a song
playing so voices exist throughout.

### Other facts about the fault

- **Intermittent**: one occurrence, not reproduced in two subsequent ~30 s attempts.
- It happened with an experimental neighbour-preview prefetch present (speculative `AudioFile` loads
  for adjacent browser entries). That change is NOT in the tree; it is neither convicted nor
  exonerated, and single trials of an intermittent race have too little power to attribute it either
  way.
- The periodic loader dump immediately before the fault showed `loader wait: n 0` — no fills completed
  in the preceding 2 s — so the card was idle at the moment of the abort.

## Options not yet evaluated

1. **Critical sections around every `voices_` structural change and every cross-tier iteration** —
   `CriticalSectionGuard` (`include/libdeluge/system.h:72`) exists. Cheapest to write; needs care
   that no long or blocking work happens inside a guard, and the note-on span is long.
2. **Keep the audio tier out of the list.** Culling posts a *request* (a voice id, or a flag on the
   `Voice`), and the owning tier performs the structural change at a safe point. Fits the tier model
   in the storage-execution-model spec: the audio tier stops reaching into structure it does not own.
3. **Make `voices_` a fixed-capacity array with stable slots** so `push_back` never reallocates and
   removal never shifts. Removes defects 1 and 2 outright and reduces 3 to a generation stamp on the
   slot. Costs the memory of a worst-case allocation per Sound.

Option 2 is the one that matches where the rest of the architecture is heading; option 3 is the one
most likely to be cheap and complete. Neither has been costed.

## Verification this will need

There is no host-side test for this today — the race needs the preemptive executor. Candidates:

- The Embassy host harness under TSan (`preemptive-race-tsan` already exercises playback-while-record
  and would likely see defect 1 if the note-on path were driven concurrently).
- On device: sustained polyphonic playback at a low `maxVoiceCount`, which makes every note-on steal.
