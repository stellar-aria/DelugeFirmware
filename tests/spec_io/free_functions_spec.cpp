#include "io/file.hpp"
#include "mock_file_io.h"

#include "cppspec.hpp"

// clang-format off
describe free_functions("deluge::io free functions", $ {
	it("mkdir succeeds, then reports EXISTS on a repeat", _ {
		mock_file_io_reset();
		expect(deluge::io::mkdir("SONGS")).to_have_value();
		auto again = deluge::io::mkdir("SONGS");
		expect(again.has_value()).to_equal(false);
		expect(again.error()).to_equal(deluge::io::Status::EXISTS);
	});

	it("unlink removes a file", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		expect(f.has_value()).to_equal(true);
		expect(f->close()).to_have_value();
		expect(deluge::io::unlink("a.txt")).to_have_value();
		auto opened = deluge::io::File::open("a.txt", DELUGE_FILE_READ);
		expect(opened.has_value()).to_equal(false);
	});

	it("rename moves a file to a new path", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("old.txt", DELUGE_FILE_WRITE_CREATE);
		expect(f.has_value()).to_equal(true);
		expect(f->close()).to_have_value();
		expect(deluge::io::rename("old.txt", "new.txt")).to_have_value();
		auto old_opened = deluge::io::File::open("old.txt", DELUGE_FILE_READ);
		expect(old_opened.has_value()).to_equal(false);
		auto new_opened = deluge::io::File::open("new.txt", DELUGE_FILE_READ);
		expect(new_opened.has_value()).to_equal(true);
	});

	it("unlink on a missing path reports NOT_FOUND", _ {
		mock_file_io_reset();
		auto result = deluge::io::unlink("missing.txt");
		expect(result.has_value()).to_equal(false);
		expect(result.error()).to_equal(deluge::io::Status::NOT_FOUND);
	});

	it("set_time updates a file's stored modification time", _ {
		mock_file_io_reset();
		auto f = deluge::io::File::open("a.txt", DELUGE_FILE_WRITE_CREATE);
		expect(f.has_value()).to_equal(true);
		expect(f->close()).to_have_value();
		DelugeTimestamp ts{.year = 2026, .month = 7, .day = 14, .hour = 12, .minute = 0, .second = 0};
		expect(deluge::io::set_time("a.txt", ts)).to_have_value();
	});

	it("set_time on a missing path reports NOT_FOUND", _ {
		mock_file_io_reset();
		DelugeTimestamp ts{};
		auto result = deluge::io::set_time("missing.txt", ts);
		expect(result.has_value()).to_equal(false);
		expect(result.error()).to_equal(deluge::io::Status::NOT_FOUND);
	});
});

CPPSPEC_SPEC(free_functions)
