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

// 8b Task 1: DelugeRegionState has no ostream<< for CppSpec's matcher output, so states are compared
// as ints via these names -- a failure then reads "expected 1, got 2" against a legible constant.
constexpr int kReady = static_cast<int>(DELUGE_REGION_READY);
constexpr int kLoading = static_cast<int>(DELUGE_REGION_LOADING);
constexpr int kUnavailable = static_cast<int>(DELUGE_REGION_UNAVAILABLE);

int acquire_state(DelugeSampleSource* src, uint32_t index, int8_t direction, DelugeSampleRegion* out) {
	return static_cast<int>(deluge_sample_region_acquire_ex(src, index, direction, /*priority=*/0, out));
}

int region_state(const DelugeSampleSource* src, uint32_t index) {
	return static_cast<int>(deluge_sample_region_state(src, index));
}

/// A region descriptor pre-filled with recognisable junk, so "`out` was not written" is a real
/// assertion rather than an absence of one.
DelugeSampleRegion sentinel_region() {
	return DelugeSampleRegion{.payload_base = reinterpret_cast<void*>(0xdeadbeefULL),
	                          .region_index = 999,
	                          .resident_bytes = 999,
	                          .lease = 999};
}

bool is_untouched(const DelugeSampleRegion& out) {
	return out.payload_base == reinterpret_cast<void*>(0xdeadbeefULL) && out.region_index == 999u
	       && out.resident_bytes == 999u && out.lease == uint64_t{999};
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

		DelugeSampleRegion out = sentinel_region();
		bool ok = deluge_sample_region_acquire(src, /*index=*/1, /*direction=*/1, /*priority=*/0, &out);
		expect(ok).to_equal(false);
		expect(is_untouched(out)).to_equal(true);
		// 8b Task 1: the boolean wrapper still says NotReady, but the lease is now RETAINED (the
		// scheduled fill must keep progressing) rather than released -- see the LOADING specs below.
		// close() is what finally drops it.
		expect(deluge_test_total_lease_count()).to_equal(1u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b: a scheduled-but-unloaded region is LOADING and its lease is RETAINED across the call", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(2);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		// Cluster 1: constructed and schedulable, but the fill has not landed.
		DelugeSampleGeometry geo = make_geometry(32);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out = sentinel_region();
		expect(acquire_state(src, /*index=*/1, /*direction=*/1, &out)).to_equal(kLoading);
		expect(is_untouched(out)).to_equal(true);
		// THE load-bearing assertion: the lease get_cluster() took is still held after the call
		// returns. Release it here and the background fill's chunk becomes stealable while the
		// caller is deferring -- the retry could then spin forever.
		expect(deluge_test_total_lease_count()).to_equal(1u);

		// A defer/retry cycle is idempotent: still LOADING, and leases do not accumulate.
		expect(acquire_state(src, 1, 1, &out)).to_equal(kLoading);
		expect(deluge_test_total_lease_count()).to_equal(1u);
		expect(acquire_state(src, 1, 1, &out)).to_equal(kLoading);
		expect(deluge_test_total_lease_count()).to_equal(1u);

		// When the fill lands, the same retry becomes READY and the retained lease folds into the
		// current pin -- exactly one lease on cluster 1, no duplicate from the retries.
		stream.set_cluster_loaded(1, true);
		expect(acquire_state(src, 1, 1, &out)).to_equal(kReady);
		expect(out.region_index).to_equal(1u);
		expect(out.resident_bytes).to_equal(kClusterSize);
		expect(deluge_test_total_lease_count()).to_equal(1u); // cluster 1 only: index 2 is out of range

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b: LOADING does not disturb the region the caller is still reading", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		// Cluster 2 left unloaded.
		DelugeSampleGeometry geo = make_geometry(48);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion current{};
		expect(acquire_state(src, 0, 1, &current)).to_equal(kReady);
		expect(deluge_test_total_lease_count()).to_equal(2u); // current(0) + prefetch(1)

		// Probing a region that is still loading must not release current(0)'s pin: the caller can
		// keep reading `current.payload_base` while it waits.
		DelugeSampleRegion probe = sentinel_region();
		expect(acquire_state(src, 2, 1, &probe)).to_equal(kLoading);
		expect(is_untouched(probe)).to_equal(true);
		auto ramp0 = make_ramp(0, kClusterSize);
		auto* bytes0 = static_cast<std::byte*>(current.payload_base);
		expect(std::equal(ramp0.begin(), ramp0.end(), bytes0)).to_equal(true);
		// current(0) + prefetch(1) + the retained pending(2).
		expect(deluge_test_total_lease_count()).to_equal(3u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b: an unreservable region is UNAVAILABLE and leaves the lease count unchanged", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		for (uint32_t i = 0; i < 3; ++i) {
			stream.set_cluster_data(i, make_ramp(i, kClusterSize));
		}
		stream.set_cluster_unavailable(2); // no free RAM / nothing stealable for cluster 2
		DelugeSampleGeometry geo = make_geometry(48);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		expect(acquire_state(src, 0, 1, &out)).to_equal(kReady);
		expect(deluge_test_total_lease_count()).to_equal(2u); // current(0) + prefetch(1)

		DelugeSampleRegion probe = sentinel_region();
		expect(acquire_state(src, 2, 1, &probe)).to_equal(kUnavailable);
		expect(is_untouched(probe)).to_equal(true);
		// Nothing leaked and nothing retained for the region that could not be reserved: there is no
		// fill in flight to keep alive, so the count is exactly what it was before the call.
		expect(deluge_test_total_lease_count()).to_equal(2u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b: a pending LOADING region that becomes unreservable is dropped, not stranded", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(2);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		// Cluster 1 constructed but unloaded.
		DelugeSampleGeometry geo = make_geometry(32);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		expect(acquire_state(src, 1, 1, &out)).to_equal(kLoading);
		expect(deluge_test_total_lease_count()).to_equal(1u);

		// The chunk is reclaimed out from under the wait; the retry now reports UNAVAILABLE and the
		// retained lease is released rather than held forever on a dead reservation.
		stream.set_cluster_unavailable(1);
		expect(acquire_state(src, 1, 1, &out)).to_equal(kUnavailable);
		expect(deluge_test_total_lease_count()).to_equal(0u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b Task 1: deluge_sample_region_state reports the neighbour's residency, by index, "
	   "without acquiring it", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		stream.set_cluster_data(2, make_ramp(2, kClusterSize), /*loaded=*/false);
		DelugeSampleGeometry geo = make_geometry(48);
		auto* src = deluge_sample_source_open(&stream, geo);

		expect(region_state(nullptr, 0)).to_equal(kUnavailable); // null-safe, like every other entry point
		expect(region_state(src, 0)).to_equal(kUnavailable);     // nothing tracked yet
		expect(region_state(src, 1)).to_equal(kUnavailable);

		DelugeSampleRegion out{};
		expect(acquire_state(src, 0, 1, &out)).to_equal(kReady);
		expect(stream.get_cluster_calls()).to_equal(2); // cluster 0 + its prefetch of cluster 1
		// The current region: READY (see deluge_sample_region_state's doc -- `current` is only ever
		// pinned once loaded).
		expect(region_state(src, 0)).to_equal(kReady);
		// The loaded neighbour: READY too, and purely observed -- no extra backing read, no lease.
		expect(region_state(src, 1)).to_equal(kReady);
		expect(stream.get_cluster_calls()).to_equal(2);
		expect(deluge_test_total_lease_count()).to_equal(2u);

		// Advance: cluster 2 is now the prefetched neighbour, and it has not loaded.
		expect(acquire_state(src, 1, 1, &out)).to_equal(kReady);
		expect(region_state(src, 2)).to_equal(kLoading);
		expect(stream.get_cluster_calls()).to_equal(3);

		// It lands -- the same indexed observation now reports READY, still without acquiring.
		stream.set_cluster_loaded(2, true);
		expect(region_state(src, 2)).to_equal(kReady);
		expect(stream.get_cluster_calls()).to_equal(3);

		// Past the last cluster there is no neighbour to look ahead to: UNAVAILABLE for any index
		// this cursor isn't tracking, including the now-superseded index 1.
		expect(acquire_state(src, 2, 1, &out)).to_equal(kReady);
		expect(region_state(src, 3)).to_equal(kUnavailable); // out of range, nothing to prefetch
		expect(region_state(src, 1)).to_equal(kUnavailable); // superseded, no longer tracked

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b Task 1: a LOADING acquire that consumes the standing prefetch still reports a truthful "
	   "state for that same index (the in-flight `pending` reservation, not UNAVAILABLE)", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(3);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize), /*loaded=*/false);
		stream.set_cluster_data(2, make_ramp(2, kClusterSize));
		DelugeSampleGeometry geo = make_geometry(48);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		// acquire(0) establishes cluster 1 as the standing prefetch (unloaded).
		expect(acquire_state(src, 0, 1, &out)).to_equal(kReady);
		expect(region_state(src, 1)).to_equal(kLoading); // the standing prefetch, not yet landed

		// The caller now asks for exactly that prefetched index. It is not loaded, so this promotes
		// the prefetch lease into `pending` and returns LOADING -- and, as a side effect, EMPTIES the
		// prefetch slot (acquire_ex's documented behaviour).
		DelugeSampleRegion probe = sentinel_region();
		expect(acquire_state(src, 1, 1, &probe)).to_equal(kLoading);
		expect(is_untouched(probe)).to_equal(true);

		// THE load-bearing assertion: a fill on cluster 1 is genuinely still in flight (it is held as
		// `pending` now instead of `prefetch`), so querying index 1 must still say LOADING -- NOT
		// UNAVAILABLE. Reporting UNAVAILABLE here would tell a deferring caller "nothing further to
		// wait for" while data is actively being loaded, driving it to give up prematurely. This is
		// the "promoted prefetch" case, where `pending`'s subject happens to coincide with what was
		// the true neighbour -- the NEXT spec covers the case where it does not.
		expect(region_state(src, 1)).to_equal(kLoading);

		// When the fill lands, the same indexed query reports READY -- still without acquiring.
		stream.set_cluster_loaded(1, true);
		expect(region_state(src, 1)).to_equal(kReady);

		// And the retry that actually acquires it lands cleanly, folding the pending lease into
		// `current` with no leak and no duplicate.
		expect(acquire_state(src, 1, 1, &probe)).to_equal(kReady);
		expect(probe.region_index).to_equal(1u);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b Task 1: an indexed query disambiguates the current region from the TRUE neighbour after a "
	   "jump-ahead acquire bypasses the standing prefetch (the review-found defect)", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(7);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		// Cluster 5: constructed but not yet loaded -- the caller is about to jump straight to it,
		// bypassing the standing prefetch of cluster 1 entirely (no acquire of clusters 2-4).
		stream.set_cluster_data(5, make_ramp(5, kClusterSize), /*loaded=*/false);
		stream.set_cluster_data(6, make_ramp(6, kClusterSize));
		DelugeSampleGeometry geo = make_geometry(7 * kClusterSize);
		auto* src = deluge_sample_source_open(&stream, geo);

		DelugeSampleRegion out{};
		// 1. acquire_ex(0, +1) -> READY: current=0, standing prefetch=1 (already loaded).
		expect(acquire_state(src, 0, 1, &out)).to_equal(kReady);
		expect(region_state(src, 1)).to_equal(kReady); // the standing prefetch

		// 2. The caller jumps ahead to index 5 -- well past the prefetched index 1, and NOT the
		//    standing prefetch, so no promotion happens; get_cluster(5) is called fresh. It isn't
		//    loaded, so this is LOADING. Per acquire_ex's doc, the LOADING path never reaches the
		//    prefetch-scheduling step, so the standing prefetch of cluster 1 is left exactly as it
		//    was -- now stale relative to where the cursor actually is (index 6 is the TRUE neighbour
		//    of index 5, but nothing has scheduled it).
		DelugeSampleRegion probe = sentinel_region();
		expect(acquire_state(src, 5, 1, &probe)).to_equal(kLoading);
		expect(is_untouched(probe)).to_equal(true);

		// THE load-bearing assertions -- this is the defect the old unindexed prefetch_state() could
		// not detect (it always answered "pending if set, else prefetch", regardless of what index
		// the caller actually meant):
		//
		// Index 5 -- what acquire_ex just answered about -- correctly reports LOADING (via `pending`).
		expect(region_state(src, 5)).to_equal(kLoading);
		// Index 6 -- the TRUE neighbour of the cursor's real position -- must NOT inherit index 5's
		// LOADING state. Nothing tracks index 6 (the LOADING path never scheduled a prefetch for it),
		// so it is UNAVAILABLE: "nothing in flight for it right now" (index 6 could still load fine
		// on a fresh acquire_ex(6, ...) later), not a false LOADING borrowed from an unrelated index.
		expect(region_state(src, 6)).to_equal(kUnavailable);
		// And the stale standing prefetch (index 1) is untouched by the jump, and stays
		// distinguishable from both 5 and 6 -- an indexed query never conflates the three.
		expect(region_state(src, 1)).to_equal(kReady);

		deluge_sample_source_close(src);
		expect(deluge_test_total_lease_count()).to_equal(0u);
	});

	it("8b: acquire_ex's READY path is the boolean acquire's true, unchanged", _ {
		deluge_test_reset_lease_tracking();
		deluge::audio::stream::SampleStream stream(2);
		stream.set_cluster_data(0, make_ramp(0, kClusterSize));
		stream.set_cluster_data(1, make_ramp(1, kClusterSize));
		DelugeSampleGeometry geo = make_geometry(32);
		auto* src = deluge_sample_source_open(&stream, geo);

		auto* src_bool = deluge_sample_source_open(&stream, geo); // a second cursor, so neither call re-acquires

		DelugeSampleRegion via_ex{};
		expect(acquire_state(src, 0, 1, &via_ex)).to_equal(kReady);
		DelugeSampleRegion via_bool{};
		expect(deluge_sample_region_acquire(src_bool, 0, 1, 0, &via_bool)).to_equal(true);
		expect(via_bool.payload_base == via_ex.payload_base).to_equal(true);
		expect(via_bool.region_index).to_equal(via_ex.region_index);
		expect(via_bool.resident_bytes).to_equal(via_ex.resident_bytes);
		expect(via_bool.lease).to_equal(via_ex.lease);

		// ...and the wrapper is false for both non-READY states.
		DelugeSampleRegion probe{};
		stream.set_cluster_loaded(1, false);
		expect(deluge_sample_region_acquire(src_bool, 1, 1, 0, &probe)).to_equal(false); // LOADING
		expect(deluge_sample_region_acquire(src, 5, 1, 0, &probe)).to_equal(false);     // UNAVAILABLE

		deluge_sample_source_close(src);
		deluge_sample_source_close(src_bool);
		expect(deluge_test_total_lease_count()).to_equal(0u);
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
