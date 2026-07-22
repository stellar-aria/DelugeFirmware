//! R0b: the host counterpart of `efatfs_fs.rs` — mounts the SAME vendored
//! `embedded-fatfs` over a host block device and reuses the storage-generic
//! `efatfs_core` (`HandleTable` + `open_context`/`read_context`) UNCHANGED, so
//! a host harness measures the *real* read path, not a parallel
//! reimplementation. See `docs/superpowers/specs/2026-07-22-r0-harness-enablement-design.md`
//! §4.1 (the shim) and Appendix A (the mount-strategy spike this file's block
//! device follows exactly).
//!
//! ## The block device: route through `deluge_block_read`/`deluge_block_write`, not `RamDisk`
//!
//! [`HostSdBlockDevice`] calls the existing `deluge_block_read`/
//! `deluge_block_write` C-ABI functions (`crate::sd`) directly — the SAME
//! dispatch FatFS's diskio uses — instead of reimplementing SD access (e.g.
//! calling `deluge_bsp::sd::read_sectors` or `sim_latency::modeled_read`
//! itself). Per Appendix A's spike: this is what lets the efatfs mount inherit
//! the already-global, boot-time `sd::sim_latency::set_off_fiber_instant` flag
//! (set once in `main.rs`, ahead of `boot_task`/`deluge_app_init`) through the
//! shared `on_fiber()`/`off_fiber_instant()`/else three-way dispatch
//! `deluge_block_read` already implements — [`mount`] below needs no
//! wrap/restore of its own; it just works on the lens/host_app boot path
//! because the flag is already set there by the time `mount()` runs.
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
//! - No `crate::fiber::on_fiber()`/`block_on_fiber` guard on the FFI bridge:
//!   there is no worker fiber on host. [`deluge_efatfs_open`]/
//!   [`deluge_efatfs_close`] just `embassy_futures::block_on` the async work
//!   directly.
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
        // Same dispatch FatFS's diskio uses (see module doc); NOT
        // `deluge_bsp::sd::read_sectors` directly.
        let status =
            crate::sd::deluge_block_read(0, data.as_mut_ptr().cast::<u8>(), block_address, count);
        if status == 0 {
            Ok(())
        } else {
            Err(HostBlockError(status))
        }
    }

    async fn write(
        &mut self,
        block_address: u32,
        data: &[Aligned<A4, [u8; 512]>],
    ) -> Result<(), Self::Error> {
        let count = data.len() as u32;
        let status =
            crate::sd::deluge_block_write(0, data.as_ptr().cast::<u8>(), block_address, count);
        if status == 0 {
            Ok(())
        } else {
            Err(HostBlockError(status))
        }
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
/// module doc) — the harness image is always a single raw FAT volume. Per
/// Appendix A, no `sim_latency::set_off_fiber_instant` wrap is needed here:
/// the caller (the host_app/lens boot path) already sets that flag globally
/// before this runs, and [`HostSdBlockDevice`] inherits it automatically
/// through the shared `deluge_block_read` dispatch.
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
    match embassy_futures::block_on(open(path)) {
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

/// C-ABI: close a streaming file handle.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_close(handle: u32) {
    embassy_futures::block_on(close(handle));
}
