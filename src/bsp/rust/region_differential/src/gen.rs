//! Deterministic, seeded generator of `Op` sequences — synthetic and
//! reproducible, no song/SD corpus, no `host_app` (mirrors the scripted-op shape
//! of `crates/deluge_resource/src/testing.rs`).
//!
//! Covers the residency shapes the region port has interesting behaviour for:
//! forward walks (prefetch hits), jumps ahead of the standing prefetch,
//! re-acquiring the same index (the idempotent-lease path), reverse steps,
//! `State` queries of the current + neighbouring indices, and independent-pin
//! `Retain`/`Release` on the last acquired lease.
use crate::ops::Op;

/// SplitMix64-style deterministic stream, so a `seed` reproduces the exact
/// sequence on every run and every host.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Generate `len` ops over a `num_clusters`-cluster stream from `seed`.
pub fn generate(seed: u64, num_clusters: u32, len: usize) -> Vec<Op> {
    assert!(num_clusters > 0, "need at least one cluster");
    let clusters = num_clusters as i64;
    let mut rng = Rng(seed);
    let mut ops = Vec::with_capacity(len);
    let mut idx: i64 = 0;

    let clamp = |v: i64| v.clamp(0, clusters - 1);

    for _ in 0..len {
        match rng.below(100) {
            0..=49 => {
                // Forward walk (drives prefetch hits on the next acquire).
                ops.push(Op::Acquire { index: idx as u32, direction: 1, priority: (rng.below(4)) as u32 });
                idx = clamp(idx + 1);
            }
            50..=64 => {
                // Jump ahead of the standing prefetch.
                let jump = 2 + rng.below(3) as i64;
                idx = clamp((idx + jump) % clusters);
                ops.push(Op::Acquire { index: idx as u32, direction: 1, priority: 0 });
            }
            65..=74 => {
                // Re-acquire the same index (the idempotent-lease path).
                ops.push(Op::Acquire { index: idx as u32, direction: 1, priority: 0 });
            }
            75..=84 => {
                // Reverse step.
                idx = clamp(idx - 1);
                ops.push(Op::Acquire { index: idx as u32, direction: -1, priority: 0 });
            }
            85..=93 => {
                // State query of the current or a neighbouring index.
                let q = clamp(idx + rng.below(3) as i64 - 1);
                ops.push(Op::State { index: q as u32 });
            }
            94..=96 => ops.push(Op::Retain),
            _ => ops.push(Op::Release),
        }
    }
    ops
}
