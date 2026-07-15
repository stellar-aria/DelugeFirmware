# Audio Stream — Phase 2a: pure format-conversion core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the intra-cluster format conversion (`Cluster::convertDataIfNecessary`, cluster.cpp:60-153) into a pure, unit-tested `deluge::audio::stream` core — a `convertWord(word, format)` functor and a `convertClusterData(...)` function with a fn-pointer yield callback — leaving `Cluster::convertDataIfNecessary` a thin wrapper.

**Architecture:** Phase 2 of the audio-stream migration splits into **2a (this plan) — the pure intra-cluster convert** and **2b (later) — the inter-cluster boundary stitch**. The split exists because exploration showed the two differ fundamentally: the *convert* reads only immutable-per-sample PODs and its sole impurity is a mid-loop `AudioEngine::runRoutine()` yield (cleanly pure-able), whereas the *stitch* reaches into and **mutates neighbor clusters** (their head/tail bytes + `extraBytesAt*Converted` flags), so it stays an orchestration-over-neighbors operation deferred to 2b. This plan does 2a only. `convertToNative(int32_t)` already switches only on `rawDataFormat` — it's already a pure `convertWord` functor, with exact card-free per-format test vectors.

**Tech Stack:** C++23; CppSpec unit specs (mirroring `tests/spec_audio_stream/`); golden-master render gate (`scripts/golden_mixdown.sh`, fixtures cordae/icoustic/highsiderr). `RawDataFormat` lives at `src/deluge/storage/audio/audio_file_format.h:30-37`. Conversion helpers: `util/audio_format_helpers.h` (`swapEndianness32`, `swapEndianness2x16`), `util/fixedpoint.h` (`q31_from_float`).

## Global Constraints

From the design spec (`docs/superpowers/specs/2026-07-15-audio-stream-module-design.md`); every task inherits these:

- **Idiomatic modern C++23**, in-tree. The pure core is dependency-light (POD in/out) — no reach into `Sample`/`Cluster`/`AudioEngine` object graphs; those are gathered by the thin wrapper and passed in.
- **The yield callback is a plain `void(*)(void*)` fn-pointer + ctx** — NOT `std::function` (per the Phase-1 lesson that per-call heap allocation perturbs allocation timing / goldens). Zero-alloc, and the portable/ABI shape the future Rust core will use.
- **Behavior-preserving.** 2a is a pure code-move + a DRY delegation; gate is golden **bit-exact** (cordae + icoustic) plus the new unit specs. `highsiderr` is KNOWN-STALE (pre-existing-divergent — a fail there is likely not this work's regression; verify via git-stash A/B, do not block). Keep structs byte-stable (run `padsweep` if any struct changes size — none should here).
- No new C ABI; the residency/read seams from Phase 0-1 are untouched.

---

## File Structure

- `src/deluge/storage/audio/stream/convert.h` — `convertWord`, `YieldFn`, `ConvertGeometry`, `convertClusterData` declarations.
- `src/deluge/storage/audio/stream/convert.cpp` — their definitions (ported from cluster.cpp:60-153 + sample.h's `convertToNative`).
- `src/deluge/model/sample/sample.h` — `Sample::convertToNative(int32_t)` delegates to `convertWord` (DRY; single source of truth).
- `src/deluge/storage/cluster/cluster.cpp` — `Cluster::convertDataIfNecessary` becomes a thin wrapper calling `convertClusterData`.
- `tests/spec_audio_stream/convert_spec.cpp` — CppSpec unit specs for `convertWord` (6 per-format vectors) and `convertClusterData` (both loops, backup output, early-outs).

---

## Task 1: `convertWord` pure functor + `Sample::convertToNative` delegation

**Files:**
- Create: `src/deluge/storage/audio/stream/convert.h`, `src/deluge/storage/audio/stream/convert.cpp`
- Modify: `src/deluge/model/sample/sample.h` (`convertToNative(int32_t)`)
- Create: `tests/spec_audio_stream/convert_spec.cpp`

**Interfaces:**
- Produces: `int32_t deluge::audio::stream::convertWord(int32_t word, RawDataFormat format)` — the exact switch body of `Sample::convertToNative(int32_t)` (sample.h:100-123). `ENDIANNESS_WRONG_24` and `NATIVE` return `word` unchanged; `FLOAT` → `q31_from_float(std::bit_cast<float>(word))`; `ENDIANNESS_WRONG_32` → `swapEndianness32`; `ENDIANNESS_WRONG_16` → `swapEndianness2x16`; `UNSIGNED_8` → `word ^ 0x80808080`.

- [ ] **Step 1: Write the failing spec** (`tests/spec_audio_stream/convert_spec.cpp`, mirroring `read_source_spec.cpp`'s DSL — quoted `"cppspec.hpp"`, `describe`/`it`/`expect`, trailing `CPPSPEC_SPEC(convert)`):

```cpp
// tests/spec_audio_stream/convert_spec.cpp
#include "storage/audio/stream/convert.h"

#include "cppspec.hpp"

#include <bit>
#include <cstdint>

using namespace deluge::audio::stream;

// clang-format off
describe convert("convertWord", $ {
	it("NATIVE is identity", _ { expect(convertWord(0x11223344, RawDataFormat::NATIVE)).to_equal(0x11223344); });
	it("ENDIANNESS_WRONG_24 is a no-op (3-byte swap is done cluster-wide)", _ {
		expect(convertWord(0x11223344, RawDataFormat::ENDIANNESS_WRONG_24)).to_equal(0x11223344); });
	it("ENDIANNESS_WRONG_32 reverses all 4 bytes", _ {
		expect(convertWord(0x01020304, RawDataFormat::ENDIANNESS_WRONG_32)).to_equal(0x04030201); });
	it("ENDIANNESS_WRONG_16 swaps within each half-word", _ {
		expect(convertWord(0x01020304, RawDataFormat::ENDIANNESS_WRONG_16)).to_equal(0x02010403); });
	it("UNSIGNED_8 flips the MSB of every byte", _ {
		expect(convertWord(0x00112233, RawDataFormat::UNSIGNED_8)).to_equal(0x8091A2B3); });
	it("FLOAT 0.5f maps to Q31 0x40000000", _ {
		expect(convertWord(std::bit_cast<int32_t>(0.5f), RawDataFormat::FLOAT)).to_equal(0x40000000); });
});

CPPSPEC_SPEC(convert)
```

Note: confirm the exact `swapEndianness2x16` / `q31_from_float` results against the real helpers while implementing; if the FLOAT-saturation or 16-swap constant differs, correct the expected value to match the actual (verbatim-faithful) helper output — the vector must assert the real function's behavior, not a guess.

- [ ] **Step 2: Run the spec, expect FAIL** (target/symbol missing). Run: `./dbt test` (or `ctest -R convert`). Expected: FAIL — `convert.h` / `convertWord` not found.

- [ ] **Step 3: Implement `convert.h` + `convert.cpp`**

`convert.h`:
```cpp
#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstdint>

namespace deluge::audio::stream {
// Pure per-word format conversion — the body of Sample::convertToNative(int32_t), depending only on the
// format. ENDIANNESS_WRONG_24 and NATIVE return the word unchanged (the 24-bit 3-byte swap is done
// group-wise in convertClusterData, added in Task 2).
int32_t convertWord(int32_t word, RawDataFormat format);
} // namespace deluge::audio::stream
```

`convert.cpp` — port the `convertToNative(int32_t)` switch verbatim (include `util/audio_format_helpers.h`, `util/fixedpoint.h`, `<bit>`):
```cpp
#include "storage/audio/stream/convert.h"
#include "util/audio_format_helpers.h"
#include "util/fixedpoint.h"
#include <bit>

namespace deluge::audio::stream {
int32_t convertWord(int32_t word, RawDataFormat format) {
	switch (format) {
	case RawDataFormat::FLOAT:            return q31_from_float(std::bit_cast<float>(word));
	case RawDataFormat::ENDIANNESS_WRONG_32: return swapEndianness32(word);
	case RawDataFormat::ENDIANNESS_WRONG_16: return swapEndianness2x16(word);
	case RawDataFormat::UNSIGNED_8:      return word ^ 0x80808080;
	case RawDataFormat::ENDIANNESS_WRONG_24: [[fallthrough]];
	case RawDataFormat::NATIVE:          break;
	}
	return word;
}
} // namespace deluge::audio::stream
```

- [ ] **Step 4: Delegate `Sample::convertToNative(int32_t)` to `convertWord`** (DRY — single source of truth). In `sample.h`, replace the switch body of `convertToNative(int32_t value) const` with `return deluge::audio::stream::convertWord(value, rawDataFormat);` (add the `storage/audio/stream/convert.h` include). Leave the `float` overload untouched.

- [ ] **Step 5: Run the spec, expect PASS.** Run: `./dbt test`. Expected: `convert` cases PASS; full suite green.

- [ ] **Step 6: Build firmware + golden gate.** Run: `dbt build Debug` (clean); `scripts/golden_mixdown.sh check` (cordae bit-exact PASS); `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact PASS). Delegation is behavior-identical, so both must stay bit-exact.

- [ ] **Step 7: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp \
        src/deluge/model/sample/sample.h tests/spec_audio_stream/convert_spec.cpp
git commit -m "feat(audio-stream): pure convertWord functor; Sample::convertToNative delegates to it"
```

---

## Task 2: `convertClusterData` pure function + `Cluster::convertDataIfNecessary` wrapper

**Files:**
- Modify: `src/deluge/storage/audio/stream/convert.h`, `src/deluge/storage/audio/stream/convert.cpp`
- Modify: `src/deluge/storage/cluster/cluster.cpp` (`convertDataIfNecessary`)
- Modify: `tests/spec_audio_stream/convert_spec.cpp`

**Interfaces:**
- Consumes: `convertWord` (Task 1).
- Produces:
  ```cpp
  using YieldFn = void (*)(void* ctx);
  struct ConvertGeometry {
      uint32_t audioDataStartPosBytes;
      uint64_t audioDataLengthBytes;
      int32_t  firstClusterIndexWithNoAudioData; // = sample->getFirstClusterIndexWithNoAudioData()
  };
  void convertClusterData(std::span<std::byte> data, int32_t clusterIndex, RawDataFormat format,
                          ConvertGeometry geometry, size_t clusterSize, size_t clusterSizeMagnitude,
                          std::span<std::byte, 3> firstThreePreConversionOut, YieldFn yield, void* yieldCtx);
  ```
  Semantics MUST match `Cluster::convertDataIfNecessary` (cluster.cpp:60-153) exactly: in-place conversion of `data[0..clusterSize)`; on `format != NATIVE` first back up `data[0..3)` into `firstThreePreConversionOut`; the two early-outs (`audioDataStartPosBytes == 0` → return; `clusterIndex < startCluster` → return); the ENDIANNESS_WRONG_24 3-byte-swap loop; the "all other formats" `int32_t`-word loop calling `convertWord`; and the two `yield(yieldCtx)` sites at the original cadences (~every 1024 bytes).

- [ ] **Step 1: Write failing specs for `convertClusterData`** in `convert_spec.cpp` — add a second `describe`. Cover: (a) NATIVE format leaves `data` untouched and does not write the backup; (b) ENDIANNESS_WRONG_24 over a small buffer swaps byte 0/2 of each 3-byte group within the audio region and writes `firstThreePreConversionOut` = the original first 3 bytes; (c) UNSIGNED_8 XORs `0x80` per byte across the word region; (d) the `audioDataStartPosBytes == 0` early-out is a no-op; (e) a `yield`-counting callback is invoked ≥1 time for a buffer large enough to cross the ~1024-byte cadence. Use a small synthetic `clusterSize` (e.g. 4096) and a captured counter via `void* ctx`. Assert exact bytes for the small deterministic cases. (Register with `CPPSPEC_SPEC` per the existing file's convention — one macro per describe-variable as the harness requires.)

- [ ] **Step 2: Run, expect FAIL** (`convertClusterData` missing). Run: `./dbt test`.

- [ ] **Step 3: Implement `convertClusterData`** in `convert.{h,cpp}`. Port the body of `Cluster::convertDataIfNecessary` (cluster.cpp:60-153) VERBATIM into the pure function, with exactly these substitutions:
  - `sample->rawDataFormat` → `format`; `sample->audioDataStartPosBytes` → `geometry.audioDataStartPosBytes`; `sample->audioDataLengthBytes` → `geometry.audioDataLengthBytes`; `sample->getFirstClusterIndexWithNoAudioData()` → `geometry.firstClusterIndexWithNoAudioData`.
  - `clusterIndex` → the `clusterIndex` param; `Cluster::size` → `clusterSize`; `Cluster::size_magnitude` → `clusterSizeMagnitude`.
  - `data` (the cluster field) → `reinterpret_cast<char*>(data.data())` (or operate on `data` as bytes) — same pointer arithmetic as the original.
  - `firstThreeBytesPreDataConversion` (the backup write at cluster.cpp:67) → write into `firstThreePreConversionOut`.
  - `sample->convertToNative(*pos)` → `convertWord(*pos, format)`.
  - `AudioEngine::logAction("from convert-data"); AudioEngine::runRoutine();` (24-bit site, cluster.cpp:117-118) AND `AudioEngine::runRoutine();` (other-formats site, cluster.cpp:146) → `if (yield) yield(yieldCtx);` at BOTH sites, preserving the original cadence and the original "skip the final-chunk yield" guard on the 24-bit path (cluster.cpp:113).
  Keep the early-outs and all loop bounds identical. Do NOT include `AudioEngine`/`Sample`/`Cluster` headers in `convert.cpp` — the function must stay dependency-light.

  **Yield-parity note (flag for the golden gate):** the original logs `logAction("from convert-data")` only at the 24-bit site, not the other-formats site. Moving both to a single `yield` means the wrapper's yield lambda decides whether to log. Have the wrapper's yield do `AudioEngine::logAction("from convert-data"); AudioEngine::runRoutine();` for both sites. `logAction` is a profiler action-marker, expected golden-neutral — **verify** cordae/icoustic stay bit-exact in Step 5; if they diverge, that marker mattered — split into two yield callbacks (one logging, one not) to restore exact parity.

- [ ] **Step 4: Make `Cluster::convertDataIfNecessary` a thin wrapper.** Replace its body (cluster.cpp:60-153) with a gather-and-call:
```cpp
void Cluster::convertDataIfNecessary() {
	deluge::audio::stream::convertClusterData(
	    std::span<std::byte>(reinterpret_cast<std::byte*>(data), Cluster::size), clusterIndex,
	    sample->rawDataFormat,
	    {sample->audioDataStartPosBytes, sample->audioDataLengthBytes,
	     sample->getFirstClusterIndexWithNoAudioData()},
	    Cluster::size, Cluster::size_magnitude,
	    std::span<std::byte, 3>(reinterpret_cast<std::byte*>(firstThreeBytesPreDataConversion), 3),
	    [](void*) {
		    AudioEngine::logAction("from convert-data");
		    AudioEngine::runRoutine();
	    },
	    nullptr);
}
```
Add the `storage/audio/stream/convert.h` include to cluster.cpp. Keep `AudioEngine` included there (the wrapper still uses it).

- [ ] **Step 5: Run specs + build + golden gate.** `./dbt test` (convert cases + full suite green); `dbt build Debug` (clean); `scripts/golden_mixdown.sh check` (cordae bit-exact); `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact). If either diverges, apply the yield-parity fallback (Step 3 note) and/or check the port against cluster.cpp:60-153 line-by-line. `FIXTURE=highsiderr` is KNOWN-STALE — if it fails, git-stash A/B to confirm pre-existing; do not block.

- [ ] **Step 6: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp \
        src/deluge/storage/cluster/cluster.cpp tests/spec_audio_stream/convert_spec.cpp
git commit -m "refactor(audio-stream): extract convertClusterData pure core; Cluster::convertDataIfNecessary a thin wrapper"
```

---

## Roadmap — after 2a

- **Phase 2b — the boundary stitch.** Extract audio_file_manager.cpp:1044-1223 into a pure-ish core: this cluster's converted bytes + both neighbor edges (`head7`/`tail7`, `firstThreePreConversion`, `loaded`, `extraBytesAt*Converted`) in → overhang + neighbor-deltas out, with the caller applying deltas back to the neighbor cluster objects (serialized per Sample). Intricate (goto-modeled-as-shared-tail; the ordering subtlety at aud_file_manager.cpp:1176-1194 where the `int32_t&` read happens after the overhang memcpy; the overhang `data[size..size+7)` is a real playback-consumed output, not scratch). Its own plan, golden-gated.
- Then Phases 3-5 + follow-ons A/B per the module design spec.
