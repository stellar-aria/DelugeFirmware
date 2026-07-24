//! SR2d's native Rust region backing driven over the SAME op sequences as the C++
//! backing — the differential's SECOND `RegionPortOps` backend. `HarnessResidency`
//! reproduces a `Scenario`'s per-cluster residency (loaded/unloaded/unavailable +
//! `make_ramp` bytes) so the Rust state machine (`deluge_sample_source::SampleSource`)
//! is exercised over identical worlds and its regions compared byte-for-byte.
//!
//! # Lease bookkeeping
//! The C++ side tracks leases in a process-wide refcount table behind the fakes
//! (`region_harness_total_lease_count`); this backend's analogue is a
//! provider-local `Rc<RefCell<Vec<u32>>>`, one slot per cluster index, shared
//! between every `HarnessPin` minted for that index and `HarnessResidency`'s own
//! `retain_token`/`release_token`. A pin's `Drop` decrements its index's slot —
//! lease balance falls out of RAII, exactly like the cursor's own slots — and
//! `RustBackend::total_leases` sums the table, the same "whole-backing lease
//! count" observable the diff compares (counts, never raw token values).
use crate::cpp_backend::Scenario;
use crate::ops::{make_ramp, RawOutcome, Region, RegionPortOps, RegionState, CLUSTER_SIZE};
use deluge_sample_source::cursor::{RegionState as CursorState, SampleSource};
use deluge_sample_source::geometry::Geometry;
use deluge_sample_source::residency::{Get, RegionPin, Residency};
use std::cell::{Cell, RefCell};
use std::rc::Rc;

/// A single cluster index's residency, precomputed from the `Scenario` at
/// `HarnessResidency::new` time — the harness never simulates a fill in flight,
/// so an index's disposition is fixed for the provider's whole life.
enum IndexKind {
    Loaded,
    Loading,
    Unavailable,
}

/// Reproduces one `Scenario`'s per-cluster residency for
/// `SampleSource::acquire_ex` to drive: a loaded cluster acquires `Ready` with a
/// pin over `make_ramp(index, CLUSTER_SIZE)`; an `unloaded` cluster acquires
/// `Loading` (and STAYS `Loading` — nothing in this harness ever lands a fill,
/// matching the C++ side's fake, which never toggles a cluster's loaded flag
/// mid-run); an `unavailable` cluster, or any index at or past `num_clusters`,
/// acquires `Unavailable`.
struct HarnessResidency {
    num_clusters: u32,
    kind: Vec<IndexKind>,
    /// Owned for the backend's whole lifetime so `HarnessPin::payload` pointers,
    /// taken from `ramps[i].as_ptr()`, stay valid — the `Vec`s are built once
    /// here and never resized afterward.
    ramps: Vec<Vec<u8>>,
    /// Shared, provider-local lease refcount table, one slot per index.
    refcounts: Rc<RefCell<Vec<u32>>>,
    /// Monotonic token generation counter, starting at 1 so `(index << 32) |
    /// gen` is never `0` for index `0` (`0` is the "no lease" sentinel the
    /// `RegionPortOps` contract reserves for `retain`/`release` no-ops).
    next_gen: Cell<u32>,
}

impl HarnessResidency {
    fn new(scenario: &Scenario) -> Self {
        let num_clusters = scenario.num_clusters;
        let mut kind = Vec::with_capacity(num_clusters as usize);
        let mut ramps = Vec::with_capacity(num_clusters as usize);
        for i in 0..num_clusters {
            if scenario.unavailable.contains(&i) {
                kind.push(IndexKind::Unavailable);
                ramps.push(Vec::new());
            } else if scenario.unloaded.contains(&i) {
                kind.push(IndexKind::Loading);
                ramps.push(make_ramp(i, CLUSTER_SIZE));
            } else {
                kind.push(IndexKind::Loaded);
                ramps.push(make_ramp(i, CLUSTER_SIZE));
            }
        }
        HarnessResidency {
            num_clusters,
            kind,
            ramps,
            refcounts: Rc::new(RefCell::new(vec![0u32; num_clusters as usize])),
            next_gen: Cell::new(1),
        }
    }

    /// The refcount table, shared with `RustBackend` so `total_leases` can sum it
    /// without the backend holding a reference into `SampleSource`'s private
    /// `residency` field (there is no accessor for it).
    fn refcounts_handle(&self) -> Rc<RefCell<Vec<u32>>> {
        Rc::clone(&self.refcounts)
    }

    /// Mint a pin over `index`, incrementing its refcount slot (a real lease —
    /// mirrors the C++ fake's `add_lease` on every `get_cluster`/reserve).
    fn make_pin(&self, index: u32, ready: bool) -> HarnessPin {
        let gen = self.next_gen.get();
        self.next_gen.set(gen.wrapping_add(1));
        self.refcounts.borrow_mut()[index as usize] += 1;
        HarnessPin {
            index,
            ready,
            payload: self.ramps[index as usize].as_ptr(),
            token: ((index as u64) << 32) | gen as u64,
            refcounts: Rc::clone(&self.refcounts),
        }
    }

    /// Decode `token`'s index and adjust its refcount slot by `delta` — the
    /// `retain_token`/`release_token` shared body. A `0` token or an
    /// out-of-range index (the "stale" case; this harness never actually mints a
    /// token that goes stale, since indices are never reused across a live pin,
    /// but the guard matches the trait's documented no-op contract) is a no-op.
    fn adjust_lease(&self, token: u64, delta: i64) {
        if token == 0 {
            return;
        }
        let index = (token >> 32) as usize;
        let mut rc = self.refcounts.borrow_mut();
        let Some(slot) = rc.get_mut(index) else {
            return;
        };
        *slot = (*slot as i64 + delta).max(0) as u32;
    }
}

impl Residency for HarnessResidency {
    type Pin = HarnessPin;

    fn acquire(&self, index: u32, _priority: u32) -> Get<HarnessPin> {
        if index >= self.num_clusters {
            return Get::Unavailable;
        }
        match self.kind[index as usize] {
            IndexKind::Unavailable => Get::Unavailable,
            IndexKind::Loading => Get::Loading(self.make_pin(index, false)),
            IndexKind::Loaded => Get::Ready(self.make_pin(index, true)),
        }
    }

    fn num_clusters(&self) -> u32 {
        self.num_clusters
    }

    fn retain_token(&self, token: u64) {
        self.adjust_lease(token, 1);
    }

    fn release_token(&self, token: u64) {
        self.adjust_lease(token, -1);
    }
}

/// One pinned cluster: tracks its index, LIVE readiness (fixed at mint time —
/// see `HarnessResidency`'s doc), a pointer into the owning `HarnessResidency`'s
/// `ramps[index]`, and a provider-local `{index, gen}` token. `Drop` releases the
/// lease it was minted with.
struct HarnessPin {
    index: u32,
    ready: bool,
    payload: *const u8,
    token: u64,
    refcounts: Rc<RefCell<Vec<u32>>>,
}

impl RegionPin for HarnessPin {
    fn index(&self) -> u32 {
        self.index
    }
    fn is_ready(&self) -> bool {
        self.ready
    }
    fn payload(&self) -> *const u8 {
        self.payload
    }
    fn token(&self) -> u64 {
        self.token
    }
}

impl Drop for HarnessPin {
    fn drop(&mut self) {
        let mut rc = self.refcounts.borrow_mut();
        if let Some(slot) = rc.get_mut(self.index as usize) {
            *slot = slot.saturating_sub(1);
        }
    }
}

/// The Rust region-port backing over one `SampleSource<HarnessResidency>` —
/// SR2d's plug-in as `ops::RegionPortOps`'s second backend (see that trait's
/// doc). Unlike `CppBackend`, this backing has NO process-wide singleton state,
/// so nothing stops two `RustBackend`s being live at once — but the differential
/// still runs backends strictly sequentially (`crate::diff::capture`), so that
/// never comes up.
pub struct RustBackend {
    source: SampleSource<HarnessResidency>,
    refcounts: Rc<RefCell<Vec<u32>>>,
}

impl RustBackend {
    /// Build a `HarnessResidency` reproducing `scenario` and open a source over
    /// it, with the SAME geometry `CppBackend::open` builds (start 0, cluster
    /// `CLUSTER_SIZE`, depth 2, ch 1, fmt 0).
    pub fn open(scenario: &Scenario) -> Self {
        let residency = HarnessResidency::new(scenario);
        let refcounts = residency.refcounts_handle();
        let geo = Geometry {
            audio_data_start_bytes: 0,
            audio_data_length_bytes: scenario.audio_data_length_bytes,
            cluster_size_bytes: CLUSTER_SIZE as u32,
            byte_depth: 2,
            num_channels: 1,
            raw_data_format: 0,
        };
        let source = SampleSource::new(residency, geo);
        RustBackend { source, refcounts }
    }
}

impl RegionPortOps for RustBackend {
    fn acquire(&mut self, index: u32, direction: i8, priority: u32) -> RawOutcome {
        let (state, out) = self.source.acquire_ex(index, direction, priority);
        let state = match state {
            CursorState::Ready => RegionState::Ready,
            CursorState::Loading => RegionState::Loading,
            CursorState::Unavailable => RegionState::Unavailable,
        };
        let (region, lease) = match out {
            Some(o) => {
                let n = o.resident_bytes as usize;
                // SAFETY: `o.payload_base` is `HarnessPin::payload()` — a pointer
                // into the owning `HarnessResidency`'s `ramps[index]`, a
                // `CLUSTER_SIZE`-byte `Vec` that outlives this backend and is
                // never resized after construction. `resident_bytes_for` (shared
                // geometry math, identical to the C++ side) clamps `n` to at
                // most `CLUSTER_SIZE`, so this read never runs past the ramp.
                let payload = unsafe { std::slice::from_raw_parts(o.payload_base, n).to_vec() };
                let region = Region {
                    region_index: o.region_index,
                    resident_bytes: o.resident_bytes,
                    payload,
                };
                (Some(region), o.lease)
            }
            None => (None, 0),
        };
        RawOutcome {
            state,
            region,
            lease,
        }
    }

    fn state(&self, index: u32) -> RegionState {
        match self.source.state(index) {
            CursorState::Ready => RegionState::Ready,
            CursorState::Loading => RegionState::Loading,
            CursorState::Unavailable => RegionState::Unavailable,
        }
    }

    fn retain(&mut self, lease: u64) {
        self.source.retain(lease);
    }

    fn release(&mut self, lease: u64) {
        self.source.release(lease);
    }

    fn total_leases(&self) -> u32 {
        self.refcounts.borrow().iter().sum()
    }
}

impl Drop for RustBackend {
    fn drop(&mut self) {
        self.source.close();
    }
}
