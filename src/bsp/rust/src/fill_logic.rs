//! Pure byte-range arithmetic for the native cluster fill (SR2d-4 Task 4): [`begin`]
//! reimplements `deluge::audio::stream::begin_fill`'s "resolve where/how much" step
//! (`storage/audio/stream/async_fill.cpp:76-98`) — the last-cluster short-read
//! sector-count calc and the cluster's byte offset within the file — from geometry
//! alone. No `StreamedChunk`, no FFI, no statics: this is arithmetic over plain
//! values, so it's trivially host-testable and needs no `unsafe`.
//!
//! A later task (`streaming_loader::prod::ProdOps::begin`) wires this in to replace
//! the `begin_fill` C++ upcall; this module only delivers the pure half.
//!
//! ## Mirroring `begin_fill` bit-for-bit, including its 32-bit truncation
//!
//! `begin_fill`'s C++ source (`sample->audioDataLengthBytes` is `uint64_t`,
//! `sample->audioDataStartPosBytes` is `uint32_t` — see `model/sample/sample.h`)
//! computes `audioDataEndPosBytes` as a 64-bit sum but stores it in a **`uint32_t`**
//! local, silently truncating mod 2^32; `startByteThisCluster` and `bytesToRead`
//! (`int32_t`) are 32-bit throughout too. [`begin`] reproduces this literally —
//! `audio_data_end_pos_bytes` is computed via a 64-bit add then narrowed to `u32`
//! with `as`, exactly like the implicit C++ narrowing conversion — rather than
//! doing the arithmetic in 64 bits throughout. This only diverges from a "clean"
//! 64-bit implementation for sample files whose end position exceeds 4 GiB (not a
//! real case FAT32 or this firmware supports), so it's a safe, deliberate match to
//! the literal C++, not a bug being reproduced for its own sake — see the
//! "Cross-check" note below for why `deluge_sample_source::geometry::resident_bytes_for`
//! does NOT share this truncation.

/// Per-cluster geometry `begin` needs, mirroring the fields of
/// `DelugeStreamingFillContext` (`include/libdeluge/streaming_fill.h`) that
/// `begin_fill` itself reads. `first_cluster_index_with_no_audio_data` and
/// `raw_data_format` are carried here only for a LATER task's convert step
/// (`fill_logic::finish_convert_stitch`, SR2d-4 Task 5) — `begin` itself doesn't
/// touch either field, so they're `#[allow(dead_code)]` for now rather than a
/// leaner struct that Task 5 would just have to widen back out again.
///
/// `#[allow(dead_code)]` on the struct itself (not just its two forward-looking
/// fields, above): `deluge-bsp-rust` is bin-only (no `[lib]` target — see
/// `Cargo.toml`), so on a plain device/host build nothing outside `#[cfg(test)]`
/// constructs this type yet — same "not called anywhere outside tests" situation
/// `fill_sidecar::get`/`set`/`chunk_cap_fits` document for their own unused-until-
/// wired items.
#[allow(dead_code)]
pub struct FillGeometry {
    pub audio_data_start_pos_bytes: u32,
    pub audio_data_length_bytes: u64,
    #[allow(dead_code)]
    pub first_cluster_index_with_no_audio_data: i32,
    pub cluster_size: u32,
    pub cluster_size_magnitude: u32,
    #[allow(dead_code)]
    pub raw_data_format: u8,
}

/// The "still recording, length unknown" sentinel (`sample_recorder.cpp`) —
/// identical to `deluge_sample_source::geometry::UNKNOWN_LENGTH_SENTINEL`, kept as
/// its own literal here rather than importing that crate (see the module doc's
/// "Cross-check" note for why the two modules stay independent).
///
/// `#[allow(dead_code)]`: only referenced from `begin` (dead itself until wired,
/// see `FillGeometry`'s doc) and from the test module.
#[allow(dead_code)]
const UNKNOWN_LENGTH_SENTINEL: u64 = 0x8FFF_FFFF_FFFF_FFFF;

/// The resolved byte range for one cluster's read: how many 512-byte sectors to
/// transfer and where the cluster starts in the file, or `ok = false` if the
/// geometry places this cluster entirely past the audio data (a fill that
/// shouldn't have been requested — `begin_fill`'s own "Shouldn't really still
/// happen" `D_PRINTLN`).
///
/// `#[allow(dead_code)]`: not constructed outside tests yet — see `FillGeometry`'s
/// doc.
#[allow(dead_code)]
pub struct BeginResult {
    pub num_sectors: u32,
    pub byte_offset: u32,
    pub ok: bool,
}

/// Reimplements `begin_fill`'s short-last-cluster sector math from geometry alone.
///
/// `index` is the cluster index (`StreamedChunk::cluster_index`, always
/// non-negative in practice — mirrored here as `u32` rather than `begin_fill`'s
/// `int32_t` since the shifts below produce the identical bit pattern either way
/// for any in-range index).
///
/// Not called anywhere yet outside tests — wiring this into `ProdOps::begin` is
/// SR2d-4 Task 5 (see the module doc).
#[allow(dead_code)]
pub fn begin(index: u32, geo: &FillGeometry) -> BeginResult {
    let mut num_sectors: u32 = geo.cluster_size >> 9;

    // `begin_fill`: `if (sample->audioDataLengthBytes && sample->audioDataLengthBytes
    // != 0x8FFFFFFFFFFFFFFF)` — a zero length short-circuits the `&&` exactly like the
    // sentinel does, so both fall straight through to the full-cluster default below.
    if geo.audio_data_length_bytes != 0 && geo.audio_data_length_bytes != UNKNOWN_LENGTH_SENTINEL {
        // `uint32_t audioDataEndPosBytes = sample->audioDataLengthBytes +
        // sample->audioDataStartPosBytes;` — 64-bit add, then truncated to 32 bits by
        // the implicit narrowing conversion into the `uint32_t` local. `wrapping_add`
        // then `as u32` reproduces that truncation bit-for-bit (see module doc).
        let audio_data_end_pos_bytes =
            geo.audio_data_length_bytes
                .wrapping_add(geo.audio_data_start_pos_bytes as u64) as u32;
        // `uint32_t startByteThisCluster = clusterIndex << Cluster::size_magnitude;`
        let start_byte_this_cluster = index << geo.cluster_size_magnitude;
        // `int32_t bytesToRead = audioDataEndPosBytes - startByteThisCluster;` — unsigned
        // 32-bit subtraction, then reinterpreted as `int32_t`; `wrapping_sub` then `as
        // i32` is the same bit-preserving conversion C++'s narrowing-to-signed does here.
        let bytes_to_read = audio_data_end_pos_bytes.wrapping_sub(start_byte_this_cluster) as i32;
        if bytes_to_read <= 0 {
            // `D_PRINTLN("fail thing"); // Shouldn't really still happen` — geometry
            // places this cluster entirely past the audio data; skip the read.
            return BeginResult {
                num_sectors: 0,
                byte_offset: 0,
                ok: false,
            };
        }
        if bytes_to_read < geo.cluster_size as i32 {
            // `numSectors = ((bytesToRead - 1) >> 9) + 1;` — `bytes_to_read > 0` here
            // (checked above), so `bytes_to_read - 1 >= 0`: no negative right-shift.
            num_sectors = (((bytes_to_read - 1) >> 9) + 1) as u32;
        }
        // Otherwise, just leave it at the normal number of sectors.
    }

    // `byte_offset = static_cast<uint32_t>(clusterIndex) << Cluster::size_magnitude;`
    // — the same shift as `start_byte_this_cluster` above, computed unconditionally
    // (the un-taken branch above never needed it, but the formula is identical).
    let byte_offset = index << geo.cluster_size_magnitude;

    BeginResult {
        num_sectors,
        byte_offset,
        ok: true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 32 KiB clusters (magnitude 15), matching a common real-world FAT cluster
    /// size — same value `fill_sidecar::SIDECAR_CAP`'s doc cites as "a common
    /// real-world default".
    const CLUSTER_SIZE: u32 = 32768;
    const CLUSTER_MAGNITUDE: u32 = 15;

    fn geo(audio_data_length_bytes: u64) -> FillGeometry {
        FillGeometry {
            audio_data_start_pos_bytes: 0,
            audio_data_length_bytes,
            first_cluster_index_with_no_audio_data: -1,
            cluster_size: CLUSTER_SIZE,
            cluster_size_magnitude: CLUSTER_MAGNITUDE,
            raw_data_format: 0,
        }
    }

    #[test]
    fn full_cluster_at_index_zero() {
        // Audio data extends well past this cluster's end (32768..65536).
        let g = geo(100_000);
        let r = begin(0, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, CLUSTER_SIZE >> 9); // 64
        assert_eq!(r.byte_offset, 0);
    }

    #[test]
    fn full_cluster_at_mid_stream_index() {
        // index 1 -> start_byte_this_cluster = 32768; audio ends at 100000, still
        // more than a full cluster ahead (67232 bytes to read).
        let g = geo(100_000);
        let r = begin(1, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, 64);
        assert_eq!(r.byte_offset, 32768);
    }

    #[test]
    fn short_last_cluster_rounds_sectors_up() {
        // index 3 -> start_byte_this_cluster = 98304; audio ends at 100000, so
        // bytes_to_read = 1696 (< cluster_size): short last cluster.
        // ((1696 - 1) >> 9) + 1 = (1695 >> 9) + 1 = 3 + 1 = 4 sectors (2048 bytes,
        // the smallest whole-sector count covering 1696 bytes).
        //
        // Cross-check: `deluge_sample_source::geometry::resident_bytes_for` with the
        // same geometry (in bytes, not sectors) returns exactly 1696 residency bytes
        // for this index — 4 sectors * 512 = 2048 bytes safely covers that. See the
        // module doc's "Cross-check" note for why this is asserted by hand here
        // rather than by importing that crate.
        let g = geo(100_000);
        let r = begin(3, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, 4);
        assert_eq!(r.byte_offset, 98304);
    }

    #[test]
    fn cluster_wholly_past_audio_end_is_not_ok() {
        // index 4 -> start_byte_this_cluster = 131072, already past audio_data_end
        // (100000): bytes_to_read wraps negative -> ok=false, geometry error.
        let g = geo(100_000);
        let r = begin(4, &g);
        assert!(!r.ok);
        assert_eq!(r.num_sectors, 0);
        assert_eq!(r.byte_offset, 0);
    }

    #[test]
    fn unknown_length_sentinel_yields_full_cluster() {
        let g = geo(UNKNOWN_LENGTH_SENTINEL);
        let r = begin(7, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, 64);
        assert_eq!(r.byte_offset, 7 << CLUSTER_MAGNITUDE);
    }

    #[test]
    fn zero_length_yields_full_cluster() {
        let g = geo(0);
        let r = begin(2, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, 64);
        assert_eq!(r.byte_offset, 2 << CLUSTER_MAGNITUDE);
    }

    #[test]
    fn nonzero_audio_data_start_pos_shifts_the_end() {
        // A sample whose WAV data starts 1000 bytes into the file: audio_data_end =
        // 1000 + 50000 = 51000. index 1 -> start_byte_this_cluster = 32768,
        // bytes_to_read = 18232 (< cluster_size): short last cluster.
        // ((18232 - 1) >> 9) + 1 = (18231 >> 9) + 1 = 35 + 1 = 36 sectors.
        let mut g = geo(50_000);
        g.audio_data_start_pos_bytes = 1000;
        let r = begin(1, &g);
        assert!(r.ok);
        assert_eq!(r.num_sectors, 36);
        assert_eq!(r.byte_offset, 32768);
    }
}
