//! Per-chunk convert-state sidecar for the native fill (SR2d-4 Task 3).
//!
//! `finish`'s post-read convert/stitch tail (`deluge_streaming_finish_fill`/`stitch_boundaries` today,
//! its native-Rust successor later) reads and writes a small piece of state per chunk: the
//! pre-conversion `first_three_bytes[3]` a NEIGHBOUR chunk's boundary stitch reads, plus two
//! `{start,end}_converted` idempotency guards so a boundary is never converted twice. Today those
//! fields live directly on the C++ `StreamedChunk`; this module moves them into a Rust-owned store
//! instead, keyed by the manager's own chunk-table slot.
//!
//! This module ONLY builds and tests the sidecar (see `tests/fill_sidecar_host.rs`) — verified
//! fill-only (nothing but the fill path reads/writes this state, see the task brief), it is not yet
//! wired into `ProdOps`/`fill_once` (`streaming_loader.rs`); that's a later task.
//!
//! ## Keying + auto-invalidation via generation
//!
//! Keyed by the manager's chunk-table SLOT (`deluge_resource_slot_of`), not `(asset, index)`, so
//! [`get`]/[`set`] are O(1) index operations rather than an O(n) scan. A slot is only meaningful
//! while it keeps backing the SAME chunk — the manager can evict an unleased chunk and hand the same
//! slot to a completely different one — so every entry is stamped with the slot's GENERATION at write
//! time (`deluge_resource_generation_of_slot`, added alongside `slot_of` by SR2d-3/Task 2). [`get`]
//! compares the entry's stored generation against the slot's CURRENT generation: a match returns the
//! stored state; a mismatch means the slot was evicted and reused since — the stale entry is treated
//! as never having existed and [`get`] returns a fresh zeroed default instead (lazily overwriting the
//! stale entry with that default at the current generation, so the slot doesn't keep re-deriving
//! "stale" on every subsequent read until something calls [`set`]).
//!
//! This is the whole invalidation story — there is deliberately NO explicit reset-on-eviction path.
//! The manager's eviction/reuse machinery never touches this sidecar at all; the generation mismatch
//! alone is what makes a reused slot's leftover entry unobservable.
//!
//! ## Synchronization: single-owner, no masking
//!
//! [`get`]/[`set`] are touched ONLY by the fill task's `finish` step — writing its own chunk's state
//! and reading its neighbours' — and that step only ever runs on `streaming_loader::streaming_fill_task`,
//! a single Embassy async task on the one thread-mode executor the whole
//! app (and the C++ loader-enqueue path) runs on. See that module's doc, "Concurrency" section: the
//! manager itself is `!Send`/`!Sync` by design for exactly this reason. Crucially, this is NOT the
//! audio render path either — render runs on a separate, preemptive `InterruptExecutor` and never
//! touches per-chunk convert-state (it only ever sees a chunk once `mark_ready`/`set_loaded` have
//! published it). So this sidecar has exactly one toucher, ever, and needs no critical section /
//! masking (not even the resource manager's own ISR-aware `Masked` — that would be the wrong
//! dependency here, same reasoning `streaming_loader::FILL_CONTEXTS` already gives for skipping it).
//!
//! A plain `RefCell` gives the dynamic-borrow-checked interior mutability a single owner wants; the
//! only reason it needs the [`SingleOwner`] wrapper below at all is that a Rust `static` must be
//! `Sync`, and `RefCell` deliberately isn't (its interior mutability isn't thread-safe by construction)
//! — `SingleOwner` is a zero-cost, unsafe `Sync` assertion backed by the single-task argument above,
//! not a real cross-thread synchronization primitive. If a second toucher (any other task, or the
//! audio ISR) ever needs this state, STOP and reconsider before reusing this type.
use core::cell::RefCell;
use core::ffi::c_void;

unsafe extern "C" {
    fn deluge_resource_slot_of(mgr: *mut c_void, ptr: *mut c_void) -> u32;
    fn deluge_resource_generation_of_slot(mgr: *mut c_void, slot: u32) -> u32;
}

/// Fixed capacity for the sidecar table, indexed 1:1 by the manager's chunk-table slot. The manager's
/// own `chunk_cap` (`general_memory_allocator.cpp`'s `slabCapacity + kAssetCap`) is sized at runtime
/// from the SDRAM size and the session's `Cluster::size` (a smaller cluster size — a card formatted
/// with small FAT clusters — yields MORE slab slots, not fewer), so unlike `streaming_loader::FILL_CONTEXT_CAP`
/// (mirroring the fixed `kAssetCap`) there is no single compile-time number
/// that is exactly right. This is deliberately generous instead: even a pathologically small
/// (`4 KiB`) session cluster size against the 64 MiB SDRAM region tops out in the high tens of
/// thousands of slab slots, plus `kAssetCap` (4096) adopted-object slots; `32768` covers every
/// realistic geometry with headroom. A slot at or beyond this cap degrades SAFELY, not incorrectly:
/// [`resolve_slot`] bounds-checks against it and reports "unresolvable" for an out-of-range slot,
/// which makes [`get`] behave as if the chunk had never been seen (a fresh default every time) and
/// makes [`set`] a silent no-op — never an out-of-bounds access. If on-device measurement ever shows
/// the real `chunk_cap` exceeding this, raise it here (same hand-synced-constant caveat
/// `streaming_loader::FILL_CONTEXT_CAP`'s own doc gives for its own asset-table cap).
const SIDECAR_CAP: usize = 32768;

/// The per-chunk convert-state `finish`'s convert/stitch tail reads/writes for a chunk and its
/// neighbours. `first_three_bytes` is the PRE-conversion first 3 bytes of the chunk's raw data (read
/// by a neighbour's boundary stitch, which needs the byte pattern spanning the cluster boundary
/// before this chunk's own in-place conversion overwrote it); `start_converted`/`end_converted` are
/// idempotency guards so a boundary is never re-stitched once it's already been handled from the
/// other side.
#[derive(Clone, Copy, Default, PartialEq, Eq)]
#[cfg_attr(test, derive(Debug))]
pub struct ConvertState {
    pub first_three_bytes: [u8; 3],
    pub start_converted: bool,
    pub end_converted: bool,
}

/// One sidecar slot: the generation it was last written at, plus the state itself. `generation == 0`
/// (never a real slot's generation — the manager's generation counter starts at 1, see
/// `deluge_resource::manager::Manager::next_gen`) is the table's "never written" sentinel, so the
/// all-zero initializer below is already a valid empty entry — no explicit reset pass needed at
/// startup, same reasoning `streaming_loader::FillContext::UNREGISTERED` gives for its own sentinel.
#[derive(Clone, Copy)]
struct Entry {
    generation: u32,
    state: ConvertState,
}

impl Entry {
    const EMPTY: Entry = Entry {
        generation: 0,
        state: ConvertState {
            first_three_bytes: [0; 3],
            start_converted: false,
            end_converted: false,
        },
    };
}

/// A zero-cost, unsafe `Sync` assertion for a type that is only ever touched from one execution
/// context — see the module doc's "Synchronization" section for the argument. Not a real
/// synchronization primitive: it adds no masking, no locking, nothing at runtime. Exists only because
/// a Rust `static` must be `Sync`, and the whole point here is NOT to pay for a primitive this single-
/// owner table doesn't need.
struct SingleOwner<T>(T);

// SAFETY: `SIDECAR` (the only instance of this type) is touched exclusively by `get`/`set` below,
// which are in turn only ever called from `streaming_fill_task`'s `finish` step — a single Embassy
// async task on the one thread-mode executor the whole app runs on, never the audio render ISR. See
// the module doc's "Synchronization" section.
unsafe impl<T> Sync for SingleOwner<T> {}

/// The sidecar table itself. `.sdram_bss` on device: at `SIDECAR_CAP` entries (`size_of::<Entry>()`
/// is 12 bytes — a 4-byte generation plus the 5-byte `ConvertState` padded to 4-byte alignment) this
/// is ~384 KiB, the same "too big for on-chip SRAM at debug opt-levels, trivial in the 64 MiB SDRAM
/// region" situation `streaming_loader::FILL_CONTEXTS` already documents and solves the same way.
/// Zeroed by `boot_mem::init_sdram_memory()` before any app code runs, which is exactly
/// `Entry::EMPTY` repeated — the explicit initializer below just keeps host-build behaviour (plain
/// `.bss`) identical in substance.
#[cfg_attr(target_os = "none", unsafe(link_section = ".sdram_bss"))]
static SIDECAR: SingleOwner<RefCell<[Entry; SIDECAR_CAP]>> =
    SingleOwner(RefCell::new([Entry::EMPTY; SIDECAR_CAP]));

/// Resolve `chunk`'s current `(slot, generation)` under `mgr`, or `None` if `chunk` isn't resident
/// (`deluge_resource_slot_of` returns `DELUGE_RESOURCE_NO_SLOT`) or its slot is beyond
/// [`SIDECAR_CAP`] (see that constant's doc — a safe degrade, not a bug).
fn resolve_slot(mgr: *mut c_void, chunk: *mut c_void) -> Option<(usize, u32)> {
    // SAFETY: `mgr` is the live singleton resource manager and `chunk` is a `StreamedChunk*` backing
    // pointer the fill path is already treating as valid for the duration of this call (mirrors
    // `streaming_loader::ProdOps::lease_count`'s identical `slot_of` call).
    let slot = unsafe { deluge_resource_slot_of(mgr, chunk) };
    if slot as usize >= SIDECAR_CAP {
        // Covers both `DELUGE_RESOURCE_NO_SLOT` (`u32::MAX`, always out of range for any sane cap)
        // and a real slot beyond this table's bound.
        return None;
    }
    // SAFETY: same `mgr`, `slot` just resolved from it above.
    let generation = unsafe { deluge_resource_generation_of_slot(mgr, slot) };
    Some((slot as usize, generation))
}

/// The convert-state currently recorded for `chunk` under `mgr`, or a fresh zeroed [`ConvertState`]
/// if `chunk` isn't resident, its slot is out of range (see [`SIDECAR_CAP`]), nothing has ever
/// [`set`] it, or the slot has since been evicted and reused for a different chunk (a generation
/// mismatch — see the module doc's "Keying + auto-invalidation" section). The mismatch case lazily
/// overwrites the stale entry with the fresh default at the slot's current generation.
///
/// Not called anywhere yet outside tests — wiring this into the native fill's `finish` tail is a
/// later task (see the module doc).
#[allow(dead_code)]
pub fn get(mgr: *mut c_void, chunk: *mut c_void) -> ConvertState {
    let Some((slot, generation)) = resolve_slot(mgr, chunk) else {
        return ConvertState::default();
    };
    let mut table = SIDECAR.0.borrow_mut();
    let entry = &mut table[slot];
    if entry.generation == generation {
        return entry.state;
    }
    // Stale (or never-written) entry: lazily stamp the fresh default at the current generation so
    // this slot doesn't keep re-deriving "stale" on every read until something calls `set`.
    *entry = Entry {
        generation,
        state: ConvertState::default(),
    };
    entry.state
}

/// Record `state` for `chunk` under `mgr`, stamped at the slot's CURRENT generation. A silent no-op
/// if `chunk` isn't resident or its slot is out of range (see [`SIDECAR_CAP`]).
///
/// Not called anywhere yet outside tests — see [`get`]'s doc.
#[allow(dead_code)]
pub fn set(mgr: *mut c_void, chunk: *mut c_void, state: ConvertState) {
    let Some((slot, generation)) = resolve_slot(mgr, chunk) else {
        return;
    };
    SIDECAR.0.borrow_mut()[slot] = Entry { generation, state };
}
