// tests/spec/file_io_spec.cpp
#include "fatfs/file_io_internal.hpp"

#include "cppspec.hpp"

#include <cstring>

using FileAccessMode = deluge::fatfs_adapter::FileAccessMode;

// clang-format off
describe file_io("file_io adapter", $ {
	it("maps DELUGE_FILE_READ to FA_READ", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_READ)).to_equal((FileAccessMode)FA_READ);
	});

	it("maps DELUGE_FILE_WRITE_CREATE to FA_WRITE|FA_CREATE_ALWAYS", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_WRITE_CREATE))
		    .to_equal((FileAccessMode)(FA_WRITE | FA_CREATE_ALWAYS));
	});

	it("maps every FatFS::Error to a non-generic DelugeStatus where one exists", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILE)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_PATH)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::EXIST)).to_equal(DELUGE_ERR_EXISTS);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::WRITE_PROTECTED)).to_equal(DELUGE_ERR_WRITE_PROTECTED);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILESYSTEM)).to_equal(DELUGE_ERR_NO_FILESYSTEM);
	});

	it("maps unrecognized FatFS::Error values to the generic DELUGE_ERR_IO", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::INT_ERR)).to_equal(DELUGE_ERR_IO);
	});

	it("converts a plain file's FILINFO into a DelugeDirEntry", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "SONG1.XML");
		info.fattrib = 0;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(has_entry).to_equal(true);
		expect(std::string(out.name)).to_equal("SONG1.XML");
		expect(out.is_directory).to_equal(false);
	});

	it("marks directories via the AM_DIR attribute bit", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "SONGS");
		info.fattrib = AM_DIR;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(out.is_directory).to_equal(true);
	});

	it("signals end-of-directory when fname is empty, without an error", _ {
		FatFS::FileInfo info{};
		info.fname[0] = 0;
		DelugeDirEntry out{};
		bool has_entry = true;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(has_entry).to_equal(false);
	});
});

CPPSPEC_SPEC(file_io)
