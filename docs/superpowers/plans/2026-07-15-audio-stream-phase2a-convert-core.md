# Audio Stream — Phase 2a: pure format-conversion core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the intra-cluster format conversion (`Cluster::convertDataIfNecessary`, cluster.cpp:60-153) into a pure, unit-tested `deluge::audio::stream` core — a `convert_word(word, format)` functor and a `convert_cluster_data(...)` function with a fn-pointer yield callback — leaving `Cluster::convertDataIfNecessary` a thin wrapper. Also retrofit the existing Phase 0-1 module code to the house snake_case convention.

**Architecture:** Phase 2 of the audio-stream migration splits into **2a (this plan) — the pure intra-cluster convert** and **2b (later) — the inter-cluster boundary stitch**. The split exists because the two differ fundamentally: the *convert* reads only immutable-per-sample PODs and its sole impurity is a mid-loop `AudioEngine::runRoutine()` yield (cleanly pure-able), whereas the *stitch* reaches into and **mutates neighbor clusters**, so it stays orchestration-heavy and is deferred to 2b. `convertToNative(int32_t)` already switches only on `rawDataFormat` — already a pure functor, with exact card-free per-format test vectors.

**Tech Stack:** C++23; CppSpec unit specs (mirroring `tests/spec_audio_stream/`); golden-master render gate (`scripts/golden_mixdown.sh`, fixtures cordae/icoustic/highsiderr). `RawDataFormat` at `src/deluge/storage/audio/audio_file_format.h:30-37`. Conversion helpers: `util/audio_format_helpers.h` (`swapEndianness32`, `swapEndianness2x16`), `util/fixedpoint.h` (`q31_from_float`).

## Global Constraints

From the design spec (`docs/superpowers/specs/2026-07-15-audio-stream-module-design.md`) and the repo `.clang-tidy`; every task inherits these:

- **Naming (house convention, per `.clang-tidy`):** namespaces + **functions/methods + variables/params** = `lower_case` (snake_case); **class/struct/enum types** = `CamelCase`; **enum constants** = `UPPER_CASE`; **class/struct members** = `lower_case`, private members suffixed `_`. (Functions/methods were just switched from `camelBack` to `lower_case` to match `deluge::io`.) Write ALL new identifiers this way; do not copy the legacy camelCase style of surrounding old code.
- **Idiomatic modern C++23**, in-tree. The pure core is dependency-light (POD in/out) — no reach into `Sample`/`Cluster`/`AudioEngine` object graphs; those are gathered by the thin wrapper and passed in.
- **The yield callback is a plain `void(*)(void*)` fn-pointer + ctx** — NOT `std::function` (per the Phase-1 lesson that per-call heap allocation perturbs allocation timing / goldens). Zero-alloc, and the portable/ABI shape the future Rust core will use.
- **Behavior-preserving.** These tasks are pure code-moves + a DRY delegation + a rename; gate is golden **bit-exact** (cordae + icoustic) plus unit specs. `highsiderr` is KNOWN-STALE (pre-existing-divergent — a fail there is likely not this work's regression; verify via git-stash A/B, do not block). Keep structs byte-stable.
- No new C ABI; the residency/read seams from Phase 0-1 are untouched.

---

## File Structure

- `src/deluge/storage/audio/stream/read_source.{h,cpp}` — Task 0 renames identifiers to snake_case.
- `src/deluge/storage/audio/stream/convert.{h,cpp}` — `convert_word`, `YieldFn`, `ConvertGeometry`, `convert_cluster_data`.
- `src/deluge/model/sample/sample.h` — `Sample::convertToNative(int32_t)` delegates to `convert_word`.
- `src/deluge/storage/cluster/cluster.cpp` — `Cluster::convertDataIfNecessary` becomes a thin wrapper.
- `src/deluge/storage/audio/audio_file_manager.cpp` — Task 0 updates the `make_read_source` call site.
- `tests/spec_audio_stream/convert_spec.cpp` — CppSpec specs for `convert_word` + `convert_cluster_data`.

---

## Task 0: snake_case retrofit of the Phase 0-1 module code

The Phase 0-1 code (`read_source.{h,cpp}`) was written before the `.clang-tidy` `FunctionCase`/`ClassMethodCase` flip to `lower_case`; it uses camelCase identifiers that now violate the convention. Bring it into line so the whole `deluge::audio::stream` module reads consistently. Pure rename — zero behavior change.

**Files:**
- Modify: `src/deluge/storage/audio/stream/read_source.h`, `src/deluge/storage/audio/stream/read_source.cpp`
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (the one call site)

**Interfaces:**
- Produces: `deluge::audio::stream::make_read_source(Sample&)` (was `makeReadSource`). Types (`ReadSource`, `StreamReadSource`, `BlockReadSource`) and the `read` method keep their names (already convention-correct — CamelCase types, lower_case method).

- [ ] **Step 1: Rename in `read_source.h`/`read_source.cpp`.** Apply exactly:
  - free function `makeReadSource` → `make_read_source` (declaration in `.h`, definition in `.cpp`).
  - private member `clusterSizeMagnitude_` → `cluster_size_magnitude_` (in `StreamReadSource`).
  - constructor params / locals: `clusterSizeMagnitude` → `cluster_size_magnitude`, `clusterIndex` → `cluster_index` (in the `read` overrides and ctors). `dst`, `stream_`, `sample_`, `handle`, `word`, `format` are already fine.
  Leave all logic identical. `ReadSource`/`StreamReadSource`/`BlockReadSource`/`MockReadSource` and `read` are unchanged.

- [ ] **Step 2: Update the call site** in `audio_file_manager.cpp`'s `readClusterData`: `deluge::audio::stream::makeReadSource(*sample)` → `deluge::audio::stream::make_read_source(*sample)`. (Grep the tree for any other `makeReadSource` reference and update — there should be exactly this one.)

- [ ] **Step 3: Build + test.** Run: `dbt build Debug` (clean link — proves the rename is consistent across the call site). Then `./dbt test` (17/17 — the spec suite still builds; `read_source_spec`/`mock_read_source.h` reference `MockReadSource`/`read`, unaffected by the rename).

- [ ] **Step 4: Commit.**
```bash
git add src/deluge/storage/audio/stream/read_source.h src/deluge/storage/audio/stream/read_source.cpp \
        src/deluge/storage/audio/audio_file_manager.cpp
git commit -m "style(audio-stream): rename Phase 0-1 identifiers to snake_case (make_read_source, members, params)"
```

---

## Task 1: `convert_word` pure functor + `Sample::convertToNative` delegation

**Files:**
- Create: `src/deluge/storage/audio/stream/convert.h`, `src/deluge/storage/audio/stream/convert.cpp`
- Modify: `src/deluge/model/sample/sample.h` (`convertToNative(int32_t)`)
- Create: `tests/spec_audio_stream/convert_spec.cpp`

**Interfaces:**
- Produces: `int32_t deluge::audio::stream::convert_word(int32_t word, RawDataFormat format)` — the exact switch body of `Sample::convertToNative(int32_t)` (sample.h:100-123). `ENDIANNESS_WRONG_24` and `NATIVE` return `word` unchanged; `FLOAT` → `q31_from_float(std::bit_cast<float>(word))`; `ENDIANNESS_WRONG_32` → `swapEndianness32`; `ENDIANNESS_WRONG_16` → `swapEndianness2x16`; `UNSIGNED_8` → `word ^ 0x80808080`.

- [ ] **Step 1: Write the failing spec** (`tests/spec_audio_stream/convert_spec.cpp`, mirroring `read_source_spec.cpp`'s DSL — quoted `"cppspec.hpp"`, `describe`/`it`/`expect`, trailing `CPPSPEC_SPEC(convert)`):

```cpp
// tests/spec_audio_stream/convert_spec.cpp
#include "storage/audio/stream/convert.h"

#include "cppspec.hpp"

#include <bit>
#include <cstdint>

using namespace deluge::audio::stream;

// clang-format off
describe convert("convert_word", $ {
	it("NATIVE is identity", _ { expect(convert_word(0x11223344, RawDataFormat::NATIVE)).to_equal(0x11223344); });
	it("ENDIANNESS_WRONG_24 is a no-op (3-byte swap is done cluster-wide)", _ {
		expect(convert_word(0x11223344, RawDataFormat::ENDIANNESS_WRONG_24)).to_equal(0x11223344); });
	it("ENDIANNESS_WRONG_32 reverses all 4 bytes", _ {
		expect(convert_word(0x01020304, RawDataFormat::ENDIANNESS_WRONG_32)).to_equal(0x04030201); });
	it("ENDIANNESS_WRONG_16 swaps within each half-word", _ {
		expect(convert_word(0x01020304, RawDataFormat::ENDIANNESS_WRONG_16)).to_equal(0x02010403); });
	it("UNSIGNED_8 flips the MSB of every byte", _ {
		expect(convert_word(0x00112233, RawDataFormat::UNSIGNED_8)).to_equal(0x8091A2B3); });
	it("FLOAT 0.5f maps to Q31 0x40000000", _ {
		expect(convert_word(std::bit_cast<int32_t>(0.5f), RawDataFormat::FLOAT)).to_equal(0x40000000); });
});

CPPSPEC_SPEC(convert)
```

Note: confirm the exact `swapEndianness2x16` / `q31_from_float` results against the real helpers while implementing; if the FLOAT-saturation or 16-swap value differs, correct the expected value to match the actual (verbatim-faithful) helper output — the vector must assert the real function's behavior.

- [ ] **Step 2: Run the spec, expect FAIL** (symbol missing). Run: `./dbt test`. Expected: FAIL — `convert.h` / `convert_word` not found.

- [ ] **Step 3: Implement `convert.h` + `convert.cpp`**

`convert.h`:
```cpp
#pragma once
#include "storage/audio/audio_file_format.h" // RawDataFormat
#include <cstdint>

namespace deluge::audio::stream {
// Pure per-word format conversion — the body of Sample::convertToNative(int32_t), depending only on the
// format. ENDIANNESS_WRONG_24 and NATIVE return the word unchanged (the 24-bit 3-byte swap is done
// group-wise in convert_cluster_data, added in Task 2).
int32_t convert_word(int32_t word, RawDataFormat format);
} // namespace deluge::audio::stream
```

`convert.cpp` — port the `convertToNative(int32_t)` switch verbatim (include `util/audio_format_helpers.h`, `util/fixedpoint.h`, `<bit>`):
```cpp
#include "storage/audio/stream/convert.h"
#include "util/audio_format_helpers.h"
#include "util/fixedpoint.h"
#include <bit>

namespace deluge::audio::stream {
int32_t convert_word(int32_t word, RawDataFormat format) {
	switch (format) {
	case RawDataFormat::FLOAT:               return q31_from_float(std::bit_cast<float>(word));
	case RawDataFormat::ENDIANNESS_WRONG_32: return swapEndianness32(word);
	case RawDataFormat::ENDIANNESS_WRONG_16: return swapEndianness2x16(word);
	case RawDataFormat::UNSIGNED_8:          return word ^ 0x80808080;
	case RawDataFormat::ENDIANNESS_WRONG_24: [[fallthrough]];
	case RawDataFormat::NATIVE:              break;
	}
	return word;
}
} // namespace deluge::audio::stream
```

- [ ] **Step 4: Delegate `Sample::convertToNative(int32_t)` to `convert_word`** (DRY — single source of truth). In `sample.h`, replace the switch body of `convertToNative(int32_t value) const` with `return deluge::audio::stream::convert_word(value, rawDataFormat);` (add the `storage/audio/stream/convert.h` include). Leave the `float` overload untouched. (Note: `convertToNative` keeps its camelCase name — it is a pre-existing method not being renamed here; only NEW identifiers must be snake_case.)

- [ ] **Step 5: Run the spec, expect PASS.** Run: `./dbt test`. Expected: `convert` cases PASS; full suite green.

- [ ] **Step 6: Build firmware + golden gate.** `dbt build Debug` (clean); `scripts/golden_mixdown.sh check` (cordae bit-exact); `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact). Delegation is behavior-identical, so both stay bit-exact.

- [ ] **Step 7: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp \
        src/deluge/model/sample/sample.h tests/spec_audio_stream/convert_spec.cpp
git commit -m "feat(audio-stream): pure convert_word functor; Sample::convertToNative delegates to it"
```

---

## Task 2: `convert_cluster_data` pure function + `Cluster::convertDataIfNecessary` wrapper

**Files:**
- Modify: `src/deluge/storage/audio/stream/convert.h`, `src/deluge/storage/audio/stream/convert.cpp`
- Modify: `src/deluge/storage/cluster/cluster.cpp` (`convertDataIfNecessary`)
- Modify: `tests/spec_audio_stream/convert_spec.cpp`

**Interfaces:**
- Consumes: `convert_word` (Task 1).
- Produces:
  ```cpp
  using YieldFn = void (*)(void* ctx);
  struct ConvertGeometry {
      uint32_t audio_data_start_pos_bytes;
      uint64_t audio_data_length_bytes;
      int32_t  first_cluster_index_with_no_audio_data; // = sample->getFirstClusterIndexWithNoAudioData()
  };
  void convert_cluster_data(std::span<std::byte> data, int32_t cluster_index, RawDataFormat format,
                            ConvertGeometry geometry, size_t cluster_size, size_t cluster_size_magnitude,
                            std::span<std::byte, 3> first_three_pre_conversion_out,
                            YieldFn yield, void* yield_ctx);
  ```
  Semantics MUST match `Cluster::convertDataIfNecessary` (cluster.cpp:60-153) exactly: in-place conversion of `data[0..cluster_size)`; on `format != NATIVE` first back up `data[0..3)` into `first_three_pre_conversion_out`; the two early-outs (`audio_data_start_pos_bytes == 0` → return; `cluster_index < start_cluster` → return); the ENDIANNESS_WRONG_24 3-byte-swap loop; the "all other formats" `int32_t`-word loop calling `convert_word`; and the two `yield(yield_ctx)` sites at the original cadences (~every 1024 bytes).

- [ ] **Step 1: Write failing specs for `convert_cluster_data`** in `convert_spec.cpp` — add a second `describe` (own `CPPSPEC_SPEC` per the harness's one-macro-per-describe convention). Cover, with a small synthetic `cluster_size` (e.g. 4096) and exact-byte assertions where deterministic:
  - (a) NATIVE leaves `data` untouched and does NOT write the backup out-param.
  - (b) ENDIANNESS_WRONG_24 over a small buffer swaps byte 0/2 of each 3-byte group within the audio region, and writes `first_three_pre_conversion_out` = the original first 3 bytes.
  - (c) UNSIGNED_8 XORs `0x80` per byte across the word region.
  - (d) `audio_data_start_pos_bytes == 0` early-out is a no-op.
  - (e) a `yield`-counting callback (`void(*)(void*)` incrementing a counter via `ctx`) is invoked ≥1 time for a buffer large enough to cross the ~1024-byte cadence.

- [ ] **Step 2: Run, expect FAIL** (`convert_cluster_data` missing). Run: `./dbt test`.

- [ ] **Step 3: Implement `convert_cluster_data`** in `convert.{h,cpp}`. Port the body of `Cluster::convertDataIfNecessary` (cluster.cpp:60-153) VERBATIM into the pure function, with exactly these substitutions:
  - `sample->rawDataFormat` → `format`; `sample->audioDataStartPosBytes` → `geometry.audio_data_start_pos_bytes`; `sample->audioDataLengthBytes` → `geometry.audio_data_length_bytes`; `sample->getFirstClusterIndexWithNoAudioData()` → `geometry.first_cluster_index_with_no_audio_data`.
  - the cluster's `clusterIndex` → the `cluster_index` param; `Cluster::size` → `cluster_size`; `Cluster::size_magnitude` → `cluster_size_magnitude`.
  - `data` (the cluster field) → `reinterpret_cast<char*>(data.data())` (same pointer arithmetic as the original).
  - `firstThreeBytesPreDataConversion` backup write (cluster.cpp:67) → write into `first_three_pre_conversion_out`.
  - `sample->convertToNative(*pos)` → `convert_word(*pos, format)`.
  - both yield sites (`AudioEngine::logAction("from convert-data"); AudioEngine::runRoutine();` at cluster.cpp:117-118, and `AudioEngine::runRoutine();` at cluster.cpp:146) → `if (yield) yield(yield_ctx);`, preserving the original cadence and the 24-bit path's "skip the final-chunk yield" guard (cluster.cpp:113).
  Keep the early-outs and all loop bounds identical. Do NOT include `AudioEngine`/`Sample`/`Cluster` headers in `convert.cpp` — the function stays dependency-light. Use snake_case for every new local variable you introduce.

  **Yield-parity note (verify at the golden gate):** the original logs `logAction("from convert-data")` only at the 24-bit site. Routing both through one `yield` means the wrapper's yield lambda logs for both. `logAction` is a profiler action-marker, expected golden-neutral — **verify** cordae/icoustic stay bit-exact in Step 5; if they diverge, that marker mattered — split into two yield callbacks (one logging, one not) to restore exact parity.

- [ ] **Step 4: Make `Cluster::convertDataIfNecessary` a thin wrapper.** Replace its body (cluster.cpp:60-153) with a gather-and-call:
```cpp
void Cluster::convertDataIfNecessary() {
	deluge::audio::stream::convert_cluster_data(
	    std::span<std::byte>(reinterpret_cast<std::byte*>(data), Cluster::size), clusterIndex,
	    sample->rawDataFormat,
	    {.audio_data_start_pos_bytes = sample->audioDataStartPosBytes,
	     .audio_data_length_bytes = sample->audioDataLengthBytes,
	     .first_cluster_index_with_no_audio_data = sample->getFirstClusterIndexWithNoAudioData()},
	    Cluster::size, Cluster::size_magnitude,
	    std::span<std::byte, 3>(reinterpret_cast<std::byte*>(firstThreeBytesPreDataConversion), 3),
	    [](void*) {
		    AudioEngine::logAction("from convert-data");
		    AudioEngine::runRoutine();
	    },
	    nullptr);
}
```
Add the `storage/audio/stream/convert.h` include to cluster.cpp. Keep `AudioEngine` included there.

- [ ] **Step 5: Run specs + build + golden gate.** `./dbt test` (convert cases + full suite green); `dbt build Debug` (clean); `scripts/golden_mixdown.sh check` (cordae bit-exact); `FIXTURE=icoustic scripts/golden_mixdown.sh check` (bit-exact). If either diverges, apply the yield-parity fallback and/or check the port against cluster.cpp:60-153 line-by-line. `FIXTURE=highsiderr` KNOWN-STALE — git-stash A/B if it fails; do not block.

- [ ] **Step 6: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp \
        src/deluge/storage/cluster/cluster.cpp tests/spec_audio_stream/convert_spec.cpp
git commit -m "refactor(audio-stream): extract convert_cluster_data pure core; Cluster::convertDataIfNecessary a thin wrapper"
```

---

## Roadmap — after 2a

- **Phase 2b — the boundary stitch.** Extract audio_file_manager.cpp:1044-1223 into a pure-ish core: this cluster's converted bytes + both neighbor edges in → overhang + neighbor-deltas out, caller applies deltas back to neighbor cluster objects (serialized per Sample). Intricate (goto-as-shared-tail; the ordering subtlety at 1176-1194 where the `int32_t&` read happens after the overhang memcpy; the overhang `data[size..size+7)` is a real playback-consumed output). Its own plan, golden-gated.
- Then Phases 3-5 + follow-ons A/B per the module design spec.
