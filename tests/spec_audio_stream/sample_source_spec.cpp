// tests/spec_audio_stream/sample_source_spec.cpp
//
// SR1 Task 3: specs for the region port (include/libdeluge/sample_source.h, backed by
// src/deluge/storage/audio/stream/sample_source.cpp) -- the port's OWN wrapper logic, not the real
// SampleStream<->SD integration (that's the golden gate in later SR1 tasks). Exercised against a
// scripted, in-memory `deluge::audio::stream::SampleStream` test double
// (fake_include/storage/audio/stream/sample_stream.h, shadowing the real header via include-path
// ordering -- see that file's comment) plus a real chunk-pointer-keyed lease refcount
// (sample_source_test_support.cpp), so prefetch promotion, the not-ready mapping, and lease
// hygiene (including the Task 2 double-acquire-leak fix) are all real assertions, not
// rubber-stamped by a no-op resource manager.
#include "libdeluge/sample_source.h"
#include "sample_source_test_support.h"
#include "storage/audio/stream/sample_stream.h" // the fake (see fake_include/)

#include "cppspec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kClusterSize = 16;

// A cluster-index-dependent, byte-distinguishable fixture: cluster i's payload is
// [(i*100)%256, (i*100+1)%256, ...] -- distinct enough that reading the wrong cluster's data is
// caught by an equality check against the expected cluster's ramp.
std::vector<std::byte> make_ramp(uint32_t cluster_index, size_t n) {
	std::vector<std::byte> out(n);
	for (size_t k = 0; k < n; ++k) {
		out[k] = static_cast<std::byte>((cluster_index * 100 + k) & 0xFF);
	}
	return out;
}

DelugeSampleGeometry make_geometry(uint64_t audio_data_length_bytes) {
	return DelugeSampleGeometry{
	    .audio_data_start_bytes = 0,
	    .audio_data_length_bytes = audio_data_length_bytes,
	    .cluster_size_bytes = kClusterSize,
	    .byte_depth = 2,
	    .num_channels = 1,
	    .raw_data_format = 0,
	};
}

} // namespace

// clang-format off
describe sample_source("deluge_sample_source_* (region port)", $ {
	it("acquire(0) is Ready with the ramp payload and full resident_bytes; the last cluster is short", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		stream.set_cluster_data(2, make_ramp(2, kClusterSize));

		// 40 bytes over 16-byte clusters: clusters 0/1 full, cluster 2 an 8-byte tail.
		DelugeSampleGeometry geo = make_geometry(40);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		bool ok = deluge_sample_region_acquire(src, /*index=*/0, /*direction=*/1, /*priority=*/0, &out);
		expect(ok).to_equal(true);
		expect(out.payload_base != nullptr).to_equal(true);
		expect(out.region_index).to_equal(0u);
		expect(out.resident_bytes).to_equal(kClusterSize);
		auto ramp0 = make_ramp(0, kClusterSize);
		auto* bytes0 = static_cast<std::byte*>(out.payload_base);
		expect(std::equal(ramp0.begin(), ramp0.end(), bytes0)).to_equal(true);

		ok = deluge_sample_region_acquire(src, /*index=*/1, /*direction=*/1, /*priority=*/0, &out);
		expect(ok).to_equal(true);
		expect(out.resident_bytes).to_equal(kClusterSize); // still a full cluster

		ok = deluge_sample_region_acquire(src, /*index=*/2, /*direction=*/1, /*priority=*/0, &out);
		expect(ok).to_equal(true);
		expect(out.region_index).to_equal(2u);
		expect(out.resident_bytes).to_equal(8u); // 40 - 32 = 8: the short last cluster

		deluge_sample_region_release(src, out.lease);
		deluge_sample_source_close(src);
	});

	it("a prefetch-primed acquire is a resident hit -- no new backing read for that index", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		for (uint32_t i = 0; i < 3; ++i) {
			stream.set_cluster_data(i, make_ramp(i, kClusterSize));
		}
		DelugeSampleGeometry geo = make_geometry(48); // 3 full clusters
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		expect(deluge_sample_region_acquire(src, 0, 1, 0, &out)).to_equal(true);
		expect(stream.get_cluster_calls(0)).to_equal(1);
		expect(stream.get_cluster_calls(1)).to_equal(1); // acquire(0)'s prefetch of cluster 1
		expect(stream.get_cluster_calls()).to_equal(2);

		expect(deluge_sample_region_acquire(src, 1, 1, 0, &out)).to_equal(true);
		// Served from the standing prefetch: NOT a new backing read for cluster 1.
		expect(stream.get_cluster_calls(1)).to_equal(1);
		expect(stream.get_cluster_calls(2)).to_equal(1); // acquire(1)'s own prefetch of cluster 2
		expect(stream.get_cluster_calls()).to_equal(3);
		expect(out.region_index).to_equal(1u);
		auto ramp1 = make_ramp(1, kClusterSize);
		auto* bytes1 = static_cast<std::byte*>(out.payload_base);
		expect(std::equal(ramp1.begin(), ramp1.end(), bytes1)).to_equal(true);

		deluge_sample_region_release(src, out.lease);
		deluge_sample_source_close(src);
	});

	it("acquire on a not-yet-loaded cluster returns false and leaves `out` untouched", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(2);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		// Cluster 1 deliberately left un-set: constructed but not loaded.
		DelugeSampleGeometry geo = make_geometry(32);
		auto* src = deluge_sample_source_open(&stream, geo);

		void* sentinel_ptr = reinterpret_cast<void*>(0xdeadbeefULL);
		DelugeSampleRegion out{
		    .payload_base = sentinel_ptr, .region_index = 999, .resident_bytes = 999, .lease = 999};
		bool ok = deluge_sample_region_acquire(src, /*index=*/1, /*direction=*/1, /*priority=*/0, &out);
		expect(ok).to_equal(false);
		expect(out.payload_base == sentinel_ptr).to_equal(true);
		expect(out.region_index).to_equal(999u);
		expect(out.resident_bytes).to_equal(999u);
		expect(out.lease).to_equal(uint64_t{999});
		// The probing lease taken on cluster 1 must be released on the NotReady path, not leaked.
		expect(deluge_test_total_lease_count()).to_equal(0u);

		deluge_sample_source_close(src);
	});

	it("normal acquire/release then close drops every lease", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(2);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		DelugeSampleGeometry geo = make_geometry(32);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		expect(deluge_sample_region_acquire(src, 0, 1, 0, &out)).to_equal(true);
		// current (cluster 0) + prefetch (cluster 1) both hold a lease at this point.
		expect(deluge_test_total_lease_count()).to_equal(2u);

		deluge_sample_region_release(src, out.lease);
		expect(deluge_test_total_lease_count()).to_equal(1u); // prefetch still held

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("acquiring two different indices without an intervening release leaks no lease "
	   "(locks in the Task 2 stale-current auto-release fix)", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(4);
		for (uint32_t i = 0; i < 4; ++i) {
			stream.set_cluster_data(i, make_ramp(i, kClusterSize));
		}
		DelugeSampleGeometry geo = make_geometry(64); // 4 full clusters
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out0{};
		expect(deluge_sample_region_acquire(src, 0, 1, 0, &out0)).to_equal(true);
		expect(deluge_test_total_lease_count()).to_equal(2u); // current(0) + prefetch(1)

		// Deliberately NOT releasing out0's lease before re-acquiring a different, non-prefetched
		// index -- this is the scenario the Task 2 fix (152ecdc43) made safe.
		DelugeSampleRegion out2{};
		expect(deluge_sample_region_acquire(src, 2, 1, 0, &out2)).to_equal(true);
		expect(out2.region_index).to_equal(2u);
		// current(2) + prefetch(3) held; the stale current(0) and superseded prefetch(1) leases
		// from the first acquire were both dropped as part of this second acquire, not leaked.
		expect(deluge_test_total_lease_count()).to_equal(2u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});
});

CPPSPEC_SPEC(sample_source)
