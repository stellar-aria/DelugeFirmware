// SR2c region-differential harness: extern "C" shim over the fake SampleStream.
//
// The fake `deluge::audio::stream::SampleStream`
// (tests/spec_audio_stream/fake_include/storage/audio/stream/sample_stream.h) is a C++ type the Rust
// driver cannot construct directly, and the lease-tracking helpers
// (deluge_test_total_lease_count / deluge_test_reset_lease_tracking) have C++ linkage. This shim wraps
// both behind a stable `extern "C"` surface so the Rust `CppBackend` can build the C++ port's world
// (create a stream with N clusters, seed cluster payloads, mark loaded/unavailable) and read the
// process-wide lease refcount that proves retain/release/close hygiene.
//
// It calls ONLY the fake's public test-configuration API + the port ABI; it adds no behaviour of its
// own, so the C++ backend the harness drives is exactly the one the CppSpec unit test exercises.
#include "sample_source_test_support.h"         // deluge_test_{total_lease_count,reset_lease_tracking}
#include "storage/audio/stream/sample_stream.h" // the fake SampleStream (via fake_include/)

#include <cstddef>
#include <cstdint>
#include <span>

using deluge::audio::stream::SampleStream;

extern "C" {

/// Create a fake stream with @p num_clusters clusters (each Cluster::size bytes, zeroed, not loaded).
void* region_harness_stream_create(size_t num_clusters) {
	return new SampleStream(num_clusters);
}

/// Destroy a stream previously returned by region_harness_stream_create.
void region_harness_stream_destroy(void* stream) {
	delete static_cast<SampleStream*>(stream);
}

/// Copy @p len bytes (must be <= Cluster::size) into cluster @p index's payload and set its loaded flag.
void region_harness_set_cluster_data(void* stream, uint32_t index, const uint8_t* data, size_t len, bool loaded) {
	auto* s = static_cast<SampleStream*>(stream);
	s->set_cluster_data(index, std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len), loaded);
}

/// Mark cluster @p index loaded (or not) without touching its payload.
void region_harness_set_cluster_loaded(void* stream, uint32_t index, bool loaded) {
	static_cast<SampleStream*>(stream)->set_cluster_loaded(index, loaded);
}

/// Make cluster @p index un-reservable (get_cluster() returns nullptr -> DELUGE_REGION_UNAVAILABLE).
void region_harness_set_cluster_unavailable(void* stream, uint32_t index, bool unavailable) {
	static_cast<SampleStream*>(stream)->set_cluster_unavailable(index, unavailable);
}

/// Total held-lease count across every chunk (net of add/release since the last reset).
uint32_t region_harness_total_lease_count() {
	return deluge_test_total_lease_count();
}

/// Clear the process-wide lease refcount — called at the top of each backend build for isolation.
void region_harness_reset_lease_tracking() {
	deluge_test_reset_lease_tracking();
}

} // extern "C"
