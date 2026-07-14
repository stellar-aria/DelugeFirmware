// tests/spec/file_io_spec.cpp
#include "fatfs/file_io_internal.hpp"

#include "cppspec.hpp"

#include <cstring>

using FileAccessMode = deluge::fatfs_adapter::FileAccessMode;

// clang-format off
describe file_io("file_io adapter", $ {
	it("maps DELUGE_FILE_READ to FA_READ", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_READ)).to_equal(static_cast<FileAccessMode>(FA_READ));
	});

	it("maps DELUGE_FILE_WRITE_CREATE to FA_WRITE|FA_CREATE_ALWAYS", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_WRITE_CREATE))
		    .to_equal(static_cast<FileAccessMode>(FA_WRITE | FA_CREATE_ALWAYS));
	});

	it("maps every FatFS::Error to a non-generic DelugeStatus where one exists", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILE)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_PATH)).to_equal(DELUGE_ERR_NOT_FOUND);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::EXIST)).to_equal(DELUGE_ERR_EXISTS);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::WRITE_PROTECTED)).to_equal(DELUGE_ERR_WRITE_PROTECTED);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NO_FILESYSTEM)).to_equal(DELUGE_ERR_NO_FILESYSTEM);
	});

	it("maps NOT_ENOUGH_CORE to DELUGE_ERR_NO_MEMORY and DENIED to DELUGE_ERR_NO_SPACE", _ {
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::NOT_ENOUGH_CORE)).to_equal(DELUGE_ERR_NO_MEMORY);
		expect(deluge::fatfs_adapter::to_deluge_status(FatFS::Error::DENIED)).to_equal(DELUGE_ERR_NO_SPACE);
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

	it("packs a DelugeTimestamp into FAT-native date/time WORDs", _ {
		DelugeTimestamp ts{.year = 2026, .month = 7, .day = 14, .hour = 16, .minute = 45, .second = 30};
		WORD date = deluge::fatfs_adapter::to_fat_date(ts);
		WORD time = deluge::fatfs_adapter::to_fat_time(ts);
		expect(date).to_equal(static_cast<WORD>(((2026 - 1980) << 9) | (7 << 5) | 14));
		expect(time).to_equal(static_cast<WORD>((16 << 11) | (45 << 5) | (30 / 2)));
	});

	it("round-trips a FAT date/time pair through from_fat_date_time", _ {
		DelugeTimestamp original{.year = 2001, .month = 3, .day = 9, .hour = 8, .minute = 5, .second = 44};
		WORD date = deluge::fatfs_adapter::to_fat_date(original);
		WORD time = deluge::fatfs_adapter::to_fat_time(original);
		DelugeTimestamp round_tripped = deluge::fatfs_adapter::from_fat_date_time(date, time);
		expect(round_tripped.year).to_equal(original.year);
		expect(round_tripped.month).to_equal(original.month);
		expect(round_tripped.day).to_equal(original.day);
		expect(round_tripped.hour).to_equal(original.hour);
		expect(round_tripped.minute).to_equal(original.minute);
		expect(round_tripped.second).to_equal(44u); // 44 is even: survives the 2s-resolution rounding exactly
	});

	it("rounds odd seconds down to FAT's 2-second resolution", _ {
		DelugeTimestamp original{.year = 2020, .month = 1, .day = 1, .hour = 0, .minute = 0, .second = 45};
		WORD time = deluge::fatfs_adapter::to_fat_time(original);
		DelugeTimestamp round_tripped = deluge::fatfs_adapter::from_fat_date_time(0, time);
		expect(round_tripped.second).to_equal(44u);
	});

	it("converts a FILINFO's size, timestamp, and attribute bits into a DelugeDirEntry", _ {
		FatFS::FileInfo info{};
		std::strcpy(info.fname, "REC001.WAV");
		info.fsize = 123456;
		info.fdate = deluge::fatfs_adapter::to_fat_date({.year = 2024, .month = 12, .day = 25, .hour = 0, .minute = 0, .second = 0});
		info.ftime = deluge::fatfs_adapter::to_fat_time({.year = 2024, .month = 12, .day = 25, .hour = 9, .minute = 30, .second = 0});
		info.fattrib = AM_RDO | AM_ARC;
		DelugeDirEntry out{};
		bool has_entry = false;
		deluge::fatfs_adapter::to_dir_entry(info, out, has_entry);
		expect(out.size).to_equal(123456u);
		expect(out.modified_time.year).to_equal(2024);
		expect(out.modified_time.hour).to_equal(9);
		expect(out.is_read_only).to_equal(true);
		expect(out.is_archive).to_equal(true);
		expect(out.is_hidden).to_equal(false);
		expect(out.is_system).to_equal(false);
	});
});

CPPSPEC_SPEC(file_io)
