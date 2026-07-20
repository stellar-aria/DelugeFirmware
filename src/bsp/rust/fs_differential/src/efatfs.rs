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

/// A mounted `embedded-fatfs` volume, read path only. Same public surface as
/// `fatfs_c::CFatFs` (`mount`/`read_file`/`read_dir`) so Task 5's `FsOps`
/// trait can wrap both identically.
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
}
