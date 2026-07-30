//! The streamed sample-audio cluster's per-cluster state, relocated from the C++ `StreamedChunk`
//! POD (U4d). A plain Rust struct (NOT repr(C) — nothing C++ reads it anymore; only its size feeds
//! the slab slot geometry). Drop-free: all fields are POD, so the manager still frees the slab slot
//! directly (no destructor to run), preserving the trivially-destructible chunk contract.
//!
//! Field set (U4d Task 1 audit): the C++ `StreamedChunk` also carries `sample`/`resource_slot`,
//! but both are write-only in the live tree — their only C++ readers
//! (`StreamedChunk::convert_data_if_necessary`/`resource_lease_asset_id`/
//! `deluge::cluster::remove_reason(StreamedChunk&, ...)`, all in `cluster.cpp`) have zero call
//! sites anywhere (dead code, superseded by the async Rust fill + `convert.h`'s pure core), so
//! neither is carried here. `cluster_index` IS live: `deluge_sample_fill::fill_logic::begin`/
//! `finish` document and use it directly in offset math.

/// Cache line, matching the C++ `CACHE_LINE_SIZE` (definitions.h). The front guard between the
/// struct header and the payload must be >= this; guarded C++-side by the slab reconcile.
const CACHE_LINE: usize = 32;

pub struct StreamedChunk {
    pub cluster_index: u32,
    pub loaded: bool,
    pub unloadable: bool,
    pub first_three_bytes: [u8; 3],
    pub extra_bytes_start_converted: bool,
    pub extra_bytes_end_converted: bool,
    pub payload: *mut u8,
}

impl StreamedChunk {
    const fn new(index: u32, payload: *mut u8) -> Self {
        StreamedChunk {
            cluster_index: index,
            loaded: false,
            unloadable: false,
            first_three_bytes: [0; 3],
            extra_bytes_start_converted: false,
            extra_bytes_end_converted: false,
            payload,
        }
    }
}

/// Byte offset of the payload from the slot base for a streamed chunk: header + one cache-line
/// front guard, matching the C++ `kChunkPayloadOffset` discipline (its own offset; the shared slab
/// slot takes the max with ComputedChunk's — see general_memory_allocator.cpp).
pub const RUST_CHUNK_PAYLOAD_OFFSET: usize = core::mem::size_of::<StreamedChunk>() + CACHE_LINE;

/// Placement-construct a StreamedChunk at `dest`, setting its payload base + cluster index.
/// # Safety
/// `dest` must be a writable slab slot of at least `RUST_CHUNK_PAYLOAD_OFFSET + Cluster::size +
/// trailing_guard` bytes.
pub(crate) unsafe fn construct(dest: *mut u8, index: u32) {
    // SAFETY: `dest` is a writable slab slot (caller contract); payload sits within it.
    unsafe {
        core::ptr::write(
            dest as *mut StreamedChunk,
            StreamedChunk::new(index, dest.add(RUST_CHUNK_PAYLOAD_OFFSET)),
        );
    }
}

/// C-ABI: the byte offset a streamed chunk needs for its payload, so the C++ slab setup can size
/// the shared slot as `max(this, ComputedChunk's kChunkPayloadOffset) + Cluster::size + guard`.
///
/// Unconditional `#[unsafe(no_mangle)]`, matching this crate's sibling export
/// (`deluge_streaming_set_fill_context` in `lib.rs`): unlike `deluge_sample_stream`/
/// `deluge_sample_source`, this crate declares no `host_app`/`sim` selector features (see
/// `Cargo.toml`'s `[features]`) — it is pulled in unconditionally wherever it's linked at all
/// (`native_fill` gates the manager-reaching fill, not symbol visibility), so there is no cfg to
/// gate this export behind.
#[unsafe(no_mangle)]
pub extern "C" fn deluge_streamed_chunk_payload_offset() -> u32 {
    RUST_CHUNK_PAYLOAD_OFFSET as u32
}

#[cfg(test)]
mod tests {
    // A plain `cargo test` on this `#![no_std]` crate links no allocator; `std` is available on
    // the host test binary, so pull it in here for `vec!` — mirrors `deluge_sample_stream`'s own
    // `#[cfg(test)] mod tests { extern crate std; ... }` convention (registry.rs).
    extern crate std;

    use std::vec;

    use super::*;

    #[test]
    fn payload_offset_clears_the_front_guard() {
        // The payload must sit at least one cache line past the struct header, so the front
        // guard (offset - struct size) covers the DMA cache-maintenance range-rounding + the
        // application front-underread — exactly the C++ kChunkPayloadOffset discipline.
        assert!(RUST_CHUNK_PAYLOAD_OFFSET >= core::mem::size_of::<StreamedChunk>() + CACHE_LINE);
        assert_eq!(
            RUST_CHUNK_PAYLOAD_OFFSET - core::mem::size_of::<StreamedChunk>(),
            CACHE_LINE
        );
    }

    #[test]
    fn construct_sets_payload_index_and_defaults() {
        // A 4KB-aligned-enough backing buffer big enough for the header+guard.
        let mut buf = vec![0u8; RUST_CHUNK_PAYLOAD_OFFSET + 64];
        let dest = buf.as_mut_ptr();
        // SAFETY: `dest` owns RUST_CHUNK_PAYLOAD_OFFSET+64 writable bytes for this test.
        unsafe { construct(dest, 7) };
        // SAFETY: construct placement-wrote a StreamedChunk at dest.
        let c = unsafe { &*(dest as *const StreamedChunk) };
        assert_eq!(c.cluster_index, 7);
        assert!(!c.loaded);
        assert!(!c.unloadable);
        assert_eq!(c.payload, unsafe { dest.add(RUST_CHUNK_PAYLOAD_OFFSET) });
    }
}
