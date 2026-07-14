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
		DelugeFile* f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &f)).to_equal(DELUGE_OK);
		const char* msg = "hello";
		uint32_t written = 0;
		expect(deluge_file_write(f, msg, 5, &written)).to_equal(DELUGE_OK);
		expect(written).to_equal((uint32_t)5);
		expect(deluge_file_close(f)).to_equal(DELUGE_OK);

		f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_OK);
		char buf[8] = {};
		uint32_t read = 0;
		expect(deluge_file_read(f, buf, 8, &read)).to_equal(DELUGE_OK);
		expect(read).to_equal((uint32_t)5);
		expect(std::string(buf, 5)).to_equal("hello");
		expect(deluge_file_close(f)).to_equal(DELUGE_OK);
	});

	it("returns DELUGE_ERR_NOT_FOUND opening a missing file for read", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		expect(deluge_file_open("missing.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
	});

	it("mkdir then mkdir again returns DELUGE_ERR_EXISTS", _ {
		mock_file_io_reset();
		expect(deluge_file_mkdir("SONGS")).to_equal(DELUGE_OK);
		expect(deluge_file_mkdir("SONGS")).to_equal(DELUGE_ERR_EXISTS);
	});

	it("unlink removes an entry", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		deluge_file_open("a.txt", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		expect(deluge_file_unlink("a.txt")).to_equal(DELUGE_OK);
		f = nullptr;
		expect(deluge_file_open("a.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
	});

	it("rename moves an entry to a new path", _ {
		mock_file_io_reset();
		DelugeFile* f = nullptr;
		deluge_file_open("old.txt", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		expect(deluge_file_rename("old.txt", "new.txt")).to_equal(DELUGE_OK);
		f = nullptr;
		expect(deluge_file_open("old.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_ERR_NOT_FOUND);
		f = nullptr;
		expect(deluge_file_open("new.txt", DELUGE_FILE_READ, &f)).to_equal(DELUGE_OK);
		deluge_file_close(f);
	});

	it("lists direct children of a directory, distinguishing files from subdirectories", _ {
		mock_file_io_reset();
		deluge_file_mkdir("SONGS");
		DelugeFile* f = nullptr;
		deluge_file_open("SONGS/a.xml", DELUGE_FILE_WRITE_CREATE, &f);
		deluge_file_close(f);
		deluge_file_mkdir("SONGS/sub");

		DelugeDir* d = nullptr;
		expect(deluge_dir_open("SONGS", &d)).to_equal(DELUGE_OK);
		bool saw_file = false, saw_dir = false;
		DelugeDirEntry entry{};
		bool has_entry = true;
		while (has_entry) {
			expect(deluge_dir_read(d, &entry, &has_entry)).to_equal(DELUGE_OK);
			if (!has_entry) break;
			if (std::string(entry.name) == "a.xml") { saw_file = true; expect(entry.is_directory).to_equal(false); }
			if (std::string(entry.name) == "sub") { saw_dir = true; expect(entry.is_directory).to_equal(true); }
		}
		expect(saw_file).to_equal(true);
		expect(saw_dir).to_equal(true);
		expect(deluge_dir_close(d)).to_equal(DELUGE_OK);
	});
});

CPPSPEC_SPEC(mock_file_io)
