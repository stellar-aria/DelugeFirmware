//! SP0 differential harness integration tests.
use fs_differential::{fatfs_c::CFatFs, ram_disk::RamDisk};

/// Drives the real, vendored C FatFS (via the FFI bridge in `fatfs_c.rs`)
/// against a FAT32 card image built by `fixtures/mk_fixture.sh`, and checks
/// it reads back the known fixture file byte-for-byte. This is the harness's
/// oracle side coming online: the same read, on the same image, will later
/// be driven through `embedded-fatfs` (Task 4) and diffed against this one.
#[test]
fn cfatfs_reads_known_file_fat32() {
    let img = std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img");
    let _disk = RamDisk::load(&img);
    let fs = CFatFs::mount();
    assert_eq!(fs.read_file("/SAMPLES/hello.txt"), b"DELUGE-SP0\n");
}
