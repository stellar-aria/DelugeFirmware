//! A safe Rust facade over the residency manager: a typed chunk handle plus the
//! query/schedule operations, composing the SAME private `Manager` methods the
//! `unsafe extern "C"` wrappers in `manager.rs` call (exposed here via `pub(crate)`,
//! bodies unchanged). This is the query/schedule half only — no lease guard yet (a
//! later task adds the RAII `Lease`), so `try_acquire`/`request` here still return a
//! bare (but typed, non-null) `Chunk`; the manager-side lease they take is not yet
//! tied to the handle's lifetime.
//!
//! One `unsafe` fn at the edge (`Resource::from_handle`, mirroring the C ABI's
//! `mgr()`); everything past it is safe, `Option`-returning Rust.

use crate::manager::{mgr, Manager};
use crate::DelugeResource;
use core::ptr::NonNull;

/// A resident chunk's backing pointer — the manager's chunk identity. `Copy`, and
/// deliberately NOT a lease: it does not release anything on drop (see the future
/// `Lease` RAII guard). Carries no lifetime because the manager's tables are
/// process-lifetime (a boot singleton); staleness is caught by the manager's own
/// identity re-validation (e.g. `try_acquire`'s asset/index check), not by the type.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Chunk(NonNull<u8>);

impl Chunk {
    /// Map a manager-returned backing pointer to a typed handle: null (miss / OOM /
    /// not-ready) -> `None`, non-null -> `Some(Chunk)`.
    fn from_ptr(p: *mut u8) -> Option<Self> {
        NonNull::new(p).map(Chunk)
    }

    /// The raw backing pointer, for handing back to a manager method that still
    /// speaks pointers (or to the C++ side during the migration).
    pub fn as_ptr(self) -> *mut u8 {
        self.0.as_ptr()
    }
}

/// The safe entry point over a manager handle (a boot-singleton). Wraps `&'m Manager`
/// — a thin, `Copy`-able view, not an owner.
pub struct Resource<'m> {
    mgr: &'m Manager,
}

impl<'m> Resource<'m> {
    /// Build a `Resource` over the opaque `*mut DelugeResource` handle the app already
    /// holds (returned by `deluge_resource_create`/`_unhooked`).
    ///
    /// # Safety
    /// `h` must be non-null and a live handle previously returned by
    /// `deluge_resource_create`/`deluge_resource_create_unhooked`, valid for at least
    /// `'m`. This mirrors exactly the safety contract the `deluge_resource_*` C-ABI
    /// wrappers already rely on for every call through `h`.
    pub unsafe fn from_handle(h: *mut DelugeResource) -> Self {
        // SAFETY: forwarded to the caller's contract above — same cast `mgr()` performs
        // for the C-ABI wrappers.
        Resource {
            mgr: unsafe { mgr(h) },
        }
    }

    /// RT-safe, non-blocking: is chunk `index` of `asset` resident AND ready? Never
    /// allocates, materializes, or blocks. `None` on a miss (not resident, or resident
    /// but still `Loading` — see `mark_ready`). Takes a hard lease on a hit (mirrors
    /// `deluge_resource_try_acquire`); no RAII guard yet, see the module doc.
    pub fn try_acquire(&self, asset: u32, index: u32) -> Option<Chunk> {
        Chunk::from_ptr(self.mgr.try_acquire(asset, index))
    }

    /// Reserve + construct (no I/O; `ready = false`) chunk `index` of `asset`, so an
    /// external loader can fill it and call `mark_ready`. `None` on OOM, a full table
    /// with nothing evictable, or an asset with no `construct` callback attached.
    pub fn request(&self, asset: u32, index: u32, size: usize) -> Option<Chunk> {
        Chunk::from_ptr(self.mgr.request(asset, index, size))
    }

    /// Publish readiness on a `request`-ed chunk after its data has been filled — the
    /// loader/embassy storage task signals the read completed. No-op if the chunk's
    /// backing is no longer resident.
    pub fn mark_ready(&self, chunk: Chunk) {
        self.mgr.mark_ready(chunk.as_ptr());
    }

    /// The O(1) chunk-table slot index backing `chunk` (for lease-count queries / the
    /// loader queue, which key by slot rather than pointer).
    pub fn slot_of(&self, chunk: Chunk) -> u32 {
        self.mgr.slot_of(chunk.as_ptr())
    }

    /// Enqueue the chunk at `slot` for loading at `priority` (lower = more urgent;
    /// re-enqueue just updates the priority).
    pub fn loader_enqueue(&self, slot: u32, priority: u32) {
        self.mgr.loader_enqueue(slot, priority);
    }

    /// Pop the most-urgent queued + still-leased chunk (clearing its queued flag), or
    /// `None` if the load queue is empty.
    pub fn loader_next(&self) -> Option<Chunk> {
        Chunk::from_ptr(self.mgr.loader_next())
    }
}

#[cfg(test)]
mod tests {
    extern crate std;

    use super::*;
    use crate::manager::BACKING_HEAP;
    use crate::value::COST_IO;
    use core::ffi::c_void;
    use std::vec::Vec;

    unsafe extern "C" fn mock_materialize(
        _ctx: *mut c_void,
        _owner: *mut c_void,
        _index: u32,
        dest: *mut u8,
        len: usize,
    ) -> bool {
        // SAFETY: `dest`/`len` come from the manager's just-allocated backing.
        unsafe { core::ptr::write_bytes(dest, 0xAB, len) };
        true
    }

    unsafe extern "C" fn mock_construct(
        _ctx: *mut c_void,
        _owner: *mut c_void,
        _index: u32,
        dest: *mut u8,
    ) {
        // SAFETY: `dest` comes from the manager's just-allocated backing.
        unsafe { *dest = 0xC0 };
    }

    const CHUNK_SIZE: usize = 4096;

    /// A manager over a throwaway test heap, mirroring the harness in `testing.rs`
    /// and the `lib.rs` unit tests (a 16-aligned `Vec<u128>` arena kept alive
    /// alongside the handle, one requestable test asset defined lazily).
    struct TestResource {
        _buf: Vec<u128>,
        handle: *mut DelugeResource,
    }

    fn test_resource() -> TestResource {
        let words = (256 * 1024usize).div_ceil(16);
        let mut buf = std::vec![0u128; words];
        // SAFETY: `buf` is a live, 16-aligned, `words * 16`-byte arena for the whole
        // test's duration (kept alive in `TestResource::_buf`).
        let h =
            unsafe { deluge_alloc::deluge_heap_create(buf.as_mut_ptr() as *mut u8, words * 16) };
        // SAFETY: `h` is the live heap handle just created above.
        let handle = unsafe { crate::deluge_resource_create(h, 4, 16) };
        assert!(!handle.is_null());
        TestResource { _buf: buf, handle }
    }

    impl TestResource {
        fn resource(&self) -> Resource<'_> {
            // SAFETY: `self.handle` is a live handle for as long as `self` (and its
            // `_buf`) is alive.
            unsafe { Resource::from_handle(self.handle) }
        }

        /// Define + make requestable a single test asset.
        fn define_test_asset(&self) -> u32 {
            // SAFETY: `self.handle` is live; `mock_materialize`/`mock_construct` have
            // the required C-ABI signature.
            let asset = unsafe {
                crate::deluge_resource_define_asset(
                    self.handle,
                    core::ptr::null_mut(),
                    Some(mock_materialize),
                    None,
                    core::ptr::null_mut(),
                    COST_IO,
                    BACKING_HEAP,
                )
            };
            unsafe {
                crate::deluge_resource_set_construct(self.handle, asset, Some(mock_construct));
            }
            asset
        }

        fn try_acquire(&self, asset: u32, index: u32) -> Option<Chunk> {
            self.resource().try_acquire(asset, index)
        }
        fn request(&self, asset: u32, index: u32, size: usize) -> Option<Chunk> {
            self.resource().request(asset, index, size)
        }
        fn mark_ready(&self, chunk: Chunk) {
            self.resource().mark_ready(chunk)
        }
    }

    #[test]
    fn try_acquire_reports_resident_ready_and_none_on_miss() {
        let rsrc = test_resource(); // build a manager over a test heap (mirror testing.rs)
        let asset = rsrc.define_test_asset();
        assert!(rsrc.try_acquire(asset, 0).is_none()); // not resident yet
        let c = rsrc.request(asset, 0, CHUNK_SIZE).expect("request");
        rsrc.mark_ready(c);
        let got = rsrc.try_acquire(asset, 0).expect("now ready");
        assert_eq!(got, c); // same chunk identity
    }
}
