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
use embedded_fatfs::{
    DefaultTimeProvider, File, FileContext, FileSystem, FsOptions, LossyOemCpConverter,
};
// embedded-fatfs keeps its own `io` traits `pub(crate)`; `File`'s `Read`/`Seek`
// impls are the public `embedded_io_async` ones, so bring those into scope to
// drive the fill loop / absolute seek below (same trait+version `bench_fs` uses).
use embedded_io_async::{Read as _, Seek as _, SeekFrom};

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

// --- File-handle table (Task 3) -------------------------------------------
//
// A fixed-capacity table of detached [`FileContext`]s keyed by a `u32` handle.
// The streaming read path opens each sample once, then re-attaches a fresh
// [`File`] from its stored context on every `read_at` — `FileContext` is cheap
// to `Clone` and carries `current_cluster`, so forward reads skip re-walking
// the cluster chain from the start. No per-op heap allocation.
//
// Lock discipline: [`HANDLES`] is NEVER held across a [`with_fs`] (FS-mutex)
// await. Every op clones the context OUT of the table, drops the `HANDLES`
// lock, does its FS work under `with_fs`, then re-locks `HANDLES` to write the
// result back. Acquisition is therefore never nested — no lock-order rule to
// remember and no deadlock. `read_at` clones (not `take`s) so a failed read
// leaves the slot valid.
//
// Slot-identity guard: `read_at`'s clone-out/write-back window is NOT atomic
// w.r.t. `open`/`close` — the FS work in between is `.await`ed with `HANDLES`
// released. If `close(handle)` runs during that window and `open` then reuses
// the same (lowest-free) index for a *different* file, a write-back guarded
// only by "slot is occupied" would silently splice the old file's advanced
// `current_cluster`/context onto the new file's slot; `File::new_from_context`
// can't catch this because the old file's directory entry is still valid on
// disk. (The absolute `seek` every call only makes `current_cluster` a perf
// optimization, not a correctness fallback — `first_cluster`/`entry` are file
// *identity*, and those are exactly what would get overwritten.) Each slot
// therefore carries a `generation` counter, bumped on every `open` (claim) and
// `close` (free). `read_at` captures `generation` alongside the cloned context and
// only writes back if `generation` is unchanged when it re-locks — otherwise the
// slot has been recycled underneath it and the write-back is dropped.

/// Max concurrent streamed files. Small fixed cap — the live streaming engine
/// holds only a handful of sample readers open at once.
const MAX_HANDLES: usize = 16;

/// One handle-table slot: an optional detached [`FileContext`] plus a
/// generation counter bumped on every `open`/`close` of that index, so
/// `read_at`'s deferred write-back can detect the slot having been recycled
/// for a different file while its FS work was in flight (see module docs
/// above).
struct Slot {
    generation: u32,
    ctx: Option<FileContext>,
}

/// `FileContext` is plain data (`DirEntryEditor` is `data`/`pos`/`dirty`, no
/// `Rc`/`RefCell`), so it is `Send` and this static compiles.
static HANDLES: Mutex<CriticalSectionRawMutex, [Slot; MAX_HANDLES]> = Mutex::new(
    [const {
        Slot {
            generation: 0,
            ctx: None,
        }
    }; MAX_HANDLES],
);

/// Fill `dst` completely from `f`'s current position. Returns `true` if the
/// whole buffer was filled, `false` on a short read (EOF before `dst` is full).
/// embedded-fatfs has no `read_exact`, so loop the `Read` impl by hand.
async fn fill<'a>(
    f: &mut File<'a, Storage, DefaultTimeProvider, LossyOemCpConverter>,
    dst: &mut [u8],
) -> bool {
    let mut filled = 0;
    while filled < dst.len() {
        match f.read(&mut dst[filled..]).await {
            Ok(0) => return false, // short read / EOF
            Ok(n) => filled += n,
            Err(_) => return false,
        }
    }
    true
}

/// Open `path`, detach it to a [`FileContext`], and stash it in a free slot.
/// Returns the slot index as the handle, or `None` if the open failed or the
/// table is full.
pub async fn open(path: &str) -> Option<u32> {
    // Open + detach under the FS mutex only; never touch HANDLES here.
    let ctx = with_fs(async |fs| {
        let f = fs.root_dir().open_file(path).await.ok()?;
        f.close().await.ok()
    })
    .await??;

    let mut table = HANDLES.lock().await;
    for (i, slot) in table.iter_mut().enumerate() {
        if slot.ctx.is_none() {
            slot.ctx = Some(ctx);
            slot.generation = slot.generation.wrapping_add(1);
            return Some(i as u32);
        }
    }
    // Table full — drop the context (no on-disk state to clean up; the File
    // was already flushed+closed by `close`).
    None
}

/// Read `dst.len()` bytes from absolute `byte_offset` of the file behind
/// `handle`. Returns `true` iff the full buffer was filled. Seeks absolutely
/// every call, so `current_cluster` correctness does not depend on the
/// write-back below (that's only a forward-seek optimization) — but the
/// write-back is still gated on a generation check to guard file *identity*
/// (`first_cluster`/`entry`) against a `close`+`open` slot recycle racing the
/// in-flight FS work; see the module-level comment above [`HANDLES`].
pub async fn read_at(handle: u32, byte_offset: u32, dst: &mut [u8]) -> bool {
    // Clone the context (and its generation) out, then release HANDLES before
    // taking the FS mutex.
    let (ctx, captured_generation) = {
        let table = HANDLES.lock().await;
        match table.get(handle as usize) {
            Some(slot) => match slot.ctx.clone() {
                Some(ctx) => (ctx, slot.generation),
                None => return false,
            },
            None => return false,
        }
    };

    let result = with_fs(async |fs| {
        let mut f = File::new_from_context(ctx, fs).await.ok()?;
        f.seek(SeekFrom::Start(u64::from(byte_offset))).await.ok()?;
        let filled = fill(&mut f, dst).await;
        let newctx = f.close().await.ok()?;
        Some((newctx, filled))
    })
    .await;

    match result {
        Some(Some((newctx, filled))) => {
            // Write the advanced context back (preserves current_cluster for
            // cheap forward seeks) — but ONLY if this slot's generation still
            // matches what we captured. A mismatch means `close` (and
            // possibly a subsequent `open` reusing this index for a
            // different file) ran while the FS work above was in flight;
            // writing back in that case would splice this file's advanced
            // context onto an unrelated file's slot. `is_some()` is kept as
            // a belt-and-suspenders check, but `generation` equality is what
            // actually prevents the identity confusion.
            let mut table = HANDLES.lock().await;
            if let Some(slot) = table.get_mut(handle as usize) {
                if slot.generation == captured_generation && slot.ctx.is_some() {
                    slot.ctx = Some(newctx);
                }
            }
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
    let mut table = HANDLES.lock().await;
    if let Some(slot) = table.get_mut(handle as usize) {
        slot.ctx = None;
        slot.generation = slot.generation.wrapping_add(1);
    }
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
