//! `region-read-differential` — U1 Task 5, the reader's byte-identical GATE: proves
//! `deluge_sample_reader`'s `open`/`window`/`advance`/`deluge_sample_read` deliver the exact same
//! bytes, for the exact same frames, that the CURRENT non-voice read path delivers today
//! (`StreamedChunk::frame_read_origin`/`payload_with_trailing_slack()`, `storage/cluster/cluster.h`)
//! — before any consumer migrates onto the reader (U2). See
//! `docs/superpowers/specs/2026-07-27-u1-sample-range-reader-design.md`'s "Testing / gates" section.
//!
//! Mirrors `region_fill_differential`'s structure (SR2d-4): a standalone Cargo crate (NOT a
//! workspace member — keeps this test-only harness off the firmware/BSP Cargo graph); `build.rs`
//! `cc`-compiles a C++ reference slice (`cpp/harness_shim.cpp`) into the test binary; `tests/`
//! holds the differential gate.
#![deny(unsafe_op_in_unsafe_fn)]

pub mod mapping;

/// The buffer-role/index-dependent, byte-distinguishable seed pattern used across the differential:
/// tag `t`'s byte `k` is `(t*100 + k) & 0xFF`. Mirrors `region_fill_differential::ramp`'s (and, one
/// rung further back, `region_differential::ops::make_ramp`'s) exact formula — a proven,
/// reproducible pattern from those rungs, reused here unchanged so a cluster's expected post-seed
/// bytes are trivial to compute independently of this crate's own harness code.
#[must_use]
pub fn ramp(tag: u32, n: usize) -> Vec<u8> {
    (0..n)
        .map(|k| ((tag as usize * 100 + k) & 0xFF) as u8)
        .collect()
}
