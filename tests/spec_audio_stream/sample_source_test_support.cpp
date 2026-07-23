// SR1 Task 3 support TU for sample_source_spec.cpp: defines the handful of symbols
// storage/cluster/cluster.h declares but storage/cluster/cluster.cpp (not linked into this driver)
// normally provides, so sample_source.cpp + the fake SampleStream
// (fake_include/storage/audio/stream/sample_stream.h) link and behave against a real refcount
// instead of the resource-manager singleton. See that header's top comment for the full rationale.
#include "sample_source_test_support.h"

#include "storage/cluster/cluster.h"

#include <stdexcept>
#include <unordered_map>

// SR1 Task 4: sample_source.cpp's pool-exhaustion path calls FREEZE_WITH_ERROR (freezeWithError), the
// terminal panic primitive the firmware/host-sim would provide. It isn't linked into this driver, so
// supply a fake that throws instead of halting — the specs never exhaust the pool (they open one source
// at a time), so this only needs to resolve the symbol; throwing keeps an accidental exhaustion visible
// rather than silently looping.
extern "C" void freezeWithError(char const* errmsg) {
	throw std::runtime_error(errmsg != nullptr ? errmsg : "freezeWithError");
}

// Cluster's slab-geometry statics. StreamedChunk::payload() (called by sample_source.cpp) reads
// Cluster::size directly; the real cluster.cpp isn't linked here, so it must be defined once,
// somewhere. 16 bytes keeps the specs' ramp fixtures small and easy to eyeball.
size_t Cluster::size = 16;
size_t Cluster::size_magnitude = 4; // log2(16)

namespace {
std::unordered_map<const void*, uint32_t> g_lease_counts;
} // namespace

namespace deluge::cluster {

void add_lease(void* chunk) {
	if (chunk == nullptr) {
		return;
	}
	++g_lease_counts[chunk];
}

void release_lease(void* chunk) {
	if (chunk == nullptr) {
		return;
	}
	auto it = g_lease_counts.find(chunk);
	if (it != g_lease_counts.end() && it->second > 0) {
		--it->second;
	}
}

} // namespace deluge::cluster

uint32_t deluge_test_total_lease_count() {
	uint32_t total = 0;
	for (auto& [ptr, count] : g_lease_counts) {
		total += count;
	}
	return total;
}

void deluge_test_reset_lease_tracking() {
	g_lease_counts.clear();
}
