//! The non-vacuity mechanism: a `RegionPortOps` wrapper that deliberately
//! corrupts ONE captured value, standing in for a hypothetical SR2d Rust backing
//! that diverges from the C++ port. A C++-vs-C++ diff is trivially green (it
//! proves the plumbing, nothing else); wrapping one side in `Perturb` and
//! asserting the diff DETECTS the injected difference is what proves the harness
//! has teeth — a gate that cannot fail on a real difference is worthless.
use crate::ops::{RawOutcome, RegionPortOps, RegionState};

/// Which single value to corrupt, and on which `Ready` acquire (0-based over
/// Ready acquires only, so it targets a specific resident region deterministically).
#[derive(Debug, Clone, Copy)]
pub enum Perturbation {
    /// XOR bit 0 of `payload[byte]` on the `on_ready_ordinal`-th Ready acquire.
    FlipPayloadByte { on_ready_ordinal: usize, byte: usize },
    /// Add one to `resident_bytes` on the `on_ready_ordinal`-th Ready acquire.
    BumpResidentBytes { on_ready_ordinal: usize },
}

/// Wraps any backing and mutates exactly one captured `RawOutcome` per the
/// `Perturbation`. All other ops pass through untouched.
pub struct Perturb<B: RegionPortOps> {
    inner: B,
    perturbation: Perturbation,
    ready_seen: usize,
}

impl<B: RegionPortOps> Perturb<B> {
    pub fn new(inner: B, perturbation: Perturbation) -> Self {
        Perturb { inner, perturbation, ready_seen: 0 }
    }
}

impl<B: RegionPortOps> RegionPortOps for Perturb<B> {
    fn acquire(&mut self, index: u32, direction: i8, priority: u32) -> RawOutcome {
        let mut o = self.inner.acquire(index, direction, priority);
        if o.state == RegionState::Ready {
            let ord = self.ready_seen;
            self.ready_seen += 1;
            if let Some(region) = o.region.as_mut() {
                match self.perturbation {
                    Perturbation::FlipPayloadByte { on_ready_ordinal, byte }
                        if ord == on_ready_ordinal && byte < region.payload.len() =>
                    {
                        region.payload[byte] ^= 0x01;
                    }
                    Perturbation::BumpResidentBytes { on_ready_ordinal } if ord == on_ready_ordinal => {
                        region.resident_bytes = region.resident_bytes.wrapping_add(1);
                    }
                    _ => {}
                }
            }
        }
        o
    }

    fn state(&self, index: u32) -> RegionState {
        self.inner.state(index)
    }
    fn retain(&mut self, lease: u64) {
        self.inner.retain(lease);
    }
    fn release(&mut self, lease: u64) {
        self.inner.release(lease);
    }
    fn total_leases(&self) -> u32 {
        self.inner.total_leases()
    }
}
