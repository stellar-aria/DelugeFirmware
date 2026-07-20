//! Mounts the vendored `embedded-fatfs` (`crates/embedded-fatfs/`) over the
//! SAME shared `DISK` image `ram_disk.rs` gives the C FatFS FFI bridge
//! (`fatfs_c.rs`), and reads through it -- the Rust half of the SP0
//! differential's read path.
//!
//! Confirmed async API, from the vendored source (not the crates.io docs,
//! not guessed -- this crate is never published and pins its own fork):
//!
//!   `embedded_fatfs::FileSystem::<IO, TP, OCC>::new<T: IntoStorage<IO>>`
//!       `(storage: T, options: FsOptions<TP, OCC>) -> Result<Self, Error<IO::Error>>`
//!       -- async, and requires `storage` to be un-seeked (asserts pos==0 after
//!       its own `seek(Start(0))`).                                    (fs.rs:377)
//!   `embedded_fatfs::FsOptions::new() -> FsOptions<DefaultTimeProvider, LossyOemCpConverter>`
//!       (fs.rs:256)
//!   `FileSystem::root_dir(&self) -> Dir<'_, IO, TP, OCC>`             -- sync (fs.rs:629)
//!   `Dir::open_file(&self, path: &str) -> Result<File<'a,IO,TP,OCC>, Error<IO::Error>>`
//!       -- async; `path` is '/'-separated, leading/trailing '/' trimmed by
//!       `split_path`, so an absolute path like "/SAMPLES/hello.txt" works the
//!       same as `CFatFs::read_file`'s.                                 (dir.rs:307)
//!   `File` implements `embedded_io_async::Read`:
//!       `async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Error<IO::Error>>`,
//!       `Ok(0)` at EOF. embedded-io-async 0.7 ships no `ReadExt::read_to_end`,
//!       so `read_file` below loops `read()` to drain the file.        (file.rs:317-368)
//!   `Dir::iter(&self) -> DirIter<'a, IO, TP, OCC>`                    -- sync (dir.rs:123)
//!   `DirIter::next(&mut self) -> Option<Result<DirEntry<'a,IO,TP,OCC>, Error<IO::Error>>>`
//!       -- async, `None` at end-of-directory.                         (dir.rs:833)
//!   `DirEntry::file_name(&self) -> String`, `::len(&self) -> u64`, `::is_dir(&self) -> bool`
//!       (dir_entry.rs:617,712,637; `file_name`/`len` need the `alloc` feature,
//!       already on in this crate's `Cargo.toml`).
//!
//! Write path (Task 6), same vendored source:
//!   `Dir::create_file(&self, path: &str) -> Result<File<'a,IO,TP,OCC>, Error<IO::Error>>`
//!       -- async; opens if it already exists, creates (empty) otherwise.   (dir.rs:339)
//!   `File::truncate(&mut self) -> Result<(), Error<IO::Error>>`
//!       -- async; resets size to the file's current offset.               (file.rs:102)
//!   `File` implements `embedded_io_async::Write`: `write`/`write_all`/`flush`.
//!       `flush` MUST be called (or `File::close`) -- `update_dir_entry_after_write`
//!       only updates the in-memory entry; the on-disk directory entry (size,
//!       modified time) is written by `flush`, not by `write` itself.       (file.rs:254,371-444)
//!   `Dir::create_dir(&self, path: &str) -> Result<Self, Error<IO::Error>>`  (dir.rs:384)
//!   `Dir::remove(&self, path: &str) -> Result<(), Error<IO::Error>>`       (dir.rs:456)
//!   `Dir::rename(&self, src_path: &str, dst_dir: &Dir<'_,IO,TP,OCC>, dst_path: &str)
//!       -> Result<(), Error<IO::Error>>`                                   (dir.rs:520)
//!
//! `IO: ReadWriteSeek` is a blanket impl over any `T: embedded_io_async::{Read,
//! Write, Seek}` (fs.rs:130-132), and `IntoStorage<T> for T` is blanket too
//! (fs.rs:343), so `MemIo` below needs only those three trait impls -- no
//! bespoke `IntoStorage`.

use crate::ops::Entry;
use crate::ram_disk::RamDisk;
use embassy_futures::block_on;
use embedded_fatfs::{DefaultTimeProvider, FileSystem, FsOptions, LossyOemCpConverter};
use embedded_io_async::{ErrorType, Read, Seek, SeekFrom, Write};

/// An `embedded_io_async` storage device over the shared `DISK` image (see
/// `ram_disk.rs`) -- the same bytes the C FatFS FFI bridge's `disk_*`
/// callbacks read/write, just addressed byte-granular instead of
/// sector-granular.
pub struct MemIo {
    pos: u64,
}

impl ErrorType for MemIo {
    type Error = core::convert::Infallible;
}

impl Read for MemIo {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
        let n = RamDisk::read_at(self.pos, buf);
        self.pos += n as u64;
        Ok(n)
    }
}

impl Write for MemIo {
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
        RamDisk::write_at(self.pos, buf);
        self.pos += buf.len() as u64;
        Ok(buf.len())
    }

    async fn flush(&mut self) -> Result<(), Self::Error> {
        Ok(())
    }
}

impl Seek for MemIo {
    async fn seek(&mut self, pos: SeekFrom) -> Result<u64, Self::Error> {
        self.pos = match pos {
            SeekFrom::Start(o) => o,
            SeekFrom::End(o) => (RamDisk::len() as i64 + o) as u64,
            SeekFrom::Current(o) => (self.pos as i64 + o) as u64,
        };
        Ok(self.pos)
    }
}

/// A mounted `embedded-fatfs` volume, read + write path. Same public
/// surface as `fatfs_c::CFatFs` (`mount`/`read_file`/`read_dir`/write ops)
/// so `ops::FsOps`/`ops::FsOpsMut` can wrap both identically.
pub struct EFatFs {
    fs: FileSystem<MemIo, DefaultTimeProvider, LossyOemCpConverter>,
}

impl EFatFs {
    /// Mount the volume currently installed via `ram_disk::RamDisk::load`.
    pub fn mount() -> Self {
        let fs =
            block_on(FileSystem::new(MemIo { pos: 0 }, FsOptions::new())).expect("FileSystem::new");
        EFatFs { fs }
    }

    /// Read a whole file's contents.
    pub fn read_file(&self, path: &str) -> Vec<u8> {
        block_on(async {
            let root = self.fs.root_dir();
            let mut file = root.open_file(path).await.expect("open_file");
            let mut out = Vec::new();
            let mut buf = [0u8; 4096];
            loop {
                let n = file.read(&mut buf).await.expect("read");
                if n == 0 {
                    break;
                }
                out.extend_from_slice(&buf[..n]);
            }
            out
        })
    }

    /// List a directory's entries, sorted by name.
    pub fn read_dir(&self, path: &str) -> Vec<Entry> {
        block_on(async {
            let root = self.fs.root_dir();
            let dir = if path.trim_matches('/').is_empty() {
                root
            } else {
                root.open_dir(path).await.expect("open_dir")
            };
            let mut iter = dir.iter();
            let mut v = Vec::new();
            while let Some(r) = iter.next().await {
                let e = r.expect("dir entry");
                let name = e.file_name();
                // HARNESS NORMALIZATION: embedded-fatfs's directory iterator
                // yields `.` and `..` pseudo-entries for non-root
                // directories; C FatFS's f_readdir never does (it suppresses
                // them internally). This is an API-convention difference
                // between the two libraries, not a data/metadata bug in
                // either -- filter them out here so both backends present
                // the same logical directory view to the differential.
                if name == "." || name == ".." {
                    continue;
                }
                v.push(Entry {
                    name,
                    size: e.len(),
                    is_dir: e.is_dir(),
                });
            }
            v.sort_by(|a, b| a.name.cmp(&b.name));
            v
        })
    }

    /// Create a directory. `path`'s parent must already exist.
    pub fn mkdir(&mut self, path: &str) {
        block_on(async {
            self.fs.root_dir().create_dir(path).await.expect("create_dir");
        });
    }

    /// Create (or truncate, if it already exists) `path` and write `bytes`
    /// as its whole contents.
    pub fn write_new(&mut self, path: &str, bytes: &[u8]) {
        block_on(async {
            let root = self.fs.root_dir();
            let mut file = root.create_file(path).await.expect("create_file");
            file.truncate().await.expect("truncate");
            file.write_all(bytes).await.expect("write_all");
            file.flush().await.expect("flush");
        });
    }

    /// Open the existing file at `path`, seek to its end, and append
    /// `bytes`.
    pub fn append(&mut self, path: &str, bytes: &[u8]) {
        block_on(async {
            let root = self.fs.root_dir();
            let mut file = root.open_file(path).await.expect("open_file");
            file.seek(SeekFrom::End(0)).await.expect("seek end");
            file.write_all(bytes).await.expect("write_all");
            file.flush().await.expect("flush");
        });
    }

    /// Delete an existing file or (empty) directory.
    pub fn delete(&mut self, path: &str) {
        block_on(async {
            self.fs.root_dir().remove(path).await.expect("remove");
        });
    }

    /// Rename/move `from` to `to`, both absolute paths.
    ///
    /// Drives the crate's `Dir::rename` directly, with no parent-resolution
    /// workaround: Task 6's write differential found that `Dir::rename`
    /// silently dropped a multi-component `dst_path`'s leading directory
    /// components (`dir.rs:559` passed the raw, untraversed `dst_dir`
    /// parameter to `rename_internal` instead of the traversed destination
    /// parent it had just computed); Task 6B fixed that in the vendored
    /// crate (see `crates/embedded-fatfs/VENDOR.md`'s "Applied local
    /// fixes"), so a plain `root.rename(from, &root, to)` with a
    /// multi-component `to` now lands in the right directory and this
    /// harness no longer needs to route around it.
    pub fn rename(&mut self, from: &str, to: &str) {
        block_on(async {
            let root = self.fs.root_dir();
            let from = from.trim_start_matches('/');
            let to = to.trim_start_matches('/');
            root.rename(from, &root, to).await.expect("rename");
        });
    }
}
