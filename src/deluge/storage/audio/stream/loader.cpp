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
#include "extern.h" // currentlyAccessingCard, allowSomeUserActionsEvenWhenInCardRoutine
#include "io/debug/log.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"

#include "deluge_resource.h" // deluge_resource_loader_{next,enqueue,has_lowest}

namespace deluge::audio::stream::loader {

// Instrumentation only, and compile-disabled (kept for future debugging; carried over verbatim from
// the pre-move code). `timeLastFinish` was always a translation-unit-scope variable, never an
// AudioFileManager member, so it moves here with no dangling-reference concern.
#define REPORT_AWAY_TIME 0

#if REPORT_AWAY_TIME
uint16_t timeLastFinish;
#endif

void pump(int32_t max_num, bool may_process_user_actions) {

	if (currentlyAccessingCard) {
		return;
	}
	if (audioFileManager.clusterConversionInProgress()) {
		return; // One might be having stuff done to it, like having its data converted, but not actually reading
		        // the card right now
	}
	if (AudioEngine::audioRoutineLocked) {
		return; // Not sure if this should be neccesary?
	}

	// Cannot call any functions in here which will read the SD card, other than loadCluster(), otherwise that'll
	// re-call this function!

	if (audioFileManager.cardUnavailableForStreaming()) {
		if (may_process_user_actions) {
			playbackHandler.slowRoutine();
		}
		return;
	}

	int32_t count = 0;

#if REPORT_AWAY_TIME
	uint16_t startTime = MTU2.TCNT_0;
	uint16_t awayTime = startTime - timeLastFinish;
	int32_t uSecAway = timerCountToUS(awayTime);
	if (uSecAway > 1000) {
		D_PRINTLN("away  %d", uSecAway);
	}
#endif

	while (true) {

		// We now have an opportunity, since we're not reading the card, to process any pending user actions like
		// undo / redo.
		if (may_process_user_actions) {
			playbackHandler.slowRoutine();
		}

		// Pop the most-urgent queued + still-leased cluster's backing (the manager skips/de-queues
		// abandoned-unleased ones). This prevents loading clusters quickly culled after enqueue.
		void* p = deluge_resource_loader_next(GeneralMemoryAllocator::get().resourceManager());

		// no more clusters to load, so exit
		if (p == nullptr) {
			return;
		}
		StreamedChunk* cluster = reinterpret_cast<StreamedChunk*>(p);

		// The unloadable domain-filter stays here (the manager doesn't know it). markAsUnloadable
		// already de-queues, so this is the safety net — loader_next has cleared its queued flag, so
		// skipping won't loop.
		if (cluster->unloadable) {
			continue;
		}

		// cluster has at least 1 "reason". If it didn't, it would have been removed from the load-queue

		// Do the actual loading
		allowSomeUserActionsEvenWhenInCardRoutine = true; // Sorry!!
		bool success;
		if (cluster->sample != nullptr && cluster->sample->stream().resource_asset_id() != DELUGE_RESOURCE_NO_ASSET) {
			// Manager-owned cluster: it's already constructed + leased (via request), so just do the
			// read directly. NOT loadCluster — its add_lease/removeReason would desync the manager
			// lease, and its `audioRoutineLocked` guard would refuse to load during the offline render
			// (the headless-render streaming starvation we're fixing). The lease persists; the read
			// just flips loaded=true (or fails, handled below as for legacy).
			success = cluster->sample->stream().read_cluster_data(*cluster, 0);
		}
		else {
			// Legacy (non-manager-owned) cluster. Task 4 removes this branch + `loadCluster` once
			// verified dead.
			success = audioFileManager.loadCluster(*cluster);
		}
		allowSomeUserActionsEvenWhenInCardRoutine = false;

		// If that didn't work, presumably because the SD card got ejected...
		if (!success) {
			D_PRINTLN("load Cluster fail");

			// If the Cluster is now down to 0 reasons (i.e. it lost a reason while being loaded), then it's already
			// been made "available" and we don't have a problem
			if (!deluge::cluster::lease_count(cluster->resource_slot)) {}

			// Otherwise, there are still "reasons" waiting for this Cluster to become loaded, so we need to put it
			// back in the loading queue. Presumably it won't actually get loaded for a while - only when the user
			// re-inserts the card
			else {

				// TODO: If that fails, it'll just get awkwardly forgotten about
				deluge_resource_loader_enqueue(GeneralMemoryAllocator::get().resourceManager(), cluster->resource_slot,
				                               0xFFFFFFFF); // lowest priority

				// Also, return now. Normally we stay here til there's nothing left in the load-queue, but now that
				// would leave us in an infinite loop!
				break;
			}
		}

		count++;
		if (count >= max_num) {
			break; // Keep things sane?
		}
	}

#if REPORT_AWAY_TIME
	timeLastFinish = MTU2.TCNT_0;
#endif
}

bool has_lowest_priority_queued() {
	return deluge_resource_loader_has_lowest(GeneralMemoryAllocator::get().resourceManager());
}

} // namespace deluge::audio::stream::loader
