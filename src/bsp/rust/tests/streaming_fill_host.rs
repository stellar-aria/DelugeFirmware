//! Host unit test for `streaming_loader::fill_once` (R1.1) — the async
//! cluster-fill drain loop, exercised against an in-memory [`FakeOps`] double
//! rather than the real C++ resource manager/`StreamedChunk`. See
//! `src/streaming_loader.rs`'s module doc ("Why the drain loop is generic over
//! `FillOps`") for why: constructing real C++ objects (or mocking the
//! `#[no_mangle]` C symbols, which would collide with the real ones under
//! `host_app`) is impractical/unsafe for a host test, so the orchestration is
//! injected instead.
//!
//! `deluge-bsp-rust` is bin-only (no `[lib]` target — see `Cargo.toml`), so this
//! recompiles `src/streaming_loader.rs` unmodified into this test binary via
//! `#[path]`, same convention as `tests/owner_host.rs`. Only the
//! `async_streaming_loader`-gated `fill_once`/`FillOps` orchestration is needed
//! here — the `host_app`-gated `prod` submodule (the real `extern "C"` wiring)
//! stays uncompiled, so this test never needs the real C++ app linked.
#![cfg(feature = "async_streaming_loader")]
#![cfg(not(target_os = "none"))]

use core::ffi::c_void;
use std::cell::RefCell;

#[path = "../src/streaming_loader.rs"]
mod streaming_loader;

use streaming_loader::{FillOps, StreamingFillDescriptor, fill_once};

/// One call `FakeOps` observed, in order — the orchestration assertion surface.
#[derive(Debug, PartialEq, Eq, Clone)]
enum Call {
    Next,
    Begin(usize),
    Read { lba: u32, count: u32 },
    Finish { chunk: usize, read_ok: bool },
    EnqueueLowest(usize),
}

/// A fake queued cluster. Owns its own backing buffer so `begin`'s
/// `StreamingFillDescriptor::dest` points at real, correctly-sized memory —
/// `fill_once` really does write `num_sectors * 512` bytes into it via the
/// unsafe slice it builds from the descriptor.
struct FakeChunk {
    id: usize,
    sector: u32,
    num_sectors: u32,
    begin_ok: bool,
    buf: RefCell<Vec<u8>>,
}

/// The `FillOps` test double: an in-memory priority queue (lower number = more
/// urgent, matching `deluge_resource_loader_next`'s real ordering) plus a
/// recording of every call made, so tests can assert on the orchestration
/// itself rather than on any real I/O or C++ side effect.
struct FakeOps {
    chunks: Vec<Box<FakeChunk>>,
    /// (chunk id, priority) pairs still queued.
    pending: RefCell<Vec<(usize, u32)>>,
    calls: RefCell<Vec<Call>>,
    /// What the next `read()` call returns.
    read_result: RefCell<bool>,
}

impl FakeOps {
    fn new(chunks: Vec<FakeChunk>, read_result: bool) -> Self {
        let chunks: Vec<Box<FakeChunk>> = chunks.into_iter().map(Box::new).collect();
        Self {
            chunks,
            pending: RefCell::new(Vec::new()),
            calls: RefCell::new(Vec::new()),
            read_result: RefCell::new(read_result),
        }
    }

    fn enqueue(&self, id: usize, priority: u32) {
        self.pending.borrow_mut().push((id, priority));
    }

    fn chunk_by_id(&self, id: usize) -> &FakeChunk {
        self.chunks
            .iter()
            .find(|c| c.id == id)
            .expect("unknown fake chunk id")
    }

    /// # Safety
    /// `ptr` must be a `*mut c_void` this `FakeOps` itself handed out via
    /// `next()` (i.e. one of `self.chunks`' addresses).
    unsafe fn chunk_from_ptr(ptr: *mut c_void) -> &'static FakeChunk {
        unsafe { &*(ptr as *const FakeChunk) }
    }
}

impl FillOps for FakeOps {
    fn next(&self) -> *mut c_void {
        self.calls.borrow_mut().push(Call::Next);
        let mut pending = self.pending.borrow_mut();
        let Some((idx, _)) = pending
            .iter()
            .enumerate()
            .min_by_key(|(_, (_, priority))| *priority)
        else {
            return core::ptr::null_mut();
        };
        let (id, _) = pending.remove(idx);
        (self.chunk_by_id(id) as *const FakeChunk) as *mut c_void
    }

    fn begin(&self, chunk: *mut c_void) -> StreamingFillDescriptor {
        // SAFETY: only ever called with a pointer this test double's `next()`
        // just returned.
        let fc = unsafe { Self::chunk_from_ptr(chunk) };
        self.calls.borrow_mut().push(Call::Begin(fc.id));
        if !fc.begin_ok {
            return StreamingFillDescriptor {
                dest: core::ptr::null_mut(),
                sector: 0,
                num_sectors: 0,
                ok: false,
            };
        }
        StreamingFillDescriptor {
            dest: fc.buf.borrow_mut().as_mut_ptr(),
            sector: fc.sector,
            num_sectors: fc.num_sectors,
            ok: true,
        }
    }

    async fn read(&self, lba: u32, count: u32, buf: &mut [u8]) -> bool {
        self.calls.borrow_mut().push(Call::Read { lba, count });
        // Prove the descriptor's byte range really is the one `fill_once` reads
        // into: stamp a recognizable pattern rather than leaving it untouched.
        buf.fill(0xAB);
        *self.read_result.borrow()
    }

    fn finish(&self, chunk: *mut c_void, read_ok: bool) -> bool {
        // SAFETY: same pointer `begin` was just called with.
        let fc = unsafe { Self::chunk_from_ptr(chunk) };
        self.calls.borrow_mut().push(Call::Finish {
            chunk: fc.id,
            read_ok,
        });
        // Mirrors `deluge_streaming_finish_fill`: returns false exactly when
        // `read_ok` is false (async_fill.cpp:70-72).
        read_ok
    }

    fn enqueue_lowest(&self, chunk: *mut c_void) {
        // SAFETY: same pointer `begin`/`finish` were just called with.
        let fc = unsafe { Self::chunk_from_ptr(chunk) };
        self.calls.borrow_mut().push(Call::EnqueueLowest(fc.id));
        self.pending.borrow_mut().push((fc.id, u32::MAX));
    }
}

fn one_chunk(id: usize, begin_ok: bool) -> FakeChunk {
    let num_sectors = 2u32;
    FakeChunk {
        id,
        sector: 100 + id as u32,
        num_sectors,
        begin_ok,
        buf: RefCell::new(vec![0u8; (num_sectors as usize) * 512]),
    }
}

/// Happy path: one queued cluster → `begin` resolves it, `read` is awaited,
/// `finish(chunk, true)` runs, then the loop pops the now-empty queue and
/// returns.
#[test]
fn fill_once_happy_path_drains_one_cluster() {
    let ops = FakeOps::new(vec![one_chunk(0, true)], /* read_result = */ true);
    ops.enqueue(0, 10);

    embassy_futures::block_on(fill_once(&ops));

    assert_eq!(
        *ops.calls.borrow(),
        vec![
            Call::Next,
            Call::Begin(0),
            Call::Read { lba: 100, count: 2 },
            Call::Finish {
                chunk: 0,
                read_ok: true
            },
            Call::Next,
        ]
    );
    // The read really did land in the chunk's own buffer.
    assert!(ops.chunk_by_id(0).buf.borrow().iter().all(|&b| b == 0xAB));
    assert!(ops.pending.borrow().is_empty());
}

/// Read failure: `read` returns false → `finish(chunk, false)` returns false →
/// the cluster is re-enqueued at `LOWEST_PRIORITY` (0xFFFF_FFFF) and the loop
/// stops immediately (mirrors `pump()`'s behaviour at `loader.cpp:123-126`) —
/// no second `next()` call in the same `fill_once`.
#[test]
fn fill_once_read_failure_reenqueues_lowest_and_stops() {
    let ops = FakeOps::new(vec![one_chunk(0, true)], /* read_result = */ false);
    ops.enqueue(0, 10);

    embassy_futures::block_on(fill_once(&ops));

    assert_eq!(
        *ops.calls.borrow(),
        vec![
            Call::Next,
            Call::Begin(0),
            Call::Read { lba: 100, count: 2 },
            Call::Finish {
                chunk: 0,
                read_ok: false
            },
            Call::EnqueueLowest(0),
        ]
    );
    assert_eq!(*ops.pending.borrow(), vec![(0, u32::MAX)]);
}

/// `begin` reports `ok = false` (unloadable / geometry error) → that chunk is
/// skipped outright (no `read`, no `finish`, no `enqueue_lowest`), and the loop
/// keeps draining the rest of the queue.
#[test]
fn fill_once_skips_chunk_when_begin_not_ok() {
    let ops = FakeOps::new(
        vec![one_chunk(0, false), one_chunk(1, true)],
        /* read_result = */ true,
    );
    ops.enqueue(0, 10); // most urgent — popped (and skipped) first
    ops.enqueue(1, 20);

    embassy_futures::block_on(fill_once(&ops));

    assert_eq!(
        *ops.calls.borrow(),
        vec![
            Call::Next,
            Call::Begin(0),
            Call::Next,
            Call::Begin(1),
            Call::Read { lba: 101, count: 2 },
            Call::Finish {
                chunk: 1,
                read_ok: true
            },
            Call::Next,
        ]
    );
}
