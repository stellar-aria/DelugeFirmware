// tests/spec_storage/coalescer_spec.cpp
#include "storage/owner.h"

#include "cppspec.hpp"

using deluge::storage::Coalescer;

// Test hook in mock_worker.cpp: when true, deluge_worker_run drops the dispatch
// (models the Embassy worker queue being full).
extern bool g_mock_worker_drop;

namespace {
// Shared test state. `reentrant_fill` re-enters `request()` while it is itself the in-flight
// fill, so the inner request MUST coalesce (single-flight) — proving the guard, not just that
// the fill ran. On host, Owner::run (mock_worker.cpp) runs the fill inline.
Coalescer* g_c = nullptr;
int g_runs = 0;

void plain_fill(void*) {
	g_runs += 1;
}

void reentrant_fill(void*) {
	g_runs += 1;
	g_c->request(reentrant_fill, nullptr); // in-flight → must be coalesced (no extra run)
}
} // namespace

// clang-format off
describe coalescer("deluge::storage::Coalescer", $ {
	it("runs the fill once (inline on host)", _ {
		Coalescer c;
		g_runs = 0;
		c.request(plain_fill, nullptr);
		expect(g_runs).to_equal(1);
	});

	it("coalesces a request issued while a fill is in flight", _ {
		Coalescer c;
		g_c = &c;
		g_runs = 0;
		c.request(reentrant_fill, nullptr);
		// reentrant_fill ran once; its inner request() saw the fill in flight and coalesced,
		// so the fill ran exactly once — not twice.
		expect(g_runs).to_equal(1);
	});

	it("releases after the fill so a later request runs again", _ {
		Coalescer c;
		g_runs = 0;
		c.request(plain_fill, nullptr);
		c.request(plain_fill, nullptr);
		expect(g_runs).to_equal(2);
	});

	it("releases the guard when the owner drops the dispatch (no permanent wedge)", _ {
		Coalescer c;
		g_runs = 0;
		g_mock_worker_drop = true;
		c.request(plain_fill, nullptr); // dispatch dropped → fill never runs
		expect(g_runs).to_equal(0);
		g_mock_worker_drop = false;
		c.request(plain_fill, nullptr); // guard was released, so this one runs
		expect(g_runs).to_equal(1);
	});

	it("sd-routine flavor runs the fill (inline on host)", _ {
		Coalescer c{/*sd_routine=*/true};
		g_runs = 0;
		c.request(plain_fill, nullptr);
		expect(g_runs).to_equal(1);
	});

	it("sd-routine flavor releases the guard when the owner drops the dispatch", _ {
		Coalescer c{/*sd_routine=*/true};
		g_runs = 0;
		g_mock_worker_drop = true;
		c.request(plain_fill, nullptr); // dispatch dropped → fill never runs
		expect(g_runs).to_equal(0);
		g_mock_worker_drop = false;
		c.request(plain_fill, nullptr); // guard released, so this one runs
		expect(g_runs).to_equal(1);
	});
});
// clang-format on

CPPSPEC_SPEC(coalescer)
