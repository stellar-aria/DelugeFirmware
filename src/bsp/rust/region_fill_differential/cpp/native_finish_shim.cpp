// SR2d-4 Task 6's native_finish GLUE harness: real StreamedChunk-shaped chunk backings (payload at
// `backing + kChunkPayloadOffset`, a NON-ZERO offset -- see the module doc in
// `tests/native_finish_glue.rs` for why that matters) plus the REAL StreamedChunk convert-state/
// payload accessor bodies `streaming_loader::prod::native_finish` calls through its
// `unsafe extern "C" { ... }` block -- `deluge_streaming_chunk_payload`, `_set_loaded`,
// `_convert_state`, `_set_convert_state` (and the two small "which manager/is this chunk
// unloadable" plumbing symbols that block also declares).
//
// Deliberately does NOT compile `storage/audio/stream/async_fill.cpp` itself: that translation
// unit's OTHER functions (the legacy `begin_fill`/`finish_fill` bodies, and ~25 weak efatfs
// fallbacks) reference `Sample`/`SampleStream`/`GeneralMemoryAllocator`/`Cluster::set_size` and the
// rest of the app's storage/model closure -- the same "large C++ closure" trap
// `tests/host_end_to_end.rs`'s module doc documents empirically for the `host_app` feature. Since a
// `cc`-compiled `.cpp` file is ONE link-time object, pulling in even one symbol from that TU would
// drag in every other symbol it references too.
//
// Instead, this shim includes ONLY the real, unmodified `storage/cluster/cluster.h` (so
// `StreamedChunk`'s field layout, `kChunkPayloadOffset`, and `payload()`/`payload_with_trailing_slack()`
// are the REAL production ones, computed by the REAL compiler from the REAL header -- not
// hand-derived), and re-states the four accessor bodies verbatim (character-for-character identical
// to `async_fill.cpp`'s own definitions). `tests/native_finish_glue.rs`'s
// `accessor_bodies_match_async_fill_cpp_verbatim` test guards against silent drift: it greps
// `async_fill.cpp`'s live source for each of these bodies and fails loudly if a future edit there
// isn't mirrored here -- turning "this shim quietly stops testing the real thing" into a loud CI
// failure, the same "loud, not silent" bias the rest of this arc follows.
//
// `Cluster::size`/`Cluster::size_magnitude` (referenced by `payload()` et al.) are ODR-declared in
// cluster.h but defined in cluster.cpp -- NOT compiled here (same reasoning as above: `cluster.cpp`
// also defines `Cluster::set_size`/`deluge::cluster::add_lease` etc., which pull in the resource
// manager glue). This shim provides its own definitions instead (`region_fill_diff_set_cluster_size`
// below sets them), which is legal exactly because cluster.cpp is never linked alongside this file --
// no duplicate-symbol hazard.
#include "storage/cluster/cluster.h"

#include "libdeluge/streaming_fill.h" // the real DelugeChunkConvertState C-ABI type

#include <cstddef>
#include <cstdint>
#include <new>

// Out-of-class definitions for Cluster's static data members (declared, not defined, in cluster.h;
// normally defined in cluster.cpp, which this shim deliberately does not compile -- see the file
// doc). `payload()`/`payload_with_trailing_slack()` read `Cluster::size` at call time, so this must
// be set (via `region_fill_diff_set_cluster_size` below) before any chunk backing this shim
// constructs is used.
size_t Cluster::size = 0;
size_t Cluster::size_magnitude = 0;

extern "C" {

/// Set the session cluster size, mirroring what `Cluster::set_size()` (cluster.cpp, not compiled
/// here) would do at boot. Must be called before any chunk this shim constructs has its payload
/// touched.
void region_fill_diff_set_cluster_size(size_t size, size_t magnitude) {
	Cluster::size = size;
	Cluster::size_magnitude = magnitude;
}

/// Byte offset of a chunk's payload from its slab-slot/backing base -- `kChunkPayloadOffset`
/// (cluster.h), the REAL compiler-computed value (`max(sizeof(StreamedChunk), sizeof(ComputedChunk))
/// + CACHE_LINE_SIZE`). Always nonzero (a StreamedChunk header plus at least one cache line) -- the
/// property that makes a "backing pointer used as payload" bug (SR2d-4 Task 2's neighbour-payload
/// Critical) detectable by seeding distinguishable bytes in each region.
size_t region_fill_diff_chunk_payload_offset(void) {
	return kChunkPayloadOffset;
}

/// Total backing-allocation size for one chunk of `cluster_size` bytes: header + front guard
/// (`kChunkPayloadOffset`) + payload (`cluster_size`) + trailing guard (`kChunkTrailingGuard`),
/// mirroring the slab-slot layout cluster.h's own doc describes.
size_t region_fill_diff_chunk_backing_size(size_t cluster_size) {
	return kChunkPayloadOffset + cluster_size + kChunkTrailingGuard;
}

/// `sizeof(StreamedChunk)` -- the real, compiler-computed struct size. `tests/native_finish_glue.rs`
/// uses this (together with `region_fill_diff_chunk_payload_offset`) to pick a cluster size whose
/// "wrong span" (a hypothetical backing-as-payload bug's `[0, cluster_size+7)` read/write) reaches
/// past the struct's own named fields into the unnamed front-guard padding `[sizeof(StreamedChunk),
/// kChunkPayloadOffset)` -- see that test's `header_and_front_guard_are_never_touched_by_a_correct_finish`
/// for what this proves.
size_t region_fill_diff_chunk_header_size(void) {
	return sizeof(StreamedChunk);
}

/// Placement-news a real `StreamedChunk` at `dest`, mirroring `SampleStream::cluster_construct`
/// (`sample_stream.cpp`) minus the `sample`/`resource_slot` field writes that function makes: this
/// harness's `native_finish` calls never dereference `cluster.sample` (that only happens inside the
/// LEGACY C++ `begin_fill`/`finish_fill` bodies this test deliberately doesn't call -- see the file
/// doc), and `resource_slot` is only read by the `ALPHA_OR_BETA_VERSION` i040 lease-count checkpoint
/// inside that same legacy `finish_fill`. Signature matches `deluge_resource::ConstructFn` exactly
/// (`ctx, owner, index, dest`), so it can be handed to `deluge_resource_set_construct` directly.
void region_fill_diff_chunk_construct(void* /*ctx*/, void* /*owner*/, uint32_t index, void* dest) {
	auto* cluster = new (dest) StreamedChunk();
	cluster->payload_ = reinterpret_cast<std::byte*>(dest) + kChunkPayloadOffset; // slot-provenance payload
	cluster->cluster_index = index;
	// cluster->loaded stays false (the fill under test sets it); cluster->sample/resource_slot stay
	// at StreamedChunk()'s own defaults (nullptr / DELUGE_RESOURCE_NO_SLOT) -- see this function's
	// doc for why that's safe here.
}

// -- The two "supporting" symbols `streaming_loader::prod`'s extern block also declares, alongside
// the four accessors below. Neither is part of either Critical's bug surface (both bugs were in
// native_finish's OWN neighbour-payload/convert-state wiring -- see the file doc's opening
// paragraph) -- test-local stand-ins, the same tier `tests/host_end_to_end.rs` already uses for its
// own `ENTER_CRITICAL_SECTION`/`EXIT_CRITICAL_SECTION`/`deluge_in_interrupt`. --

static void* g_active_manager = nullptr;

/// Set the "one process-wide resource manager" `deluge_streaming_resource_manager` below returns.
/// Real production code resolves this via `GeneralMemoryAllocator::get().resourceManager()`; on
/// host, without that singleton compiled in, the test sets the pointer directly instead. Must be
/// called before `streaming_loader::prod::ProdOps::new()` (which caches this return value).
void region_fill_diff_set_active_manager(void* mgr) {
	g_active_manager = mgr;
}

DelugeResource* deluge_streaming_resource_manager(void) {
	return reinterpret_cast<DelugeResource*>(g_active_manager);
}

bool deluge_streaming_chunk_unloadable(void* chunk_backing) {
	return reinterpret_cast<StreamedChunk*>(chunk_backing)->unloadable;
}

// -- The four accessors under test -- character-for-character identical to async_fill.cpp's own
// bodies (verified by tests/native_finish_glue.rs's `accessor_bodies_match_async_fill_cpp_verbatim`
// test). Real `StreamedChunk` field accesses over the REAL struct layout `cluster.h` defines. --

uint8_t* deluge_streaming_chunk_payload(void* chunk_backing) {
	return reinterpret_cast<uint8_t*>(reinterpret_cast<StreamedChunk*>(chunk_backing)->payload().data());
}

void deluge_streaming_chunk_set_loaded(void* chunk_backing) {
	reinterpret_cast<StreamedChunk*>(chunk_backing)->loaded = true;
}

DelugeChunkConvertState deluge_streaming_chunk_convert_state(void* chunk_backing) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	DelugeChunkConvertState state{};
	for (size_t i = 0; i < 3; ++i) {
		state.first_three_bytes[i] = static_cast<uint8_t>(cluster->first_three_bytes_pre_data_conversion[i]);
	}
	state.start_converted = cluster->extra_bytes_at_start_converted;
	state.end_converted = cluster->extra_bytes_at_end_converted;
	return state;
}

void deluge_streaming_chunk_set_convert_state(void* chunk_backing, DelugeChunkConvertState state) {
	auto* cluster = reinterpret_cast<StreamedChunk*>(chunk_backing);
	for (size_t i = 0; i < 3; ++i) {
		cluster->first_three_bytes_pre_data_conversion[i] = static_cast<char>(state.first_three_bytes[i]);
	}
	cluster->extra_bytes_at_start_converted = state.start_converted;
	cluster->extra_bytes_at_end_converted = state.end_converted;
}

} // extern "C"
