//! SP0 differential harness integration tests.
use fs_differential::{diff::compare_read, efatfs::EFatFs, fatfs_c::CFatFs, ram_disk::RamDisk};
use std::sync::Mutex;

/// `DISK` (`ram_disk.rs`) and the C FatFS single volume (`FF_VOLUMES=1`,
/// `f_mount`'s process-wide `FatFs[]` table) are both process-wide
/// singletons. `cargo test` runs `#[test]`s in parallel threads by default,
/// so any two tests that load an image / mount a filesystem would race on
/// that shared state. Every such test takes this lock first, for the
/// duration of the whole load-mount-read sequence, so only one is ever
/// touching the shared image/volume at a time -- an alternative to
/// `--test-threads=1` that doesn't serialize the whole binary.
static TEST_LOCK: Mutex<()> = Mutex::new(());

/// Drives the real, vendored C FatFS (via the FFI bridge in `fatfs_c.rs`)
/// against a FAT32 card image built by `fixtures/mk_fixture.sh`, and checks
/// it reads back the known fixture file byte-for-byte. This is the harness's
/// oracle side coming online: the same read, on the same image, will later
/// be driven through `embedded-fatfs` (Task 4) and diffed against this one.
#[test]
fn cfatfs_reads_known_file_fat32() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img");
    let _disk = RamDisk::load(&img);
    let fs = CFatFs::mount();
    assert_eq!(fs.read_file("/SAMPLES/hello.txt"), b"DELUGE-SP0\n");
}

/// Same known file, same image, driven through the vendored `embedded-fatfs`
/// (the Rust half of the differential) instead of the C FatFS FFI bridge.
/// Mounts over the SAME shared `DISK` image via `efatfs::MemIo`, so this is
/// the read-path proof that both stacks agree from one on-disk image.
#[test]
fn efatfs_reads_known_file_fat32() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img");
    let _disk = RamDisk::load(&img);
    let fs = EFatFs::mount();
    assert_eq!(fs.read_file("/SAMPLES/hello.txt"), b"DELUGE-SP0\n");
}

/// Walks the WHOLE fixture tree through both backends and asserts they agree
/// on every directory listing and every file's bytes -- SP0's core
/// instrument, run against the FAT32 image.
fn run_read_diff(env: &str) {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = std::env::var(env)
        .unwrap_or_else(|_| panic!("run mk_fixture.sh; set {env}=/tmp/<variant>.img"));
    let _disk = RamDisk::load(&img);
    let (c, e) = (CFatFs::mount(), EFatFs::mount());
    compare_read(&c, &e).expect("read/enumerate differential");
}

#[test]
fn read_diff_fat32() {
    run_read_diff("SP0_FAT32");
}

#[test]
fn read_diff_fat16() {
    run_read_diff("SP0_FAT16");
}
