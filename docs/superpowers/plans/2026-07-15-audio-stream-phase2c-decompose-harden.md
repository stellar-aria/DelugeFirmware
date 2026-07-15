# Audio Stream — Phase 2c: decompose + UB-harden the reconstruction core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Now that the reconstruction core (`convert_cluster_data`, `stitch_boundaries`) is extracted and unit-tested, (1) remove the unaligned/strict-aliasing UB in its int32-over-byte accesses, and (2) decompose the two giant functions into focused, independently-readable helpers. Behavior-preserving — the 15 unit specs + golden bit-exact are the safety net.

**Architecture:** The UB fix comes first and centralizes: three shared inline helpers in `convert.h` (`load_word_unaligned`/`store_word_unaligned`/`convert_word_in_place`, memcpy-based — the compiler folds each into one load/store at -O2, so zero-cost and bit-identical) replace all four `reinterpret_cast<int32_t*>` sites (3 in stitch, 1 in convert's word loop). Then the decomposition: `convert_cluster_data` → a thin main + `convert_24bit_range` + `convert_word_range` (template helpers on `Yield`, header-defined); `stitch_boundaries` → a thin dispatcher + `stitch_prev` + `stitch_next` (in `stitch.cpp`'s anonymous namespace).

**Tech Stack:** C++23; CppSpec unit specs (`tests/spec_audio_stream/`: `convert_cluster_spec` 5 cases + `stitch_spec` 4 cases); golden-master gate (`scripts/golden_mixdown.sh`).

## Global Constraints

- **Behavior-preserving.** Every task's gate: `./dbt test` (20/20) + `scripts/golden_mixdown.sh check` (cordae bit-exact) + `FIXTURE=icoustic` (bit-exact). The UB fix (Task 1) is behavior-sensitive — `memcpy` must produce byte-identical output to the `reinterpret_cast` it replaces (it does; same bytes). Decompositions (Tasks 2-3) are pure code-moves. `highsiderr` KNOWN-STALE — A/B if it fails, don't block.
- **Naming (house convention):** snake_case functions/methods/variables; CamelCase types; `_`-suffixed private members. New helpers snake_case.
- **Dependency-light:** `convert.{h,cpp}` and `stitch.cpp` stay free of AudioEngine/Sample/Cluster. The new helpers use only `<cstring>` + `<cstddef>`/`<cstdint>`.
- **No perf regression** in the hot convert word loop — the memcpy helpers MUST compile to a plain load/store (verify by keeping them tiny `inline`; do not add bounds checks or branches).

---

## File Structure

- `src/deluge/storage/audio/stream/convert.h` — add the 3 safe-word helpers; Task 2 decomposes `convert_cluster_data` (template body stays here).
- `src/deluge/storage/audio/stream/stitch.cpp` — Task 1 replaces the 3 casts; Task 3 extracts `stitch_prev`/`stitch_next`.
- (specs unchanged — they already cover both functions; they are the regression net.)

---

## Task 1: safe-word helpers + remove all int32-over-byte UB

**Files:**
- Modify: `src/deluge/storage/audio/stream/convert.h` (add helpers; fix the word loop)
- Modify: `src/deluge/storage/audio/stream/stitch.cpp` (fix the 3 straddle casts)

**Interfaces:**
- Produces (in `convert.h`, namespace `deluge::audio::stream`):
```cpp
inline int32_t load_word_unaligned(const std::byte* p) {
	int32_t w;
	std::memcpy(&w, p, sizeof(w));
	return w;
}
inline void store_word_unaligned(std::byte* p, int32_t w) {
	std::memcpy(p, &w, sizeof(w));
}
// Convert one word at a (possibly unaligned) byte address in place — UB-free replacement for
// `*(int32_t*)p = convert_word(*(int32_t*)p, format)`.
inline void convert_word_in_place(std::byte* p, RawDataFormat format) {
	store_word_unaligned(p, convert_word(load_word_unaligned(p), format));
}
```
(Add `#include <cstring>` to `convert.h`.)

- [ ] **Step 1: Add the three helpers to `convert.h`** (above `convert_cluster_data`, after `convert_word`'s declaration). They are `inline` free functions.

- [ ] **Step 2: Fix `convert_cluster_data`'s word loop** (the `else` / "all other bit depths" branch in the template body). Currently:
```cpp
int32_t* pos;
if (cluster_index == start_cluster) { pos = (int32_t*)&char_data[start_pos & (cluster_size - 1)]; }
else { pos = (int32_t*)&char_data[start_pos & 0b11]; }
int32_t* end_pos; /* ...computed as (int32_t*)&char_data[...] ... */
for (; pos < end_pos; pos++) {
	if (!((uintptr_t)pos & 0b1111111100)) { yield(); }
	*pos = convert_word(*pos, format);
}
```
Replace with byte-pointer iteration (same addresses, step 4), using the safe helper:
```cpp
std::byte* pos;
if (cluster_index == start_cluster) { pos = data.data() + (start_pos & (cluster_size - 1)); }
else { pos = data.data() + (start_pos & 0b11); }
std::byte* end_pos; /* = data.data() + <same end offset as before> */
for (; pos < end_pos; pos += 4) {
	if (!((uintptr_t)pos & 0b1111111100)) { yield(); }
	convert_word_in_place(pos, format);
}
```
Keep the `end_pos` offset computation identical (just build a `std::byte*` at the same offset instead of an `int32_t*`). The yield-cadence check stays `(uintptr_t)pos` (address-based, unchanged). Result is byte-identical.

- [ ] **Step 3: Fix the 3 straddle casts in `stitch.cpp`.** At each of the three sites (currently `auto* this_number = reinterpret_cast<int32_t*>(&<addr>); *this_number = convert_word(*this_number, format);`), replace the two lines with `convert_word_in_place(&<addr>, format);`:
  - `&prev->tail[misalignment]` (the prev other-formats site)
  - `&self_data[start_pos]` (the next other-formats, not-yet-converted site)
  - `&self_data[start_pos]` (the next other-formats, already-converted site)
  Delete the "NOTE (accepted UB)" comment block — it's no longer UB. (Keep the surrounding explanatory comments about *what* is happening.)

- [ ] **Step 4: Build + test + golden.** `dbt build Debug` clean; `./dbt test` 20/20; `scripts/golden_mixdown.sh check` (cordae bit-exact) + `FIXTURE=icoustic` (bit-exact). The output MUST be bit-identical — if it diverges, the byte-offset/end_pos translation is wrong (diff the addresses against the original int32* arithmetic). `FIXTURE=highsiderr` known-stale.

- [ ] **Step 5: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h src/deluge/storage/audio/stream/stitch.cpp
git commit -m "refactor(audio-stream): UB-free int32-over-byte access via memcpy helpers (convert + stitch)"
```

---

## Task 2: decompose `convert_cluster_data`

**Files:**
- Modify: `src/deluge/storage/audio/stream/convert.h`

**Interfaces:**
- Produces (header-defined, `template <class Yield>` where they yield):
  - `convert_24bit_range(std::span<std::byte> data, std::byte* begin, std::byte* end, Yield yield)` — the ENDIANNESS_WRONG_24 3-byte-swap loop (the `while`/inner-`while` with the 1024-byte cadence + the "skip final-chunk yield" guard).
  - `convert_word_range(std::span<std::byte> data, std::byte* begin, std::byte* end, RawDataFormat format, Yield yield)` — the other-formats word loop (using `convert_word_in_place`, the address-based yield cadence).
- `convert_cluster_data` keeps its signature; its body becomes: early-out (`start_pos==0`); if `format != NATIVE` → backup 3 bytes; compute `start_pos`/`start_cluster`; early-out (`cluster_index < start_cluster`); compute the per-format `begin`/`end` byte pointers exactly as today; dispatch to `convert_24bit_range` or `convert_word_range`.

- [ ] **Step 1: Extract `convert_24bit_range`** into `convert.h` (anonymous namespace or a `detail`-style helper above `convert_cluster_data`). Move the ENDIANNESS_WRONG_24 `while (true) { ... byteswap ... yield(); }` loop verbatim, parameterized by `begin`/`end` byte pointers + `yield`. The `pos`/`end_pos` computation for the 24-bit case stays in `convert_cluster_data` (it's format-specific) and is passed in as `begin`/`end`.

- [ ] **Step 2: Extract `convert_word_range`** similarly — the other-formats `for` loop (post-Task-1: `for (pos; pos<end; pos+=4) { if (cadence) yield(); convert_word_in_place(pos, format); }`), parameterized by `begin`/`end`/`format`/`yield`.

- [ ] **Step 3: Rewrite `convert_cluster_data`'s body** to: the two early-outs + the backup + the per-format `begin`/`end` computation (unchanged arithmetic) + a dispatch: `if (format == ENDIANNESS_WRONG_24) convert_24bit_range(data, begin, end, yield); else convert_word_range(data, begin, end, format, yield);`. The main function should now be ~30 lines.

- [ ] **Step 4: Build + test + golden.** `dbt build Debug`; `./dbt test` 20/20; `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact. Pure move — must stay bit-exact.

- [ ] **Step 5: Commit.**
```bash
git add src/deluge/storage/audio/stream/convert.h
git commit -m "refactor(audio-stream): split convert_cluster_data into convert_24bit_range/convert_word_range + thin main"
```

---

## Task 3: decompose `stitch_boundaries`

**Files:**
- Modify: `src/deluge/storage/audio/stream/stitch.cpp`

**Interfaces:**
- Produces (in `stitch.cpp`'s anonymous namespace, alongside `byteswap3`):
  - `void stitch_prev(std::span<std::byte> self_data, StitchPrevEdge& prev, RawDataFormat format, int32_t misalignment, int32_t cluster_index, uint32_t audio_data_start_pos_bytes, size_t cluster_size)` — the entire prev-half body (the unconditional overhang refresh + the per-format straddle conversion + `*prev.end_boundary_converted = true`). Does NOT set `self_start_boundary_converted` (the caller does).
  - `void stitch_next(std::span<std::byte> self_data, StitchNextEdge& next, RawDataFormat format, int32_t misalignment, int32_t cluster_index, uint32_t audio_data_start_pos_bytes, size_t cluster_size)` — the entire next-half body (the `need_copy7` machinery + final copy7). Does NOT set `self_end_boundary_converted`.
- `stitch_boundaries` becomes: `int32_t misalignment = ...; if (prev) { stitch_prev(self_data, *prev, format, misalignment, cluster_index, audio_data_start_pos_bytes, cluster_size); self_start_boundary_converted = true; } if (next) { stitch_next(...); self_end_boundary_converted = true; }`.

- [ ] **Step 1: Extract `stitch_prev`** — move the body inside the current `if (prev != nullptr) { ... }` (everything EXCEPT `self_start_boundary_converted = true;`) into `stitch_prev`, taking `StitchPrevEdge& prev` (so `prev->` becomes `prev.`). Verbatim move.

- [ ] **Step 2: Extract `stitch_next`** — move the body inside the current `if (next != nullptr) { ... }` (everything EXCEPT `self_end_boundary_converted = true;`) into `stitch_next`, taking `StitchNextEdge& next`. Verbatim move (the `bool need_copy7` local + the final `if (need_copy7)` copy7 go inside).

- [ ] **Step 3: Rewrite `stitch_boundaries`** to the thin dispatcher above (misalignment + two guarded calls + the two self-flag writes). Confirm the flag writes land in the same place (after each half, only when that neighbor is present).

- [ ] **Step 4: Build + test + golden.** `dbt build Debug`; `./dbt test` 20/20 (all 4 stitch specs); `scripts/golden_mixdown.sh check` + `FIXTURE=icoustic` bit-exact. Pure move.

- [ ] **Step 5: Commit.**
```bash
git add src/deluge/storage/audio/stream/stitch.cpp
git commit -m "refactor(audio-stream): split stitch_boundaries into stitch_prev/stitch_next + thin dispatcher"
```

---

## Roadmap — after 2c

The reconstruction core is now pure, UB-free, and decomposed. Next: Phase 3 (de-overload `Cluster` → `StreamedChunk`/`ComputedChunk`) per the module design spec, or a test-hygiene sweep of the accumulated Minors (superfluous `audio_format_helpers.cpp` test source; `read_source_spec` `error()==DELUGE_ERR_PARAM` assertion; `mock_read_source` truncate branch; yield-parity `logAction` doc).
