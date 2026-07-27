//! The reader's pure state: [`Reader`], a small frame-cursor over one sample's source residency.
//! Task 1 (SR-U1) is lifecycle-only — [`Reader::open`]/[`Reader::seek`], plus ordinary `Drop` for
//! close (see `abi.rs`). `window`/`advance` (the actual streaming reads, which take the first lease
//! on a miss) are later tasks; nothing here does I/O.

use deluge_resource::facade::Lease;

/// Mirrors `include/libdeluge/sample_reader.h`'s `DelugeReadHint`. Crosses the FFI by value (see
/// `abi::deluge_sample_reader_open`), so `#[repr(u8)]` (fixed width) exactly matches the header's
/// explicit `: uint8_t` — the same fixed-width need `deluge_sample_source::abi::DelugeRegionState`
/// documents for its own enum.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ReadHint {
    /// Share the source residency cache — warm clusters the voice already holds are free hits.
    Cached = 0,
    /// One-shot whole-sample scan (waveform pre-scan, wavetable build): do NOT evict/pollute the
    /// voice's warm working set for a read that will never repeat.
    Scan = 1,
}

/// Mirrors `include/libdeluge/sample_source.h`'s `DelugeSampleGeometry` field-for-field, exactly
/// like `deluge_sample_source::geometry::Geometry` does for the voice port — a SEPARATE local
/// mirror rather than a shared dependency on that crate, so the non-voice reader stays decoupled
/// from the voice cursor. Each side of the port re-mirrors the one C struct it needs; this is the
/// established convention in this workspace (compare `deluge_sample_fill::FillContext`'s own
/// independent mirror of its header).
#[repr(C)]
#[derive(Clone, Copy, Default, Debug)]
pub struct Geometry {
    pub audio_data_start_bytes: u32,
    pub audio_data_length_bytes: u64,
    pub cluster_size_bytes: u32,
    pub byte_depth: u8,
    pub num_channels: u8,
    pub raw_data_format: u8,
}

/// A reader's frame-cursor state over one sample's source residency
/// (`include/libdeluge/sample_reader.h`'s `DelugeSampleReader`).
///
/// Task 1 is lifecycle only: `open`/`seek`, plus ordinary `Drop` for close (see `abi.rs`'s
/// `deluge_sample_reader_close`, which just drops the owning `Box`) — no I/O, no `window`/`advance`
/// yet (later tasks). `held_lease` is `None` for this whole task: nothing here ever takes one —
/// the first lease is taken by `window()`'s must-load-now acquire, which does not exist yet.
pub struct Reader {
    asset: u32,
    geometry: Geometry,
    direction: i8,
    hint: ReadHint,
    current_frame: u64,
    // `pub(crate)`, not private: `abi.rs`'s own tests reach through the opaque pointer to attach a
    // real lease directly (mirroring what a future `window()` would do), since there is no public
    // ABI call yet that populates this field. Never exposed outside this crate.
    pub(crate) held_lease: Option<Lease>,
}

impl Reader {
    /// Build a reader over `asset` — the resource-manager Asset id ALREADY defined for this sample
    /// (the header's `source_id`; resolved by the caller via `deluge_streaming_define_asset`, the
    /// SAME asset the voice port's own residency uses — see the header's doc for `source_id`, and
    /// `chunk_residency.cpp`'s `deluge_streaming_define_asset` for how the C++ side obtains it).
    /// Unlike `deluge_sample_source_open`'s `stream_backing` bridge (a `void*` needing a
    /// `deluge_sample_stream_asset_id` FFI round-trip to resolve), `source_id` here already IS that
    /// resolved numeric asset id — the header takes it directly, so `open` does no resolution of
    /// its own beyond storing it.
    ///
    /// Positioned at `start_frame`, reading in `direction` (+1 forward, -1 reverse), with `hint`
    /// steering eviction pressure (see [`ReadHint`]). Pure state construction: no I/O, no lease
    /// taken yet.
    ///
    /// # `geometry`'s gap
    /// `geometry` is zero-initialized here. The header's `open()` signature takes only
    /// `source_id` — deliberately, per the design doc — not a `DelugeSampleGeometry`, and no
    /// per-asset geometry registry keyed by a bare asset id is reachable from this crate today:
    /// the closest existing table, `deluge_sample_fill`'s per-asset fill-context, tracks a
    /// narrower, DIFFERENT record (no `byte_depth`/`num_channels`) and this crate does not depend
    /// on that crate (see this crate's Cargo.toml doc for why the two stayed siblings rather than
    /// folding together). Real geometry is needed before `window()`/`advance()` (a later task) can
    /// do per-frame stride math; resolving it — extending a fill-context-like table, adding a
    /// parallel geometry table, or threading `DelugeSampleGeometry` through `open()`'s own
    /// signature — is an open question for that task, not invented here. Task 1's lifecycle tests
    /// (open/seek/close) never inspect `geometry`'s field values, so the gap does not block them.
    pub fn open(asset: u32, start_frame: u64, direction: i8, hint: ReadHint) -> Self {
        Reader {
            asset,
            geometry: Geometry::default(),
            direction,
            hint,
            current_frame: start_frame,
            held_lease: None,
        }
    }

    /// Random-access reposition (for the both-directions hop search, a later task): release any
    /// held pin — a later `window()` call re-pins at the new position — and move the cursor to
    /// `frame`. Like `open()` without re-allocating a reader.
    pub fn seek(&mut self, frame: u64) {
        self.held_lease = None; // Option::drop releases the lease, if any, exactly once.
        self.current_frame = frame;
    }

    pub fn asset(&self) -> u32 {
        self.asset
    }

    pub fn geometry(&self) -> Geometry {
        self.geometry
    }

    pub fn direction(&self) -> i8 {
        self.direction
    }

    pub fn hint(&self) -> ReadHint {
        self.hint
    }

    pub fn current_frame(&self) -> u64 {
        self.current_frame
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::boxed::Box;
    use deluge_resource::facade::Resource;
    use deluge_resource::value::COST_IO;
    use deluge_resource::DelugeResource;
    extern crate std;
    use core::ffi::c_void;
    use std::vec::Vec;

    const CHUNK_SIZE: usize = 4096;

    /// `construct` seeds a per-index ramp — the same tiny fixture the sibling region-port crates
    /// use (`cursor.rs`/`manager_residency.rs` in `deluge_sample_source`); nothing here reads the
    /// bytes back (Task 1 has no window/read), it just needs SOME construct callback attached so
    /// `Resource::request` can reserve a chunk to mint a real `Lease`.
    unsafe extern "C" fn make_ramp_construct(
        _ctx: *mut c_void,
        _owner: *mut c_void,
        index: u32,
        dest: *mut u8,
    ) {
        for b in 0..CHUNK_SIZE {
            // SAFETY: `dest` is the manager's just-allocated `CHUNK_SIZE`-byte backing for this
            // chunk (per `ConstructFn`'s contract).
            unsafe {
                *dest.add(b) = index.wrapping_add(b as u32) as u8;
            }
        }
    }

    /// Backing arena for the leaked test heap, kept alive for the process's remaining life —
    /// mirrors the sibling crates' own boot-singleton test-harness contract.
    struct TestHeap {
        _buf: Vec<u128>,
    }

    /// Build a manager over a fresh test heap (leaking its backing arena) with one requestable
    /// asset attached, mirroring `deluge_sample_source`'s own test harness.
    fn test_manager_and_asset() -> (*mut DelugeResource, u32) {
        let words = (256 * 1024usize).div_ceil(16);
        let mut buf: Vec<u128> = std::vec![0u128; words];
        let ptr = buf.as_mut_ptr() as *mut u8;
        // SAFETY: `buf` is leaked below so the arena stays alive for the whole test binary's
        // life, matching the sibling crates' own boot-singleton test-harness contract.
        let h = unsafe { deluge_alloc::deluge_heap_create(ptr, words * 16) };
        std::mem::forget(TestHeap { _buf: buf });
        // SAFETY: `h` is the live heap handle just created above.
        let handle = unsafe { deluge_resource::deluge_resource_create(h, 4, 16) };
        assert!(!handle.is_null());
        // SAFETY: `handle` is live; `make_ramp_construct` has the required C-ABI signature.
        let asset = unsafe {
            deluge_resource::deluge_resource_define_asset(
                handle,
                core::ptr::null_mut(),
                None,
                None,
                core::ptr::null_mut(),
                COST_IO,
                deluge_resource::manager::BACKING_HEAP,
            )
        };
        unsafe {
            deluge_resource::deluge_resource_set_construct(
                handle,
                asset,
                Some(make_ramp_construct),
            );
        }
        (handle, asset)
    }

    #[test]
    fn open_sets_the_expected_initial_state_with_no_lease_held() {
        let (_handle, asset) = test_manager_and_asset();
        let reader = Reader::open(asset, 123, -1, ReadHint::Scan);
        assert_eq!(reader.asset(), asset);
        assert_eq!(reader.current_frame(), 123);
        assert_eq!(reader.direction(), -1);
        assert_eq!(reader.hint(), ReadHint::Scan);
        assert!(reader.held_lease.is_none(), "open must not take a lease");
    }

    /// Task 1's central lifecycle assertion: `seek` both updates `current_frame` AND drops
    /// whatever lease the reader is holding — proven with a REAL manager lease (not a stand-in),
    /// since `window()` doesn't exist yet to populate `held_lease` through the public API. The
    /// test reaches the private field directly (this `mod` is a child of `reader`'s own module),
    /// mirroring how a future `window()` would populate it.
    #[test]
    fn seek_drops_the_held_lease_and_updates_current_frame() {
        let (handle, asset) = test_manager_and_asset();
        // SAFETY: `handle` is live for the test's duration.
        let resource = unsafe { Resource::from_handle(handle) };

        let mut reader = Reader::open(asset, 10, 1, ReadHint::Cached);
        assert_eq!(reader.current_frame(), 10);

        let lease = resource.request(asset, 0, CHUNK_SIZE).expect("request");
        let slot = resource.slot_of(lease.chunk());
        assert_eq!(
            resource.lease_count_by_slot(slot),
            1,
            "the fresh request holds exactly one lease before seek"
        );
        reader.held_lease = Some(lease);

        reader.seek(42);

        assert_eq!(reader.current_frame(), 42, "seek must move the cursor");
        assert_eq!(
            resource.lease_count_by_slot(slot),
            0,
            "seek must release the held lease"
        );
    }

    /// `seek` with nothing held is a plain reposition — no lease to drop, no panic.
    #[test]
    fn seek_with_no_held_lease_just_moves_the_cursor() {
        let (_handle, asset) = test_manager_and_asset();
        let mut reader = Reader::open(asset, 0, 1, ReadHint::Cached);
        reader.seek(7);
        assert_eq!(reader.current_frame(), 7);
    }

    /// Close (a later task's C-ABI wrapper just drops the owning `Box<Reader>` — see `abi.rs`)
    /// releases any held lease without leaking, proven here at the `Box`/`Drop` level Task 1
    /// actually implements: a boxed reader (mirroring what `deluge_sample_reader_open` hands out)
    /// is non-null, and dropping it (mirroring `deluge_sample_reader_close`) returns the manager's
    /// lease count to its pre-open baseline.
    #[test]
    fn boxed_open_is_non_null_and_dropping_it_releases_any_held_lease_without_leak() {
        let (handle, asset) = test_manager_and_asset();
        // SAFETY: `handle` is live for the test's duration.
        let resource = unsafe { Resource::from_handle(handle) };

        let mut reader = Reader::open(asset, 0, 1, ReadHint::Cached);
        let lease = resource.request(asset, 1, CHUNK_SIZE).expect("request");
        let slot = resource.slot_of(lease.chunk());
        reader.held_lease = Some(lease);
        assert_eq!(resource.lease_count_by_slot(slot), 1);

        let ptr: *mut Reader = Box::into_raw(Box::new(reader));
        assert!(!ptr.is_null(), "open (boxed) must be non-null");

        // SAFETY: `ptr` was just produced by `Box::into_raw` above and has not been freed yet.
        unsafe { drop(Box::from_raw(ptr)) }; // mirrors deluge_sample_reader_close

        assert_eq!(
            resource.lease_count_by_slot(slot),
            0,
            "close must release the held lease, with no leak"
        );
    }
}
