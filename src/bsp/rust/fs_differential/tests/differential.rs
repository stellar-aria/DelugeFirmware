//! SP0 differential harness integration tests.
use fs_differential::ops::Op;
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

/// The write-path corpus (Task 6): mkdir, a multi-cluster LFN-named write,
/// an extend that grows the same file further, a short-name write, a
/// rename, and a delete of a pre-existing fixture file. Exercises create,
/// extend-across-cluster-boundary, LFN entry creation, rename, and delete
/// in one pass, on both FAT variants.
fn write_corpus() -> Vec<Op> {
    vec![
        Op::Mkdir("/REC".into()),
        Op::Write("/REC/take 01.wav".into(), vec![0xAB; 40_000]), // multi-cluster, LFN name
        Op::Extend("/REC/take 01.wav".into(), vec![0xCD; 9_000]), // extend, further multi-cluster growth
        Op::Write("/REC/SHORT.RAW".into(), b"hi".to_vec()),
        Op::Rename("/REC/SHORT.RAW".into(), "/REC/Renamed Long.raw".into()),
        Op::Delete("/SAMPLES/hello.txt".into()),
    ]
}

/// Replays `write_corpus()` on independent copies of the same image through
/// both backends and diffs the resulting logical trees -- SP0's write-path
/// instrument, run against the FAT32 image.
#[test]
fn write_diff_fat32() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img");
    fs_differential::diff::replay_and_compare(&img, &write_corpus()).expect("write differential");
}

/// Same corpus, FAT16 image.
#[test]
fn write_diff_fat16() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = std::env::var("SP0_FAT16").expect("run mk_fixture.sh; set SP0_FAT16=/tmp/fat16.img");
    fs_differential::diff::replay_and_compare(&img, &write_corpus()).expect("write differential");
}

/// FAT32-only raw-entry parsing helper for the `..` demonstration probe
/// below. Both backends' own read APIs are useless here: C FatFS's
/// `f_readdir` never yields `.`/`..` at all (see `fatfs_c.rs`), and this
/// harness's `EFatFs::read_dir` filters them out to normalize against that
/// (see `efatfs.rs`) -- so the only way to see what each backend actually
/// wrote into a `..` entry's first-cluster field is to read the raw 32-byte
/// directory entry bytes straight out of the image.
mod raw_fat32 {
    /// The BPB fields needed to locate a cluster's first byte and a
    /// directory's entries within a raw FAT32 image (boot-sector layout;
    /// see e.g. Microsoft's `fatgen103.doc` or `src/fatfs/ff.c`'s BPB
    /// offsets).
    struct Bpb32 {
        bytes_per_sector: u32,
        sectors_per_cluster: u32,
        first_data_sector: u32,
        root_cluster: u32,
    }

    fn parse_bpb32(img: &[u8]) -> Bpb32 {
        let bytes_per_sector = u16::from_le_bytes([img[11], img[12]]) as u32;
        let sectors_per_cluster = img[13] as u32;
        let reserved_sectors = u16::from_le_bytes([img[14], img[15]]) as u32;
        let num_fats = img[16] as u32;
        let fat_size_32 = u32::from_le_bytes([img[36], img[37], img[38], img[39]]);
        let root_cluster = u32::from_le_bytes([img[44], img[45], img[46], img[47]]);
        let first_data_sector = reserved_sectors + num_fats * fat_size_32;
        Bpb32 { bytes_per_sector, sectors_per_cluster, first_data_sector, root_cluster }
    }

    fn cluster_offset(bpb: &Bpb32, cluster: u32) -> usize {
        let sector = bpb.first_data_sector + (cluster - 2) * bpb.sectors_per_cluster;
        sector as usize * bpb.bytes_per_sector as usize
    }

    /// Scans 32-byte directory entries starting at byte offset `dir_off` for
    /// a short-name (11-byte, space-padded) match, skipping deleted (0xE5)
    /// and LFN (attr 0x0F) entries. Returns the matching entry's own byte
    /// offset.
    fn find_sfn_entry(img: &[u8], dir_off: usize, sfn11: &[u8; 11]) -> Option<usize> {
        let mut off = dir_off;
        loop {
            if img[off] == 0x00 {
                return None; // end of directory
            }
            let attr = img[off + 11];
            if img[off] != 0xE5 && attr != 0x0F && &img[off..off + 11] == sfn11 {
                return Some(off);
            }
            off += 32;
        }
    }

    /// The first-cluster field (hi word at 0x14, lo word at 0x1A) of the
    /// 32-byte directory entry starting at `entry_off`.
    fn entry_first_cluster(img: &[u8], entry_off: usize) -> u32 {
        let hi = u16::from_le_bytes([img[entry_off + 0x14], img[entry_off + 0x15]]);
        let lo = u16::from_le_bytes([img[entry_off + 0x1A], img[entry_off + 0x1B]]);
        ((hi as u32) << 16) | lo as u32
    }

    /// For a FAT32 image that has had a directory named `dirname` (an 8.3
    /// short name, e.g. `"REC        "`, 11 bytes space-padded) created
    /// directly under the root: returns `(dir's own first cluster, the
    /// first-cluster field written into that dir's own ".." entry)`.
    pub fn probe_dotdot_cluster(img: &[u8], dirname_sfn11: &[u8; 11]) -> (u32, u32) {
        let bpb = parse_bpb32(img);
        let root_off = cluster_offset(&bpb, bpb.root_cluster);
        let dir_entry_off =
            find_sfn_entry(img, root_off, dirname_sfn11).expect("directory entry not found under root");
        let dir_cluster = entry_first_cluster(img, dir_entry_off);
        let dir_off = cluster_offset(&bpb, dir_cluster);
        // "." is entry index 0, ".." is entry index 1 in a freshly-created
        // directory (both C FatFS's f_mkdir and embedded-fatfs's create_dir
        // write them in that order -- see ff.c's f_mkdir_and_get and
        // dir.rs's create_dir).
        let dotdot_off = dir_off + 32;
        assert_eq!(&img[dotdot_off..dotdot_off + 2], b"..", "expected '..' entry at index 1");
        (dir_cluster, entry_first_cluster(img, dotdot_off))
    }
}

/// DEMONSTRATION (not a fix -- see Task 6B): the vendoring survey flagged
/// that embedded-fatfs's `Dir::create_dir` writes the ROOT's own first
/// cluster into a new directory's `..` entry when that directory is created
/// directly under the FAT32 root, instead of the FAT convention (which both
/// the FAT spec and C FatFS follow) of writing 0 there to mean "parent is
/// the root". This divergence is invisible to `write_diff_fat32` above
/// because both `FsOps::read_dir` implementations filter `.`/`..` out (see
/// `raw_fat32` module doc) -- so this probe reads the raw on-disk `..`
/// entry directly, bypassing both backends' directory-listing APIs, and
/// prints/asserts the concrete cluster numbers each one wrote.
#[test]
fn fat32_dotdot_cluster_probe() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img_path = std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img");
    let orig = std::fs::read(&img_path).expect("read fixture image");
    let rec_sfn: [u8; 11] = *b"REC        ";

    let disk = RamDisk::load_bytes(&orig);
    let mut c = CFatFs::mount();
    c.mkdir("/REC");
    drop(c);
    let c_image = disk.snapshot();
    let (c_rec_cluster, c_dotdot_cluster) = raw_fat32::probe_dotdot_cluster(&c_image, &rec_sfn);

    let disk = RamDisk::load_bytes(&orig);
    let mut e = EFatFs::mount();
    e.mkdir("/REC");
    drop(e);
    let e_image = disk.snapshot();
    let (e_rec_cluster, e_dotdot_cluster) = raw_fat32::probe_dotdot_cluster(&e_image, &rec_sfn);

    eprintln!(
        "FAT32 '..' probe: C FatFS -- REC cluster={c_rec_cluster}, '..' cluster field={c_dotdot_cluster}; \
         embedded-fatfs -- REC cluster={e_rec_cluster}, '..' cluster field={e_dotdot_cluster}"
    );

    // C FatFS convention (and the FAT spec's): a directory whose parent is
    // the root writes 0 into its ".." entry, regardless of the root's own
    // actual first-cluster number.
    assert_eq!(c_dotdot_cluster, 0, "C FatFS should write 0 into '..' under root");

    // The known embedded-fatfs bug: it writes the root's own first cluster
    // instead of 0. If this ever starts asserting `e_dotdot_cluster == 0`,
    // the bug has been fixed upstream/in-vendor -- update this probe (and
    // Task 6B) rather than treating that as a probe failure.
    assert_ne!(
        e_dotdot_cluster, 0,
        "expected embedded-fatfs's known '..'-under-root bug (writes root cluster, not 0); \
         if this fails, the bug appears fixed -- see Task 6B"
    );
    let bpb_root_cluster = u32::from_le_bytes([e_image[44], e_image[45], e_image[46], e_image[47]]);
    assert_eq!(
        e_dotdot_cluster, bpb_root_cluster,
        "expected embedded-fatfs to have written the root's own first cluster"
    );
}
