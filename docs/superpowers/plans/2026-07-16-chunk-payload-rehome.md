# Chunk Payload Hardening + Re-home Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the fragility of the hand-rolled flexible-array-member payload in `StreamedChunk`/`ComputedChunk` — the `dummy[CACHE_LINE_SIZE]` + `data[CACHE_LINE_SIZE]` "MUST BE THE LAST TWO MEMBERS" idiom — by routing all payload access through a typed `payload()` view (Stage 1), then re-homing the payload onto a stored, slot-provenance pointer so the chunk types become pure metadata headers with no FAM over-read and no last-member requirement (Stage 2).

**Architecture:** A cluster chunk is a metadata header immediately followed, in the same resource-manager slab slot, by a `Cluster::size` (32768) byte payload the SD driver DMAs into. Today the payload is a mis-declared `char data[CACHE_LINE_SIZE]` over-read to 32768 bytes; `dummy` + a trailing gap are **load-bearing guards** (see Global Constraints). Stage 1 freezes the layout and adds a typed accessor + compile-time enforcement + documentation. Stage 2 stores an aligned/guarded payload pointer computed from the slot at construct, deletes `data`/`dummy`, and frees the header field order.

**Tech Stack:** C++23 in-tree; `std::span`/`std::byte`; the `deluge_resource`/`deluge_slab` C ABI; `static_assert`/`offsetof`; CppSpec + golden sweep + `padsweep` + `./dbt build Debug`.

## Global Constraints

- **The guards are load-bearing — they may shrink to nothing but must never drop below one cache line.** `data` is NOT cache-line-aligned (`offsetof` 56/60; the slab guarantees only 16-byte alignment). The SD-read cache maintenance (`invalidate_range_all_caches`, RZA1 `sd_read.c`) rounds the buffer range **out** to 32-byte lines, reaching up to 31 bytes before the payload and past it. Additionally, application code deliberately **under-reads** the front guard (the `&data[pos] - 4 + byteDepth` misalignment trick, `sample.cpp`/`time_stretcher.cpp`/`voice_sample.cpp`) and **over-reads/writes** the trailing guard (stitch spans `Cluster::size + 7`; recorder `memcpy`s 5 past). **Invariant to preserve everywhere: ≥ `CACHE_LINE_SIZE` (32) bytes of in-slot guard slack BEFORE the payload and ≥ `CACHE_LINE_SIZE` AFTER it.**
- **Golden gate.** Stage 1 and Stage 2 Tasks 1–2 are **golden-bit-exact** (cordae + icoustic) — the render is address-independent (sim zero-on-acquire + `padsweep`-proven), so re-homing the payload at any in-slot offset with the guards preserved does not change output. Run `padsweep` on any struct-size change. `highsiderr` known-stale — A/B verify, never block.
- **DMA cache-line correctness is a STATIC geometric invariant, proven by `static_assert` — not an empirical/hardware property.** The SD-read cache maintenance rounds the DMA range out by at most one cache line each side (start down to a line, end up to a line, ≤ `CACHE_LINE_SIZE - 1` bytes). Therefore: **guards ≥ `CACHE_LINE_SIZE` each side ⟹ the rounded range stays within guard bytes ⟹ it never touches the chunk header (before the front guard) or the next slot (after the trailing guard).** This holds whether or not the payload is cache-line-aligned, so a `static_assert` on the geometry (front guard ≥ `CACHE_LINE_SIZE`, trailing guard ≥ `CACHE_LINE_SIZE`) is a complete proof of DMA safety — stronger than any test. Neither the sim, qemu, nor hardware is *needed* to verify it: qemu cannot model the cache-vs-DMA coherency the failure mode depends on (it treats memory as coherent, cache-maintenance ops are no-ops; the project's user-mode `qemu-arm` has no cache/DMA at all and the RZA1 `sd_read.c`/`cache.c` path isn't even compiled into those builds), and the sim likewise has no cache maintenance. A hardware smoke is empirical belt-and-suspenders only (mainly confirming `CACHE_LINE_SIZE = 32` matches the real cortex-a9 line size, which it does). **Consequence:** the optional Task 3 (aligning the payload) is NOT a correctness risk — the same guard invariant covers it — it is merely low-benefit; the recommendation to defer it is a cost/benefit call, not a risk one.
- **RT contract.** The per-sample render loop never calls through `->data`/`payload()` — every RT site caches a raw `char*` cursor at cluster-boundary cadence. `payload()` therefore need not be per-sample-cheap; a plain inline accessor is fine.
- **House style (`.clang-tidy`):** snake_case new identifiers; idiomatic C++23; house `///` + `@`-style Doxygen. Commit prefix `refactor(audio-stream):`. clang-format/ruff pre-commit → recover with a FRESH `git commit`, never `--amend`.

## Design decisions (FLAGGED for review; from the surface map)

- **DD1 — `payload()` shape.** `std::span<std::byte> payload()` + a `const` overload on each of `StreamedChunk`/`ComputedChunk`, spanning `[payload base, Cluster::size)`. Sites that under/over-hang the payload keep their pointer arithmetic off `payload().data()` (e.g. `payload().data() + pos - 4 + byteDepth`) — the guard slack still backs those accesses; the accessor just replaces the raw member. Byte base (not `char`) so callers `reinterpret_cast` at the point of use, as they do today.
- **DD2 — Stage 2 stores a pointer, at the SAME slot offset (not aligned) in Task 2.** `std::byte* payload_` set at construct from the slot base (`reinterpret_cast<std::byte*>(dest) + kPayloadOffset`) — slot provenance, which kills the FAM over-read UB. `kPayloadOffset`/`kTrailingGuard` are explicit constants chosen to reproduce the current geometry's guard sizes (≥ `CACHE_LINE_SIZE` each side) and keep the render byte-identical. Cache-line alignment is deferred to the optional Task 3.
- **DD3 — Keep `SampleCluster` and the four construct callbacks as the payload-pointer seat.** The construct callbacks (`cluster_materialize`/`cluster_construct`/`sampleCacheConstruct`/`percCacheConstruct`) already receive the slot `dest` and set metadata; they gain one line to set `payload_`.
- **DD4 — Decomposition:** Stage 1 (accessor + enforcement + docs, layout frozen) → Stage 2 (re-home to stored pointer, delete `data`/`dummy`) → Stage 2 Task 3 (optional align, hardware-gated). Two golden-verifiable increments plus one deferred optional step.

---

## File Structure

- **Modify:** `src/deluge/storage/cluster/cluster.{h,cpp}` — the two chunk structs; add `payload()`, the guard documentation, the `static_assert` enforcement (Stage 1); store `payload_` + delete `data`/`dummy` + explicit geometry constants (Stage 2).
- **Modify (Stage 1 repoint — the ~50 `->data` sites, per the surface map):** `sample_stream.cpp`, `cluster.cpp` (`convert_data_if_necessary`), `cluster_byte_source.cpp`, `sample_low_level_reader.cpp`, `voice_sample.cpp`, `sample.cpp`, `sample_recorder.cpp`, `time_stretcher.cpp`, `waveform_renderer.cpp`, `wave_table.cpp`.
- **Modify (Stage 1 dead-arg cleanup):** `sample_stream.cpp`, `sample.cpp`, `sample_cache.cpp` (the `sizeof(chunk) + Cluster::size` args to `deluge_resource_request`/`acquire` — dead for `BACKING_SLAB`).
- **Modify (Stage 2 construct sites):** `sample_stream.cpp` (`cluster_materialize`/`cluster_construct`), `sample_cache.cpp` (`sampleCacheConstruct`), `sample.cpp` (`percCacheConstruct`).
- **Modify (Stage 2 slot sizing):** `general_memory_allocator.cpp` (the slab-slot size computation → explicit geometry).

---

## STAGE 1 — Hardening (golden-bit-exact, low risk)

### Task 1: introduce `payload()`, document the guards, enforce the invariants

**Files:** Modify `cluster.h` (both structs), `cluster.cpp` (static_asserts + any out-of-line accessor).

- [ ] **Step 1:** Add to `StreamedChunk` and `ComputedChunk`: `[[nodiscard]] std::span<std::byte> payload();` + a `const` overload, returning a span over the payload base (`reinterpret_cast<std::byte*>(data)`) of length `Cluster::size`. Give it `///` Doxygen stating it is the DMA'd cluster payload and that its declared backing (`data`) is a placeholder over-allocated to `Cluster::size` in the slab slot.
- [ ] **Step 2:** Replace the terse `// MUST BE THE LAST TWO MEMBERS` with a documentation block naming the three guard roles (DMA cache-line rounding absorption since `data` is unaligned; the application front-underread; the application trailing over-read/write) and stating the ≥`CACHE_LINE_SIZE`-each-side invariant.
- [ ] **Step 3:** Add `static_assert`s in `cluster.cpp` pinning the fragile invariants for BOTH structs: `data` is the last member (`offsetof(T, data) + CACHE_LINE_SIZE == sizeof(T)`), `dummy` immediately precedes it (`offsetof(T, dummy) + CACHE_LINE_SIZE == offsetof(T, data)`), and `sizeof(T)` is unchanged (a guard against silently perturbing the layout). Keep the existing `is_standard_layout`/`!is_polymorphic` asserts.
- [ ] **Step 4:** `./dbt build Debug` clean (the asserts must pass at current layout); `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep`.
- [ ] **Step 5:** Commit `refactor(audio-stream): add typed payload() view + enforce/document the chunk guard invariants`.

### Task 2: repoint the ~50 `->data` sites to `payload()`

**Files (per the surface map):** `sample_stream.cpp` (:203 read dest, :252/:264/:273 stitch edges — note `:273` is the explicit `Cluster::size + 7` trailing span, :176 the ALPHA align check), `cluster.cpp` (:66 convert span), `cluster_byte_source.cpp:66`, `sample_low_level_reader.cpp` (:45,69,112,115,164,189,204,213,216,229,232,337 — the boundary cursor rebases), `voice_sample.cpp` (:715,864-865,936), `sample.cpp` (:620,658,875,953,1439 — incl. the negative-offset front-underreads), `sample_recorder.cpp` (~20 sites incl. the `+Cluster::size` trailing over-reads at :957,1248,1396), `time_stretcher.cpp` (:744,1170), `waveform_renderer.cpp:483`, `wave_table.cpp:428`.

**Transform:** `cluster->data` (pointer use) → `cluster->payload().data()`; `cluster->data[i]` → `cluster->payload()[i]`; `&cluster->data[i]` → `&cluster->payload()[i]` (or `cluster->payload().data() + i`); casts unchanged (`(int32_t*)&cluster->payload()[x]`). The under/over-hang sites keep their arithmetic off `payload().data()` — the guard slack still backs them, byte-for-byte identical addresses. This is a mechanical, address-preserving pass → bit-exact.

- [ ] **Step 1:** Repoint every site. Because the addresses are identical (`payload().data() == data`), no behaviour changes; the compiler + goldens are the net. Consider splitting by subsystem if reviewing in one pass is unwieldy (reader/voice/timestretch RT-boundary group; recorder group; the cold analysis/UI group; the stream/convert/stitch group).
- [ ] **Step 2:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep`. Grep confirms zero remaining raw `->data`/`.data` on a chunk (except the accessor's own `data` reference in cluster.h).
- [ ] **Step 3:** Commit `refactor(audio-stream): route all chunk payload access through payload()`.

### Task 3: drop the dead slab-request size args

**Files:** `sample_stream.cpp` (:317,:334,:350 `deluge_resource_request`/`acquire(..., sizeof(StreamedChunk) + Cluster::size)`), `sample.cpp:609`, `sample_cache.cpp:222`.

The `size` argument is ignored by `BACKING_SLAB` assets (the slab always returns its fixed `slot_size`; verified in `manager.rs::alloc_backing`). Replace the misleading `sizeof(chunk) + Cluster::size` with a clear constant/comment (e.g. `0` with a `// slab-backed: size ignored, slot governs` note, or a shared `kSlabBackedSizeIgnored`) — pick the form that reads clearest and matches the ABI's documented contract.

- [ ] **Step 1:** Clean the six call sites.
- [ ] **Step 2:** `./dbt build Debug` clean; `./dbt test`; goldens bit-exact (behaviour-neutral — the arg was dead).
- [ ] **Step 3:** Commit `refactor(audio-stream): drop dead slab-request size args (BACKING_SLAB ignores them)`.

### Stage 1 review
Whole-stage review: the accessor is address-preserving, the static_asserts enforce the guard invariants, every site repointed, the dead args gone — all golden-bit-exact.

---

## STAGE 2 — Re-home the payload (golden-bit-exact core; optional hardware-gated align)

### Task 1: store `payload_`, set at construct (same address, coexistence)

**Files:** `cluster.h` (add member + change `payload()` to return it), the four construct callbacks (`sample_stream.cpp` cluster_materialize/construct, `sample_cache.cpp` sampleCacheConstruct, `sample.cpp` percCacheConstruct).

- [ ] **Step 1:** Add `std::byte* payload_ = nullptr;` to each chunk (a normal header member — NOT last, order-free). `payload()` returns `{payload_, Cluster::size}`.
- [ ] **Step 2:** In each construct callback, set `cluster->payload_ = reinterpret_cast<std::byte*>(&cluster->data);` (the SAME address as today — keep `data`/`dummy` for this task; coexistence). This proves the stored-pointer path with byte-identical addresses.
- [ ] **Step 3:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) bit-exact; `padsweep`.
- [ ] **Step 4:** Commit `refactor(audio-stream): address the chunk payload via a stored payload_ pointer`.

### Task 2: explicit slot geometry; delete `data`/`dummy`; free the layout

**Files:** `cluster.{h,cpp}` (delete `data`/`dummy`, add geometry constants + static_asserts), the four construct callbacks (`payload_ = slot base + kPayloadOffset`), `general_memory_allocator.cpp` (slot sizing).

- [ ] **Step 1:** Define explicit geometry constants (in cluster.h): `kPayloadOffset` (bytes from the slot base to the payload — chosen ≥ header size AND leaving a ≥`CACHE_LINE_SIZE` front guard) and `kTrailingGuard` (≥ `CACHE_LINE_SIZE`). Choose values that keep the total slot size and the front/trailing guard ≥ one cache line — the render is address-independent so the exact offset is free, but the DMA invariant (≥cache-line guards) MUST hold. `static_assert` both guards ≥ `CACHE_LINE_SIZE`.
- [ ] **Step 2:** Delete `dummy` + `data` (and the "MUST BE THE LAST TWO MEMBERS" comment + the Stage-1 last-member asserts). The chunk structs are now pure metadata + `payload_`; field order is unconstrained.
- [ ] **Step 3:** In the construct callbacks, set `cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kPayloadOffset;` (slot-provenance pointer → the FAM over-read UB is gone). Confirm `dest` is the slab slot base at each site.
- [ ] **Step 4:** `general_memory_allocator.cpp`: slot size = `kPayloadOffset + Cluster::size + kTrailingGuard` (explicit geometry, not `sizeof(chunk)`). `static_assert`/verify it is ≥ the Stage-1 slot size for both chunk types (never under-provision).
- [ ] **Step 5:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) — **target bit-exact** (address-independent render + preserved guards). If a fixture shifts, it is a deterministic heap-layout shift (slot size changed) not a behaviour change — A/B verify + re-baseline, and confirm the guard invariants still hold. `padsweep`.
- [ ] **Step 6:** **DMA due-diligence (documented, not CI-verifiable):** confirm in the report that the front and trailing guards remain ≥ `CACHE_LINE_SIZE`, so the SD cache-maintenance rounding still lands only in guard bytes — preserving the existing hardware behaviour by construction. Note that a hardware smoke (load + play a streamed sample) is prudent though the change targets byte-identical DMA behaviour.
- [ ] **Step 7:** Commit `refactor(audio-stream): re-home chunk payload onto explicit slot geometry; drop the FAM`.

### Task 3 (OPTIONAL — low-benefit, recommend defer): cache-line-align the payload

Aligning `payload_` to a cache-line boundary (`kPayloadOffset` a multiple of `CACHE_LINE_SIZE`) would let the DMA front-rounding stop over-reaching before the payload — a marginal tidiness gain. It is **not a correctness risk**: the guard invariant (front/trailing guard ≥ `CACHE_LINE_SIZE`, `static_assert`-enforced in Task 2) proves DMA safety for aligned or unaligned payloads alike (see Global Constraints). The front guard is still needed regardless, for the application front-underread. Since the DMA already works fine unaligned and aligning buys almost nothing, **recommend NOT doing this** unless a measured problem motivates it — a cost/benefit call, not a risk one.

- [ ] **Step 1 (only if pursued):** Set `kPayloadOffset` to a `CACHE_LINE_SIZE` multiple; keep both guards ≥ one cache line (`static_assert` still holds — this is what proves it correct).
- [ ] **Step 2:** `./dbt build Debug` clean; `./dbt test`; goldens (cordae + icoustic) — bit-exact (render is address-independent); `padsweep`.
- [ ] **Step 3:** Optional hardware smoke as belt-and-suspenders (stream a large non-native-format sample under memory pressure) — confidence, not a gate; the `static_assert` is the verification.
- [ ] **Step 4:** Commit `refactor(audio-stream): cache-line-align the chunk payload`.

### Stage 2 review
Whole-stage review: the FAM over-read is gone (slot-provenance `payload_`), `data`/`dummy` + the last-member requirement are gone, the guard invariants are `static_assert`-enforced, and (Tasks 1–2) the render is bit-exact with the DMA guards preserved by construction.

---

## Verification (whole increment)

- Per-task: `./dbt build Debug` + `./dbt test` (20/20) + goldens + `padsweep`. Stage 1 + Stage 2 Tasks 1–3 all target bit-exact (the render is address-independent).
- **DMA cache-line safety is verified by the `static_assert` on the guard geometry (≥ `CACHE_LINE_SIZE` each side), not by any test.** That assert is a proof — no CI path (sim or qemu) can exercise the cache-vs-DMA coherency the guards address, and none needs to. A hardware smoke is optional empirical confidence, never a gate.

## After this follow-on

The chunk types are pure headers; the payload is an explicit, documented, provenance-correct slot region. Natural next steps if desired: the same `payload()` view is the seam a future Rust port addresses; and the deferred Phase-3 dead-code (`resource_lease_asset_id`, `ComputedChunk::sampleCache`) + the batched test-hygiene ticket remain.

## Self-Review notes (author)

- Spec coverage: the fragility items (FAM over-read UB, unenforced MUST-BE-LAST, implicit/undocumented guards, dead slab-size args) all map to Stage-1/Stage-2 tasks. The guards themselves are preserved (they are load-bearing) — the win is safety/clarity, not a smaller layout.
- Open risks to surface at review: (a) Stage 2 Task 2's `kPayloadOffset`/`kTrailingGuard` values must keep both guards ≥ `CACHE_LINE_SIZE` — the one place a subtle error would reintroduce the DMA hazard. It is NOT golden-catchable, but it IS caught by the required `static_assert` on the guard geometry (the assert is the verification; goldens/qemu/hardware cannot exercise the cache-vs-DMA path). (b) The recorder's explicit `Cluster::size + 5` over-reads and the `-4 + byteDepth` front-underreads must keep landing in guard slack after re-home — the repoint (Stage 1) is address-preserving so this holds, but Stage 2's offset change must keep ≥ the same slack. (c) Confirm `dest` at every construct callback is the true slot base (the value the slab returned), not an interior pointer, so `payload_` has whole-slot provenance.
