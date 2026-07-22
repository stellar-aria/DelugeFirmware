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

use aligned::{Aligned, A4};
use block_device_adapters::BufStream;
use block_device_driver::BlockDevice;
use efatfs_core::HandleTable;
use embassy_futures::block_on;
use embedded_fatfs::{DefaultTimeProvider, FileSystem, FsOptions, LossyOemCpConverter};
use fs_differential::{efatfs::EFatFs, fatfs_c::CFatFs, ram_disk::RamDisk};
use std::sync::atomic::{AtomicU64, Ordering};
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
        assert_eq!(
            &buf, b"DELUGE-SP0\n",
            "core detach/reattach corrupted hello.txt"
        );

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
        assert_eq!(
            got_kick, expected_kick,
            "interleave corrupted the Kicks wav"
        );
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

// --- Lens-1 margin proxy: SD-transfer overhead of an efatfs cluster read -----
//
// The Lens-1 concern is that embedded-fatfs's per-read cost (FAT-chain walk +
// single-block `BufStream`) is heavier than the raw cluster→sector-map read the
// C-FatFS path does (one contiguous DMA of the cluster's data sectors). The
// fully-integrated modeled-latency margin (the real C++ streaming scenario in
// `lens1_vt_sim`) is deferred; this is the lightweight direct proxy: count the
// 512-byte block reads embedded-fatfs issues to sequentially read a multi-
// cluster file, and compare against the ideal (one data read per data sector,
// no FAT-walk/re-read overhead). A pathological O(n) chain re-walk on every
// read — the spec's stated risk — would blow this number up.

/// Total 512-byte blocks read since the last reset (process-wide; serialized by
/// `TEST_LOCK`).
static BLOCKS_READ: AtomicU64 = AtomicU64::new(0);

/// `BlockDevice<512>` that counts blocks read, delegating to the shared `DISK`
/// exactly as `FileBlockDevice` does.
struct CountingBlockDevice;

impl BlockDevice<512> for CountingBlockDevice {
    type Align = A4;
    type Error = core::convert::Infallible;

    async fn read(
        &mut self,
        block_address: u32,
        data: &mut [Aligned<A4, [u8; 512]>],
    ) -> Result<(), Self::Error> {
        BLOCKS_READ.fetch_add(data.len() as u64, Ordering::Relaxed);
        for (i, blk) in data.iter_mut().enumerate() {
            let off = (block_address as u64 + i as u64) * 512;
            RamDisk::read_at(off, &mut blk[..]);
        }
        Ok(())
    }

    async fn write(
        &mut self,
        block_address: u32,
        data: &[Aligned<A4, [u8; 512]>],
    ) -> Result<(), Self::Error> {
        for (i, blk) in data.iter().enumerate() {
            let off = (block_address as u64 + i as u64) * 512;
            RamDisk::write_at(off, &blk[..]);
        }
        Ok(())
    }

    async fn size(&mut self) -> Result<u64, Self::Error> {
        Ok(RamDisk::len())
    }
}

#[test]
fn efatfs_core_sequential_read_transfer_overhead() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());

    // The mk_fixture.sh FAT32 image is formatted `-c 64` → 64 sectors (32 KiB)
    // per cluster; that's the raw-map path's contiguous read size.
    const CLUSTER_SECTORS: u64 = 64;
    let kick_path = "/SAMPLES/big_multicluster.bin"; // 1 MiB = 32 clusters

    // Whole-file size via the ordinary EFatFs oracle (uncounted).
    let file_len = EFatFs::mount().read_file(kick_path).len() as u64;
    let data_sectors = file_len.div_ceil(512); // ideal: one read per data sector
    let clusters = file_len.div_ceil(CLUSTER_SECTORS * 512);

    // Mount a fresh FS over the counting device and read the file sequentially,
    // one cluster (32 KiB) at a time, through the real efatfs_core.
    let storage = BufStream::<CountingBlockDevice, 512>::new(CountingBlockDevice);
    let fs: FileSystem<_, DefaultTimeProvider, LossyOemCpConverter> =
        block_on(FileSystem::new(storage, FsOptions::new())).expect("mount counting FS");

    block_on(async {
        let ctx = efatfs_core::open_context(&fs, kick_path)
            .await
            .expect("open kick");
        let mut table = HandleTable::new();
        let h = table.insert(ctx).expect("insert");

        BLOCKS_READ.store(0, Ordering::Relaxed); // count only the streamed reads
        let cluster_bytes = (CLUSTER_SECTORS * 512) as usize;
        let mut off: u32 = 0;
        while (off as u64) < file_len {
            let want = cluster_bytes.min((file_len - off as u64) as usize);
            let mut buf = vec![0u8; want];
            assert!(
                table.read_at_owned(&fs, h, off, &mut buf).await,
                "cluster read at {off} was short"
            );
            off += want as u32;
        }
    });

    let blocks = BLOCKS_READ.load(Ordering::Relaxed);
    let overhead = blocks as f64 / data_sectors as f64;
    println!(
        "lens1-proxy: file={file_len}B clusters={clusters} data_sectors={data_sectors} \
         efatfs_block_reads={blocks} overhead={overhead:.2}x (ideal 1.00x = raw-map data-only)"
    );

    // Sequential reads should stay near the ideal — a few extra reads per
    // cluster for the FAT-chain walk, NOT an O(n) re-walk of the whole chain on
    // every cluster (which would scale the overhead with `clusters`). Guard
    // generously (3x) so this catches a pathological regression, not tuning
    // noise.
    assert!(
        overhead < 3.0,
        "efatfs sequential-read transfer overhead {overhead:.2}x too high \
         (blocks={blocks}, data_sectors={data_sectors}, clusters={clusters}) — \
         possible O(n) FAT-chain re-walk regression"
    );
}

/// Looping-playback proxy: models a sample looping over its back half — one
/// BACKWARD seek at the loop point, then ~1024 clusters read forward
/// SEQUENTIALLY before the next wrap. `seek()` has no "continue from
/// current_cluster" branch (file.rs:489-516), so the one seek per wrap does
/// re-walk the chain from `first_cluster`, but that cost is amortized over
/// the ~1024 sequential reads that follow it — each of those hits `seek()`'s
/// "already at this offset" early return (file.rs:485) and costs nothing
/// extra. Four wraps here spend one re-walk each against 4096 total reads.
///
/// IMPORTANT: this measures the cost real looping playback actually pays —
/// it does NOT measure seek cost in isolation, and its low overhead is NOT
/// evidence that seeking is cheap in general. An amortized workload like
/// this can't show what a seek-dense workload costs; that's what
/// `efatfs_core_adversarial_seek_transfer_overhead` (below) is for, where
/// every single read is a long-distance backward seek with nothing to
/// amortize it against.
#[test]
fn efatfs_core_looping_playback_transfer_overhead() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());

    const CLUSTER_SECTORS: u64 = 64; // mk_fixture.sh formats FAT32 with -c 64 (32 KiB)
    let path = "/SAMPLES/huge.bin"; // 64 MiB = 2048 clusters
    let file_len = EFatFs::mount().read_file(path).len() as u64;
    let cluster_bytes = (CLUSTER_SECTORS * 512) as usize;
    let clusters = file_len.div_ceil(cluster_bytes as u64);

    // Loop over the back half: clusters [clusters/2, clusters), repeated. Each
    // wrap is a backward seek across many clusters.
    let loop_start = clusters / 2;
    const PASSES: u64 = 4;
    let reads = PASSES * (clusters - loop_start);
    let data_sectors = reads * CLUSTER_SECTORS; // ideal: data sectors only

    let storage = BufStream::<CountingBlockDevice, 512>::new(CountingBlockDevice);
    let fs: FileSystem<_, DefaultTimeProvider, LossyOemCpConverter> =
        block_on(FileSystem::new(storage, FsOptions::new())).expect("mount counting FS");

    block_on(async {
        let ctx = efatfs_core::open_context(&fs, path).await.expect("open");
        let mut table = HandleTable::new();
        let h = table.insert(ctx).expect("insert");

        BLOCKS_READ.store(0, Ordering::Relaxed);
        let mut buf = vec![0u8; cluster_bytes];
        for _ in 0..PASSES {
            for c in loop_start..clusters {
                let off = (c * cluster_bytes as u64) as u32;
                assert!(
                    table.read_at_owned(&fs, h, off, &mut buf).await,
                    "loop read at cluster {c} was short"
                );
            }
        }
    });

    let blocks = BLOCKS_READ.load(Ordering::Relaxed);
    let overhead = blocks as f64 / data_sectors as f64;
    println!(
        "lens1-loop-proxy: clusters={clusters} loop_start={loop_start} passes={PASSES} \
         reads={reads} data_sectors={data_sectors} efatfs_block_reads={blocks} \
         overhead={overhead:.2}x (ideal 1.00x = raw-map data-only)"
    );

    // Measured 1.03x (looping playback amortizes its one backward seek per
    // wrap over ~1024 sequential reads) — this is the evidence SP1b's planned
    // cached FAT cluster chain was dropped for; see
    // `efatfs_core_adversarial_seek_transfer_overhead` for the isolated
    // seek-cost measurement and .superpowers/sdd/task-1-report.md for the
    // full derivation. A regression here means looping-sample playback
    // itself got slower, not merely that seeking got more expensive.
    assert!(
        overhead < 1.10,
        "looping-playback transfer overhead {overhead:.2}x regressed \
         (blocks={blocks}, data_sectors={data_sectors}, clusters={clusters})"
    );
}

// --- Adversarial seek-cost isolation ----------------------------------------
//
// The looping-playback proxy above amortizes its one backward seek per wrap
// over ~1024 sequential reads, so it cannot show what seeking itself costs.
// This test isolates that: every single read is a long-distance backward
// seek, with nothing sequential to absorb the FAT-chain re-walk cost into.
//
// Cost model (confirmed by the numbers below): a re-walk to cluster `k`
// touches `ceil(k/128)` FAT sectors, NOT `k` reads — FAT32 packs 128 4-byte
// entries per 512-byte FAT sector, so the re-walk is O(n/128) sector reads,
// not O(n). Against 64 data sectors per 32 KiB cluster, constant
// long-distance seeking costs about `1 + n/16384` for an n-cluster file.
// Measured at n=2048 (64 MB, `/SAMPLES/huge.bin`): **1.15x**, ~9.8 extra
// block reads per read vs a predicted 8.
//
// Consequence: typical Deluge samples (single-digit MB) cost on the order of
// 1.6% even under CONSTANT adversarial seeking, and real looping playback
// (above) costs 1.03x. That is why SP1b's cached FAT cluster chain was
// dropped — the re-walk it would have eliminated is already cheap at
// realistic sizes. See .superpowers/sdd/task-1-report.md for the full
// measurement record.

/// Two seek-dense access patterns over the same 2048-cluster file, neither of
/// which ever reads two consecutive clusters in file order: reverse
/// (`(0..clusters).rev()`) and ping-pong (alternating ends, walking inward).
/// Each pattern gets a fresh mount so `BufStream`'s single-block cache starts
/// cold and one pattern's tail can't warm the other's head.
#[test]
fn efatfs_core_adversarial_seek_transfer_overhead() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());

    const CLUSTER_SECTORS: u64 = 64; // mk_fixture.sh formats FAT32 with -c 64 (32 KiB)
    let path = "/SAMPLES/huge.bin"; // 64 MiB = 2048 clusters; see mk_fixture.sh for why
    let file_len = EFatFs::mount().read_file(path).len() as u64;
    let cluster_bytes = (CLUSTER_SECTORS * 512) as usize;
    let clusters = file_len.div_ceil(cluster_bytes as u64) as usize;
    let data_sectors = clusters as u64 * CLUSTER_SECTORS; // ideal: one read per data sector

    for (name, reverse) in [("reverse", true), ("pingpong", false)] {
        let cluster_at = |i: usize| -> usize {
            if reverse {
                clusters - 1 - i
            } else if i % 2 == 0 {
                i / 2
            } else {
                clusters - 1 - i / 2
            }
        };

        let storage = BufStream::<CountingBlockDevice, 512>::new(CountingBlockDevice);
        let fs: FileSystem<_, DefaultTimeProvider, LossyOemCpConverter> =
            block_on(FileSystem::new(storage, FsOptions::new())).expect("mount counting FS");

        block_on(async {
            let ctx = efatfs_core::open_context(&fs, path).await.expect("open");
            let mut table = HandleTable::new();
            let h = table.insert(ctx).expect("insert");

            BLOCKS_READ.store(0, Ordering::Relaxed);
            let mut buf = vec![0u8; cluster_bytes];
            for i in 0..clusters {
                let c = cluster_at(i);
                let off = (c as u64 * cluster_bytes as u64) as u32;
                assert!(
                    table.read_at_owned(&fs, h, off, &mut buf).await,
                    "{name} read at cluster {c} (i={i}) was short"
                );
            }
        });

        let blocks = BLOCKS_READ.load(Ordering::Relaxed);
        let reads = clusters as u64;
        let overhead = blocks as f64 / data_sectors as f64;
        println!(
            "lens1-adversarial-seek-proxy[{name}]: clusters={clusters} reads={reads} \
             data_sectors={data_sectors} efatfs_block_reads={blocks} overhead={overhead:.2}x \
             (ideal 1.00x = raw-map data-only)"
        );

        assert!(
            overhead < 1.30,
            "{name} adversarial-seek transfer overhead {overhead:.2}x too high \
             (blocks={blocks}, data_sectors={data_sectors}, clusters={clusters}) — \
             possible FAT-chain re-walk regression"
        );
    }
}

/// Forward-continue regression proof for `seek()` (file.rs's `impl Seek for
/// File`). The adversarial test above establishes the cost model: a chain
/// re-walk to cluster `k` touches `ceil(k/128)` FAT sectors (FAT32 packs 128
/// 4-byte entries per 512-byte FAT sector), so a 32-cluster file (the old
/// `big_multicluster.bin` fixture) fits in ONE FAT sector regardless of
/// whether `seek()` restarts from `first_cluster` or continues from
/// `current_cluster` — that fixture cannot discriminate the fix from its
/// absence. `/SAMPLES/huge.bin` (64 MiB = 2048 clusters) can: this hops
/// forward 256 clusters at a time from cluster 0 to 1792, landing at k =
/// 256, 512, 768, 1024, 1280, 1536, 1792 (mean k = 1024).
///
/// Without the fix, each hop restarts at `first_cluster`, so cost is
/// `ceil(k/128)` FAT sectors per hop; averaged over the seven hops that is
/// `ceil(1024/128)` = 8 FAT sectors against 64 data sectors/cluster, predicting
/// ~1.125x — **measured 1.154x** (517 blocks / 448 data sectors). With the fix,
/// each hop only walks the 256-cluster delta from `current_cluster`, costing
/// `ceil(256/128)` = 2 FAT sectors, predicting ~1.03x — **measured 1.060x**
/// (475/448). The idealized model undercounts both sides by the same constant:
/// `read_context` reattaches a fresh `File` on every read via
/// `File::new_from_context` (file.rs:76-91, the PR #59 stale-context guard),
/// which reads+compares the on-disk 32-byte directory entry every time —
/// one more block read per read call, independent of the chain walk, present
/// whether or not this fix applies. The 1.10x bound sits well below the
/// measured broken number (1.154x) and with clear margin above the measured
/// fixed number (1.060x), so a regression back to the `first_cluster` restart
/// still trips it.
#[test]
fn efatfs_forward_seek_does_not_restart_chain_walk() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let _disk = RamDisk::load(&fat32());

    const CLUSTER_SECTORS: u64 = 64; // mk_fixture.sh formats FAT32 with -c 64 (32 KiB)
    let path = "/SAMPLES/huge.bin"; // 64 MiB = 2048 clusters
    let cluster_bytes = (CLUSTER_SECTORS * 512) as usize;

    let storage = BufStream::<CountingBlockDevice, 512>::new(CountingBlockDevice);
    let fs: FileSystem<_, DefaultTimeProvider, LossyOemCpConverter> =
        block_on(FileSystem::new(storage, FsOptions::new())).expect("mount counting FS");

    block_on(async {
        let ctx = efatfs_core::open_context(&fs, path).await.expect("open");
        let mut table = HandleTable::new();
        let h = table.insert(ctx).expect("insert");
        let mut buf = vec![0u8; cluster_bytes];

        // Land at an early cluster first, outside the measured window, so the
        // measured hops all start from a real `current_cluster`, not `None`.
        assert!(table.read_at_owned(&fs, h, 0, &mut buf).await, "initial read");

        BLOCKS_READ.store(0, Ordering::Relaxed);
        let mut reads = 0u64;
        for c in [256u64, 512, 768, 1024, 1280, 1536, 1792] {
            let off = (c * cluster_bytes as u64) as u32;
            assert!(
                table.read_at_owned(&fs, h, off, &mut buf).await,
                "read at cluster {c}"
            );
            reads += 1;
        }
        let blocks = BLOCKS_READ.load(Ordering::Relaxed);
        let data_sectors = reads * CLUSTER_SECTORS;
        let overhead = blocks as f64 / data_sectors as f64;
        println!("forward-seek: reads={reads} blocks={blocks} overhead={overhead:.2}x");

        // Measured 1.154x without the fix (restart from first_cluster) vs
        // 1.060x with it (continue from current_cluster) — see the doc
        // comment above for the full derivation, including the constant
        // per-read directory-entry-validation cost (PR #59) that both
        // numbers carry. 1.10x sits with real margin below the broken
        // number and above the fixed one.
        assert!(
            overhead < 1.10,
            "forward seek overhead {overhead:.2}x suggests seek() is still restarting \
             the chain walk at first_cluster (blocks={blocks}, data_sectors={data_sectors})"
        );
    });
}

/// R0a: the AUDIO read access pattern — cluster-aligned reads in playback order,
/// including loop-point backward seeks — must be byte-identical through efatfs and
/// C FatFS. Complements the whole-file/interleave differentials with the pattern the
/// streaming engine actually issues. The C-FatFS whole-file read is the oracle.
#[test]
fn efatfs_streaming_pattern_matches_cfatfs_fat32() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = fat32();
    let _disk = RamDisk::load(&img);
    let e = EFatFs::mount();

    const CLUSTER_SECTORS: usize = 64; // mk_fixture.sh formats FAT32 -c 64 (32 KiB clusters)
    const CLUSTER_BYTES: usize = CLUSTER_SECTORS * 512;
    let path = "/SAMPLES/huge.bin"; // 64 MiB = 2048 clusters

    // Oracle: the whole file via C FatFS (a DIFFERENT filesystem from efatfs).
    let oracle = CFatFs::mount().read_file(path);
    let clusters = oracle.len().div_ceil(CLUSTER_BYTES);
    assert!(clusters > 4, "fixture too small to exercise the pattern");

    // Playback-order sequence with loop-point backward seeks: forward through the
    // file, then loop the back half twice (the case efatfs's seek() re-walk fix and
    // the streaming path care about).
    let mut order: Vec<usize> = (0..clusters).collect();
    let loop_start = clusters / 2;
    for _ in 0..2 {
        order.extend(loop_start..clusters);
    }

    let ctx = e.open_context(path);
    let mut ctx = ctx;
    for c in order {
        let off = (c * CLUSTER_BYTES) as u32;
        let want = CLUSTER_BYTES.min(oracle.len() - c * CLUSTER_BYTES);
        let mut buf = vec![0u8; want];
        let (newctx, filled) = e.read_at_context(&ctx, off, &mut buf);
        ctx = newctx;
        assert!(filled, "efatfs short read at cluster {c} (off {off})");
        assert_eq!(
            &buf[..],
            &oracle[c * CLUSTER_BYTES..c * CLUSTER_BYTES + want],
            "efatfs streaming-pattern read diverged from C FatFS at cluster {c}"
        );
    }
}

/// R0a non-vacuity: reading the WRONG cluster must NOT match the oracle — proves the
/// streaming differential above can actually detect a divergence (per the project's
/// real-execution mandate). If this ever passes-as-equal, the differential is blind.
#[test]
fn efatfs_streaming_pattern_nonvacuous() {
    let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let img = fat32();
    let _disk = RamDisk::load(&img);
    let e = EFatFs::mount();

    const CLUSTER_BYTES: usize = 64 * 512;
    let path = "/SAMPLES/huge.bin";
    let oracle = CFatFs::mount().read_file(path);

    // Read cluster 3 but compare against the oracle's cluster 0 slice — a real
    // multi-cluster file has distinct cluster contents unless the test is blind.
    let ctx = e.open_context(path);
    let mut buf = vec![0u8; CLUSTER_BYTES];
    let (_ctx, filled) = e.read_at_context(&ctx, (3 * CLUSTER_BYTES) as u32, &mut buf);
    assert!(filled, "short read");
    assert_ne!(
        &buf[..],
        &oracle[0..CLUSTER_BYTES],
        "cluster 3 matched the oracle's cluster 0 — the fixture's clusters are not \
         distinguishable, so the streaming differential cannot detect a wrong-offset read"
    );
}

