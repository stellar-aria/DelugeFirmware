// RAM-backed diskio shim for the FatFS reentrancy stress harness.
//
// FatFS's own volume grant (ff_req_grant/ff_rel_grant, see ff_sync.cpp)
// serializes every call into this file, so there is no locking here -- TSan
// running over the multi-threaded workload (Task 2) is what confirms that
// serialization actually holds.
#include "diskio.h"

#include <cstring>
#include <vector>

namespace {

constexpr unsigned kSectorSize = 512;
constexpr unsigned kSectorCount = 16384; // 16384 * 512 = 8 MiB

std::vector<uint8_t> g_disk(static_cast<size_t>(kSectorCount) * kSectorSize);

} // namespace

extern "C" {

DSTATUS disk_status(BYTE pdrv) {
	(void)pdrv;
	return 0;
}

DSTATUS disk_initialize(BYTE pdrv) {
	(void)pdrv;
	return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	std::memcpy(buff, g_disk.data() + static_cast<size_t>(sector) * kSectorSize,
	            static_cast<size_t>(count) * kSectorSize);
	return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	std::memcpy(g_disk.data() + static_cast<size_t>(sector) * kSectorSize, buff,
	            static_cast<size_t>(count) * kSectorSize);
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
	(void)pdrv;
	switch (cmd) {
	case CTRL_SYNC:
		return RES_OK;
	case GET_SECTOR_COUNT:
		*static_cast<LBA_t*>(buff) = kSectorCount;
		return RES_OK;
	case GET_SECTOR_SIZE:
		*static_cast<WORD*>(buff) = kSectorSize;
		return RES_OK;
	case GET_BLOCK_SIZE:
		*static_cast<DWORD*>(buff) = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}

} // extern "C"
