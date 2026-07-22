//! R0b: the host counterpart of `efatfs_fs.rs` — mounts the SAME vendored
//! `embedded-fatfs` over a host block device and reuses the storage-generic
//! `efatfs_core` (`HandleTable` + `open_context`/`read_context`) UNCHANGED, so
//! a host harness measures the *real* read path, not a parallel
//! reimplementation. See `docs/superpowers/specs/2026-07-22-r0-harness-enablement-design.md`
//! §4.1 (the shim) and Appendix A (the mount-strategy spike this file's block
//! device follows exactly).
//!
//! ## The block device: AWAIT `crate::sd::locked_*_sectors`, exactly like the device
//!
//! [`HostSdBlockDevice`] `.await`s the SD_BUS-guarded async helpers
//! `crate::sd::locked_read_sectors`/`locked_write_sectors` — a byte-for-byte
//! mirror of the device `fat_block_device::SdBlockDevice`. It does NOT call the
//! synchronous `deluge_block_read`/`deluge_block_write` C-ABI (an earlier
//! version did, to inherit the boot-time `set_off_fiber_instant` flag — see the
//! history below).
//!
//! ### Why the sync `deluge_block_read` path was WRONG (the Lens 1 deadlock)
//!
//! `deluge_block_read`/`_write` are *synchronous* C-ABI functions: internally
//! they drive the transfer with `block_on_fiber` (on the worker fiber) or
//! `embassy_futures::block_on` (off it). The streaming READ path runs on the
//! async fill task (`streaming_loader::streaming_fill_task`), which is OFF the
//! worker fiber — so a shim read reached that way took the off-fiber
//! `block_on` branch: a NON-yielding busy spin. When a recorder card-write on
//! the fiber had suspended mid-transfer holding `SD_BUS` (`block_on_fiber`
//! yields the fiber but keeps the guard), the fill task's `block_on(SD_BUS
//! .lock())` could never acquire it AND never returned control to the executor
//! — so the write's fiber could never resume to release `SD_BUS`. On Lens 1's
//! single-threaded discrete-event driver that is a hard deadlock inside
//! `executor.poll()` (100% CPU, virtual clock frozen); Lens 2 dodged it only by
//! having a second OS thread. The device `SdBlockDevice` never had the bug
//! because it `.await`s (yields) instead of nesting a `block_on`. This shim now
//! does the same: on the fill task the read genuinely suspends, letting the
//! executor advance the clock, fire the `sim_latency` pump `Timer`, resume the
//! fiber, finish the write, and release `SD_BUS`. Modeled latency now applies
//! to efatfs streaming reads exactly as it does to the C-FatFS path, so Lens 1
//! measures a real margin for this read path.
//!
//! ## Host vs. device differences from `efatfs_fs.rs`
//!
//! - No partition-window detection: the harness's SD images (mtools
//!   `mformat`/`mcopy`, no MBR — see `sd_image.rs`) are always a single FAT
//!   volume spanning the whole raw image, exactly like `fs_differential`'s
//!   `EFatFs::mount()`. The device path needs `partition_window()` because a
//!   real card may carry an MBR; the host image never does.
//! - Same `embassy_sync` `Mutex<CriticalSectionRawMutex, _>` shape as the
//!   device (not a bespoke `RefCell`): `embassy-sync` + `critical-section`
//!   (`std` feature) are already cross-target deps, so reusing the identical
//!   lock discipline here is zero extra risk and keeps this a faithful mirror
//!   of `efatfs_fs.rs` rather than a new design. Host is still logically
//!   single-threaded (one `block_on`/one executor thread), so contention never
//!   actually happens — the `Mutex` is here for shape-fidelity, not for a real
//!   concurrency need.
//! - The FFI bridge ([`deluge_efatfs_open`]/[`deluge_efatfs_close`]) uses the
//!   same `on_fiber()`-gated `block_on_fiber`/`block_on` dispatch as the device
//!   bridge. On host there IS a worker fiber (the C++ app's), and
//!   `SampleStream::open_read_stream` calls these from it — so the on-fiber
//!   `block_on_fiber` (a coroutine yield) is the live path, letting an
//!   open-path SD read pend without livelocking the single-threaded harness.
//!   (An earlier revision assumed no host fiber and always used
//!   `embassy_futures::block_on`; that non-yielding spin is unsafe once the
//!   block device awaits modeled latency — see the block-device section above.)
//!
//! Test infrastructure only (host_app-gated) — no device path touched.
#![cfg(feature = "host_app")]
#![allow(dead_code)]

use aligned::{A4, Aligned};
use block_device_adapters::BufStream;
use block_device_driver::BlockDevice;
use embassy_sync::{blocking_mutex::raw::CriticalSectionRawMutex, mutex::Mutex};
use embedded_fatfs::{DefaultTimeProvider, FileSystem, FsOptions, LossyOemCpConverter};

use crate::efatfs_core::{self, HandleTable};

/// Host `block_device_driver::BlockDevice<512>` that routes every read/write
/// through `crate::sd::deluge_block_read`/`deluge_block_write` — the same
/// C-ABI dispatch FatFS's diskio uses on both host and device. See this
/// module's doc for why this (not a bespoke `RamDisk`/`sd::read_sectors` call)
/// is the load-bearing design choice.
pub struct HostSdBlockDevice;

/// Wraps `deluge_block_read`/`_write`'s raw `DelugeStatus` (`i8`) code, just to
/// satisfy `BlockDevice::Error: Debug` — no richer handling needed on this
/// harness-only path.
#[derive(Debug)]
pub struct HostBlockError(i8);

impl BlockDevice<512> for HostSdBlockDevice {
    type Align = A4;
    type Error = HostBlockError;

    async fn read(
        &mut self,
        block_address: u32,
        data: &mut [Aligned<A4, [u8; 512]>],
    ) -> Result<(), Self::Error> {
        let count = data.len() as u32;
        // AWAIT the SD_BUS-guarded async helper, exactly like the device
        // `fat_block_device::SdBlockDevice::read` — NOT the synchronous
        // `deluge_block_read` C-ABI. See this module's doc for why the sync
        // path deadlocks Lens 1's single-threaded virtual clock.
        //
        // SAFETY: `Aligned<A4, [u8; 512]>` is `#[repr(C)]` over the inner
        // `[u8; 512]` plus a zero-sized alignment marker, so a
        // `[Aligned<A4, [u8; 512]>]` slice is a contiguous run of 512-byte
        // blocks with no inter-element padding; reinterpreting it as
        // `data.len() * 512` flat bytes is sound, and the resulting `&mut [u8]`
        // is validly derived from `data` and does not outlive it. Identical
        // reinterpret to the device `SdBlockDevice`.
        let flat = unsafe {
            core::slice::from_raw_parts_mut(data.as_mut_ptr().cast::<u8>(), data.len() * 512)
        };
        crate::sd::locked_read_sectors(block_address, count, flat)
            .await
            .map_err(|_| HostBlockError(-5))
    }

    async fn write(
        &mut self,
        block_address: u32,
        data: &[Aligned<A4, [u8; 512]>],
    ) -> Result<(), Self::Error> {
        let count = data.len() as u32;
        // See `read` above — AWAIT the SD_BUS-guarded async helper, mirroring
        // the device `SdBlockDevice::write`, not the sync `deluge_block_write`.
        // SAFETY: same layout argument as `read`, read-only direction.
        let flat =
            unsafe { core::slice::from_raw_parts(data.as_ptr().cast::<u8>(), data.len() * 512) };
        crate::sd::locked_write_sectors(block_address, count, flat)
            .await
            .map_err(|_| HostBlockError(-5))
    }

    async fn size(&mut self) -> Result<u64, Self::Error> {
        Ok(u64::from(deluge_bsp::sd::total_sectors()) * 512)
    }
}

type Storage = BufStream<HostSdBlockDevice, 512>;
pub type Fs = FileSystem<Storage, DefaultTimeProvider, LossyOemCpConverter>;

// Same single-owner shape as the device `efatfs_fs.rs` (see its doc) — host is
// logically single-threaded, so contention never actually occurs, but reusing
// the identical `Mutex` shape costs nothing and keeps this a faithful mirror.
static FS: Mutex<CriticalSectionRawMutex, Option<Fs>> = Mutex::new(None);

/// Mount the FS once, storing it in [`FS`]. No partition-window detection (see
/// module doc) — the harness image is always a single raw FAT volume. Called
/// (`.await`ed) from the host_app/lens boot task, a genuine async context: the
/// mount's block reads go through [`HostSdBlockDevice`], which now `.await`s
/// `crate::sd::locked_*_sectors`, so they suspend and resume normally on the
/// executor — no `set_off_fiber_instant` wrap or special-casing needed here.
pub async fn mount() -> Result<(), ()> {
    let storage = BufStream::<HostSdBlockDevice, 512>::new(HostSdBlockDevice);
    let fs = FileSystem::new(storage, FsOptions::new())
        .await
        .map_err(|_| ())?;
    *FS.lock().await = Some(fs);
    Ok(())
}

/// Run `f` with exclusive access to the mounted FS (the ONLY entry point).
/// Returns `None` if [`mount`] hasn't run (or failed) yet. See the device
/// `efatfs_fs::with_fs` for why `f` must be an `AsyncFnOnce`.
pub async fn with_fs<R, F>(f: F) -> Option<R>
where
    F: AsyncFnOnce(&Fs) -> R,
{
    let g = FS.lock().await;
    match g.as_ref() {
        Some(fs) => Some(f(fs).await),
        None => None,
    }
}

// --- File-handle table — composes `efatfs_core`'s split primitives under the
// SAME lock discipline as the device `efatfs_fs.rs` (HANDLES never held across
// a `with_fs` await; see that file's doc for the full rationale). Deliberately
// NOT `HandleTable::read_at_owned` (that convenience holds the table across
// the FS await) — mirroring the device composition keeps this a faithful
// shape-for-shape port, even though host's single executor thread means the
// two orderings are equally deadlock-free here.
static HANDLES: Mutex<CriticalSectionRawMutex, HandleTable> = Mutex::new(HandleTable::new());

/// Open `path`, detach it to a [`embedded_fatfs::FileContext`], and stash it in
/// a free slot. Returns the slot index as the handle, or `None` if the FS is
/// unmounted, the open failed, or the table is full.
pub async fn open(path: &str) -> Option<u32> {
    let ctx = with_fs(async |fs| efatfs_core::open_context(fs, path).await).await??;
    HANDLES.lock().await.insert(ctx)
}

/// Read `dst.len()` bytes from absolute `byte_offset` of the file behind
/// `handle`. Returns `true` iff the full buffer was filled. This is what
/// `ProdOps::read` calls on host (matching the device `efatfs_fs::read_at`
/// signature).
pub async fn read_at(handle: u32, byte_offset: u32, dst: &mut [u8]) -> bool {
    let Some((generation, ctx)) = HANDLES.lock().await.checkout(handle) else {
        return false;
    };

    let result =
        with_fs(async |fs| efatfs_core::read_context(fs, ctx, byte_offset, dst).await).await;

    match result {
        Some(Some((newctx, filled))) => {
            HANDLES.lock().await.commit(handle, generation, newctx);
            filled
        }
        _ => false,
    }
}

/// Close `handle`, freeing its slot. No-op for an out-of-range handle.
pub async fn close(handle: u32) {
    HANDLES.lock().await.remove(handle);
}

// --- FFI bridge --------------------------------------------------------------
//
// Host sibling of `efatfs_fs.rs`'s FFI bridge. No worker fiber exists on
// host, so unlike the device bridge (which requires `crate::fiber::on_fiber()`
// and routes through `block_on_fiber`), this just `embassy_futures::block_on`s
// the async work directly.

use core::ffi::{CStr, c_char};

/// C-ABI: open a sample file for streaming; writes the handle to `*out_handle`.
/// Returns false if the path/pointer is null or invalid, the FS is unmounted,
/// or the open failed.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_open(path: *const c_char, out_handle: *mut u32) -> bool {
    if path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string supplied by the caller,
    // valid for the duration of this call (same contract as the device FFI).
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    // Drive the async open the SAME way the device bridge (`efatfs_fs.rs`) does:
    // when this C-ABI is called from the worker fiber (the real case — C++'s
    // `SampleStream::open_read_stream` runs on the fiber), use `block_on_fiber`,
    // a coroutine YIELD that lets the executor keep running while an SD read
    // pends. A plain `embassy_futures::block_on` here is a non-yielding busy
    // spin: on a single-threaded harness (Lens 1) it would livelock the moment
    // an open-path read needs the executor to advance (pump `Timer` / release
    // `SD_BUS`), exactly the deadlock this module's `read` fix addresses. Off
    // fiber (no coroutine to yield — only pre-fiber/boot contexts), fall back to
    // `embassy_futures::block_on`; there the reads have nothing to contend with.
    let opened = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(open(path))
    } else {
        embassy_futures::block_on(open(path))
    };
    match opened {
        Some(h) => {
            // SAFETY: `out_handle` is non-null (checked above) and points at a
            // `u32` the caller owns for the duration of this synchronous call.
            unsafe {
                *out_handle = h;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: close a streaming file handle. Uses the same `on_fiber`-gated
/// `block_on_fiber`/`block_on` dispatch as [`deluge_efatfs_open`] — though
/// `close` issues no SD reads (it only frees a [`HANDLES`] slot), so neither
/// driver can pend here.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_close(handle: u32) {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(close(handle));
    } else {
        embassy_futures::block_on(close(handle));
    }
}
