//! Safe wrapper over the vendored C FatFS (compiled by `build.rs`), read
//! path only.
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
}

const FA_READ: u8 = 0x01;
const AM_DIR: u8 = 0x10;

/// A mounted C-FatFS volume, read path only.
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
