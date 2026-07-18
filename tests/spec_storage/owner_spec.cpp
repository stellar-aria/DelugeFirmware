// tests/spec_storage/owner_spec.cpp
#include "storage/owner.h"

#include "cppspec.hpp"

using deluge::storage::Owner;

// clang-format off
describe owner("deluge::storage::Owner", $ {
	it("runs the op (inline on host)", _ {
		static int ran = 0;
		ran = 0;
		Owner::run([](void* ctx) { *static_cast<int*>(ctx) += 1; }, &ran);
		// Host build: deluge_worker_run runs inline, so the op has completed.
		expect(ran).to_equal(1);
	});

	it("runs an sd-routine op (inline on host)", _ {
		static int ran = 0;
		ran = 0;
		Owner::run_sd_routine([](void* ctx) { *static_cast<int*>(ctx) += 1; }, &ran);
		expect(ran).to_equal(1);
	});
});
// clang-format on

CPPSPEC_SPEC(owner)
