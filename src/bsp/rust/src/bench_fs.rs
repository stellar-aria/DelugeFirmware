//! SP1 Task 4: on-device read-throughput benchmark, `embedded-fatfs` vs the
//! vendored C FatFS — both ultimately bottoming out at `deluge_bsp::sd` (the
//! real SDHI1 DMA driver), so the comparison isolates the two filesystem
//! *implementations'* overhead rather than the hardware underneath them.
//!
//! **NEEDS-HARDWARE.** This module only proves the comparison is wired up
//! correctly and cross-compiles for device — the MB/s numbers it prints are
//! only meaningful from a real on-device run (no host proxy substitutes: the
//! whole point is genuine SDHI/DMA timing, which no simulator models). See
//! `.superpowers/sdd/task-4-brief.md` / `task-4-report.md`.
//!
//! Non-default: gated behind the `bench_fs` cargo feature (see `Cargo.toml`),
//! so a normal firmware build never links or runs this. Enabled, it still
//! produces a normally-flashable image: [`run`] executes once at boot (from
//! `main.rs`'s `app_task`, after the SD block driver is up but before
//! `deluge_app_init`), prints its result line, and then boot continues
//! exactly as usual.
//!
//! # The two read paths
//!
//! - **efatfs**: `embedded_fatfs::FileSystem` over
//!   `BufStream<`[`SdBlockDevice`](crate::fat_block_device::SdBlockDevice)`, 512>`
//!   — the exact device storage stack Task 3 built and Task 2/3's
//!   `fs_differential` harness already exercises on host. `BufStream`
//!   buffers exactly one 512-byte block internally (see its module doc) —
//!   there is no multi-block readahead knob in this vendored
//!   `block_device_adapters` fork to expose.
//! - **cfatfs**: the vendored `src/fatfs` C library's raw `f_open`/`f_read`
//!   C ABI, called directly (see [`cfatfs`]) — the same library, same
//!   `ffconf.h`, the C++ app links and the firmware's own
//!   `StorageManager`/`Filesystem` C++ wrapper (`src/fatfs/fatfs.cpp`) calls
//!   through, just without going via that C++ wrapper (so this benchmark
//!   doesn't need to boot the whole app first).
//!
//! Both reads are sequential (never concurrent — the C path only starts once
//! the efatfs path has finished and its `FileSystem` has been dropped), same
//! target file, same-size read-into-scratch-buffer loop, timed with
//! `embassy_time::Instant` — a fair apples-to-apples comparison of the two
//! filesystem implementations' overhead on top of the identical underlying
//! transfer.
#![cfg(all(target_os = "none", feature = "bench_fs"))]

extern crate alloc;

use alloc::{ffi::CString, string::String};

use block_device_adapters::BufStream;
use embassy_time::{Duration, Instant};
use embedded_fatfs::{DefaultTimeProvider, FileSystem, FsOptions, LossyOemCpConverter};
use embedded_io_async::Read as _;

use crate::fat_block_device::SdBlockDevice;

type Efatfs = FileSystem<BufStream<SdBlockDevice, 512>, DefaultTimeProvider, LossyOemCpConverter>;

/// Backing arena for `crate::FS_ALLOCATOR` (see `main.rs`'s doc comment on
/// that static). `embedded-fatfs`'s `alloc` feature needs a live heap before
/// its first allocation — Task 3 registered the global allocator but left it
/// UNINITIALISED (an allocation through a null handle returns null rather
/// than UB, but nothing that actually needs `alloc` can run correctly until
/// `.init()` is called with a real backing arena). This benchmark is the
/// first thing in this crate that needs `alloc` (LFN directory-scan scratch,
/// this module's own `String`/`CString` use), so it owns that one-time init.
///
/// 96 KiB: generous headroom over what a directory scan of a few hundred
/// entries plus one path string plausibly needs, sized the same order as the
/// existing `RUST_SRAM_POOL` (main.rs) and well within the RZ/A1L's 2.875 MB
/// on-chip SRAM (see `linker/memory_rtt.x`) — plain `.bss`, no special
/// section placement needed for a feature-gated benchmark tool.
const FS_ARENA_SIZE: usize = 96 * 1024;
static mut FS_ARENA: [u8; FS_ARENA_SIZE] = [0; FS_ARENA_SIZE];

/// Read-buffer size shared by BOTH read paths, for a fair comparison: same
/// chunk size, same sequential-read shape, same target file. Block-aligned
/// (a multiple of 512) so the efatfs path's `BufStream` takes its direct
/// block-aligned fast path (see `block_device_adapters::BufStream`'s doc)
/// rather than its copy-through-cache path — the more representative
/// comparison against the C path's own `FF_MAX_SS`(512)-at-a-time internal
/// window.
const READ_CHUNK: usize = 4096;

/// Raw C-FatFS FFI: `f_mount`/`f_open`/`f_read`/`f_close` against the
/// vendored `src/fatfs/ff.h` (same library + same `ffconf.h` the C++ app
/// links), called directly instead of through the C++ `Filesystem` wrapper.
///
/// `Fatfs`/`Fil` are opaque, exact-size-and-align byte blobs — FatFs only
/// ever reads/writes within the real struct's bounds, so as long as the size
/// and alignment are AT LEAST the real ones this is sound (undersized or
/// under-aligned would not be). Unlike `fs_differential/src/fatfs_c.rs`'s
/// HOST-only guard structs (which build under `-DDELUGE_HOST`, where
/// `ff.h`'s `FF_CACHE_ALIGN` is a no-op — no DMA/cache on host, see that
/// macro's own comment), these are the real DEVICE sizes: `FIL`/`FATFS` both
/// carry a `FF_CACHE_ALIGN` (`__attribute__((aligned(32)))`) member (the
/// `buf`/`win` DMA/cache windows) on the actual arm-eabi build, which forces
/// 32-byte alignment (and matching size padding) onto the whole struct.
///
/// These exact numbers were NOT guessed or ported from the host values —
/// they were measured directly by cross-compiling a `sizeof`/`alignof` probe
/// against the real `src/fatfs/ff.h` with this repo's own
/// `toolchain/v25/linux-x86_64/arm-none-eabi-gcc` under the same
/// `ARCH_FLAGS` (`-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
/// -mthumb-interwork -mlittle-endian`) `scripts/cmake/CMakeToolchainDeluge.cmake`
/// passes the firmware build (arm-eabi's default short-enums ABI applies
/// either way here — none of `FATFS`/`FIL` embed an enum field, so it can't
/// affect their layout):
///   `sizeof(FATFS) == 640, alignof(FATFS) == 32`
///   `sizeof(FIL)   == 576, alignof(FIL)   == 32`
mod cfatfs {
    // Oversized opaque guards (>= the measured device sizeof above), mirroring
    // `fs_differential/src/fatfs_c.rs`'s "generous headroom" convention: FatFS only
    // writes within the real struct, so a larger guard is always safe, and the margin
    // means a future `ff.h`/`ffconf.h` change can't silently make these undersized (UB)
    // without a re-probe. Both remain multiples of 32 to preserve `align(32)`.
    #[repr(C, align(32))]
    pub struct Fatfs(pub [u8; 704]); // >= measured sizeof(FATFS)==640 on device (22*32)
    #[repr(C, align(32))]
    pub struct Fil(pub [u8; 640]); // >= measured sizeof(FIL)==576 on device (20*32)

    // Edition 2024: extern blocks must be `unsafe`. Mirrors the exact
    // signatures `fs_differential/src/fatfs_c.rs`'s host harness declares
    // (same vendored `ff.h`), linked here against the device build's
    // `libfatfs.a` (archived by `build.rs`, see its `("src/fatfs",
    // "libfatfs.a")` linker-input entry) instead of a host one.
    unsafe extern "C" {
        pub fn f_mount(fs: *mut Fatfs, path: *const u8, opt: u8) -> i32;
        pub fn f_open(fp: *mut Fil, path: *const u8, mode: u8) -> i32;
        pub fn f_read(fp: *mut Fil, buff: *mut u8, btr: u32, br: *mut u32) -> i32;
        pub fn f_close(fp: *mut Fil) -> i32;
    }

    pub const FA_READ: u8 = 0x01;
}

/// Install [`crate::FS_ALLOCATOR`]'s backing heap over [`FS_ARENA`]. Call
/// once, before any Rust `alloc` use in this crate (see both statics' doc
/// comments).
fn init_allocator() {
    // SAFETY: `FS_ARENA` is a crate-private static this module alone ever
    // touches; this runs at most once (from `run`, itself called at most
    // once from `app_task`, before anything else here uses `alloc`).
    // `addr_of_mut!` + `size_of_val(&*p)` (not `&mut FS_ARENA` directly)
    // mirrors the exact idiom `main.rs` already uses for `RUST_SRAM_POOL`,
    // avoiding a `static_mut_refs` reference to the static itself.
    let (base, size) = unsafe {
        let p = core::ptr::addr_of_mut!(FS_ARENA);
        (p.cast::<u8>(), core::mem::size_of_val(&*p))
    };
    // SAFETY: `base`/`size` describe that same live, exclusively-owned
    // 'static arena; `deluge_heap_create` only ever writes within
    // `[base, base+size)`.
    let handle = unsafe { fs_alloc::deluge_heap_create(base, size) };
    assert!(
        !handle.is_null(),
        "bench_fs: {FS_ARENA_SIZE}-byte FS_ARENA too small for a DelugeHeap control block"
    );
    crate::FS_ALLOCATOR.init(handle);
    log::info!(
        "bench_fs: FS_ALLOCATOR initialised over a {} KiB arena",
        FS_ARENA_SIZE / 1024
    );
}

/// Binary MB/s (MiB/s, bytes / 2^20 / seconds) — the usual convention for
/// storage-throughput numbers, not decimal (bytes / 1e6). `elapsed == 0`
/// (a transfer somehow completing within a single clock tick) reports `0.0`
/// instead of dividing by zero; shouldn't happen for a real multi-KB SD
/// read, but would be an obvious, loud tell if the timer wiring were ever
/// wrong, rather than a silent NaN/Inf in the log line.
fn mb_per_s(bytes: u64, elapsed: Duration) -> f64 {
    let micros = elapsed.as_micros();
    if micros == 0 {
        return 0.0;
    }
    let secs = micros as f64 / 1_000_000.0;
    (bytes as f64 / (1024.0 * 1024.0)) / secs
}

/// Scan `SAMPLES/` (falling back to the root directory if that doesn't
/// exist — keeps this working on a card that doesn't use the Deluge's usual
/// layout) for the largest regular file, non-recursively. Returns its
/// FatFS-relative path (no leading `/`, matching the app's own path
/// convention — see e.g. `audio_file_manager.h`'s `"SAMPLES/CLIPS"`) and
/// size.
async fn find_largest_file(fs: &Efatfs) -> Option<(String, u64)> {
    let root = fs.root_dir();
    let (dir, prefix) = match root.open_dir("SAMPLES").await {
        Ok(d) => (d, "SAMPLES/"),
        Err(_) => (root, ""),
    };
    let mut iter = dir.iter();
    let mut best: Option<(String, u64)> = None;
    while let Some(entry) = iter.next().await {
        let Ok(entry) = entry else { continue };
        if entry.is_dir() {
            continue;
        }
        let size = entry.len();
        let bigger = best
            .as_ref()
            .map_or(true, |(_, best_size)| size > *best_size);
        if bigger {
            best = Some((entry.file_name(), size));
        }
    }
    best.map(|(name, size)| {
        let mut full = String::from(prefix);
        full.push_str(&name);
        (full, size)
    })
}

/// Read `path` sequentially through `embedded-fatfs`, timing the whole
/// open-to-EOF loop. Returns `(bytes_read, MiB/s)`.
async fn efatfs_bench_read(fs: &Efatfs, path: &str) -> Option<(u64, f64)> {
    let root = fs.root_dir();
    let mut file = match root.open_file(path).await {
        Ok(f) => f,
        Err(e) => {
            log::error!("bench_fs: efatfs open_file({path:?}) failed: {e:?}");
            return None;
        }
    };
    let mut buf = [0u8; READ_CHUNK];
    let start = Instant::now();
    let mut total: u64 = 0;
    loop {
        let n = match file.read(&mut buf).await {
            Ok(n) => n,
            Err(e) => {
                log::error!("bench_fs: efatfs read({path:?}) failed: {e:?}");
                return None;
            }
        };
        if n == 0 {
            break;
        }
        total += n as u64;
    }
    let elapsed = start.elapsed();
    Some((total, mb_per_s(total, elapsed)))
}

/// Read `path` sequentially through the raw C FatFS ABI, timing the
/// open-to-EOF loop. Returns `(bytes_read, MiB/s)`.
///
/// Mounts its own local `Fatfs`, reads, then explicitly UNMOUNTS
/// (`f_mount(null, "", 0)`) before returning. This matters: FatFs keeps a
/// single global table (`FatFs[FF_VOLUMES]` in `ff.c`) mapping each logical
/// drive to whichever `FATFS*` was last mounted onto it. If we left our
/// local `fs` mounted, that table slot would keep pointing at this
/// function's stack frame after it returns — a dangling pointer the C++
/// app's own later `StorageManager::initSD()` (which mounts its own,
/// differently-addressed `fileSystem` global, `src/fatfs/fatfs.cpp`) would
/// either dereference or silently clobber. Unmounting first guarantees the
/// app's own subsequent mount starts clean either way — the same reasoning
/// `fs_differential::CFatFs`'s `Drop` documents for the host harness.
fn cfatfs_bench_read(path: &str) -> Option<(u64, f64)> {
    let mut fs = cfatfs::Fatfs([0; 704]);
    // SAFETY: `fs` is a freshly zeroed, correctly sized+aligned `FATFS` blob
    // (see `cfatfs`'s module doc); `f_mount` initialises it in place. `path`
    // "" + `opt` 1 mounts the sole logical drive (`FF_VOLUMES == 1`)
    // immediately, matching `fs_differential::CFatFs::mount`'s call shape.
    let rc = unsafe { cfatfs::f_mount(&mut fs, c"".as_ptr().cast::<u8>(), 1) };
    if rc != 0 {
        log::error!("bench_fs: cfatfs f_mount failed FR={rc}");
        return None;
    }

    let Ok(cpath) = CString::new(path) else {
        log::error!("bench_fs: cfatfs path {path:?} has an interior NUL");
        // SAFETY: unmount — see this fn's doc comment.
        unsafe { cfatfs::f_mount(core::ptr::null_mut(), c"".as_ptr().cast::<u8>(), 0) };
        return None;
    };

    let mut fp = cfatfs::Fil([0; 640]);
    // SAFETY: `fp` is a freshly zeroed, correctly sized+aligned `FIL` blob;
    // `f_open` initialises it in place on success.
    let rc = unsafe { cfatfs::f_open(&mut fp, cpath.as_ptr().cast::<u8>(), cfatfs::FA_READ) };
    if rc != 0 {
        log::error!("bench_fs: cfatfs f_open({path:?}) failed FR={rc}");
        // SAFETY: unmount — see this fn's doc comment.
        unsafe { cfatfs::f_mount(core::ptr::null_mut(), c"".as_ptr().cast::<u8>(), 0) };
        return None;
    }

    let mut buf = [0u8; READ_CHUNK];
    let start = Instant::now();
    let mut total: u64 = 0;
    loop {
        let mut br: u32 = 0;
        // SAFETY: `fp` is a live, just-opened `FIL`; `buf` is a valid
        // mutable buffer of its own declared length — the exact call shape
        // `f_read` expects.
        let rc = unsafe { cfatfs::f_read(&mut fp, buf.as_mut_ptr(), buf.len() as u32, &mut br) };
        if rc != 0 {
            log::error!("bench_fs: cfatfs f_read({path:?}) failed FR={rc}");
            break;
        }
        if br == 0 {
            break;
        }
        total += br as u64;
    }
    let elapsed = start.elapsed();

    // SAFETY: `fp`/`fs` are still the same live objects `f_open`/`f_mount`
    // initialised above.
    unsafe {
        cfatfs::f_close(&mut fp);
        cfatfs::f_mount(core::ptr::null_mut(), c"".as_ptr().cast::<u8>(), 0);
    }

    Some((total, mb_per_s(total, elapsed)))
}

/// Run the SP1 Task 4 benchmark: mount `embedded-fatfs`, find the largest
/// file under `SAMPLES/` (or root), time a sequential read of it through
/// both `embedded-fatfs` and the raw C FatFS ABI, and log the
/// `SP1_BENCH efatfs read=… MB/s ; cfatfs read=… MB/s` result line.
///
/// Called once from `main.rs`'s `app_task`, after `sd::boot_init()` (the SD
/// block driver — `deluge_bsp::sd` — is up) but before `deluge_app_init`
/// (nothing else has touched the card yet, so the two reads below are
/// genuinely uncontended). Boot continues normally afterward either way —
/// a failure here (no card, no file found, a read error) just skips straight
/// to the normal boot instead of panicking, so this feature never bricks the
/// image it's built into.
pub async fn run() {
    log::info!("bench_fs: SP1 Task 4 -- embedded-fatfs vs C FatFS read throughput");
    init_allocator();

    let storage = BufStream::<SdBlockDevice, 512>::new(SdBlockDevice);
    let fs = match FileSystem::new(storage, FsOptions::new()).await {
        Ok(fs) => fs,
        Err(e) => {
            log::error!("bench_fs: embedded-fatfs mount failed: {e:?} -- benchmark skipped");
            return;
        }
    };

    let Some((path, size)) = find_largest_file(&fs).await else {
        log::error!("bench_fs: no file found under SAMPLES/ or / -- benchmark skipped");
        return;
    };
    log::info!("bench_fs: target file \"{path}\" ({size} bytes)");

    let Some((efatfs_bytes, efatfs_mbps)) = efatfs_bench_read(&fs, &path).await else {
        return;
    };
    // Release efatfs's mount before mounting the C side, so the two are
    // never concurrently mounted even though (unlike `f_mount`'s single
    // global table, see `cfatfs_bench_read`'s doc) nothing would actually
    // conflict if they were — keeps the "sequential, not concurrent"
    // comparison honest in spirit as well as in the timing.
    drop(fs);

    let Some((cfatfs_bytes, cfatfs_mbps)) = cfatfs_bench_read(&path) else {
        return;
    };

    if efatfs_bytes != cfatfs_bytes || efatfs_bytes != size {
        log::warn!(
            "bench_fs: byte-count mismatch -- efatfs read {efatfs_bytes}, cfatfs read \
             {cfatfs_bytes}, directory entry said {size}; results below may not be a fair \
             comparison"
        );
    }

    log::info!("SP1_BENCH efatfs read={efatfs_mbps:.2} MB/s ; cfatfs read={cfatfs_mbps:.2} MB/s");
}
