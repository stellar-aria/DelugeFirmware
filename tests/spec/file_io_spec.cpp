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

	it("maps DELUGE_FILE_WRITE_CREATE_NEW to FA_WRITE|FA_CREATE_NEW", _ {
		expect(deluge::fatfs_adapter::to_fatfs_mode(DELUGE_FILE_WRITE_CREATE_NEW))
		    .to_equal(static_cast<FileAccessMode>(FA_WRITE | FA_CREATE_NEW));
	});

	// StorageManager::createFile (src/deluge/storage/storage_manager.cpp) itself can't be exercised from this
	// target -- it pulls in most of the app (Song, FavouriteManager, InstrumentClipView, AudioFileManager,
	// SoundEditor, Display, UITimerManager, ...), and this test target's host build has no real mountable
	// filesystem to open against anyway (mock_diskio.cpp reports STA_NOINIT unconditionally -- see this file's
	// other tests for the same accepted gap). What we *can* check here, at this boundary, is the shape of
	// createFile's mode selection: `mayOverwrite ? DELUGE_FILE_WRITE_CREATE : DELUGE_FILE_WRITE_CREATE_NEW`,
	// verified end-to-end through to the FatFS flags it resolves to.
	it("createFile's mayOverwrite selects DELUGE_FILE_WRITE_CREATE vs DELUGE_FILE_WRITE_CREATE_NEW", _ {
		auto createFileMode = [](bool mayOverwrite) {
			return mayOverwrite ? DELUGE_FILE_WRITE_CREATE : DELUGE_FILE_WRITE_CREATE_NEW;
		};
		expect(deluge::fatfs_adapter::to_fatfs_mode(createFileMode(true)))
		    .to_equal(static_cast<FileAccessMode>(FA_WRITE | FA_CREATE_ALWAYS));
		expect(deluge::fatfs_adapter::to_fatfs_mode(createFileMode(false)))
		    .to_equal(static_cast<FileAccessMode>(FA_WRITE | FA_CREATE_NEW));
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

	it("open_by_locator constructs a File with exactly the given locator fields, matching openFilePointer's contract", _ {
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 42, 100, 5000);
		expect(file.inner().obj.fs).to_equal(&fakeFs);
		expect(file.inner().obj.id).to_equal(static_cast<WORD>(42));
		expect(file.inner().obj.sclust).to_equal(static_cast<DWORD>(100));
		expect(file.inner().obj.objsize).to_equal(static_cast<FSIZE_t>(5000));
		expect(file.inner().flag).to_equal(static_cast<BYTE>(FA_READ));
		expect(file.inner().err).to_equal(static_cast<BYTE>(0));
		expect(file.inner().sect).to_equal(static_cast<DWORD>(0));
		expect(file.inner().fptr).to_equal(static_cast<FSIZE_t>(0));
		expect(file.size()).to_equal(5000u);
		expect(file.tell()).to_equal(0u);

		// fakeFs is zero-initialized (fs_type == 0), so FatFS's own validate()
		// safely rejects it as FR_INVALID_OBJECT on close -- confirms this
		// doesn't crash against an object that was never really f_open'd. This
		// test target has no real disk backing (see this file's scope note),
		// so genuine I/O was never on the table anyway -- this only verifies
		// the field construction itself.
		auto closed = file.close();
		expect(closed.has_value()).to_equal(false);
		expect(closed.error()).to_equal(FatFS::Error::INVALID_OBJECT);
	});

	it("dir_cache_begin+append populates entries findable by dir_cache_lookup", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG2.XML", &fakeFs, 7, 200, 3000);

		const auto* entry = deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG2.XML");
		expect(entry != nullptr).to_equal(true);
		expect(entry->fs).to_equal(&fakeFs);
		expect(entry->id).to_equal(static_cast<WORD>(7));
		expect(entry->sclust).to_equal(static_cast<DWORD>(200));
		expect(entry->objsize).to_equal(static_cast<FSIZE_t>(3000));
	});

	it("dir_cache_lookup misses on a directory that doesn't match", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SAMPLES/SONG1.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup misses on a filename that isn't cached", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG_MISSING.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_append no-ops if handle doesn't match the active scan", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handleA = reinterpret_cast<const void*>(0x1000);
		const void* handleB = reinterpret_cast<const void*>(0x2000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handleA);
		deluge::fatfs_adapter::dir_cache_append(handleB, "INTRUDER.XML", &fakeFs, 7, 999, 999);

		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/INTRUDER.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup returns nullptr when the cache was never populated", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		expect(deluge::fatfs_adapter::dir_cache_lookup("SONGS/SONG1.XML") == nullptr).to_equal(true);
	});

	it("dir_cache_lookup handles a root-level (no-slash) path", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "ROOT.XML", &fakeFs, 3, 50, 500);

		const auto* entry = deluge::fatfs_adapter::dir_cache_lookup("ROOT.XML");
		expect(entry != nullptr).to_equal(true);
		expect(entry->sclust).to_equal(static_cast<DWORD>(50));
	});

	it("dir_cache_append silently drops entries beyond kDirCacheCapacity", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("BIGDIR", handle);
		char name[16];
		for (size_t i = 0; i < deluge::fatfs_adapter::kDirCacheCapacity + 5; i++) {
			std::snprintf(name, sizeof(name), "F%zu.WAV", i);
			deluge::fatfs_adapter::dir_cache_append(handle, name, &fakeFs, 1, static_cast<DWORD>(i + 100), 10);
		}
		expect(deluge::fatfs_adapter::g_dir_cache.entry_count).to_equal(deluge::fatfs_adapter::kDirCacheCapacity);

		// The first kDirCacheCapacity entries are still found; nothing crashed on overflow.
		const auto* first = deluge::fatfs_adapter::dir_cache_lookup("BIGDIR/F0.WAV");
		expect(first != nullptr).to_equal(true);
	});

	it("deluge_file_open takes the fast path on a cache hit", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		size_t hits_before = deluge::fatfs_adapter::g_dir_cache_hits;

		DelugeFile* file = nullptr;
		DelugeStatus status = deluge_file_open("SONGS/SONG1.XML", DELUGE_FILE_READ, &file);
		expect(status).to_equal(DELUGE_OK);
		expect(deluge::fatfs_adapter::g_dir_cache_hits).to_equal(hits_before + 1);

		uint32_t size = 0;
		expect(deluge_file_size(file, &size)).to_equal(DELUGE_OK);
		expect(size).to_equal(2000u);

		// fakeFs isn't really mounted, so closing correctly reports an error
		// rather than crashing -- same reasoning as Task 1's open_by_locator test.
		DelugeStatus closeStatus = deluge_file_close(file);
		expect(closeStatus).to_equal(DELUGE_ERR_IO);
	});

	it("deluge_file_open falls through to the normal path on a cache miss", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		size_t misses_before = deluge::fatfs_adapter::g_dir_cache_misses;

		DelugeFile* file = nullptr;
		// No real mounted disk in this test target, so this will fail --
		// what matters is that it took the miss path (counter incremented)
		// and returned a defined error rather than crashing.
		DelugeStatus status = deluge_file_open("NOWHERE/MISSING.XML", DELUGE_FILE_READ, &file);
		expect(status != DELUGE_OK).to_equal(true);
		expect(deluge::fatfs_adapter::g_dir_cache_misses).to_equal(misses_before + 1);
	});

	it("a write-create open invalidates the cache", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);
		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		deluge::fatfs_adapter::dir_cache_append(handle, "SONG1.XML", &fakeFs, 7, 100, 2000);
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(true);

		DelugeFile* file = nullptr;
		(void)deluge_file_open("SONGS/NEWFILE.XML", DELUGE_FILE_WRITE_CREATE, &file);
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);
	});

	it("mkdir/unlink/rename each invalidate the cache", _ {
		deluge::fatfs_adapter::dir_cache_reset_for_test();
		FATFS fakeFs{};
		const void* handle = reinterpret_cast<const void*>(0x1000);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_mkdir("SONGS/NEWDIR");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_unlink("SONGS/SONG1.XML");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);

		deluge::fatfs_adapter::dir_cache_begin("SONGS", handle);
		(void)deluge_file_rename("SONGS/SONG1.XML", "SONGS/SONG2.XML");
		expect(deluge::fatfs_adapter::g_dir_cache.valid).to_equal(false);
	});
});

CPPSPEC_SPEC(file_io)
