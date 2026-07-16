// FatFS reentrancy sync stubs for the host stress harness (replaces
// src/fatfs/ffsystem.c's FF_FS_REENTRANT block, which is Win32-only sample
// code). Backed by std::mutex: the host proxy for the Embassy volume mutex
// that will guard the real firmware's FF_FS_REENTRANT=1 build. TSan
// understands std::mutex as a happens-before edge, so under the concurrent
// workload (Task 2) it only flags accesses this grant fails to actually
// cover -- the property under test.
#include "ff.h"

#include <mutex>

// The vendored ff.c (create_chain(), src/fatfs/ff.c:1507) reaches out to this
// app-level MIDI-thru-during-cluster-write counter (real definition:
// src/deluge/playback/playback_handler.cpp). Nothing in this harness drives
// cluster writes through that path; a plain zero-initialized definition is
// enough to satisfy the linker (mirrors tests/32bit_unit_tests/mocks/mock_diskio.cpp).
int pendingGlobalMIDICommandNumClustersWritten = 0;

extern "C" {

int ff_cre_syncobj(BYTE /*vol*/, FF_SYNC_t* sobj) {
	*sobj = new std::mutex();
	return 1;
}

int ff_del_syncobj(FF_SYNC_t sobj) {
	delete static_cast<std::mutex*>(sobj);
	return 1;
}

int ff_req_grant(FF_SYNC_t sobj) {
	static_cast<std::mutex*>(sobj)->lock();
	return 1;
}

void ff_rel_grant(FF_SYNC_t sobj) {
	static_cast<std::mutex*>(sobj)->unlock();
}

// FF_FS_NORTC == 0 requires this. No RTC on host; fixed timestamp
// (2026-07-16 00:00:00) in FatFS packed form.
DWORD get_fattime(void) {
	return ((DWORD)(2026 - 1980) << 25) | ((DWORD)7 << 21) | ((DWORD)16 << 16);
}

} // extern "C"
