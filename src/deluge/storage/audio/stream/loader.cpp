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

#include "libdeluge/storage_owner.h" // deluge_storage_on_owner
#include "libdeluge/system.h"        // deluge_in_interrupt

#include <atomic>

#include "deluge_resource.h" // deluge_resource_loader_{next,enqueue,has_lowest}

namespace deluge::audio::stream::loader {

// Instrumentation only, compile-disabled by default (kept for future debugging). `timeLastFinish` is
// deliberately a translation-unit-scope variable rather than a class member — nothing else needs it, and
// it carries no lifetime dependency on any particular owning object.
#define REPORT_AWAY_TIME 0

#if REPORT_AWAY_TIME
uint16_t timeLastFinish;
#endif

// Lowest loader-queue priority — a cluster that is still wanted but not currently loadable (e.g. the card
// was pulled mid-read) is re-queued here so it waits behind everything else. Matches the value
// deluge_resource_loader_has_lowest() looks for.
constexpr uint32_t kLowestLoaderPriority = 0xFFFFFFFF;

namespace {
/// Reconstruct one popped, manager-owned cluster. Opens the user-action gate for the duration of the card
/// read (see `deluge::sync::StorageOp`) so the handful of safe UI actions can run while it blocks. Every
/// queued cluster is manager-owned — get_cluster() ran ensure_resource_asset() and construct/materialize
/// set `cluster->sample` before it was enqueued — so it is already constructed + leased; the read just
/// flips `loaded` true (or fails).
/// @return `true` to keep draining; `false` only when the read failed while the cluster is still wanted
///         (callers still hold reasons), in which case the caller re-queues it and stops.
bool reconstruct_one(StreamedChunk* cluster) {
	bool ok;
	{
		deluge::sync::StorageOp storage_op; // permits safe UI actions for the read's duration
		ok = cluster->sample->stream().read_cluster_data(*cluster, 0);
	}
	if (ok) {
		return true;
	}

	D_PRINTLN("load Cluster fail"); // most likely the card was ejected mid-read
	// If the cluster dropped to 0 reasons while loading, it has already been made available — nothing to do,
	// keep draining. Otherwise callers still want it, so the caller re-queues it and stops.
	return deluge::cluster::lease_count(cluster->resource_slot) == 0;
}
} // namespace

void pump(int32_t max_num, bool may_process_user_actions) {
	// Admission. Nothing below may touch the SD card except read_cluster_data(), or it would re-enter here.
	// Refuse while the card is mid-access or the audio routine holds the lock (the latter guards the
	// cooperative convert-yield re-entrancy; its necessity is unverified but retained).
	if (currentlyAccessingCard || AudioEngine::audioRoutineLocked) {
		return;
	}
	// Card gone / uninitialised: nothing to load, but still let queued user actions (undo/redo) breathe.
	if (audioFileManager.cardUnavailableForStreaming()) {
		if (may_process_user_actions) {
			playbackHandler.slowRoutine();
		}
		return;
	}

#if REPORT_AWAY_TIME
	uint16_t startTime = MTU2.TCNT_0;
	uint16_t awayTime = startTime - timeLastFinish;
	int32_t uSecAway = timerCountToUS(awayTime);
	if (uSecAway > 1000) {
		D_PRINTLN("away  %d", uSecAway);
	}
#endif

	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	for (int32_t count = 0; count < max_num;) {
		// Between reads is a safe point to process pending user actions (undo/redo).
		if (may_process_user_actions) {
			playbackHandler.slowRoutine();
		}

		// Pop the most-urgent queued + still-leased cluster (the manager de-queues abandoned-unleased ones
		// itself). Empty queue → done.
		auto* cluster = reinterpret_cast<StreamedChunk*>(deluge_resource_loader_next(mgr));
		if (cluster == nullptr) {
			return;
		}

		// Safety net: markAsUnloadable already de-queued this and loader_next cleared its queued flag, so
		// skipping can't loop. An unloadable cluster doesn't count against max_num.
		if (cluster->unloadable) {
			continue;
		}

		if (!reconstruct_one(cluster)) {
			// Read failed while still wanted — re-queue at lowest priority and stop, else we'd keep
			// re-popping the same cluster until the card is back.
			deluge_resource_loader_enqueue(mgr, cluster->resource_slot, kLowestLoaderPriority);
			break;
		}

		count++;
	}

#if REPORT_AWAY_TIME
	timeLastFinish = MTU2.TCNT_0;
#endif
}

bool has_lowest_priority_queued() {
	return deluge_resource_loader_has_lowest(GeneralMemoryAllocator::get().resourceManager());
}

namespace {
// Coalesced dispatch of the fill onto the storage owner (the worker fiber on Embassy).
// Only ever touched on the main executor (the streaming requesters + the fiber that runs
// the fill), so not cross-thread.
deluge::storage::Coalescer g_loader_coalescer;
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
