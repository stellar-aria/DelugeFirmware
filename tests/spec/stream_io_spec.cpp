// tests/spec/stream_io_spec.cpp
#include "fatfs/stream_io_internal.hpp"

#include "cppspec.hpp"

// clang-format off
describe stream_io("stream_io adapter", $ {
	it("returns DELUGE_ERR_UNSUPPORTED for DELUGE_STREAM_WRITE_CREATE (not yet implemented)", _ {
		DelugeStream* stream = nullptr;
		DelugeStatus status = deluge_stream_open("SAMPLES/TEST.WAV", DELUGE_STREAM_WRITE_CREATE, &stream);
		expect(status).to_equal(DELUGE_ERR_UNSUPPORTED);
	});

	it("returns DELUGE_ERR_UNSUPPORTED for DELUGE_STREAM_WRITE_CREATE_NEW (not yet implemented)", _ {
		DelugeStream* stream = nullptr;
		DelugeStatus status = deluge_stream_open("SAMPLES/TEST.WAV", DELUGE_STREAM_WRITE_CREATE_NEW, &stream);
		expect(status).to_equal(DELUGE_ERR_UNSUPPORTED);
	});

	it("returns DELUGE_ERR_UNSUPPORTED for DELUGE_STREAM_WRITE_APPEND (not yet implemented)", _ {
		DelugeStream* stream = nullptr;
		DelugeStatus status = deluge_stream_open("SAMPLES/TEST.WAV", DELUGE_STREAM_WRITE_APPEND, &stream);
		expect(status).to_equal(DELUGE_ERR_UNSUPPORTED);
	});

	it("resolve_read_layout on a zero-size file needs no FAT walk", _ {
		// open_by_locator is pure field construction (no I/O), same precedent as file_io_spec.cpp's
		// "open_by_locator constructs a File with exactly the given locator fields" test -- safe to call
		// against an unmounted fake FATFS because objsize=0 makes resolve_read_layout return before it
		// ever touches fs->csize or walks the FAT chain.
		FATFS fakeFs{};
		fakeFs.csize = 8; // must be non-zero, but resolve_read_layout must not read it for a 0-size file
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/0);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};

		DelugeStatus status = deluge::fatfs_adapter::resolve_read_layout(impl);
		expect(status).to_equal(DELUGE_OK);
		expect(impl.num_clusters).to_equal(0u);
		expect(impl.layout == nullptr).to_equal(true);
	});

	it("read_at rejects a non-cluster-aligned byte_offset without touching the block device", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/512);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.cluster_size_bytes = 512;
		impl.num_clusters = 1;
		impl.file_size = 512;
		impl.layout = layout;

		uint8_t dst[64];
		uint32_t out_read = 999;
		DelugeStatus status = deluge_stream_read_at(reinterpret_cast<DelugeStream*>(&impl), 7, dst, 64, &out_read);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		expect(out_read).to_equal(0u);
		impl.layout = nullptr; // don't let ~StreamImpl (none defined, but file.close() runs) touch the stack array
	});

	it("read_at rejects a count larger than one cluster", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/1024);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.cluster_size_bytes = 512;
		impl.num_clusters = 2;
		impl.file_size = 1024;
		impl.layout = layout;

		uint8_t dst[600];
		uint32_t out_read = 999;
		DelugeStatus status = deluge_stream_read_at(reinterpret_cast<DelugeStream*>(&impl), 0, dst, 600, &out_read);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		impl.layout = nullptr;
	});

	it("read_at accepts a sector-rounded count that exceeds file_size but stays within the cluster's "
	   "physical allocation", _ {
		// file_size (400) is not a multiple of cluster_size_bytes (512): the last cluster is padded
		// out to the cluster boundary on disk, same as e.g. Cymbal 2.wav's last cluster (862874-byte
		// file, 11264-byte sector-rounded read request). A cluster-aligned, in-range read of the full
		// cluster (512 bytes) must not be rejected just because it's more than the logical file_size.
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/400);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.cluster_size_bytes = 512;
		impl.num_clusters = 1;
		impl.file_size = 400;
		impl.layout = layout;

		uint8_t dst[512];
		uint32_t out_read = 999;
		DelugeStatus status = deluge_stream_read_at(reinterpret_cast<DelugeStream*>(&impl), 0, dst, 512, &out_read);
		// This spec build links mocks/mock_diskio.cpp, whose deluge_block_read always reports "no disk"
		// (DELUGE_ERR_NODEV) -- there's no mountable image here (see file_io_spec.cpp's scope note), so
		// the read itself can never actually complete. The bug under test is in *validation*, before the
		// block device is ever touched: DELUGE_ERR_NODEV (not DELUGE_ERR_PARAM) is proof the request
		// passed validation and reached deluge_block_read.
		expect(status).to_equal(DELUGE_ERR_NODEV);
		expect(out_read).to_equal(0u);
		impl.layout = nullptr;
	});

	it("sector_of returns DELUGE_ERR_PARAM for an out-of-range cluster index", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[1] = {{.sector = 1000}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/512);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.num_clusters = 1;
		impl.layout = layout;

		uint32_t sector = 0;
		DelugeStatus status = deluge_stream_sector_of(reinterpret_cast<DelugeStream*>(&impl), 5, &sector);
		expect(status).to_equal(DELUGE_ERR_PARAM);
		impl.layout = nullptr;
	});

	it("sector_of returns the resolved sector for a valid cluster index", _ {
		deluge::fatfs_adapter::StreamLayoutEntry layout[2] = {{.sector = 1000}, {.sector = 1008}};
		FATFS fakeFs{};
		FatFS::File file = FatFS::File::open_by_locator(&fakeFs, 1, 100, /*objsize=*/1024);
		deluge::fatfs_adapter::StreamImpl impl{std::move(file), DELUGE_STREAM_READ};
		impl.num_clusters = 2;
		impl.layout = layout;

		uint32_t sector = 0;
		DelugeStatus status = deluge_stream_sector_of(reinterpret_cast<DelugeStream*>(&impl), 1, &sector);
		expect(status).to_equal(DELUGE_OK);
		expect(sector).to_equal(1008u);
		impl.layout = nullptr;
	});
});

CPPSPEC_SPEC(stream_io)
