//! `region-read-differential` — U1 Task 5, the reader's byte-identical GATE: proves
//! `deluge_sample_reader`'s `open`/`window`/`advance`/`deluge_sample_read` deliver the exact same
//! bytes, for the exact same frames, as a direct read of a chunk's own resident payload buffer at
//! an independently computed frame -> (cluster, byte-offset) location — before any consumer
//! migrates onto the reader (U2). See
//! `docs/superpowers/specs/2026-07-27-u1-sample-range-reader-design.md`'s "Testing / gates" section.
//!
//! Mirrors `region_fill_differential`'s structure (SR2d-4): a standalone Cargo crate (NOT a
//! workspace member — keeps this test-only harness off the firmware/BSP Cargo graph); `tests/`
//! holds the differential gate. Pure Rust (U4d): this crate used to `cc`-compile a small C++
//! reference slice (`cpp/harness_shim.cpp`) that read through the real, unmodified C++
//! `StreamedChunk::frame_read_origin`/`payload_with_trailing_slack()` — U4d relocated the streamed
//! chunk's storage into `deluge_sample_fill::chunk` (Rust) and deleted `frame_read_origin` for the
//! streamed role entirely, so the oracle (`tests/differential.rs`'s `oracle_frame`) now reads a
//! chunk's payload directly through the Rust accessor, and this crate needs no `build.rs`/C++ at
//! all — see that test file's own module doc for the full "U4d note".
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
