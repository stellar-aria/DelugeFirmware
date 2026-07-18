// tests/spec_sync/sd_access_spec.cpp
#include "sync/sd_access.h"

#include "cppspec.hpp"

#include <cstdint>

// Provide the definition the query reads (in firmware it lives in the diskio layer).
extern "C" uint8_t currentlyAccessingCard = 0;

// clang-format off
describe sd_access("deluge::sync::sd_busy", $ {
	it("is false when currentlyAccessingCard is 0", _ {
		currentlyAccessingCard = 0;
		expect(deluge::sync::sd_busy()).to_be_falsy();
	});

	it("is true when currentlyAccessingCard is non-zero", _ {
		currentlyAccessingCard = 1;
		expect(deluge::sync::sd_busy()).to_be_truthy();
	});
});

CPPSPEC_SPEC(sd_access)
