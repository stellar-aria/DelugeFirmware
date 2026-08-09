// tests/spec_io/existence_conflation_spec.cpp
#include "io/file.hpp"
#include "mock_file_io.h"
#include "storage/existence_policy.h"

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

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));

		expect(present.has_value()).to_equal(false);
		expect(present.error() == deluge::io::Status::BUSY).to_equal(true);
	});

	it("still reports a genuine absence as absent", _ {
		mock_file_io_reset();
		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/NoSuchFile.XML", DELUGE_FILE_READ));
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(false);
	});

	it("reports an existing file as present", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(true);
	});

	// A non-BUSY, non-NOT_FOUND failure must also surface as an error, not absence --
	// the rule is "NOT_FOUND is the only path to false", not "anything unusual is false".
	it("does not report a non-BUSY I/O failure as absent either", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);
		mock_file_io_inject_status("SETTINGS/MIDIFollow.XML", DELUGE_ERR_IO);

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));

		expect(present.has_value()).to_equal(false);
		expect(present.error() == deluge::io::Status::IO).to_equal(true);
	});

	// deluge::storage::existence_policy layers a named three-state answer, and the one
	// bootstrap rule, on top of the same fileExists()-shaped result -- exercised through the
	// real mock/File::open chain exactly like the cases above, not hand-built expecteds.
	it("classifies a refused check on an existing file as Undeterminable / UseInMemoryOnly", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);
		mock_file_io_inject_status("SETTINGS/MIDIFollow.XML", DELUGE_ERR_BUSY);

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));

		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Undeterminable)
		    .to_equal(true);
		expect(deluge::storage::bootstrap_action(present) == deluge::storage::Bootstrap::UseInMemoryOnly)
		    .to_equal(true);
	});

	it("classifies a non-BUSY refusal on an existing file as Undeterminable / UseInMemoryOnly", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);
		mock_file_io_inject_status("SETTINGS/MIDIFollow.XML", DELUGE_ERR_IO);

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));

		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Undeterminable)
		    .to_equal(true);
		expect(deluge::storage::bootstrap_action(present) == deluge::storage::Bootstrap::UseInMemoryOnly)
		    .to_equal(true);
	});

	it("classifies a genuinely missing path as Absent / WriteDefaults", _ {
		mock_file_io_reset();
		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/NoSuchFile.XML", DELUGE_FILE_READ));

		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Absent).to_equal(true);
		expect(deluge::storage::bootstrap_action(present) == deluge::storage::Bootstrap::WriteDefaults)
		    .to_equal(true);
	});

	it("classifies an existing path as Present / Load", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_WRITE_CREATE, &h))
		    .to_equal(DELUGE_OK);
		deluge_efatfs_file_close(h);

		auto present = deluge::io::presence_from_open(
		    deluge::io::File::open("SETTINGS/MIDIFollow.XML", DELUGE_FILE_READ));

		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Present).to_equal(true);
		expect(deluge::storage::bootstrap_action(present) == deluge::storage::Bootstrap::Load).to_equal(true);
	});

	// deluge::io::presence_from_open's Directory overload -- same rules as the File overload,
	// exercised through Directory::open/deluge_efatfs_dir_open/deluge_efatfs_mkdir instead.
	it("does not report a refused directory check as absent", _ {
		mock_file_io_reset();
		expect(deluge_efatfs_mkdir("SONGS")).to_equal(DELUGE_OK);
		mock_file_io_inject_status("SONGS", DELUGE_ERR_BUSY);

		auto present = deluge::io::presence_from_open(deluge::io::Directory::open("SONGS"));

		expect(present.has_value()).to_equal(false);
		expect(present.error() == deluge::io::Status::BUSY).to_equal(true);
		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Undeterminable)
		    .to_equal(true);
	});

	it("still reports a genuinely missing directory as absent", _ {
		mock_file_io_reset();
		auto present = deluge::io::presence_from_open(deluge::io::Directory::open("NoSuchDir"));
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(false);
		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Absent).to_equal(true);
	});

	it("reports an existing directory as present", _ {
		mock_file_io_reset();
		expect(deluge_efatfs_mkdir("SONGS")).to_equal(DELUGE_OK);

		auto present = deluge::io::presence_from_open(deluge::io::Directory::open("SONGS"));
		expect(present.has_value()).to_equal(true);
		expect(*present).to_equal(true);
		expect(deluge::storage::presence_of(present) == deluge::storage::Presence::Present).to_equal(true);
	});
});

CPPSPEC_SPEC(existence_conflation)
