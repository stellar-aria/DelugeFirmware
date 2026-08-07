// tests/spec_io/mock_file_io_spec.cpp
#include "mock_file_io.h"

extern "C" {
#include "libdeluge/file_io.h"
}

#include "cppspec.hpp"

#include <cstring>

// clang-format off
describe mock_file_io("the file_io.h mock backing", $ {
	it("round-trips a write then read", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &h)).to_equal(true);
		const char* msg = "hello";
		uint32_t written = 0;
		expect(deluge_efatfs_file_write(h, msg, 5, &written)).to_equal(true);
		expect(written).to_equal((uint32_t)5);
		deluge_efatfs_file_close(h);

		h = 0;
		expect(deluge_efatfs_file_open("a.txt", DELUGE_FILE_READ, &h)).to_equal(true);
		char buf[8] = {};
		uint32_t read = 0;
		expect(deluge_efatfs_file_read_exact(h, buf, 8, &read)).to_equal(true);
		expect(read).to_equal((uint32_t)5);
		expect(std::string(buf, 5)).to_equal("hello");
		deluge_efatfs_file_close(h);
	});

	it("read fills the buffer, zero-padding a short tail", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		deluge_efatfs_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &h);
		const char* msg = "hi";
		uint32_t written = 0;
		deluge_efatfs_file_write(h, msg, 2, &written);
		deluge_efatfs_file_close(h);

		h = 0;
		expect(deluge_efatfs_file_open("a.txt", DELUGE_FILE_READ, &h)).to_equal(true);
		char buf[4] = {'x', 'x', 'x', 'x'};
		uint32_t read = 0;
		expect(deluge_efatfs_file_read(h, buf, 4, &read)).to_equal(true);
		expect(read).to_equal((uint32_t)4);
		expect(buf[0]).to_equal('h');
		expect(buf[1]).to_equal('i');
		expect(buf[2]).to_equal('\0');
		expect(buf[3]).to_equal('\0');
		deluge_efatfs_file_close(h);
	});

	it("returns false opening a missing file for read", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		expect(deluge_efatfs_file_open("missing.txt", DELUGE_FILE_READ, &h)).to_equal(false);
	});

	it("mkdir is idempotent -- a repeat succeeds", _ {
		// embedded-fatfs's create_dir is idempotent (unlike C-FatFS's f_mkdir,
		// which reports EXIST on a repeat).
		mock_file_io_reset();
		expect(deluge_efatfs_mkdir("SONGS")).to_equal(true);
		expect(deluge_efatfs_mkdir("SONGS")).to_equal(true);
	});

	it("mkdir on a path already occupied by a file fails", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		deluge_efatfs_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &h);
		deluge_efatfs_file_close(h);
		expect(deluge_efatfs_mkdir("a.txt")).to_equal(false);
	});

	it("unlink removes an entry", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		deluge_efatfs_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &h);
		deluge_efatfs_file_close(h);
		expect(deluge_efatfs_unlink("a.txt")).to_equal(true);
		h = 0;
		expect(deluge_efatfs_file_open("a.txt", DELUGE_FILE_READ, &h)).to_equal(false);
	});

	it("rename moves an entry to a new path", _ {
		mock_file_io_reset();
		uint32_t h = 0;
		deluge_efatfs_file_open("old.txt", DELUGE_FILE_WRITE_CREATE, &h);
		deluge_efatfs_file_close(h);
		expect(deluge_efatfs_rename("old.txt", "new.txt")).to_equal(true);
		h = 0;
		expect(deluge_efatfs_file_open("old.txt", DELUGE_FILE_READ, &h)).to_equal(false);
		h = 0;
		expect(deluge_efatfs_file_open("new.txt", DELUGE_FILE_READ, &h)).to_equal(true);
		deluge_efatfs_file_close(h);
	});

	it("lists direct children of a directory, distinguishing files from subdirectories", _ {
		mock_file_io_reset();
		deluge_efatfs_mkdir("SONGS");
		uint32_t h = 0;
		deluge_efatfs_file_open("SONGS/a.xml", DELUGE_FILE_WRITE_CREATE, &h);
		deluge_efatfs_file_close(h);
		deluge_efatfs_mkdir("SONGS/sub");

		uint32_t d = 0;
		expect(deluge_efatfs_dir_open("SONGS", &d)).to_equal(true);
		bool saw_file = false, saw_dir = false;
		bool has_entry = true;
		while (has_entry) {
			char name[DELUGE_MAX_FILENAME] = {};
			bool is_dir = false;
			uint32_t size = 0;
			uint32_t modified = 0;
			uint8_t attrs = 0;
			expect(deluge_efatfs_dir_read(d, name, DELUGE_MAX_FILENAME, &is_dir, &size, &modified, &attrs,
			                              &has_entry))
			    .to_equal(true);
			if (!has_entry) break;
			if (std::string(name) == "a.xml") { saw_file = true; expect(is_dir).to_equal(false); }
			if (std::string(name) == "sub") { saw_dir = true; expect(is_dir).to_equal(true); }
		}
		expect(saw_file).to_equal(true);
		expect(saw_dir).to_equal(true);
		deluge_efatfs_dir_close(d);
	});
});

CPPSPEC_SPEC(mock_file_io)
