//! A single Rust-owned in-RAM disk image, plus the five `disk_*` low-level
//! I/O callbacks and `get_fattime` that the vendored C FatFS (compiled by
//! `build.rs`) links against (see `src/fatfs/diskio.h` for the exact
//! prototypes these mirror).
//!
//! Only one image is live at a time (`DISK` is a single process-wide slot),
//! matching the harness's single-threaded, single-volume (`FF_VOLUMES=1`)
//! use: load an image, drive C FatFS against it, done.
use std::sync::Mutex;

static DISK: Mutex<Vec<u8>> = Mutex::new(Vec::new());
const SS: usize = 512; // FF_MIN_SS == FF_MAX_SS == 512 (see ffconf.h)

pub struct RamDisk;

impl RamDisk {
    /// Load a card image file into the shared in-RAM disk, replacing
    /// whatever was there before.
    pub fn load(image_path: &str) -> Self {
        Self::load_bytes(&std::fs::read(image_path).expect("read image"))
    }

    /// Load raw image bytes into the shared in-RAM disk, replacing whatever
    /// was there before. Used by the write-path differential
    /// (`diff::replay_and_compare`) to give each backend its OWN fresh copy
    /// of the same starting fixture -- write ops mutate the shared `DISK`,
    /// so C FatFS and embedded-fatfs must never run against one image at
    /// the same time (see `tests/differential.rs`'s `TEST_LOCK`).
    pub fn load_bytes(bytes: &[u8]) -> Self {
        *DISK.lock().unwrap() = bytes.to_vec();
        RamDisk
    }

    /// A snapshot copy of the current disk contents, for differential
    /// before/after comparisons.
    pub fn snapshot(&self) -> Vec<u8> {
        DISK.lock().unwrap().clone()
    }

    /// Byte-granular read from the shared image into `buf`, starting at byte
    /// offset `pos`. Returns the number of bytes actually copied (`0` once
    /// `pos` is at or past the end of the image -- EOF). Thin wrapper over
    /// the same `DISK` mutex `disk_read` uses, so the C FatFS bridge
    /// (sector-granular, via `disk_read`) and embedded-fatfs (via
    /// `block_dev::FileBlockDevice` → `BufStream`) both see one image.
    pub fn read_at(pos: u64, buf: &mut [u8]) -> usize {
        let d = DISK.lock().unwrap();
        let pos = pos as usize;
        if pos >= d.len() {
            return 0;
        }
        let n = buf.len().min(d.len() - pos);
        buf[..n].copy_from_slice(&d[pos..pos + n]);
        n
    }

    /// Byte-granular write into the shared image at byte offset `pos`,
    /// growing the image (zero-filled) if the write runs past its current
    /// end. Same `DISK` mutex as `read_at`/`disk_write`.
    pub fn write_at(pos: u64, buf: &[u8]) {
        let mut d = DISK.lock().unwrap();
        let pos = pos as usize;
        let end = pos + buf.len();
        if end > d.len() {
            d.resize(end, 0);
        }
        d[pos..end].copy_from_slice(buf);
    }

    /// Current size of the shared image, in bytes.
    pub fn len() -> u64 {
        DISK.lock().unwrap().len() as u64
    }
}

/// The vendored `src/fatfs/ff.c` bakes in a Deluge-specific extern global
/// (`create_chain()` resets it after growing a file's cluster chain -- see
/// ff.c:1507/1564; the app side owns it in
/// `src/deluge/playback/playback_handler.cpp`). The read-only path this
/// harness drives never touches it at runtime, but the C linker still
/// requires the symbol to exist because `create_chain()` is compiled into
/// the same translation unit as the functions we do call.
#[allow(non_upper_case_globals)]
#[no_mangle]
pub static mut pendingGlobalMIDICommandNumClustersWritten: i32 = 0;

#[no_mangle]
pub extern "C" fn disk_status(_pdrv: u8) -> u8 {
    0 // no STA_* bits set: always ready
}

#[no_mangle]
pub extern "C" fn disk_initialize(_pdrv: u8) -> u8 {
    0
}

#[no_mangle]
pub extern "C" fn get_fattime() -> u32 {
    // Fixed 2024-01-01 00:00:00 stamp, matching host firmware behavior.
    // FAT time-stamp packing: ((Y-1980)<<25)|(M<<21)|(D<<16)|(h<<11)|(m<<5)|(s>>1)
    ((2024u32 - 1980) << 25) | (1 << 21) | (1 << 16)
}

/// # Safety
/// `buff` must be valid for writes of `count * SS` bytes -- upheld by C
/// FatFS, the only caller (it always passes its own `win`/file sector
/// buffer, sized `FF_MAX_SS`, alongside a `count` that fits it).
#[no_mangle]
pub unsafe extern "C" fn disk_read(_pdrv: u8, buff: *mut u8, sector: u32, count: u32) -> i32 {
    let d = DISK.lock().unwrap();
    let off = sector as usize * SS;
    let len = count as usize * SS;
    std::ptr::copy_nonoverlapping(d[off..off + len].as_ptr(), buff, len);
    0 // RES_OK
}

/// # Safety
/// `buff` must be valid for reads of `count * SS` bytes -- same caller
/// contract as `disk_read`.
#[no_mangle]
pub unsafe extern "C" fn disk_write(_pdrv: u8, buff: *const u8, sector: u32, count: u32) -> i32 {
    let mut d = DISK.lock().unwrap();
    let off = sector as usize * SS;
    let len = count as usize * SS;
    std::ptr::copy_nonoverlapping(buff, d[off..off + len].as_mut_ptr(), len);
    0 // RES_OK
}

#[no_mangle]
pub extern "C" fn disk_ioctl(_pdrv: u8, cmd: u8, buff: *mut core::ffi::c_void) -> i32 {
    // CTRL_SYNC=0, GET_SECTOR_COUNT=1, GET_SECTOR_SIZE=2, GET_BLOCK_SIZE=3
    // (src/fatfs/diskio.h). Only CTRL_SYNC is actually required by ff.c under
    // this harness's ffconf (FF_FS_READONLY=0 needs it; GET_SECTOR_COUNT and
    // GET_BLOCK_SIZE are only needed when FF_USE_MKFS=1, and GET_SECTOR_SIZE
    // only when FF_MAX_SS != FF_MIN_SS -- neither holds here) but all four
    // are implemented for robustness.
    match cmd {
        0 => 0, // CTRL_SYNC
        1 => {
            // GET_SECTOR_COUNT
            unsafe {
                *(buff as *mut u32) = (DISK.lock().unwrap().len() / SS) as u32;
            }
            0
        }
        2 => {
            // GET_SECTOR_SIZE
            unsafe {
                *(buff as *mut u16) = SS as u16;
            }
            0
        }
        3 => {
            // GET_BLOCK_SIZE (erase block size, in sectors; 1 = no preference)
            unsafe {
                *(buff as *mut u32) = 1;
            }
            0
        }
        _ => 0,
    }
}
