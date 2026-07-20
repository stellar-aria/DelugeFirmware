//! Filesystem-op result types shared across the C-FatFS and embedded-fatfs
//! backends.
//!
//! `Entry` is owned by Task 5 (the directory-listing differential surface);
//! it is forward-declared here, ahead of that task, because Task 3's
//! `fatfs_c::CFatFs::read_dir` already needs a concrete return type. Task 5
//! must keep these exact field names/types when it builds out the rest of
//! this module.
pub struct Entry {
    pub name: String,
    pub size: u64,
    pub is_dir: bool,
}
