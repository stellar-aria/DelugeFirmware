// tests/spec_io/status_spec.cpp
#include "io/file.hpp"

#include "cppspec.hpp"

// clang-format off
describe status("deluge::io::Status", $ {
	it("maps DELUGE_OK to Status::OK", _ {
		expect(deluge::io::to_status(DELUGE_OK)).to_equal(deluge::io::Status::OK);
	});
	it("maps DELUGE_ERR_NOT_FOUND to Status::NOT_FOUND", _ {
		expect(deluge::io::to_status(DELUGE_ERR_NOT_FOUND)).to_equal(deluge::io::Status::NOT_FOUND);
	});
	it("maps DELUGE_ERR_EXISTS to Status::EXISTS", _ {
		expect(deluge::io::to_status(DELUGE_ERR_EXISTS)).to_equal(deluge::io::Status::EXISTS);
	});
	it("maps DELUGE_ERR_NO_MEMORY to Status::NO_MEMORY", _ {
		expect(deluge::io::to_status(DELUGE_ERR_NO_MEMORY)).to_equal(deluge::io::Status::NO_MEMORY);
	});
	it("maps DELUGE_ERR_WRITE_PROTECTED to Status::WRITE_PROTECTED", _ {
		expect(deluge::io::to_status(DELUGE_ERR_WRITE_PROTECTED)).to_equal(deluge::io::Status::WRITE_PROTECTED);
	});
});

CPPSPEC_SPEC(status)
