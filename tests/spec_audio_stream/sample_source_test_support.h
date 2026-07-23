#pragma once
// SR1 Task 3 test-only lease accounting, paired with the deluge::cluster::add_lease/release_lease
// fakes defined in sample_source_test_support.cpp. Those fakes stand in for the real,
// resource-manager-backed implementations in storage/cluster/cluster.cpp (not linked into this
// driver -- see fake_include/storage/audio/stream/sample_stream.h's header comment for why), so
// sample_source_spec.cpp can assert real lease-count behaviour (no resource-manager singleton, no
// card) instead of trusting the port's bookkeeping on faith.
#include <cstdint>

/// @return The held-lease count summed across every chunk this test process has seen, net of
///         add_lease()/release_lease() calls since the last deluge_test_reset_lease_tracking().
uint32_t deluge_test_total_lease_count();

/// Clear all tracked lease counts. Call at the top of each spec case for test isolation.
void deluge_test_reset_lease_tracking();
