//! The region-port cursor's full tri-state `acquire_ex` state machine: `current`
//! (the pinned, loaded region the caller reads), `pending` (a retained LOADING
//! reservation from a prior acquire, so its fill keeps progressing across
//! defer/retry), and `prefetch` (a standing reservation for the neighbouring
//! region in the caller's playback direction, so the next acquire of it is a
//! resident hit). Reimplements `sample_source.cpp:217-366` natively: steps 2-4
//! (current/pending), step 1 (promotion off a prefetch hit), step 5 (neighbour
//! prefetch on the READY path), `deluge_sample_region_state`, and retain/release.
//!
//! Slots hold RAII pins (`Cell<Option<R::Pin>>`); a transition `take`s the outgoing
//! pin (dropping it releases exactly one lease) and `set`s the new one — lease
//! balance falls out of ownership, never hand-balanced. Every touch of a slot (and
//! of `prefetch_index`, its tracked-index sidecar) is wrapped in
//! `deluge_resource::sync::Masked` — the SAME asymmetric critical section the
//! manager's own tables use — because these slots are touched from more than one
//! context: `acquire_ex` from the audio ISR, and `close` from either the main
//! thread (voice teardown) or the render ISR itself (a reader reused via
//! `ensureSource`). `Masked` adapts to the caller's context via a live
//! `deluge_in_interrupt()` check, so the discipline is correct regardless of
//! which thread runs which — it never relies on a fixed thread affinity.
//!
//! Invariant (relied on by `state`'s "at most one slot matches" correctness):
//! `current`, `pending`, and `prefetch` never track the same index simultaneously.
//! The promotion path (which consumes the standing prefetch for the index it
//! matches) and the READY-path prefetch's own dedupe (`prefetch_index != next`)
//! together maintain it.

use crate::geometry::{resident_bytes_for, Geometry};
use crate::residency::{Get, RegionPin, Residency};
use core::cell::Cell;
use deluge_resource::sync::{m_get, Masked};

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
/// keeps progressing across defer/retry), and `prefetch` (a standing reservation
/// for the neighbouring region in the caller's playback direction, tracked by
/// `prefetch_index`, `u32::MAX` = empty). Slots hold RAII pins; a transition drops
/// the outgoing pin (releasing its lease).
pub struct SampleSource<R: Residency> {
    residency: R,
    geo: Geometry,
    current: Cell<Option<R::Pin>>,
    pending: Cell<Option<R::Pin>>,
    prefetch: Cell<Option<R::Pin>>,
    prefetch_index: Cell<u32>,
}

impl<R: Residency> SampleSource<R> {
    pub fn new(residency: R, geo: Geometry) -> Self {
        Self {
            residency,
            geo,
            current: Cell::new(None),
            pending: Cell::new(None),
            prefetch: Cell::new(None),
            prefetch_index: Cell::new(u32::MAX),
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

    /// The index `slot`'s pin tracks, if any. `take`+inspect+`set` under ONE masked
    /// window (not two `m_get`/`m_set`-style calls) — a window split in two would
    /// let a concurrent `close()` observe the slot as transiently `None` and clear
    /// it as a no-op, only for this call's restore to silently revive the pin
    /// `close()` meant to release. Mirrors the manager's own multi-statement
    /// `Masked::enter()` sections (e.g. `manager.rs`'s `evict_lowest`), used
    /// whenever a read must stay coherent with the write that follows it. The
    /// shared primitive behind `current_index` and `state`'s `pending`/`prefetch`
    /// resolution.
    fn slot_index(slot: &Cell<Option<R::Pin>>) -> Option<u32> {
        let _m = Masked::enter();
        let v = slot.take();
        let idx = v.as_ref().map(RegionPin::index);
        slot.set(v);
        idx
    }

    /// The index `current` is pinned to, if any.
    fn current_index(&self) -> Option<u32> {
        Self::slot_index(&self.current)
    }

    /// Masked read of the standing prefetch's tracked index (`u32::MAX` = empty).
    /// A plain `Cell<u32>` read, so this reuses `deluge_resource::sync::m_get`
    /// rather than a bespoke take/set window — the same primitive `manager.rs`
    /// uses for its own `Copy` cells.
    fn prefetch_index(&self) -> u32 {
        m_get(&self.prefetch_index)
    }

    /// Masked swap of the prefetch slot AND its tracked index TOGETHER, so the two
    /// never observably diverge across a window seam (a reader between two
    /// separately-masked writes could otherwise see a pin with a stale/absent
    /// index, or vice versa).
    fn set_prefetch(&self, pin: Option<R::Pin>, idx: u32) {
        let _m = Masked::enter();
        self.prefetch.set(pin); // drops the outgoing pin -> releases its lease
        self.prefetch_index.set(idx);
    }

    /// Step 1 (`sample_source.cpp:229-238`): if the standing prefetch tracks
    /// `index`, take it out and clear `prefetch_index` — the lease transfers from
    /// `prefetch` to this call, so the caller skips a fresh `residency.acquire`.
    /// `None` if there is no hit (leaves `prefetch`/`prefetch_index` untouched).
    /// One masked window over both cells: a read-then-conditionally-take that
    /// `set_prefetch` (a plain swap) can't express.
    fn take_promoted(&self, index: u32) -> Option<R::Pin> {
        let _m = Masked::enter();
        if self.prefetch_index.get() != index {
            return None;
        }
        let pin = self.prefetch.take();
        if pin.is_some() {
            self.prefetch_index.set(u32::MAX);
        }
        pin
    }

    /// Step 5 (`sample_source.cpp:294-309`), READY-path only, called AFTER
    /// `current` is pinned: prefetch the neighbour in `direction`, if in range and
    /// not already the standing prefetch. A `Loading` neighbour is stored as-is
    /// (any state is fine — it just isn't ready yet); `Unavailable` leaves
    /// `prefetch` empty.
    fn prefetch_neighbour(&self, index: u32, direction: i8, priority: u32) {
        debug_assert!(
            direction == 1 || direction == -1,
            "prefetch direction must be ±1; direction 0 would self-prefetch and violate the never-same-index invariant"
        );
        let next_signed = index as i64 + direction as i64;
        if next_signed < 0 {
            return;
        }
        let next = next_signed as u32;
        if next >= self.residency.num_clusters() {
            return;
        }
        if self.prefetch_index() == next {
            return; // already the standing prefetch -- nothing to do
        }
        // Drop any standing prefetch (a different index) BEFORE acquiring the new
        // one, mirroring the C++ contract's ordering.
        self.set_prefetch(None, u32::MAX);
        match self.residency.acquire(next, priority) {
            Get::Unavailable => {
                // Nothing came back; prefetch stays empty (already cleared above).
            }
            Get::Loading(pin) | Get::Ready(pin) => self.set_prefetch(Some(pin), next),
        }
    }

    /// Non-blocking, synchronous: schedules a fill if needed but never waits for
    /// one. `direction` steers the neighbour prefetched on the READY path.
    pub fn acquire_ex(
        &self,
        index: u32,
        direction: i8,
        priority: u32,
    ) -> (RegionState, Option<RegionOut>) {
        // Step 1: a hit on the standing prefetch promotes it in place of a fresh
        // acquire -- same tri-state logic runs on the promoted pin below, keyed by
        // its LIVE readiness (a promoted-but-not-yet-loaded prefetch falls through
        // to the Loading arm, which moves it to `pending` -- exactly the "promotion
        // empties prefetch, moves the reservation to pending" contract).
        let get = match self.take_promoted(index) {
            Some(pin) => {
                if pin.is_ready() {
                    Get::Ready(pin)
                } else {
                    Get::Loading(pin)
                }
            }
            None => self.residency.acquire(index, priority),
        };
        match get {
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
                // Step 5: prefetch the forward/backward neighbour now that
                // `current` holds its new pin.
                self.prefetch_neighbour(index, direction, priority);
                (RegionState::Ready, Some(out))
            }
        }
    }

    /// Masked take/inspect/restore: true iff `slot`'s pin tracks `index` (matched
    /// by `RegionPin::index`, the pin's OWN tracked index — never a side-channel
    /// copy). Leaves the slot untouched.
    fn slot_matches(slot: &Cell<Option<R::Pin>>, index: u32) -> bool {
        let _m = Masked::enter();
        let v = slot.take();
        let m = matches!(&v, Some(p) if p.index() == index);
        slot.set(v);
        m
    }

    /// Masked take/inspect/restore: `Some(Ready|Loading)` iff `slot`'s pin tracks
    /// `index`, resolved by re-reading LIVE `is_ready()` (a pin taken as Loading
    /// may have since landed); `None` if the slot is empty or tracks a different
    /// index. Leaves the slot untouched.
    fn slot_state_if_index(slot: &Cell<Option<R::Pin>>, index: u32) -> Option<RegionState> {
        let _m = Masked::enter();
        let v = slot.take();
        let result = match &v {
            Some(p) if p.index() == index => Some(if p.is_ready() {
                RegionState::Ready
            } else {
                RegionState::Loading
            }),
            _ => None,
        };
        slot.set(v);
        result
    }

    /// Reimplements `deluge_sample_region_state` (`sample_source.cpp:321-352`):
    /// pure observation (no acquire, no lease, no mutation) that resolves `index`
    /// against each slot's OWN tracked index, `current` -> `pending` -> `prefetch`,
    /// with LIVE readiness. `current` is checked first: whenever it is set it is,
    /// by invariant, already loaded (only a ready pin is ever pinned as `current`
    /// -- see `acquire_ex`'s READY arm), so a match there is always `Ready`. By
    /// the cursor's own never-same-index invariant (see the module doc), at most
    /// one of the three checks below can match.
    pub fn state(&self, index: u32) -> RegionState {
        if Self::slot_matches(&self.current, index) {
            return RegionState::Ready;
        }
        if let Some(s) = Self::slot_state_if_index(&self.pending, index) {
            return s;
        }
        if let Some(s) = Self::slot_state_if_index(&self.prefetch, index) {
            return s;
        }
        RegionState::Unavailable
    }

    /// Independent-pin retain/release — forwards the opaque token to the provider
    /// (generation-checked; no-op on 0/stale). Cursor-independent, matching the C
    /// ABI's `deluge_sample_region_retain`/`_release`.
    pub fn retain(&self, token: u64) {
        self.residency.retain_token(token);
    }

    /// See [`Self::retain`].
    pub fn release(&self, token: u64) {
        self.residency.release_token(token);
    }

    pub fn close(&self) {
        Self::set_slot(&self.current, None);
        Self::set_slot(&self.pending, None);
        self.set_prefetch(None, u32::MAX);
    }

    /// The index `pending` is retained for, if any. Test-only peek (used to assert
    /// the never-same-index invariant); production code never needs `pending`'s
    /// index in isolation — `state`'s own resolution goes through
    /// `slot_state_if_index` instead.
    #[cfg(test)]
    fn pending_index(&self) -> Option<u32> {
        Self::slot_index(&self.pending)
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
        // direction=1 (a realistic forward-playback caller — real callers never
        // pass 0, see voice_sample.cpp/sample_low_level_reader.cpp's playDirection)
        // so each READY acquire also prefetches its forward neighbour (step 5);
        // the lease-count assertions below account for that extra standing lease
        // alongside the original current/fuse/dedupe assertions.
        let (src, h, asset) = new_source(4);
        mark_index_ready(h, asset, 0);

        let (state, out) = src.acquire_ex(0, 1, 0);
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
        // current(0) + the neighbour prefetch of index 1 the READY path just
        // scheduled (not yet marked ready, so it sits as a Loading prefetch).
        assert_eq!(
            total_leases(h),
            2,
            "one current lease + one standing prefetch lease after first Ready"
        );

        // Advance to index 1: the standing prefetch from above already tracks it,
        // so this is a PROMOTE (no fresh acquire) once it's marked ready — the fuse
        // of the old current(0) is the only lease this step drops; the prefetch of
        // the NEW neighbour (index 2) it schedules next is the only lease it adds.
        mark_index_ready(h, asset, 1);
        let (state1, out1) = src.acquire_ex(1, 1, 0);
        assert_eq!(state1, RegionState::Ready);
        let out1 = out1.expect("ready must fill out");
        assert_eq!(out1.region_index, 1);
        assert_eq!(
            total_leases(h),
            2,
            "advancing current promotes+fuses net zero; prefetching index 2 adds one"
        );

        // Re-acquire index 1 (already current): idempotent dedupe (the fresh
        // cache-hit lease taken to resolve it is dropped immediately), and
        // out.lease/payload equal the standing current's. The standing prefetch of
        // index 2 is untouched (already the standing prefetch — step 5 no-ops).
        let (state1b, out1b) = src.acquire_ex(1, 1, 0);
        assert_eq!(state1b, RegionState::Ready);
        let out1b = out1b.expect("ready must fill out");
        assert_eq!(
            total_leases(h),
            2,
            "re-acquiring the current index is net zero; the standing prefetch is untouched"
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

    /// A READY acquire prefetches its forward neighbour (step 5); the NEXT acquire
    /// of that neighbour is a PROMOTE — no fresh `residency.acquire` — and stays
    /// lease-balanced (the lease simply transfers from `prefetch` to `current`).
    #[test]
    fn ready_prefetches_forward_neighbour_and_next_acquire_promotes_it() {
        // num_clusters=2 so index 1's own forward neighbour (index 2) is out of
        // range: after the promote below, no further prefetch fires, keeping the
        // final lease count a clean, unambiguous 1.
        let (src, h, asset) = new_source(2);
        mark_index_ready(h, asset, 0);
        mark_index_ready(h, asset, 1);

        let (state0, _out0) = src.acquire_ex(0, 1, 0);
        assert_eq!(state0, RegionState::Ready);
        assert_eq!(
            total_leases(h),
            2,
            "current(0) + the prefetched neighbour(1)"
        );
        assert_eq!(
            src.state(1),
            RegionState::Ready,
            "neighbour 1 is prefetched and already ready"
        );

        // Promote: index 1 is the standing prefetch, so this must NOT take a fresh
        // lease -- if it mistakenly did (a fresh cache-hit acquire, then fused),
        // the net lease count after would be identical (a fresh Ready acquire nets
        // the same 2->1 via its own fuse+dedupe), so the real tell is the
        // never-same-index invariant: a buggy non-promoting path would leave the
        // OLD prefetch(1) still standing alongside the new current(1), i.e. total
        // would stay 2 with current and prefetch both tracking index 1. Promotion
        // instead transfers the lease, so current(0) is fused away and nothing
        // remains to prefetch (index 2 is out of range) -- total drops to 1.
        let (state1, out1) = src.acquire_ex(1, 1, 0);
        assert_eq!(state1, RegionState::Ready);
        let out1 = out1.expect("ready must fill out");
        assert_eq!(out1.region_index, 1);
        assert_eq!(
            total_leases(h),
            1,
            "promotion transfers the prefetch lease into current -- no new reservation"
        );
        assert_eq!(
            src.state(0),
            RegionState::Unavailable,
            "old current(0) was fused away"
        );
        assert_eq!(src.state(1), RegionState::Ready, "current(1), promoted");
    }

    /// The discriminating truthfulness case: an indexed `state` query must resolve
    /// by each slot's OWN tracked index, never by assuming "the standing prefetch
    /// is always index+1 of the last acquire". After a jump ahead of the standing
    /// prefetch, `state` of the prefetch's neighbour must report what is ACTUALLY
    /// tracked (nothing, here -- LOADING never triggers step 5), not the jumped-to
    /// index's own Loading state.
    #[test]
    fn indexed_state_is_truthful_after_jump_ahead_of_prefetch() {
        let (src, h, asset) = new_source(8);
        mark_index_ready(h, asset, 0);

        // acquire(0) -> READY, prefetches neighbour 1 (not marked ready -> Loading).
        let (state0, _out0) = src.acquire_ex(0, 1, 0);
        assert_eq!(state0, RegionState::Ready);

        // acquire(5) jumps past the standing prefetch: LOADING (pending=5). The
        // LOADING path never runs step 5, so the standing prefetch(1) is untouched
        // -- nothing this cursor tracks ever becomes 6.
        let (state5, out5) = src.acquire_ex(5, 1, 0);
        assert_eq!(state5, RegionState::Loading);
        assert!(out5.is_none());

        assert_eq!(src.state(0), RegionState::Ready, "current");
        assert_eq!(
            src.state(5),
            RegionState::Loading,
            "pending, the jumped-to index"
        );
        assert_eq!(
            src.state(1),
            RegionState::Loading,
            "the ORIGINAL prefetch neighbour, still standing and untouched by the jump"
        );
        assert_eq!(
            src.state(6),
            RegionState::Unavailable,
            "the TRUE neighbour of 5 -- nothing tracks it, since acquire(5) was LOADING \
             (step 5 never ran) and must NOT be conflated with index 5's own Loading state"
        );
    }

    /// The three-slot resolution order (`current` -> `pending` -> `prefetch`) with
    /// LIVE readiness: a slot's match is re-queried at `state()` time, not cached
    /// from whenever it was last touched.
    #[test]
    fn state_matches_current_pending_prefetch_by_tracked_index() {
        let (src, h, asset) = new_source(4);
        mark_index_ready(h, asset, 0);

        let (state0, _out0) = src.acquire_ex(0, 1, 0); // Ready(current=0), prefetch(1)=Loading
        assert_eq!(state0, RegionState::Ready);
        let (state2, _out2) = src.acquire_ex(2, 1, 0); // Loading(pending=2); prefetch(1) untouched
        assert_eq!(state2, RegionState::Loading);

        assert_eq!(src.state(0), RegionState::Ready, "current");
        assert_eq!(src.state(2), RegionState::Loading, "pending");
        assert_eq!(
            src.state(1),
            RegionState::Loading,
            "prefetch, not yet landed"
        );
        assert_eq!(src.state(3), RegionState::Unavailable, "tracked by no slot");

        // Land the prefetch's chunk (simulating the loader) without touching the
        // cursor at all -- `state` must re-observe it live, not report a cached
        // Loading.
        mark_index_ready(h, asset, 1);
        assert_eq!(
            src.state(1),
            RegionState::Ready,
            "prefetch's readiness is a LIVE re-check, not cached at acquire time"
        );
    }

    /// `retain`/`release` forward the opaque lease token to the provider
    /// (balance +1/-1); `close` drops current+pending+prefetch, returning the
    /// manager's total lease count to its pre-acquire baseline.
    #[test]
    fn retain_release_balance_and_close_drops_all_slots() {
        let (src, h, asset) = new_source(2);
        assert_eq!(total_leases(h), 0, "baseline");
        mark_index_ready(h, asset, 0);

        let (state, out) = src.acquire_ex(0, 1, 0); // Ready(current=0), prefetch(1)=Loading
        assert_eq!(state, RegionState::Ready);
        let out = out.expect("ready must fill out");
        assert_eq!(total_leases(h), 2, "current + standing prefetch");

        src.retain(out.lease);
        assert_eq!(total_leases(h), 3, "retain adds an independent lease");
        src.release(out.lease);
        assert_eq!(total_leases(h), 2, "release drops it back");

        src.close();
        assert_eq!(
            total_leases(h),
            0,
            "close drops current+pending+prefetch back to baseline"
        );
    }

    /// A caller jumping to a different index while an earlier LOADING reservation
    /// is still outstanding supersedes it without leaking: the old pending is
    /// dropped, the new one leased, net one pending lease throughout.
    #[test]
    fn jump_ahead_supersedes_pending_without_leak() {
        let (src, h, _asset) = new_source(8);

        let (state3, out3) = src.acquire_ex(3, 1, 0); // not resident -> Loading, pending=3
        assert_eq!(state3, RegionState::Loading);
        assert!(out3.is_none());
        assert_eq!(total_leases(h), 1, "one pending lease");

        let (state7, out7) = src.acquire_ex(7, 1, 0); // jump ahead -> Loading, pending=7
        assert_eq!(state7, RegionState::Loading);
        assert!(out7.is_none());
        assert_eq!(
            total_leases(h),
            1,
            "the old pending(3) is released, the new pending(7) leased -- net one"
        );
        assert_eq!(
            src.state(3),
            RegionState::Unavailable,
            "superseded, no longer tracked"
        );
        assert_eq!(src.state(7), RegionState::Loading, "the new pending");
    }

    /// `current`, `pending`, and `prefetch` must never track the same index at
    /// once (the invariant `state`'s "at most one slot matches" correctness relies
    /// on) -- exercised across a representative sequence covering a Ready-path
    /// prefetch, a jump past it, a promote-to-Ready, and a promote-to-Loading
    /// (moved into `pending`, per the "promotion empties prefetch, moves the
    /// reservation to pending" contract).
    #[test]
    fn slots_never_track_the_same_index() {
        fn assert_distinct(src: &SampleSource<ManagerResidency>) {
            let cur = src.current_index();
            let pending = src.pending_index();
            let pre = src.prefetch_index();
            let pre = (pre != u32::MAX).then_some(pre);
            let tracked: Vec<u32> = [cur, pending, pre].into_iter().flatten().collect();
            let mut sorted = tracked.clone();
            sorted.sort_unstable();
            sorted.dedup();
            assert_eq!(
                sorted.len(),
                tracked.len(),
                "slots must track pairwise-distinct indices: current={cur:?} pending={pending:?} prefetch={pre:?}"
            );
        }

        let (src, h, asset) = new_source(8);
        mark_index_ready(h, asset, 0);

        src.acquire_ex(0, 1, 0); // Ready(current=0), prefetch(1)=Loading
        assert_distinct(&src);

        src.acquire_ex(5, 1, 0); // jump ahead -> Loading(pending=5); prefetch(1) untouched
        assert_distinct(&src);

        mark_index_ready(h, asset, 1);
        src.acquire_ex(1, 1, 0); // promote(1) -> Ready: fuses pending(5) away, fuses current(0),
        assert_distinct(&src); // prefetches(2)=Loading

        src.acquire_ex(2, 1, 0); // promote(2), not ready -> Loading: moves into pending, empties prefetch
        assert_distinct(&src);

        mark_index_ready(h, asset, 2);
        src.acquire_ex(2, 1, 0); // fresh acquire (prefetch is empty) -> Ready: fuses pending(2) away,
        assert_distinct(&src); // prefetches(3)=Loading
    }

    /// SR3f: ports `sample_source_spec.cpp`'s "8b: a scheduled-but-unloaded region
    /// is LOADING and its lease is RETAINED across the call" -- specifically its
    /// tail ("when the fill lands, the same retry becomes READY and the retained
    /// lease folds into the current pin -- exactly one lease, no duplicate from
    /// the retries"). `acquire_loading_retains_pending_and_leaves_current` never
    /// marks the index ready, so this landing fold was previously exercised only
    /// by the (now-deleted) C++ mirror and its differential harness -- this test
    /// is now its sole coverage.
    #[test]
    fn loading_retry_that_lands_ready_folds_into_a_single_lease() {
        let (src, h, asset) = new_source(4);

        // Not yet resident: reserved -> Loading, the lease retained across the call.
        let (state, out) = src.acquire_ex(1, 1, 0);
        assert_eq!(state, RegionState::Loading);
        assert!(out.is_none());
        assert_eq!(total_leases(h), 1, "pending holds exactly one lease");

        // A defer/retry cycle before the fill lands is idempotent: still Loading,
        // still exactly one lease.
        let (state2, out2) = src.acquire_ex(1, 1, 0);
        assert_eq!(state2, RegionState::Loading);
        assert!(out2.is_none());
        assert_eq!(
            total_leases(h),
            1,
            "retrying before landing must stay one lease"
        );

        // The fill lands; the same retry now reports Ready and folds the retained
        // pending lease into `current` -- no duplicate from the retries. The READY
        // path's own neighbour prefetch (direction=1) adds the only other lease.
        mark_index_ready(h, asset, 1);
        let (state3, out3) = src.acquire_ex(1, 1, 0);
        assert_eq!(state3, RegionState::Ready);
        let out3 = out3.expect("ready must fill out");
        assert_eq!(out3.region_index, 1);
        assert_eq!(
            total_leases(h),
            2,
            "current(1), folded from the retained pending, plus the fresh prefetch(2)"
        );
    }

    /// SR3f: ports `sample_source_spec.cpp`'s "8b: a pending LOADING region that
    /// becomes unreservable is dropped, not stranded" -- exercises the
    /// `Get::Unavailable` arm's `release_pending()` with a REAL outstanding
    /// pending lease already in hand, unlike the zero-baseline
    /// `acquire_unavailable_leaves_nothing_leased`. The real `ManagerResidency`
    /// has no lever to make an ALREADY-reserved (leased) index turn unreservable
    /// mid-flight -- a live lease can't be evicted -- so an out-of-range index
    /// stands in for "the next acquire reports Unavailable"; the load-bearing
    /// claim (an outstanding pending lease is released, not stranded) is the
    /// same either way. Also locks in the C++ spec's "lease == 0 is a no-op"
    /// case for `retain`/`release` against a live (here, zero) balance.
    #[test]
    fn pending_lease_is_dropped_not_stranded_when_the_next_acquire_is_unavailable() {
        let (src, h, _asset) = new_source(4);

        let (state, out) = src.acquire_ex(1, 1, 0); // not resident -> Loading, pending=1
        assert_eq!(state, RegionState::Loading);
        assert!(out.is_none());
        assert_eq!(total_leases(h), 1, "pending holds exactly one lease");

        let (state2, out2) = src.acquire_ex(100, 1, 0); // out of range -> Unavailable
        assert_eq!(state2, RegionState::Unavailable);
        assert!(out2.is_none());
        assert_eq!(
            total_leases(h),
            0,
            "the outstanding pending lease is released, not stranded"
        );

        // lease == 0 is a no-op in both directions and does not disturb the balance.
        src.retain(0);
        src.release(0);
        assert_eq!(total_leases(h), 0);
    }

    /// SR3f: ports `sample_source_spec.cpp`'s "8b Task 1: a LOADING acquire that
    /// consumes the standing prefetch still reports a truthful state for that
    /// same index (the in-flight `pending` reservation, not UNAVAILABLE)".
    /// `take_promoted`'s "falls through to the Loading arm, which moves it to
    /// pending" contract (see `acquire_ex`'s step-1 doc) is exercised by
    /// `slots_never_track_the_same_index`'s sequence, but that test only checks
    /// the never-same-index invariant there, never that `state()` stays truthful
    /// (LOADING, not UNAVAILABLE) while the promoted reservation sits in `pending`.
    #[test]
    fn promoted_prefetch_that_is_not_yet_ready_lands_in_pending_and_state_stays_truthful() {
        let (src, h, asset) = new_source(2);
        mark_index_ready(h, asset, 0);

        // acquire(0) -> Ready, prefetches neighbour 1 (not marked ready -> Loading).
        let (state0, _out0) = src.acquire_ex(0, 1, 0);
        assert_eq!(state0, RegionState::Ready);
        assert_eq!(
            src.state(1),
            RegionState::Loading,
            "standing prefetch, not yet landed"
        );

        // Acquire exactly the prefetched index while it is still not ready: this
        // promotes prefetch(1) -- `take_promoted` hits -- but since the pin isn't
        // ready yet, it falls through to the Loading arm and moves into `pending`,
        // emptying `prefetch`.
        let (state1, out1) = src.acquire_ex(1, 1, 0);
        assert_eq!(state1, RegionState::Loading);
        assert!(out1.is_none());

        // THE load-bearing assertion: index 1 is now tracked by `pending`, not
        // lost in the prefetch->pending handoff -- state() must still say
        // LOADING, never UNAVAILABLE (which would tell a deferring caller nothing
        // is in flight, when a fill genuinely still is).
        assert_eq!(src.state(1), RegionState::Loading);
        assert_eq!(total_leases(h), 2, "current(0) + the promoted pending(1)");

        // It lands -- the same indexed query reports READY, still without acquiring.
        mark_index_ready(h, asset, 1);
        assert_eq!(src.state(1), RegionState::Ready);

        // And the retry that actually acquires it lands cleanly: the pending
        // lease folds into `current`, no leak, no duplicate. num_clusters=2, so
        // index 1 has no in-range forward neighbour to prefetch, keeping the
        // final count an unambiguous single lease.
        let (state1b, out1b) = src.acquire_ex(1, 1, 0);
        assert_eq!(state1b, RegionState::Ready);
        let out1b = out1b.expect("ready must fill out");
        assert_eq!(out1b.region_index, 1);
        assert_eq!(
            total_leases(h),
            1,
            "current(1) only -- old current(0) fused, pending folded, nothing further to prefetch"
        );
    }
}
