//! The C++ region-port backing driven over FFI — one `RegionPortOps` impl.
//!
//! Wraps the `extern "C"` ABI in `include/libdeluge/sample_source.h` (backed by
//! `src/deluge/storage/audio/stream/sample_source.cpp`, compiled into this test
//! binary by `build.rs`) plus this crate's `harness_shim.cpp`, which builds the
//! fake `SampleStream` world the port reads through.
//!
//! # Singletons — one live at a time
//! `sample_source.cpp` uses PROCESS-WIDE state: the static source pool
//! `g_source_pool`, and the process-wide lease refcount behind the fakes. So two
//! `CppBackend`s must NEVER be live simultaneously — the differential runs them
//! strictly sequentially (`crate::diff::capture`), each opening its own source
//! and releasing it (pool slot + leases) on `Drop` before the next is built.
use crate::ops::{make_ramp, RawOutcome, Region, RegionPortOps, RegionState, CLUSTER_SIZE};
use std::os::raw::c_void;

#[repr(C)]
struct DelugeSampleSource {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DelugeSampleGeometry {
    audio_data_start_bytes: u32,
    audio_data_length_bytes: u64,
    cluster_size_bytes: u32,
    byte_depth: u8,
    num_channels: u8,
    raw_data_format: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DelugeSampleRegion {
    payload_base: *mut c_void,
    region_index: u32,
    resident_bytes: u32,
    lease: u64,
}

extern "C" {
    // The region-port ABI (include/libdeluge/sample_source.h).
    fn deluge_sample_source_open(
        stream_backing: *mut c_void,
        geometry: DelugeSampleGeometry,
    ) -> *mut DelugeSampleSource;
    fn deluge_sample_region_acquire_ex(
        src: *mut DelugeSampleSource,
        index: u32,
        direction: i8,
        priority: u32,
        out: *mut DelugeSampleRegion,
    ) -> i32;
    fn deluge_sample_region_state(src: *const DelugeSampleSource, index: u32) -> i32;
    fn deluge_sample_region_retain(lease: u64);
    fn deluge_sample_region_release(lease: u64);
    fn deluge_sample_source_close(src: *mut DelugeSampleSource);

    // The harness shim (cpp/harness_shim.cpp) over the fake SampleStream.
    fn region_harness_stream_create(num_clusters: usize) -> *mut c_void;
    fn region_harness_stream_destroy(stream: *mut c_void);
    fn region_harness_set_cluster_data(
        stream: *mut c_void,
        index: u32,
        data: *const u8,
        len: usize,
        loaded: bool,
    );
    fn region_harness_set_cluster_unavailable(stream: *mut c_void, index: u32, unavailable: bool);
    fn region_harness_total_lease_count() -> u32;
    fn region_harness_reset_lease_tracking();
}

/// The world a backing is built over: how many clusters, the audio-data length
/// (drives the short-last-cluster `resident_bytes`), and which clusters are
/// deliberately not-yet-loaded (→ LOADING) or un-reservable (→ UNAVAILABLE).
/// Every loaded cluster is seeded with `make_ramp`. Both backends in a
/// differential build from the SAME `Scenario`, so a baseline C++-vs-C++ run is
/// byte-identical by construction.
#[derive(Debug, Clone)]
pub struct Scenario {
    pub num_clusters: u32,
    pub audio_data_length_bytes: u64,
    /// Clusters constructed but not marked loaded (acquire → LOADING).
    pub unloaded: Vec<u32>,
    /// Clusters `get_cluster()` returns null for (acquire → UNAVAILABLE).
    pub unavailable: Vec<u32>,
}

impl Scenario {
    /// A dense, all-loaded scenario over `num_clusters` clusters whose audio
    /// length leaves the last cluster short (so `resident_bytes` varies).
    pub fn dense(num_clusters: u32, audio_data_length_bytes: u64) -> Self {
        Scenario {
            num_clusters,
            audio_data_length_bytes,
            unloaded: Vec::new(),
            unavailable: Vec::new(),
        }
    }
}

/// The C++ region-port backing over one open source.
pub struct CppBackend {
    stream: *mut c_void,
    src: *mut DelugeSampleSource,
}

impl CppBackend {
    /// Build the fake-stream world from `scenario`, reset the process-wide lease
    /// refcount, and open one source over it.
    ///
    /// # Singleton discipline
    /// Only one `CppBackend` may be live at a time (see the module doc). Caller
    /// must drop the previous one first.
    pub fn open(scenario: &Scenario) -> Self {
        unsafe {
            region_harness_reset_lease_tracking();
            let stream = region_harness_stream_create(scenario.num_clusters as usize);
            assert!(!stream.is_null(), "harness stream create failed");
            for i in 0..scenario.num_clusters {
                if scenario.unavailable.contains(&i) {
                    region_harness_set_cluster_unavailable(stream, i, true);
                    continue;
                }
                let loaded = !scenario.unloaded.contains(&i);
                let ramp = make_ramp(i, CLUSTER_SIZE);
                region_harness_set_cluster_data(stream, i, ramp.as_ptr(), ramp.len(), loaded);
            }
            let geometry = DelugeSampleGeometry {
                audio_data_start_bytes: 0,
                audio_data_length_bytes: scenario.audio_data_length_bytes,
                cluster_size_bytes: CLUSTER_SIZE as u32,
                byte_depth: 2,
                num_channels: 1,
                raw_data_format: 0,
            };
            let src = deluge_sample_source_open(stream, geometry);
            assert!(!src.is_null(), "deluge_sample_source_open returned null (pool exhausted)");
            CppBackend { stream, src }
        }
    }
}

impl RegionPortOps for CppBackend {
    fn acquire(&mut self, index: u32, direction: i8, priority: u32) -> RawOutcome {
        let mut out = DelugeSampleRegion {
            payload_base: std::ptr::null_mut(),
            region_index: 0,
            resident_bytes: 0,
            lease: 0,
        };
        let raw = unsafe {
            deluge_sample_region_acquire_ex(self.src, index, direction, priority, &mut out)
        };
        let state = RegionState::from_raw(raw);
        let (region, lease) = if state == RegionState::Ready {
            // Copy resident_bytes out of the pinned payload NOW — never retain
            // the borrowed pointer across the next op (it may be re-pinned).
            let n = out.resident_bytes as usize;
            let payload = unsafe {
                assert!(!out.payload_base.is_null(), "READY region has null payload_base");
                std::slice::from_raw_parts(out.payload_base as *const u8, n).to_vec()
            };
            let region = Region {
                region_index: out.region_index,
                resident_bytes: out.resident_bytes,
                payload,
            };
            (Some(region), out.lease)
        } else {
            (None, 0)
        };
        RawOutcome { state, region, lease }
    }

    fn state(&self, index: u32) -> RegionState {
        RegionState::from_raw(unsafe { deluge_sample_region_state(self.src, index) })
    }

    fn retain(&mut self, lease: u64) {
        unsafe { deluge_sample_region_retain(lease) };
    }

    fn release(&mut self, lease: u64) {
        unsafe { deluge_sample_region_release(lease) };
    }

    fn total_leases(&self) -> u32 {
        unsafe { region_harness_total_lease_count() }
    }
}

impl Drop for CppBackend {
    fn drop(&mut self) {
        // Close the source (releases its pool slot + all held leases), then
        // destroy the fake stream — leaving the singletons clean for the next
        // sequential backend.
        unsafe {
            deluge_sample_source_close(self.src);
            region_harness_stream_destroy(self.stream);
        }
    }
}
