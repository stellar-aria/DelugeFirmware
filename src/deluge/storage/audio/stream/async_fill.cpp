/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "storage/audio/stream/async_fill.h"

#include "libdeluge/file_io.h"   // R2 Task 4: deluge_efatfs_file_*/_dir_* weak stubs
#include "libdeluge/stream_io.h" // R3 Task 2: deluge_efatfs_stream_* weak stubs

#include "io/debug/log.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "storage/audio/stream/sample_residency.h"
#include "storage/audio/stream/sample_stream.h"
#include "storage/audio/stream/stitch.h"
#include "storage/cluster/cluster.h"
#include <cstddef>
#include <optional>
#include <span>

#include "deluge_resource.h" // deluge_resource_mark_ready

// FFI layout guard (M4): `StreamingFillDescriptor` crosses the C++/Rust boundary by value (see
// streaming_fill.h) with a hand-written `#[repr(C)]` mirror in streaming_loader.rs. These
// static_asserts catch field drift at compile time on whichever side notices first. Expressed
// pointer-width-relative (not hardcoded byte offsets) so the same assertions hold unchanged on
// both the 32-bit ARM device and the 64-bit host_app build: `dest` leads at offset 0, then
// `num_sectors` (u32, packed right after the pointer), then `ok` (1 byte, at ptr+4), then 3 pad
// bytes to realign the next u32, then `handle` at ptr+8 and `byte_offset` at ptr+12, and the
// struct's overall size pads up to the pointer's own alignment (its strictest member) —
// sizeof(ptr)+16 covers that on both widths (20 on 32-bit: dest[4]+num_sectors[4]+ok[1]->pad[3]+
// handle[4]+byte_offset[4]=20; 24 on 64-bit: dest[8]+num_sectors[4]+ok[1]->pad[3]+handle[4]+
// byte_offset[4]=24, already a multiple of the 8-byte pointer alignment). Verified by compiling
// both builds, not derived from a naive guess.
static_assert(offsetof(StreamingFillDescriptor, dest) == 0);
static_assert(offsetof(StreamingFillDescriptor, num_sectors) == sizeof(uint8_t*));
static_assert(offsetof(StreamingFillDescriptor, ok) == sizeof(uint8_t*) + 4);
static_assert(offsetof(StreamingFillDescriptor, handle) == sizeof(uint8_t*) + 8);
static_assert(offsetof(StreamingFillDescriptor, byte_offset) == sizeof(uint8_t*) + 12);
static_assert(sizeof(StreamingFillDescriptor) == sizeof(uint8_t*) + 16);

// FFI layout guard for DelugeStreamingFillContext (SR2d-4 Task 1), mirroring FillContext's own
// `core::mem::offset_of!` guard in streaming_loader.rs. Unlike StreamingFillDescriptor, this struct
// holds no pointer, so its layout is identical on the 32-bit device and the 64-bit host_app build:
// two leading u32s (0, 4), then the u64 realigned at its natural 8-byte boundary (already aligned,
// at 8), then i32/u32/u32 (16, 20, 24), then the trailing u8 (28), padded up to the u64 member's
// 8-byte alignment (32 total).
static_assert(offsetof(DelugeStreamingFillContext, efatfs_handle) == 0);
static_assert(offsetof(DelugeStreamingFillContext, audio_data_start_pos_bytes) == 4);
static_assert(offsetof(DelugeStreamingFillContext, audio_data_length_bytes) == 8);
static_assert(offsetof(DelugeStreamingFillContext, first_cluster_index_with_no_audio_data) == 16);
static_assert(offsetof(DelugeStreamingFillContext, cluster_size) == 20);
static_assert(offsetof(DelugeStreamingFillContext, cluster_size_magnitude) == 24);
static_assert(offsetof(DelugeStreamingFillContext, raw_data_format) == 28);
static_assert(offsetof(DelugeStreamingFillContext, byte_depth) == 29);
static_assert(offsetof(DelugeStreamingFillContext, num_channels) == 30);
static_assert(sizeof(DelugeStreamingFillContext) == 32);

// FFI layout guard for DelugeChunkConvertState (SR2d-4 Task 1), mirroring ConvertState's own
// `core::mem::offset_of!` guard in streaming_loader.rs. No pointer members, and every member
// (`uint8_t[3]` then two `bool`s) is 1-byte-aligned, so this layout is identical on the 32-bit
// device and the 64-bit host_app build: no padding anywhere, laid out back-to-back.
static_assert(offsetof(DelugeChunkConvertState, first_three_bytes) == 0);
static_assert(offsetof(DelugeChunkConvertState, start_converted) == 3);
static_assert(offsetof(DelugeChunkConvertState, end_converted) == 4);
static_assert(sizeof(DelugeChunkConvertState) == 5);

extern "C" {

DelugeResource* deluge_streaming_resource_manager(void) {
	return GeneralMemoryAllocator::get().resourceManager();
}

// Weak fallback for the region-port open() bridge's stream-backing -> resource-asset accessor
// (SR2d-5 Task 1). The real definition (sample_stream.cpp) forwards to
// deluge_streaming_define_asset() (chunk_residency.cpp) wherever a real SampleStream is compiled in; build
// configs that link this TU without one (a minimal test driver assembling its own source list,
// mirroring the other weak fallbacks in this file) resolve this no-op instead.
__attribute__((weak)) uint32_t deluge_sample_stream_asset_id(void* /*stream_backing*/) {
	return DELUGE_RESOURCE_NO_ASSET;
}

bool deluge_streaming_chunk_unloadable(void* chunk_backing) {
	return reinterpret_cast<StreamedChunk*>(chunk_backing)->unloadable;
}

// The two StreamedChunk field-touch accessors the native Rust fill task needs (SR2d-4 Task 2):
// payload pointer (read/DMA destination) and the loaded flag the C++ region cursor polls for
// readiness. Same shape as deluge_streaming_chunk_unloadable just above -- real bodies only, no
// weak fallback, because this TU (async_fill.cpp) is part of the shared deluge_SOURCES glob and so
// always compiles and links into every BSP, not just the Rust/Embassy one.
uint8_t* deluge_streaming_chunk_payload(void* chunk_backing) {
	return reinterpret_cast<uint8_t*>(reinterpret_cast<StreamedChunk*>(chunk_backing)->payload().data());
}

void deluge_streaming_chunk_set_loaded(void* chunk_backing) {
	reinterpret_cast<StreamedChunk*>(chunk_backing)->loaded = true;
}

// StreamedChunk convert-state get/set accessors (SR2d-4 Task 1): thin accessors over the same
// three fields the legacy sync-fiber finish_fill() above reads/writes directly
// (first_three_bytes_pre_data_conversion, extra_bytes_at_start_converted,
// extra_bytes_at_end_converted -- cluster.h:102-106). The native Rust fill's begin/finish
// (streaming_loader.rs's native_begin/native_finish) read/write convert-state directly through
// these (SR2d-4 Task 2) -- the single store for this state, replacing an earlier per-chunk sidecar
// table (since deleted). Same shape as deluge_streaming_chunk_payload/_set_loaded just above --
// real bodies only, no weak fallback, because this TU is part of the shared deluge_SOURCES glob
// and so always compiles and links into every BSP.
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

// Weak fallbacks for the two async-streaming-loader selector/wakeup symbols. The Rust Embassy BSP
// provides the real definitions (streaming_loader.rs) whenever it links this crate —
// unconditionally, so `deluge_streaming_async_active()` always resolves there regardless of
// whether `async_streaming_loader` is enabled (its return value depends on the cargo feature; the
// symbol's existence does not). Every other BSP/config (legacy/host-cooperative sim, rza1) never
// links that crate, so these weak definitions are what resolve instead: "no async backing, never
// signalled" — i.e. today's synchronous-fiber-pump behaviour.
__attribute__((weak)) bool deluge_streaming_async_active(void) {
	return false;
}

__attribute__((weak)) void deluge_streaming_signal_fill(void) {
	// No async task to wake on this BSP/config.
}

// Weak fallbacks for the embedded-fatfs streaming READ symbols. The Rust Embassy BSP provides the
// real definitions (efatfs_fs.rs / streaming_loader.rs) whenever it links this crate with the
// `efatfs_streaming` feature; every other BSP/config resolves these instead: "no efatfs backing" —
// open/read_at always fail, close is a no-op, and the selector is false. NOTE (R1): the streaming
// read is now efatfs-only — there is NO C-FatFS read fallback anymore. On a non-efatfs BSP (the
// legacy C/C++ RZA1 BSP, committed for retirement in favour of the Rust BSP) open_read_stream()
// therefore fails and streamed samples do not load; that BSP's streaming read is retired, not
// silently falling back.
__attribute__((weak)) bool deluge_efatfs_open(const char* /*path*/, uint32_t* /*out_handle*/) {
	return false;
}

__attribute__((weak)) void deluge_efatfs_close(uint32_t /*handle*/) {
	// No efatfs handle table on this BSP/config.
}

__attribute__((weak)) bool deluge_efatfs_read_at(uint32_t /*handle*/, uint32_t /*byte_offset*/, void* /*dst*/,
                                                 uint32_t /*count*/, uint32_t* /*out_read*/) {
	return false; // No efatfs handle table on this BSP/config.
}

__attribute__((weak)) bool deluge_streaming_efatfs_active(void) {
	return false;
}

// Weak fallbacks for R2 Task 4's task-context efatfs file/directory C-ABI
// (`include/libdeluge/file_io.h`). The Rust Embassy BSP provides the real
// definitions (`efatfs_fs.rs` device / `efatfs_host_shim.rs` host) whenever it
// links this crate with the `efatfs_streaming` feature; every other
// BSP/config resolves these instead. `deluge::io::File`/`Directory`
// (file.cpp) only ever call these when `deluge_streaming_efatfs_active()` is
// true, so a BSP without the real symbols never reaches them at runtime --
// these exist purely so the link succeeds.
__attribute__((weak)) bool deluge_efatfs_file_open(const char* /*path*/, uint8_t /*mode*/, uint32_t* /*out_handle*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_read(uint32_t /*handle*/, void* /*dst*/, uint32_t /*count*/,
                                                   uint32_t* /*out_read*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_read_exact(uint32_t /*handle*/, void* /*dst*/, uint32_t /*count*/,
                                                         uint32_t* /*out_read*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_write(uint32_t /*handle*/, const void* /*src*/, uint32_t /*count*/,
                                                    uint32_t* /*out_written*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_seek(uint32_t /*handle*/, uint32_t /*offset*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_size(uint32_t /*handle*/, uint32_t* /*out_size*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_file_truncate(uint32_t /*handle*/, uint32_t /*new_len*/) {
	return false;
}

__attribute__((weak)) void deluge_efatfs_file_close(uint32_t /*handle*/) {
	// No task-context file table on this BSP/config.
}

__attribute__((weak)) bool deluge_efatfs_dir_open(const char* /*path*/, uint32_t* /*out_handle*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_dir_read(uint32_t /*handle*/, char* /*out_name*/, uint32_t /*out_name_cap*/,
                                                  bool* /*out_is_dir*/, uint32_t* /*out_size*/,
                                                  uint32_t* /*out_modified*/, uint8_t* /*out_attrs*/,
                                                  bool* /*out_has_entry*/) {
	return false;
}

__attribute__((weak)) void deluge_efatfs_dir_close(uint32_t /*handle*/) {
	// No dir-cursor table on this BSP/config.
}

__attribute__((weak)) bool deluge_efatfs_mkdir(const char* /*path*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_unlink(const char* /*path*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_rename(const char* /*old_path*/, const char* /*new_path*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_set_time(const char* /*path*/, uint16_t /*year*/, uint8_t /*month*/,
                                                  uint8_t /*day*/, uint8_t /*hour*/, uint8_t /*minute*/,
                                                  uint8_t /*second*/) {
	return false;
}

// Weak fallbacks for R3 Task 2's persistent stream-write efatfs C-ABI
// (`include/libdeluge/stream_io.h`). The Rust Embassy BSP provides the real definitions
// (`efatfs_fs.rs` device / `efatfs_host_shim.rs` host) whenever it links this crate with the
// `efatfs_streaming` feature; every other BSP/config resolves these instead. `deluge::io::Stream`
// (stream.cpp) only ever calls these when `deluge_streaming_efatfs_active()` is true, so a BSP
// without the real symbols never reaches them at runtime -- these exist purely so the link
// succeeds.
__attribute__((weak)) bool deluge_efatfs_stream_open(const char* /*path*/, uint8_t /*mode*/, uint32_t* /*out_handle*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_write_at(uint32_t /*handle*/, uint32_t /*byte_offset*/,
                                                         const void* /*src*/, uint32_t /*count*/,
                                                         uint32_t* /*out_written*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_read_at_via(uint32_t /*handle*/, uint32_t /*byte_offset*/,
                                                            void* /*dst*/, uint32_t /*count*/, uint32_t* /*out_read*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_flush(uint32_t /*handle*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_truncate(uint32_t /*handle*/, uint32_t /*new_len*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_size(uint32_t /*handle*/, uint32_t* /*out_size*/) {
	return false;
}

__attribute__((weak)) bool deluge_efatfs_stream_close(uint32_t /*handle*/) {
	return false; // No stream-write context table on this BSP/config.
}

} // extern "C"
