//! The Rust async cluster-fill task (R1.1) and its BSP wiring (R2.1): drains the
//! resource manager's loader queue (`deluge_resource_loader_next`) on the same
//! thread-mode Embassy executor the fiber pump and the C++ enqueue path run on,
//! awaiting the actual SD read instead of running it synchronously inside the
//! C++ `pump()` fiber op.
//!
//! ## Two compilation tiers
//!
//! This module compiles in two tiers, because the selector the C++ side calls
//! (`deluge_streaming_async_active`) must resolve on the Embassy BSP regardless
//! of whether the async backing is actually enabled — a cargo feature can't
//! reach a C++ `#define`, so "is the async task active" has to be a runtime
//! call, and that call needs a real symbol to link against either way:
//!
//! - Always compiled whenever this module is (i.e. on the Embassy BSP, device
//!   or `host_app` — see the `mod streaming_loader;` cfg in `main.rs`), no
//!   matter the `async_streaming_loader` feature: [`FILL_WAKE`], the two
//!   `extern "C"` selector/wakeup functions just below it
//!   (`deluge_streaming_async_active`, `deluge_streaming_signal_fill`), and the
//!   per-asset fill-context table (`deluge_streaming_set_fill_context`,
//!   [`fill_context_for`]) — asset definition, which registers it, happens on
//!   every BSP at sample-load, not just the ones with the async fill task
//!   built in. Every other BSP/config (legacy/host-cooperative sim, rza1)
//!   never links this crate at all; the `__attribute__((weak))` C++ fallbacks
//!   in `async_fill.cpp` resolve there instead.
//! - Feature-gated behind `async_streaming_loader`: the actual drain machinery
//!   ([`FillOps`], [`fill_once`], [`ProdOps`], [`streaming_fill_task`]). Spawned
//!   in `main.rs` (device `main` + host `host_app`) only under that feature.
//!
//! ## Why the drain loop is generic over [`FillOps`]
//!
//! Constructing a real `StreamedChunk`/`SampleStream`/`Sample` plus a seeded
//! resource manager inside a Rust host test is impractical, and mocking the
//! `#[no_mangle]` C symbols below would collide with the real ones whenever this
//! crate also links the C++ app (`host_app`). So the orchestration itself
//! ([`fill_once`]) takes its four operations — `next`/`begin`/`read`/`finish` (plus
//! the failure-path re-enqueue) — through the [`FillOps`] trait instead of calling
//! the `extern "C"` functions directly. [`ProdOps`] below wires that trait to the
//! real C ABI + the efatfs streaming read (`crate::efatfs_fs`/`crate::efatfs_host_shim`); the
//! host unit test
//! (`tests/streaming_fill_host.rs`) wires it to an in-memory fake queue instead.
//! This is the whole reason [`fill_once`] is independently testable — the real
//! end-to-end fill (real manager, real chunks) is validated later, in R1.2 (TSan)
//! and R3.2 (full scenario).
//!
//! ## Concurrency
//!
//! The manager (`DelugeResource*`) is `!Send`/`!Sync` by design — single-executor
//! only. [`streaming_fill_task`] and the C++ enqueue path (`loader.cpp`) both run
//! on the one thread-mode Embassy executor the whole app runs on, so the raw
//! pointer never needs to (and must never) cross threads.
use core::cell::RefCell;
use core::ffi::c_void;

use embassy_sync::blocking_mutex::Mutex;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::signal::Signal;

/// Raised to wake [`streaming_fill_task`] out of its idle wait — e.g. when a
/// cluster is newly enqueued. Same shape as `fiber::WORKER_WAKE`. Always
/// compiled (see the module doc's "Two compilation tiers") — with the feature
/// off, or before the task is spawned, nobody ever awaits it; signalling it is
/// just a harmless flag set.
pub static FILL_WAKE: Signal<CriticalSectionRawMutex, ()> = Signal::new();

// ── Always-compiled C ABI: selector + wakeup (R2.1) ─────────────────────────
// See `include/libdeluge/streaming_fill.h`'s doc comments for the C-side contract.

/// Whether the async streaming-fill task owns the loader queue on this
/// build. The return value is the only thing that depends on the cargo
/// feature — the symbol itself must always exist so `loader.cpp`'s call site
/// links regardless of which config produced this BSP image.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_streaming_async_active() -> bool {
    cfg!(feature = "async_streaming_loader")
}

/// Whether the embedded-fatfs streaming READ path owns the read on this build.
/// Like [`deluge_streaming_async_active`], the return value is the only thing
/// that depends on the cargo feature — the symbol itself must always exist so
/// the C++ call site (`streaming_fill.h`) links regardless of config. See that
/// header's `deluge_streaming_efatfs_active` doc for the C-side contract; the
/// real `deluge_efatfs_open`/`_close` bridge lives in `efatfs_fs.rs`.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_streaming_efatfs_active() -> bool {
    cfg!(feature = "efatfs_streaming")
}

/// Wake [`streaming_fill_task`] out of its idle wait. Safe to call whether or
/// not the task exists yet — [`Signal::signal`] just records "latest value
/// pending"; a `Signal` nobody is waiting on drops the previous pending value
/// (if any) and stores the new one, which is fine here since the payload is
/// `()` (a pure wakeup, not data).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_streaming_signal_fill() {
    FILL_WAKE.signal(());
}

// ── Always-compiled C ABI: per-asset fill-context table (SR2d-4 Task 1) ─────
// Registered by C++ at sample-load (`SampleStream::ensure_resource_asset()`/`open_read_stream()`,
// see `sample_stream.cpp`, and `SampleRecorder::setup()`/`finalizeRecordedFile()` — SR2d-4 Task 5);
// read by the native fill task via [`fill_context_for`] (`prod::ProdOps::begin`/`finish`, SR2d-4
// Task 5). See `include/libdeluge/streaming_fill.h`'s doc for the C-side contract.

/// Fixed capacity for the per-asset fill-context table, keyed by asset id. Mirrors `kAssetCap`
/// (`general_memory_allocator.cpp`) — the resource manager's asset-table capacity — so every asset
/// id the manager can ever hand out has a slot here. A plain literal, not read from the C++ side (the
/// two are kept in sync by hand); if `kAssetCap` is ever raised, raise this too. A `DELUGE_RESOURCE_NO_ASSET`
/// (`u32::MAX`) id is naturally rejected by the same bounds check as any other out-of-range id.
///
/// At this size (4096 × 32 bytes = 128 KiB) the table does NOT fit in on-chip SRAM at debug
/// opt-levels — measured: `cargo device`'s `dev`-profile link failed with the SRAM region
/// overflowing (a `.bss` placement here pushed `.ARM.exidx` past the RTT reservation), while the
/// SAME table built clean under `--release` (see `linker/memory_rtt.x`'s own "a large live feature
/// ... needs [release-like density] to fit at debug opt-levels" caveat — this table just measurably
/// hit that ceiling too). Rather than shrink the table below a size that would make it useless for
/// real per-asset coverage, [`FILL_CONTEXTS`] is placed in SDRAM instead (`.sdram_bss`, 64 MiB, a
/// rounding error at this size) on device — the same fix already used for `fiber::WORKER_STACK`,
/// `audio::RENDER_BLOCK`/`INPUT_BLOCK`, and `ffi_extra::NEWLIB_HEAP`.
const FILL_CONTEXT_CAP: usize = 4096;

/// Mirrors `include/libdeluge/streaming_fill.h`'s `DelugeStreamingFillContext` exactly (verbatim
/// field order/types) — the per-asset geometry `deluge_streaming_set_fill_context` registers at
/// sample-load and the native fill task reads via [`fill_context_for`]. See that header's doc for
/// what each field means; `raw_data_format` mirrors
/// `RawDataFormat` (`audio_file_format.h`) as its `u8` underlying representation, not re-exposed as a
/// Rust enum here (nothing on this side interprets it yet).
#[repr(C)]
#[cfg_attr(test, derive(PartialEq, Eq, Debug))]
#[derive(Clone, Copy)]
pub struct FillContext {
    pub efatfs_handle: u32,
    pub audio_data_start_pos_bytes: u32,
    pub audio_data_length_bytes: u64,
    pub first_cluster_index_with_no_audio_data: i32,
    pub cluster_size: u32,
    pub cluster_size_magnitude: u32,
    pub raw_data_format: u8,
}

impl FillContext {
    /// The table's "unregistered" sentinel: `cluster_size == 0` never occurs in a real registration
    /// (`Cluster::size` is always a nonzero power of two — see `sample_stream.cpp`'s
    /// `register_fill_context`), so it's used in place of `Option<FillContext>`'s discriminant. That
    /// saves the tag's padding (`Option<FillContext>` is 40 bytes vs. this struct's own 32 — see the
    /// FFI layout guard below) across [`FILL_CONTEXT_CAP`] entries, which matters here: see that
    /// constant's doc for why this table is on a tight SRAM budget.
    const UNREGISTERED: FillContext = FillContext {
        efatfs_handle: 0,
        audio_data_start_pos_bytes: 0,
        audio_data_length_bytes: 0,
        first_cluster_index_with_no_audio_data: 0,
        cluster_size: 0,
        cluster_size_magnitude: 0,
        raw_data_format: 0,
    };
}

/// FFI layout guard, mirroring the `static_assert`s in `async_fill.cpp` — see that file's comment
/// for the byte-offset derivation. Unlike [`StreamingFillDescriptor`], this struct holds no pointer,
/// so the layout is identical on the 32-bit device and the 64-bit host_app build: two leading `u32`s,
/// then the `u64` (already 8-aligned), then `i32`/`u32`/`u32`, then the trailing `u8`, padded up to
/// the `u64` member's 8-byte alignment.
const _: () = {
    assert!(core::mem::offset_of!(FillContext, efatfs_handle) == 0);
    assert!(core::mem::offset_of!(FillContext, audio_data_start_pos_bytes) == 4);
    assert!(core::mem::offset_of!(FillContext, audio_data_length_bytes) == 8);
    assert!(core::mem::offset_of!(FillContext, first_cluster_index_with_no_audio_data) == 16);
    assert!(core::mem::offset_of!(FillContext, cluster_size) == 20);
    assert!(core::mem::offset_of!(FillContext, cluster_size_magnitude) == 24);
    assert!(core::mem::offset_of!(FillContext, raw_data_format) == 28);
    assert!(size_of::<FillContext>() == 32);
};

/// The per-asset fill-context table: written on the main/load thread
/// (`deluge_streaming_set_fill_context`, called from `SampleStream::ensure_resource_asset()`/
/// `open_read_stream()` at sample-load) and read by [`streaming_fill_task`] (`prod::ProdOps::begin`/
/// `finish`, via [`fill_context_for`]). Neither side ever runs on the audio render ISR — asset definition happens at
/// sample-load, and the fill task runs on the same thread-mode Embassy executor the C++ enqueue path
/// does (see the module doc's "Concurrency" section) — so this table doesn't need the resource
/// manager's asymmetric ISR-skipping `Masked` critical section (`deluge_resource::sync`). Using it
/// here would also be the wrong dependency: this file compiles standalone (via `#[path]`) in the
/// plain host unit test (`tests/streaming_fill_context_host.rs`), which does NOT link `deluge_resource`
/// — only `host_app` pulls that crate in (see `Cargo.toml`). A plain `embassy_sync` blocking
/// `Mutex<CriticalSectionRawMutex, _>` — the exact primitives [`FILL_WAKE`] above already uses —
/// needs no new mechanism and no new dependency, and is available in every context this file compiles
/// in (device, `host_app`, and the plain-host test recompile alike).
///
/// `.sdram_bss` on device only (see [`FILL_CONTEXT_CAP`]'s doc for why): that section is zeroed by
/// `boot_mem::init_sdram_memory()` before any app code runs (including this table's first possible
/// writer/reader), so the all-zero initializer below — every entry `FillContext::UNREGISTERED` — is
/// exactly what's already there; the explicit initializer just keeps `host_app`/host-test behaviour
/// (plain `.bss`, zeroed by the normal C runtime/process image) identical in substance.
#[cfg_attr(target_os = "none", unsafe(link_section = ".sdram_bss"))]
static FILL_CONTEXTS: Mutex<CriticalSectionRawMutex, RefCell<[FillContext; FILL_CONTEXT_CAP]>> =
    Mutex::new(RefCell::new([FillContext::UNREGISTERED; FILL_CONTEXT_CAP]));

/// Register (or replace) asset `asset`'s streaming fill-context. See
/// `include/libdeluge/streaming_fill.h`'s doc for the C-side contract; `mgr` is unused today (there
/// is exactly one process-wide resource manager) but kept in the signature for parity with the rest
/// of the manager-scoped C ABI. Out-of-range asset ids (`>= FILL_CONTEXT_CAP`, which also catches
/// `DELUGE_RESOURCE_NO_ASSET`) are silently ignored, as is the pathological `ctx.cluster_size == 0`
/// (indistinguishable from [`FillContext::UNREGISTERED`] — see its doc; never occurs from the real
/// C++ caller).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_streaming_set_fill_context(
    _mgr: *mut c_void,
    asset: u32,
    ctx: FillContext,
) {
    if asset as usize >= FILL_CONTEXT_CAP || ctx.cluster_size == 0 {
        return;
    }
    FILL_CONTEXTS.lock(|table| table.borrow_mut()[asset as usize] = ctx);
}

/// Look up `asset`'s registered fill-context, or `None` if it was never registered (or `asset` is out
/// of range). Read side of [`deluge_streaming_set_fill_context`]; wired into `prod::ProdOps::begin`/
/// `finish` (SR2d-4 Task 5, its real, non-test caller). `#[allow(dead_code)]`: `mod prod` only
/// compiles on the device or under `host_app` (see the module doc's "Two compilation tiers"), so a
/// PLAIN host build/clippy of this file (no `--target`, no `host_app` — including
/// `tests/streaming_fill_host.rs`'s own `#[path]` recompilation, which needs `async_streaming_loader`
/// but deliberately not `host_app`) still never reaches this call site outside `#[cfg(test)]`
/// elsewhere (`tests/streaming_fill_context_host.rs`, which recompiles this same file WITHOUT
/// `mod prod` either and calls this directly).
#[allow(dead_code)]
pub fn fill_context_for(asset: u32) -> Option<FillContext> {
    if asset as usize >= FILL_CONTEXT_CAP {
        return None;
    }
    let ctx = FILL_CONTEXTS.lock(|table| table.borrow()[asset as usize]);
    (ctx.cluster_size != 0).then_some(ctx)
}

/// Mirrors `include/libdeluge/streaming_fill.h`'s `StreamingFillDescriptor`
/// exactly (verbatim field order/types) — this is the C-ABI boundary type.
#[cfg(feature = "async_streaming_loader")]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StreamingFillDescriptor {
    pub dest: *mut u8,
    pub num_sectors: u32,
    pub ok: bool,
    pub handle: u32,
    pub byte_offset: u32,
}

/// FFI layout guard (M4), mirroring the `static_assert`s in `async_fill.cpp` — see that file's
/// comment for the byte-offset derivation. `core::mem::offset_of!` + `size_of` are both `const`,
/// so this is a compile-time check with no runtime cost; a field-order/type drift on either side
/// fails the build instead of silently corrupting the read across the boundary. After `ok` (1 byte
/// at ptr+4) come 3 pad bytes, then `handle` at ptr+8 and `byte_offset` at ptr+12, and the struct
/// pads up to the pointer's alignment → ptr+16 (20 on the 4-byte-ptr device, 24 on the 8-byte-ptr
/// host).
#[cfg(feature = "async_streaming_loader")]
const _: () = {
    assert!(core::mem::offset_of!(StreamingFillDescriptor, dest) == 0);
    assert!(core::mem::offset_of!(StreamingFillDescriptor, num_sectors) == size_of::<*mut u8>());
    assert!(core::mem::offset_of!(StreamingFillDescriptor, ok) == size_of::<*mut u8>() + 4);
    assert!(core::mem::offset_of!(StreamingFillDescriptor, handle) == size_of::<*mut u8>() + 8);
    assert!(
        core::mem::offset_of!(StreamingFillDescriptor, byte_offset) == size_of::<*mut u8>() + 12
    );
    assert!(size_of::<StreamingFillDescriptor>() == size_of::<*mut u8>() + 16);
};

/// Mirrors `include/libdeluge/streaming_fill.h`'s `DelugeChunkConvertState` exactly (verbatim
/// field order/types) — the per-chunk convert-state get/set accessors added in SR2d-4 Task 1,
/// ahead of the later task that rewires the native fill's `finish` onto this store instead of its
/// own `fill_sidecar.rs` table. Declared unconditionally alongside [`StreamingFillDescriptor`]
/// (not `async_streaming_loader`-gated on its own) since it shares that struct's C-ABI-mirror
/// role, but nothing outside `mod prod`'s (not-yet-called) extern declarations below references it
/// yet, so it only actually needs to exist where those do.
#[cfg(feature = "async_streaming_loader")]
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DelugeChunkConvertState {
    pub first_three_bytes: [u8; 3],
    pub start_converted: bool,
    pub end_converted: bool,
}

/// FFI layout guard (SR2d-4 Task 1), mirroring the `static_assert`s in `async_fill.cpp` — see that
/// file's comment for the byte-offset derivation. No pointer members, and every member (`[u8; 3]`
/// then two `bool`s) is 1-byte-aligned, so the layout is identical on the 32-bit device and the
/// 64-bit host_app build: no padding anywhere, laid out back-to-back.
#[cfg(feature = "async_streaming_loader")]
const _: () = {
    assert!(core::mem::offset_of!(DelugeChunkConvertState, first_three_bytes) == 0);
    assert!(core::mem::offset_of!(DelugeChunkConvertState, start_converted) == 3);
    assert!(core::mem::offset_of!(DelugeChunkConvertState, end_converted) == 4);
    assert!(size_of::<DelugeChunkConvertState>() == 5);
};

/// `kLowestLoaderPriority` (`loader.cpp`) — re-enqueue value for a cluster whose
/// read just failed while still wanted, so it sinks behind everything else
/// instead of being popped again immediately.
#[cfg(feature = "async_streaming_loader")]
const LOWEST_PRIORITY: u32 = 0xFFFF_FFFF;

/// The four operations [`fill_once`] drains the loader queue through. `chunk`
/// values are opaque `StreamedChunk*` backing pointers, as returned by
/// [`FillOps::next`] and passed back unchanged to `begin`/`finish`/
/// `enqueue_lowest` — this trait never interprets them, only threads them
/// through, so a test double can hand back whatever identity it likes.
///
/// A generic `fill_once<O: FillOps>` (rather than `dyn FillOps` or free
/// functions) keeps this a zero-cost, monomorphized boundary in the production
/// path while still being fully substitutable in tests.
#[cfg(feature = "async_streaming_loader")]
pub trait FillOps {
    /// Pop the most-urgent queued+still-leased chunk, or a null pointer if the
    /// queue is empty (`deluge_resource_loader_next`).
    fn next(&self) -> *mut c_void;
    /// Whether `chunk` has been marked unloadable since it was enqueued
    /// (`deluge_streaming_chunk_unloadable`) — mirrors `pump()`'s safety-net
    /// skip right after `next()` (`loader.cpp`'s "Safety net" comment): already
    /// dequeued, so skipping can't loop, and it doesn't count against the fill
    /// budget.
    fn is_unloadable(&self, chunk: *mut c_void) -> bool;
    /// Resolve `chunk`'s destination buffer + physical sector range
    /// (`deluge_streaming_begin_fill`). `ok == false` means skip this chunk
    /// entirely (unloadable / geometry error) — no read, no `finish`.
    fn begin(&self, chunk: *mut c_void) -> StreamingFillDescriptor;
    /// Await the read for descriptor `d` into `buf`. Returns whether it
    /// succeeded — routes to the efatfs handle at `d.byte_offset`; `read_at`
    /// handles a bad/zero handle by returning false.
    async fn read(&self, d: &StreamingFillDescriptor, buf: &mut [u8]) -> bool;
    /// Run the post-read convert/stitch/publish tail
    /// (`deluge_streaming_finish_fill`). Only called after a *successful* read
    /// — mirrors `reconstruct_one`'s success arm (`loader.cpp`), which likewise
    /// never reaches its convert/stitch/publish tail on a failed read.
    fn finish(&self, chunk: *mut c_void, read_ok: bool) -> bool;
    /// `chunk`'s current hard-lease count (`deluge_resource_slot_of` +
    /// `deluge_resource_lease_count_by_slot`), consulted only after a failed
    /// read to decide drop-vs-requeue (see `fill_once`).
    fn lease_count(&self, chunk: *mut c_void) -> u32;
    /// Re-enqueue `chunk` at [`LOWEST_PRIORITY`] (`deluge_resource_loader_enqueue`)
    /// — the read failed while the chunk was still wanted.
    fn enqueue_lowest(&self, chunk: *mut c_void);
}

/// Drain the loader queue: for each queued cluster, resolve → await the read →
/// convert/stitch/mark-ready. Mirrors `loader.cpp`'s `pump()`/`reconstruct_one`
/// exactly, just with the read awaited instead of run inline:
/// - the post-`next()` unloadable safety-net skip (`loader.cpp`'s "Safety net"
///   comment) — drop, keep draining, don't count it;
/// - the post-`begin()` `!ok` skip (unloadable / geometry error) — drop, keep
///   draining;
/// - on a **successful** read: run the convert/stitch/publish `finish` tail,
///   then keep draining (`reconstruct_one`'s `true` arm);
/// - on a **failed** read: `finish` is never called (it's the success-only
///   tail — see `reconstruct_one`, which never reaches its convert/stitch/
///   publish body on a failed read either). Instead check the lease count: if
///   it dropped to 0 while loading, the chunk is already unwanted — drop it
///   and keep draining (`reconstruct_one`'s `lease_count(...) == 0` arm).
///   Otherwise a caller still wants it — re-enqueue at lowest priority and
///   stop, else we'd keep re-popping the same cluster until the card is back
///   (`reconstruct_one`'s `false` arm / `loader.cpp:122-127`).
#[cfg(feature = "async_streaming_loader")]
pub async fn fill_once<O: FillOps>(ops: &O) {
    loop {
        let chunk = ops.next();
        if chunk.is_null() {
            return;
        }

        if ops.is_unloadable(chunk) {
            // Safety net: already de-queued by `next()`, so skipping can't
            // loop. Doesn't count against the fill budget.
            continue;
        }

        let d = ops.begin(chunk);
        if !d.ok {
            // Unloadable / geometry error — already dequeued by `next`; skip it,
            // don't loop on it, keep draining the rest of the queue.
            continue;
        }

        // SAFETY: `d.ok` is true, so `dest` is a valid, exclusively-owned
        // destination for exactly `num_sectors * 512` bytes (the production
        // `begin` resolves it from the chunk's own payload buffer; the test
        // double's `begin` hands back its own owned backing storage).
        let buf =
            unsafe { core::slice::from_raw_parts_mut(d.dest, (d.num_sectors as usize) * 512) };
        let read_ok = ops.read(&d, buf).await;

        if read_ok {
            // Success tail: convert/stitch/publish, then keep draining.
            ops.finish(chunk, true);
            continue;
        }

        // Read failed. If the cluster already dropped to 0 leases while
        // loading, it's already unwanted — drop it and keep draining.
        // Otherwise a caller still wants it: re-queue at lowest priority and
        // stop.
        if ops.lease_count(chunk) == 0 {
            continue;
        }
        ops.enqueue_lowest(chunk);
        return;
    }
}

// ── Production wiring ───────────────────────────────────────────────────────
// Only compiled where the real C-ABI symbols below are actually linked: always
// on device, and on host only under `host_app` (which links the host-built C++
// app object closure — see Cargo.toml). Kept separate from `fill_once`/`FillOps`
// above (which compile under `async_streaming_loader` alone) so a plain host
// build of this feature — no `host_app` — never emits a reference to an
// undefined extern symbol. Also requires `async_streaming_loader` itself
// (`fill_once`/`FillOps` are gated on it) — see the module doc's "Two
// compilation tiers".
#[cfg(all(
    feature = "async_streaming_loader",
    any(target_os = "none", feature = "host_app")
))]
mod prod {
    use super::{DelugeChunkConvertState, FillOps, LOWEST_PRIORITY, StreamingFillDescriptor};
    use core::ffi::c_void;

    unsafe extern "C" {
        fn deluge_streaming_resource_manager() -> *mut c_void;
        fn deluge_streaming_chunk_unloadable(chunk_backing: *mut c_void) -> bool;
        // The two StreamedChunk field-touch accessors the native fill uses (SR2d-4 Task 2): payload
        // pointer (read/DMA destination, and the base of the `cluster_size + 7`-byte
        // `payload_with_trailing_slack()` span `finish_convert_stitch` needs) + set-loaded.
        fn deluge_streaming_chunk_payload(chunk_backing: *mut c_void) -> *mut u8;
        fn deluge_streaming_chunk_set_loaded(chunk_backing: *mut c_void);
        // SR2d-4 Task 1 + Task 2: the StreamedChunk convert-state get/set accessors -- `native_finish`
        // below reads/writes self's + each neighbour's convert-state through these instead of the
        // retired `fill_sidecar.rs` table (see this task's commit for the swap).
        fn deluge_streaming_chunk_convert_state(
            chunk_backing: *mut c_void,
        ) -> DelugeChunkConvertState;
        fn deluge_streaming_chunk_set_convert_state(
            chunk_backing: *mut c_void,
            state: DelugeChunkConvertState,
        );
        fn deluge_resource_loader_next(mgr: *mut c_void) -> *mut c_void;
        fn deluge_resource_loader_enqueue(mgr: *mut c_void, slot: u32, priority: u32);
        fn deluge_resource_slot_of(mgr: *mut c_void, ptr: *mut c_void) -> u32;
        fn deluge_resource_lease_count_by_slot(mgr: *mut c_void, slot: u32) -> u32;
        // SR2d-4 Task 5: the native `begin`/`finish` wiring, replacing the
        // `deluge_streaming_begin_fill`/`_finish_fill` upcalls (whose C++ DEFINITIONS stay --
        // `sample_stream.cpp`'s legacy sync-fiber `read_cluster_data` path still calls them; this is
        // just their last Rust caller going away).
        //
        // `deluge_resource_chunk_ident`: recovers a loader-queue chunk's `(asset, index)` identity
        // (out-params; `false` on a miss) so `begin`/`finish` can look up its per-asset fill-context
        // (`fill_context_for`) -- mirrors `deluge_resource_slot_of` just above.
        fn deluge_resource_chunk_ident(
            mgr: *mut c_void,
            ptr: *mut c_void,
            out_asset: *mut u32,
            out_index: *mut u32,
        ) -> bool;
        // `deluge_resource_try_acquire`/`_release`: the neighbour-lease safety mechanism `finish`
        // uses to gather a stitch neighbour (see its doc) -- `try_acquire` is the SAME "resident AND
        // ready" check `prevCluster && prevCluster->loaded` performs in the C++, but atomically also
        // takes a hard lease on a hit (so the neighbour can't be evicted out from under the stitch);
        // `release` drops that lease again once the stitch is done.
        fn deluge_resource_try_acquire(mgr: *mut c_void, asset: u32, index: u32) -> *mut u8;
        fn deluge_resource_release(mgr: *mut c_void, ptr: *mut u8);
        fn deluge_resource_mark_ready(mgr: *mut c_void, ptr: *mut c_void);
    }

    /// The "geometry error / not resolvable" `StreamingFillDescriptor` -- mirrors `begin_fill`'s own
    /// early-return literally (`dest: nullptr, num_sectors: 0, ok: false, handle: 0, byte_offset: 0`,
    /// `async_fill.cpp:89-90`). `ProdOps::begin` returns this whenever a chunk's identity or
    /// fill-context can't be resolved -- should not happen in practice (every chunk on the loader
    /// queue is a resident, still-leased `StreamedChunk` whose asset registered its context at
    /// `ensure_resource_asset()` before it could ever be enqueued -- see the module doc), but failing
    /// closed here is strictly safer than dereferencing a geometry that isn't there. `fill_once`
    /// already treats `!d.ok` as "skip this chunk, don't read, don't call finish" (the same path an
    /// unloadable/geometry-error chunk already takes), so this degrades exactly like that existing,
    /// tested case.
    const fn geometry_error() -> StreamingFillDescriptor {
        StreamingFillDescriptor {
            dest: core::ptr::null_mut(),
            num_sectors: 0,
            ok: false,
            handle: 0,
            byte_offset: 0,
        }
    }

    /// Resolve `chunk`'s `(asset, index)` identity and its asset's registered fill-context, or
    /// `None` if either lookup misses (see [`geometry_error`]'s doc for why that "shouldn't really
    /// still happen" but is handled anyway).
    fn resolve(mgr: *mut c_void, chunk: *mut c_void) -> Option<(u32, u32, super::FillContext)> {
        let mut asset = 0u32;
        let mut index = 0u32;
        // SAFETY: `mgr` is the live singleton resource manager; `chunk` is a still-leased
        // `StreamedChunk*` the caller is already treating as valid for this call.
        if !unsafe { deluge_resource_chunk_ident(mgr, chunk, &mut asset, &mut index) } {
            return None;
        }
        let ctx = super::fill_context_for(asset)?;
        Some((asset, index, ctx))
    }

    /// `DelugeChunkConvertState` (the C-ABI mirror `deluge_streaming_chunk_convert_state`/
    /// `_set_convert_state` cross) -> `fill_logic::ConvertState`: the two are separate types with the
    /// identical three-field shape (see `fill_logic::ConvertState`'s doc for why they aren't the same
    /// type) -- a trivial field-for-field copy at the one tier where both exist. SR2d-4 Task 2: was
    /// `fill_sidecar::ConvertState -> fill_logic::ConvertState` before this task swapped the store.
    fn to_logic_state(s: DelugeChunkConvertState) -> crate::fill_logic::ConvertState {
        crate::fill_logic::ConvertState {
            first_three_bytes: s.first_three_bytes,
            start_converted: s.start_converted,
            end_converted: s.end_converted,
        }
    }

    /// The inverse of [`to_logic_state`], for writing `finish_convert_stitch`'s (possibly updated)
    /// output back onto the `StreamedChunk` via `deluge_streaming_chunk_set_convert_state`.
    fn from_logic_state(s: crate::fill_logic::ConvertState) -> DelugeChunkConvertState {
        DelugeChunkConvertState {
            first_three_bytes: s.first_three_bytes,
            start_converted: s.start_converted,
            end_converted: s.end_converted,
        }
    }

    /// `FillContext` (the C-ABI-mirroring registration record) -> `FillGeometry` (`fill_logic`'s
    /// pure-arithmetic input) -- a plain field subset (drops `efatfs_handle`, which `begin` threads
    /// through separately into the descriptor's own `handle` field, not through the geometry).
    fn to_fill_geometry(ctx: &super::FillContext) -> crate::fill_logic::FillGeometry {
        crate::fill_logic::FillGeometry {
            audio_data_start_pos_bytes: ctx.audio_data_start_pos_bytes,
            audio_data_length_bytes: ctx.audio_data_length_bytes,
            first_cluster_index_with_no_audio_data: ctx.first_cluster_index_with_no_audio_data,
            cluster_size: ctx.cluster_size,
            cluster_size_magnitude: ctx.cluster_size_magnitude,
            raw_data_format: ctx.raw_data_format,
        }
    }

    /// Resolve `chunk_backing`'s destination buffer + physical sector range
    /// (`deluge_streaming_begin_fill`'s native replacement — SR2d-4 Task 5, factored into a free fn
    /// in Task 2). Looks up the live singleton resource manager itself (`deluge_streaming_resource_manager`)
    /// rather than taking `mgr` as a parameter: there is exactly one process-wide manager, and this
    /// shape lets [`ProdOps::begin`] (the async fill task) and the synchronous C++ fill path (Task 3/4,
    /// once this is exposed through a strong C-ABI wrapper) call the SAME function with nothing but the
    /// chunk pointer. `chunk_backing` must be a queued (or, from the sync path, otherwise still-leased),
    /// resident `StreamedChunk*` -- see the module doc's "Sync-context safety" note on [`native_finish`]
    /// below, which applies identically here (this function touches strictly less state: no neighbour
    /// gather, no convert-state read/write).
    fn native_begin(chunk_backing: *mut c_void) -> StreamingFillDescriptor {
        // SAFETY: returns the one process-wide GeneralMemoryAllocator resource manager; a stable
        // singleton pointer, no aliasing/ownership concern.
        let mgr = unsafe { deluge_streaming_resource_manager() };
        let Some((_asset, index, ctx)) = resolve(mgr, chunk_backing) else {
            return geometry_error();
        };
        let geo = to_fill_geometry(&ctx);
        let r = crate::fill_logic::begin(index, &geo);
        if !r.ok {
            return geometry_error();
        }
        // SAFETY: `chunk_backing` is the same resident, leased `StreamedChunk*` `resolve` just
        // validated has a registered fill-context.
        let dest = unsafe { deluge_streaming_chunk_payload(chunk_backing) };
        StreamingFillDescriptor {
            dest,
            num_sectors: r.num_sectors,
            ok: true,
            handle: ctx.efatfs_handle,
            byte_offset: r.byte_offset,
        }
    }

    /// Run the post-read convert/stitch/publish tail for `chunk_backing`
    /// (`deluge_streaming_finish_fill`'s native replacement — SR2d-4 Task 5, factored into a free fn
    /// and moved onto the `StreamedChunk` convert-state accessors in Task 2, off the retired
    /// `fill_sidecar.rs` table). `read_ok` mirrors `finish_fill`'s own early-out contract (see the
    /// body below); only called with `true` from [`fill_once`]'s current calling convention.
    ///
    /// Looks up the live singleton resource manager itself, exactly like [`native_begin`] — see that
    /// function's doc for why (both are meant to be callable from the async task AND, from Task 3/4
    /// on, the synchronous C++ fill path via a shared C-ABI wrapper).
    ///
    /// ## Sync-context safety
    ///
    /// Every operation this function performs beyond plain arithmetic goes through one of three
    /// primitives, and all three are already relied on from a SYNCHRONOUS caller elsewhere in this
    /// codebase, not just this async task:
    /// - [`deluge_resource_try_acquire`]/[`deluge_resource_release`]: the manager's own masked
    ///   critical section (`deluge_resource::sync::Masked`) guards every table mutation these make,
    ///   the same masking the C++ sync-fiber path's own manager calls (`deluge_resource_request`,
    ///   `deluge_resource_release`, etc. — see `sample_stream.cpp`) already go through today. Nothing
    ///   about calling them from a synchronous, non-async context is new.
    /// - [`deluge_streaming_chunk_convert_state`]/[`deluge_streaming_chunk_set_convert_state`]: plain
    ///   field reads/writes on a `StreamedChunk*` (see `async_fill.cpp`'s definitions) — no locking at
    ///   all, by design, exactly like the pre-existing [`deluge_streaming_chunk_payload`]/
    ///   [`deluge_streaming_chunk_set_loaded`] this function already called before this task. Safe
    ///   because the chunk this function touches (`chunk_backing` itself, and each neighbour just
    ///   after its own successful `try_acquire`) is hard-leased for the duration of this call — the
    ///   SAME "leased, so exclusively mine to mutate until I release it" discipline the legacy
    ///   sync-fiber `finish_fill` (`async_fill.cpp`) already relies on when it writes these same
    ///   fields directly. A synchronous caller on the single thread-mode executor (the C++ sync fill
    ///   path never runs on the audio render ISR either — see `streaming_loader`'s module doc) has the
    ///   identical exclusivity guarantee this async task has today.
    /// - [`deluge_resource_mark_ready`]: also masked inside the manager, same as the acquire/release
    ///   pair above.
    ///
    /// In short: nothing this function does depends on running on the Embassy executor specifically —
    /// it depends only on the caller already holding a lease on `chunk_backing` (true for both the
    /// loader-queue chunk this async task pops and the chunk the sync C++ path would already be
    /// holding a lease on to call this at all) and on the manager's existing masked-critical-section
    /// discipline, which is unconditional regardless of caller. No new cross-context hazard was found.
    fn native_finish(chunk_backing: *mut c_void, read_ok: bool) -> bool {
        if !read_ok {
            // Mirrors `finish_fill`'s own `if (!read_ok) { return false; }` early-out
            // (`async_fill.cpp:107-110`) -- dead in practice under `fill_once`'s current
            // calling convention (it only ever calls `finish` after `read` succeeds; see
            // `fill_once`'s doc), kept for parity with the upcall's contract this replaces.
            return false;
        }

        // SAFETY: returns the one process-wide GeneralMemoryAllocator resource manager; a stable
        // singleton pointer, no aliasing/ownership concern.
        let mgr = unsafe { deluge_streaming_resource_manager() };

        // Native `finish` -- convert + stitch + publish. `chunk_backing` is the same pointer `begin`
        // was just called with, whose data has just been successfully read into its payload buffer.
        let Some((asset, index, ctx)) = resolve(mgr, chunk_backing) else {
            return false;
        };
        let geo = to_fill_geometry(&ctx);
        let payload_len = ctx.cluster_size as usize + 7;

        // SAFETY: `chunk_backing` is a resident, still-leased `StreamedChunk*`; its payload buffer is
        // `payload_with_trailing_slack()` -- `cluster_size + 7` bytes, matching `payload_len`.
        let self_payload = unsafe {
            core::slice::from_raw_parts_mut(
                deluge_streaming_chunk_payload(chunk_backing),
                payload_len,
            )
        };
        // SAFETY: `chunk_backing` is a resident, still-leased `StreamedChunk*` (see this function's
        // doc, "Sync-context safety" above).
        let mut self_state =
            to_logic_state(unsafe { deluge_streaming_chunk_convert_state(chunk_backing) });

        // Gather each neighbour, mirroring `async_fill.cpp:116-157`'s "present AND loaded" gate
        // in one step: `try_acquire` reports resident-and-ready (the manager's `Loading` state
        // is exactly the not-yet-`loaded` case the C++ checks) AND atomically takes a hard lease
        // on a hit, so the neighbour can't be evicted out from under the stitch below the way a
        // separate check-then-touch could race against an eviction between the two. The lease is
        // released again right after the stitch (see the bottom of this function) -- it exists
        // purely to pin the neighbour's payload buffer alive for this call's duration, not to
        // hold it any longer than that (mirroring the C++'s use-then-forget borrow of
        // `prevCluster`/`nextCluster`, just made eviction-safe under the manager's real
        // lease/evict machinery, which the synchronous C++ path never had to contend with).
        let mut prev_lease: Option<*mut u8> = None;
        let mut prev_state = crate::fill_logic::ConvertState::default();
        if let Some(prev_index) = index.checked_sub(1) {
            // SAFETY: `mgr` is the live manager; `asset`/`prev_index` are a plain lookup.
            let p = unsafe { deluge_resource_try_acquire(mgr, asset, prev_index) };
            if !p.is_null() {
                // SAFETY: `p` was just leased+validated resident by `try_acquire` above.
                prev_state = to_logic_state(unsafe {
                    deluge_streaming_chunk_convert_state(p as *mut c_void)
                });
                prev_lease = Some(p);
            }
        }
        let prev_view = prev_lease.map(|p| crate::fill_logic::NeighbourView {
            // SAFETY: `p` was just leased+validated resident by `try_acquire` above; its
            // payload is `cluster_size + 7` bytes, matching `payload_len`.
            payload: unsafe { core::slice::from_raw_parts_mut(p, payload_len) },
            unconverted_head: &prev_state.first_three_bytes,
            start_converted: &mut prev_state.start_converted,
            end_converted: &mut prev_state.end_converted,
        });

        let mut next_lease: Option<*mut u8> = None;
        let mut next_state = crate::fill_logic::ConvertState::default();
        if let Some(next_index) = index.checked_add(1) {
            // SAFETY: `mgr` is the live manager; `asset`/`next_index` are a plain lookup.
            let p = unsafe { deluge_resource_try_acquire(mgr, asset, next_index) };
            if !p.is_null() {
                // SAFETY: `p` was just leased+validated resident by `try_acquire` above.
                next_state = to_logic_state(unsafe {
                    deluge_streaming_chunk_convert_state(p as *mut c_void)
                });
                next_lease = Some(p);
            }
        }
        let next_view = next_lease.map(|p| crate::fill_logic::NeighbourView {
            // SAFETY: same as the prev branch above.
            payload: unsafe { core::slice::from_raw_parts_mut(p, payload_len) },
            unconverted_head: &next_state.first_three_bytes,
            start_converted: &mut next_state.start_converted,
            end_converted: &mut next_state.end_converted,
        });

        crate::fill_logic::finish_convert_stitch(
            self_payload,
            index,
            &geo,
            &mut self_state,
            prev_view,
            next_view,
        );

        // Write back the (possibly updated) convert-state -- self, and each neighbour actually
        // visited -- then release the neighbour leases taken above.
        // SAFETY: `chunk_backing` is still the valid, leased `StreamedChunk*` from `next()`.
        unsafe {
            deluge_streaming_chunk_set_convert_state(chunk_backing, from_logic_state(self_state))
        };
        if let Some(p) = prev_lease {
            // SAFETY: `p` was leased by `deluge_resource_try_acquire` above and is still resident.
            unsafe {
                deluge_streaming_chunk_set_convert_state(
                    p as *mut c_void,
                    from_logic_state(prev_state),
                )
            };
            // SAFETY: `p` was leased by `deluge_resource_try_acquire` above; released exactly
            // once, now that the stitch that needed it pinned alive is done.
            unsafe { deluge_resource_release(mgr, p) };
        }
        if let Some(p) = next_lease {
            // SAFETY: same as the prev write-back above.
            unsafe {
                deluge_streaming_chunk_set_convert_state(
                    p as *mut c_void,
                    from_logic_state(next_state),
                )
            };
            // SAFETY: same as the prev release above.
            unsafe { deluge_resource_release(mgr, p) };
        }

        // SAFETY: `chunk_backing` is still the valid, leased `StreamedChunk*` from `next()`.
        unsafe { deluge_streaming_chunk_set_loaded(chunk_backing) };
        // Manager-owned readiness: mirrors `finish_fill`'s own `deluge_resource_mark_ready`
        // call (`async_fill.cpp:169-173`) so the async/RT `try_acquire` path sees this chunk
        // ready.
        // SAFETY: `mgr`/`chunk_backing` are both still valid.
        unsafe { deluge_resource_mark_ready(mgr, chunk_backing) };
        true
    }

    /// The real [`FillOps`], wired to `libdeluge/streaming_fill.h` +
    /// `deluge_resource.h`'s loader-queue C ABI and the efatfs streaming read
    /// (`crate::efatfs_fs`/`crate::efatfs_host_shim`). `!Send`/`!Sync` (a raw
    /// `DelugeResource*`) by construction — must only ever run on the single
    /// thread-mode executor the C++ enqueue path also runs on.
    pub struct ProdOps {
        mgr: *mut c_void,
    }

    impl ProdOps {
        /// Must only be constructed and used on the single thread-mode Embassy
        /// executor the whole app (and the C++ enqueue path) runs on — see the
        /// module doc's Concurrency section.
        pub fn new() -> Self {
            // SAFETY: returns the one process-wide GeneralMemoryAllocator
            // resource manager; no aliasing/ownership concern, it's a stable
            // singleton pointer.
            let mgr = unsafe { deluge_streaming_resource_manager() };

            // No sidecar-overflow guard here anymore (SR2d-4 Task 2): that guard
            // (`deluge_resource_chunk_cap` vs. `fill_sidecar::SIDECAR_CAP`, `FREEZE_WITH_ERROR
            // ("SDC1")`) protected the fixed-capacity `fill_sidecar.rs` table this constructor used
            // to size-check at startup. `native_finish` below no longer reads/writes that table --
            // convert-state now lives directly on each `StreamedChunk` via
            // `deluge_streaming_chunk_convert_state`/`_set_convert_state`, which has no separate
            // capacity to overflow (it's a plain field access on a chunk the caller already holds),
            // so the guard has nothing left to protect. `fill_sidecar.rs` itself is deleted in a
            // later step of this fill-unification; until then it just sits unused.
            Self { mgr }
        }
    }

    impl FillOps for ProdOps {
        fn next(&self) -> *mut c_void {
            // SAFETY: `mgr` is the valid singleton resource manager.
            unsafe { deluge_resource_loader_next(self.mgr) }
        }

        fn is_unloadable(&self, chunk: *mut c_void) -> bool {
            // SAFETY: `chunk` was just returned by `next()`.
            unsafe { deluge_streaming_chunk_unloadable(chunk) }
        }

        fn begin(&self, chunk: *mut c_void) -> StreamingFillDescriptor {
            // `chunk` was just returned by `next()` (a queued, still-leased `StreamedChunk*`).
            native_begin(chunk)
        }

        async fn read(&self, d: &StreamingFillDescriptor, buf: &mut [u8]) -> bool {
            #[cfg(all(target_os = "none", feature = "efatfs_streaming"))]
            {
                return crate::efatfs_fs::read_at(d.handle, d.byte_offset, buf).await;
            }
            #[cfg(all(feature = "host_app", feature = "efatfs_streaming"))]
            {
                return crate::efatfs_host_shim::read_at(d.handle, d.byte_offset, buf).await;
            }
            // No efatfs backing compiled in (should not occur once efatfs_streaming is default): fail the
            // read so the loader re-enqueues rather than silently reading nothing.
            #[cfg(not(feature = "efatfs_streaming"))]
            {
                let _ = (d, buf);
                false
            }
        }

        fn finish(&self, chunk: *mut c_void, read_ok: bool) -> bool {
            // `chunk` is the same pointer `begin` was just called with, whose data has just been
            // successfully read into its payload buffer (when `read_ok`).
            native_finish(chunk, read_ok)
        }

        fn lease_count(&self, chunk: *mut c_void) -> u32 {
            // SAFETY: `mgr`/`chunk` are both still valid (the chunk hasn't been
            // freed — it's still leased, just its read failed).
            let slot = unsafe { deluge_resource_slot_of(self.mgr, chunk) };
            unsafe { deluge_resource_lease_count_by_slot(self.mgr, slot) }
        }

        fn enqueue_lowest(&self, chunk: *mut c_void) {
            // SAFETY: `mgr`/`chunk` are both still valid (the chunk hasn't been
            // freed — it's still leased, just its read failed).
            let slot = unsafe { deluge_resource_slot_of(self.mgr, chunk) };
            unsafe { deluge_resource_loader_enqueue(self.mgr, slot, LOWEST_PRIORITY) };
        }
    }
}

#[cfg(all(
    feature = "async_streaming_loader",
    any(target_os = "none", feature = "host_app")
))]
pub use prod::ProdOps;

/// The fill task: wakes on [`FILL_WAKE`], drains the loader queue via the real
/// [`ProdOps`], then goes back to sleep. Spawned in `main.rs` (device `main` +
/// host `host_app`) under `async_streaming_loader` (R2.1); the C++ enqueue path
/// (`sample_stream.cpp`) wakes it via `deluge_streaming_signal_fill`.
#[cfg(all(
    feature = "async_streaming_loader",
    any(target_os = "none", feature = "host_app")
))]
#[embassy_executor::task]
pub async fn streaming_fill_task() {
    let ops = ProdOps::new();
    loop {
        FILL_WAKE.wait().await;
        fill_once(&ops).await;
    }
}
