#include "mock_streaming_fill.h"

#include "libdeluge/system.h" // ENTER_CRITICAL_SECTION/EXIT_CRITICAL_SECTION/deluge_in_interrupt

// Trivial, never-actually-exercised bodies -- see mock_streaming_fill.h's own doc for why the link
// needs them at all even though this spec's one scenario (an asset with no registered
// fill-context) never reaches any of them at runtime.

uint32_t deluge_streaming_define_asset(Sample* /*sample*/) {
	return 0;
}

// deluge_resource's masking discipline (crates/deluge_resource/src/sync.rs) calls these three
// through the C ABI on every acquire/release; the app's BSP normally supplies the real
// mask-interrupts bodies (unavailable to this host test, same as the streaming_fill.h accessors
// above). This spec is single-threaded and never runs in ISR context, so no-op/false bodies are
// exact (not just adequate) -- mirrors tests/unit/mocks/hal_mocks.cpp's own deluge_in_interrupt.
extern "C" {
void ENTER_CRITICAL_SECTION() {
}
void EXIT_CRITICAL_SECTION() {
}
bool deluge_in_interrupt() {
	return false;
}
}

namespace {
DelugeResource* g_active_manager = nullptr;
}

void set_active_manager(DelugeResource* handle) {
	g_active_manager = handle;
}

DelugeResource* deluge_streaming_resource_manager() {
	return g_active_manager;
}

uint8_t* deluge_streaming_chunk_payload(void* chunk_backing) {
	return static_cast<uint8_t*>(chunk_backing);
}

void deluge_streaming_chunk_set_loaded(void* /*chunk_backing*/) {
}

DelugeChunkConvertState deluge_streaming_chunk_convert_state(void* /*chunk_backing*/) {
	return {};
}

void deluge_streaming_chunk_set_convert_state(void* /*chunk_backing*/, DelugeChunkConvertState /*state*/) {
}

bool deluge_efatfs_read_at(uint32_t /*handle*/, uint32_t /*byte_offset*/, void* /*dst*/, uint32_t /*count*/,
                           uint32_t* /*out_read*/) {
	return false;
}
