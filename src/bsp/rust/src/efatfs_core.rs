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

use embedded_fatfs::{File, FileContext, FileSystem, OemCpConverter, ReadWriteSeek, TimeProvider};
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
