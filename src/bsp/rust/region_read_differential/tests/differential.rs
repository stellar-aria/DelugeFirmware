//! U1 Task 5 — the reader differential, the rung's gate: drives `deluge_sample_reader`'s
//! `open`/`window`/`advance`/`deluge_sample_read` over REAL, placement-new'd `StreamedChunk`s
//! (a real `deluge_resource` manager, a synthetic sample whose converted cluster bytes are KNOWN)
//! and asserts the bytes it returns are BYTE-IDENTICAL to the CURRENT non-voice read path — the
//! real `StreamedChunk::frame_read_origin`/`payload_with_trailing_slack()` (`storage/cluster/
//! cluster.h`), reached through `cpp/harness_shim.cpp`'s `region_read_diff_frame_direct`/
//! `_via_origin` oracle (see that file's own module doc for the derivation from the real
//! production function).
//!
//! ## Why this is a genuine, non-vacuous cross-check
//!
//! [`mapping`] (this crate's `src/mapping.rs`) is an INDEPENDENT reimplementation of the
//! frame -> (cluster, byte-offset) arithmetic, kept deliberately separate from
//! `deluge_sample_reader::reader`'s own (private) `locate`/`Geometry` — see that module's own doc.
//! The oracle bytes this file fetches come from the REAL `StreamedChunk` via the REAL
//! `frame_read_origin`/`payload_with_trailing_slack()` (C++, `cc`-compiled, untouched by this
//! crate), addressed using this crate's OWN mapping. The reader under test resolves clusters and
//! offsets through its OWN (different) code path (`Reader::window`'s `locate`/`acquire_and_fill`/
//! the self-pin). If either side's arithmetic — or the reader's straddle/stitch handling — diverges
//! from the real read path, the byte comparison below catches it directly, not a hand-derived
//! approximation of it.
//!
//! ## The synthetic sample
//!
//! Every case builds a fresh [`ChunkHarness`]: a real `deluge_resource` manager (slab-backed, the
//! same backing kind production streaming clusters use) over a real heap, with `N` real
//! `StreamedChunk`s placement-new'd via `deluge_resource_request` (never payload == backing — the
//! SR2d-4 lesson `deluge_sample_reader`'s own tests already flag; payload is always reached through
//! the real `deluge_streaming_chunk_payload` accessor). Each cluster's own `cluster_size` bytes are
//! seeded with a deterministic, per-cluster-index ramp (`region_read_differential::ramp`), then
//! EVERY cluster is `native_finish`ed, in increasing index order, so the REAL stitch
//! (`deluge_sample_convert::stitch_boundaries`, via `deluge_sample_fill::native_finish`) publishes
//! each cluster's own head into its predecessor's trailing 7-byte slack — the exact mechanism
//! `Reader::window`'s own straddle handling relies on (see its doc). Every geometry here uses
//! `RawDataFormat::Native` (0), so the convert step is a no-op and the seeded ramp survives into the
//! resident payload unchanged, and `audio_data_start_bytes == 0`, so [`mapping::Geometry::total_frames`]'s
//! simple "frame's whole span fits in `[0, audio_data_length_bytes)`" EOF rule holds exactly (see its
//! own doc). All `N` clusters are pre-filled before any reader is opened, so `deluge_efatfs_read_at`
//! (this file's own stub, always failing) is never actually exercised except by the ONE dedicated
//! forced-failure case, which deliberately leaves its cluster unconstructed.
#![cfg(not(target_os = "none"))]

use core::ffi::c_void;
use core::ptr;

use deluge_resource::{
    BACKING_SLAB, deluge_resource_create, deluge_resource_define_asset, deluge_resource_request,
    deluge_resource_set_construct, deluge_resource_set_slab,
};
use deluge_sample_fill::{FillContext, deluge_streaming_set_fill_context};
use deluge_sample_reader::abi::{
    DelugeFrameWindow, deluge_sample_read, deluge_sample_reader_advance,
    deluge_sample_reader_close, deluge_sample_reader_ok, deluge_sample_reader_open,
    deluge_sample_reader_seek, deluge_sample_reader_window,
};
use deluge_sample_reader::reader::ReadHint;

use region_read_differential::mapping::{self, Geometry as MapGeo};
use region_read_differential::ramp;

// -- Single-threaded-per-test critical section: `deluge_resource`'s `sync::Masked` calls these
// three C-ABI symbols; every test here is a plain, single-threaded exercise (never the audio-ISR
// context), so inert no-op stand-ins are semantically correct. Same stand-ins
// `region_fill_differential`'s own test files use. --
#[unsafe(no_mangle)]
extern "C" fn ENTER_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn EXIT_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn deluge_in_interrupt() -> bool {
    false
}

/// `deluge_sample_reader::reader::Reader`'s synchronous card-read primitive
/// (`include/libdeluge/streaming_fill.h`). Unconditionally fails, without touching `dst` — this
/// harness pre-fills and `native_finish`es every cluster it constructs BEFORE any reader is opened
/// (see the module doc), so `acquire_leased` is always a cache hit and this stub is never reached
/// except by [`forced_read_failure_sets_not_ok_distinct_from_eof`], which deliberately leaves its
/// one cluster unconstructed so the reader must fall through to a real fill attempt — exactly the
/// case this stub exists to fail.
#[unsafe(no_mangle)]
extern "C" fn deluge_efatfs_read_at(
    _handle: u32,
    _byte_offset: u32,
    _dst: *mut c_void,
    _count: u32,
    _out_read: *mut u32,
) -> bool {
    false
}

/// `cpp/harness_shim.cpp`'s own entry points — real `StreamedChunk` construction/introspection plus
/// the differential's own oracle reads. See that file's module doc for what each does.
mod shim {
    use core::ffi::c_void;

    unsafe extern "C" {
        pub fn region_read_diff_set_cluster_size(size: usize, magnitude: usize);
        pub fn region_read_diff_chunk_backing_size(cluster_size: usize) -> usize;
        pub fn region_read_diff_chunk_construct(
            ctx: *mut c_void,
            owner: *mut c_void,
            index: u32,
            dest: *mut u8,
        );
        pub fn region_read_diff_set_active_manager(mgr: *mut c_void);
        pub fn region_read_diff_frame_direct(
            chunk_backing: *mut c_void,
            byte_offset: u32,
            frame_bytes: u32,
            out: *mut u8,
        );
        pub fn region_read_diff_frame_via_origin(
            chunk_backing: *mut c_void,
            byte_offset: u32,
            byte_depth: u8,
            num_channels: u8,
            out: *mut u8,
        );
        // Needed here too (not just internally by `deluge_sample_fill`/`deluge_sample_reader`) so
        // this harness can seed a chunk's payload through the REAL accessor, never payload ==
        // backing (the SR2d-4 lesson).
        pub fn deluge_streaming_chunk_payload(chunk_backing: *mut c_void) -> *mut u8;
    }
}

/// Serializes every test in this file — mirrors `deluge_sample_reader`'s own `host_streaming_stubs::TEST_LOCK`:
/// `deluge_sample_fill`'s per-asset `FILL_CONTEXTS` table is a GLOBAL static keyed by bare asset id
/// (each fresh manager hands out ids starting from 0), and `cpp/harness_shim.cpp`'s
/// `g_active_manager` is a single (non-thread-local) global too — two tests running concurrently
/// (`cargo test`'s default) could otherwise collide on either. Every test takes this lock for its
/// whole run.
static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// 512 bytes (`2^9`) — realistic minimum cluster size (>= one 512-byte sector), matching
/// `deluge_sample_reader`'s own `reader::tests::window_tests`.
const CLUSTER_SIZE: u32 = 512;
const CLUSTER_MAGNITUDE: u32 = 9;

/// 24-bit mono: `byte_depth = 3, num_channels = 1` -> `frame_stride = 3`, which does NOT divide
/// `CLUSTER_SIZE` (`512 % 3 == 2`) — the real case `sample_recorder.cpp` sets, and per the brief the
/// stride the straddle/scan cases must use: this is where the boundary arithmetic matters (T3's own
/// bugs lived exactly here).
const BYTE_DEPTH: u8 = 3;
const NUM_CHANNELS: u8 = 1;
const FRAME_STRIDE: u32 = BYTE_DEPTH as u32 * NUM_CHANNELS as u32;

/// `num_full_clusters` full (`CLUSTER_SIZE`-byte) clusters, plus an optional trailing short cluster
/// of `short_last_cluster_bytes` real bytes (`0` for none — every cluster is full). The harness must
/// then pre-fill `num_full_clusters + (short_last_cluster_bytes > 0) as u32` clusters total.
fn fill_context(num_full_clusters: u32, short_last_cluster_bytes: u32) -> FillContext {
    let audio_data_length_bytes =
        num_full_clusters as u64 * CLUSTER_SIZE as u64 + short_last_cluster_bytes as u64;
    FillContext {
        efatfs_handle: 0,
        audio_data_start_pos_bytes: 0,
        audio_data_length_bytes,
        first_cluster_index_with_no_audio_data: -1, // Native format never reads this (see module doc)
        cluster_size: CLUSTER_SIZE,
        cluster_size_magnitude: CLUSTER_MAGNITUDE,
        raw_data_format: 0, // Native -- convert is a no-op, the seeded ramp survives verbatim
        byte_depth: BYTE_DEPTH,
        num_channels: NUM_CHANNELS,
    }
}

fn map_geometry(ctx: &FillContext) -> MapGeo {
    MapGeo {
        audio_data_start_bytes: ctx.audio_data_start_pos_bytes,
        audio_data_length_bytes: ctx.audio_data_length_bytes,
        cluster_size_bytes: ctx.cluster_size,
        byte_depth: ctx.byte_depth,
        num_channels: ctx.num_channels,
    }
}

/// A real manager + real, placement-new'd `StreamedChunk`s over a real heap — see the module doc.
struct ChunkHarness {
    asset: u32,
    /// Cluster index -> its resident backing pointer. Every entry is held under a lease taken by
    /// this harness's own initial `deluge_resource_request` call and NEVER released — the chunk
    /// stays resident (and un-evictable) for this harness's whole life, regardless of what a reader
    /// under test later acquires/releases on top (mirrors
    /// `region_fill_differential::tests::native_finish_glue::ChunkHarness`'s identical convention).
    backings: Vec<*mut u8>,
    /// Kept alive for the harness's whole life; never read directly.
    _arena: Vec<u128>,
}

impl ChunkHarness {
    /// `ctx.audio_data_length_bytes` determines `num_clusters` implicitly (the caller passes the
    /// SAME `ctx` this constructs from); `prefill` is the number of clusters to actually seed +
    /// `native_finish` (0 for the forced-failure case, which wants a fresh, never-filled chunk that
    /// the reader itself must construct on demand).
    fn new(ctx: FillContext, prefill: u32) -> Self {
        let words = (4 * 1024 * 1024usize).div_ceil(16); // 4 MiB arena -- ample for a handful of clusters
        let mut arena: Vec<u128> = vec![0u128; words];
        let base = arena.as_mut_ptr() as *mut u8;
        // SAFETY: `arena` is a live, 16-aligned, `words * 16`-byte allocation for as long as `self`
        // (and thus `arena`) is alive.
        let heap = unsafe { deluge_alloc::deluge_heap_create(base, words * 16) };
        assert!(!heap.is_null());

        let backing_size =
            unsafe { shim::region_read_diff_chunk_backing_size(CLUSTER_SIZE as usize) };
        // SAFETY: `heap` is the live handle just created above; capacity comfortably exceeds every
        // case's own cluster count.
        let slab =
            unsafe { deluge_alloc::slab::deluge_slab_create_unmanaged(heap, backing_size, 32) };
        assert!(!slab.is_null());
        // SAFETY: `heap` is live.
        let handle = unsafe { deluge_resource_create(heap, 4, 32) };
        assert!(!handle.is_null());
        // SAFETY: `handle`/`slab` are both live, over the same heap.
        unsafe { deluge_resource_set_slab(handle, slab) };
        // SAFETY: `handle` is live.
        let asset = unsafe {
            deluge_resource_define_asset(
                handle,
                ptr::null_mut(),
                None,
                None,
                ptr::null_mut(),
                1,
                BACKING_SLAB,
            )
        };
        // SAFETY: `handle`/`asset` are live/valid; `region_read_diff_chunk_construct` has the exact
        // `ConstructFn` C-ABI signature.
        unsafe {
            deluge_resource_set_construct(
                handle,
                asset,
                Some(shim::region_read_diff_chunk_construct),
            );
        }
        // SAFETY: sets `Cluster::size`/`size_magnitude` before any chunk's payload is touched (no
        // chunk has been requested yet).
        unsafe {
            shim::region_read_diff_set_cluster_size(
                CLUSTER_SIZE as usize,
                CLUSTER_MAGNITUDE as usize,
            )
        };
        // SAFETY: `handle` is the live manager just created; must happen before any reader call or
        // `native_finish` call reads it back.
        unsafe { shim::region_read_diff_set_active_manager(handle as *mut c_void) };
        deluge_streaming_set_fill_context(ptr::null_mut(), asset, ctx);

        let mut backings = Vec::with_capacity(prefill as usize);
        for index in 0..prefill {
            // SAFETY: `handle`/`asset` are live/valid; `backing_size` matches the slab's own slot
            // size (BACKING_SLAB ignores the `size` argument regardless -- see `cluster.h`'s
            // `kSlabBackedSizeIgnored` doc).
            let backing = unsafe { deluge_resource_request(handle, asset, index, backing_size) };
            assert!(!backing.is_null(), "request failed for cluster {index}");
            // SAFETY: `backing` was just resident-constructed above; its payload is `CLUSTER_SIZE`
            // bytes, reached through the REAL accessor (never payload == backing -- the SR2d-4
            // lesson).
            let payload = unsafe { shim::deluge_streaming_chunk_payload(backing as *mut c_void) };
            let seed = ramp(index, CLUSTER_SIZE as usize);
            // SAFETY: `payload` is `CLUSTER_SIZE` bytes, exclusively held here (nothing else
            // touches it until `native_finish` below); `seed` is exactly `CLUSTER_SIZE` bytes.
            unsafe { core::ptr::copy_nonoverlapping(seed.as_ptr(), payload, seed.len()) };
            backings.push(backing);
        }
        // Finish every cluster in increasing order -- Native format's convert step is a no-op, so
        // the seeded ramp survives; the REAL stitch still runs, publishing each cluster's own head
        // into its predecessor's trailing slack once the predecessor is already resident+ready
        // (see the module doc).
        for (index, &backing) in backings.iter().enumerate() {
            assert!(
                deluge_sample_fill::native_finish(backing as *mut c_void, true),
                "native_finish failed for cluster {index}"
            );
        }

        ChunkHarness {
            asset,
            backings,
            _arena: arena,
        }
    }

    fn backing(&self, index: u32) -> *mut u8 {
        self.backings[index as usize]
    }
}

/// Fetch one frame's `frame_bytes` bytes via the direct oracle (`region_read_diff_frame_direct`) --
/// the current read path's own bytes at `(cluster_index, byte_offset)`, straight from the real,
/// resident `StreamedChunk`.
fn oracle_frame(
    h: &ChunkHarness,
    cluster_index: u32,
    byte_offset: u32,
    frame_bytes: u32,
) -> Vec<u8> {
    let backing = h.backing(cluster_index);
    let mut out = vec![0u8; frame_bytes as usize];
    // SAFETY: `backing` is a resident, `native_finish`ed chunk (`ChunkHarness::new`); `byte_offset +
    // frame_bytes` stays within `payload_with_trailing_slack()`'s `cluster_size + 7` bytes for
    // every case this file drives (frame_bytes <= 4, byte_offset < cluster_size).
    unsafe {
        shim::region_read_diff_frame_direct(
            backing as *mut c_void,
            byte_offset,
            frame_bytes,
            out.as_mut_ptr(),
        )
    };
    out
}

/// The independent oracle read: walk `num_frames` frames from `start_frame` in `direction`,
/// stopping at [`MapGeo::total_frames`]'s own end-of-audio boundary, concatenating each frame's
/// [`oracle_frame`] bytes in VISITED order (so a backward walk's output is naturally comparable to
/// the reader's own backward-visited byte order -- see [`handle_read`]'s doc).
fn oracle_read(
    h: &ChunkHarness,
    geo: &MapGeo,
    start_frame: u64,
    num_frames: u32,
    direction: i8,
) -> Vec<u8> {
    let total = geo.total_frames();
    let mut out = Vec::new();
    let mut frame = start_frame as i64;
    for _ in 0..num_frames {
        if frame < 0 || frame as u64 >= total {
            break;
        }
        let (cluster_index, byte_offset) = mapping::locate(frame as u64, geo);
        out.extend(oracle_frame(h, cluster_index, byte_offset, FRAME_STRIDE));
        frame += direction as i64;
    }
    out
}

/// Drive the reader's zero-copy handle (`open` -> `window`/`advance` -> `close`), collecting every
/// visited frame's bytes in VISITED order: for a forward reader that's increasing frame order (the
/// window's pointer plus increasing byte offsets); for a backward reader (`direction == -1`),
/// `window()`'s own contract has the pointer sit at the CURRENT (highest) frame of the run with
/// `frame_count` frames extending toward DECREASING addresses (see
/// `deluge_sample_reader::reader::Reader::window`'s own doc) -- so this walks `k` from `0..n` with a
/// NEGATIVE per-frame stride when `direction < 0`, landing on the same decreasing-frame-index
/// sequence [`oracle_read`] produces for the same `direction`.
///
/// Returns `(collected_bytes, frames_visited, reader_ok_at_the_end)`.
fn handle_read(
    asset: u32,
    start_frame: u64,
    num_frames: u32,
    direction: i8,
    hint: ReadHint,
) -> (Vec<u8>, u32, bool) {
    let ptr = deluge_sample_reader_open(asset, start_frame, direction, hint);
    assert!(!ptr.is_null());
    let mut collected = Vec::new();
    let mut remaining = num_frames;
    let mut visited = 0u32;
    loop {
        if remaining == 0 {
            break;
        }
        // SAFETY: `ptr` is live, not yet closed.
        let w: DelugeFrameWindow = unsafe { deluge_sample_reader_window(ptr) };
        if w.frame_count == 0 {
            break;
        }
        let n = w.frame_count.min(remaining);
        let base = w.frames as *const u8;
        for k in 0..n {
            let step = if direction >= 0 {
                k as isize
            } else {
                -(k as isize)
            };
            // SAFETY: `base` is `window`'s own pinned, resident payload pointer; per its contract
            // `n <= frame_count` whole (possibly straddling) frames of `FRAME_STRIDE` bytes are
            // valid starting at `base` and continuing in the reader's own `direction` -- exactly
            // the span `base + step*FRAME_STRIDE .. + FRAME_STRIDE` this loop reads.
            let frame_ptr = unsafe { base.offset(step * FRAME_STRIDE as isize) };
            for b in 0..FRAME_STRIDE {
                // SAFETY: see above.
                collected.push(unsafe { *frame_ptr.add(b as usize) });
            }
        }
        // SAFETY: `ptr` is live, not yet closed.
        unsafe { deluge_sample_reader_advance(ptr, n) };
        remaining -= n;
        visited += n;
    }
    // SAFETY: `ptr` is live, not yet closed.
    let ok = unsafe { deluge_sample_reader_ok(ptr) };
    // SAFETY: `ptr` is live, not yet closed, and not used again after this call.
    unsafe { deluge_sample_reader_close(ptr) };
    (collected, visited, ok)
}

/// Drive the stateless `deluge_sample_read` copy-out (always forward, `ReadHint::Cached` — see the
/// header's own contract). Returns `(bytes_written, frames_written)`.
fn via_read(asset: u32, start_frame: u64, num_frames: u32) -> (Vec<u8>, u32) {
    let dest_bytes = num_frames as usize * FRAME_STRIDE as usize;
    let mut dest = vec![0xAAu8; dest_bytes];
    // SAFETY: `dest` is exactly `dest_bytes` bytes long.
    let written = unsafe {
        deluge_sample_read(
            asset,
            start_frame,
            num_frames,
            dest.as_mut_ptr() as *mut c_void,
            dest_bytes,
        )
    };
    dest.truncate(written as usize * FRAME_STRIDE as usize);
    (dest, written)
}

// =====================================================================================================
// The battery: within-cluster, straddle (fwd/bwd), last-cluster short read, reverse full scan, whole
// forward scan, random-seek sequence -- every one asserting handle_read == oracle_read, and (forward
// cases) via_read == oracle_read too, so all three agree.
// =====================================================================================================

#[test]
fn within_one_cluster_forward_matches_oracle_and_read() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0); // 3 full clusters, 1536 bytes, total_frames = 512
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);

    let (start, count) = (10u64, 20u32); // well inside cluster 0 -- no boundary in play
    let expected = oracle_read(&h, &geo, start, count, 1);
    let (actual, visited, ok) = handle_read(h.asset, start, count, 1, ReadHint::Cached);
    assert_eq!(visited, count);
    assert!(ok);
    assert_eq!(
        actual, expected,
        "handle read diverged from the current-path oracle"
    );

    let (via, via_frames) = via_read(h.asset, start, count);
    assert_eq!(via_frames, count);
    assert_eq!(
        via, expected,
        "deluge_sample_read diverged from the current-path oracle"
    );
    assert_eq!(
        via, actual,
        "deluge_sample_read and the handle loop must agree with each other"
    );
}

#[test]
fn straddle_forward_across_one_cluster_boundary_matches_oracle_and_read() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);

    // Frame 165..185: crosses cluster 0/1's boundary (the straddling frame sits at frame 170, byte
    // 510) and continues well past it into cluster 1's own plain payload -- the "boundary re-pin
    // near cluster ends with a non-dividing stride" edge T3 documented, and the re-pin across the
    // boundary within one continuous read.
    let (start, count) = (165u64, 20u32);
    let expected = oracle_read(&h, &geo, start, count, 1);
    let (actual, visited, ok) = handle_read(h.asset, start, count, 1, ReadHint::Cached);
    assert_eq!(visited, count);
    assert!(ok);
    assert_eq!(actual, expected);

    let (via, via_frames) = via_read(h.asset, start, count);
    assert_eq!(via_frames, count);
    assert_eq!(via, expected);
}

#[test]
fn straddle_backward_across_one_cluster_boundary_matches_oracle() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);

    // Start at frame 185 (cluster 1, offset 43) and read 25 frames backward -- lands on frame 161
    // (cluster 0, offset 483), crossing the SAME boundary as the forward case above, in reverse.
    let (start, count) = (185u64, 25u32);
    let expected = oracle_read(&h, &geo, start, count, -1);
    let (actual, visited, ok) = handle_read(h.asset, start, count, -1, ReadHint::Cached);
    assert_eq!(visited, count);
    assert!(ok);
    assert_eq!(actual, expected);
}

#[test]
fn last_cluster_short_read_matches_oracle_and_read() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    // 3 full clusters + a short 4th cluster (100 real bytes) -> audio_data_length_bytes = 1636,
    // total_frames = 545 (1636 / 3, floor -- the trailing 1-byte partial frame is NOT valid).
    let ctx = fill_context(3, 100);
    let h = ChunkHarness::new(ctx, 4);
    let geo = map_geometry(&ctx);
    assert_eq!(geo.total_frames(), 545);

    // Ask for 20 frames starting at 540 -- only 5 real frames remain (540..545); must come back
    // short, not padded, and reader_ok() must stay true (this is genuine end-of-audio, not a
    // failure).
    let (start, count) = (540u64, 20u32);
    let expected = oracle_read(&h, &geo, start, count, 1);
    assert_eq!(expected.len(), 5 * FRAME_STRIDE as usize);

    let (actual, visited, ok) = handle_read(h.asset, start, count, 1, ReadHint::Cached);
    assert_eq!(visited, 5, "only 5 real frames remain past frame 540");
    assert!(ok, "running out of real audio is EOF, not a failure");
    assert_eq!(actual, expected);

    let (via, via_frames) = via_read(h.asset, start, count);
    assert_eq!(via_frames, 5);
    assert_eq!(via, expected);
}

#[test]
fn reverse_full_sample_scan_matches_oracle() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);
    let total = geo.total_frames();

    let expected = oracle_read(&h, &geo, total - 1, total as u32, -1);
    assert_eq!(expected.len(), total as usize * FRAME_STRIDE as usize);

    let (actual, visited, ok) = handle_read(h.asset, total - 1, total as u32, -1, ReadHint::Scan);
    assert_eq!(
        visited as u64, total,
        "a full backward scan must visit every frame"
    );
    assert!(ok);
    assert_eq!(
        actual, expected,
        "a whole-sample backward scan diverged from the oracle"
    );
}

#[test]
fn whole_sample_forward_scan_matches_oracle_and_read_and_ends_at_true_eof() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);
    let total = geo.total_frames();

    let expected = oracle_read(&h, &geo, 0, total as u32, 1);
    assert_eq!(expected.len(), total as usize * FRAME_STRIDE as usize);

    let (actual, visited, ok) = handle_read(h.asset, 0, total as u32, 1, ReadHint::Scan);
    assert_eq!(visited as u64, total);
    assert_eq!(actual, expected);

    // Distinct-from-failure check: reading one MORE frame past the end must report EOF
    // (frame_count == 0) while reader_ok() stays TRUE -- contrast with
    // `forced_read_failure_sets_not_ok_distinct_from_eof` below, where frame_count == 0 too but
    // reader_ok() is false.
    let ptr = deluge_sample_reader_open(h.asset, total, 1, ReadHint::Cached);
    // SAFETY: `ptr` is live.
    let w = unsafe { deluge_sample_reader_window(ptr) };
    assert!(w.frames.is_null());
    assert_eq!(w.frame_count, 0);
    // SAFETY: `ptr` is live.
    assert!(
        unsafe { deluge_sample_reader_ok(ptr) },
        "true end-of-audio must leave reader_ok() true"
    );
    // SAFETY: `ptr` is live, not used again after this call.
    unsafe { deluge_sample_reader_close(ptr) };
    assert!(ok);

    let (via, via_frames) = via_read(h.asset, 0, total as u32);
    assert_eq!(via_frames as u64, total);
    assert_eq!(via, expected);
}

#[test]
fn random_seek_sequence_matches_oracle_at_every_stop() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);
    let total = geo.total_frames();

    // A deterministic "random" hop sequence (not sequential, crosses boundaries in both
    // directions, includes a couple of near-the-end positions) -- one reader, `seek` between
    // stops, `window`/`advance` after each.
    let stops: [u64; 8] = [37, 402, 5, 170, 511, 0, 256, 340];
    const PER_STOP: u32 = 6;

    let ptr = deluge_sample_reader_open(h.asset, 0, 1, ReadHint::Cached);
    for &frame in &stops {
        // SAFETY: `ptr` is live.
        unsafe { deluge_sample_reader_seek(ptr, frame) };
        let want = PER_STOP.min((total - frame) as u32);
        let expected = oracle_read(&h, &geo, frame, want, 1);

        let mut collected = Vec::new();
        let mut remaining = want;
        while remaining > 0 {
            // SAFETY: `ptr` is live.
            let w = unsafe { deluge_sample_reader_window(ptr) };
            if w.frame_count == 0 {
                break;
            }
            let n = w.frame_count.min(remaining);
            let base = w.frames as *const u8;
            for b in 0..(n * FRAME_STRIDE) {
                // SAFETY: `base` is `window`'s own pinned payload, valid for `n * FRAME_STRIDE`
                // bytes forward from here per its own contract.
                collected.push(unsafe { *base.add(b as usize) });
            }
            // SAFETY: `ptr` is live.
            unsafe { deluge_sample_reader_advance(ptr, n) };
            remaining -= n;
        }
        assert_eq!(
            collected, expected,
            "seek to frame {frame} then reading {want} frames diverged from the oracle"
        );
    }
    // SAFETY: `ptr` is live.
    assert!(unsafe { deluge_sample_reader_ok(ptr) });
    // SAFETY: `ptr` is live, not used again.
    unsafe { deluge_sample_reader_close(ptr) };
}

/// A case that forces a read failure (rather than EOF): a cluster the harness never constructs, so
/// the reader must fall through to `Reader::acquire_and_fill`'s fresh-`request` + `fill_now` path,
/// which calls this file's own `deluge_efatfs_read_at` stub -- unconditionally `false`. Asserts
/// `reader_ok()` -> false, DISTINCT from `frame_count == 0 && reader_ok() == true`
/// (`whole_sample_forward_scan_matches_oracle_and_read_and_ends_at_true_eof`'s own EOF check).
#[test]
fn forced_read_failure_sets_not_ok_distinct_from_eof() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(1, 0); // one full cluster's worth of real audio -- but never prefilled
    let h = ChunkHarness::new(ctx, 0); // prefill = 0: cluster 0 is registered geometry-wise, never resident

    let ptr = deluge_sample_reader_open(h.asset, 0, 1, ReadHint::Cached);
    // SAFETY: `ptr` is live.
    assert!(
        unsafe { deluge_sample_reader_ok(ptr) },
        "open itself must still succeed (geometry resolves)"
    );
    // SAFETY: `ptr` is live.
    let w = unsafe { deluge_sample_reader_window(ptr) };
    assert!(w.frames.is_null());
    assert_eq!(w.frame_count, 0);
    // SAFETY: `ptr` is live.
    assert!(
        !unsafe { deluge_sample_reader_ok(ptr) },
        "a forced read failure must clear reader_ok() -- distinct from true EOF, which leaves it true"
    );
    // SAFETY: `ptr` is live, not used again.
    unsafe { deluge_sample_reader_close(ptr) };
}

// =====================================================================================================
// Oracle self-check: the simpler `_direct` oracle used throughout this file agrees with the
// brief-mandated `frame_read_origin`-based oracle, so using `_direct` everywhere above stays tied,
// by proof, to the real production function.
// =====================================================================================================

#[test]
fn direct_oracle_matches_frame_read_origin_oracle() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(2, 0); // 2 full clusters -- enough to exercise a straddle near cluster 0's tail
    let h = ChunkHarness::new(ctx, 2);

    // A spread of byte offsets in cluster 0, including two that reach past `CLUSTER_SIZE` (512)
    // into cluster 1's stitched trailing slack: byte_offset 510's 3-byte frame is [510, 511, 512],
    // and 511's is [511, 512, 513] -- both genuinely straddle the boundary.
    for &byte_offset in &[0u32, 1, 61, 400, 509, 510, 511] {
        let mut via_origin = vec![0u8; FRAME_STRIDE as usize];
        // SAFETY: `h.backing(0)` is resident and `native_finish`ed; `byte_offset + BYTE_DEPTH *
        // NUM_CHANNELS` stays within `payload_with_trailing_slack()`'s span for every offset above.
        unsafe {
            shim::region_read_diff_frame_via_origin(
                h.backing(0) as *mut c_void,
                byte_offset,
                BYTE_DEPTH,
                NUM_CHANNELS,
                via_origin.as_mut_ptr(),
            )
        };
        let direct = oracle_frame(&h, 0, byte_offset, FRAME_STRIDE);
        assert_eq!(
            via_origin, direct,
            "the direct oracle and the frame_read_origin oracle disagreed at byte_offset {byte_offset}"
        );
    }
}

/// Guards `cpp/harness_shim.cpp`'s four `StreamedChunk` accessor bodies against silently drifting
/// from `async_fill.cpp`'s own production definitions — the same drift guard
/// `region_fill_differential`'s `tests/native_finish_glue.rs` already runs over ITS OWN copy of
/// these bodies; this crate keeps a THIRD copy (see `harness_shim.cpp`'s own doc), so it needs the
/// same guard.
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
            "async_fill.cpp's `{name}` body no longer matches cpp/harness_shim.cpp's copy -- update \
             the shim (and this test's expected text) to match, so the shim keeps testing what's \
             actually shipped"
        );
    }
}

// =====================================================================================================
// NON-VACUITY (mandatory, the SR2d-4 lesson): a passing comparison above proves nothing unless a
// REAL divergence is provably caught. Perturb the oracle's own frame position/geometry by exactly
// the amounts a genuinely wrong reader offset/stride would produce, and confirm the SAME comparison
// this file uses throughout now FAILS.
// =====================================================================================================

#[test]
fn non_vacuity_an_off_by_one_start_frame_is_detected() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);

    let (start, count) = (165u64, 20u32); // the same straddling range as the forward straddle case
    let (actual, _, _) = handle_read(h.asset, start, count, 1, ReadHint::Cached);
    let expected = oracle_read(&h, &geo, start, count, 1);
    assert_eq!(
        actual, expected,
        "precondition: the real (unperturbed) comparison must agree before perturbing"
    );

    // Perturb the OFFSET the oracle is read from by one frame -- standing in for a hypothetical
    // reader bug that resolved `current_frame` one frame off (T3's own boundary bugs lived exactly
    // in this class of off-by-one).
    let mutated = oracle_read(&h, &geo, start + 1, count, 1);
    assert_ne!(
        actual, mutated,
        "non-vacuity FAILED: a one-frame-offset oracle mutation was not detected -- the comparison \
         has no teeth"
    );
}

#[test]
fn non_vacuity_an_off_by_one_stride_is_detected() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let ctx = fill_context(3, 0);
    let h = ChunkHarness::new(ctx, 3);
    let geo = map_geometry(&ctx);

    let (start, count) = (10u64, 20u32); // a plain within-cluster range -- isolates the stride mutation
    let (actual, _, _) = handle_read(h.asset, start, count, 1, ReadHint::Cached);
    let expected = oracle_read(&h, &geo, start, count, 1);
    assert_eq!(
        actual, expected,
        "precondition: the real comparison must agree before perturbing"
    );

    // Perturb the STRIDE (byte_depth) the oracle maps frames with by one byte -- standing in for a
    // hypothetical reader bug that resolved the wrong frame stride from geometry.
    let mutated_geo = MapGeo {
        byte_depth: geo.byte_depth + 1,
        ..geo
    };
    let mutated = oracle_read(&h, &mutated_geo, start, count, 1);
    assert_ne!(
        actual, mutated,
        "non-vacuity FAILED: a one-byte stride mutation was not detected -- the comparison has no \
         teeth"
    );
}
