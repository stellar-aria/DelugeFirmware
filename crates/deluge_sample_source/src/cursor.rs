//! The region-port cursor's tri-state `acquire_ex` core: `current` (the pinned,
//! loaded region the caller reads) and `pending` (a retained LOADING reservation
//! from a prior acquire, so its fill keeps progressing across defer/retry).
//! Reimplements `sample_source.cpp:217-292` steps 2-4 natively (prefetch/promotion
//! is a later task — this cursor has no `prefetch` slot yet).
//!
//! Slots hold RAII pins (`Cell<Option<R::Pin>>`); a transition `take`s the outgoing
//! pin (dropping it releases exactly one lease) and `set`s the new one — lease
//! balance falls out of ownership, never hand-balanced. Every touch of a slot is
//! wrapped in `deluge_resource::sync::Masked` — the SAME asymmetric critical
//! section the manager's own tables use — because these slots are touched from
//! both the audio ISR (`acquire_ex`) and the main thread (`close`).

use crate::geometry::{resident_bytes_for, Geometry};
use crate::residency::{Get, RegionPin, Residency};
use core::cell::Cell;
use deluge_resource::sync::Masked;

/// Mirrors `DelugeRegionState`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RegionState {
    Ready = 1,
    Loading = 2,
    Unavailable = 3,
}

/// Mirrors `DelugeSampleRegion`: the region descriptor `acquire_ex` fills on
/// `RegionState::Ready`.
pub struct RegionOut {
    pub payload_base: *const u8,
    pub region_index: u32,
    pub resident_bytes: u32,
    pub lease: u64,
}

/// One region-port cursor: `current` (the pinned, loaded region the caller reads),
/// `pending` (a retained LOADING reservation from a prior acquire, so its fill
/// keeps progressing across defer/retry), and — added in the prefetch task —
/// `prefetch`. Slots hold RAII pins; a transition drops the outgoing pin
/// (releasing its lease).
pub struct SampleSource<R: Residency> {
    residency: R,
    geo: Geometry,
    current: Cell<Option<R::Pin>>,
    pending: Cell<Option<R::Pin>>,
    // prefetch + prefetch_index added in Task 4.
}

impl<R: Residency> SampleSource<R> {
    pub fn new(residency: R, geo: Geometry) -> Self {
        Self {
            residency,
            geo,
            current: Cell::new(None),
            pending: Cell::new(None),
        }
    }

    /// Masked swap of a slot: `take`s the outgoing pin under the mask and drops it
    /// (inside the same window `Cell::set` performs the replace-and-drop), then
    /// installs `v`. A single-cell O(1) window, matching the manager's own `m_set`
    /// granularity (`deluge_resource::sync::m_set`) — just not `Copy`-bounded, since
    /// a pin isn't `Copy`.
    fn set_slot(slot: &Cell<Option<R::Pin>>, v: Option<R::Pin>) {
        let _m = Masked::enter();
        slot.set(v);
    }

    /// Drop any retained LOADING reservation (release its lease). Called on all
    /// three outcome paths AFTER this call's own pin is in hand — so a retry
    /// landing on the same chunk nets exactly one lease (the provider's cache-hit
    /// already re-leased).
    fn release_pending(&self) {
        Self::set_slot(&self.pending, None); // drops the Option's pin -> releases its lease
    }

    /// The index `current` is pinned to, if any. `take`+inspect+`set` under ONE
    /// masked window (not two `m_get`/`m_set`-style calls) — a window split in two
    /// would let a concurrent `close()` observe `current` as transiently `None` and
    /// clear it as a no-op, only for this call's restore to silently revive the pin
    /// `close()` meant to release. Mirrors the manager's own multi-statement
    /// `Masked::enter()` sections (e.g. `manager.rs`'s `evict_lowest`), used
    /// whenever a read must stay coherent with the write that follows it.
    fn current_index(&self) -> Option<u32> {
        let _m = Masked::enter();
        let cur = self.current.take();
        let idx = cur.as_ref().map(RegionPin::index);
        self.current.set(cur);
        idx
    }

    /// Non-blocking, synchronous: schedules a fill if needed but never waits for
    /// one. `_direction` is accepted for signature parity with the prefetch task
    /// but unused here (no prefetch slot yet).
    pub fn acquire_ex(
        &self,
        index: u32,
        _direction: i8,
        priority: u32,
    ) -> (RegionState, Option<RegionOut>) {
        // (Task 4 inserts the prefetch-promotion shortcut here.)
        match self.residency.acquire(index, priority) {
            Get::Unavailable => {
                // Nothing was leased for this region — there is no fill in flight —
                // and any earlier pending reservation is moot, so drop that too.
                self.release_pending();
                (RegionState::Unavailable, None)
            }
            Get::Loading(pin) => {
                // Retain as pending; leave current alone (the caller may still be
                // reading the region it already has). release_pending AFTER the new
                // pin is in hand, so a retry on the same chunk nets exactly one lease.
                self.release_pending();
                Self::set_slot(&self.pending, Some(pin));
                (RegionState::Loading, None)
            }
            Get::Ready(pin) => {
                // Any pending reservation is superseded — if it is this very chunk
                // (the retry that finally landed) this drops the duplicate lease the
                // provider's acquire just added, leaving the single lease `current`
                // is about to hold.
                self.release_pending();
                let token = pin.token();
                let payload_base = pin.payload();
                // Fuse-release the old current unless it is THIS index already
                // (dedupe: re-acquiring the current index must stay a single lease).
                // Identity is by index — the cursor's slots never track the same
                // index at once.
                let same = matches!(self.current_index(), Some(i) if i == index);
                if same {
                    // The provider's acquire took a SECOND lease on the chunk already
                    // pinned as `current` (a cache-hit re-leases). Drop THIS new pin
                    // so `current` keeps its single original lease — that is what
                    // makes re-acquiring the same index truly idempotent. payload_base
                    // and token were already read off this pin above, and since it is
                    // the SAME chunk as the standing `current`, they are identical to
                    // current's — reading them before this drop is correct.
                    drop(pin);
                } else {
                    Self::set_slot(&self.current, Some(pin)); // drops old current -> fuses its lease
                }
                let out = RegionOut {
                    payload_base,
                    region_index: index,
                    resident_bytes: resident_bytes_for(index, &self.geo),
                    lease: token,
                };
                (RegionState::Ready, Some(out))
            }
        }
    }

    pub fn close(&self) {
        Self::set_slot(&self.current, None);
        Self::set_slot(&self.pending, None);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::geometry::Geometry;
    use crate::manager_residency::ManagerResidency;
    extern crate std;

    use core::ffi::c_void;
    use deluge_resource::value::COST_IO;
    use deluge_resource::DelugeResource;
    use std::vec::Vec;

    const CLUSTER_SIZE: usize = 16;
    const CHUNK_CAP: usize = 16;

    /// `construct` seeds a per-index ramp: `dest[b] = index as u8 + b as u8`.
    unsafe extern "C" fn make_ramp_construct(
        _ctx: *mut c_void,
        _owner: *mut c_void,
        index: u32,
        dest: *mut u8,
    ) {
        for b in 0..CLUSTER_SIZE {
            // SAFETY: `dest` is the manager's just-allocated `CLUSTER_SIZE`-byte
            // backing for this chunk (per `ConstructFn`'s contract).
            unsafe {
                *dest.add(b) = index.wrapping_add(b as u32) as u8;
            }
        }
    }

    /// Struct backing the leaked test heap arena (kept alive for the process's
    /// remaining life — mirrors `ManagerResidency`'s boot-singleton contract).
    struct TestHeap {
        _buf: Vec<u128>,
    }

    /// Build a manager over a fresh test heap and leak its backing arena, mirroring
    /// `manager_residency::tests::test_manager_handle`.
    fn test_manager_handle() -> *mut DelugeResource {
        let words = (256 * 1024usize).div_ceil(16);
        let mut buf: Vec<u128> = std::vec![0u128; words];
        let ptr = buf.as_mut_ptr() as *mut u8;
        // SAFETY: `buf` is leaked below so the arena stays alive for the whole test
        // binary's life, matching `ManagerResidency::new`'s boot-singleton contract.
        let h = unsafe { deluge_alloc::deluge_heap_create(ptr, words * 16) };
        std::mem::forget(TestHeap { _buf: buf });
        // SAFETY: `h` is the live heap handle just created above.
        let handle = unsafe { deluge_resource::deluge_resource_create(h, 4, CHUNK_CAP) };
        assert!(!handle.is_null());
        handle
    }

    /// Define a requestable asset whose `construct` seeds `make_ramp(index, 16)`
    /// (no `materialize` — this cursor drives readiness through `mark_index_ready`,
    /// standing in for the loader, which is a later task).
    fn define_ramp_asset(h: *mut DelugeResource) -> u32 {
        // SAFETY: `h` is a live handle (from `test_manager_handle`); the callback
        // has the required C-ABI signature.
        let asset = unsafe {
            deluge_resource::deluge_resource_define_asset(
                h,
                core::ptr::null_mut(),
                None,
                None,
                core::ptr::null_mut(),
                COST_IO,
                deluge_resource::manager::BACKING_HEAP,
            )
        };
        // SAFETY: `h`/`asset` are live/valid per the call above.
        unsafe {
            deluge_resource::deluge_resource_set_construct(h, asset, Some(make_ramp_construct));
        }
        asset
    }

    /// Drive readiness through the manager directly (standing in for the loader
    /// this provider only schedules — real fill is a later task). Releases the
    /// lease it takes to reserve/find the chunk, so it doesn't skew a test's
    /// subsequent lease-count assertions.
    fn mark_index_ready(h: *mut DelugeResource, asset: u32, index: u32) {
        // SAFETY: `h` is a live handle; these are the same FFI-safe C-ABI calls the
        // facade wraps.
        let ptr = unsafe { deluge_resource::deluge_resource_try_acquire(h, asset, index) };
        let ptr = if ptr.is_null() {
            // SAFETY: `h`/`asset` are live/valid.
            let p =
                unsafe { deluge_resource::deluge_resource_request(h, asset, index, CLUSTER_SIZE) };
            assert!(!p.is_null(), "reserve for mark-ready failed");
            p
        } else {
            ptr
        };
        // SAFETY: `h`/`ptr` are live/valid per the calls above.
        unsafe { deluge_resource::deluge_resource_mark_ready(h, ptr) };
        // SAFETY: `h`/`ptr` are live/valid.
        unsafe { deluge_resource::deluge_resource_release(h, ptr) };
    }

    /// Sum of every slot's live lease count across the manager's whole chunk table
    /// — the lease-balance oracle: RAII pins should never leak or double-release,
    /// so this total must move by exactly the amount each test expects.
    fn total_leases(h: *mut DelugeResource) -> u64 {
        (0..CHUNK_CAP as u32)
            // SAFETY: `h` is a live handle; `lease_count_by_slot` tolerates any
            // slot index (returns 0 out of range), per its own doc.
            .map(|slot| unsafe {
                deluge_resource::deluge_resource_lease_count_by_slot(h, slot) as u64
            })
            .sum()
    }

    fn test_geo(num_clusters: u32) -> Geometry {
        Geometry {
            audio_data_start_bytes: 0,
            audio_data_length_bytes: (num_clusters as u64) * (CLUSTER_SIZE as u64),
            cluster_size_bytes: CLUSTER_SIZE as u32,
            byte_depth: 2,
            num_channels: 1,
            raw_data_format: 0,
        }
    }

    /// A `SampleSource<ManagerResidency>` over a fresh test heap + ramp asset, plus
    /// the raw handle/asset for driving readiness / reading lease counts.
    fn new_source(num_clusters: u32) -> (SampleSource<ManagerResidency>, *mut DelugeResource, u32) {
        let h = test_manager_handle();
        let asset = define_ramp_asset(h);
        let geo = test_geo(num_clusters);
        // SAFETY: `h` is a live handle for the test's duration (leaked arena).
        let residency = unsafe { ManagerResidency::new(h, asset, CLUSTER_SIZE, num_clusters) };
        (SampleSource::new(residency, geo), h, asset)
    }

    #[test]
    fn acquire_unavailable_leaves_nothing_leased() {
        let (src, h, _asset) = new_source(4);
        let (state, out) = src.acquire_ex(100, 0, 0); // out of range -> Unavailable
        assert_eq!(state, RegionState::Unavailable);
        assert!(out.is_none());
        assert_eq!(total_leases(h), 0, "unavailable must leak/hold nothing");
    }

    #[test]
    fn acquire_loading_retains_pending_and_leaves_current() {
        let (src, h, _asset) = new_source(4);
        let (state, out) = src.acquire_ex(0, 0, 0); // not resident -> reserved -> Loading
        assert_eq!(state, RegionState::Loading);
        assert!(out.is_none());
        assert_eq!(total_leases(h), 1, "pending holds exactly one lease");

        // Re-acquire the same (still-loading) index: idempotent, still Loading,
        // still exactly one pending lease (the old pending is dropped, the fresh
        // cache-hit lease from get_cluster/acquire takes its place).
        let (state2, out2) = src.acquire_ex(0, 0, 0);
        assert_eq!(state2, RegionState::Loading);
        assert!(out2.is_none());
        assert_eq!(
            total_leases(h),
            1,
            "re-acquiring a loading index must stay one pending lease"
        );
    }

    #[test]
    fn acquire_ready_pins_current_fills_out_and_fuses_old_current() {
        let (src, h, asset) = new_source(4);
        mark_index_ready(h, asset, 0);

        let (state, out) = src.acquire_ex(0, 0, 0);
        assert_eq!(state, RegionState::Ready);
        let out0 = out.expect("ready must fill out");
        assert_eq!(out0.region_index, 0);
        assert_eq!(out0.resident_bytes, CLUSTER_SIZE as u32);
        // payload_base = the seeded ramp for index 0: dest[b] = 0 + b.
        for b in 0..CLUSTER_SIZE {
            // SAFETY: `payload_base` is the manager's live, ready, leased backing
            // for cluster 0, at least `CLUSTER_SIZE` bytes (per this test's geometry
            // and `deluge_resource_lease_count_by_slot`'s pinning contract).
            let byte = unsafe { *out0.payload_base.add(b) };
            assert_eq!(byte, b as u8, "ramp byte {b} of cluster 0");
        }
        assert_eq!(total_leases(h), 1, "one current lease after first Ready");

        // Advance to index 1 (ready): index 0's lease is fused (released); still
        // exactly one current lease.
        mark_index_ready(h, asset, 1);
        let (state1, out1) = src.acquire_ex(1, 0, 0);
        assert_eq!(state1, RegionState::Ready);
        let out1 = out1.expect("ready must fill out");
        assert_eq!(out1.region_index, 1);
        assert_eq!(
            total_leases(h),
            1,
            "advancing current must fuse the old lease, not add one"
        );

        // Re-acquire index 1 (already current): idempotent dedupe, still exactly
        // one lease, and out.lease/payload equal the standing current's.
        let (state1b, out1b) = src.acquire_ex(1, 0, 0);
        assert_eq!(state1b, RegionState::Ready);
        let out1b = out1b.expect("ready must fill out");
        assert_eq!(
            total_leases(h),
            1,
            "re-acquiring the current index must stay a single lease"
        );
        assert_eq!(
            out1b.lease, out1.lease,
            "dedupe: same token as standing current"
        );
        assert_eq!(
            out1b.payload_base, out1.payload_base,
            "dedupe: same payload as standing current"
        );
    }
}
