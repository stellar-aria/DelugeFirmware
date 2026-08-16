# Deadline-ordered streaming loader

**Status:** designed, not implemented.
**Date:** 2026-08-16.
**Supersedes nothing.** Related: `2026-08-08-storage-execution-model-end-state-design.md` (tier
rules), `2026-08-15-async-note-on-residency-design.md` (the note-on residency work that exposed
this), `2026-08-16-voices-cross-tier-race.md` (unrelated defect found in the same investigation).

## Problem

The streaming loader queue is ordered by **musical importance**, not by **when the data is needed**.

`ManagerResidency::acquire` passes its caller's `priority` straight to `Resource::loader_enqueue`
(`crates/deluge_sample_source/src/manager_residency.rs:122`), and that value originates as
`Voice::getPriorityRating()` (`src/deluge/model/voice/voice.cpp:2555`) — a packed composite of
manual voice priority, the Sound's voice count, envelope state, and time-entered-state. It is the
same number `AudioEngine::terminateOneVoice` uses to decide what to cull.

`Manager::loader_next` then serves the lowest value first. So a voice five milliseconds from running
dry waits behind a musically-more-important voice with 180 ms of slack. That is a priority inversion
with respect to deadlines, and it produces underruns under load while total bandwidth is nowhere near
exhausted.

### Evidence

Measured on device 2026-08-16, after the chunk-table indexing removed the scan overhead that had been
masking this:

```
5 × sample_low_level_reader.cpp: late or reached end of waveform. last Cluster was: N
5 × sample_low_level_reader.cpp: next failed
```

Each pair is one streaming voice terminated mid-note. In the same capture there was exactly **one**
force-cull, so these are not the engine shedding load — they are the loader delivering the wrong
chunk first.

The bandwidth arithmetic says this is a scheduling problem, not a capacity one. A 32 KB cluster
(`Cluster::size_magnitude == 15`) is ~186 ms of 44.1 kHz stereo 16-bit audio. Card reads measured at
roughly 3.8 MB/s (approximate: derived from a 38 ms two-cluster fill once the ~21 ms of table
scanning is subtracted), so one cluster takes ~8.6 ms — a ~20× margin per voice, and on the order of
20 concurrent streams before saturation. Normal polyphony is far below that.

## Non-goal: eliminating underruns entirely

Underruns cannot be made impossible on SD. Read latency has an unbounded tail — a card can stall for
tens of milliseconds doing internal garbage collection — and no scheduling policy prevents that. The
handler therefore has to exist regardless, which means the correct posture is:

1. make deadline misses rare (this document),
2. degrade gracefully when one happens anyway.

Today's degradation is `moveOnToNextCluster` setting `currentPlayPos = nullptr` and terminating the
voice. Stalling a voice for a render is strictly better than killing it in every case. **That change
is NOT in this document's scope** — it is tracked separately — but it is the floor this design
assumes underneath it, and neither change makes the other unnecessary.

## Design

### The encoding

`priority` becomes an **absolute deadline**: the `AudioEngine::audioSampleTimer` value by which the
chunk must be resident. Lower = sooner = more urgent, which is already how `loader_next` orders, so
the queue mechanism itself is unchanged.

Absolute rather than relative, because a relative deadline recorded at enqueue time goes stale:
entries enqueued at different moments age by different amounts, and the queue only backs up under
exactly the load where ordering matters. Absolute deadlines do not age, so no re-enqueueing or
periodic refresh is needed.

Two sentinels fall out of the existing values unchanged:

| Value | Meaning | Already used by |
| --- | --- | --- |
| `0` | needed immediately | `BLOCKING_FILL_PRIORITY` (`streaming_loader.rs`) |
| `u32::MAX` | speculative; no deadline | the warm-hint reservation path (`reservation.rs`, `enqueue_unfilled`) |

`u32::MAX` must keep sorting last, because `Manager::loader_has_lowest` keys off it exactly
(`queue_priority == u32::MAX`) and `load_song_ui.cpp:481` waits on that predicate to know when
speculative prefetching has drained.

### Wrap handling

`audioSampleTimer` is a `uint32_t` and wraps roughly every 27 hours of rendered audio, so
`loader_next`'s plain `a < b` would invert across the boundary.

`loader_next` changes to a wrap-safe signed-difference comparison — `(a - b) as i32 < 0` — with
`u32::MAX` special-cased so the speculative sentinel still sorts last rather than reading as "just
behind the current time".

**`audioSampleTimer` is deliberately NOT widened to 64 bits.** It has 168 uses across ~30 files, and
33 of them already rely on the `(int32_t)(a - b)` wrap-safe idiom that widening would silently
break. More seriously, `audioSampleTimer += numSamples` runs in the audio ISR while UI and fiber
contexts read it; a 64-bit read on this 32-bit ARM is not atomic, so widening would introduce a
tearing race on the most widely-read timer in the firmware in order to fix a once-per-27-hours
ordering blip. The signed-difference idiom is both correct and what the codebase already does in 33
places.

If a monotonic 64-bit clock is ever wanted, the right shape is a separate counter owned by the
storage path — not widening the global the audio ISR writes.

### Computing the deadline

`SampleLowLevelReader` gains one field: the phase increment it last rendered with, updated per
render. The deadline for the next cluster is then

```
source_frames_left  = bytes_remaining_in_current_cluster / bytes_per_frame
frames_until_needed = source_frames_left × kMaxSampleValue / phaseIncrement
deadline            = audioSampleTimer + frames_until_needed   (saturating below u32::MAX)
```

`phaseIncrement` is Q24 with `kMaxSampleValue == 1 << 24` meaning 1:1 playback
(`definitions_cxx.hpp:1023`), so `× kMaxSampleValue / phaseIncrement` converts source frames to
output frames — the same conversion `SampleHolder::getLengthInSamplesAtSystemSampleRate` already
performs as `(lengthInSamples << 24) / neutralPhaseIncrement`. A faster (higher) phase increment
yields a nearer deadline, which is the intended behaviour.

The saturation clamp stops **below** `u32::MAX`: that value means "speculative", so letting an
overflow reach it would invert a very-distant chunk into the least urgent thing in the queue instead
of merely a distant one.

A cached field rather than threading `phaseIncrement` down to the enqueue sites: `moveOnToNextCluster`
and `changeClusterIfNecessary` do not take it (only `considerUpcomingWindow` does), so threading it
would churn several signatures for a value at most one render (~2.9 ms) stale against a ~186 ms
cluster.

Assuming a **fixed** rate instead was considered and rejected. It reduces to ordering by bytes
remaining, which is correct only while every voice plays at the same speed — and a pitched-up voice,
which consumes its cluster faster, is precisely the one most likely to underrun.

### Policy: pure EDF

Order strictly by deadline. Musical importance no longer influences load order at all; it continues
to drive culling (`terminateOneVoice`, `getPriorityRating`), which is where it belongs.

A background voice about to run dry therefore beats a lead with slack. That is the intended
behaviour: the lead has time to be served afterwards and will not glitch, whereas the background
voice would.

Pure EDF does not starve: a stream's deadline advances as it plays, and a missed deadline clamps to
`0` (maximum urgency), so a late chunk always sorts ahead of on-time work.

Banded EDF and an importance floor were both considered. The floor reintroduces exactly the
inversion causing the underruns. Banding adds tunable policy that would need justifying against
measurements we do not have.

### Explicitly out of scope for the first cut

**Time-stretched voices' true consumption rate.** The stretcher consumes through its own readers at
a rate that is not simply the phase increment, so their computed deadline will be wrong — in an
unknown direction, since nobody has measured what the stretcher's real rate is.

They nonetheless use the same computation as everything else, deliberately. The alternative
considered was a fixed stand-in constant, and it was rejected on two grounds: the reader has no
`TimeStretcher` available at `deadlineForNextCluster`, so detecting the case would need new plumbing;
and an unmeasured magic number is not obviously better than a wrong-but-principled computation — it
just moves the error somewhere harder to notice. A wrong deadline for a stretched voice degrades to
roughly today's behaviour for that voice, which is the status quo, not a regression.

This is a known gap. Closing it starts with measuring the stretcher's consumption rate, not with
picking a constant.

**Lookahead depth.** The cursor holds one standing prefetch. Deeper lookahead would absorb more
scheduling jitter and is the natural follow-up, but it is a separate change with its own memory cost
and should be judged against a re-measured underrun count.

**Admission control.** The only mechanism that actually *guarantees* a deadline is refusing to start
a stream when projected demand exceeds sustainable bandwidth. Out of scope; noted because it, not
EDF, is what a hard guarantee would require.

## Files affected

| File | Change |
| --- | --- |
| `crates/deluge_resource/src/manager.rs` | `loader_next` wrap-safe comparison; `u32::MAX` sorts last |
| `crates/deluge_resource/include/deluge_resource.h` | `loader_enqueue` priority docs: deadline semantics |
| `include/libdeluge/sample_source.h` | `priority` param docs on `acquire`/`acquire_ex` |
| `crates/deluge_sample_source/src/cursor.rs` | pass the deadline through to `prefetch_neighbour` |
| `src/deluge/model/sample/sample_low_level_reader.h/.cpp` | cached phase increment; deadline computation |
| `src/deluge/model/voice/voice.cpp` | stop passing `getPriorityRating()` as the loader priority |

## Testing

**Unit, no hardware:**

- `deluge_resource`: `loader_next` returns the earliest deadline; ties are stable; `u32::MAX` sorts
  last even against a large deadline; ordering survives the `audioSampleTimer` wrap (a deadline just
  past the wrap beats one just before it).
- CppSpec: the deadline computation — a faster phase increment yields a nearer deadline; an
  already-late chunk clamps to 0; saturation never produces `u32::MAX` by accident, since that value
  means "speculative" and would invert the chunk's urgency.

**On device:** count `late or reached end of waveform` lines in an RTT capture over the same song and
duration. The pre-change baseline is **5** in a ~40 s playback window with one force-cull (capture of
2026-08-16). Success is that count dropping toward zero without a rise in force-culls, which would
indicate the loader is now stealing CPU rather than scheduling better.

## Risks

- **A wrong deadline is worse than no deadline.** If the computation underestimates urgency, a voice
  underruns sooner than it does today. The unit spec on the computation is the guard, and the
  saturation rule exists so an overflow can never silently mean "speculative".
- **Ordering changes are hard to attribute by ear.** The RTT counter, not listening, is the
  measurement of record.
- **The cached phase increment is stale by up to one render.** Bounded and small (~2.9 ms against
  ~186 ms), but it means a voice whose pitch jumps sharply upward gets one slightly-late deadline.
