// tests/spec_storage/latest_wins_spec.cpp
#include "storage/latest_wins.h"

#include "cppspec.hpp"

using deluge::storage::LatestWins;

// clang-format off
describe latest_wins("deluge::storage::LatestWins", $ {
	it("dispatches immediately when idle", _ {
		LatestWins<int> lw;
		expect(lw.request(1)).to_be_true();   // idle → dispatch now
		expect(lw.in_flight()).to_be_true();
		expect(lw.current()).to_equal(1);
	});

	it("coalesces a request made while in flight (does not dispatch)", _ {
		LatestWins<int> lw;
		lw.request(1);
		expect(lw.request(2)).to_be_false();  // in flight → queued, no dispatch
	});

	it("re-dispatches the LATEST queued target on completion", _ {
		LatestWins<int> lw;
		lw.request(1);          // dispatch 1
		lw.request(2);          // queued
		lw.request(3);          // queued (supersedes 2)
		auto next = lw.complete();
		expect(next.has_value()).to_be_true();
		expect(next.value()).to_equal(3);     // latest wins, not 2
		expect(lw.current()).to_equal(3);
		expect(lw.in_flight()).to_be_true();  // still in flight for the re-dispatch
	});

	it("goes idle on completion when nothing was queued", _ {
		LatestWins<int> lw;
		lw.request(1);
		auto next = lw.complete();
		expect(next.has_value()).to_be_false();
		expect(lw.in_flight()).to_be_false();
	});

	it("converges: after the re-dispatch completes with nothing new, it idles", _ {
		LatestWins<int> lw;
		lw.request(1);
		lw.request(2);          // queued
		lw.complete();          // re-dispatch 2
		auto next = lw.complete();
		expect(next.has_value()).to_be_false();
		expect(lw.in_flight()).to_be_false();
	});
});
// clang-format on

CPPSPEC_SPEC(latest_wins)
