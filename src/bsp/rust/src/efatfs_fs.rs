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

use crate::efatfs_core::{self, DirHandleTable, HandleTable, LocatorTable, TaskFileTable};
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

// --- Task-context file/dir tables (Task 4) ----------------------------------
//
// Separate from [`HANDLES`] above -- see `efatfs_core::TaskFileTable`'s doc for
// why task-context file I/O (explicit `seek()` + position-implicit `read`/
// `write`, `file_io.h`'s contract) needs its own position-carrying table rather
// than reusing the streaming path's `Slot`. [`DIR_HANDLES`]/[`LOCATORS`] back
// `include/libdeluge/file_io.h`'s directory-enumeration + opaque-reopen C-ABI.

static TASK_FILES: Mutex<CriticalSectionRawMutex, TaskFileTable> = Mutex::new(TaskFileTable::new());
static DIR_HANDLES: Mutex<CriticalSectionRawMutex, DirHandleTable> =
    Mutex::new(DirHandleTable::new());
static LOCATORS: Mutex<CriticalSectionRawMutex, LocatorTable> = Mutex::new(LocatorTable::new());

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

/// One fitting directory entry, plus its locator-table handle
/// ([`efatfs_core::NO_LOCATOR`] for a directory entry, which has none).
struct DirReadResult {
    info: efatfs_core::DirEntryInfo,
    locator: u32,
}

/// Outcome of advancing a [`DirCursor`](efatfs_core) past zero or more
/// too-long-for-the-C-buffer entries (see the module comment above) to either a
/// fitting entry or end-of-directory.
enum NextFit {
    Eof,
    Found(
        efatfs_core::DirEntryInfo,
        Option<efatfs_core::EfatfsLocator>,
    ),
}

/// Advance `handle`'s cursor to the next entry whose name fits in
/// `max_name_bytes` (the C caller's buffer, minus room for the NUL --
/// see `deluge_efatfs_dir_read`), skipping any that don't -- belt-and-suspenders
/// with the `efatfs_core::readdir_open` fix (Task 4) that already keeps ONE
/// oversized name from failing the whole snapshot: this additionally guards the
/// narrower "fits the 256-byte `heapless::String` but not the caller's
/// NUL-terminated buffer" edge (a name of exactly 256 UTF-8 bytes fits the
/// former, not the latter).
///
/// Outer `None`: bad handle or an FS error (`with_fs` returned `None`, or a
/// mid-walk error the `?` inside propagates) -- the C-ABI reports this as
/// `false`. Inner `None`: end of directory -- `deluge_dir_read`'s existing
/// contract ("no more entries" is not an error). `Some(result)`: a fitting
/// entry, its locator already stashed in [`LOCATORS`] if it's a file.
async fn task_dir_read(handle: u32, max_name_bytes: usize) -> Option<Option<DirReadResult>> {
    let mut dir_table = DIR_HANDLES.lock().await;
    let cursor = dir_table.get_mut(handle)?;
    // R2 Task 4 review fix: `readdir_next` is FS-free (it only walks the
    // in-memory snapshot `cursor` already holds), so this no longer routes
    // through `with_fs` -- that used to serialize an in-memory directory-page
    // read behind the single FS mutex, and thus behind any in-flight
    // streaming SD read, for no reason.
    let next = loop {
        match efatfs_core::readdir_next(cursor)? {
            None => break NextFit::Eof,
            Some(info) => {
                if info.name.len() >= max_name_bytes {
                    continue; // doesn't fit the caller's buffer; skip, don't fail the browse
                }
                let locator = efatfs_core::readdir_locator(cursor);
                break NextFit::Found(info, locator);
            }
        }
    };
    drop(dir_table); // release before taking LOCATORS below
    Some(match next {
        NextFit::Eof => None,
        NextFit::Found(info, locator) => {
            let locator_handle = match locator {
                Some(loc) => LOCATORS.lock().await.insert(loc),
                None => efatfs_core::NO_LOCATOR,
            };
            Some(DirReadResult {
                info,
                locator: locator_handle,
            })
        }
    })
}

async fn task_dir_close(handle: u32) {
    DIR_HANDLES.lock().await.remove(handle);
}

/// Open the file behind `locator_handle` (a handle previously returned via
/// [`DirReadResult::locator`] by [`deluge_efatfs_dir_read`]), installing it into
/// [`TASK_FILES`] like [`task_file_open`]. `None` if the locator handle is
/// unknown/stale (see `efatfs_core::LocatorTable::get`) or the underlying file
/// has since been deleted/moved (`efatfs_core::open_by_locator`).
async fn task_open_by_locator(locator_handle: u32) -> Option<u32> {
    let loc = LOCATORS.lock().await.get(locator_handle)?;
    let ctx = with_fs(async |fs| efatfs_core::open_by_locator(fs, loc).await).await??;
    TASK_FILES.lock().await.insert(ctx)
}

// --- Task-context FFI bridge (Task 4) ---------------------------------------
//
// Same `on_fiber()`-gated `block_on_fiber` bridge discipline as the streaming
// bridge above. See `include/libdeluge/file_io.h` for the C-side contract of
// every function below.

/// C-ABI: open a task-context file. `mode` matches `DelugeFileOpenMode`'s
/// declaration-order values (0=READ, 1=WRITE_CREATE, 2=WRITE_CREATE_NEW).
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_open(
    path: *const c_char,
    mode: u8,
    out_handle: *mut u32,
) -> bool {
    if !crate::fiber::on_fiber() || path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    match crate::fiber::block_on_fiber(task_file_open(path, mode)) {
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
    if !crate::fiber::on_fiber() || dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    if crate::fiber::block_on_fiber(task_file_read(handle, buf)) {
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
    if !crate::fiber::on_fiber() || dst.is_null() || out_read.is_null() {
        return false;
    }
    // SAFETY: `dst` points at `count` writable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts_mut(dst, count as usize) };
    match crate::fiber::block_on_fiber(task_file_read_exact(handle, buf)) {
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
    if !crate::fiber::on_fiber() || src.is_null() || out_written.is_null() {
        return false;
    }
    // SAFETY: `src` points at `count` readable bytes owned by the caller for this call.
    let buf = unsafe { core::slice::from_raw_parts(src, count as usize) };
    match crate::fiber::block_on_fiber(task_file_write(handle, buf)) {
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
    if !crate::fiber::on_fiber() {
        return false;
    }
    crate::fiber::block_on_fiber(task_file_seek(handle, offset))
}

/// C-ABI: total file size in bytes.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_size(handle: u32, out_size: *mut u32) -> bool {
    if !crate::fiber::on_fiber() || out_size.is_null() {
        return false;
    }
    match crate::fiber::block_on_fiber(task_file_size(handle)) {
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
    if !crate::fiber::on_fiber() {
        return false;
    }
    crate::fiber::block_on_fiber(task_file_truncate(handle, new_len))
}

/// C-ABI: close a task-context file handle.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_file_close(handle: u32) {
    if crate::fiber::on_fiber() {
        crate::fiber::block_on_fiber(task_file_close(handle));
    }
}

/// C-ABI: open `path` as a directory for iteration.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_dir_open(path: *const c_char, out_handle: *mut u32) -> bool {
    if !crate::fiber::on_fiber() || path.is_null() || out_handle.is_null() {
        return false;
    }
    // SAFETY: `path` is a NUL-terminated C string valid for the duration of this call.
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    match crate::fiber::block_on_fiber(task_dir_open(path)) {
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

/// C-ABI: read the next directory entry into the caller's out-params (see
/// `include/libdeluge/file_io.h`'s `DelugeDirEntry`/`deluge_dir_read` for the
/// shape this mirrors). `out_name` must point at `out_name_cap` writable bytes
/// (`DELUGE_MAX_FILENAME` in practice); an entry whose name doesn't fit is
/// skipped internally (see [`task_dir_read`]), never truncated into `out_name`.
/// `out_locator` receives a locator-table handle usable with
/// `deluge_efatfs_open_by_locator`, or `DELUGE_EFATFS_NO_LOCATOR` for a
/// directory entry.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_dir_read(
    handle: u32,
    out_name: *mut c_char,
    out_name_cap: u32,
    out_is_dir: *mut bool,
    out_size: *mut u32,
    out_modified: *mut u32,
    out_attrs: *mut u8,
    out_locator: *mut u32,
    out_has: *mut bool,
) -> bool {
    if !crate::fiber::on_fiber()
        || out_name.is_null()
        || out_name_cap == 0
        || out_is_dir.is_null()
        || out_size.is_null()
        || out_modified.is_null()
        || out_attrs.is_null()
        || out_locator.is_null()
        || out_has.is_null()
    {
        return false;
    }
    match crate::fiber::block_on_fiber(task_dir_read(handle, out_name_cap as usize)) {
        Some(None) => {
            // SAFETY: `out_has` is non-null (checked above).
            unsafe {
                *out_has = false;
            }
            true
        }
        Some(Some(result)) => {
            let name_bytes = result.info.name.as_bytes();
            // SAFETY: `task_dir_read` only returns entries with
            // `name.len() < out_name_cap`, so `name_bytes.len() + 1 <= out_name_cap`;
            // every `out_*` pointer is non-null (checked above) and owned by the
            // caller for the duration of this call.
            unsafe {
                let dst =
                    core::slice::from_raw_parts_mut(out_name.cast::<u8>(), out_name_cap as usize);
                dst[..name_bytes.len()].copy_from_slice(name_bytes);
                dst[name_bytes.len()] = 0;
                *out_is_dir = result.info.is_dir;
                *out_size = result.info.size;
                *out_modified = result.info.modified;
                *out_attrs = result.info.attrs;
                *out_locator = result.locator;
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
    }
}

/// C-ABI: create a directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_mkdir(path: *const c_char) -> bool {
    if !crate::fiber::on_fiber() || path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    crate::fiber::block_on_fiber(with_fs(async |fs| efatfs_core::mkdir(fs, path).await))
        .flatten()
        .is_some()
}

/// C-ABI: delete a file or empty directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_unlink(path: *const c_char) -> bool {
    if !crate::fiber::on_fiber() || path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    crate::fiber::block_on_fiber(with_fs(async |fs| efatfs_core::unlink(fs, path).await))
        .flatten()
        .is_some()
}

/// C-ABI: rename/move a file or directory.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_rename(old_path: *const c_char, new_path: *const c_char) -> bool {
    if !crate::fiber::on_fiber() || old_path.is_null() || new_path.is_null() {
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
    crate::fiber::block_on_fiber(with_fs(async |fs| efatfs_core::rename(fs, old, new).await))
        .flatten()
        .is_some()
}

/// C-ABI: set a file or directory's last-modified timestamp (decomposed
/// fields, packed here via `efatfs_core::pack_timestamp` before reaching the
/// FAT DOS date/time `set_time` primitive).
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
    if !crate::fiber::on_fiber() || path.is_null() {
        return false;
    }
    let path = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(p) => p,
        Err(_) => return false,
    };
    let packed = efatfs_core::pack_timestamp(year, month, day, hour, minute, second);
    crate::fiber::block_on_fiber(with_fs(async |fs| {
        efatfs_core::set_time(fs, path, packed).await
    }))
    .flatten()
    .is_some()
}

/// C-ABI: open the file behind `locator_handle` (see [`task_open_by_locator`]),
/// writing the resulting task-context file handle to `*out_file_handle`.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_efatfs_open_by_locator(
    locator_handle: u32,
    out_file_handle: *mut u32,
) -> bool {
    if !crate::fiber::on_fiber() || out_file_handle.is_null() {
        return false;
    }
    match crate::fiber::block_on_fiber(task_open_by_locator(locator_handle)) {
        Some(h) => {
            // SAFETY: `out_file_handle` is non-null (checked above).
            unsafe {
                *out_file_handle = h;
            }
            true
        }
        None => false,
    }
}
