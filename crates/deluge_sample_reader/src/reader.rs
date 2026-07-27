//! The reader's pure state: [`Reader`], a small frame-cursor over one sample's source residency.
//! Task 1 (SR-U1) landed lifecycle-only — `open`/`seek`, plus ordinary `Drop` for close (see
//! `abi.rs`). Task 3 fills in the read core: `open` now resolves real geometry (Step 0), and
//! `window`/`advance` do the actual streaming reads — must-load-now acquire, a composed
//! synchronous fill on a miss, and a self-pinning single held lease.

use core::ffi::c_void;

use deluge_resource::facade::Lease;
use deluge_resource::{DelugeResource, Resource};

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

/// "Still recording, length unknown" sentinel (`sample_recorder.cpp`), identical to
/// `deluge_sample_source::geometry::UNKNOWN_LENGTH_SENTINEL` and `deluge_sample_fill::fill_logic`'s
/// own copy — kept as its own literal here too rather than importing either crate, matching this
/// workspace's established "each side re-mirrors the one constant it needs" convention (see this
/// module's own `Geometry` doc).
const UNKNOWN_LENGTH_SENTINEL: u64 = 0x8FFF_FFFF_FFFF_FFFF;

/// Valid payload bytes for cluster `index` of `geo`'s audio-data byte-stream — mirrored verbatim
/// from `deluge_sample_source::geometry::resident_bytes_for` (itself reimplemented natively from
/// `sample_source.cpp:149-168`), NOT imported: this crate deliberately stays independent of
/// `deluge_sample_source` (see this crate's Cargo.toml doc), so the short-last-cluster arithmetic
/// is re-derived here rather than shared. Full cluster size for a full cluster, the remainder for a
/// short last cluster, `0` for a cluster wholly past the end, and full cluster under the
/// unknown-length sentinel / zero length (still-recording samples, or a geometry not yet finalized).
fn resident_bytes_for(index: u32, geo: &Geometry) -> u32 {
    let cluster_size = geo.cluster_size_bytes;
    if geo.audio_data_length_bytes == 0 || geo.audio_data_length_bytes == UNKNOWN_LENGTH_SENTINEL {
        return cluster_size;
    }
    let audio_data_end = geo.audio_data_length_bytes + geo.audio_data_start_bytes as u64;
    let start_this_cluster = index as u64 * cluster_size as u64;
    if audio_data_end <= start_this_cluster {
        return 0;
    }
    let bytes_to_read = audio_data_end - start_this_cluster;
    if bytes_to_read < cluster_size as u64 {
        bytes_to_read as u32
    } else {
        cluster_size
    }
}

/// One resolved frame position: which cluster it falls in, its byte offset within that cluster,
/// and how many bytes of that cluster are valid audio data (`resident_bytes_for`). Mirrors
/// `sample.cpp`'s own frame->cluster mapping (`getPlayByteLowLevel`/the perc-cache scan, both
/// derived from `sourceBytePos = audioDataStartPosBytes + startPosSamples * bytesPerSample`,
/// `sourceClusterIndex = sourceBytePos >> Cluster::size_magnitude`, `bytePosWithinCluster =
/// sourceBytePos & (Cluster::size - 1)`): `geo.cluster_size_bytes` is always a real `Cluster::size`
/// (a power of two), so plain `/`/`%` reproduce that shift/mask exactly, without needing a
/// `cluster_size_magnitude` field on this crate's own `Geometry` mirror. `None` only for a
/// malformed geometry (zero frame stride or zero cluster size) — never for an ordinary past-the-end
/// position, which instead resolves normally with `resident == 0` (the caller's EOF signal).
fn locate(frame: u64, geo: &Geometry) -> Option<(u32, u32, u32)> {
    let frame_stride = geo.byte_depth as u64 * geo.num_channels as u64;
    if frame_stride == 0 || geo.cluster_size_bytes == 0 {
        return None;
    }
    let abs_byte_pos =
        (geo.audio_data_start_bytes as u64).saturating_add(frame.saturating_mul(frame_stride));
    let cluster_size = geo.cluster_size_bytes as u64;
    let cluster_index = (abs_byte_pos / cluster_size).min(u32::MAX as u64) as u32;
    let byte_offset = (abs_byte_pos % cluster_size) as u32;
    let resident = resident_bytes_for(cluster_index, geo);
    Some((cluster_index, byte_offset, resident))
}

/// Run the synchronous cluster fill on `chunk_backing`: resolve where/how much to read
/// (`native_begin`), read exactly that span in ONE call (mirrors the real synchronous fill path,
/// `SampleStream::read_cluster_data` -> `EfatfsReadSource::read`, which issues a single
/// `deluge_efatfs_read_at` over the whole `num_sectors*512`-byte span rather than looping sector by
/// sector — see `read_source.cpp`), then run the post-read convert/stitch/publish tail
/// (`native_finish`). This is U1's own composition — C2a (`deluge_sample_fill`) provided the two
/// primitives, not the glue between them; `deluge-bsp-rust`'s async fill task composes them the
/// same way, just with an `.await`ed read in place of this synchronous one.
///
/// Returns `native_finish`'s own result — `false` on a geometry-resolution failure (`!d.ok`), a
/// failed/short read, or a failed convert/stitch/publish; `true` once the chunk is converted,
/// stitched, and published ready.
fn fill_now(chunk_backing: *mut c_void) -> bool {
    let d = deluge_sample_fill::native_begin(chunk_backing);
    if !d.ok {
        return false;
    }
    let count = d.num_sectors * 512;
    let mut out_read: u32 = 0;
    // SAFETY: `d.dest` is this chunk's just-allocated payload buffer (resolved by `native_begin`
    // from the same registered geometry that sized this chunk's backing), valid for at least
    // `count` bytes; `out_read` is a valid local out-param.
    let read_ok = unsafe {
        deluge_efatfs_read_at(
            d.handle,
            d.byte_offset,
            d.dest as *mut c_void,
            count,
            &mut out_read,
        )
    };
    deluge_sample_fill::native_finish(chunk_backing, read_ok)
}

unsafe extern "C" {
    /// The single process-wide resource-manager singleton (`include/libdeluge/streaming_fill.h`).
    /// Resolved once at `open()` time and cached — the same boot-singleton contract
    /// `deluge_sample_source::manager_residency::ManagerResidency::new` requires of its own handle.
    fn deluge_streaming_resource_manager() -> *mut c_void;
    /// A resident chunk's payload base (`backing + kChunkPayloadOffset`), the single source of
    /// truth for the payload offset (SR2d-4 lesson: never assume payload == backing).
    fn deluge_streaming_chunk_payload(chunk_backing: *mut c_void) -> *mut u8;
    /// The synchronous card read (`include/libdeluge/streaming_fill.h`) — see [`fill_now`].
    fn deluge_efatfs_read_at(
        handle: u32,
        byte_offset: u32,
        dst: *mut c_void,
        count: u32,
        out_read: *mut u32,
    ) -> bool;
}

/// A reader's frame-cursor state over one sample's source residency
/// (`include/libdeluge/sample_reader.h`'s `DelugeSampleReader`).
pub struct Reader {
    asset: u32,
    geometry: Geometry,
    direction: i8,
    hint: ReadHint,
    current_frame: u64,
    /// `false` if `asset` had no registered fill-context at `open()` time (geometry stayed
    /// zeroed — a reader over an unregistered asset can't map frames), OR if a later `window()`
    /// hit a malformed-geometry / acquire-or-fill failure. Distinct from EOF (`frame_count == 0`
    /// with `ok` still `true`) — see `deluge_sample_reader_ok`'s header doc.
    ok: bool,
    /// The process-wide resource-manager handle, resolved once at `open()` (see the
    /// `deluge_streaming_resource_manager` extern above).
    manager: *mut c_void,
    // `pub(crate)`, not private: `abi.rs`'s own tests reach through the opaque pointer to attach a
    // real lease directly, since Task 1 landed before `window()` existed to populate this field
    // through the public API. Never exposed outside this crate.
    pub(crate) held_lease: Option<Lease>,
    /// The cluster index `held_lease` is pinned to, if any — `window()`'s self-pin bookkeeping:
    /// `advance()` drops the held lease exactly when the cursor's new position leaves this index.
    held_cluster: Option<u32>,
}

impl Reader {
    /// Build a reader over `asset` — the resource-manager Asset id ALREADY defined for this sample
    /// (the header's `source_id`; resolved by the caller via `deluge_streaming_define_asset`, the
    /// SAME asset the voice port's own residency uses — see the header's doc for `source_id`, and
    /// `chunk_residency.cpp`'s `deluge_streaming_define_asset` for how the C++ side obtains it).
    ///
    /// Positioned at `start_frame`, reading in `direction` (+1 forward, -1 reverse), with `hint`
    /// steering eviction pressure (see [`ReadHint`]). Pure state construction plus one table
    /// lookup: no I/O, no lease taken yet.
    ///
    /// # Geometry resolution (Task 3 Step 0)
    /// The header's `open()` signature takes only `source_id` — deliberately, per the design doc:
    /// geometry is resolved RUST-SIDE from the asset, not passed by the caller. This looks up
    /// `asset`'s per-asset fill-context (`deluge_sample_fill::fill_context_for`, the SAME table
    /// `deluge_streaming_set_fill_context` registers once at sample-load and the native fill
    /// already reads) and maps its fields onto this reader's own `Geometry` mirror. If `asset` has
    /// no registered context (a reader opened before the sample's first stream use, or over a bare
    /// asset id with nothing registered), `geometry` stays zeroed and the reader is marked
    /// not-ok — it cannot map frames without real geometry, and `window()` must not pretend
    /// otherwise.
    pub fn open(asset: u32, start_frame: u64, direction: i8, hint: ReadHint) -> Self {
        // SAFETY: returns the one process-wide resource-manager singleton; a stable pointer, no
        // aliasing/ownership concern (mirrors deluge_sample_fill::native's own call).
        let manager = unsafe { deluge_streaming_resource_manager() };
        let (geometry, ok) = match deluge_sample_fill::fill_context_for(asset) {
            Some(ctx) => (
                Geometry {
                    audio_data_start_bytes: ctx.audio_data_start_pos_bytes,
                    audio_data_length_bytes: ctx.audio_data_length_bytes,
                    cluster_size_bytes: ctx.cluster_size,
                    byte_depth: ctx.byte_depth,
                    num_channels: ctx.num_channels,
                    raw_data_format: ctx.raw_data_format,
                },
                true,
            ),
            None => (Geometry::default(), false),
        };
        Reader {
            asset,
            geometry,
            direction,
            hint,
            current_frame: start_frame,
            ok,
            manager,
            held_lease: None,
            held_cluster: None,
        }
    }

    /// Random-access reposition (for the both-directions hop search, a later task): release any
    /// held pin — a later `window()` call re-pins at the new position — and move the cursor to
    /// `frame`. Like `open()` without re-allocating a reader.
    pub fn seek(&mut self, frame: u64) {
        self.held_lease = None; // Option::drop releases the lease, if any, exactly once.
        self.held_cluster = None;
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

    /// See `deluge_sample_reader_ok`'s header doc.
    pub fn ok(&self) -> bool {
        self.ok
    }

    /// `deluge_resource_acquire`'s must-load-now shape, composed from the facade's own primitives
    /// (the facade has no single blocking "acquire, materializing via a caller-supplied
    /// synchronous fill" call — see this crate's Task 3 report for why `Manager::acquire`'s real
    /// C-ABI counterpart isn't reused instead): a cache hit (`acquire_leased`) returns immediately;
    /// a miss `request`s a fresh reservation (construct-only, mirroring the async prefetch path's
    /// OWN asset registration) and then runs the fill SYNCHRONOUSLY in-line rather than handing it
    /// to the async loader, so this call never returns until the cluster is genuinely resident —
    /// matching `window()`'s "BLOCKS to make the data resident" contract. `None` on a reservation
    /// failure or a failed fill (the reservation's lease is dropped either way, so nothing leaks).
    ///
    /// # `DELUGE_READ_SCAN`'s hint
    /// The design calls for `DELUGE_READ_SCAN` to pass the manager a low-value/no-retain hint so a
    /// one-shot scan doesn't win eviction over the voice's warm working set. Checked against the
    /// facade/manager surface this crate can reach (`acquire_leased`/`request`/`try_acquire`/`peek`/
    /// `loader_enqueue`): none of them takes a per-acquire eviction-value or recency-bypass
    /// parameter today — eviction rank is purely a function of the ASSET's registered cost, size,
    /// and recency (`deluge_resource::value::evict_rank`), and `loader_enqueue`'s own `priority`
    /// only orders the async fill queue, which this synchronous path never uses. So `hint` is
    /// stored on `Reader` (from Task 1) and forwarded through the public API, but has no manager
    /// lever to act on yet — an honest, derived-not-invented gap, not silently dropped.
    fn acquire_and_fill(&self, cluster_index: u32) -> Option<Lease> {
        if self.manager.is_null() {
            // No manager yet (e.g. boot ordering) -- nothing to acquire through. Mirrors
            // `deluge_sample_source_open`'s own null-manager guard.
            return None;
        }
        // SAFETY: `self.manager` is non-null (checked above) and, per the boot-singleton contract
        // `deluge_streaming_resource_manager` documents, live for the process's remaining life.
        let resource = unsafe { Resource::from_handle(self.manager as *mut DelugeResource) };
        if let Some(lease) = resource.acquire_leased(self.asset, cluster_index) {
            return Some(lease);
        }
        let lease = resource.request(
            self.asset,
            cluster_index,
            self.geometry.cluster_size_bytes as usize,
        )?;
        let backing = lease.chunk().as_ptr() as *mut c_void;
        if !fill_now(backing) {
            return None; // `lease` drops here, releasing the failed reservation.
        }
        resource.mark_ready(lease.chunk());
        Some(lease)
    }

    /// The contiguous run of valid, already-converted frames at the cursor. See the header doc for
    /// `deluge_sample_reader_window`'s full contract.
    ///
    /// Maps `current_frame` to its `(cluster_index, byte_offset, resident_bytes)` via [`locate`];
    /// `byte_offset >= resident_bytes` is end-of-audio (`{null, 0}`, `ok` untouched — this is the
    /// ordinary, expected way a forward/backward scan terminates, not a failure). Otherwise
    /// must-load-now acquires (and, on a miss, synchronously fills) the cluster via
    /// [`Self::acquire_and_fill`], replacing any prior held lease (ONE held pin — the self-pin
    /// invariant), and returns a pointer into it plus how many WHOLE frames remain resident in the
    /// reader's own `direction` from here — forward: to the end of this cluster's valid audio data;
    /// backward: back to the start of this cluster. Deliberately conservative: this never reaches
    /// into a cluster's trailing stitch-slack even where the boundary stitch would make a
    /// straddling frame safe to read from here (a real optimization the byte-identity differential,
    /// Task 5, may motivate later) — every frame this reports is wholly inside `resident_bytes`, so
    /// it's correct regardless of whether a neighbour has been stitched yet.
    ///
    /// An acquire/fill failure sets `ok = false` and returns `{null, 0}` — distinct from the EOF
    /// case above (which leaves `ok` alone).
    pub fn window(&mut self) -> (*const u8, u32) {
        if !self.ok {
            return (core::ptr::null(), 0);
        }
        let Some((cluster_index, byte_offset, resident)) =
            locate(self.current_frame, &self.geometry)
        else {
            self.ok = false;
            return (core::ptr::null(), 0);
        };
        if byte_offset >= resident {
            return (core::ptr::null(), 0); // End-of-audio; not a failure.
        }
        let frame_stride = (self.geometry.byte_depth as u32) * (self.geometry.num_channels as u32);
        let frame_count = if self.direction >= 0 {
            (resident - byte_offset) / frame_stride
        } else {
            byte_offset / frame_stride + 1
        };
        if frame_count == 0 {
            return (core::ptr::null(), 0); // Only a partial trailing frame remains; treat as EOF.
        }
        if self.held_cluster != Some(cluster_index) {
            let Some(lease) = self.acquire_and_fill(cluster_index) else {
                self.ok = false;
                self.held_lease = None;
                self.held_cluster = None;
                return (core::ptr::null(), 0);
            };
            self.held_lease = Some(lease);
            self.held_cluster = Some(cluster_index);
        }
        // SAFETY: `held_lease` (just confirmed `Some` above) pins a resident chunk for as long as
        // this reader holds it; `deluge_streaming_chunk_payload` returns that chunk's payload base.
        let payload_base = unsafe {
            deluge_streaming_chunk_payload(
                self.held_lease.as_ref().unwrap().chunk().as_ptr() as *mut c_void
            )
        };
        // SAFETY: `byte_offset < resident <= cluster_size_bytes`, so this stays within the pinned
        // chunk's own payload span.
        let frames = unsafe { payload_base.add(byte_offset as usize) };
        (frames as *const u8, frame_count)
    }

    /// Advance the cursor `frames` frames in this reader's own `direction`, saturating at 0 (never
    /// underflows) and at the sample's own total frame count (never runs away past the end on
    /// repeated forward advances). Crosses cluster boundaries internally: if the new position
    /// leaves the currently-held backing's cluster, the held lease is dropped (self-pin release) —
    /// the next `window()` re-pins at the new position.
    pub fn advance(&mut self, frames: u32) {
        let delta = frames as i64 * self.direction as i64;
        self.current_frame = if delta >= 0 {
            self.current_frame.saturating_add(delta as u64)
        } else {
            self.current_frame.saturating_sub((-delta) as u64)
        };
        if self.ok
            && self.geometry.audio_data_length_bytes != 0
            && self.geometry.audio_data_length_bytes != UNKNOWN_LENGTH_SENTINEL
        {
            let frame_stride = self.geometry.byte_depth as u64 * self.geometry.num_channels as u64;
            if let Some(total_frames) = self
                .geometry
                .audio_data_length_bytes
                .checked_div(frame_stride)
            {
                if self.current_frame > total_frames {
                    self.current_frame = total_frames;
                }
            }
        }
        if let Some(held) = self.held_cluster {
            let still_within = matches!(
                locate(self.current_frame, &self.geometry),
                Some((cluster_index, _, _)) if cluster_index == held
            );
            if !still_within {
                self.held_lease = None; // Option::drop releases the lease, if any.
                self.held_cluster = None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::host_streaming_stubs::{set_active_manager, TEST_LOCK};
    use alloc::boxed::Box;
    use deluge_resource::facade::Resource;
    use deluge_resource::value::COST_IO;
    use deluge_resource::DelugeResource;
    use deluge_sample_fill::{deluge_streaming_set_fill_context, FillContext};
    extern crate std;
    use core::ffi::c_void;
    use std::vec::Vec;

    const CHUNK_SIZE: usize = 4096;

    /// `construct` seeds a per-index ramp — the same tiny fixture the sibling region-port crates
    /// use (`cursor.rs`/`manager_residency.rs` in `deluge_sample_source`); nothing here reads the
    /// bytes back (these lifecycle tests never call `window()`), it just needs SOME construct
    /// callback attached so `Resource::request` can reserve a chunk to mint a real `Lease`.
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

    /// A `FillContext` compatible with `CHUNK_SIZE` (4096 = 2^12) — just enough for `Reader::open`
    /// (Step 0) to resolve real geometry (`ok() == true`); these lifecycle tests never call
    /// `window()`, so the exact values beyond `cluster_size`/`cluster_size_magnitude` don't matter.
    fn lifecycle_fill_context() -> FillContext {
        FillContext {
            efatfs_handle: 0,
            audio_data_start_pos_bytes: 0,
            audio_data_length_bytes: 0, // unknown/irrelevant here -> resident_bytes_for treats as unbounded
            first_cluster_index_with_no_audio_data: -1,
            cluster_size: CHUNK_SIZE as u32,
            cluster_size_magnitude: 12, // 2^12 == 4096 == CHUNK_SIZE
            raw_data_format: 0,
            byte_depth: 2,
            num_channels: 1,
        }
    }

    /// Build a manager over a fresh test heap (leaking its backing arena) with one requestable
    /// asset attached, mirroring `deluge_sample_source`'s own test harness. Also registers the
    /// asset's fill-context (Step 0's own requirement: `Reader::open` now resolves geometry from
    /// this table) and routes this thread's `deluge_streaming_resource_manager()` stub to `handle`
    /// — every caller must hold [`TEST_LOCK`] for its whole run, since both the fill-context table
    /// and the stub's active-manager cell are shared, process-wide state (see their own docs).
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
        deluge_streaming_set_fill_context(core::ptr::null_mut(), asset, lifecycle_fill_context());
        set_active_manager(handle as *mut c_void);
        (handle, asset)
    }

    #[test]
    fn open_sets_the_expected_initial_state_with_no_lease_held() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let (_handle, asset) = test_manager_and_asset();
        let reader = Reader::open(asset, 123, -1, ReadHint::Scan);
        assert_eq!(reader.asset(), asset);
        assert_eq!(reader.current_frame(), 123);
        assert_eq!(reader.direction(), -1);
        assert_eq!(reader.hint(), ReadHint::Scan);
        assert!(reader.held_lease.is_none(), "open must not take a lease");
        assert!(
            reader.ok(),
            "geometry must resolve from the registered fill-context (Step 0)"
        );
    }

    /// Task 1's central lifecycle assertion: `seek` both updates `current_frame` AND drops
    /// whatever lease the reader is holding — proven with a REAL manager lease (not a stand-in),
    /// attached directly to `held_lease` (this `mod` is a child of `reader`'s own module),
    /// mirroring how `window()` itself populates it.
    #[test]
    fn seek_drops_the_held_lease_and_updates_current_frame() {
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
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
        let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
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

    /// `resident_bytes_for`'s own mirror-check: identical shape to
    /// `deluge_sample_source::geometry::resident_bytes_for`'s own unit test, proving this crate's
    /// independent re-derivation (see this fn's doc for why it isn't shared) matches.
    #[test]
    fn resident_bytes_for_full_clusters_short_last_and_sentinel() {
        fn geo(len: u64) -> Geometry {
            Geometry {
                audio_data_start_bytes: 0,
                audio_data_length_bytes: len,
                cluster_size_bytes: 16,
                byte_depth: 2,
                num_channels: 1,
                raw_data_format: 0,
            }
        }
        let g = geo(68); // 4*16 + 4 -> clusters 0..3 full, cluster 4 has 4 bytes
        assert_eq!(resident_bytes_for(0, &g), 16);
        assert_eq!(resident_bytes_for(3, &g), 16);
        assert_eq!(resident_bytes_for(4, &g), 4);
        assert_eq!(resident_bytes_for(5, &g), 0);
        let s = geo(UNKNOWN_LENGTH_SENTINEL);
        assert_eq!(resident_bytes_for(99, &s), 16);
        let z = geo(0);
        assert_eq!(resident_bytes_for(0, &z), 16);
    }

    // ── Task 3: window/advance over a real synthetic fill (the read core + self-pin) ───────────
    //
    // A synthetic sample: cluster_size=512 bytes, byte_depth=2/num_channels=1 (stride=2, so a
    // 512-byte cluster holds exactly 256 frames), RawDataFormat::Native (0) so
    // `deluge_sample_fill::native_finish`'s convert/stitch tail never rewrites `self`'s own
    // payload (see the module doc's derivation) — the fill stub's deterministic ramp
    // (`byte_offset + i`, `lib.rs`'s `host_streaming_stubs::deluge_efatfs_read_at`) survives to
    // `window()` untouched, so a test can assert on it directly. Real `deluge_resource` manager,
    // real `deluge_sample_fill::{native_begin, native_finish}`, real (if synthetic) chunks — not
    // seeded payload == backing (`PAYLOAD_OFFSET` is a real nonzero front guard) — per the SR2d-4
    // lesson this task's brief calls out.
    mod window_tests {
        use super::*;
        use crate::host_streaming_stubs::{set_force_read_failure, PAYLOAD_OFFSET};

        // 512, not a smaller power of two: `fill_logic::begin`'s sector math is
        // `cluster_size >> 9` (512-byte sectors, mirroring the real `deluge_efatfs_read_at`
        // granularity) — a cluster smaller than one sector would report `num_sectors == 0` and
        // `fill_now` would never read anything. Real `Cluster::size` is always far larger (a real
        // FAT cluster, KBs); 512 is the smallest size that keeps this synthetic sample honest.
        const CLUSTER_SIZE: u32 = 512;
        const CLUSTER_MAGNITUDE: u32 = 9; // 2^9 == 512
        /// Real per-chunk backing needs `PAYLOAD_OFFSET` (front guard) plus `CLUSTER_SIZE`
        /// (payload) plus 7 more bytes of trailing slack that `native_finish` always touches (see
        /// its own `payload_len` doc), totalling 535 bytes; rounded up to 544 (a 16-byte multiple)
        /// for the slab, mirroring the real `kChunkPayloadOffset`-sized slab slots production
        /// configures for `BACKING_SLAB` assets (`deluge_alloc::slab`'s own 16-byte-aligned-slot
        /// contract — see `deluge_resource::lib`'s `slab_backed_asset_acquires_evicts_reloads`
        /// test).
        const BACKING_SIZE: usize = 544;

        const _: () = assert!(
            BACKING_SIZE >= PAYLOAD_OFFSET + CLUSTER_SIZE as usize + 7,
            "BACKING_SIZE must fit the front guard + payload + trailing slack"
        );

        unsafe extern "C" fn noop_construct(
            _ctx: *mut c_void,
            _owner: *mut c_void,
            _index: u32,
            _dest: *mut u8,
        ) {
            // Nothing to initialize: the payload is populated by `fill_now`'s synthetic read, not
            // by `construct` (which only runs to satisfy `Resource::request`'s "requestable asset
            // needs a construct callback" precondition — see `manager_residency.rs`'s own tests).
        }

        struct TestHeap {
            _buf: Vec<u128>,
        }

        fn geo(audio_data_length_bytes: u64) -> FillContext {
            FillContext {
                efatfs_handle: 0,
                audio_data_start_pos_bytes: 0,
                audio_data_length_bytes,
                first_cluster_index_with_no_audio_data: -1,
                cluster_size: CLUSTER_SIZE,
                cluster_size_magnitude: CLUSTER_MAGNITUDE,
                raw_data_format: 0, // Native
                byte_depth: 2,
                num_channels: 1,
            }
        }

        /// Build a manager+asset over a `BACKING_SIZE`-slot-size slab (mirroring production's own
        /// `DELUGE_RESOURCE_BACKING_SLAB` streaming-cluster assets — `chunk_residency.cpp`'s
        /// `deluge_streaming_define_asset` — rather than `BACKING_HEAP`: a slab's per-slot size is
        /// fixed at slab-creation time and the `size` argument `Reader::acquire_and_fill` passes to
        /// `request` is ignored, exactly like the real `kSlabBackedSizeIgnored` convention
        /// `sample_residency.cpp` documents), register `ctx` as the asset's fill-context, and route
        /// the stub's active manager — see `test_manager_and_asset` (the parent module) for why
        /// every caller must hold [`TEST_LOCK`] for its whole run.
        fn harness(ctx: FillContext) -> (*mut DelugeResource, u32) {
            let words = (256 * 1024usize).div_ceil(16);
            let mut buf: Vec<u128> = std::vec![0u128; words];
            let ptr = buf.as_mut_ptr() as *mut u8;
            // SAFETY: leaked below, alive for the test binary's whole life.
            let h = unsafe { deluge_alloc::deluge_heap_create(ptr, words * 16) };
            std::mem::forget(TestHeap { _buf: buf });
            // SAFETY: `h` is the live heap handle just created above; `BACKING_SIZE`/16 chunk slots
            // is ample for this module's own tests (at most 2 clusters resident at once).
            let slab =
                unsafe { deluge_alloc::slab::deluge_slab_create_unmanaged(h, BACKING_SIZE, 16) };
            assert!(!slab.is_null());
            // SAFETY: `h` is the live heap handle just created above.
            let handle = unsafe { deluge_resource::deluge_resource_create(h, 4, 16) };
            assert!(!handle.is_null());
            // SAFETY: `handle`/`slab` are both live, over the same heap.
            unsafe { deluge_resource::deluge_resource_set_slab(handle, slab) };
            // SAFETY: `handle` is live; `noop_construct` has the required C-ABI signature.
            let asset = unsafe {
                deluge_resource::deluge_resource_define_asset(
                    handle,
                    core::ptr::null_mut(),
                    None,
                    None,
                    core::ptr::null_mut(),
                    COST_IO,
                    deluge_resource::manager::BACKING_SLAB,
                )
            };
            unsafe {
                deluge_resource::deluge_resource_set_construct(handle, asset, Some(noop_construct));
            }
            deluge_streaming_set_fill_context(core::ptr::null_mut(), asset, ctx);
            set_active_manager(handle as *mut c_void);
            (handle, asset)
        }

        /// The synthetic fill's expected post-fill bytes for cluster `index`: the
        /// `deluge_efatfs_read_at` stub's ramp, `(index * CLUSTER_SIZE + i) as u8`.
        fn expected_cluster_bytes(index: u32) -> Vec<u8> {
            let base = index * CLUSTER_SIZE;
            (0..CLUSTER_SIZE)
                .map(|i| base.wrapping_add(i) as u8)
                .collect()
        }

        #[test]
        fn window_at_frame_zero_matches_the_synthetic_fill_and_reports_frames_to_backing_end() {
            let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            set_force_read_failure(false);
            let (_handle, asset) = harness(geo(3 * CLUSTER_SIZE as u64)); // 3 full clusters

            let mut reader = Reader::open(asset, 0, 1, ReadHint::Cached);
            assert!(
                reader.ok(),
                "geometry must resolve from the registered fill-context"
            );

            let (ptr, frame_count) = reader.window();
            assert!(
                !ptr.is_null(),
                "window must return a real pointer on a successful fill"
            );
            assert_eq!(
                frame_count,
                CLUSTER_SIZE / 2,
                "256 frames of stride 2 fill the whole 512-byte cluster"
            );

            // SAFETY: `ptr` is a pinned, resident chunk's payload for as long as `reader`'s
            // `held_lease` (dropped only at the end of this scope) is alive.
            let got: Vec<u8> = (0..CLUSTER_SIZE as usize)
                .map(|i| unsafe { *ptr.add(i) })
                .collect();
            assert_eq!(
                got,
                expected_cluster_bytes(0),
                "window's bytes must match the synthetic fill's converted data \
                 (Native format -> convert/stitch is a no-op over it)"
            );
            assert!(reader.ok(), "a successful window must not clear ok()");
        }

        #[test]
        fn window_past_the_last_audio_frame_is_eof_not_a_failure() {
            let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            set_force_read_failure(false);
            // 10 bytes of real audio (5 frames of stride 2): cluster 0 is short.
            let (_handle, asset) = harness(geo(10));
            // Frame 5 -> abs byte pos 10 -> exactly the end of the real audio data.
            let mut reader = Reader::open(asset, 5, 1, ReadHint::Cached);
            assert!(reader.ok());

            let (ptr, frame_count) = reader.window();
            assert!(ptr.is_null(), "EOF must return a null pointer");
            assert_eq!(frame_count, 0, "EOF must report frame_count == 0");
            assert!(
                reader.ok(),
                "end-of-audio is the ordinary, expected way a scan terminates, not a failure"
            );
        }

        #[test]
        fn window_self_pins_and_advance_releases_across_a_boundary_but_not_within_it() {
            let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            set_force_read_failure(false);
            let (handle, asset) = harness(geo(3 * CLUSTER_SIZE as u64));
            // SAFETY: `handle` is live for the test's duration.
            let resource = unsafe { Resource::from_handle(handle) };

            let mut reader = Reader::open(asset, 0, 1, ReadHint::Cached);
            let (ptr0, frame_count0) = reader.window();
            assert!(!ptr0.is_null());
            assert_eq!(frame_count0, CLUSTER_SIZE / 2);

            let chunk0 = reader
                .held_lease
                .as_ref()
                .expect("window must self-pin the backing it points into")
                .chunk();
            let slot0 = resource.slot_of(chunk0);
            assert_eq!(
                resource.lease_count_by_slot(slot0),
                1,
                "the manager must report exactly one lease held on the backing"
            );

            // Advance within cluster 0 (256 frames wide): the pin must be retained, not re-taken.
            reader.advance(5);
            assert_eq!(reader.current_frame(), 5);
            assert!(
                reader.held_lease.is_some(),
                "advance within the backing must keep the pin"
            );
            assert_eq!(resource.lease_count_by_slot(slot0), 1);

            // Advance to exactly cluster 1's first frame (frame 5 + 251 = frame 256 -> byte 512 ->
            // cluster 1, offset 0): the self-pin invariant means the OLD pin is released as soon as
            // the cursor leaves it, even before the next window() call re-pins.
            reader.advance(251);
            assert_eq!(reader.current_frame(), 256);
            assert!(
                reader.held_lease.is_none(),
                "advance past the backing must release the pin (self-pin invariant)"
            );
            assert_eq!(
                resource.lease_count_by_slot(slot0),
                0,
                "the released pin must drop the manager's lease count for cluster 0"
            );

            // The next window() re-pins on the new cluster and returns the frames there.
            let (ptr1, frame_count1) = reader.window();
            assert!(!ptr1.is_null());
            assert_eq!(
                frame_count1,
                CLUSTER_SIZE / 2,
                "cluster 1 at its own offset 0 is a fresh full cluster"
            );
            let got: Vec<u8> = (0..CLUSTER_SIZE as usize)
                .map(|i| unsafe { *ptr1.add(i) })
                .collect();
            assert_eq!(
                got,
                expected_cluster_bytes(1),
                "cluster 1's window bytes must match the synthetic fill"
            );
        }

        #[test]
        fn forced_read_failure_clears_ok_distinct_from_eof() {
            let _lock = TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
            let (_handle, asset) = harness(geo(3 * CLUSTER_SIZE as u64));
            set_force_read_failure(true);

            let mut reader = Reader::open(asset, 0, 1, ReadHint::Cached);
            assert!(
                reader.ok(),
                "open itself must still succeed (geometry resolves)"
            );

            let (ptr, frame_count) = reader.window();
            assert!(ptr.is_null());
            assert_eq!(frame_count, 0);
            assert!(
                !reader.ok(),
                "a forced read failure must clear ok() -- distinct from EOF, which leaves it set"
            );

            set_force_read_failure(false); // restore for other tests sharing this thread
        }
    }
}
