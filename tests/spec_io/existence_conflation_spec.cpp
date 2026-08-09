// tests/spec_io/existence_conflation_spec.cpp
#include "mock_file_io.h"
#include "storage/storage_manager.h"

extern "C" {
#include "libdeluge/file_io.h"
}

#include "cppspec.hpp"

// clang-format off
describe existence_conflation("existence checks distinguish absent from undeterminable", $ {

	// Seed a real file, then make the filesystem REFUSE to answer for it.
	it("does not report a refused check as absent", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);
		mock_file_io_inject_status("SETTINGS/MIDIFollow.XML", DELUGE_ERR_BUSY);

		auto present = StorageManager::fileExists("SETTINGS/MIDIFollow.XML");

		expect(present.has_value()).to_equal(false);
		expect(present.error() == deluge::io::Status::BUSY).to_equal(true);
	});

	it("still reports a genuine absence as absent", _ {
		mock_file_io_reset();
		auto present = StorageManager::fileExists("SETTINGS/NoSuchFile.XML");
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(false);
	});

	it("reports an existing file as present", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);

		auto present = StorageManager::fileExists("SETTINGS/MIDIFollow.XML");
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(true);
	});
});

CPPSPEC_SPEC(existence_conflation)
