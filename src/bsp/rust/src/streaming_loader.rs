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
// see `sample_stream.cpp`); read by the native fill task once a later task wires that read in
// (`fill_context_for` has no caller yet outside tests). See `include/libdeluge/streaming_fill.h`'s
// doc for the C-side contract.

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
/// sample-load and the native fill task will read via [`fill_context_for`], once a later task wires
/// that in. See that header's doc for what each field means; `raw_data_format` mirrors
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
/// `open_read_stream()` at sample-load) and read by [`streaming_fill_task`] once a later task wires
/// that read in. Neither side ever runs on the audio render ISR — asset definition happens at
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
/// of range). Read side of [`deluge_streaming_set_fill_context`]; wired into `ProdOps::begin`/`finish`
/// by a later task (SR2d-4) — nothing calls this yet outside tests.
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
    use super::{FillOps, LOWEST_PRIORITY, StreamingFillDescriptor};
    use core::ffi::c_void;

    unsafe extern "C" {
        fn deluge_streaming_resource_manager() -> *mut c_void;
        fn deluge_streaming_chunk_unloadable(chunk_backing: *mut c_void) -> bool;
        fn deluge_streaming_begin_fill(chunk: *mut c_void) -> StreamingFillDescriptor;
        fn deluge_streaming_finish_fill(chunk: *mut c_void, read_ok: bool) -> bool;
        fn deluge_resource_loader_next(mgr: *mut c_void) -> *mut c_void;
        fn deluge_resource_loader_enqueue(mgr: *mut c_void, slot: u32, priority: u32);
        fn deluge_resource_slot_of(mgr: *mut c_void, ptr: *mut c_void) -> u32;
        fn deluge_resource_lease_count_by_slot(mgr: *mut c_void, slot: u32) -> u32;
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
            // SAFETY: `chunk` was just returned by `next()` (a queued, still-
            // leased `StreamedChunk*`).
            unsafe { deluge_streaming_begin_fill(chunk) }
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
            // SAFETY: `chunk` is the same pointer `begin` was just called with.
            unsafe { deluge_streaming_finish_fill(chunk, read_ok) }
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
