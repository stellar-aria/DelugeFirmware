# Audio Stream — Phase 2b: pure inter-cluster boundary stitch

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the inter-cluster boundary-stitch block from `AudioFileManager::readClusterData` (audio_file_manager.cpp:1044-1223) into a pure `deluge::audio::stream::stitch_boundaries(...)` that operates only on passed-in byte spans + flag references — no reach into `Cluster` objects — leaving `readClusterData` to gather the neighbor edge spans and call it.

**Architecture:** This is the second (and final) half of the reconstruction core (2a did the intra-cluster conversion). The stitch fixes up sample frames that straddle a FAT-cluster boundary for non-native formats; it reads/writes THIS cluster's head+tail+overhang AND its neighbors' edge bytes + their `extraBytesAt*Converted` flags. **Key design (better than the spec's "deltas out" sketch):** each cluster's `data` allocation is over-allocated by `Cluster::size + CACHE_LINE_SIZE`, and prev/next/self buffers are in DISTINCT allocations (no aliasing), so the pure function takes **mutable `std::span`s that view directly into the neighbor clusters' buffers** + the neighbor flags by pointer, and mutates them in place. The function never dereferences a `Cluster` — the caller (`readClusterData` now, `SampleStream` in Phase 4) does the pointer-chasing to build the spans. Zero-copy, still pure, and the same shape a Rust port uses (`&mut [u8]` slices + `&mut bool`).

**Tech Stack:** C++23; CppSpec unit specs (`tests/spec_audio_stream/`); golden-master render gate (`scripts/golden_mixdown.sh`). Depends on Phase 2a's `convert_word` (the stitch converts a straddling word via it) and `RawDataFormat`.

## Global Constraints

- **Naming (house convention):** snake_case functions/methods/variables; CamelCase types; UPPER_CASE enum constants; lower_case members (`_`-suffixed). Port EVERY legacy camelCase local to snake_case (the source is legacy camelCase) — grep the new function for lowercase-then-uppercase and confirm zero remain.
- **Idiomatic C++23**, dependency-light: `stitch.cpp` includes only `stitch.h` + `convert.h` + std headers — NOT AudioEngine/Sample/Cluster. The pure function takes spans + PODs + flag pointers.
- **No C-style callbacks.** Use a template callable (`template <class Yield> … (Yield yield)`), never `void(*)(void*)` + ctx — zero-alloc and idiomatic. (Task 0 converts Phase 2a's `convert_cluster_data` to this; `stitch_boundaries` itself needs no callback — the stitch is short and doesn't yield.)
- **No `goto`** in the port: model the `copy7ToMe` label as a `bool need_copy7` funneling to one shared tail memcpy (see the control-flow sketch in this plan).
- **Preserve the load-bearing ordering subtlety** (§ port notes): in the other-formats misaligned branch, `start_pos` can reach `cluster_size-1`, so the straddling `int32` word overlaps the overhang `[cluster_size, cluster_size+7)`; the overhang MUST be populated (from `next` head or the transient pre-conversion staging) BEFORE `convert_word` reads that word, and then finalized from `next` head. Model as `stage_overhang → convert_straddle_word → finalize_overhang`, not incidental statement order.
- **Behavior-preserving.** Gate: golden **bit-exact** (cordae + icoustic) + unit specs. `highsiderr` KNOWN-STALE (A/B if it fails, don't block). Keep structs byte-stable.

---

## File Structure

- `src/deluge/storage/audio/stream/stitch.h` — `StitchPrevEdge`, `StitchNextEdge`, `stitch_boundaries` decl.
- `src/deluge/storage/audio/stream/stitch.cpp` — the pure port.
- `src/deluge/storage/audio/audio_file_manager.cpp` — `readClusterData` gathers edge spans + calls `stitch_boundaries` (replaces lines 1044-1223).
- `tests/spec_audio_stream/stitch_spec.cpp` — CppSpec specs (own file — the harness dispatches ctest by file stem).

---

## The interface

```cpp
// stitch.h
#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::audio::stream {

// Edge of the PREVIOUS cluster: a mutable view of prevCluster->data over [cluster_size-4, cluster_size+7)
// (11 bytes) — so tail[4+k] == prevCluster->data[cluster_size+k], tail[0..4) == prevCluster->data[size-4..size).
// `end_boundary_converted` points at prevCluster->extraBytesAtEndConverted.
struct StitchPrevEdge {
	std::span<std::byte> tail;        // 11 bytes: prevCluster->data[cluster_size-4 .. cluster_size+7)
	bool* end_boundary_converted;
};

// Edge of the NEXT cluster: a mutable view of nextCluster->data[0..7), plus a read-only view of
// nextCluster->firstThreeBytesPreDataConversion[0..3). `start_boundary_converted` points at
// nextCluster->extraBytesAtStartConverted.
struct StitchNextEdge {
	std::span<std::byte> head;                       // 7 bytes: nextCluster->data[0..7)
	std::span<const std::byte, 3> unconverted_head;   // nextCluster->firstThreeBytesPreDataConversion
	bool* start_boundary_converted;
};

// Pure boundary stitch for ONE cluster, matching audio_file_manager.cpp:1044-1223. Mutates `self_data`
// (this cluster's data + overhang; must be cluster_size+7 bytes usable), the neighbor edge spans, and
// the four flags, all in place. `prev`/`next` are nullptr when that neighbor is absent-or-not-loaded
// (the caller only supplies a loaded neighbor). No Cluster/Sample/AudioEngine reach.
void stitch_boundaries(std::span<std::byte> self_data, int32_t cluster_index, RawDataFormat format,
                       uint32_t audio_data_start_pos_bytes, size_t cluster_size,
                       bool& self_start_boundary_converted, bool& self_end_boundary_converted,
                       StitchPrevEdge* prev, StitchNextEdge* next);

} // namespace deluge::audio::stream
```

---

## Task 0: rework `convert_cluster_data` yield to a template callable (+ TODO note)

Phase 2a's `convert_cluster_data` takes a C-style `void(*)(void*)` yield + `void* ctx`. The ctx is dead weight in production (the real yield lambda is captureless; ctx is only used by the test), and the C-ism is out of place. Convert it to a template callable. The yield itself is a cooperative-scheduling shim — it exists only because the conversion runs cooperatively on the audio context; under preemptive audio (the Embassy `InterruptExecutor`) it's unnecessary — so also record its removal in TODO.md. Behavior-identical (golden bit-exact).

**Files:**
- Modify: `src/deluge/storage/audio/stream/convert.h`, `src/deluge/storage/audio/stream/convert.cpp`
- Modify: `src/deluge/storage/cluster/cluster.cpp` (the wrapper's yield lambda)
- Modify: `tests/spec_audio_stream/convert_cluster_spec.cpp` (the yield-counter case)
- Modify: `TODO.md`

**Interfaces:**
- Produces: `template <class Yield> void convert_cluster_data(std::span<std::byte> data, int32_t cluster_index, RawDataFormat format, ConvertGeometry geometry, size_t cluster_size, size_t cluster_size_magnitude, std::span<std::byte, 3> unconverted_head_out, Yield yield);` — the definition moves INTO `convert.h` (templates are header-defined). The `YieldFn` typedef and the `void* yield_ctx` param are removed. Body calls `yield();` at the two former yield sites (no null check — callers always pass a callable; pass `[]{}` for no-op).

- [ ] **Step 1: Move `convert_cluster_data` into `convert.h` as a template.** Cut its definition from `convert.cpp` and paste into `convert.h` as `template <class Yield> void convert_cluster_data(...)`, replacing the two `if (yield) yield(yield_ctx);` sites with `yield();`. Delete the `using YieldFn = ...;` typedef. `convert.h` will need the includes the body uses (`<algorithm>`, `<bit>`, `<cstdint>`, `<span>`, `<cstddef>`); it still must NOT include AudioEngine/Sample/Cluster. `convert.cpp` keeps only `convert_word` (the template calls `convert_word`, declared in the same header — link resolves it). Add a doc comment on `convert_cluster_data`: the `yield` is a cooperative-scheduling shim, called ~every 1024 bytes to pump the audio routine during a long conversion; unnecessary (and removable) once audio is preemptively scheduled — see TODO.md.

- [ ] **Step 2: Update the wrapper** in `cluster.cpp` (`Cluster::convertDataIfNecessary`): change the yield argument from `[](void*) { AudioEngine::logAction("from convert-data"); AudioEngine::runRoutine(); }, nullptr` to `[] { AudioEngine::logAction("from convert-data"); AudioEngine::runRoutine(); }` (captureless, no ctx arg). Keep the existing comment about the deliberate log widening.

- [ ] **Step 3: Update the spec** `convert_cluster_spec.cpp`: the yield-counter case becomes `int count = 0; convert_cluster_data(..., [&] { count++; });` (capturing lambda); the other cases pass `[] {}` (no-op). Confirm the counter assertion still holds.

- [ ] **Step 4: Add the TODO.md note.** Append to `TODO.md`:
```
- [] remove convert_cluster_data's `yield` callback + the AudioEngine pump lambda in Cluster::convertDataIfNecessary once AudioEngine::routine()/runRoutine() runs on the preemptive Embassy InterruptExecutor task — the cooperative mid-conversion yield is unnecessary under preemption (see src/deluge/storage/audio/stream/convert.h)
```

- [ ] **Step 5: Build, test, golden.** `dbt build Debug` clean; `./dbt test` (19/19 — convert_cluster cases still green with the new lambda-based yield); `scripts/golden_mixdown.sh check` (cordae bit-exact) + `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact). Template-vs-fnptr and the identical lambda body make this behavior-identical.

- [ ] **Step 6: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp \
        src/deluge/storage/cluster/cluster.cpp tests/spec_audio_stream/convert_cluster_spec.cpp TODO.md
git commit -m "refactor(audio-stream): convert_cluster_data yield -> template callable; TODO note re preemption"
```

---

## Task 1: the pure `stitch_boundaries` port + specs

**Files:**
- Create: `src/deluge/storage/audio/stream/stitch.h`, `src/deluge/storage/audio/stream/stitch.cpp`
- Create: `tests/spec_audio_stream/stitch_spec.cpp`

**Interfaces:**
- Consumes: `convert_word` (Phase 2a), `RawDataFormat`.
- Produces: `stitch_boundaries(...)` per the interface above.

- [ ] **Step 1: Write failing specs** (`stitch_spec.cpp`, own file, `describe stitch(...)` + `CPPSPEC_SPEC(stitch)`). Use a small synthetic `cluster_size` (e.g. 32) and byte-exact assertions. Cases:
  - (a) **No neighbors** (`prev=nullptr, next=nullptr`): `self_data` unchanged, `self_start_boundary_converted`/`self_end_boundary_converted` NOT set (they're set only inside the neighbor-present branches). Assert.
  - (b) **NATIVE, both neighbors present**: no conversion happens; `prev.tail[4..11)` (== prev overhang `[size..size+7)`) becomes `self_data[0..7)`; `self_data[size..size+7)` becomes `next.head[0..7)`; `self_start_boundary_converted` and `self_end_boundary_converted` both become true; prev/next flags untouched. Assert exact bytes + flags.
  - (c) **UNSIGNED_8 (a simple non-native), misaligned (audio_data_start_pos_bytes=1), both neighbors present, both neighbor flags false**: the prev-half converts the straddling word in `prev.tail` and copies 3 bytes back to `self_data[0..3)`, sets `*prev.end_boundary_converted`; the next-half stages the overhang, converts the straddling word in `self_data`'s tail, writes 3 bytes to `next.head[0..3)`, sets `*next.start_boundary_converted`. Hand-derive the expected bytes from the control-flow sketch + `convert_word(w, UNSIGNED_8) = w ^ 0x80808080`. (This is the hard case — derive it carefully; the golden gate in Task 2 is the backstop.)
  - (d) **Idempotency via flags**: next-half with `*next.start_boundary_converted == true` uses `next.unconverted_head` (not `next.head`) as the staging source and takes the `need_copy7` fallthrough. Assert it reads `unconverted_head`.

- [ ] **Step 2: Run, expect FAIL** (`stitch.h`/`stitch_boundaries` missing). Run: `./dbt test`.

- [ ] **Step 3: Implement `stitch_boundaries`** in `stitch.{h,cpp}`, porting audio_file_manager.cpp:1044-1223 with these rules:
  - **Substitutions:** `sample->rawDataFormat`→`format`; `sample->audioDataStartPosBytes`→`audio_data_start_pos_bytes`; `clusterIndex`→`cluster_index`; `Cluster::size`→`cluster_size`; `cluster.data[k]`→`self_data[k]` (span; the overhang `cluster.data[size+k]` is `self_data[cluster_size+k]`, valid because `self_data` is `cluster_size+7`); `sample->convertToNative(w)`→`convert_word(w, format)`.
  - **Prev neighbor:** the `if (clusterIndex > 0)` + `if (prevCluster && prevCluster->loaded)` gate becomes `if (prev != nullptr)` (the caller only passes a loaded prev, and only when `cluster_index > 0`). `prevCluster->data[cluster_size + k]` → `prev->tail[4 + k]`; `prevCluster->data[start_pos]` where `start_pos = cluster_size - 4 + misalignment` (or `cluster_size - bytes_unconverted` for 24-bit) → `prev->tail[start_pos - (cluster_size - 4)]` (i.e. index into the 11-byte tail; note the 24-bit `start_pos = cluster_size - bytes_unconverted` with `bytes_unconverted ∈ {1,2}` maps to `prev->tail[4 - bytes_unconverted]`). `prevCluster->extraBytesAtEndConverted` → `*prev->end_boundary_converted`.
  - **Next neighbor:** `if (clusterIndex < clusters.size()-1)` + `if (nextCluster && nextCluster->loaded)` → `if (next != nullptr)`. `nextCluster->data[k]` (k∈[0,7)) → `next->head[k]`; `nextCluster->firstThreeBytesPreDataConversion` → `next->unconverted_head`; `nextCluster->extraBytesAtStartConverted` → `*next->start_boundary_converted`.
  - **Self flags:** `cluster.extraBytesAtStartConverted = true` (line 1109) → `self_start_boundary_converted = true`; `cluster.extraBytesAtEndConverted = true` (line 1221) → `self_end_boundary_converted = true`. (Do NOT set `cluster.loaded` / call `mark_ready` — those stay in `readClusterData`, after the stitch.)
  - **The goto:** replace `goto copy7ToMe;` with `need_copy7 = true;` and, at the end of the next-half, `if (need_copy7) memcpy(&self_data[cluster_size], next->head.data(), 7);` (the `copy7ToMe` body).
  - **Ordering subtlety:** preserve the `stage_overhang → convert_straddle_word → finalize_overhang` data dependency in the other-formats misaligned arms (see the control-flow sketch below and the Global Constraints).
  - **Naming:** snake_case EVERY local (`misalignment`, `start_pos`, `bytes_unconverted_before_cluster`, `bytes_unconverted_before_next_cluster`, `need_copy7`, `temp`, …). Grep-confirm zero camelCase.
  - `stitch.cpp` includes only `stitch.h`, `convert.h`, and std (`<cstring>` for memcpy, `<cstddef>`). NO AudioEngine/Sample/Cluster.

  **Control-flow sketch (goto-free) — reproduce faithfully:**
  ```
  misalignment = audio_data_start_pos_bytes & 0b11
  // PREV HALF:
  if prev != nullptr:
      copy self_data[0..7) -> prev->tail[4..11)                      // prev overhang refresh (unconditional)
      if format == WRONG_24 and !*prev->end_boundary_converted:
          bytes_unconverted = (cluster_index*cluster_size - audio_data_start_pos_bytes) % 3
          if bytes_unconverted != 0:
              byteswap3 the word at prev->tail[4 - bytes_unconverted ..]
              copy prev->tail[4..6) -> self_data[0..2)
          *prev->end_boundary_converted = true
      elif format != NATIVE and !*prev->end_boundary_converted:
          if misalignment != 0:
              start_idx = 4 - 4 + misalignment  // = misalignment; word at prev->tail[misalignment..+4)
              prev->tail[misalignment..+4) = convert_word(that int32, format)
              copy prev->tail[4..7) -> self_data[0..3)
          *prev->end_boundary_converted = true
      // NATIVE: nothing
      self_start_boundary_converted = true                               // ALWAYS (prev present)
  // NEXT HALF:
  if next != nullptr:
      need_copy7 = false
      if format == WRONG_24:
          bytes_unconverted_next = ((cluster_index+1)*cluster_size - audio_data_start_pos_bytes) % 3
          if bytes_unconverted_next != 0:
              if !*next->start_boundary_converted: self_data[size..size+7) = next->head[0..7)   // stage
              else:                             self_data[size..size+2) = next->unconverted_head[0..2)
              byteswap3 word at self_data[size - bytes_unconverted_next ..]
              if !*next->start_boundary_converted:
                  *next->start_boundary_converted = true
                  next->head[0..2) = self_data[size..size+2)
              else: need_copy7 = true
          else: need_copy7 = true
      elif format != NATIVE:
          if misalignment != 0:
              start_pos = size - 4 + misalignment
              if !*next->start_boundary_converted:
                  self_data[size..size+7) = next->head[0..7)                    // stage (before convert!)
                  self_data[start_pos..+4) = convert_word(that int32, format)   // reads staged overhang
                  next->head[0..3) = self_data[size..size+3)
                  *next->start_boundary_converted = true
              else:
                  self_data[size..size+3) = next->unconverted_head[0..3)         // transient stage
                  self_data[start_pos..+4) = convert_word(that int32, format)   // reads transient
                  need_copy7 = true                                             // finalize overrides transient
          else: need_copy7 = true
      else: need_copy7 = true   // NATIVE
      if need_copy7: self_data[size..size+7) = next->head[0..7)         // copy7ToMe
      self_end_boundary_converted = true                                   // ALWAYS (next present)
  ```
  Verify this sketch line-by-line against the actual audio_file_manager.cpp:1044-1223 as you port — the sketch is a guide, the source is the truth.

- [ ] **Step 4: Run specs, expect PASS.** `./dbt test` — stitch cases green.

- [ ] **Step 5: Commit.**
```bash
git add src/deluge/storage/audio/stream/stitch.h src/deluge/storage/audio/stream/stitch.cpp \
        tests/spec_audio_stream/stitch_spec.cpp
git commit -m "feat(audio-stream): pure stitch_boundaries core for inter-cluster boundary fixups"
```

---

## Task 2: wire `readClusterData` to gather edges + call `stitch_boundaries`

**Files:**
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (`readClusterData`, replace lines 1044-1223)

**Interfaces:**
- Consumes: `stitch_boundaries` + the edge structs (Task 1).

- [ ] **Step 1: Replace the stitch block** (audio_file_manager.cpp:1044-1223) with a gather-and-call. Build the edge structs from the neighbor clusters, then call `stitch_boundaries`:
  - `prev`: if `clusterIndex > 0` and `sample->clusters[clusterIndex-1].cluster` is non-null and `->loaded`, construct `StitchPrevEdge{ .tail = span(&prevCluster->data[Cluster::size - 4], 11), .end_boundary_converted = &prevCluster->extraBytesAtEndConverted }`; else pass `nullptr`.
  - `next`: if `clusterIndex < (int32_t)sample->clusters.size() - 1` and `nextCluster` non-null and `->loaded`, construct `StitchNextEdge{ .head = span(nextCluster->data, 7), .unconverted_head = span(nextCluster->firstThreeBytesPreDataConversion, 3), .start_boundary_converted = &nextCluster->extraBytesAtStartConverted }`; else `nullptr`.
  - self span: `span(reinterpret_cast<std::byte*>(cluster.data), Cluster::size + 7)`.
  - Call `deluge::audio::stream::stitch_boundaries(self_span, clusterIndex, sample->rawDataFormat, sample->audioDataStartPosBytes, Cluster::size, cluster.extraBytesAtStartConverted, cluster.extraBytesAtEndConverted, prevPtr, nextPtr)`.
  - Keep `cluster.loaded = true;` and the `deluge_resource_mark_ready` block (lines 1225+) exactly as-is, AFTER the call. Add the `storage/audio/stream/stitch.h` include.

- [ ] **Step 2: Build + golden.** `dbt build Debug` clean. `scripts/golden_mixdown.sh check` (cordae bit-exact) + `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact). The non-native-format boundary fixups are what this exercises — if either diverges, diff your gathered spans/offsets against the original neighbor accesses (esp. the `prev->tail` index base `cluster_size-4` and the overhang `cluster_size+k`). `FIXTURE=highsiderr` KNOWN-STALE — A/B if it fails, don't block.

- [ ] **Step 3: Commit.**
```bash
git add src/deluge/storage/audio/audio_file_manager.cpp
git commit -m "refactor(audio-stream): route readClusterData's boundary stitch through stitch_boundaries"
```

---

## Roadmap — after 2b

The reconstruction core (read via ReadSource + convert + stitch) is now fully pure and Rust-portable. Next: Phase 3 (de-overload `Cluster` into `StreamedChunk`/`ComputedChunk`), Phase 4 (`SampleStream` — which will own the neighbor-gather/apply that `readClusterData` does today, and hold the `ReadSource` per Phase-1's deferred alloc note), Phase 5 (loader consolidation), then follow-ons A/B. Per the module design spec.
