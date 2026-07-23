#pragma once
// TEST DOUBLE for storage/audio/stream/sample_stream.h (SR1 Task 3, sample_source_spec.cpp).
//
// sample_source.cpp (the region port) reinterpret_casts its opaque `stream_backing` to
// `deluge::audio::stream::SampleStream*` and calls exactly two members on it: get_cluster() and
// num_clusters(). The REAL SampleStream needs a live `Sample&` plus the Rust resource-manager
// singleton and cluster slab just to construct -- disproportionate for testing the port's OWN
// wrapper logic (prefetch promotion, stale-current auto-release, not-ready mapping, lease
// accounting), which is this test's actual job. The real SampleStream<->SD integration is covered
// by the golden gate in later SR1 tasks, not here.
//
// This header shadows the real one via include-path ordering in CMakeLists.txt (the same technique
// this driver already uses for sim/compat's <arm_neon.h> -> SIMDe shim): it is the ONLY definition
// of `deluge::audio::stream::SampleStream` linked into this driver -- the real sample_stream.cpp is
// never compiled in. A scripted, in-memory cluster pool stands in for the residency table.
// `deluge::cluster::add_lease`/`release_lease` are faked to match, in sample_source_test_support.cpp
// (which also defines the `Cluster::size`/`size_magnitude` statics StreamedChunk::payload() reads,
// since the real storage/cluster/cluster.cpp isn't linked either) -- so lease-hygiene bugs in the
// port (e.g. the double-acquire leak fixed in SR1 Task 2) are still caught for real.
#include "definitions_cxx.hpp"       // Error, ClusterLoad (CLUSTER_ENQUEUE)
#include "storage/cluster/cluster.h" // StreamedChunk, Cluster::size, deluge::cluster::add_lease

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace deluge::audio::stream {

class SampleStream {
public:
	explicit SampleStream(size_t num_clusters) : slots_(num_clusters) {
		for (size_t i = 0; i < num_clusters; ++i) {
			Slot& slot = slots_[i];
			slot.storage.assign(Cluster::size, std::byte{0});
			slot.chunk.cluster_index = static_cast<uint32_t>(i);
			slot.chunk.payload_ = slot.storage.data();
		}
	}

	/// @name Test configuration
	/// @{

	/// Fill cluster `index`'s payload with `bytes` (must fit within Cluster::size) and mark it
	/// loaded (matching the real materialize-then-read sequence) unless `loaded` is false.
	void set_cluster_data(uint32_t index, std::span<const std::byte> bytes, bool loaded = true) {
		Slot& slot = slots_.at(index);
		std::copy(bytes.begin(), bytes.end(), slot.storage.begin());
		slot.chunk.loaded = loaded;
	}

	/// Make cluster `index` un-reservable: get_cluster() returns nullptr for it (and takes no lease),
	/// standing in for the real "no free RAM / nothing stealable" outcome the region port maps to
	/// DELUGE_REGION_UNAVAILABLE. Distinct from an in-range-but-unloaded cluster (DELUGE_REGION_LOADING).
	void set_cluster_unavailable(uint32_t index, bool unavailable = true) {
		slots_.at(index).unavailable = unavailable;
	}

	/// Mark cluster `index` loaded (or not) without touching its payload — the "the fill landed"
	/// transition a deferring caller is waiting for.
	void set_cluster_loaded(uint32_t index, bool loaded) { slots_.at(index).chunk.loaded = loaded; }

	/// Number of get_cluster() calls made across every index (the "backing read" instrumentation).
	[[nodiscard]] int get_cluster_calls() const { return total_calls_; }
	/// Number of get_cluster() calls made for cluster `index` specifically.
	[[nodiscard]] int get_cluster_calls(uint32_t index) const { return slots_.at(index).call_count; }

	/// @}
	/// @name Production-shaped API -- what sample_source.cpp actually calls
	/// @{

	StreamedChunk* get_cluster(uint32_t index, int32_t /*load_instruction*/ = CLUSTER_ENQUEUE,
	                           uint32_t /*priority_rating*/ = 0xFFFFFFFF, Error* error = nullptr) {
		if (error != nullptr) {
			*error = Error::NONE;
		}
		if (index >= slots_.size()) {
			return nullptr;
		}
		Slot& slot = slots_[index];
		++slot.call_count;
		++total_calls_;
		if (slot.unavailable) {
			// The real get_cluster()'s no-RAM path: null return, and no lease taken.
			return nullptr;
		}
		// Mirrors get_cluster()'s real contract ("always takes a manager lease on a non-null
		// return") so the port's release-on-NotReady and stale-current-release paths are exercised
		// against a real refcount, not a no-op.
		deluge::cluster::add_lease(&slot.chunk);
		return &slot.chunk;
	}

	[[nodiscard]] size_t num_clusters() const { return slots_.size(); }

	/// @}

private:
	struct Slot {
		std::vector<std::byte> storage;
		StreamedChunk chunk{};
		int call_count = 0;
		bool unavailable = false;
	};
	std::vector<Slot> slots_;
	int total_calls_ = 0;
};

} // namespace deluge::audio::stream
