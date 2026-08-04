//! Filesystem-op result types shared across the C-FatFS and embedded-fatfs
//! backends.
//!
//! `Entry` is the directory-listing entry type both backends' `read_dir`
//! return. `FsOps` is the common interface `diff::compare_read` walks both
//! backends through.
use crate::efatfs::EFatFs;
use crate::fatfs_c::CFatFs;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    pub name: String,
    pub size: u64,
    pub is_dir: bool,
}

/// Common read/enumerate surface both FatFS backends present, so
/// `diff::compare_read` can walk either one through a single `&dyn FsOps`
/// without caring which concrete stack it's driving.
pub trait FsOps {
    fn read_file(&self, path: &str) -> Vec<u8>;
    fn read_dir(&self, path: &str) -> Vec<Entry>; // sorted by name
}

impl FsOps for CFatFs {
    fn read_file(&self, path: &str) -> Vec<u8> {
        CFatFs::read_file(self, path)
    }
    fn read_dir(&self, path: &str) -> Vec<Entry> {
        CFatFs::read_dir(self, path)
    }
}

impl FsOps for EFatFs {
    fn read_file(&self, path: &str) -> Vec<u8> {
        EFatFs::read_file(self, path)
    }
    fn read_dir(&self, path: &str) -> Vec<Entry> {
        EFatFs::read_dir(self, path)
    }
}

/// A single write-path operation the differential replays
/// identically on both backends, each against its own copy of the same
/// starting image (see `diff::replay_and_compare`). Covers the write-path
/// primitives the Deluge's own file I/O actually exercises: create a
/// directory, write a new (possibly multi-cluster) file, extend an existing
/// file (potentially across a cluster boundary), rename, and delete.
#[derive(Debug, Clone)]
pub enum Op {
    Mkdir(String),
    Write(String, Vec<u8>),
    Extend(String, Vec<u8>),
    Delete(String),
    Rename(String, String),
}

/// Write-path surface both backends present, mirroring `FsOps` for reads.
/// `&mut self` because both backends wrap real mutable filesystem state (a
/// mounted C FatFS volume / an `embedded_fatfs::FileSystem`) even though the
/// actual bytes land in the shared `DISK` RAM image (`ram_disk.rs`), not in
/// `self` itself.
pub trait FsOpsMut: FsOps {
    /// Create a directory. `path`'s parent must already exist.
    fn mkdir(&mut self, path: &str);
    /// Create (or truncate, if it already exists) `path` and write `bytes`
    /// as its whole contents.
    fn write_new(&mut self, path: &str, bytes: &[u8]);
    /// Open the existing file at `path`, seek to its end, and append
    /// `bytes`.
    fn append(&mut self, path: &str, bytes: &[u8]);
    /// Delete an existing file or (empty) directory.
    fn delete(&mut self, path: &str);
    /// Rename/move `from` to `to`.
    fn rename(&mut self, from: &str, to: &str);

    /// Dispatch a single `Op` to the matching method above.
    fn apply(&mut self, op: &Op) {
        match op {
            Op::Mkdir(p) => self.mkdir(p),
            Op::Write(p, b) => self.write_new(p, b),
            Op::Extend(p, b) => self.append(p, b),
            Op::Delete(p) => self.delete(p),
            Op::Rename(a, b) => self.rename(a, b),
        }
    }
}

impl FsOpsMut for CFatFs {
    fn mkdir(&mut self, path: &str) {
        CFatFs::mkdir(self, path)
    }
    fn write_new(&mut self, path: &str, bytes: &[u8]) {
        CFatFs::write_new(self, path, bytes)
    }
    fn append(&mut self, path: &str, bytes: &[u8]) {
        CFatFs::append(self, path, bytes)
    }
    fn delete(&mut self, path: &str) {
        CFatFs::delete(self, path)
    }
    fn rename(&mut self, from: &str, to: &str) {
        CFatFs::rename(self, from, to)
    }
}

impl FsOpsMut for EFatFs {
    fn mkdir(&mut self, path: &str) {
        EFatFs::mkdir(self, path)
    }
    fn write_new(&mut self, path: &str, bytes: &[u8]) {
        EFatFs::write_new(self, path, bytes)
    }
    fn append(&mut self, path: &str, bytes: &[u8]) {
        EFatFs::append(self, path, bytes)
    }
    fn delete(&mut self, path: &str) {
        EFatFs::delete(self, path)
    }
    fn rename(&mut self, from: &str, to: &str) {
        EFatFs::rename(self, from, to)
    }
}
