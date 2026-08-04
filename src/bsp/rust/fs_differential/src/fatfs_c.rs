//! Safe wrapper over the vendored C FatFS (compiled by `build.rs`): read
//! path plus write path (mkdir/write/append/delete/rename).
//!
//! `extern "C"` declarations mirror the exact R0.14b signatures in
//! `src/fatfs/ff.h` under this harness's `ffconf.h` (FF_FS_EXFAT=0,
//! FF_LBA64=0, FF_LFN_UNICODE=0 -- so `FSIZE_t`/`LBA_t` are 32-bit `DWORD`
//! and `TCHAR` is plain `char`, not the `u64`/wide-char forms a
//! more-featured FatFS build would use).
use crate::ops::Entry;

// Opaque struct guards. `FATFS`/`FIL`/`DIR` are never field-accessed from
// Rust -- FatFs only ever reads/writes within the real struct's bounds, so
// an oversized `[u8; N]` blob is safe (UB only if the guard is too SMALL).
// Real host `sizeof` under this exact ffconf.h (confirmed by compiling
// src/fatfs/ff.c standalone with `-DDELUGE_HOST`, matching build.rs's
// flags, and printing `sizeof`): FATFS=640, FIL=568, DIR=64. Guards below
// give each generous headroom above that measured size.
#[repr(C)]
pub struct FATFS {
    _o: [u8; 704], // real sizeof(FATFS) == 640
}
#[repr(C)]
pub struct FIL {
    _o: [u8; 640], // real sizeof(FIL) == 568
}
#[repr(C)]
pub struct DIR {
    _o: [u8; 128], // real sizeof(DIR) == 64
}

// FILINFO IS field-accessed from Rust (read_dir reads fsize/fname/fattrib
// directly), so its layout must match the real C struct exactly, not just
// be large enough. Real layout under FF_FS_EXFAT=0 (FSIZE_t == DWORD ==
// u32, NOT u64 -- exFAT is what would widen it to QWORD) at FF_LFN_BUF=255
// / FF_SFN_BUF=12:
//   fsize: u32 (offset 0), fdate: u16 (4), ftime: u16 (6), fattrib: u8 (8),
//   altname: [u8; 13] (9), fname: [u8; 256] (22), total 280 w/ trailing
//   pad-to-4. Confirmed by the same host probe compile (sizeof(FILINFO) ==
//   280, offsetof(fname) == 22).
#[repr(C)]
pub struct FILINFO {
    pub fsize: u32,
    pub fdate: u16,
    pub ftime: u16,
    pub fattrib: u8,
    pub altname: [u8; 13],
    pub fname: [u8; 256],
}

extern "C" {
    fn f_mount(fs: *mut FATFS, path: *const u8, opt: u8) -> i32;
    fn f_open(fp: *mut FIL, path: *const u8, mode: u8) -> i32;
    fn f_read(fp: *mut FIL, buff: *mut u8, btr: u32, br: *mut u32) -> i32;
    fn f_close(fp: *mut FIL) -> i32;
    fn f_opendir(dp: *mut DIR, path: *const u8) -> i32;
    fn f_readdir(dp: *mut DIR, fno: *mut FILINFO) -> i32;
    fn f_closedir(dp: *mut DIR) -> i32;
    // Write path.
    fn f_write(fp: *mut FIL, buff: *const u8, btw: u32, bw: *mut u32) -> i32;
    fn f_mkdir(path: *const u8) -> i32;
    fn f_unlink(path: *const u8) -> i32;
    fn f_rename(path_old: *const u8, path_new: *const u8) -> i32;
    fn f_sync(fp: *mut FIL) -> i32;
    // `exists`/`mtime`'s oracle probe.
    fn f_stat(path: *const u8, fno: *mut FILINFO) -> i32;
}

const FA_READ: u8 = 0x01;
const FA_WRITE: u8 = 0x02;
const FA_CREATE_ALWAYS: u8 = 0x08;
const FA_OPEN_APPEND: u8 = 0x30;
const AM_DIR: u8 = 0x10;

/// A mounted C-FatFS volume, read + write path.
pub struct CFatFs {
    // Never read directly after `mount()` -- FatFs's internal `FatFs[]`
    // table holds the raw pointer this Box owns, so the field's whole job
    // is keeping that allocation alive (and freeing it, on Drop, only
    // after we've told FatFs to forget the pointer -- see `Drop` below).
    #[allow(dead_code)]
    fs: Box<FATFS>,
}

impl CFatFs {
    /// Mount the volume currently installed via `ram_disk::RamDisk::load`.
    pub fn mount() -> Self {
        let mut fs = Box::new(FATFS { _o: [0; 704] });
        let rc = unsafe { f_mount(&mut *fs, c"".as_ptr().cast::<u8>(), 1) };
        assert_eq!(rc, 0, "f_mount FR={rc}");
        CFatFs { fs }
    }

    /// Read a whole file's contents.
    pub fn read_file(&self, path: &str) -> Vec<u8> {
        let c = std::ffi::CString::new(path).unwrap();
        let mut fp = FIL { _o: [0; 640] };
        assert_eq!(
            unsafe { f_open(&mut fp, c.as_ptr() as *const u8, FA_READ) },
            0
        );
        let mut out = Vec::new();
        let mut buf = [0u8; 4096];
        let mut br = 0u32;
        loop {
            assert_eq!(
                unsafe { f_read(&mut fp, buf.as_mut_ptr(), buf.len() as u32, &mut br) },
                0
            );
            if br == 0 {
                break;
            }
            out.extend_from_slice(&buf[..br as usize]);
        }
        unsafe {
            f_close(&mut fp);
        }
        out
    }

    /// List a directory's entries, sorted by name.
    pub fn read_dir(&self, path: &str) -> Vec<Entry> {
        let c = std::ffi::CString::new(path).unwrap();
        let mut dp = DIR { _o: [0; 128] };
        assert_eq!(unsafe { f_opendir(&mut dp, c.as_ptr() as *const u8) }, 0);
        let mut v = Vec::new();
        loop {
            let mut fno = FILINFO {
                fsize: 0,
                fdate: 0,
                ftime: 0,
                fattrib: 0,
                altname: [0; 13],
                fname: [0; 256],
            };
            assert_eq!(unsafe { f_readdir(&mut dp, &mut fno) }, 0);
            if fno.fname[0] == 0 {
                break; // end of directory
            }
            let end = fno.fname.iter().position(|&b| b == 0).unwrap_or(256);
            v.push(Entry {
                name: String::from_utf8_lossy(&fno.fname[..end]).into_owned(),
                size: fno.fsize as u64,
                is_dir: fno.fattrib & AM_DIR != 0,
            });
        }
        unsafe {
            f_closedir(&mut dp);
        }
        v.sort_by(|a, b| a.name.cmp(&b.name));
        v
    }

    /// Test helper: `read_dir` collapsed to the differential's
    /// comparison shape (`(name, is_dir, size)`), for the enumeration-set
    /// equivalence test against [`EFatFs::readdir_all`](crate::efatfs::EFatFs::readdir_all).
    pub fn readdir_all(&self, path: &str) -> Vec<(String, bool, u32)> {
        self.read_dir(path)
            .into_iter()
            .map(|e| (e.name, e.is_dir, e.size as u32))
            .collect()
    }

    fn zeroed_filinfo() -> FILINFO {
        FILINFO {
            fsize: 0,
            fdate: 0,
            ftime: 0,
            fattrib: 0,
            altname: [0; 13],
            fname: [0; 256],
        }
    }

    /// `f_stat`-backed existence check: the oracle for whether an
    /// efatfs-side create/rename/unlink actually landed on the shared disk,
    /// as seen by the OTHER filesystem implementation. True iff `path`
    /// names an existing file or directory.
    pub fn exists(&self, path: &str) -> bool {
        let c = std::ffi::CString::new(path).unwrap();
        let mut fno = Self::zeroed_filinfo();
        unsafe { f_stat(c.as_ptr() as *const u8, &mut fno) == 0 }
    }

    /// `path`'s packed FAT modified date/time (`(FILINFO::fdate << 16) |
    /// FILINFO::ftime`) — the oracle for `efatfs_core::set_time`'s same
    /// packing. Panics if `path` does not exist.
    pub fn mtime(&self, path: &str) -> u32 {
        let c = std::ffi::CString::new(path).unwrap();
        let mut fno = Self::zeroed_filinfo();
        assert_eq!(
            unsafe { f_stat(c.as_ptr() as *const u8, &mut fno) },
            0,
            "f_stat({path})"
        );
        (u32::from(fno.fdate) << 16) | u32::from(fno.ftime)
    }

    /// Create a directory. `path`'s parent must already exist.
    pub fn mkdir(&mut self, path: &str) {
        let c = std::ffi::CString::new(path).unwrap();
        assert_eq!(
            unsafe { f_mkdir(c.as_ptr() as *const u8) },
            0,
            "f_mkdir({path})"
        );
    }

    /// Open `path` with `mode`, write the whole of `bytes` (looping `f_write`
    /// until it's all landed -- a single RAM-disk write is expected to
    /// finish in one call, but nothing guarantees that), sync, and close.
    fn open_write(&mut self, path: &str, mode: u8, bytes: &[u8]) {
        let c = std::ffi::CString::new(path).unwrap();
        let mut fp = FIL { _o: [0; 640] };
        assert_eq!(
            unsafe { f_open(&mut fp, c.as_ptr() as *const u8, mode) },
            0,
            "f_open({path}, mode={mode:#x})"
        );
        let mut written = 0usize;
        while written < bytes.len() {
            let mut bw = 0u32;
            let rc = unsafe {
                f_write(
                    &mut fp,
                    bytes[written..].as_ptr(),
                    (bytes.len() - written) as u32,
                    &mut bw,
                )
            };
            assert_eq!(rc, 0, "f_write({path})");
            assert!(bw > 0, "f_write({path}) made no progress -- disk full?");
            written += bw as usize;
        }
        assert_eq!(unsafe { f_sync(&mut fp) }, 0, "f_sync({path})");
        unsafe {
            f_close(&mut fp);
        }
    }

    /// Create (or truncate, if it already exists) `path` and write `bytes`
    /// as its whole contents.
    pub fn write_new(&mut self, path: &str, bytes: &[u8]) {
        self.open_write(path, FA_WRITE | FA_CREATE_ALWAYS, bytes);
    }

    /// Open the existing file at `path` and append `bytes`. `FA_OPEN_APPEND`
    /// makes `f_open` itself seek to end-of-file, so no separate `f_lseek`
    /// call (or extern declaration) is needed here.
    pub fn append(&mut self, path: &str, bytes: &[u8]) {
        self.open_write(path, FA_WRITE | FA_OPEN_APPEND, bytes);
    }

    /// Delete an existing file or (empty) directory.
    pub fn delete(&mut self, path: &str) {
        let c = std::ffi::CString::new(path).unwrap();
        assert_eq!(
            unsafe { f_unlink(c.as_ptr() as *const u8) },
            0,
            "f_unlink({path})"
        );
    }

    /// Rename/move `from` to `to`.
    pub fn rename(&mut self, from: &str, to: &str) {
        let cf = std::ffi::CString::new(from).unwrap();
        let ct = std::ffi::CString::new(to).unwrap();
        assert_eq!(
            unsafe { f_rename(cf.as_ptr() as *const u8, ct.as_ptr() as *const u8) },
            0,
            "f_rename({from} -> {to})"
        );
    }
}

impl Drop for CFatFs {
    fn drop(&mut self) {
        // Unmount so a later `mount()` in another test isn't confused by
        // stale FatFs volume-mount-ID state (`fs.id` bump on each mount is
        // how FatFs detects "this FIL/DIR came from a since-unmounted
        // volume").
        unsafe {
            f_mount(std::ptr::null_mut(), c"".as_ptr().cast::<u8>(), 0);
        }
    }
}
