//! Per-sample audio geometry and the short-last-cluster residency math, reimplemented
//! natively from `sample_source.cpp:149-168`.

/// Immutable per-sample audio geometry, mirroring `DelugeSampleGeometry`
/// (`include/libdeluge/sample_source.h`), parsed once above the port.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Geometry {
    pub audio_data_start_bytes: u32,
    pub audio_data_length_bytes: u64,
    pub cluster_size_bytes: u32,
    pub byte_depth: u8,
    pub num_channels: u8,
    pub raw_data_format: u8,
}

/// "Still recording, length unknown" sentinel (see `sample_recorder.cpp`); leave the
/// full cluster resident.
const UNKNOWN_LENGTH_SENTINEL: u64 = 0x8FFF_FFFF_FFFF_FFFF;

/// Valid payload bytes for cluster `index`, clamping the last cluster to the
/// geometry's audio-data end (bytes, not sectors). Full cluster for a full cluster,
/// the remainder for a short last cluster, `0` for a cluster wholly past the end,
/// and full cluster under the unknown-length sentinel / zero length.
pub fn resident_bytes_for(index: u32, geo: &Geometry) -> u32 {
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

#[cfg(test)]
mod tests {
    use super::*;
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
    #[test]
    fn full_clusters_and_short_last_and_sentinel() {
        let g = geo(68); // 4*16 + 4 -> clusters 0..3 full, cluster 4 has 4 bytes
        assert_eq!(resident_bytes_for(0, &g), 16);
        assert_eq!(resident_bytes_for(3, &g), 16);
        assert_eq!(resident_bytes_for(4, &g), 4);
        // A cluster entirely past the end -> 0.
        assert_eq!(resident_bytes_for(5, &g), 0);
        // Still-recording sentinel -> full cluster regardless of index.
        let s = geo(0x8FFFFFFFFFFFFFFF);
        assert_eq!(resident_bytes_for(99, &s), 16);
        // Zero length -> full cluster (unknown length, same as sentinel guard).
        let z = geo(0);
        assert_eq!(resident_bytes_for(0, &z), 16);
    }
}
