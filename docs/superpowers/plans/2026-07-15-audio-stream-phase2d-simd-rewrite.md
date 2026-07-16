# Audio Stream — Phase 2d: idiomatic C++23 rewrite + argon/NEON SIMD

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rewrite the reconstruction core's two functions as idiomatic, modern C++23 (not verbatim-preserved C-style), and SIMD-accelerate the bulk format conversion via the house **argon** abstraction (NEON on device, SIMDe on host-sim). `convert_cluster_data` becomes a dispatch-once + vectorized-prefix/scalar-tail loop covering all non-native formats; `stitch_boundaries` gets an idiomatic rewrite (no SIMD — it's edge fixup).

**Architecture:** The scalar `convert_word` stays as the per-word reference + scalar-tail worker. A new vectorized path dispatches on `RawDataFormat` **once**, then runs a tight argon loop over 16-byte (or, for 24-bit, 48-byte) chunks + a scalar tail, yielding every ~1024 bytes as today. Format→argon-op mapping (all bit-exact except FLOAT): `ENDIANNESS_WRONG_32` → `Argon<uint8_t>::Load(p).Reverse32bit()`; `ENDIANNESS_WRONG_16` → `.Reverse16bit()`; `UNSIGNED_8` → `^ Argon<uint8_t>{0x80}`; `ENDIANNESS_WRONG_24` → `LoadInterleaved<3>` (vld3) → swap channels 0↔2 → `store_interleaved<3>` (vst3); `FLOAT` → `Argon<float>::ConvertTo<int32_t,31>()` (the wide sibling of the scalar VFP `q31_from_float`), **with a verified fallback to scalar if the SIMDe host path diverges by even 1 LSB**.

**Tech Stack:** C++23 (`std::span`, `std::byteswap`); argon (`argon.hpp`, headers under `build*/_deps/argon-src/include/`, pinned `lib/CMakeLists.txt`); SIMDe on host-sim (`sim/CMakeLists.txt:186-214`); CppSpec unit specs (the primary correctness gate here); golden-master (`scripts/golden_mixdown.sh`, no-regression backstop).

## Global Constraints

- **Behavior-preserving OUTPUT.** The converted bytes must be identical to the current scalar path for every format. Gate: the strengthened unit specs (byte-exact per format, at sizes straddling the vectorized-prefix ↔ scalar-tail boundary) + golden bit-exact (cordae/icoustic). `highsiderr` KNOWN-STALE.
- **FLOAT is the one bit-exactness risk.** `ConvertTo<int32_t,31>()` is bit-exact to `q31_from_float` on ARM (same instruction family). On host-sim it's SIMDe's independent emulation — the FLOAT spec MUST assert exact bytes incl. NaN + saturation edges; if the host path diverges, FLOAT stays scalar (Task 4). Never ship a host-divergent FLOAT (the goldens run on that host path).
- **Use argon, not raw intrinsics** — so it compiles+runs on host-sim via SIMDe unchanged. Avoid `ArgonHalf<float>` scalar broadcast (documented SIMDe gotcha); this work uses `Argon<uint8_t>` + full `Argon<float>`.
- **Naming (house convention):** snake_case functions/variables; CamelCase types; `_`-suffixed private members. **Idiomatic C++23** — `std::span`, structured bindings, `std::byteswap` for scalar endianness; no raw pointer casts (the Phase-2c memcpy helpers stay for the scalar tail).
- **Preserve the yield** (~every 1024 bytes) — cooperative-scheduling shim (removable under preemption, per TODO.md). And the 24-bit `begin` group-alignment the caller already computes.
- Dependency-light stays *mostly*: the module now depends on argon (a SIMD lib), which is fine — but NOT on AudioEngine/Sample/Cluster.

---

## File Structure

- `src/deluge/storage/audio/stream/convert.h` / `convert.cpp` — the SIMD rewrite (the vectorized loop likely moves to `convert.cpp` as non-template free functions taking the yield as a `template`, OR stays header-template; implementer's call for cleanest structure).
- `src/deluge/storage/audio/stream/stitch.cpp` — idiomatic rewrite (Task 5).
- `src/deluge/storage/audio/stream/CMakeLists.txt` (or the module's build wiring) + `tests/spec_audio_stream/CMakeLists.txt` — argon + SIMDe include/link (Task 2).
- `tests/spec_audio_stream/convert_cluster_spec.cpp` (+ maybe `convert_spec.cpp`) — strengthened specs.

---

## Task 1: strengthen the convert specs (the regression net, against the current scalar path)

Establish byte-exact expected values per format BEFORE the rewrite, so the SIMD path is validated against a strong net. These run against the CURRENT scalar `convert_cluster_data` and must pass as-is.

**Files:** Modify `tests/spec_audio_stream/convert_cluster_spec.cpp` (and/or `convert_spec.cpp`).

- [ ] **Step 1: Add per-format `convert_cluster_data` cases**, each at a buffer size that straddles a vectorized boundary (e.g. `cluster_size` such that the audio region is 16-byte-vector-count + a non-zero remainder — e.g. 50 or 70 bytes of audio, forcing a scalar tail once the SIMD lands): `ENDIANNESS_WRONG_32` (byte-reverse per word), `ENDIANNESS_WRONG_16` (per-halfword), `UNSIGNED_8` (already have one — add a size-straddling variant), `ENDIANNESS_WRONG_24` (add a size that crosses the future 48-byte chunk boundary, i.e. > 48 bytes of 3-byte groups + remainder), and `FLOAT` (assert exact Q31 bytes for a few float inputs incl. `0.5f`→`0x40000000`, `1.0f`→saturate `0x7FFFFFFF`, a negative, and a NaN/`+inf` input). Hand-derive expected bytes from the scalar semantics (`convert_word` + the 24-bit swap).
- [ ] **Step 2: Run** `./dbt test` — all new cases GREEN against the current scalar implementation. (If a hand-derived value is wrong, fix the EXPECTED to match the real scalar output — the scalar path is the reference.)
- [ ] **Step 3: Commit.**
```bash
git add tests/spec_audio_stream/convert_cluster_spec.cpp
git commit -m "test(audio-stream): strengthen convert specs (per-format, prefix/tail-straddling sizes) before SIMD rewrite"
```

---

## Task 2: wire argon + SIMDe into the spec build; first SIMD format (UNSIGNED_8)

Prove the whole pipeline end-to-end on the simplest format (XOR) — build integration + one argon loop + spec + golden — before the rest.

**Files:** Modify `tests/spec_audio_stream/CMakeLists.txt` (argon + SIMDe, mirroring `tests/dsp/CMakeLists.txt`); `convert.{h,cpp}`.

- [ ] **Step 1: Wire argon+SIMDe into the convert spec driver.** The reference is **`sim/CMakeLists.txt` lines ~186-214** (SIMDe FetchContent pinned v0.8.2 + the `sim/compat` `<arm_neon.h>` intercept + `SIMDE_NO_NATIVE`/`SIMDE_ENABLE_NATIVE_ALIASES`, and the two include dirs — root for `<simde/arm/neon.h>`, `simde/` subdir for argon's bare `<arm/neon.h>`), plus the **argon FetchContent** (see `lib/CMakeLists.txt` for the pin). Replicate that in `tests/spec_audio_stream/CMakeLists.txt` so the CppSpec driver compiles `convert.cpp`/`convert.h` (which will `#include "argon.hpp"`). **Match the sim's SIMD config** — critically the same bitness + `SIMDE_NO_NATIVE` state the golden sim uses (default -m32 portable-C SIMDe), so the FLOAT `vcvtq_n_s32_f32` path the spec exercises is the SAME one the golden renders through (otherwise Task 4's FLOAT bit-exactness check tests a different backend than ships). Confirm a trivial `#include "argon.hpp"` compiles in the spec build FIRST. **If this integration proves a genuine yak-shave (the CppSpec driver can't cleanly take the sim's SIMD toolchain config, or the bitness can't be matched), STOP and report NEEDS_CONTEXT with exactly what blocked** — do not hack a partial integration that unit-tests a different SIMD backend than the golden (false confidence). The controller will re-plan (e.g. validate the SIMD path via a dedicated non-native golden fixture instead).
- [ ] **Step 2: Implement the vectorized loop skeleton + UNSIGNED_8.** Add a `convert_range_simd`-style path: for `UNSIGNED_8`, loop `Argon<uint8_t>` over the 16-byte-aligned prefix (`argon::vectorizeable_size<uint8_t>` for the count), `(Argon<uint8_t>::Load(p) ^ Argon<uint8_t>{std::byte{0x80}}).StoreTo(p)`, then a scalar tail via `convert_word_in_place`/`convert_word`, preserving the ~1024-byte yield cadence. Dispatch UNSIGNED_8 to this path; the other formats keep their current scalar loops for now.
- [ ] **Step 3: Verify.** `dbt build Debug` (firmware — argon already available in the app build) clean; `./dbt test` (the UNSIGNED_8 spec cases now exercise the SIMD path via SIMDe — MUST be byte-exact) 20+/N green; `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` bit-exact.
- [ ] **Step 4: Commit.**
```bash
git add tests/spec_audio_stream/CMakeLists.txt src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp
git commit -m "feat(audio-stream): argon/SIMDe in convert spec build + SIMD UNSIGNED_8 conversion"
```

---

## Task 3: SIMD the remaining integer formats (WRONG_32, WRONG_16, WRONG_24)

**Files:** Modify `convert.{h,cpp}` (+ specs if a case needs adjusting).

- [ ] **Step 1: `ENDIANNESS_WRONG_32` / `ENDIANNESS_WRONG_16`** — same 16-byte loop skeleton, transform = `Argon<uint8_t>::Load(p).Reverse32bit()` / `.Reverse16bit()`, scalar tail via `convert_word`. Dispatch both to it.
- [ ] **Step 2: `ENDIANNESS_WRONG_24`** — the de-interleave path over 48-byte chunks: `auto [c0,c1,c2] = Argon<uint8_t>::LoadInterleaved<3>(p); argon::store_interleaved(p, c2, c1, c0);` (confirm the exact argon call spelling — `LoadInterleaved<3>` at vector.hpp:968, `store_interleaved` at store.hpp:68). The caller's group-aligned `begin` means no scalar prologue; the scalar `byteswap3` handles the <48-byte tail. Keep the yield cadence.
- [ ] **Step 3: Verify.** `dbt build Debug`; `./dbt test` (the WRONG_32/16/24 spec cases now hit the SIMD path — byte-exact); `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact.
- [ ] **Step 4: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp
git commit -m "feat(audio-stream): SIMD WRONG_32/WRONG_16 (Reverse) + WRONG_24 (vld3/vst3 de-interleave) conversion"
```

---

## Task 4: SIMD FLOAT with verified host fallback

**Files:** Modify `convert.{h,cpp}`.

- [ ] **Step 1: Implement SIMD FLOAT** — the 16-byte loop as `Argon<float>::Load(reinterpret-safe p)` → `.ConvertTo<int32_t,31>()` → store, scalar tail via `convert_word` (which calls `q31_from_float`). Dispatch FLOAT to it.
- [ ] **Step 2: VERIFY host bit-exactness.** Run `./dbt test` — the FLOAT spec cases (Task 1: `0.5f`, `1.0f` saturate, negative, NaN/inf) MUST be byte-exact on the SIMDe host path. **Decision gate:**
  - If GREEN → FLOAT SIMD stays. Proceed.
  - If ANY FLOAT case diverges (even 1 LSB) on host → **revert FLOAT to the scalar path** (keep the SIMD dispatch for the integer formats only; FLOAT falls through to the scalar `convert_word` loop), and note the SIMDe `vcvtq_n_s32_f32` divergence in the report + a TODO.md entry. Do NOT ship a host-divergent FLOAT.
- [ ] **Step 3: Golden.** `dbt build Debug`; `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact (these are integer PCM — FLOAT likely not exercised, but confirm no regression).
- [ ] **Step 4: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/convert.cpp
git commit -m "feat(audio-stream): SIMD FLOAT->Q31 via Argon::ConvertTo (verified host-bit-exact; scalar fallback if SIMDe diverges)"
```

---

## Task 5: idiomatic C++23 rewrite of `stitch_boundaries`

No SIMD — the stitch is ~7-byte edge fixup. Rewrite `stitch_prev`/`stitch_next`/the dispatcher for clarity: `std::span` subviews for the edge windows, `std::byteswap`/`convert_word_in_place` instead of raw casts (already UB-free from 2c), named local steps for the overhang stage/convert/finalize, structured control flow replacing the residual C-isms. Behavior-identical.

**Files:** Modify `src/deluge/storage/audio/stream/stitch.cpp`.

- [ ] **Step 1: Rewrite** `stitch_prev` / `stitch_next` idiomatically (clearer than the verbatim-ported form), keeping the exact semantics: the overhang refresh, the per-format straddle conversion, the `unconverted_head`-vs-`head` staging, the flag writes, the `need_copy7` finalize. Use `std::span` windows + `convert_word_in_place` for the straddle word; keep `byteswap3` for the 24-bit.
- [ ] **Step 2: Verify.** `dbt build Debug`; `./dbt test` (all 4 stitch specs green); `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact. (Behavior-identical — the specs + goldens are the net for this rewrite.)
- [ ] **Step 3: Commit.**
```bash
git add src/deluge/storage/audio/stream/stitch.cpp
git commit -m "refactor(audio-stream): idiomatic C++23 rewrite of stitch_prev/stitch_next"
```

---

## Task 6: idiomatic C++23 rewrite of `convert_cluster_data` + its range helpers

Same idiomatic treatment as Task 5's stitch rewrite, applied to `convert_cluster_data`, `convert_24bit_range`, and `convert_word_range` in `convert.h`. Improve the variable names and modernize the C-ish style — while PRESERVING the SIMD integration (Tasks 2-4b) and exact conversion behavior. Behavior-identical; the net is the strengthened x86 specs + the qemu-arm parity spec + golden bit-exact.

**Files:** Modify `src/deluge/storage/audio/stream/convert.h`.

- [ ] **Step 1: Rewrite `convert_cluster_data`'s body + the scalar range loops idiomatically.**
  - Replace the `char* char_data = reinterpret_cast<char*>(data.data())` alias + raw pointer arithmetic with `std::span<std::byte>` subviews / offsets where it clarifies (the byte-offset geometry is easier to read as span indices than `char*` pointer math). Keep `data.data()` byte pointers only where the SIMD loop / `convert_word_in_place` genuinely needs them.
  - **Improve the names:** e.g. `char_data` (eliminate or rename), `bytes_eating_into_another_3byte` → something like `bytes_into_prev_group`, `end_at_byte_pos`/`end_at_pos_within_cluster` → clearer audio-region-end names, `start_cluster` → e.g. `first_audio_cluster` if clearer. Make the geometry computation (start_pos, the first-audio-cluster check, the per-format begin/end) read cleanly.
  - `convert_24bit_range` (scalar 3-byte swap loop) and `convert_word_range` (scalar word loop) — modernize their bodies (span/clearer locals; keep `byteswap3`/`convert_word_in_place`), same behavior.
  - Structured control flow + clear intermediate names for the early-outs, the 3-byte backup, the format dispatch.

- [ ] **Step 2: PRESERVE (hard constraints):**
  - The SIMD paths: `convert_range_simd` (the 16-byte prefix for UNSIGNED_8/WRONG_32/16 + NEON-gated FLOAT) and `convert_24bit_range_simd` (the vld3/vst3 prefix) — called from `convert_word_range`/`convert_24bit_range` exactly as now. Don't disturb the `#if defined(__ARM_NEON)...` FLOAT gate.
  - The scalar tails, the ~1024-byte yield cadence + the template `Yield` param, the `unconverted_head_out` backup, the two early-outs (`audio_data_start_pos_bytes == 0`; `cluster_index < start_cluster`), and every begin/end offset value (the arithmetic must stay identical — golden bit-exact).
  - Dependency-light (no AudioEngine/Sample/Cluster); snake_case; header templates stay non-anonymous.

- [ ] **Step 3: Verify BOTH arches.** `dbt build Debug` clean; `./dbt test` (all convert/convert_cluster specs green — the strengthened per-format straddling cases are the primary net); `scripts/golden_mixdown.sh check` (cordae) + `FIXTURE=icoustic` bit-exact. Rebuild + rerun the qemu-arm parity suite (per Task 4b) to confirm the SIMD paths still match real NEON.
- [ ] **Step 4: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h
git commit -m "refactor(audio-stream): idiomatic C++23 rewrite of convert_cluster_data + range helpers (names, span, structure)"
```

If any spec or golden diverges, an offset/order changed — do NOT commit; report BLOCKED with the divergence.

---

## Roadmap — after 2d

Fold the deferred 2c cosmetics opportunistically (byteswap3 dedup, the `detail` sub-namespace). Then Phase 3 (de-overload `Cluster` → `StreamedChunk`/`ComputedChunk`), and the batched test-hygiene ticket, per the module design spec.
