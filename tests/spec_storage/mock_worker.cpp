// tests/spec_storage/mock_worker.cpp
//
// Host-only stand-in for include/libdeluge/worker.h's deluge_worker_run. The real
// definitions are the scheduler's cooperative default (task_scheduler_c_api.cpp,
// runs fn(ctx) inline) or the Embassy BSP's stackful-fiber dispatch (src/bsp/rust);
// neither is appropriate to drag into a host unit spec. This mirrors the cooperative
// default's inline semantics without pulling in the scheduler, matching the
// mock_file_io.cpp / mock_source.h pattern already used by the other spec dirs.
#include "libdeluge/worker.h"

// Test hook: when true, `deluge_worker_run` drops the op (does not run it) and returns
// false, modelling the Embassy worker queue being full. Lets specs exercise the
// Coalescer's drop-recovery (that it releases its single-flight guard on a dropped
// dispatch instead of wedging). Reset to false between tests that use it.
bool g_mock_worker_drop = false;

extern "C" bool deluge_worker_run(void (*fn)(void*), void* ctx) {
	if (g_mock_worker_drop) {
		return false; // dropped — the op does not run
	}
	fn(ctx);
	return true;
}
