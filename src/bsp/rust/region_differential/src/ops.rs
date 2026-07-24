//! The shared op vocabulary and the backend-agnostic `RegionPortOps` trait both
//! region-port backings are driven through.
//!
//! Everything in this module is deliberately backend-independent: the C++
//! backing (`crate::cpp_backend`) and SR2d's future Rust backing implement the
//! SAME `RegionPortOps`, and the diff (`crate::diff`) compares only the
//! backend-agnostic values captured here — never a raw lease pointer, which is
//! process-/backend-specific and meaningless across the two.

/// Cluster payload size, in bytes. Matches `Cluster::size` in
/// `sample_source_test_support.cpp` (16) — the geometry the C++ port's
/// `payload()` and the ramp fixture both agree on.
pub const CLUSTER_SIZE: usize = 16;

/// Residency outcome of a region query, mirroring `DelugeRegionState`
/// (`include/libdeluge/sample_source.h`). Values match the C ABI (1/2/3) so a
/// divergence report reads against a legible constant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RegionState {
    Ready = 1,
    Loading = 2,
    Unavailable = 3,
}

impl RegionState {
    /// Map the raw `DelugeRegionState` (as returned across the C ABI) to this enum.
    pub fn from_raw(raw: i32) -> Self {
        match raw {
            1 => RegionState::Ready,
            2 => RegionState::Loading,
            3 => RegionState::Unavailable,
            other => panic!("unknown DelugeRegionState {other}"),
        }
    }
}

/// One acquired, resident region, captured in a backend-agnostic form: the
/// payload BYTES are copied out at capture time (`resident_bytes` of them),
/// never the raw borrowed `payload_base` pointer or the opaque lease token —
/// both of which are backend-specific and cannot be diffed across two backings.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Region {
    pub region_index: u32,
    pub resident_bytes: u32,
    /// `resident_bytes` bytes copied out of the region's `payload_base`.
    pub payload: Vec<u8>,
}

/// The raw result of a single `acquire`, as a backing hands it back: the
/// residency state, the resident region (only on `Ready`), and the opaque lease
/// token. The lease is carried so the DRIVER can feed it to a later
/// `Retain`/`Release` op — it is NEVER placed in the diffed record (see
/// `crate::diff`).
#[derive(Debug, Clone)]
pub struct RawOutcome {
    pub state: RegionState,
    pub region: Option<Region>,
    pub lease: u64,
}

/// A single operation replayed identically against every backing. The op
/// vocabulary the SR2d differential shares.
#[derive(Debug, Clone)]
pub enum Op {
    /// Make the region containing `index` resident-or-scheduled, pin it, and
    /// report the outcome. `direction` is +1 (forward) / -1 (reverse) prefetch.
    Acquire { index: u32, direction: i8, priority: u32 },
    /// Pure observation of `index`'s residency — no lease, no fetch.
    State { index: u32 },
    /// The caller's independent-pin retain on the LAST acquired lease.
    Retain,
    /// The caller's independent-pin release on the LAST acquired lease.
    Release,
}

/// The interface every region-port backing presents to the differential driver.
///
/// # SR2d plug-in point
/// This trait is the seam SR2d slots into. SR2d's native Rust region backing
/// implements `RegionPortOps` as a SECOND backend; the C++ backend
/// (`crate::cpp_backend::CppBackend`) and the diff (`crate::diff`) are UNCHANGED.
/// Because the driver runs ONE backend at a time (run → capture → drop, then the
/// next), swapping in the Rust backing as backend B needs no other change — and
/// sidesteps the C++ singletons (`g_source_pool`, the process-wide lease table)
/// that forbid two C++ backings being live at once.
///
/// A backing is expected to open exactly one source over a world built by the
/// harness (see `crate::cpp_backend::Scenario`) and close it on `Drop`.
pub trait RegionPortOps {
    /// Acquire the region containing `index`; pin it and report the outcome.
    fn acquire(&mut self, index: u32, direction: i8, priority: u32) -> RawOutcome;
    /// Residency of `index` right now, without acquiring anything.
    fn state(&self, index: u32) -> RegionState;
    /// Take an independent pin on `lease` (no-op if `lease == 0`).
    fn retain(&mut self, lease: u64);
    /// Drop an independent pin on `lease` (no-op if `lease == 0`).
    fn release(&mut self, lease: u64);
    /// Total held-lease count across the backing right now — the observable
    /// side-effect that makes retain/release/close hygiene diffable.
    fn total_leases(&self) -> u32;
}

/// The cluster-index-dependent, byte-distinguishable seed pattern — a Rust
/// replica of `make_ramp` in `tests/spec_audio_stream/sample_source_spec.cpp`:
/// cluster `index`'s byte `k` is `(index * 100 + k) & 0xFF`. Both backings seed
/// clusters with this, and the diff compares captured payloads against it, so a
/// backing that returns the WRONG cluster's data is caught.
pub fn make_ramp(cluster_index: u32, n: usize) -> Vec<u8> {
    (0..n)
        .map(|k| ((cluster_index as usize * 100 + k) & 0xFF) as u8)
        .collect()
}
