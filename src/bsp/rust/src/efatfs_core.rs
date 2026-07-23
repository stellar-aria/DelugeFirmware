//! Storage-generic, host-testable core of the embedded-fatfs streaming read
//! path (SP1a Task 7a). This module holds the load-bearing logic — the
//! file-handle table, the `FileContext` detach/reattach, the generation guard,
//! and the fill loop — with NO device dependencies: it is generic over the
//! embedded-fatfs `FileSystem`'s IO type, uses no `static`s, no fibers, and no
//! `SdBlockDevice`, so a host test (and the `lens1_vt_sim` margin harness) can
//! drive the exact same code over a host/sim block device.
//!
//! `efatfs_fs.rs` (device-only) wraps this: it owns the mounted `FileSystem`
//! and the `HandleTable` behind two async `Mutex`es and composes the SPLIT
//! primitives below ([`HandleTable::checkout`] / [`read_context`] /
//! [`HandleTable::commit`]) under its lock discipline — HANDLES is cloned-out
//! and released BEFORE the FS mutex is taken, then re-taken to write back.
//! Holding the table borrow across the FS await (as a monolithic `&mut self`
//! op would) is what [`HandleTable::read_at_owned`] does for single-threaded
//! host/sim callers; the device path must NOT use it, or it would invert the
//! FS→HANDLES order `open` takes and deadlock.
//!
//! Handle model: the streaming read path opens each sample once, detaches it to
//! a [`FileContext`] (cheap to `Clone`, carries `current_cluster` so forward
//! reads skip re-walking the cluster chain), and re-attaches a fresh `File`
//! from that context on every read. No per-op heap allocation.

#![allow(dead_code)]

// R2 Task 3: `readdir_open`'s snapshot `Vec` needs `alloc` -- this module
// compiles both `no_std` (device) and `std` (host_app / fs_differential's
// `#[path]` include), and `extern crate` visibility is per-module in Rust
// 2018+, so this can't rely on another module's `extern crate alloc;`
// (`bench_fs.rs` declares its own for the same reason). Harmless under a
// `std` build too -- `alloc` is always in the sysroot there.
extern crate alloc;
use alloc::vec::Vec;

use embedded_fatfs::{
    Date, DateTime, File, FileContext, FileSystem, OemCpConverter, ReadWriteSeek, Time,
    TimeProvider,
};
// embedded-fatfs keeps its own `io` traits `pub(crate)`; `File`'s `Read`/`Seek`
// impls are the public `embedded_io_async` ones, so bring those into scope to
// drive the fill loop / absolute seek below.
use embedded_io_async::{Read as _, Seek as _, SeekFrom, Write as _};

/// Max concurrent streamed files. Small fixed cap — the live streaming engine
/// holds only a handful of sample readers open at once.
pub const MAX_HANDLES: usize = 16;

/// One handle-table slot: an optional detached [`FileContext`] plus a
/// generation counter bumped on every claim (`insert`) and free (`remove`) of
/// that index, so a deferred write-back can detect the slot having been
/// recycled for a different file while its FS work was in flight.
struct Slot {
    generation: u32,
    ctx: Option<FileContext>,
}

/// A fixed-capacity table of detached [`FileContext`]s keyed by a `u32` handle.
///
/// The device layer keeps one of these behind an async `Mutex`; host/sim
/// callers own one directly. All methods are FS-agnostic except the two that
/// take a `&FileSystem` ([`read_at_owned`] and, indirectly, the free functions
/// [`open_context`] / [`read_context`] below).
pub struct HandleTable {
    slots: [Slot; MAX_HANDLES],
}

impl Default for HandleTable {
    fn default() -> Self {
        Self::new()
    }
}

impl HandleTable {
    pub const fn new() -> Self {
        Self {
            slots: [const {
                Slot {
                    generation: 0,
                    ctx: None,
                }
            }; MAX_HANDLES],
        }
    }

    /// Stash `ctx` in the lowest free slot, bumping that slot's generation.
    /// Returns the slot index as the handle, or `None` if the table is full.
    pub fn insert(&mut self, ctx: FileContext) -> Option<u32> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.ctx.is_none() {
                slot.ctx = Some(ctx);
                slot.generation = slot.generation.wrapping_add(1);
                return Some(i as u32);
            }
        }
        None
    }

    /// Clone the [`FileContext`] behind `handle` out of the table, together with
    /// the slot's current generation. `None` if the handle is out of range or
    /// the slot is free. Clones (not takes) so a failed read leaves the slot
    /// valid.
    pub fn checkout(&self, handle: u32) -> Option<(u32, FileContext)> {
        let slot = self.slots.get(handle as usize)?;
        let ctx = slot.ctx.clone()?;
        Some((slot.generation, ctx))
    }

    /// Write `newctx` back into `handle`'s slot ONLY if the slot is still
    /// occupied AND its generation still matches `captured_generation`. A
    /// mismatch means `remove` (and possibly a later `insert` reusing this
    /// index for a *different* file) ran while the FS work was in flight;
    /// writing back then would splice this file's advanced context onto an
    /// unrelated file's slot, and `File::new_from_context`'s validation can't
    /// catch it (the old file's directory entry is still valid on disk). The
    /// `is_some()` check is belt-and-suspenders; the generation equality is
    /// what actually prevents the identity confusion.
    pub fn commit(&mut self, handle: u32, captured_generation: u32, newctx: FileContext) {
        if let Some(slot) = self.slots.get_mut(handle as usize) {
            if slot.generation == captured_generation && slot.ctx.is_some() {
                slot.ctx = Some(newctx);
            }
        }
    }

    /// Free `handle`'s slot. No-op for an out-of-range handle. Bumps the slot's
    /// generation so any deferred write-back captured before this `remove` is
    /// detected as stale even if no subsequent `insert` reuses the index.
    pub fn remove(&mut self, handle: u32) {
        if let Some(slot) = self.slots.get_mut(handle as usize) {
            slot.ctx = None;
            slot.generation = slot.generation.wrapping_add(1);
        }
    }

    /// Single-threaded convenience: checkout → [`read_context`] → commit under
    /// one exclusive `&mut self` borrow. For host tests and the `lens1_vt_sim`
    /// margin harness ONLY — the device path must compose the split primitives
    /// under its two mutexes instead (see the module doc's lock-discipline
    /// note), never this, or it would hold the table across the FS await and
    /// deadlock against `open`'s FS→table order.
    pub async fn read_at_owned<IO, TP, OCC>(
        &mut self,
        fs: &FileSystem<IO, TP, OCC>,
        handle: u32,
        byte_offset: u32,
        dst: &mut [u8],
    ) -> bool
    where
        IO: ReadWriteSeek,
        TP: TimeProvider,
        OCC: OemCpConverter,
    {
        let Some((generation, ctx)) = self.checkout(handle) else {
            return false;
        };
        let Some((newctx, filled)) = read_context(fs, ctx, byte_offset, dst).await else {
            return false;
        };
        self.commit(handle, generation, newctx);
        filled
    }
}

/// Fill `dst` completely from `f`'s current position. Returns `true` if the
/// whole buffer was filled (either by real reads, or by zero-padding a tail
/// that ran past logical EOF — see below), `false` only on an actual read
/// error. embedded-fatfs has no `read_exact`, so loop `Read` by hand.
///
/// C2 fix: the streaming loader's last-cluster read is sector-rounded
/// (`begin_fill` in `async_fill.cpp` computes
/// `numSectors = ceil((audioDataEnd - clusterStart) / 512)`), and
/// `audioDataEnd` is frequently NOT sector-aligned (e.g. a WAV whose `data`
/// chunk ends the file). That legitimately asks for up to ~511 bytes past the
/// file's logical size — but never past the last cluster's on-disk
/// allocation, which FAT always pads up to the cluster boundary. The retired
/// raw-sector C-FatFS reader tolerated this by construction (it read whole
/// allocated sectors, unbounded by file_size); `File::read` is a LOGICAL-file
/// reader and returns `Ok(0)` at EOF instead. Rather than fail the whole
/// fill, treat EOF as "the rest is unused cluster padding" and zero it —
/// `dst[filled..]` is deterministic and the convert/stitch pipeline never
/// consumes past the real audio-data length anyway.
///
/// CALLER CONTRACT: this makes EOF non-distinguishable from a valid short tail —
/// a read that begins at/beyond EOF (`filled == 0` on the first `Ok(0)`) also
/// returns `true` with an all-zero buffer, NOT an error. Callers must therefore
/// bound the request to within the file (at most ~1 cluster past logical EOF, as
/// `begin_fill` does via `audioDataLengthBytes` clamped to the real on-disk file
/// size in `Sample::finalizeAfterLoad`). A caller that lets `byte_offset` land
/// fully past EOF would silently read zeros as if valid data. Today the only
/// production caller is the streaming `begin_fill` path, which enforces this.
async fn fill<IO, TP, OCC>(f: &mut File<'_, IO, TP, OCC>, dst: &mut [u8]) -> bool
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
{
    let mut filled = 0;
    while filled < dst.len() {
        match f.read(&mut dst[filled..]).await {
            Ok(0) => {
                // EOF before dst is full: zero-pad the rest and report success.
                dst[filled..].fill(0);
                return true;
            }
            Ok(n) => filled += n,
            Err(_) => return false,
        }
    }
    true
}

/// Open `path` over `fs` and detach it to a [`FileContext`] (open → `close`).
/// `None` if the open failed.
pub async fn open_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    path: &str,
) -> Option<FileContext>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let f = fs.root_dir().open_file(path).await.ok()?;
    f.close().await.ok()
}

/// Re-attach a `File` from `ctx`, seek to absolute `byte_offset`, fill `dst`,
/// and detach again. Returns the advanced [`FileContext`] plus whether the
/// whole buffer was filled; `None` on any FS error (reattach/seek/read/close).
/// Seeks absolutely every call, so `current_cluster` correctness does not depend
/// on reusing the returned context — reusing it only makes forward seeks cheap.
pub async fn read_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    ctx: FileContext,
    byte_offset: u32,
    dst: &mut [u8],
) -> Option<(FileContext, bool)>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let mut f = File::new_from_context(ctx, fs).await.ok()?;
    f.seek(SeekFrom::Start(u64::from(byte_offset))).await.ok()?;
    let filled = fill(&mut f, dst).await;
    let newctx = f.close().await.ok()?;
    Some((newctx, filled))
}

/// R2 Task 1: EOF-honest counterpart of [`read_context`]/[`fill`]. Loops
/// `File::read` the same way, but on the FIRST `Ok(0)` (real EOF, or a
/// request that started at/beyond it) stops and returns the true accumulated
/// byte count in `0..=dst.len()` — it does NOT zero-pad the remainder of
/// `dst` the way `fill` does for the streaming-read path's last-cluster
/// tolerance. Task-context callers (the write path's read-modify-write users,
/// size probes, etc.) need to know the real file length, not a padded one.
/// `None` only on an actual FS error (reattach/seek/read/close), same as
/// `read_context`.
pub async fn read_context_exact<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    ctx: FileContext,
    byte_offset: u32,
    dst: &mut [u8],
) -> Option<(FileContext, usize)>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let mut f = File::new_from_context(ctx, fs).await.ok()?;
    f.seek(SeekFrom::Start(u64::from(byte_offset))).await.ok()?;
    let mut filled = 0;
    while filled < dst.len() {
        match f.read(&mut dst[filled..]).await {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(_) => return None,
        }
    }
    let newctx = f.close().await.ok()?;
    Some((newctx, filled))
}

/// Re-attach a `File` from `ctx`, seek to absolute `byte_offset`, write all of
/// `src` (looping `File::write` `write_all`-style since embedded-fatfs's own
/// `write_all` returns `()` rather than a byte count), and detach again.
/// Mirrors `read_context`'s detach/reattach/seek discipline. `close` (via
/// `File`'s `flush`) is what persists the advanced size/mtime dir-entry edit
/// to disk — `File::write` alone only updates the in-memory context (see
/// `file.rs`'s `update_dir_entry_after_write`). `None` on any FS error.
pub async fn write_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    ctx: FileContext,
    byte_offset: u32,
    src: &[u8],
) -> Option<(FileContext, usize)>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let mut f = File::new_from_context(ctx, fs).await.ok()?;
    f.seek(SeekFrom::Start(u64::from(byte_offset))).await.ok()?;
    let mut written = 0;
    while written < src.len() {
        match f.write(&src[written..]).await {
            Ok(0) => break,
            Ok(n) => written += n,
            Err(_) => return None,
        }
    }
    let newctx = f.close().await.ok()?;
    Some((newctx, written))
}

/// File length in bytes, via `Seek(End(0))` — `File::size` is private to
/// embedded-fatfs's own `file` module, so the portable way to read a file's
/// length from outside the crate is the same trick any `Seek` consumer uses:
/// seeking to the end returns the new absolute position, which IS the length.
/// Leaves the detached context's cursor at EOF; harmless, since every
/// `read_context`/`write_context`/`read_context_exact` reattach seeks
/// absolutely before doing anything else. `None` on any FS error.
pub async fn size_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    ctx: FileContext,
) -> Option<(FileContext, u32)>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let mut f = File::new_from_context(ctx, fs).await.ok()?;
    let size = f.seek(SeekFrom::End(0)).await.ok()?;
    let newctx = f.close().await.ok()?;
    Some((newctx, size as u32))
}

/// Truncate the file to `new_len` bytes. embedded-fatfs's `File::truncate`
/// truncates AT THE FILE'S CURRENT POSITION (sets size = offset), so seek to
/// `new_len` first, then truncate. `None` on any FS error (including a
/// `new_len` past the current size — `Seek` clamps to EOF rather than
/// growing the file, so this primitive can only shrink, matching every task
/// context caller today).
pub async fn truncate_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    ctx: FileContext,
    new_len: u32,
) -> Option<(FileContext, ())>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let mut f = File::new_from_context(ctx, fs).await.ok()?;
    f.seek(SeekFrom::Start(u64::from(new_len))).await.ok()?;
    f.truncate().await.ok()?;
    let newctx = f.close().await.ok()?;
    Some((newctx, ()))
}

// --- R2 Task 2: path ops (create/unlink/rename/mkdir/set_time) -------------
//
// Unlike the handle-based ops above, these operate directly on `fs.root_dir()`
// plus a `'/'`-separated path -- there is no open `FileContext` to reattach.
// `create_context` is the one exception: it opens/creates the file, then
// detaches it to a `FileContext` exactly like `open_context` does, so its
// caller gets a handle to install into the table. Every crate error maps to
// `None`; the device layer (`efatfs_fs.rs`, Task 4) decides how to surface
// that as a task-context result code.

/// Create `path`, returning a detached [`FileContext`] for the new file --
/// mirrors [`open_context`]'s open→`close()` detach, but via `create_file`
/// instead of `open_file`.
///
/// `exclusive == false` is WRITE_CREATE: open-or-create, then truncate to
/// empty (embedded-fatfs's own `create_file` opens an existing file
/// as-is -- see its doc comment -- so the truncate here is what gives
/// WRITE_CREATE its "starts empty" semantics, matching `EFatFs::write_new`'s
/// create+truncate pairing).
///
/// `exclusive == true` is WRITE_CREATE_NEW: `None` if `path` already exists.
/// embedded-fatfs has no atomic create-if-absent primitive (`Dir::create_file`
/// always opens-or-creates), so this checks `Dir::exists` first -- a
/// check-then-create race is unreachable here: task-context file ops are
/// single-threaded, both on host (this test binary's single `block_on`) and
/// on device (one task drives the mounted `FileSystem` at a time under
/// `efatfs_fs::with_fs`'s mutex).
pub async fn create_context<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    path: &str,
    exclusive: bool,
) -> Option<FileContext>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let root = fs.root_dir();
    if exclusive && root.exists(path).await.ok()? {
        return None;
    }
    let mut f = root.create_file(path).await.ok()?;
    f.truncate().await.ok()?;
    f.close().await.ok()
}

/// Delete the file or empty directory at `path`. `None` on any FS error
/// (including a non-existent path or a non-empty directory).
pub async fn unlink<IO, TP, OCC>(fs: &FileSystem<IO, TP, OCC>, path: &str) -> Option<()>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    fs.root_dir().remove(path).await.ok()
}

/// Create the directory at `path`; `path`'s parent must already exist.
/// `None` on any FS error.
pub async fn mkdir<IO, TP, OCC>(fs: &FileSystem<IO, TP, OCC>, path: &str) -> Option<()>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    fs.root_dir().create_dir(path).await.ok()?;
    Some(())
}

/// Rename/move `old` to `new`, both paths relative to the volume root.
/// `None` on any FS error, including `new` already existing.
pub async fn rename<IO, TP, OCC>(fs: &FileSystem<IO, TP, OCC>, old: &str, new: &str) -> Option<()>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let root = fs.root_dir();
    root.rename(old, &root, new).await.ok()
}

/// Set `path`'s modified-time directory-entry field from `timestamp`, a
/// packed 32-bit FAT date/time exactly matching C-FatFS's `get_fattime()` /
/// `FILINFO::fdate,ftime` convention this codebase already uses elsewhere
/// (`src/fatfs/ff.c`'s `GET_FATTIME()`): the high 16 bits are the DOS date
/// (`(year-1980)<<9 | month<<5 | day`), the low 16 bits are the DOS time
/// (`hour<<11 | min<<5 | sec/2`) -- i.e. the same halves `Date`/`Time` encode,
/// concatenated as `(date << 16) | time`.
///
/// `File::set_modified` only updates the in-memory entry (same caveat as
/// `write_context`'s doc comment); `close()`'s flush is what persists it.
/// `None` on any FS error, or if `timestamp` decodes to an out-of-range
/// date/time component (`Date`/`Time` panic on out-of-range fields, so this
/// validates by hand instead of decoding blind).
pub async fn set_time<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    path: &str,
    timestamp: u32,
) -> Option<()>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let dos_date = (timestamp >> 16) as u16;
    let dos_time = timestamp as u16;
    let year = (dos_date >> 9) + 1980;
    let month = (dos_date >> 5) & 0xF;
    let day = dos_date & 0x1F;
    let hour = dos_time >> 11;
    let min = (dos_time >> 5) & 0x3F;
    let sec = (dos_time & 0x1F) * 2;
    if !(1..=12).contains(&month) || !(1..=31).contains(&day) || hour > 23 || min > 59 || sec > 59 {
        return None;
    }

    let mut f = fs.root_dir().open_file(path).await.ok()?;
    #[allow(deprecated)]
    // embedded-fatfs deprecates set_modified in favor of a custom TimeProvider;
    // task-context callers (deluge's `fileSetTimeDate`/rename-with-timestamp
    // callers) set an explicit timestamp, not "now", so there is no
    // `TimeProvider` shaped for this.
    f.set_modified(DateTime::new(
        Date::new(year, month, day),
        Time::new(hour, min, sec, 0),
    ));
    f.close().await.ok()?;
    Some(())
}

// --- R2 Task 3: directory enumeration + opaque open-by-locator -------------
//
// `Dir`/`DirIter` borrow `&FileSystem` (`Dir::iter(&self) -> DirIter<'a,
// ..>`, `crates/embedded-fatfs/src/dir.rs:134`), so a live iterator can't be
// held across an `.await` boundary or returned from a function bound to a
// short-lived `&FileSystem` borrow -- the same "no borrow across the FS call"
// constraint every other primitive in this module sidesteps via detached,
// owned `FileContext`s. `readdir_open` sidesteps it the same way: it walks
// the WHOLE directory to completion under one `with_fs` call and returns an
// owned snapshot (`DirCursor`, a `Vec` + a walk index), not a live borrow.
//
// Locator plan-time investigation (recorded here; see the Task-3 commit body
// for the summary). The brief's default shape was an
// `EfatfsLocator{first_cluster: u32, size: u32}` with `open_by_locator`
// "constructing a File from first_cluster". That is NOT cheaply available
// from OUTSIDE this crate:
//   - `DirEntry::first_cluster()` and `File::new` are both `pub(crate)` in
//     embedded-fatfs (dir_entry.rs:647, file.rs:52) -- unreachable here
//     without vendoring a new public constructor.
//   - Even from inside the crate, the only supported "resume this file"
//     entry point, `File::new_from_context`, REJECTS a context with no
//     `entry: Option<DirEntryEditor>` outright (file.rs:77,
//     `context.entry.as_ref().ok_or(Error::InvalidInput)?`). A `File` built
//     from a bare cluster with no entry also loses its EOF/size bound
//     (`File::size` returns `None` without an `entry`, file.rs:224-229) and
//     can never persist (`truncate`/`flush`'s dir-entry update no-op or
//     panic without one, file.rs:104-124,271-280). A first_cluster alone is
//     missing exactly the on-disk validation and size-tracking the `entry`
//     carries -- the entry validation this module's other primitives already
//     lean on (see `read_context`'s doc comment and the module doc's
//     generation-guard rationale).
//
// What IS public and already free: `DirEntry::to_file()` builds a `File`
// complete with its `DirEntryEditor`, and `File::close()` detaches that to a
// plain `FileContext` -- the SAME opaque value `open_context`/`HandleTable`
// already pass around by handle. So `readdir_open`'s single snapshot pass
// detaches a `FileContext` per FILE entry exactly the way `open_context`
// does, and `EfatfsLocator` carries THAT (plus a cached `size`, so a caller
// doesn't need to reopen just to learn it). `open_by_locator` is then
// `File::new_from_context` + `close()` -- genuinely O(1) (one directory-entry
// byte comparison at the position `close()` captured, no path walk, no
// cluster-chain restart from `first_cluster`) and keeps the SAME
// entry-validated read/write/flush semantics every other `*_context`
// primitive here has. This is neither the brief's literal
// `{first_cluster, size}` shape nor the path-based fallback it names as the
// accepted degrade -- it is the crate's own supported "resume a file"
// mechanism, reused; no vendored-crate changes were needed. Directories have
// no `FileContext` (`DirEntry::to_file()` panics on a directory entry --
// dir_entry.rs:666), so `EfatfsLocator`/`open_by_locator` are FILE-only;
// `readdir_open` leaves the locator `None` for `is_dir` entries.

/// One directory entry snapshotted by [`readdir_open`]/[`readdir_next`].
///
/// `modified` is the same packed 32-bit FAT date/time [`set_time`]
/// documents: `(dos_date << 16) | dos_time`, `dos_date =
/// (year-1980)<<9 | month<<5 | day`, `dos_time = hour<<11 | min<<5 | sec/2`
/// -- matching C-FatFS's `FILINFO::fdate,ftime` convention this codebase
/// already uses elsewhere.
///
/// `attrs` is the raw FAT attribute byte
/// (`embedded_fatfs::FileAttributes::bits()`: READ_ONLY=0x01, HIDDEN=0x02,
/// SYSTEM=0x04, VOLUME_ID=0x08, DIRECTORY=0x10, ARCHIVE=0x20), bit-for-bit
/// C-FatFS's `FILINFO::fattrib` (`AM_*`).
#[derive(Clone)]
pub struct DirEntryInfo {
    pub name: heapless::String<256>,
    pub is_dir: bool,
    pub size: u32,
    pub modified: u32,
    pub attrs: u8,
}

/// Opaque, O(1)-reopenable locator for a FILE entry surfaced by
/// [`readdir_next`] via [`readdir_locator`] -- Task 5's sample browser wants
/// O(1) reopen without a directory-tree walk. See this section's module
/// comment for the plan-time investigation behind this shape. `size` is
/// cached from the same snapshot pass so a caller can read it without
/// reopening.
#[derive(Clone)]
#[repr(C)]
pub struct EfatfsLocator {
    ctx: FileContext,
    pub size: u32,
}

/// A directory snapshot collected by [`readdir_open`]: an owned `Vec` of
/// `(entry, locator)` pairs plus a walk index -- see this section's module
/// comment for why this can't be a live `DirIter` borrow instead.
pub struct DirCursor {
    entries: Vec<(DirEntryInfo, Option<EfatfsLocator>)>,
    idx: usize,
}

/// Pack an embedded-fatfs `DateTime` into the `(dos_date << 16) | dos_time`
/// convention [`set_time`] documents and consumes, using `Date`/`Time`'s
/// public `year`/`month`/`day`/`hour`/`min`/`sec` fields (the crate's own
/// `encode()` doing the same math is `pub(crate)`).
fn pack_fat_datetime(dt: DateTime) -> u32 {
    let dos_date = ((dt.date.year - 1980) << 9) | (dt.date.month << 5) | dt.date.day;
    let dos_time = (dt.time.hour << 11) | (dt.time.min << 5) | (dt.time.sec / 2);
    (u32::from(dos_date) << 16) | u32::from(dos_time)
}

/// Open `path` as a directory and snapshot ALL of its entries in one pass
/// (see this section's module comment for why a snapshot, not a live
/// iterator). Skips the `.`/`..` pseudo-entries embedded-fatfs's iterator
/// yields for non-root directories -- C-FatFS's `f_readdir` never surfaces
/// those, so this matches its enumeration surface (same filter
/// `fs_differential::efatfs::EFatFs::read_dir` already applies). `path`
/// empty (or all `/`) opens the volume root. `None` on any FS error,
/// including `path` not naming a directory.
pub async fn readdir_open<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    path: &str,
) -> Option<DirCursor>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let root = fs.root_dir();
    let dir = if path.trim_matches('/').is_empty() {
        root
    } else {
        root.open_dir(path).await.ok()?
    };
    let mut iter = dir.iter();
    let mut entries = Vec::new();
    while let Some(r) = iter.next().await {
        let e = r.ok()?;
        let name = e.file_name();
        if name == "." || name == ".." {
            continue;
        }
        // R2 Task 4 fix: a name that doesn't fit `heapless::String<256>` (FAT LFN is
        // 255 UTF-16 units, which can exceed 256 UTF-8 bytes) used to fail this
        // whole snapshot via `?` -- ONE oversized filename anywhere in the
        // directory took out the ENTIRE listing. Skip just that entry instead: `?`
        // here would propagate to the function's `Option<DirCursor>` return and
        // abort the walk with `None`, which the C-ABI (`efatfs_fs.rs`/
        // `efatfs_host_shim.rs`, Task 4) can't distinguish from "path is not a
        // directory" / a real FS error.
        let Ok(name) = heapless::String::try_from(name.as_str()) else {
            continue;
        };
        let is_dir = e.is_dir();
        let size = if is_dir { 0 } else { e.len() as u32 };
        let info = DirEntryInfo {
            name,
            is_dir,
            size,
            modified: pack_fat_datetime(e.modified()),
            attrs: e.attributes().bits(),
        };
        // Directories have no FileContext to detach (DirEntry::to_file()
        // panics on one) -- only FILE entries get a locator.
        let locator = if is_dir {
            None
        } else {
            let ctx = e.to_file().close().await.ok()?;
            Some(EfatfsLocator { ctx, size })
        };
        entries.push((info, locator));
    }
    Some(DirCursor { entries, idx: 0 })
}

/// Advance `cursor` and return the next entry. Outer `Option` is an FS
/// error (`None`); inner `Option` is end-of-directory (`None`). `cursor` is
/// an owned snapshot (see [`readdir_open`]), so this never touches the FS or
/// does any FS I/O -- it used to take an (unused) `&FileSystem` parameter
/// purely to mirror this module's other `fs`-taking primitives, but that made
/// callers route an in-memory snapshot read through the single FS mutex for
/// no reason (R2 Task 4 review finding: it serialized dir-page reads behind
/// any in-flight streaming SD read). Dropped: this is synchronous and
/// FS-free. An FS-error outer `None` cannot occur for a snapshot cursor
/// today, but the signature leaves room for a future non-snapshot cursor
/// that could fail mid-walk.
#[allow(clippy::unnecessary_wraps)]
pub fn readdir_next(cursor: &mut DirCursor) -> Option<Option<DirEntryInfo>> {
    let Some((info, _locator)) = cursor.entries.get(cursor.idx) else {
        return Some(None); // end of directory
    };
    let info = info.clone();
    cursor.idx += 1;
    Some(Some(info))
}

/// The [`EfatfsLocator`] for the entry most recently yielded by
/// [`readdir_next`] -- `None` for a directory entry (see [`EfatfsLocator`]'s
/// doc), or if `readdir_next` has not yet been called on this cursor, or the
/// cursor is already at end-of-directory.
pub fn readdir_locator(cursor: &DirCursor) -> Option<EfatfsLocator> {
    let i = cursor.idx.checked_sub(1)?;
    cursor.entries.get(i)?.1.clone()
}

/// Reopen the file `loc` names -- O(1): [`File::new_from_context`]
/// revalidates against the single on-disk directory entry `loc`'s
/// [`FileContext`] already points at (no directory-tree walk, no
/// cluster-chain restart from `first_cluster`), the same validation
/// `read_context`/`write_context` rely on. `None` if the file has since been
/// deleted/modified/moved (the on-disk entry no longer matches) or on any
/// other FS error.
pub async fn open_by_locator<IO, TP, OCC>(
    fs: &FileSystem<IO, TP, OCC>,
    loc: EfatfsLocator,
) -> Option<FileContext>
where
    IO: ReadWriteSeek,
    TP: TimeProvider,
    OCC: OemCpConverter,
{
    let f = File::new_from_context(loc.ctx, fs).await.ok()?;
    f.close().await.ok()
}

/// Pack a decomposed timestamp into the `(dos_date << 16) | dos_time` convention
/// [`set_time`] consumes -- the encode-side counterpart of that function's own
/// decode. Shared here (rather than duplicated in `efatfs_fs.rs` AND
/// `efatfs_host_shim.rs`) because both C-ABI bridges receive the task-context
/// `DelugeTimestamp`'s decomposed fields (year/month/day/hour/minute/second, see
/// `include/libdeluge/types.h`) from `deluge_efatfs_set_time` and need the same
/// packing before calling [`set_time`]. No range validation here -- `set_time`
/// already validates the packed result before decoding it back.
pub fn pack_timestamp(year: u16, month: u8, day: u8, hour: u8, minute: u8, second: u8) -> u32 {
    let dos_date = ((year - 1980) << 9) | (u16::from(month) << 5) | u16::from(day);
    let dos_time = (u16::from(hour) << 11) | (u16::from(minute) << 5) | u16::from(second / 2);
    (u32::from(dos_date) << 16) | u32::from(dos_time)
}

// --- R2 Task 4: task-context file-handle table + dir-cursor table + locator
// table -------------------------------------------------------------------
//
// The streaming-read `HandleTable` above is deliberately NOT reused for
// task-context file I/O: task-context `deluge::io::File` callers `seek()` then
// `read()`/`write()` with no explicit byte offset (`file_io.h`'s
// `deluge_file_seek`/`_read`/`_write` contract), so each handle needs a
// PERSISTED cursor position threaded through every op -- something the
// streaming table's `Slot` (generation + `FileContext` only) has no field for
// and does not need (the streaming read path always passes an explicit
// absolute `byte_offset`). Extending the streaming `Slot`/`HandleTable` to
// carry a position would touch the already-proven R1 streaming path for a
// field it never uses; a separate, small `TaskFileTable` keeps the two
// concerns apart. `readdir_next`/`readdir_locator` are synchronous (no `.await`,
// no FS access -- see the module comment above [`DirCursor`]), so
// [`DirHandleTable`] needs no generation-guarded checkout/commit dance: its
// slots are claimed/read/freed under one lock, never split across an FS-mutex
// await.

/// Max concurrent task-context file handles (mirrors [`HandleTable`]'s
/// `MAX_HANDLES` cap; task-context file I/O is not high-concurrency).
pub const MAX_TASK_FILES: usize = 16;

struct TaskFileSlot {
    generation: u32,
    ctx: Option<FileContext>,
    /// The handle's current byte position -- `deluge_file_seek` sets it directly
    /// (no FS access needed); every read/write reattach explicitly seeks the
    /// underlying `File` to this value before touching it (see `read_context`/
    /// `write_context`'s absolute-seek discipline), so this is authoritative
    /// regardless of whatever cursor state the detached `FileContext` itself
    /// carries.
    position: u32,
}

/// A fixed-capacity table of task-context file handles: a detached
/// [`FileContext`] plus a persisted cursor `position`, keyed by a `u32` handle.
/// Composed under the SAME checkout/`with_fs`/commit discipline [`HandleTable`]
/// documents (never held across the FS-mutex await) -- `efatfs_fs.rs`/
/// `efatfs_host_shim.rs` own one behind an async `Mutex`.
pub struct TaskFileTable {
    slots: [TaskFileSlot; MAX_TASK_FILES],
}

impl Default for TaskFileTable {
    fn default() -> Self {
        Self::new()
    }
}

impl TaskFileTable {
    pub const fn new() -> Self {
        Self {
            slots: [const {
                TaskFileSlot {
                    generation: 0,
                    ctx: None,
                    position: 0,
                }
            }; MAX_TASK_FILES],
        }
    }

    /// Stash `ctx` in the lowest free slot at position 0. Returns the slot index
    /// as the handle, or `None` if the table is full.
    pub fn insert(&mut self, ctx: FileContext) -> Option<u32> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.ctx.is_none() {
                slot.ctx = Some(ctx);
                slot.position = 0;
                slot.generation = slot.generation.wrapping_add(1);
                return Some(i as u32);
            }
        }
        None
    }

    /// Clone `handle`'s [`FileContext`] and current position out of the table,
    /// together with the slot's generation. `None` if out of range or free.
    pub fn checkout(&self, handle: u32) -> Option<(u32, FileContext, u32)> {
        let slot = self.slots.get(handle as usize)?;
        let ctx = slot.ctx.clone()?;
        Some((slot.generation, ctx, slot.position))
    }

    /// Write `newctx`/`new_position` back into `handle`'s slot, generation-gated
    /// exactly like [`HandleTable::commit`] (see its doc for the stale-write-back
    /// rationale a `remove`+`insert` recycle guards against).
    pub fn commit(
        &mut self,
        handle: u32,
        captured_generation: u32,
        newctx: FileContext,
        new_position: u32,
    ) {
        if let Some(slot) = self.slots.get_mut(handle as usize) {
            if slot.generation == captured_generation && slot.ctx.is_some() {
                slot.ctx = Some(newctx);
                slot.position = new_position;
            }
        }
    }

    /// Set `handle`'s cursor position directly -- no FS access needed. `false`
    /// if the handle is out of range or free.
    pub fn seek(&mut self, handle: u32, offset: u32) -> bool {
        match self.slots.get_mut(handle as usize) {
            Some(slot) if slot.ctx.is_some() => {
                slot.position = offset;
                true
            }
            _ => false,
        }
    }

    /// Free `handle`'s slot. No-op for an out-of-range handle. Bumps the
    /// generation, same rationale as [`HandleTable::remove`].
    pub fn remove(&mut self, handle: u32) {
        if let Some(slot) = self.slots.get_mut(handle as usize) {
            slot.ctx = None;
            slot.position = 0;
            slot.generation = slot.generation.wrapping_add(1);
        }
    }
}

/// Max concurrent open directory browses (small -- task context browses one
/// folder at a time; headroom for a nested "up one level" during navigation).
pub const MAX_DIR_HANDLES: usize = 8;

struct DirSlot {
    cursor: Option<DirCursor>,
}

/// A fixed-capacity table of open [`DirCursor`] snapshots keyed by a `u32`
/// handle. Unlike [`TaskFileTable`]/[`HandleTable`], entries need no generation
/// guard: [`readdir_next`]/[`readdir_locator`] are synchronous and never touch
/// the FS, so a slot is claimed, read some number of times, and freed all under
/// one lock -- there is no split checkout/`with_fs`/commit window for a
/// concurrent `close` to race.
pub struct DirHandleTable {
    slots: [DirSlot; MAX_DIR_HANDLES],
}

impl Default for DirHandleTable {
    fn default() -> Self {
        Self::new()
    }
}

impl DirHandleTable {
    pub const fn new() -> Self {
        Self {
            slots: [const { DirSlot { cursor: None } }; MAX_DIR_HANDLES],
        }
    }

    /// Stash `cursor` in the lowest free slot. Returns the slot index as the
    /// handle, or `None` if the table is full.
    pub fn insert(&mut self, cursor: DirCursor) -> Option<u32> {
        for (i, slot) in self.slots.iter_mut().enumerate() {
            if slot.cursor.is_none() {
                slot.cursor = Some(cursor);
                return Some(i as u32);
            }
        }
        None
    }

    /// Borrow `handle`'s cursor mutably (for [`readdir_next`]/[`readdir_locator`]).
    /// `None` if out of range or free.
    pub fn get_mut(&mut self, handle: u32) -> Option<&mut DirCursor> {
        self.slots.get_mut(handle as usize)?.cursor.as_mut()
    }

    /// Free `handle`'s slot. No-op for an out-of-range handle.
    pub fn remove(&mut self, handle: u32) {
        if let Some(slot) = self.slots.get_mut(handle as usize) {
            slot.cursor = None;
        }
    }
}

/// Sentinel returned by [`LocatorTable::insert`]'s caller in
/// `efatfs_fs.rs`/`efatfs_host_shim.rs` for a directory entry with no locator
/// (a directory, not a file -- see [`EfatfsLocator`]'s doc) -- "no locator
/// available", never a value [`LocatorTable::insert`] can itself produce (see
/// its doc).
pub const NO_LOCATOR: u32 = u32::MAX;

/// Max live locator handles. A ring buffer (see [`LocatorTable::insert`]), not
/// a "table full" cap -- this bounds how many of the MOST RECENTLY surfaced
/// directory entries can still be opened by locator, not how many entries a
/// browse can enumerate.
pub const MAX_LOCATORS: usize = 64;

struct LocatorSlot {
    generation: u32,
    loc: Option<EfatfsLocator>,
}

/// A directory-browse-lifetime-independent table of [`EfatfsLocator`]s, keyed
/// by an opaque `u32` handle that packs a generation into its high 8 bits and
/// the ring slot index into its low 24 bits (`MAX_LOCATORS` is nowhere near
/// 2^24, so the two never collide).
///
/// Deliberately NOT keyed to any [`DirHandleTable`] entry's lifetime: Task 5's
/// sample browser snapshots a whole folder's entries to display a scrollable
/// list (readdir_next never rewinds), which means it may enumerate -- and
/// close -- the directory handle well before the user picks an entry to open.
/// A [`LocatorTable`] entry must therefore outlive its originating
/// [`DirHandleTable`] slot.
///
/// [`insert`](Self::insert) never fails: it is a fixed-size RING that always
/// claims the next slot round-robin, overwriting whatever locator (if any)
/// was there. This bounds memory without a "table full" degrade path to
/// handle at the C-ABI layer, at the cost of very old (long-scrolled-past)
/// locator handles silently no longer resolving once their slot is recycled --
/// [`get`](Self::get) detects that via the packed generation and returns
/// `None`, which the browser (Task 5) is expected to treat as "reopen by path
/// instead," the same graceful degrade a deleted/moved file already forces
/// via [`open_by_locator`]'s own `None` case.
pub struct LocatorTable {
    slots: [LocatorSlot; MAX_LOCATORS],
    next: usize,
}

impl Default for LocatorTable {
    fn default() -> Self {
        Self::new()
    }
}

impl LocatorTable {
    pub const fn new() -> Self {
        Self {
            slots: [const {
                LocatorSlot {
                    generation: 0,
                    loc: None,
                }
            }; MAX_LOCATORS],
            next: 0,
        }
    }

    /// Claim the ring's next slot for `loc`, overwriting its previous contents.
    /// Always succeeds. Returns the packed handle (generation << 24 | index).
    pub fn insert(&mut self, loc: EfatfsLocator) -> u32 {
        let idx = self.next;
        self.next = (self.next + 1) % MAX_LOCATORS;
        let slot = &mut self.slots[idx];
        slot.generation = (slot.generation + 1) & 0xFF;
        slot.loc = Some(loc);
        (slot.generation << 24) | (idx as u32)
    }

    /// Clone the [`EfatfsLocator`] behind `handle`, iff the slot it names still
    /// carries the SAME generation `handle` was minted with (i.e. hasn't been
    /// recycled by the ring for a different entry since). `None` for
    /// [`NO_LOCATOR`], an out-of-range index, or a stale/recycled generation.
    pub fn get(&self, handle: u32) -> Option<EfatfsLocator> {
        let idx = (handle & 0x00FF_FFFF) as usize;
        let generation = handle >> 24;
        let slot = self.slots.get(idx)?;
        if slot.generation != generation {
            return None;
        }
        slot.loc.clone()
    }
}
