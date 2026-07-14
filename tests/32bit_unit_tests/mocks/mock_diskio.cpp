// Link-only diskio backend for the host CppSpec build. `deluge_spec` compiles the
// real FatFS engine (ff.c/ffsystem.c/ffunicode.c/fatfs.cpp) so file_io.cpp's
// deluge_file_*/deluge_dir_* functions can call the real FatFS::File/Directory
// implementations, but no spec exercises actual file I/O (no mountable image is
// synthesized here — see file_io_spec.cpp's scope note). These stubs exist purely
// to satisfy the linker; every entry point reports "no disk", matching FatFS's own
// documented behaviour for a drive that never initializes.
#include "diskio.h"

// The vendored ff.c (create_chain(), src/fatfs/ff.c:1507) reaches out to this
// app-level MIDI-thru-during-cluster-write counter (real definition:
// src/deluge/playback/playback_handler.cpp). No spec drives cluster writes, so
// a plain zero-initialized definition is enough to satisfy the linker.
int pendingGlobalMIDICommandNumClustersWritten = 0;

DSTATUS disk_initialize(BYTE pdrv) {
	(void)pdrv;
	return STA_NODISK | STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv) {
	(void)pdrv;
	return STA_NODISK | STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	(void)buff;
	(void)sector;
	(void)count;
	return RES_NOTRDY;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
	(void)pdrv;
	(void)buff;
	(void)sector;
	(void)count;
	return RES_NOTRDY;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
	(void)pdrv;
	(void)cmd;
	(void)buff;
	return RES_NOTRDY;
}

// FF_FS_NORTC == 0 in ffconf.h requires this. No RTC on host; fixed timestamp
// (2024-01-01 00:00:00) in FatFS packed form, matching src/bsp/host/host_platform.c.
DWORD get_fattime(void) {
	return ((DWORD)(2024 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
