//! SR2d-4 Task 6's cross-path/single-store regression: the ONE test the whole-branch review said
//! was missing (`progress.md`'s "GATE-GAP LEARNING") — every OTHER host gate exercises either
//! `fill_logic::finish_convert_stitch` in isolation (over hand-built plain buffers — `differential.rs`,
//! `tests/fill_logic_host.rs`) or `streaming_loader::fill_once`/`FillOps` against a fully FAKE
//! `FillOps` (`tests/streaming_fill_host.rs`, in `deluge-bsp-rust`), or `ProdOps::begin`/`finish`'s
//! ORCHESTRATION only, with plain local `ConvertState` values standing in for the real store
//! (`tests/host_end_to_end.rs`, this crate). None of them drive `streaming_loader::prod::native_finish`
//! itself — its `try_acquire` → payload-accessor → convert-state-accessor → stitch → write-back chain —
//! over the REAL `StreamedChunk` accessors. That gap is exactly where SR2d-4's Task 2 review found TWO
//! real, silent-corruption Criticals (see `.superpowers/sdd/progress.md`'s "SR2d-4-UNIFY execution"):
//!
//!  1. **The convert-state double-store**: before this arc's unification (commits c8be0f8ae..66e584de2),
//!     the synchronous C++ fill path (`read_cluster_data` → `finish_fill`) wrote convert-state directly
//!     onto `StreamedChunk`, while the async Rust fill task wrote a SEPARATE Rust-side sidecar table —
//!     two disjoint stores. A boundary stitch between a sync-loaded cluster and an async-prefetched
//!     neighbour read the EMPTY store, corrupting the boundary silently on every affected note.
//!     Structurally fixed by routing BOTH paths through `native_finish` over the ONE `StreamedChunk`
//!     accessor store (`deluge_streaming_chunk_convert_state`/`_set_convert_state`) — this test proves
//!     that unification actually holds by driving what were the "two paths" (two separate `finish`
//!     calls, mirroring a sync-loaded neighbour and an async-loaded self) through the SAME real store
//!     and confirming the second call reads the first's write-back, not a zeroed default.
//!  2. **Backing-vs-payload** (commit 9585be616): `native_finish` built each neighbour's payload slice
//!     directly from the raw `try_acquire` pointer (`StreamedChunk*`, the chunk's BACKING) instead of
//!     `deluge_streaming_chunk_payload(p)` (`backing + kChunkPayloadOffset`, the chunk's PAYLOAD) — the
//!     neighbour stitch read/wrote the neighbour's header bytes instead of its samples.
//!
//! ## How this test drives the REAL glue
//!
//! `deluge-bsp-rust` is bin-only, so `src/streaming_loader.rs` (like `src/fill_logic.rs` elsewhere in
//! this crate) is pulled in unmodified via `#[path]`. Its `mod prod` — which holds `native_finish`,
//! `native_begin`, and `ProdOps` — only compiles under `cfg(any(target_os = "none", feature =
//! "host_app"))` (plus `async_streaming_loader`); this crate's `Cargo.toml` declares BOTH features
//! (default-on) so `mod prod` recompiles here on a plain x86-64 host, unmodified. `native_finish`
//! itself stays private — this test never needed it `pub`: `ProdOps` (which owns it) and the `FillOps`
//! trait are already `pub`, and `<ProdOps as FillOps>::finish` is a one-line call straight into
//! `native_finish`, so driving `ops.finish(chunk, true)` on a real `ProdOps` IS driving the real
//! function. **No visibility change to `streaming_loader.rs` was needed or made.**
//!
//! `mod prod`'s `unsafe extern "C"` block declares 14 symbols this test must supply. Eight
//! (`deluge_resource_loader_next`/`_enqueue`, `_slot_of`, `_lease_count_by_slot`, `_chunk_ident`,
//! `_try_acquire`, `_release`, `_mark_ready`) are real `#[no_mangle]` Rust symbols from the
//! `deluge_resource` crate (already a dev-dependency) — genuinely real, no test double. The remaining
//! six are C++-defined in production (`async_fill.cpp`). Compiling THAT file directly was rejected: it
//! is one translation unit whose OTHER functions (the legacy `begin_fill`/`finish_fill` bodies, ~25 weak
//! efatfs fallbacks) reference `Sample`/`SampleStream`/`GeneralMemoryAllocator` and the rest of the
//! app's storage/model closure — the exact "large C++ closure" trap `tests/host_end_to_end.rs`'s module
//! doc already documents empirically for the `host_app` feature (20+ undefined boot-surface symbols).
//! Since a `cc`-compiled `.cpp` is ONE link-time object, pulling in even one symbol from it would drag
//! in every other undefined reference in the same file.
//!
//! So `cpp/native_finish_shim.cpp` (this crate's own `cc`-compiled slice, built by `build.rs`) is the
//! MINIMAL testable seam: it includes ONLY the real, unmodified `storage/cluster/cluster.h` — so
//! `StreamedChunk`'s field layout, `kChunkPayloadOffset`, and `payload()`/`payload_with_trailing_slack()`
//! are the REAL, compiler-computed production ones — and re-states the four accessor bodies
//! (`deluge_streaming_chunk_payload`/`_set_loaded`/`_convert_state`/`_set_convert_state`)
//! CHARACTER-FOR-CHARACTER identical to `async_fill.cpp`'s own definitions (see
//! `accessor_bodies_match_async_fill_cpp_verbatim` below, which greps the live source and fails loudly
//! on drift). The other two symbols the extern block needs (`deluge_streaming_resource_manager`,
//! `deluge_streaming_chunk_unloadable`) are NOT part of either Critical's bug surface (both bugs were in
//! `native_finish`'s own neighbour-payload/convert-state wiring, not in "which manager is the global
//! one") — test-local stand-ins, the same tier `tests/host_end_to_end.rs` already uses for its own
//! `ENTER_CRITICAL_SECTION`/`EXIT_CRITICAL_SECTION`/`deluge_in_interrupt`.
//!
//! ## What this test does NOT exercise
//!
//! - `native_begin` (the sector/byte-offset descriptor arithmetic) — untouched by either Critical
//!   (both were `finish`-side), already covered byte-identically by `fill_logic_host.rs`'s `begin`
//!   tests and `differential.rs`. This test seeds each chunk's payload directly, mirroring "the read
//!   already completed".
//! - The real C++ legacy `begin_fill`/`finish_fill` bodies, `Sample`/`SampleStream`, or
//!   `GeneralMemoryAllocator` — deliberately out of reach (see above); `cluster.sample`/`resource_slot`
//!   are never dereferenced by anything this test calls.
//! - The efatfs read (`ProdOps::read`) — this test never calls it; covered elsewhere
//!   (`fs_differential`, `streaming_fill_host.rs`'s descriptor-plumbing test).
//! - On-device timing/DMA — this is a host, single-threaded, synchronous exercise of the same calls
//!   the async task and the sync fill path make; the real hardware validation is Kate's on-device gate
//!   (non-native-format playback across note-on/seek/loop — see the task brief).
//!
//! ## Teeth: this test was verified (by hand, outside the committed suite) to FAIL on both Criticals
//!
//! Both original bugs were reintroduced locally against this exact test (one at a time, `git
//! checkout`-restored immediately after) and the failure output captured — see the Task 6 report for
//! the exact diffs and failure text. The mechanism each perturbation breaks:
//! - **Backing-vs-payload**: [`CLUSTER_SIZE`] is chosen so `CLUSTER_SIZE + 7` lands STRICTLY between
//!   `sizeof(StreamedChunk)` and `kChunkPayloadOffset` (see [`geometry_and_layout_invariants_hold`]) —
//!   a neighbour-payload read/write from the raw backing pointer instead of the payload accessor would
//!   read/corrupt the struct's own fields and the unnamed front-guard padding, never reaching the real
//!   seeded payload ramp at all, so the stitched output diverges from the C++ reference immediately.
//! - **Double-store**: the single-visit boundaries alone (prev/next both fresh, each visited exactly
//!   once by self) turned out NOT to have teeth for this class by themselves — `stitch_next`/
//!   `stitch_prev`'s "already converted?" branch (`stitch.cpp`) is false on every first visit
//!   regardless of which store answered it, so a neighbour-state read landing on a zeroed default
//!   happens to coincide with the correct "never touched" value there (verified empirically: the
//!   naive version of this test, checking only the single-visit boundaries plus a bare post-hoc
//!   convert-state readback, still PASSED with the double-store bug reintroduced). The part of
//!   `cross_path_finish_matches_cpp_reference_over_real_streamed_chunks` that actually has teeth is
//!   its second act: PREV gets overwritten with fresh bytes ("reloaded") and re-`finish`ed, now with
//!   SELF as its ready NEXT — since self's start_converted is TRUE by then (set by self's own
//!   `finish` above), prev's reload must read that true value back through the SAME store to
//!   correctly take the "already converted" branch (which stages the recompute from self's captured
//!   `unconverted_head`, not its current payload) — a disjoint/empty second store answers false
//!   instead, taking the wrong branch and producing concretely different bytes in prev's reloaded
//!   payload. [`neighbour_convert_state_is_read_back_through_the_shared_store`] complements this by
//!   proving the WRITE side (the flags really do land in the shared store, non-default), but the
//!   reload re-`finish` is what proves the READ side actually depends on it.
#![cfg(not(target_os = "none"))]
// `mod prod`'s `streaming_fill_task` (compiled unconditionally once this crate's
// `async_streaming_loader`/`host_app` features are on — see the module doc) carries
// `#[embassy_executor::task]`, whose expansion needs this nightly feature — same as
// `deluge-bsp-rust`'s own `src/main.rs`. This test never spawns/runs that task; the attribute just
// needs to expand to compile `mod prod` at all.
#![feature(impl_trait_in_assoc_type)]

use core::ffi::c_void;
use core::ptr;

use deluge_resource::{
    BACKING_HEAP, DelugeResource, deluge_resource_create, deluge_resource_define_asset,
    deluge_resource_request, deluge_resource_set_construct,
};

#[path = "../../src/fill_logic.rs"]
mod fill_logic;
// `#[allow(dead_code)]`: this test drives `native_finish` through `ops.finish(...)` directly (a
// plain fn call — see the module doc's "How this test drives the REAL glue"), never through
// `fill_once`/`streaming_fill_task` — so several items `mod prod` needs to compile as a whole (the
// `FillOps` trait's `next`/`is_unloadable`/`begin`/`read`/`lease_count`/`enqueue_lowest`,
// `fill_once` itself, `LOWEST_PRIORITY`, `ProdOps::mgr`, and a few extern declarations only
// `fill_once`'s callers reach) go unused in THIS crate's recompilation, unlike `deluge-bsp-rust`'s
// own binary where `main.rs` wires all of it up. Scoped to this one `mod` item (not a blanket
// crate-level allow) — `streaming_loader.rs` itself is untouched (see the module doc: no
// visibility/behavioural change was made to it).
#[allow(dead_code)]
#[path = "../../src/streaming_loader.rs"]
mod streaming_loader;

use streaming_loader::{FillContext, FillOps, ProdOps};

// Single-threaded-per-test critical section: `deluge_resource`'s `sync::Masked` calls these three
// C-ABI symbols; this test never models the audio-ISR context, so inert no-op stand-ins are
// semantically correct (nothing here can race with itself — a single thread, no concurrent "ISR").
// Same stand-ins `tests/host_end_to_end.rs` already uses.
#[unsafe(no_mangle)]
extern "C" fn ENTER_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn EXIT_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn deluge_in_interrupt() -> bool {
    false
}

/// The C++ shim's (`cpp/native_finish_shim.cpp`) test-only entry points: real `StreamedChunk`
/// construction/introspection plus the two non-Critical-surface plumbing setters (see the module
/// doc). NOT the four accessors under test themselves — those are called only indirectly, through
/// `native_finish`'s own `unsafe extern "C"` block in `mod streaming_loader::prod` — EXCEPT
/// `deluge_streaming_chunk_convert_state`, independently re-declared here too (same real C++
/// symbol, same signature) so a test can read a chunk's convert-state back WITHOUT going through
/// `native_finish` again — see `neighbour_convert_state_is_read_back_through_the_shared_store`.
mod shim {
    use core::ffi::c_void;

    unsafe extern "C" {
        pub fn region_fill_diff_set_cluster_size(size: usize, magnitude: usize);
        pub fn region_fill_diff_chunk_payload_offset() -> usize;
        pub fn region_fill_diff_chunk_backing_size(cluster_size: usize) -> usize;
        pub fn region_fill_diff_chunk_header_size() -> usize;
        pub fn region_fill_diff_chunk_construct(
            ctx: *mut c_void,
            owner: *mut c_void,
            index: u32,
            dest: *mut u8,
        );
        pub fn region_fill_diff_set_active_manager(mgr: *mut c_void);
        // Independent re-declaration of the SAME real accessor `native_finish` itself calls (see
        // `native_finish_shim.cpp`) — used only for read-back assertions, never to drive the fill.
        pub fn deluge_streaming_chunk_convert_state(
            chunk_backing: *mut c_void,
        ) -> super::streaming_loader::DelugeChunkConvertState;
    }
}

/// Small cluster (64 bytes) chosen so `CLUSTER_SIZE + 7` (a neighbour's full
/// `payload_with_trailing_slack()` span) lands strictly between `sizeof(StreamedChunk)` (40 bytes,
/// verified below) and `kChunkPayloadOffset` (72 bytes, verified below) — see
/// [`geometry_and_layout_invariants_hold`] and the module doc's "Teeth" section.
const CLUSTER_SIZE: u32 = 64;
const CLUSTER_MAGNITUDE: u32 = 6; // 2^6 = 64

/// Misaligned start (mirrors `fill_logic`'s own `stitch_geo()`), UNSIGNED_8 (non-native — the
/// corruption class the task brief calls out), audio data long enough that none of clusters 0/1/2 is
/// the last.
fn geo() -> FillContext {
    FillContext {
        efatfs_handle: 0, // unused: this test never calls native_begin / the read step
        audio_data_start_pos_bytes: 1,
        audio_data_length_bytes: 100_000,
        first_cluster_index_with_no_audio_data: 10,
        cluster_size: CLUSTER_SIZE,
        cluster_size_magnitude: CLUSTER_MAGNITUDE,
        raw_data_format: 2, // Unsigned8
    }
}

fn cpp_geo() -> region_fill_differential::cpp_ref::Geometry {
    let g = geo();
    region_fill_differential::cpp_ref::Geometry {
        audio_data_start_pos_bytes: g.audio_data_start_pos_bytes,
        audio_data_length_bytes: g.audio_data_length_bytes,
        first_cluster_index_with_no_audio_data: g.first_cluster_index_with_no_audio_data,
        cluster_size: g.cluster_size as usize,
        cluster_size_magnitude: g.cluster_size_magnitude as usize,
        raw_data_format: g.raw_data_format,
    }
}

/// A manager over a throwaway test heap with one requestable, `StreamedChunk`-backed test asset —
/// mirrors `tests/host_end_to_end.rs::TestManager`, except `construct` placement-news a REAL
/// `StreamedChunk` (`shim::region_fill_diff_chunk_construct`) instead of stamping raw bytes.
struct ChunkHarness {
    handle: *mut DelugeResource,
    asset: u32,
    payload_offset: usize,
    header_size: usize,
    backing_size: usize,
    _buf: Vec<u128>,
}

impl ChunkHarness {
    fn new() -> Self {
        let words = (256 * 1024usize).div_ceil(16);
        let mut buf = vec![0u128; words];
        // SAFETY: `buf` is a live, 16-aligned, `words * 16`-byte arena for as long as `self` (and
        // thus `buf`) is alive.
        let heap = unsafe { fs_alloc::deluge_heap_create(buf.as_mut_ptr() as *mut u8, words * 16) };
        // SAFETY: `heap` is the live handle just created above.
        let handle = unsafe { deluge_resource_create(heap, 4, 8) };
        assert!(!handle.is_null());
        // SAFETY: `handle` is live for the duration of this call.
        let asset = unsafe {
            deluge_resource_define_asset(
                handle,
                ptr::null_mut(),
                None,
                None,
                ptr::null_mut(),
                1,
                BACKING_HEAP,
            )
        };
        // SAFETY: `handle`/`asset` are live and valid; `region_fill_diff_chunk_construct` has the
        // exact `ConstructFn` C-ABI signature.
        unsafe {
            deluge_resource_set_construct(
                handle,
                asset,
                Some(shim::region_fill_diff_chunk_construct),
            );
        }
        // SAFETY: pure queries, no preconditions.
        let payload_offset = unsafe { shim::region_fill_diff_chunk_payload_offset() };
        let header_size = unsafe { shim::region_fill_diff_chunk_header_size() };
        let backing_size =
            unsafe { shim::region_fill_diff_chunk_backing_size(CLUSTER_SIZE as usize) };
        // SAFETY: sets `Cluster::size`/`size_magnitude` before any chunk's payload is touched (no
        // chunk has been requested yet).
        unsafe {
            shim::region_fill_diff_set_cluster_size(
                CLUSTER_SIZE as usize,
                CLUSTER_MAGNITUDE as usize,
            )
        };
        // SAFETY: `handle` is the live manager just created; `region_fill_diff_set_active_manager`
        // stores it for `deluge_streaming_resource_manager` to hand back — must happen before
        // `ProdOps::new()` reads it.
        unsafe { shim::region_fill_diff_set_active_manager(handle as *mut c_void) };

        // Register this asset's fill-context — `native_finish`'s `resolve()` looks this up via
        // `fill_context_for(asset)` (see `streaming_loader.rs`'s `mod prod`); without it every
        // `finish` call fails closed (`resolve` returns `None`) before ever reaching the neighbour
        // gather this test is here to exercise.
        streaming_loader::deluge_streaming_set_fill_context(ptr::null_mut(), asset, geo());

        ChunkHarness {
            handle,
            asset,
            payload_offset,
            header_size,
            backing_size,
            _buf: buf,
        }
    }

    /// Reserve + construct chunk `index` (async/`request` path — no I/O), returning its raw backing
    /// pointer (`== StreamedChunk*`, offset 0 — NOT the payload; see [`Self::payload_of`]).
    fn request(&self, index: u32) -> *mut u8 {
        // SAFETY: `self.handle`/`self.asset` are live and valid; `self.backing_size` is exactly what
        // `region_fill_diff_chunk_construct` (via `Cluster::size`, already set) expects.
        unsafe { deluge_resource_request(self.handle, self.asset, index, self.backing_size) }
    }

    /// The chunk's payload base (`backing + kChunkPayloadOffset`), as a byte slice covering its full
    /// `payload_with_trailing_slack()` span (`CLUSTER_SIZE + 7` bytes).
    ///
    /// # Safety
    /// `backing` must be a live, exclusively-held chunk backing from [`Self::request`], and the
    /// returned slice's lifetime must not outlive that exclusivity.
    unsafe fn payload_of<'a>(&self, backing: *mut u8) -> &'a mut [u8] {
        // SAFETY: `backing` is a resident chunk this harness constructed with `payload_offset` bytes
        // of front guard and `CLUSTER_SIZE + 7` bytes of real payload after it (see `backing_size`'s
        // construction); forwarded to the caller's own exclusivity contract.
        unsafe {
            core::slice::from_raw_parts_mut(
                backing.add(self.payload_offset),
                CLUSTER_SIZE as usize + 7,
            )
        }
    }

    /// The unnamed front-guard padding `[sizeof(StreamedChunk), kChunkPayloadOffset)` — never a real
    /// `StreamedChunk` field, so a correct `finish` must never touch it. See the module doc's
    /// "Teeth" section.
    ///
    /// # Safety
    /// Same contract as [`Self::payload_of`].
    unsafe fn front_guard_of<'a>(&self, backing: *mut u8) -> &'a mut [u8] {
        let len = self.payload_offset - self.header_size;
        // SAFETY: forwarded to the caller; `backing + header_size .. backing + payload_offset` is
        // entirely within this chunk's own backing allocation (`backing_size` bytes).
        unsafe { core::slice::from_raw_parts_mut(backing.add(self.header_size), len) }
    }
}

/// One neighbour/self chunk's request + seeded payload + captured front-guard snapshot, threading
/// everything a test needs to drive `ops.finish` and later assert on it.
struct SeededChunk {
    backing: *mut u8,
    front_guard_before: Vec<u8>,
}

fn seed(h: &ChunkHarness, index: u32, tag: u32) -> SeededChunk {
    let backing = h.request(index);
    assert!(!backing.is_null(), "request failed for index {index}");
    // SAFETY: `backing` was just resident-constructed by `request`, exclusively held here (nothing
    // else touches it until `ops.finish` is called on it below).
    let payload = unsafe { h.payload_of(backing) };
    payload.copy_from_slice(&region_fill_differential::ramp(
        tag,
        CLUSTER_SIZE as usize + 7,
    ));
    // Stamp the front guard with a distinct, recognizable marker BEFORE any `finish` call, so a
    // later re-read can prove it was never touched (see the module doc's "Teeth" section).
    // SAFETY: same as above.
    let guard = unsafe { h.front_guard_of(backing) };
    guard.fill(0xC0u8.wrapping_add(tag as u8));
    let front_guard_before = guard.to_vec();
    SeededChunk {
        backing,
        front_guard_before,
    }
}

/// Full cross-path scenario: three real `StreamedChunk`-backed clusters (prev=0, self=1, next=2)
/// over one real `deluge_resource` manager. `ops.finish` is called on prev, then next, then self —
/// by the time self's `finish` runs, BOTH neighbours are already resident+ready (their own `finish`
/// already published them via `deluge_resource_mark_ready`), so self's `native_finish` genuinely
/// exercises `try_acquire` → the payload accessor → the convert-state accessor → stitch →
/// write-back for both edges — the real cross-path/single-store scenario (mirrors a sync-loaded
/// neighbour, then an async-loaded self, both funnelled through the ONE native tail).
///
/// Asserts byte-identity against the SAME C++ reference (`cpp_ref::finish_fill_over_buffers`) the
/// fill-differential (`differential.rs`) uses, run in the identical 3-call order over independently
/// seeded plain buffers — proving this isn't just "the wiring runs without crashing" but that the
/// REAL glue produces the exact same bytes/flags an independently-implemented C++ orchestration
/// does, for a NON-NATIVE format (the corruption class both Criticals hit).
#[test]
fn cross_path_finish_matches_cpp_reference_over_real_streamed_chunks() {
    let h = ChunkHarness::new();
    let prev = seed(&h, 0, 1000);
    let self_c = seed(&h, 1, 1);
    let next = seed(&h, 2, 2000);

    // `ProdOps::new()` is a safe fn (its own body is what needs `unsafe`, internally, to call
    // `deluge_streaming_resource_manager` — already set to `h.handle` by `ChunkHarness::new()`).
    let ops = ProdOps::new();

    // -- Real cross-path drive: prev, then next (each with no ready neighbour yet), then self
    // (both neighbours now resident+ready). --
    assert!(ops.finish(prev.backing as *mut c_void, true));
    assert!(ops.finish(next.backing as *mut c_void, true));
    assert!(ops.finish(self_c.backing as *mut c_void, true));

    // -- Read back the real StreamedChunk state after the full sequence. --
    // SAFETY: all three backings are still resident (never released/evicted).
    let self_payload_after = unsafe { h.payload_of(self_c.backing) }.to_vec();
    let prev_payload_after = unsafe { h.payload_of(prev.backing) }.to_vec();
    let next_payload_after = unsafe { h.payload_of(next.backing) }.to_vec();

    // -- Independent C++ reference, run in the SAME 3-call order over independently seeded plain
    // buffers (same ramp tags as `seed` above). --
    let mut prev_buf = region_fill_differential::ramp(1000, CLUSTER_SIZE as usize + 7);
    let mut self_buf = region_fill_differential::ramp(1, CLUSTER_SIZE as usize + 7);
    let mut next_buf = region_fill_differential::ramp(2000, CLUSTER_SIZE as usize + 7);
    let g = cpp_geo();

    let mut prev_state = region_fill_differential::cpp_ref::ConvertState::default();
    let mut next_state = region_fill_differential::cpp_ref::ConvertState::default();
    let mut self_state = region_fill_differential::cpp_ref::ConvertState::default();

    // prev's own finish: no prev-of-prev, no next (self not "ready" yet in this replay either).
    region_fill_differential::cpp_ref::finish_fill_over_buffers(
        &mut prev_buf,
        0,
        &g,
        &mut prev_state,
        None,
        None,
    );
    // next's own finish: no prev (self not ready), no next-of-next.
    region_fill_differential::cpp_ref::finish_fill_over_buffers(
        &mut next_buf,
        2,
        &g,
        &mut next_state,
        None,
        None,
    );
    // self's own finish: both neighbours now "ready" — their post-finish buffers/flags feed in.
    let (mut prev_start_unused, mut prev_end) = (false, prev_state.end_converted);
    let (mut next_start, mut next_end_unused) = (next_state.start_converted, false);
    region_fill_differential::cpp_ref::finish_fill_over_buffers(
        &mut self_buf,
        1,
        &g,
        &mut self_state,
        Some(region_fill_differential::cpp_ref::Neighbour {
            payload: &mut prev_buf,
            unconverted_head: &prev_state.first_three_bytes, // unread on the prev side
            start_converted: &mut prev_start_unused,
            end_converted: &mut prev_end,
        }),
        Some(region_fill_differential::cpp_ref::Neighbour {
            payload: &mut next_buf,
            unconverted_head: &next_state.first_three_bytes,
            start_converted: &mut next_start,
            end_converted: &mut next_end_unused,
        }),
    );

    assert_eq!(
        self_payload_after, self_buf,
        "self payload diverged from the C++ reference — the cross-path glue produced different \
         bytes than an independently-implemented orchestration over the SAME inputs/order"
    );
    assert_eq!(
        prev_payload_after, prev_buf,
        "prev payload diverged (self's finish stitches prev's OWN tail bytes)"
    );
    assert_eq!(
        next_payload_after, next_buf,
        "next payload diverged (self's finish stitches next's OWN head bytes)"
    );

    // Sanity: a real boundary conversion actually happened on both edges (misaligned, non-native) —
    // otherwise this whole scenario would vacuously pass with an all-native/no-op stitch.
    assert!(
        self_state.start_converted && self_state.end_converted,
        "expected a real straddle conversion on both self edges — geometry/format sanity check"
    );

    // -- Independent readback through the REAL accessor, bypassing native_finish entirely — proves
    // the write-back landed in the ONE shared StreamedChunk store, not a zeroed default or a
    // different store than what a later reader (or the manager's own next fill) would see. --
    neighbour_convert_state_is_read_back_through_the_shared_store(&prev, &next);

    // -- Cross-path/single-store proof, part 2: PREV gets evicted and reloaded (fresh raw bytes,
    // as if freshly read off disk again) and re-`finish`ed, now with SELF as its ready NEXT
    // neighbour. Self's own finish above already wrote `self.start_converted = true` into the
    // shared store as a side effect (see the `self_state.start_converted` assertion above) — prev's
    // reload MUST see that true value (not a zeroed default) to correctly take
    // `stitch_next`'s "already converted" branch (`stitch.cpp`), which stages the straddle
    // recompute from SELF's OWN CAPTURED PRE-CONVERSION bytes (`unconverted_head`) rather than
    // self's current, already-converted payload — a concretely different, byte-detectable
    // outcome if that neighbour-state read fell back to a disjoint/empty second store (the
    // double-store bug's exact failure mode). The single-visit boundaries proved above (prev/next
    // both fresh, visited exactly once each) can't exercise this branch at all — `already_converted`
    // is false either way on a first visit — so this second act is the part of the scenario that
    // actually distinguishes "shared store" from "double store" for the READ side, complementing
    // `neighbour_convert_state_is_read_back_through_the_shared_store`'s proof for the WRITE side.
    {
        let prev_reload_tag = 3000;
        // SAFETY: `prev.backing` is still resident (never released/evicted) — this overwrite
        // models a fresh disk read into the same still-leased slot, exactly as a real reload
        // would (the manager's own eviction/reload cycle is out of scope here; this test drives
        // `native_finish` directly, mirroring what a reload's post-read call would do).
        unsafe { h.payload_of(prev.backing) }.copy_from_slice(&region_fill_differential::ramp(
            prev_reload_tag,
            CLUSTER_SIZE as usize + 7,
        ));
        assert!(ops.finish(prev.backing as *mut c_void, true));
        // SAFETY: `prev.backing` is still resident.
        let prev_reloaded_after = unsafe { h.payload_of(prev.backing) }.to_vec();

        let mut prev_buf2 =
            region_fill_differential::ramp(prev_reload_tag, CLUSTER_SIZE as usize + 7);
        let mut prev_state2 = region_fill_differential::cpp_ref::ConvertState::default();
        // `self_buf`/`self_state` here are exactly what the THIRD reference call above already
        // computed and verified byte-identical to the real self chunk — self's role as prev's
        // "next" reads its OWN start_converted (true) and unconverted_head (its captured
        // pre-conversion bytes); its end_converted is unused in the "next" role (see
        // `cpp_ref::Neighbour`'s field docs), and self's payload is only READ here, never mutated
        // (the "already converted" stitch path never writes back into `next.head`).
        let mut self_start_for_reload = self_state.start_converted;
        let mut self_end_unused = false;
        region_fill_differential::cpp_ref::finish_fill_over_buffers(
            &mut prev_buf2,
            0,
            &g,
            &mut prev_state2,
            None,
            Some(region_fill_differential::cpp_ref::Neighbour {
                payload: &mut self_buf,
                unconverted_head: &self_state.first_three_bytes,
                start_converted: &mut self_start_for_reload,
                end_converted: &mut self_end_unused,
            }),
        );

        assert_eq!(
            prev_reloaded_after, prev_buf2,
            "prev's RELOAD payload diverged from the C++ reference — the second finish's read of \
             self's shared convert-state (start_converted/unconverted_head) didn't match what \
             self's own finish actually published, the double-store bug's exact symptom"
        );
    }

    // -- Front-guard non-corruption: the unnamed padding between the struct's real fields and its
    // payload must be byte-identical to what `seed` stamped — proves the stitch touched the PAYLOAD
    // region, never the header/front-guard. See the module doc's "Teeth" section. --
    for (name, c) in [("prev", &prev), ("self", &self_c), ("next", &next)] {
        // SAFETY: all three backings are still resident.
        let guard_after = unsafe { h.front_guard_of(c.backing) };
        assert_eq!(
            guard_after, c.front_guard_before,
            "{name}'s front-guard padding was touched by finish — a backing-vs-payload confusion \
             would corrupt exactly this region"
        );
    }
}

/// Independent proof that prev/next's convert-state, as read back through the REAL
/// `deluge_streaming_chunk_convert_state` accessor AFTER self's `finish` wrote it back, matches what
/// self's `finish` actually computed — NOT the zeroed default `ConvertState::default()` a
/// disjoint/empty second store (the pre-unification double-store bug's observable symptom) would
/// report. Split out as its own function (rather than inlined above) so its intent reads as a
/// standalone claim, matching the task brief's own framing ("confirm native_finish(N) reads it back
/// (not a zeroed default)").
fn neighbour_convert_state_is_read_back_through_the_shared_store(
    prev: &SeededChunk,
    next: &SeededChunk,
) {
    // SAFETY: `prev.backing`/`next.backing` are still resident, still valid `StreamedChunk*`s.
    let prev_state_now =
        unsafe { shim::deluge_streaming_chunk_convert_state(prev.backing as *mut c_void) };
    let next_state_now =
        unsafe { shim::deluge_streaming_chunk_convert_state(next.backing as *mut c_void) };

    // self's own edges being converted (asserted above) implies BOTH neighbours' shared boundary
    // flags were flipped true by self's write-back — the exact bit a disjoint-store bug would leave
    // at its zeroed default (false) forever, no matter how many times it's re-read.
    assert!(
        prev_state_now.end_converted,
        "prev's end_converted must read back TRUE through the shared store after self's finish \
         wrote it back — a zeroed default here means the write-back went to a different store than \
         this readback (the double-store bug's exact symptom)"
    );
    assert!(
        next_state_now.start_converted,
        "next's start_converted must read back TRUE through the shared store after self's finish \
         wrote it back — same double-store symptom as prev's end_converted above"
    );
}

/// Sanity/self-check on this test's own layout assumptions (see the module doc's "Teeth" section):
/// `CLUSTER_SIZE + 7` (a neighbour's full payload span) must land strictly between
/// `sizeof(StreamedChunk)` and `kChunkPayloadOffset` — otherwise a hypothetical backing-vs-payload
/// bug's wrong `[0, CLUSTER_SIZE+7)` span could either (a) stay entirely within the struct's own
/// named fields without ever reaching the recognizable front-guard marker, or (b) reach all the way
/// into the REAL payload region, muddying whether a failure came from the right cause. If this ever
/// fails (e.g. `StreamedChunk` grows a field), `CLUSTER_SIZE` needs raising to match — a loud build/
/// test failure here, not silently losing this test's teeth.
#[test]
fn geometry_and_layout_invariants_hold() {
    // SAFETY: pure queries, no preconditions.
    let header_size = unsafe { shim::region_fill_diff_chunk_header_size() };
    let payload_offset = unsafe { shim::region_fill_diff_chunk_payload_offset() };
    let wrong_span_len = CLUSTER_SIZE as usize + 7;
    assert!(
        wrong_span_len > header_size,
        "CLUSTER_SIZE + 7 ({wrong_span_len}) must exceed sizeof(StreamedChunk) ({header_size}) so a \
         backing-vs-payload bug's wrong span reaches the front-guard marker"
    );
    assert!(
        wrong_span_len <= payload_offset,
        "CLUSTER_SIZE + 7 ({wrong_span_len}) must not exceed kChunkPayloadOffset ({payload_offset}) \
         so a backing-vs-payload bug's wrong span never reaches the REAL payload region"
    );
}

/// Guards `cpp/native_finish_shim.cpp`'s four accessor bodies against silently drifting from
/// `async_fill.cpp`'s own production definitions (see the module doc's "How this test drives the
/// REAL glue" section for why the shim can't compile that file directly). Each body below is a
/// character-for-character copy — if a future edit to `async_fill.cpp` changes any of these four
/// functions without updating the shim to match, this test fails loudly instead of the shim quietly
/// testing stale/wrong bodies.
#[test]
fn accessor_bodies_match_async_fill_cpp_verbatim() {
    let live = include_str!("../../../../deluge/storage/audio/stream/async_fill.cpp");
    let bodies = [
        (
            "deluge_streaming_chunk_payload",
            "uint8_t* deluge_streaming_chunk_payload(void* chunk_backing) {\n\treturn reinterpret_cast<uint8_t*>(reinterpret_cast<StreamedChunk*>(chunk_backing)->payload().data());\n}",
        ),
        (
            "deluge_streaming_chunk_set_loaded",
            "void deluge_streaming_chunk_set_loaded(void* chunk_backing) {\n\treinterpret_cast<StreamedChunk*>(chunk_backing)->loaded = true;\n}",
        ),
        (
            "deluge_streaming_chunk_convert_state",
            "DelugeChunkConvertState deluge_streaming_chunk_convert_state(void* chunk_backing) {\n\tauto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);\n\tDelugeChunkConvertState state{};\n\tfor (size_t i = 0; i < 3; ++i) {\n\t\tstate.first_three_bytes[i] = static_cast<uint8_t>(cluster->first_three_bytes_pre_data_conversion[i]);\n\t}\n\tstate.start_converted = cluster->extra_bytes_at_start_converted;\n\tstate.end_converted = cluster->extra_bytes_at_end_converted;\n\treturn state;\n}",
        ),
        (
            "deluge_streaming_chunk_set_convert_state",
            "void deluge_streaming_chunk_set_convert_state(void* chunk_backing, DelugeChunkConvertState state) {\n\tauto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);\n\tfor (size_t i = 0; i < 3; ++i) {\n\t\tcluster->first_three_bytes_pre_data_conversion[i] = static_cast<char>(state.first_three_bytes[i]);\n\t}\n\tcluster->extra_bytes_at_start_converted = state.start_converted;\n\tcluster->extra_bytes_at_end_converted = state.end_converted;\n}",
        ),
    ];
    for (name, body) in bodies {
        assert!(
            live.contains(body),
            "async_fill.cpp's `{name}` body no longer matches cpp/native_finish_shim.cpp's copy — \
             update the shim (and this test's expected text) to match, so the shim keeps testing \
             what's actually shipped"
        );
    }
}
