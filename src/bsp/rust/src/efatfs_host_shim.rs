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

use crate::efatfs_core::{self, DirHandleTable, HandleTable, TaskFileTable};

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

/// C-ABI: host sibling of `efatfs_fs::deluge_efatfs_read_at`. Uses the same `on_fiber`-gated
/// `block_on_fiber`/`block_on` dispatch as [`deluge_efatfs_open`] — on-fiber it yields so an SD read
/// can pend without livelocking the single-threaded harness (see this module's `read` doc).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_read_at(
    handle: u32,
    byte_offset: u32,
    dst: *mut u8,
    count: u32,
    out_read: *mut u32,
) -> bool {
    if dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    let filled = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(read_at(handle, byte_offset, buf))
    } else {
        embassy_futures::block_on(read_at(handle, byte_offset, buf))
    };
    if filled {
        // SAFETY: `out_read` is non-null (checked above), a `u32` the caller owns.
        unsafe {
            *out_read = count;
        }
        true
    } else {
        false
    }
}

// --- Task-context file/dir tables (Task 4) ----------------------------------
//
// Host sibling of `efatfs_fs.rs`'s task-context tables/bridge -- same split
// rationale (a separate position-carrying [`TaskFileTable`], distinct from the
// streaming [`HANDLES`]) and the same `on_fiber`-gated `block_on_fiber`/
// `block_on` dispatch every function in this module already uses. See that
// file's doc comments for the design; comments here focus on host-specific
// differences only.

static TASK_FILES: Mutex<CriticalSectionRawMutex, TaskFileTable> = Mutex::new(TaskFileTable::new());
static DIR_HANDLES: Mutex<CriticalSectionRawMutex, DirHandleTable> =
    Mutex::new(DirHandleTable::new());

/// `DelugeFileOpenMode` mode selector matching `file_io.h`'s C enum's implicit
/// declaration-order values: 0 = READ, 1 = WRITE_CREATE, 2 = WRITE_CREATE_NEW.
async fn task_file_open(path: &str, mode: u8) -> Option<u32> {
    let ctx = with_fs(async |fs| match mode {
        0 => efatfs_core::open_context(fs, path).await,
        1 => efatfs_core::create_context(fs, path, false).await,
        2 => efatfs_core::create_context(fs, path, true).await,
        _ => None,
    })
    .await??;
    TASK_FILES.lock().await.insert(ctx)
}

/// Fill-semantics read (zero-pads a short tail at EOF, like [`read_at`]) at the
/// handle's current position, advancing it by `dst.len()` on success.
async fn task_file_read(handle: u32, dst: &mut [u8]) -> bool {
    let Some((generation, ctx, pos)) = TASK_FILES.lock().await.checkout(handle) else {
        return false;
    };
    let result = with_fs(async |fs| efatfs_core::read_context(fs, ctx, pos, dst).await).await;
    match result {
        Some(Some((newctx, filled))) => {
            TASK_FILES
                .lock()
                .await
                .commit(handle, generation, newctx, pos + dst.len() as u32);
            filled
        }
        _ => false,
    }
}

/// EOF-honest read at the handle's current position -- the default for the
/// port's `File::read` (`file.cpp`). Returns the TRUE byte count (may be less
/// than `dst.len()` at EOF, never zero-padded), advancing the position by that
/// count.
async fn task_file_read_exact(handle: u32, dst: &mut [u8]) -> Option<usize> {
    let (generation, ctx, pos) = TASK_FILES.lock().await.checkout(handle)?;
    let (newctx, n) =
        with_fs(async |fs| efatfs_core::read_context_exact(fs, ctx, pos, dst).await).await??;
    TASK_FILES
        .lock()
        .await
        .commit(handle, generation, newctx, pos + n as u32);
    Some(n)
}

/// Write at the handle's current position, advancing it by the bytes actually
/// written.
async fn task_file_write(handle: u32, src: &[u8]) -> Option<usize> {
    let (generation, ctx, pos) = TASK_FILES.lock().await.checkout(handle)?;
    let (newctx, n) =
        with_fs(async |fs| efatfs_core::write_context(fs, ctx, pos, src).await).await??;
    TASK_FILES
        .lock()
        .await
        .commit(handle, generation, newctx, pos + n as u32);
    Some(n)
}

/// Set the handle's cursor position directly. No FS access needed.
async fn task_file_seek(handle: u32, offset: u32) -> bool {
    TASK_FILES.lock().await.seek(handle, offset)
}

/// File length in bytes. Leaves the handle's position untouched.
async fn task_file_size(handle: u32) -> Option<u32> {
    let (generation, ctx, pos) = TASK_FILES.lock().await.checkout(handle)?;
    let (newctx, size) = with_fs(async |fs| efatfs_core::size_context(fs, ctx).await).await??;
    TASK_FILES
        .lock()
        .await
        .commit(handle, generation, newctx, pos);
    Some(size)
}

/// Truncate to `new_len` bytes. Leaves the handle's position untouched (matches
/// POSIX `ftruncate`'s file-offset-unchanged convention -- a subsequent read at
/// a now-past-EOF position simply returns 0 bytes).
async fn task_file_truncate(handle: u32, new_len: u32) -> bool {
    let Some((generation, ctx, pos)) = TASK_FILES.lock().await.checkout(handle) else {
        return false;
    };
    let result = with_fs(async |fs| efatfs_core::truncate_context(fs, ctx, new_len).await).await;
    match result {
        Some(Some((newctx, ()))) => {
            TASK_FILES
                .lock()
                .await
                .commit(handle, generation, newctx, pos);
            true
        }
        _ => false,
    }
}

/// Close a task-context file handle, freeing its slot.
async fn task_file_close(handle: u32) {
    TASK_FILES.lock().await.remove(handle);
}

async fn task_dir_open(path: &str) -> Option<u32> {
    let cursor = with_fs(async |fs| efatfs_core::readdir_open(fs, path).await).await??;
    DIR_HANDLES.lock().await.insert(cursor)
}

/// Outcome of advancing a `DirCursor` (`efatfs_core`) past zero or more
/// too-long-for-the-C-buffer entries to either a fitting entry or
/// end-of-directory. See `efatfs_fs.rs`'s identical type for the rationale.
enum NextFit {
    Eof,
    Found(efatfs_core::DirEntryInfo),
}

/// Advance `handle`'s cursor to the next entry whose name fits in
/// `max_name_bytes`, skipping any that don't. See `efatfs_fs.rs::task_dir_read`
/// for the full rationale (identical here).
async fn task_dir_read(
    handle: u32,
    max_name_bytes: usize,
) -> Option<Option<efatfs_core::DirEntryInfo>> {
    let mut dir_table = DIR_HANDLES.lock().await;
    let cursor = dir_table.get_mut(handle)?;
    // R2 Task 4 review fix: `readdir_next` is FS-free (it only walks the
    // in-memory snapshot `cursor` already holds), so this no longer routes
    // through `with_fs` -- see `efatfs_fs.rs::task_dir_read` for the full
    // rationale (identical here).
    let next = loop {
        match efatfs_core::readdir_next(cursor)? {
            None => break NextFit::Eof,
            Some(info) => {
                if info.name.len() >= max_name_bytes {
                    continue; // doesn't fit the caller's buffer; skip, don't fail the browse
                }
                break NextFit::Found(info);
            }
        }
    };
    Some(match next {
        NextFit::Eof => None,
        NextFit::Found(info) => Some(info),
    })
}

async fn task_dir_close(handle: u32) {
    DIR_HANDLES.lock().await.remove(handle);
}

// --- Task-context FFI bridge (Task 4) ---------------------------------------
//
// Same `on_fiber`-gated `block_on_fiber`/`block_on` dispatch as
// [`deluge_efatfs_open`] above. See `include/libdeluge/file_io.h` for the
// C-side contract of every function below.

/// C-ABI: open a task-context file. `mode` matches `DelugeFileOpenMode`'s
/// declaration-order values (0=READ, 1=WRITE_CREATE, 2=WRITE_CREATE_NEW).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_open(
    path: *const c_char,
    mode: u8,
    out_handle: *mut u32,
) -> bool {
    if path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let opened = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_open(path, mode))
    } else {
        embassy_futures::block_on(task_file_open(path, mode))
    };
    match opened {
        Some(h) => {
            // SAFETY: `out_handle` is non-null (checked above), owned by the caller.
            unsafe {
                *out_handle = h;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: fill-semantics read (see [`task_file_read`]) at the handle's current
/// position; always reports `*out_read = count` on success.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_read(
    handle: u32,
    dst: *mut u8,
    count: u32,
    out_read: *mut u32,
) -> bool {
    if dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    let filled = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_read(handle, buf))
    } else {
        embassy_futures::block_on(task_file_read(handle, buf))
    };
    if filled {
        // SAFETY: `out_read` is non-null (checked above).
        unsafe {
            *out_read = count;
        }
        true
    } else {
        false
    }
}

/// C-ABI: EOF-honest read (see [`task_file_read_exact`]) -- the default for the
/// port's `File::read`. `*out_read` is the true byte count, `<= count`.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_read_exact(
    handle: u32,
    dst: *mut u8,
    count: u32,
    out_read: *mut u32,
) -> bool {
    if dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_read_exact(handle, buf))
    } else {
        embassy_futures::block_on(task_file_read_exact(handle, buf))
    };
    match result {
        Some(n) => {
            // SAFETY: `out_read` is non-null (checked above).
            unsafe {
                *out_read = n as u32;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: write at the handle's current position. `*out_written` is the true
/// byte count written.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_write(
    handle: u32,
    src: *const u8,
    count: u32,
    out_written: *mut u32,
) -> bool {
    if src.is_null() || out_written.is_null() {
        return false;
    }
    // SAFETY: `src` points at `count` readable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts(src, count as usize) };
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_write(handle, buf))
    } else {
        embassy_futures::block_on(task_file_write(handle, buf))
    };
    match result {
        Some(n) => {
            // SAFETY: `out_written` is non-null (checked above).
            unsafe {
                *out_written = n as u32;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: move the handle's cursor to an absolute byte offset.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_seek(handle: u32, offset: u32) -> bool {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_seek(handle, offset))
    } else {
        embassy_futures::block_on(task_file_seek(handle, offset))
    }
}

/// C-ABI: total file size in bytes.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_size(handle: u32, out_size: *mut u32) -> bool {
    if out_size.is_null() {
        return false;
    }
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_size(handle))
    } else {
        embassy_futures::block_on(task_file_size(handle))
    };
    match result {
        Some(sz) => {
            // SAFETY: `out_size` is non-null (checked above).
            unsafe {
                *out_size = sz;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: truncate to `new_len` bytes.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_truncate(handle: u32, new_len: u32) -> bool {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_truncate(handle, new_len))
    } else {
        embassy_futures::block_on(task_file_truncate(handle, new_len))
    }
}

/// C-ABI: close a task-context file handle.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_close(handle: u32) {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_close(handle));
    } else {
        embassy_futures::block_on(task_file_close(handle));
    }
}

/// C-ABI: open `path` as a directory for iteration.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_dir_open(path: *const c_char, out_handle: *mut u32) -> bool {
    if path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let opened = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_dir_open(path))
    } else {
        embassy_futures::block_on(task_dir_open(path))
    };
    match opened {
        Some(h) => {
            // SAFETY: `out_handle` is non-null (checked above), owned by the caller.
            unsafe {
                *out_handle = h;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: read the next directory entry. See `efatfs_fs.rs`'s identical
/// function for the full contract this mirrors.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_dir_read(
    handle: u32,
    out_name: *mut c_char,
    out_name_cap: u32,
    out_is_dir: *mut bool,
    out_size: *mut u32,
    out_modified: *mut u32,
    out_attrs: *mut u8,
    out_has: *mut bool,
) -> bool {
    if out_name.is_null()
        || out_name_cap == 0
        || out_is_dir.is_null()
        || out_size.is_null()
        || out_modified.is_null()
        || out_attrs.is_null()
        || out_has.is_null()
    {
        return false;
    }
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_dir_read(handle, out_name_cap as usize))
    } else {
        embassy_futures::block_on(task_dir_read(handle, out_name_cap as usize))
    };
    match result {
        Some(None) => {
            // SAFETY: `out_has` is non-null (checked above).
            unsafe {
                *out_has = false;
            }
            true
        }
        Some(Some(info)) => {
            let name_bytes = info.name.as_bytes();
            // SAFETY: `task_dir_read` only returns entries with
            // `name.len() < out_name_cap`, so `name_bytes.len() + 1 <= out_name_cap`;
            // every `out_*` pointer is non-null (checked above) and owned by the
            // caller for the duration of this call.
            unsafe {
                let dst =
                    core::slice::from_raw_parts_mut(out_name.cast::<u8>(), out_name_cap as usize);
                dst[..name_bytes.len()].copy_from_slice(name_bytes);
                dst[name_bytes.len()] = 0;
                *out_is_dir = info.is_dir;
                *out_size = info.size;
                *out_modified = info.modified;
                *out_attrs = info.attrs;
                *out_has = true;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: close a directory handle.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_dir_close(handle: u32) {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_dir_close(handle));
    } else {
        embassy_futures::block_on(task_dir_close(handle));
    }
}

/// C-ABI: create a directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_mkdir(path: *const c_char) -> bool {
    if path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let fut = with_fs(async |fs| efatfs_core::mkdir(fs, path).await);
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(fut)
    } else {
        embassy_futures::block_on(fut)
    }
    .flatten()
    .is_some()
}

/// C-ABI: delete a file or empty directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_unlink(path: *const c_char) -> bool {
    if path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let fut = with_fs(async |fs| efatfs_core::unlink(fs, path).await);
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(fut)
    } else {
        embassy_futures::block_on(fut)
    }
    .flatten()
    .is_some()
}

/// C-ABI: rename/move a file or directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_rename(old_path: *const c_char, new_path: *const c_char) -> bool {
    if old_path.is_null() || new_path.is_null() {
        return false;
    }
    let old = match unsafe { CStr::from_ptr(old_path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let new = match unsafe { CStr::from_ptr(new_path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let fut = with_fs(async |fs| efatfs_core::rename(fs, old, new).await);
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(fut)
    } else {
        embassy_futures::block_on(fut)
    }
    .flatten()
    .is_some()
}

/// C-ABI: set a file or directory's last-modified timestamp (decomposed
/// fields, packed here via `efatfs_core::pack_timestamp`).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_set_time(
    path: *const c_char,
    year: u16,
    month: u8,
    day: u8,
    hour: u8,
    minute: u8,
    second: u8,
) -> bool {
    if path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let packed = efatfs_core::pack_timestamp(year, month, day, hour, minute, second);
    let fut = with_fs(async |fs| efatfs_core::set_time(fs, path, packed).await);
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(fut)
    } else {
        embassy_futures::block_on(fut)
    }
    .flatten()
    .is_some()
}

// --- Persistent stream-write handle table (R3 Task 2) -----------------------
//
// Host sibling of `efatfs_fs.rs`'s persistent stream-write table/bridge -- same rationale (a
// single `FileContext` held across an entire recording, advanced via `write_context_noflush` and
// only persisted to the on-disk directory entry on flush/close) and the same `on_fiber`-gated
// `block_on_fiber`/`block_on` dispatch every function in this module already uses. See that
// file's doc comments for the design; comments here focus on host-specific differences only.

static STREAM_WRITE_CTX: Mutex<CriticalSectionRawMutex, HandleTable> =
    Mutex::new(HandleTable::new());

/// `DelugeStreamMode` mode selector matching `stream_io.h`'s C enum's implicit declaration-order
/// values: 0 = READ, 1 = WRITE_CREATE, 2 = WRITE_CREATE_NEW, 3 = WRITE_APPEND.
async fn stream_open(path: &str, mode: u8) -> Option<u32> {
    let ctx = with_fs(async |fs| match mode {
        0 => efatfs_core::open_context(fs, path).await,
        1 => efatfs_core::create_context(fs, path, false).await,
        2 => efatfs_core::create_context(fs, path, true).await,
        3 => efatfs_core::open_context(fs, path).await, // append: open existing, no truncation
        _ => None,
    })
    .await??;
    STREAM_WRITE_CTX.lock().await.insert(ctx)
}

/// Write `src` at absolute `byte_offset`, advancing the handle's persisted context WITHOUT
/// flushing the size/mtime edit to disk (`efatfs_core::write_context_noflush`). Returns the true
/// byte count written, or `None` on a bad handle or FS error.
async fn stream_write_at(handle: u32, byte_offset: u32, src: &[u8]) -> Option<usize> {
    let (generation, ctx) = STREAM_WRITE_CTX.lock().await.checkout(handle)?;
    let (newctx, n) =
        with_fs(async |fs| efatfs_core::write_context_noflush(fs, ctx, byte_offset, src).await)
            .await??;
    STREAM_WRITE_CTX
        .lock()
        .await
        .commit(handle, generation, newctx);
    Some(n)
}

/// EOF-honest read at absolute `byte_offset`, bounded by the handle's IN-MEMORY size
/// (`efatfs_core::read_at_via_context`) -- can read back data written earlier in the same
/// unflushed session. Returns the true byte count read, or `None` on a bad handle or FS error.
async fn stream_read_at_via(handle: u32, byte_offset: u32, dst: &mut [u8]) -> Option<usize> {
    let (generation, ctx) = STREAM_WRITE_CTX.lock().await.checkout(handle)?;
    let (newctx, n) =
        with_fs(async |fs| efatfs_core::read_at_via_context(fs, ctx, byte_offset, dst).await)
            .await??;
    STREAM_WRITE_CTX
        .lock()
        .await
        .commit(handle, generation, newctx);
    Some(n)
}

/// Persist the handle's accumulated in-memory size/mtime edit to the on-disk directory entry
/// (`efatfs_core::flush_context`). Leaves the slot occupied -- callers may keep writing after a
/// flush (e.g. a periodic mid-recording flush).
async fn stream_flush(handle: u32) -> bool {
    let Some((generation, ctx)) = STREAM_WRITE_CTX.lock().await.checkout(handle) else {
        return false;
    };
    let result = with_fs(async |fs| efatfs_core::flush_context(fs, ctx).await).await;
    match result {
        Some(Some(newctx)) => {
            STREAM_WRITE_CTX
                .lock()
                .await
                .commit(handle, generation, newctx);
            true
        }
        _ => false,
    }
}

/// Truncate the file behind `handle` to `new_len` bytes.
async fn stream_truncate(handle: u32, new_len: u32) -> bool {
    let Some((generation, ctx)) = STREAM_WRITE_CTX.lock().await.checkout(handle) else {
        return false;
    };
    let result = with_fs(async |fs| efatfs_core::truncate_context(fs, ctx, new_len).await).await;
    match result {
        Some(Some((newctx, ()))) => {
            STREAM_WRITE_CTX
                .lock()
                .await
                .commit(handle, generation, newctx);
            true
        }
        _ => false,
    }
}

/// File length in bytes.
async fn stream_size(handle: u32) -> Option<u32> {
    let (generation, ctx) = STREAM_WRITE_CTX.lock().await.checkout(handle)?;
    let (newctx, size) = with_fs(async |fs| efatfs_core::size_context(fs, ctx).await).await??;
    STREAM_WRITE_CTX
        .lock()
        .await
        .commit(handle, generation, newctx);
    Some(size)
}

/// R3 Task 3 (TEMPORARY -- retired in Task 6): physical sector backing the handle's
/// most-recently-written cluster (`FileSystem::sector_of_context` -- a Deluge-fork addition on
/// `embedded-fatfs`), validated against `cluster_index`. `None` on a bad handle, an unmounted FS,
/// or a `cluster_index` that doesn't match the most-recently-written cluster. Purely a local
/// `FileContext` read (no FS I/O), so `checkout`'s clone is all this needs -- there is no advanced
/// state to write back, unlike `stream_write_at`/`stream_size`.
async fn stream_sector_of(handle: u32, cluster_index: u32) -> Option<u32> {
    let (_generation, ctx) = STREAM_WRITE_CTX.lock().await.checkout(handle)?;
    with_fs(async |fs| fs.sector_of_context(&ctx, cluster_index)).await?
}

/// Flush (see [`stream_flush`]) then free `handle`'s slot. Returns whether the flush succeeded;
/// the slot is freed either way.
async fn stream_close(handle: u32) -> bool {
    let flushed = stream_flush(handle).await;
    STREAM_WRITE_CTX.lock().await.remove(handle);
    flushed
}

// --- Persistent stream-write FFI bridge (R3 Task 2) -------------------------
//
// Same `on_fiber`-gated `block_on_fiber`/`block_on` dispatch as [`deluge_efatfs_open`] above. See
// `include/libdeluge/stream_io.h` for the C-side contract of every function below.

/// C-ABI: open a persistent stream-write handle. `mode` matches `DelugeStreamMode`'s
/// declaration-order values (0=READ, 1=WRITE_CREATE, 2=WRITE_CREATE_NEW, 3=WRITE_APPEND).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_open(
    path: *const c_char,
    mode: u8,
    out_handle: *mut u32,
) -> bool {
    if path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let opened = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_open(path, mode))
    } else {
        embassy_futures::block_on(stream_open(path, mode))
    };
    match opened {
        Some(h) => {
            // SAFETY: `out_handle` is non-null (checked above), owned by the caller.
            unsafe {
                *out_handle = h;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: write at absolute `byte_offset` (no flush -- see [`stream_write_at`]). `*out_written` is
/// the true byte count written.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_write_at(
    handle: u32,
    byte_offset: u32,
    src: *const u8,
    count: u32,
    out_written: *mut u32,
) -> bool {
    if src.is_null() || out_written.is_null() {
        return false;
    }
    // SAFETY: `src` points at `count` readable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts(src, count as usize) };
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_write_at(handle, byte_offset, buf))
    } else {
        embassy_futures::block_on(stream_write_at(handle, byte_offset, buf))
    };
    match result {
        Some(n) => {
            // SAFETY: `out_written` is non-null (checked above).
            unsafe {
                *out_written = n as u32;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: EOF-honest read at absolute `byte_offset` (see [`stream_read_at_via`]). `*out_read` is
/// the true byte count read, `<= count`.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_read_at_via(
    handle: u32,
    byte_offset: u32,
    dst: *mut u8,
    count: u32,
    out_read: *mut u32,
) -> bool {
    if dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_read_at_via(handle, byte_offset, buf))
    } else {
        embassy_futures::block_on(stream_read_at_via(handle, byte_offset, buf))
    };
    match result {
        Some(n) => {
            // SAFETY: `out_read` is non-null (checked above).
            unsafe {
                *out_read = n as u32;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: persist the handle's accumulated size/mtime edit to disk.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_flush(handle: u32) -> bool {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_flush(handle))
    } else {
        embassy_futures::block_on(stream_flush(handle))
    }
}

/// C-ABI: truncate to `new_len` bytes.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_truncate(handle: u32, new_len: u32) -> bool {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_truncate(handle, new_len))
    } else {
        embassy_futures::block_on(stream_truncate(handle, new_len))
    }
}

/// C-ABI: total size in bytes.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_size(handle: u32, out_size: *mut u32) -> bool {
    if out_size.is_null() {
        return false;
    }
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_size(handle))
    } else {
        embassy_futures::block_on(stream_size(handle))
    };
    match result {
        Some(sz) => {
            // SAFETY: `out_size` is non-null (checked above).
            unsafe {
                *out_size = sz;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: flush and close a persistent stream-write handle, freeing its slot.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_close(handle: u32) -> bool {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_close(handle))
    } else {
        embassy_futures::block_on(stream_close(handle))
    }
}

/// C-ABI (R3 Task 3, TEMPORARY -- retired in Task 6): physical sector backing `handle`'s
/// most-recently-written cluster (`cluster_index`, 0-based). Keeps `SampleRecorder::writeCluster`'s
/// `sdAddress` bookkeeping (and `BlockReadSource`'s consumption of it) valid while the recorder
/// still writes through `deluge::io::Stream` -- see `include/libdeluge/stream_io.h`.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_stream_sector_of(
    handle: u32,
    cluster_index: u32,
    out_sector: *mut u32,
) -> bool {
    if out_sector.is_null() {
        return false;
    }
    let result = if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(stream_sector_of(handle, cluster_index))
    } else {
        embassy_futures::block_on(stream_sector_of(handle, cluster_index))
    };
    match result {
        Some(sector) => {
            // SAFETY: `out_sector` is non-null (checked above).
            unsafe {
                *out_sector = sector;
            }
            true
        }
        None => false,
    }
}
