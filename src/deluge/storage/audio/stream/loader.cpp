/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "storage/audio/stream/loader.h"
#include "extern.h" // currentlyAccessingCard
#include "io/debug/log.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/owner.h"
#include "sync/storage_op.h"

#include "libdeluge/storage_owner.h"  // deluge_storage_on_owner
#include "libdeluge/streaming_fill.h" // deluge_streaming_async_active
#include "libdeluge/system.h"         // deluge_in_interrupt

#include <atomic>

#include "deluge_resource.h" // deluge_resource_loader_{next,enqueue,has_lowest}

namespace deluge::audio::stream::loader {

void pump(int32_t max_num, bool may_process_user_actions) {
	// The synchronous C++ cluster-fill drain was retired along with the C-host renderers: every
	// remaining target loads streamed clusters through the async Embassy fill task
	// (`streaming_loader::streaming_fill_task`), and `deluge_streaming_async_active()` is true on
	// all of them, so this is a no-op. Kept as a stub only so the in-app callers (audio_engine,
	// load_song_ui, browser, audio_file_manager, deluge.cpp) still link; removing those calls and
	// this file is a follow-up cleanup.
	(void)max_num;
	(void)may_process_user_actions;
}

bool has_lowest_priority_queued() {
	return deluge_resource_loader_has_lowest(GeneralMemoryAllocator::get().resourceManager());
}

namespace {
// Coalesced dispatch of the fill onto the storage owner (the worker fiber on Embassy).
// Only ever touched on the main executor (the streaming requesters + the fiber that runs
// the fill), so not cross-thread. HIGH-priority: the audio-streaming fill must not queue
// behind UI/recorder work (NORMAL) on the shared worker ring — an underrun is audible.
deluge::storage::Coalescer g_loader_coalescer{/*sd_routine=*/false, /*priority=*/true};
// The winning request()'s args, read by loader_fill when it runs. A coalesced-away request
// on Embassy updates these but its own dispatch is dropped; the in-flight fill picks up the
// latest values (last-writer-wins). Legacy runs inline, so these are always this call's args.
std::atomic<int32_t> g_fill_max{128};
std::atomic<bool> g_fill_mpua{false};

void loader_fill(void*) {
	pump(g_fill_max.load(std::memory_order_relaxed), g_fill_mpua.load(std::memory_order_relaxed));
}
} // namespace

void request_pump(int32_t max_num, bool may_process_user_actions) {
	// Same gate as pump() above — when the async task owns the loader queue, don't even dispatch
	// onto the fiber for what would be a no-op pump().
	if (deluge_streaming_async_active()) {
		return;
	}
	// Never from an ISR / the audio interrupt-executor: there `deluge_storage_on_owner()` is
	// false (it isn't the worker fiber), so we would take the coalescer path and race its
	// main-executor-only state. The audio render does no card I/O, so reaching here from an
	// interrupt is a programming error; do nothing rather than corrupt the coalescer.
	if (deluge_in_interrupt()) {
		return;
	}
	// Already on the storage owner (the fiber on Embassy; always, on legacy/host where the
	// caller *is* the owner) — run the fill now rather than dispatching. This makes legacy/host
	// behaviourally identical to a direct pump() (the coalescer is never touched → golden-inert),
	// and keeps a fiber-context caller from re-dispatching onto the fiber it is already running on.
	if (deluge_storage_on_owner()) {
		pump(max_num, may_process_user_actions);
		return;
	}
	g_fill_max.store(max_num, std::memory_order_relaxed);
	g_fill_mpua.store(may_process_user_actions, std::memory_order_relaxed);
	g_loader_coalescer.request(loader_fill, nullptr);
}

} // namespace deluge::audio::stream::loader
