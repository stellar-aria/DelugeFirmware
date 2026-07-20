//! Filesystem-op result types shared across the C-FatFS and embedded-fatfs
//! backends.
//!
//! `Entry` is owned by Task 5 (the directory-listing differential surface);
//! it was forward-declared ahead of this task because Task 3's
//! `fatfs_c::CFatFs::read_dir` already needed a concrete return type. This
//! task adds `FsOps`, the common interface `diff::compare_read` walks both
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
