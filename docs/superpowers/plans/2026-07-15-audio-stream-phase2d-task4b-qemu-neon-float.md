# Audio Stream — Phase 2d Task 4b: qemu-arm SIMD parity + NEON-gated FLOAT

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Verify the SIMD conversion primitives against **real ARM NEON** (via qemu-arm), and — if FLOAT parity holds there — re-enable SIMD FLOAT gated on real-NEON (SIMD on ARM hardware / qemu / Apple Silicon; scalar only on x86-SIMDe hosts). Recovers the on-device FLOAT speedup that Task 4 dropped, without breaking the host-sim goldens, and adds a dual-arch net for the whole SIMD suite.

**Background:** Task 4 found that **SIMDe's software emulation** of `vcvtq_n_s32_f32` diverges from scalar `q31_from_float` (1.0f wraps instead of saturating; NaN→0 instead of 0x7FFFFFFF), so FLOAT was reverted to scalar everywhere. But that's a SIMDe bug, not real NEON — on hardware, `vcvtq_n_s32_f32` and the scalar VFP `vcvt.s32.f32` are the same instruction family. `tests/qemu/` already runs CppSpec specs under qemu-arm with real NEON (`sim/arm-linux-toolchain.cmake`, cortex-a9 `-mfpu=neon`), and `tests/qemu/spec/fixedpoint_vfp_spec.cpp` is the exact precedent (VFP vcvt parity check under qemu). We add a NEON parity check for the argon SIMD ops.

**Tech Stack:** qemu-arm CppSpec (`tests/qemu/`); argon on real `<arm_neon.h>` (no SIMDe on the ARM side); `q31_from_float` / `swapEndianness*` scalar references.

## Global Constraints

- **FLOAT must never be host-divergent.** On x86-SIMDe (where the goldens render), FLOAT stays SCALAR. SIMD FLOAT is enabled ONLY where argon compiles to real NEON/MVE (`__ARM_NEON`/`__ARM_FEATURE_MVE` defined — true for the arm-none firmware, qemu-arm, Apple Silicon; false for x86-SIMDe). Verify the gate macro is set on the firmware + qemu builds and NOT on the x86 sim.
- **The qemu-arm parity is the on-device FLOAT gate.** The x86 goldens can't test the firmware's SIMD FLOAT (host uses scalar) — the qemu-arm spec is what proves it. If the qemu-arm FLOAT parity FAILS (unlikely), FLOAT stays scalar everywhere (Task 2 is skipped) and we keep only the integer dual-arch net.
- snake_case; idiomatic C++23. Don't regress the x86 specs (20/20) or the goldens (cordae/icoustic bit-exact — they run scalar FLOAT).

---

## Task 1: qemu-arm SIMD parity spec (verify argon ops == scalar on real NEON)

**Files:**
- Modify: `tests/qemu/spec/CMakeLists.txt` (add argon so the spec can call `Argon<...>`).
- Create: `tests/qemu/spec/convert_simd_neon_parity_spec.cpp`.

- [ ] **Step 1: Wire argon into the qemu-arm spec build.** Add the argon FetchContent (pin per `lib/CMakeLists.txt`) + include dir to `tests/qemu/spec/CMakeLists.txt`. On ARM the toolchain provides a real `<arm_neon.h>` — NO SIMDe, no compat shim needed (argon's `arm_simd.hpp` dispatch picks the native NEON header when `__ARM_NEON` is set). Prove a bare `#include "argon.hpp"` compiles + a trivial `Argon<uint8_t>` use runs under qemu-arm FIRST. **If argon won't cleanly build under the arm-none/arm-linux qemu toolchain, STOP and report NEEDS_CONTEXT** (what blocked) — don't hack it.

- [ ] **Step 2: Write the parity spec** (`convert_simd_neon_parity_spec.cpp`, mirroring `fixedpoint_vfp_spec.cpp`'s style — edge cases + a deterministic sweep, assert zero mismatches). Under qemu-arm (real NEON), for the SAME inputs, assert the argon SIMD op == the scalar reference, lane-by-lane:
  - **FLOAT (the crux):** `Argon<float>::ConvertTo<int32_t,31>()` lanes == `q31_from_float()` for each float — cover `0.5f`, `1.0f` (saturate → `0x7FFFFFFF`), `-1.0f`, `-0.5f`, `NaN`, `+inf`, `-inf`, denormals, values just under/over ±1.0, and a pseudo-random float sweep. **This is the decision gate for Task 2.**
  - **Integer ops (the dual-arch net):** `Argon<uint8_t>::Load(p).Reverse32bit()` == per-word `swapEndianness32`; `.Reverse16bit()` == `swapEndianness2x16`; `^ Argon<uint8_t>{0x80}` == `^0x80` per byte; `LoadInterleaved<3>`+`store_interleaved` channel-swap == the 3-byte byte0↔byte2 swap. Over a byte buffer + a sweep.

- [ ] **Step 3: Build + run under qemu-arm.** Build the `tests/qemu` suite (arm toolchain) and run the specs under qemu (the same way `fixedpoint_vfp_spec` is run — see how the qemu test target/CTest is invoked; it may be slow). Capture the result.
  - **DECISION GATE (record explicitly):** does the FLOAT parity pass on real NEON?
    - PASS → SIMD FLOAT is correct on-device. Task 2 proceeds.
    - FAIL → real NEON `vcvtq_n_s32_f32` also diverges from scalar `q31_from_float` (surprising — would mean the scalar model is wrong, or an argon fracbits mismatch). Report the divergence; FLOAT stays scalar everywhere; SKIP Task 2. (We still keep the integer dual-arch net.)

- [ ] **Step 4: Commit.**
```bash
git add tests/qemu/spec/CMakeLists.txt tests/qemu/spec/convert_simd_neon_parity_spec.cpp
git commit -m "test(audio-stream): qemu-arm parity spec — argon SIMD conversion ops vs scalar on real NEON"
```

---

## Task 2: re-enable SIMD FLOAT gated on real-NEON (ONLY if Task 1's FLOAT parity passed)

**Files:** Modify `src/deluge/storage/audio/stream/convert.h`.

- [ ] **Step 1: Gate the FLOAT dispatch.** In `convert_range_simd` (and its dispatch), enable the SIMD FLOAT path (`Argon<float>::Load(...).ConvertTo<int32_t,31>()`, established+tested in Task 4's investigation) **only when compiling for real NEON** — `#if defined(__ARM_NEON) || defined(__ARM_FEATURE_MVE)` → SIMD FLOAT; `#else` (x86-SIMDe) → FLOAT falls through to the scalar `convert_word` path (as it does now). Keep the doc comment explaining the split + the TODO.md pointer (update it to note the gate + the qemu verification).
- [ ] **Step 2: Verify BOTH arches.**
  - x86 (`./dbt test`): the FLOAT spec cases pass via SCALAR (unchanged); 20/20. Goldens `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact (host runs scalar FLOAT — no change).
  - qemu-arm: the parity spec's FLOAT case passes via the SIMD path (real NEON). (Re-run the qemu suite.)
  - `dbt build Debug` (firmware, arm — now compiles the SIMD FLOAT path) clean.
- [ ] **Step 3: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h TODO.md
git commit -m "feat(audio-stream): SIMD FLOAT->Q31 on real NEON (qemu-verified), scalar on SIMDe hosts"
```

---

## Roadmap

After this, the convert SIMD is complete + dual-arch-verified, FLOAT accelerated on-device. Remaining: Task 5 (idiomatic stitch rewrite) from the main Phase 2d plan, then the Phase 2d final review. Consider making the qemu-arm convert parity a standing dual-arch gate for future SIMD work in this module.
