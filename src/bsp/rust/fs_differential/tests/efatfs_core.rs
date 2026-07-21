//! SP1a Task 7a: host coverage of the REAL `efatfs_core` handle-table logic.
//!
//! The device handle table (`src/efatfs_fs.rs`) is `#![cfg(target_os = "none")]`
//! and can't be reached from any host build; Task 7a extracted its logic into
//! the storage-generic `src/efatfs_core.rs`. This test `#[path]`-includes that
//! exact source and drives it over `fs_differential`'s host `FileSystem`
//! (`FileBlockDevice` → `BufStream` → embedded-fatfs), so the generation guard,
//! the `FileContext` detach/reattach, and the fill loop get real execution on
//! host — coverage the Task-3 reviewer flagged as missing.
//!
//! DIVERGENCE: host is single-threaded `block_on`, so this validates the core's
//! logic (round-trip, interleave, recycle guard) ONLY, not the device
//! `static`/embassy-`Mutex` serialization — that is the on-device Task 8 gate.

// Recompile the real core into this test binary (same `#[path]` convention the
// BSP host tests use). efatfs_core depends only on embedded_fatfs +
// embedded_io_async, both of which are fs_differential deps.
#[path = "../../src/efatfs_core.rs"]
mod efatfs_core;

use efatfs_core::HandleTable;
use embassy_futures::block_on;
use fs_differential::{efatfs::EFatFs, ram_disk::RamDisk};
use std::sync::Mutex;

// Shared with tests/differential.rs's rationale: the RAM `DISK` + C FatFS
// volume are process-wide singletons, so serialize the load-mount-read span.
static TEST_LOCK: Mutex<()> = Mutex::new(());

fn fat32() -> String {
    std::env::var("SP0_FAT32").expect("run mk_fixture.sh; set SP0_FAT32=/tmp/fat32.img")
}

/// (a) round-trip + (b) two-handle interleave, driven through the REAL
/// `efatfs_core::{open_context, HandleTable::read_at_owned}` — the same code
/// the device wraps, not a parallel reimplementation.
#[test]
fn efatfs_core_open_read_and_interleave_fat32() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());
    let efatfs = EFatFs::mount();
    let fs = efatfs.raw();
    let mut table = HandleTable::new();

    block_on(async {
        // (a) open → insert → read_at_owned(0) → whole-file bytes.
        let ctx = efatfs_core::open_context(fs, "/SAMPLES/hello.txt")
            .await
            .expect("open_context hello.txt");
        let h = table.insert(ctx).expect("table not full");
        let mut buf = [0u8; 11]; // len(b"DELUGE-SP0\n")
        assert!(
            table.read_at_owned(fs, h, 0, &mut buf).await,
            "hello.txt fill was short"
        );
        assert_eq!(&buf, b"DELUGE-SP0\n", "core detach/reattach corrupted hello.txt");

        // (b) two handles, reads interleaved round-robin, reassembled bytes must
        //     match the whole-file oracle for each.
        let kick_path = "/SAMPLES/Kicks/Deep House Kick (loud).wav";
        let expected_hello = efatfs.read_file("/SAMPLES/hello.txt");
        let expected_kick = efatfs.read_file(kick_path);

        let kctx = efatfs_core::open_context(fs, kick_path)
            .await
            .expect("open_context kick");
        let hk = table.insert(kctx).expect("table not full");

        let (mut got_hello, mut got_kick) = (Vec::new(), Vec::new());
        let (mut off_hello, mut off_kick) = (0u32, 0u32);
        const CHUNK: usize = 8;
        loop {
            let mut progressed = false;
            for (handle, off, expected, got) in [
                (h, &mut off_hello, &expected_hello, &mut got_hello),
                (hk, &mut off_kick, &expected_kick, &mut got_kick),
            ] {
                let remaining = expected.len() - *off as usize;
                if remaining == 0 {
                    continue;
                }
                let want = CHUNK.min(remaining);
                let mut chunk = vec![0u8; want];
                assert!(
                    table.read_at_owned(fs, handle, *off, &mut chunk).await,
                    "interleaved chunk at offset {off} was short"
                );
                got.extend_from_slice(&chunk);
                *off += want as u32;
                progressed = true;
            }
            if !progressed {
                break;
            }
        }
        assert_eq!(got_hello, expected_hello, "interleave corrupted hello.txt");
        assert_eq!(got_kick, expected_kick, "interleave corrupted the Kicks wav");
    });
}

/// The generation guard: a `checkout` captured before a `remove` + `insert`
/// recycle of the same slot index must NOT be able to `commit` a stale context
/// onto the file now occupying that slot. This is the device `read_at`
/// write-back race the Task-3 fix guards against — untestable on the device
/// (statics), covered here against the real core.
#[test]
fn efatfs_core_generation_guard_rejects_stale_commit() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());
    let efatfs = EFatFs::mount();
    let fs = efatfs.raw();
    let mut table = HandleTable::new();

    block_on(async {
        let kick_path = "/SAMPLES/Kicks/Deep House Kick (loud).wav";

        // Occupy a slot with hello.txt, then capture a checkout (handle + gen)
        // and the hello context to try to write back later.
        let hello_ctx = efatfs_core::open_context(fs, "/SAMPLES/hello.txt")
            .await
            .expect("open hello");
        let h = table.insert(hello_ctx).expect("insert hello");
        let (stale_gen, stale_ctx) = table.checkout(h).expect("checkout hello");

        // Recycle that exact slot index for a DIFFERENT file (kick): remove
        // bumps the generation, insert reuses the lowest-free index (== h).
        table.remove(h);
        let kick_ctx = efatfs_core::open_context(fs, kick_path)
            .await
            .expect("open kick");
        let h2 = table.insert(kick_ctx).expect("insert kick");
        assert_eq!(h2, h, "insert should reuse the freed lowest index");

        // The stale write-back must be rejected (generation moved on).
        table.commit(h, stale_gen, stale_ctx);

        // Reading the recycled handle must yield KICK bytes, not corrupted /
        // hello data — proving the stale commit did not splice hello's context
        // onto the kick slot.
        let expected_kick = efatfs.read_file(kick_path);
        let mut buf = vec![0u8; 16.min(expected_kick.len())];
        assert!(
            table.read_at_owned(fs, h, 0, &mut buf).await,
            "kick read after recycle was short"
        );
        assert_eq!(
            &buf[..],
            &expected_kick[..buf.len()],
            "stale commit corrupted the recycled slot's file identity"
        );
    });
}
