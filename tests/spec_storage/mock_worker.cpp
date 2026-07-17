// tests/spec_storage/mock_worker.cpp
//
// Host-only stand-in for include/libdeluge/worker.h's deluge_worker_run. The real
// definitions are the scheduler's cooperative default (task_scheduler_c_api.cpp,
// runs fn(ctx) inline) or the Embassy BSP's stackful-fiber dispatch (src/bsp/rust);
// neither is appropriate to drag into a host unit spec. This mirrors the cooperative
// default's inline semantics without pulling in the scheduler, matching the
// mock_file_io.cpp / mock_source.h pattern already used by the other spec dirs.
#include "libdeluge/worker.h"

extern "C" void deluge_worker_run(void (*fn)(void*), void* ctx) {
	fn(ctx);
}
