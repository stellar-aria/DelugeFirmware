// tests/spec_sync/storage_op_spec.cpp
#include "sync/storage_op.h"

#include "cppspec.hpp"

// The global the module owns; declared in extern.h, defined in storage_op.cpp.
extern bool allowSomeUserActionsEvenWhenInCardRoutine;

// clang-format off
describe storage_op("deluge::sync::StorageOp", $ {
	before_each([] { allowSomeUserActionsEvenWhenInCardRoutine = false; });

	it("permits user actions while alive", _ {
		deluge::sync::StorageOp op;
		expect(allowSomeUserActionsEvenWhenInCardRoutine).to_be_truthy();
	});

	it("restores the prior state on destruction", _ {
		{ deluge::sync::StorageOp op; }
		expect(allowSomeUserActionsEvenWhenInCardRoutine).to_be_falsy();
	});

	it("nests without clobbering (restores to the outer scope's value)", _ {
		deluge::sync::StorageOp outer;
		{ deluge::sync::StorageOp inner; }
		// Inner's destructor must restore to true (outer still active), not false.
		expect(allowSomeUserActionsEvenWhenInCardRoutine).to_be_truthy();
	});
});

CPPSPEC_SPEC(storage_op)
