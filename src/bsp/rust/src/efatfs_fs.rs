//! SP1a Task 2: the single-owner mounted `embedded-fatfs` `FileSystem` —
//! mounts ONE `FileSystem` over the real SD card (`SdBlockDevice`, see
//! `fat_block_device.rs`) and stores it behind an `embassy_sync` async
//! `Mutex`. [`with_fs`] is the ONLY way live code may touch the FS.
//!
//! embedded-fatfs's `FileSystem` wraps its disk in a `RefCell`, which assumes
//! *exclusive* (non-reentrant, non-concurrent) access — two fibers/tasks
//! calling into it at once would panic the `RefCell` (or worse, race the
//! underlying SD transfer). An async `Mutex` (not the `RefCell` itself, and
//! not a blocking lock) serializes all FS operations here while letting a
//! holder `.await` (yield the executor) across the underlying SD transfer,
//! rather than busy-spinning or blocking other tasks outright.
//!
//! Device-only and flag-gated: nothing in this module is called yet. The
//! file-handle table (Task 3), the FFI boundary (Task 4), and the read-path
//! swap (Task 5/6) are what actually invoke [`mount`]/[`with_fs`] — until
//! then this module is dead code by design, hence the blanket
//! `#![allow(dead_code)]` below (keeps the `efatfs_streaming` build
//! warning-clean without disturbing the default build, which doesn't compile
//! this file at all).
#![cfg(all(target_os = "none", feature = "efatfs_streaming"))]
#![allow(dead_code)]

use aligned::{A4, Aligned};
use block_device_adapters::{BufStream, StreamSlice};
use block_device_driver::BlockDevice;
use embassy_sync::{blocking_mutex::raw::CriticalSectionRawMutex, mutex::Mutex};
use embedded_fatfs::{DefaultTimeProvider, FileSystem, FsOptions, LossyOemCpConverter};

use crate::efatfs_core::{self, HandleTable};
use crate::fat_block_device::SdBlockDevice;

type Storage = StreamSlice<BufStream<SdBlockDevice, 512>>;
pub type Fs = FileSystem<Storage, DefaultTimeProvider, LossyOemCpConverter>;

// Single-owner: embedded-fatfs's RefCell disk assumes exclusive access, so ALL
// FS ops serialize here. Async mutex (not the RefCell) — holders yield on SD.
static FS: Mutex<CriticalSectionRawMutex, Option<Fs>> = Mutex::new(None);

/// Detect MBR partition (real cards) vs superfloppy, return the byte window.
/// Same logic proven on-device in `bench_fs.rs`: sector 0 is either a FAT VBR
/// (partitionless "superfloppy" — jump `EB`/`E9`) or an MBR whose partition-0
/// entry (type @ +4, start_lba @ +8, sector-count @ +12 from offset 446) gives
/// the real FAT volume's byte window. On any sector-0 read error, fall back
/// to the whole-device window rather than panicking — `mount` below still
/// surfaces the eventual `FileSystem::new` failure as `Err(())`.
async fn partition_window() -> (u64, u64) {
    let mut s0: [Aligned<A4, [u8; 512]>; 1] = [Aligned([0u8; 512])];
    if SdBlockDevice.read(0, &mut s0).await.is_err() {
        return (0, SdBlockDevice.size().await.unwrap_or(u64::MAX));
    }
    let s = &s0[0][..];
    let sig = u16::from_le_bytes([s[510], s[511]]);
    let is_fat_vbr = s[0] == 0xEB || s[0] == 0xE9;
    let plba = u32::from_le_bytes([s[454], s[455], s[456], s[457]]);
    let nsec = u32::from_le_bytes([s[458], s[459], s[460], s[461]]);
    if !is_fat_vbr && sig == 0xAA55 && plba != 0 {
        let start = plba as u64 * 512;
        (start, start + nsec as u64 * 512)
    } else {
        (0, SdBlockDevice.size().await.unwrap_or(u64::MAX))
    }
}

/// Mount the FS once, storing it in [`FS`]. Idempotent-unsafe by design (no
/// caller yet — later tasks are responsible for calling this exactly once at
/// boot, after the SD block driver is up).
pub async fn mount() -> Result<(), ()> {
    let (start, end) = partition_window().await;
    let slice = StreamSlice::new(
        BufStream::<SdBlockDevice, 512>::new(SdBlockDevice),
        start,
        end,
    )
    .await
    .map_err(|_| ())?;
    let fs = FileSystem::new(slice, FsOptions::new())
        .await
        .map_err(|_| ())?;
    *FS.lock().await = Some(fs);
    Ok(())
}

/// Run `f` with exclusive access to the mounted FS (the ONLY entry point).
/// Returns `None` if [`mount`] hasn't run (or failed) yet.
///
/// `f` is an `AsyncFnOnce` (not `FnOnce(&Fs) -> impl Future`): only the async-
/// closure form ties the returned future's lifetime to the borrowed `&Fs`, so
/// the body may `.await` while holding that borrow.
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

// --- File-handle table (Task 3; logic extracted to `efatfs_core` in Task 7a) --
//
// The table type, the generation guard, the `FileContext` detach/reattach, and
// the fill loop all live in the storage-generic, host-testable [`efatfs_core`]
// module. This device layer owns one [`HandleTable`] behind an async `Mutex`
// and composes the core's split primitives under the lock discipline below.
//
// Lock discipline: [`HANDLES`] is NEVER held across a [`with_fs`] (FS-mutex)
// await. `read_at` `checkout`s the context OUT of the table, drops the
// `HANDLES` lock, does its FS work under `with_fs`, then re-locks `HANDLES` to
// `commit` the result back (gated on the slot's generation, so a `close`+`open`
// recycle racing the in-flight read can't splice a stale context onto a
// different file). Acquisition is therefore never nested — `open` takes
// FS→HANDLES, `read_at` takes HANDLES→FS-then-HANDLES with the FS work OUTSIDE
// the HANDLES lock, so the two never hold both at once and can't deadlock.
// (This is why the device path must not use `HandleTable::read_at_owned`, which
// holds the table across the FS await — see its doc.)

/// `FileContext` is plain data (`DirEntryEditor` is `data`/`pos`/`dirty`, no
/// `Rc`/`RefCell`), so [`HandleTable`] is `Send` and this static compiles.
static HANDLES: Mutex<CriticalSectionRawMutex, HandleTable> = Mutex::new(HandleTable::new());

/// Open `path`, detach it to a [`FileContext`], and stash it in a free slot.
/// Returns the slot index as the handle, or `None` if the FS is unmounted, the
/// open failed, or the table is full.
pub async fn open(path: &str) -> Option<u32> {
    // Open + detach under the FS mutex only; never touch HANDLES here (keeps the
    // FS→HANDLES order that pairs deadlock-free with read_at's HANDLES→FS).
    let ctx = with_fs(async |fs| efatfs_core::open_context(fs, path).await).await??;
    HANDLES.lock().await.insert(ctx)
}

/// Read `dst.len()` bytes from absolute `byte_offset` of the file behind
/// `handle`. Returns `true` iff the full buffer was filled. Composes the core's
/// split primitives under the lock discipline: `checkout` (release HANDLES) →
/// `read_context` under `with_fs` → `commit` (re-lock HANDLES, generation-gated).
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
        // FS not mounted, or reattach/seek/read failed — slot left untouched.
        _ => false,
    }
}

/// Close `handle`, freeing its slot. No-op for an out-of-range handle. Bumps
/// the slot's generation so any `read_at` write-back already in flight for
/// this handle (captured before this `close`) is detected as stale even if no
/// subsequent `open` reuses the index.
pub async fn close(handle: u32) {
    HANDLES.lock().await.remove(handle);
}

// --- FFI bridge (Task 4) ---------------------------------------------------
//
// C++ calls these synchronously at sample-load (`open_read_stream`, Task 6), but
// [`open`]/[`close`] above are async. Bridge via `crate::fiber::block_on_fiber`
// — the fiber-aware `block_on` that polls the future on the worker fiber and
// yields the executor (not the whole app) across the SD transfer. Valid ONLY
// while `crate::fiber::on_fiber()`; off-fiber the bridge can't run, so open
// fails (caller falls back to the C-FatFS sector path) and close is skipped.
use core::ffi::{CStr, c_char};

/// C-ABI: open a sample file for streaming; writes the handle to `*out_handle`.
/// Returns false (caller falls back to the C-FatFS map) if not on the worker
/// fiber, the path/pointer is null or invalid, the FS is unmounted, or the open
/// failed. See `include/libdeluge/streaming_fill.h` for the C-side contract.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_open(path: *const c_char, out_handle: *mut u32) -> bool {
    if !crate::fiber::on_fiber() || path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string supplied by open_read_stream (Task 6),
    // valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    match crate::fiber::block_on_fiber(open(path)) {
        Some(h) => {
            // SAFETY: `out_handle` is non-null (checked above) and points at a `u32` the C++
            // caller owns for the duration of this synchronous call.
            unsafe {
                *out_handle = h;
            }
            true
        }
        None => false,
    }
}

/// C-ABI: close a streaming file handle. Bridges to the async [`close`] via
/// `block_on_fiber` only while on the worker fiber.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_close(handle: u32) {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(close(handle));
    }
    // An off-fiber close can't bridge to the async table, so the slot leaks until reuse —
    // acceptable for SP1a (flag-gated, few handles). A deferred-close queue is future work.
}

/// C-ABI: synchronously read `count` bytes at `byte_offset` of the file behind `handle`, bridging
/// the async [`read_at`] through `block_on_fiber` — valid only on the worker fiber. Writes
/// `*out_read = count` and returns true iff the full buffer was filled. See
/// `include/libdeluge/streaming_fill.h` for the contract.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_read_at(
    handle: u32,
    byte_offset: u32,
    dst: *mut u8,
    count: u32,
    out_read: *mut u32,
) -> bool {
    if !crate::fiber::on_fiber() || dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the C++ caller for the duration of
    // this synchronous call (the cluster payload buffer in read_cluster_data).
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    if crate::fiber::block_on_fiber(read_at(handle, byte_offset, buf)) {
        // SAFETY: `out_read` is non-null (checked above), a `u32` the caller owns.
        unsafe {
            *out_read = count;
        }
        true
    } else {
        false
    }
}
