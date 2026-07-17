//! block_device.h + FatFS diskio — SD card over `deluge_bsp::sd` (device) or a
//! file-backed shim (host).
//!
//! The app's FatFS layer (its `disk_read`/`disk_write` shims live in
//! audio_file_manager.cpp) calls the BSP-provided `deluge_block_read`/
//! `deluge_block_write` (block_device.h), plus
//! `disk_initialize`/`disk_status`/`disk_ioctl`/`get_fattime`. On device we back
//! those with deluge_bsp::sd's async SDHI+DMA driver via `block_on` — SD ops
//! complete on the SDHI/DMA-completion IRQ, so `block_on` drives them to
//! completion without needing another task to run.
//!
//! NOTE: `block_on` stalls the executor (and the app tick → audio) for the
//! duration of a transfer. Fine for bring-up (loads aren't real-time); audio-
//! during-storage yielding (storage_wait.h / scheduler) is a later refinement.
//!
//! On host (`target_os` != `"none"`) there is no `deluge_bsp::sd`/SDHI driver —
//! `deluge_block_read`/`deluge_block_write` instead seek+read/write a small
//! backing file (see [`host_disk`]), so the FatFS-shaped C ABI can be exercised
//! (and round-tripped) off-target. No C++ app is linked on host in M1, so the
//! FatFS `disk_*` entry points are otherwise unused there; they're still given
//! working host bodies (rather than gated out) since a future milestone's host
//! FatFS exercise will call them.
#![allow(non_upper_case_globals)]

#[cfg(target_os = "none")]
use core::sync::atomic::{AtomicBool, Ordering};

#[cfg(target_os = "none")]
use deluge_bsp::sd;
#[cfg(target_os = "none")]
use embassy_futures::block_on;

#[cfg(target_os = "none")]
use crate::sys::{
    DelugeCardEvent, DelugeCardEvent_DELUGE_CARD_EVENT_EJECTED as CARD_EJECTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_INSERTED as CARD_INSERTED,
    DelugeCardEvent_DELUGE_CARD_EVENT_NONE as CARD_NONE, DelugeStatus,
    DelugeStatus_DELUGE_ERR_IO as DELUGE_ERR_IO, DelugeStatus_DELUGE_ERR_NODEV as DELUGE_ERR_NODEV,
    DelugeStatus_DELUGE_ERR_WRITE_PROTECTED as DELUGE_ERR_WRITE_PROTECTED,
    DelugeStatus_DELUGE_OK as DELUGE_OK,
};

// Host stand-ins for the bindgen `crate::sys` types used below (`block_device.h`
// / `types.h`). `mod sys` is device-only — no C++ ABI is linked on host in M1 —
// so mirror the C types' shapes directly here (same convention as fiber.rs's
// `RunCondition`): `DelugeStatus` is `i8` (its values run -13..=0, the width
// clang's `-fshort-enums` would pick for the armv7a app), `DelugeCardEvent` is
// `u8` (0..=2). Only the variants this file actually returns are named.
#[cfg(not(target_os = "none"))]
type DelugeStatus = i8;
#[cfg(not(target_os = "none"))]
const DELUGE_OK: DelugeStatus = 0;
#[cfg(not(target_os = "none"))]
const DELUGE_ERR_IO: DelugeStatus = -5;
#[cfg(not(target_os = "none"))]
const DELUGE_ERR_NODEV: DelugeStatus = -6;

#[cfg(not(target_os = "none"))]
type DelugeCardEvent = u8;
#[cfg(not(target_os = "none"))]
const CARD_NONE: DelugeCardEvent = 0;

// FatFS diskio status/result codes (src/fatfs/diskio.h).
const STA_NOINIT: u8 = 0x01;
const STA_NODISK: u8 = 0x02;
const STA_PROTECT: u8 = 0x04;
const RES_OK: i32 = 0;
const RES_ERROR: i32 = 1;
const RES_WRPRT: i32 = 2;
const RES_NOTRDY: i32 = 3;
const RES_PARERR: i32 = 4;
const CTRL_SYNC: u8 = 0;
const GET_SECTOR_COUNT: u8 = 1;
const GET_SECTOR_SIZE: u8 = 2;
const GET_BLOCK_SIZE: u8 = 3;
const SECTOR_SIZE: usize = 512;

/// Set once `disk_initialize` has driven `sd::init()` at least once. Until then,
/// the controller has never muxed `SD_CD` (P7_0 → SDHI fn 3) or started `SD_CLK`,
/// so `sd::is_inserted()` reads a meaningless card-detect line. We must NOT report
/// `STA_NODISK` from that pre-init read: FatFS skips `disk_initialize` whenever the
/// disk looks absent, and `disk_initialize` is the only path that brings the detect
/// circuit to life — detection would be gated on init, and init on detection
/// (deadlock). Report "not initialized, presence unknown" instead so FatFS attempts
/// the init that makes card-detect valid. Device-only: the host shim has no
/// controller bring-up step (see [`host_disk`]).
#[cfg(target_os = "none")]
static CONTROLLER_UP: AtomicBool = AtomicBool::new(false);

/// Bring the SD controller up from an async (Embassy task) context, once at boot.
///
/// `sd::init()` sequences card power-up with `embassy_time::Timer` delays. This BSP
/// uses the *integrated* (intrusive) timer queue, so a `Timer` only resolves when
/// polled by a real Embassy task — `embassy_futures::block_on`'s synthetic waker
/// makes `Timer` panic (`from_embassy_waker`). The sync FatFS `disk_initialize`
/// path can therefore never run `init()`. Instead we init here, from `app_task`,
/// before `deluge_app_init` drives the first C++ storage access: afterwards the card
/// reports ready, FatFS skips `disk_initialize`, and reads/writes stay on `block_on`
/// (pure SDHI/DMA-IRQ futures — no timers). Re-runnable for card-swap from async.
/// Device-only: `app_task` (this fn's sole caller) is itself device-only.
#[cfg(target_os = "none")]
pub async fn boot_init() {
    let r = sd::init().await;
    // The pin mux + SDHI bring-up has run, so card-detect is meaningful now.
    CONTROLLER_UP.store(true, Ordering::Release);
    log::info!(
        "sd: boot_init — init={:?} inserted={} wp={}",
        r.is_ok(),
        sd::is_inserted(),
        sd::is_write_protected(),
    );
}

/// FatFS DSTATUS bits for the current card state. Device-only (see [`sd`]).
#[cfg(target_os = "none")]
fn status_bits() -> u8 {
    // DIAG (SD bring-up): log the raw card-detect/ready state the first few times
    // status is queried, so "No SD card present" can be traced to is_inserted()
    // vs. is_ready() vs. init failure.
    {
        static DIAG_LEFT: AtomicBool = AtomicBool::new(true);
        if DIAG_LEFT.swap(false, Ordering::Relaxed) {
            log::info!(
                "DIAG sd: status_bits — inserted={} ready={} wp={}",
                sd::is_inserted(),
                sd::is_ready(),
                sd::is_write_protected(),
            );
        }
    }
    if !sd::is_ready() {
        // Only trust the card-detect read once the controller has been brought up.
        let absent = CONTROLLER_UP.load(Ordering::Acquire) && !sd::is_inserted();
        return STA_NOINIT | if absent { STA_NODISK } else { 0 };
    }
    if sd::is_write_protected() {
        STA_PROTECT
    } else {
        0
    }
}

#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn disk_initialize(pdrv: u8) -> u8 {
    if pdrv != 0 {
        return STA_NOINIT;
    }
    // sd::init() can't run here: its embassy-time Timers panic under `block_on`
    // (integrated timer queue — see `boot_init`). It runs eagerly from `app_task`
    // (boot_init) after the PIC baud handshake (pic::wait_ready) and before
    // deluge_app_init — waiting for the PIC first avoids the handshake race that
    // corrupted the pads. So the card is ready by the time the app first calls this;
    // if somehow not, just report status (never block_on(init) here).
    if !sd::is_ready() {
        log::warn!("sd: disk_initialize before async init done");
    }
    status_bits()
}

#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn disk_status(pdrv: u8) -> u8 {
    if pdrv != 0 {
        return STA_NOINIT;
    }
    status_bits()
}

/// Host: no controller bring-up or card-detect exists — the backing file (opened
/// lazily by [`host_disk`]) is ready as soon as it can be opened/sized, so both
/// FatFS DSTATUS entry points just report "ready" for unit 0.
#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn disk_initialize(pdrv: u8) -> u8 {
    if pdrv != 0 {
        return STA_NOINIT;
    }
    host_disk::ensure_open();
    0
}

#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn disk_status(pdrv: u8) -> u8 {
    if pdrv != 0 {
        return STA_NOINIT;
    }
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn disk_ioctl(pdrv: u8, cmd: u8, buff: *mut core::ffi::c_void) -> i32 {
    if pdrv != 0 {
        return RES_NOTRDY;
    }
    match cmd {
        // Writes complete synchronously (block_on / a direct host file write), so
        // nothing is ever pending.
        CTRL_SYNC => RES_OK,
        GET_SECTOR_COUNT => {
            if buff.is_null() {
                return RES_PARERR;
            }
            #[cfg(target_os = "none")]
            let sectors = sd::total_sectors();
            #[cfg(not(target_os = "none"))]
            let sectors = host_disk::total_sectors();
            unsafe { *(buff as *mut u32) = sectors };
            RES_OK
        }
        GET_SECTOR_SIZE => {
            if buff.is_null() {
                return RES_PARERR;
            }
            unsafe { *(buff as *mut u16) = SECTOR_SIZE as u16 };
            RES_OK
        }
        GET_BLOCK_SIZE => {
            if buff.is_null() {
                return RES_PARERR;
            }
            unsafe { *(buff as *mut u32) = 1 };
            RES_OK
        }
        _ => RES_PARERR,
    }
}

// NOTE: SD I/O must use `block_on` (which parks the executor for
// the transfer), NOT a fiber-yielding drive. FatFS is not re-entrant and the app's
// SD-reentrancy guard (`currentlyAccessingCard`) is only set by the legacy C diskio
// (src/RZA1/diskio.c), which this BSP does not link — so on this BSP it is always 0.
// Parking during the transfer is what serializes SD access; yielding mid-transfer
// (block_on_fiber) let other tasks re-enter FatFS and corrupted it (manifested as
// "NO MORE PRESETS FOUND" on track create, and would also break song/sample loads).
// Re-introducing fiber-aware SD requires first serializing all SD access on this BSP.
#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_read(
    unit: u8,
    dst: *mut u8,
    sector: u32,
    count: u32,
) -> DelugeStatus {
    if unit != 0 || !sd::is_ready() {
        return DELUGE_ERR_NODEV;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `dst` holds `count` sectors.
    let out = unsafe { core::slice::from_raw_parts_mut(dst, len) };
    match block_on(sd::read_sectors(sector, count, out)) {
        Ok(()) => DELUGE_OK,
        Err(_) => DELUGE_ERR_IO,
    }
}

#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_write(
    unit: u8,
    src: *const u8,
    sector: u32,
    count: u32,
) -> DelugeStatus {
    if unit != 0 || !sd::is_ready() {
        return DELUGE_ERR_NODEV;
    }
    if sd::is_write_protected() {
        return DELUGE_ERR_WRITE_PROTECTED;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `src` holds `count` sectors.
    let data = unsafe { core::slice::from_raw_parts(src, len) };
    match block_on(sd::write_sectors(sector, count, data)) {
        Ok(()) => DELUGE_OK,
        Err(e) => {
            log::warn!("deluge_block_write err {e:?} (sector={sector} count={count})");
            DELUGE_ERR_IO
        }
    }
}

/// Host shim: seek+read `count` sectors from the backing file (see [`host_disk`]).
/// No card-detect/write-protect concept on host — `DELUGE_ERR_NODEV` is only for
/// a non-zero unit; I/O errors (short read, file-open failure) map to
/// `DELUGE_ERR_IO`.
#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_read(
    unit: u8,
    dst: *mut u8,
    sector: u32,
    count: u32,
) -> DelugeStatus {
    if unit != 0 {
        return DELUGE_ERR_NODEV;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `dst` holds `count` sectors.
    let out = unsafe { core::slice::from_raw_parts_mut(dst, len) };
    match host_disk::read_sectors(sector, out) {
        Ok(()) => DELUGE_OK,
        Err(e) => {
            log::warn!("deluge_block_read(host) err {e} (sector={sector} count={count})");
            DELUGE_ERR_IO
        }
    }
}

/// Host shim: seek+write `count` sectors to the backing file (see [`host_disk`]).
#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_write(
    unit: u8,
    src: *const u8,
    sector: u32,
    count: u32,
) -> DelugeStatus {
    if unit != 0 {
        return DELUGE_ERR_NODEV;
    }
    let len = count as usize * SECTOR_SIZE;
    // SAFETY: caller guarantees `src` holds `count` sectors.
    let data = unsafe { core::slice::from_raw_parts(src, len) };
    match host_disk::write_sectors(sector, data) {
        Ok(()) => DELUGE_OK,
        Err(e) => {
            log::warn!("deluge_block_write(host) err {e} (sector={sector} count={count})");
            DELUGE_ERR_IO
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn disk_timerproc() {
    // SD is fully event-driven (SDHI/DMA IRQs); no periodic timer work needed.
}

#[unsafe(no_mangle)]
pub extern "C" fn get_fattime() -> u32 {
    // 2024-01-01 00:00:00, FAT-packed. No RTC on this BSP yet.
    ((2024u32 - 1980) << 25) | (1 << 21) | (1 << 16)
}

// ── block_device.h: SD unit + card-detect ─────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_sd_unit() -> u8 {
    0
}

/// Pull-based card-detect: report INSERTED/EJECTED edges of the card-present
/// state. The first poll just latches the boot state (no spurious event), so the
/// app's initial card read isn't double-triggered (mirrors the rza1 latch).
#[cfg(target_os = "none")]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_poll_card_event(unit: u8) -> DelugeCardEvent {
    if unit != 0 {
        return CARD_NONE;
    }
    static INITIALISED: AtomicBool = AtomicBool::new(false);
    static LAST_INSERTED: AtomicBool = AtomicBool::new(false);
    let now = sd::is_inserted();
    if !INITIALISED.swap(true, Ordering::Relaxed) {
        LAST_INSERTED.store(now, Ordering::Relaxed);
        return CARD_NONE;
    }
    let was = LAST_INSERTED.swap(now, Ordering::Relaxed);
    if now == was {
        CARD_NONE
    } else if now {
        CARD_INSERTED
    } else {
        CARD_EJECTED
    }
}

/// Host: the backing file has no card-detect line and is present for the whole
/// process lifetime, so there is never an insert/eject edge to report.
#[cfg(not(target_os = "none"))]
#[unsafe(no_mangle)]
pub extern "C" fn deluge_block_poll_card_event(_unit: u8) -> DelugeCardEvent {
    CARD_NONE
}

/// File-backed block device (host only): the `deluge_block_read`/
/// `deluge_block_write` shim's actual storage. A single lazily-opened,
/// lazily-sized file stands in for the SD card; `sector` maps directly to a
/// `SECTOR_SIZE`-byte offset. Path is overridable via `DELUGE_SD_IMAGE` (default:
/// a temp file), so a test run can point at its own throwaway image.
#[cfg(not(target_os = "none"))]
mod host_disk {
    use std::fs::{File, OpenOptions};
    use std::io::{self, Read, Seek, SeekFrom, Write};
    use std::path::PathBuf;
    use std::sync::{Mutex, OnceLock};

    use super::SECTOR_SIZE;

    /// Small volume: enough for host bring-up/round-trip checks, cheap to
    /// allocate (sparse on any filesystem that supports holes).
    const DEFAULT_SECTORS: u64 = 16 * 1024; // 8 MiB

    struct Disk {
        file: File,
        sectors: u32,
    }

    static DISK: OnceLock<Mutex<Disk>> = OnceLock::new();

    fn image_path() -> PathBuf {
        std::env::var_os("DELUGE_SD_IMAGE")
            .map(PathBuf::from)
            .unwrap_or_else(|| std::env::temp_dir().join("deluge-host-sd.img"))
    }

    fn open_or_create() -> Mutex<Disk> {
        let path = image_path();
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .open(&path)
            .unwrap_or_else(|e| panic!("sd(host): failed to open backing file {path:?}: {e}"));
        let want_len = DEFAULT_SECTORS * SECTOR_SIZE as u64;
        let len = file
            .metadata()
            .unwrap_or_else(|e| panic!("sd(host): failed to stat backing file {path:?}: {e}"))
            .len();
        if len < want_len {
            file.set_len(want_len).unwrap_or_else(|e| {
                panic!("sd(host): failed to size backing file {path:?} to {want_len} bytes: {e}")
            });
        }
        let sectors = (want_len.max(len) / SECTOR_SIZE as u64) as u32;
        log::info!(
            "sd(host): backing file {} ({sectors} sectors)",
            path.display()
        );
        Mutex::new(Disk { file, sectors })
    }

    fn disk() -> &'static Mutex<Disk> {
        DISK.get_or_init(open_or_create)
    }

    /// Open (and create/size, if needed) the backing file now, rather than
    /// lazily on first read/write. Called from `disk_initialize`.
    pub fn ensure_open() {
        let _ = disk();
    }

    pub fn total_sectors() -> u32 {
        disk().lock().unwrap().sectors
    }

    pub fn read_sectors(sector: u32, out: &mut [u8]) -> io::Result<()> {
        let mut d = disk().lock().unwrap();
        d.file
            .seek(SeekFrom::Start(sector as u64 * SECTOR_SIZE as u64))?;
        d.file.read_exact(out)
    }

    pub fn write_sectors(sector: u32, data: &[u8]) -> io::Result<()> {
        let mut d = disk().lock().unwrap();
        d.file
            .seek(SeekFrom::Start(sector as u64 * SECTOR_SIZE as u64))?;
        d.file.write_all(data)
    }
}
