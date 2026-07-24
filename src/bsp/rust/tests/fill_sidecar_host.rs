//! Host round-trip test for the per-chunk convert-state sidecar (SR2d-4 Task 3):
//! `fill_sidecar::get`/`set`, keyed by the manager's chunk-table slot + generation.
//!
//! Unlike `tests/streaming_fill_host.rs`'s `FakeOps` double, this builds a REAL `deluge_resource`
//! manager over a test heap (via its own C-ABI) and drives real `request`/`release` calls — the
//! sidecar's whole point is to auto-invalidate against the manager's actual
//! generation-bump-on-slot-reuse behaviour, which a fake can't stand in for. See
//! `src/fill_sidecar.rs`'s module doc for the design, and
//! `crates/deluge_resource/src/manager.rs`'s own `generation_bumps_on_slot_reuse_and_gates_stale_retain`
//! test for the same single-chunk-slot-table trick used below to force a deterministic generation
//! bump (with a bigger table, eviction order is a value-function detail; a 1-slot table makes reuse
//! of the SAME slot unavoidable).
//!
//! `deluge-bsp-rust` is bin-only (no `[lib]` target — see `Cargo.toml`), so this recompiles
//! `src/fill_sidecar.rs` unmodified into this test binary via `#[path]`, same convention as
//! `tests/streaming_fill_host.rs`/`tests/streaming_fill_context_host.rs`. This file has no internal
//! feature gates of its own, but only links on host because `Cargo.toml`'s host-only
//! `[dev-dependencies]` — NOT the optional `host_app`-gated dependency the production build uses —
//! give this test binary real `deluge_resource_slot_of`/`_generation_of_slot`/etc. symbols to
//! resolve against, without needing the full host-built C++ app closure.
#![cfg(not(target_os = "none"))]

use core::ffi::c_void;
use core::ptr;

use deluge_resource::{
    BACKING_HEAP, DelugeResource, deluge_resource_create, deluge_resource_define_asset,
    deluge_resource_release, deluge_resource_request, deluge_resource_set_construct,
    deluge_resource_slot_of,
};

#[path = "../src/fill_sidecar.rs"]
mod fill_sidecar;

use fill_sidecar::{ConvertState, get, set};

// `deluge_resource`'s `Masked` critical section (`sync.rs`) calls these three C-ABI symbols; in the
// real device/host_app link they're `services.rs`'s real interrupt-mask primitives, and inside
// `deluge_resource`'s OWN `cargo test` binary they're that crate's private `#[cfg(test)]` stubs —
// neither applies here (this test binary links `deluge_resource` as a plain dependency, not `cfg(test)`,
// and never links `services.rs`). This test is single-threaded and never runs concurrently with an
// "audio ISR", so inert no-op stand-ins are semantically correct (nothing here can race with itself):
// no torn reads/writes are possible without a second execution context to race against.
#[unsafe(no_mangle)]
extern "C" fn ENTER_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn EXIT_CRITICAL_SECTION() {}
#[unsafe(no_mangle)]
extern "C" fn deluge_in_interrupt() -> bool {
    false
}

/// Mirrors `manager.rs`'s own `mock_construct` test helper: a requestable asset needs SOME
/// `construct` callback to be requestable at all (see `deluge_resource_set_construct`'s doc); its
/// actual byte pattern is irrelevant here, only chunk identity/residency matters.
unsafe extern "C" fn mock_construct(
    _ctx: *mut c_void,
    _owner: *mut c_void,
    _index: u32,
    dest: *mut u8,
) {
    // SAFETY: `dest` comes from the manager's just-allocated backing.
    unsafe { *dest = 0xC0 };
}

/// A manager over a throwaway 16-aligned test heap, plus one requestable test asset. `chunk_cap`
/// is caller-chosen: the generation-bump test below uses a SINGLE chunk slot so a second,
/// distinct-index `request` is forced to evict + reuse slot 0 — the only way to deterministically
/// exercise a generation bump without depending on the value function's eviction order over a
/// bigger table (same reasoning as `manager.rs`'s own generation test).
struct TestManager {
    handle: *mut DelugeResource,
    asset: u32,
    // Kept alive for `self`'s whole lifetime; the manager's tables live inside it.
    _buf: Vec<u128>,
}

impl TestManager {
    fn new(chunk_cap: usize) -> Self {
        // 16-aligned arena (`Vec<u128>`), mirroring `manager.rs`'s own `test_manager` helper.
        let words = (256 * 1024usize).div_ceil(16);
        let mut buf = vec![0u128; words];
        // SAFETY: `buf` is a live, 16-aligned, `words * 16`-byte arena for as long as `self` (and
        // thus `buf`) is alive — `buf` is moved into the returned `TestManager`, never freed
        // while `handle` is in use.
        // `fs_alloc` is `crates/deluge_alloc` under its `Cargo.toml` rename (shared with the
        // device-only dependency of the same underlying package — see that entry's comment).
        let heap = unsafe { fs_alloc::deluge_heap_create(buf.as_mut_ptr() as *mut u8, words * 16) };
        // SAFETY: `heap` is the live handle just created above.
        let handle = unsafe { deluge_resource_create(heap, 4, chunk_cap) };
        assert!(!handle.is_null());
        // SAFETY: `handle` is live for the duration of this call.
        let asset = unsafe {
            deluge_resource_define_asset(
                handle,
                ptr::null_mut(),
                None,
                None,
                ptr::null_mut(),
                1,
                BACKING_HEAP,
            )
        };
        // SAFETY: `handle`/`asset` are live and valid.
        unsafe { deluge_resource_set_construct(handle, asset, Some(mock_construct)) };
        TestManager {
            handle,
            asset,
            _buf: buf,
        }
    }

    fn mgr(&self) -> *mut c_void {
        self.handle as *mut c_void
    }

    fn request(&self, index: u32) -> *mut c_void {
        // SAFETY: `self.handle`/`self.asset` are live and valid for the duration of this call.
        (unsafe { deluge_resource_request(self.handle, self.asset, index, 64) }) as *mut c_void
    }

    fn release(&self, chunk: *mut c_void) {
        // SAFETY: `chunk` was returned by `request` above and is still resident.
        unsafe { deluge_resource_release(self.handle, chunk as *mut u8) };
    }

    fn slot_of(&self, chunk: *mut c_void) -> u32 {
        // SAFETY: `self.handle` is live; `chunk` may or may not still be resident (that's exactly
        // what `slot_of` reports).
        unsafe { deluge_resource_slot_of(self.handle, chunk as *mut u8) }
    }
}

fn some_state(tag: u8) -> ConvertState {
    ConvertState {
        first_three_bytes: [tag, tag.wrapping_add(1), tag.wrapping_add(2)],
        start_converted: true,
        end_converted: tag.is_multiple_of(2),
    }
}

/// An unseen chunk — nothing has ever `set` it — reports the zeroed default.
#[test]
fn unseen_chunk_is_default() {
    let m = TestManager::new(16);
    let chunk = m.request(0);
    assert!(!chunk.is_null());

    assert_eq!(get(m.mgr(), chunk), ConvertState::default());
}

/// A written entry persists across a `get` (and a second `get` doesn't disturb it).
#[test]
fn written_entry_persists() {
    let m = TestManager::new(16);
    let chunk = m.request(0);
    assert!(!chunk.is_null());

    let state = some_state(7);
    set(m.mgr(), chunk, state);
    assert_eq!(get(m.mgr(), chunk), state);
    assert_eq!(get(m.mgr(), chunk), state);
}

/// After the slot is evicted and reused for a DIFFERENT chunk, `get` returns a FRESH default — the
/// stale entry is gone. Forces the generation bump with a single-chunk-slot table: releasing chunk0
/// (dropping it to 0 leases, evictable) and then requesting a DIFFERENT index MUST evict slot 0 and
/// reuse the very same slot for the new chunk (asserted below), which is exactly the scenario
/// `fill_sidecar`'s generation check exists for. Proves the invalidation path actually fires, not an
/// incidental pass: the reused chunk lands at the same slot yet still reads back default, not
/// chunk0's leftover state.
#[test]
fn slot_reuse_bumps_generation_and_invalidates_stale_entry() {
    let m = TestManager::new(1); // single chunk slot: forces eviction + reuse below

    let chunk0 = m.request(0);
    assert!(!chunk0.is_null());
    let slot0 = m.slot_of(chunk0);

    let state = some_state(0xAB);
    set(m.mgr(), chunk0, state);
    assert_eq!(get(m.mgr(), chunk0), state);

    m.release(chunk0); // leases -> 0, evictable
    let chunk1 = m.request(1); // distinct index: must evict + reuse slot0
    assert!(!chunk1.is_null());
    let slot1 = m.slot_of(chunk1);
    assert_eq!(
        slot1, slot0,
        "single-slot table: reuse must be the same slot"
    );

    // The new occupant reads back a FRESH default, not chunk0's stale state — the generation
    // mismatch invalidated it.
    assert_eq!(get(m.mgr(), chunk1), ConvertState::default());
}
