/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "model/sample/sample_cluster.h"
#include "definitions_cxx.hpp"
#include "io/debug/log.h"
#include "model/sample/sample.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/audio/stream/sample_stream.h" // the forwarder's target: SampleStream::get_cluster
#include "storage/cluster/cluster.h"
#include <cstddef>

SampleCluster::~SampleCluster() {
	if (cluster) {

#if ALPHA_OR_BETA_VERSION
		uint32_t reasons = deluge::cluster::lease_count(cluster->resource_slot);
		if (cluster == audioFileManager.clusterBeingLoaded && reasons > 0) {
			reasons--;
		}

		if (reasons) {
			D_PRINTLN("uh oh, some reasons left...  %d", reasons);

			// Bay_Mud got this, and thinks a FlashAir card might have been a catalyst. It still "shouldn't" be able to
			// happen though.
			FREEZE_WITH_ERROR("E036");
		}
#endif
		deluge::cluster::free_chunk(cluster);
	}
}

void SampleCluster::ensureNoReason(Sample* sample) {
	if (cluster) {
		if (deluge::cluster::lease_count(cluster->resource_slot)) {
			D_PRINTLN("Cluster has reason!  %d %d", deluge::cluster::lease_count(cluster->resource_slot),
			          sample->filePath.c_str());
			FREEZE_WITH_ERROR("E068");
			delayMS(50);
		}
	}
}

// Calling this will add a reason to the loaded Cluster!
// priorityRating is only relevant if enqueuing.
//
// COEXISTENCE thin forwarder (Phase 4, Task 2): the dispatch itself moved onto
// deluge::audio::stream::SampleStream::get_cluster (sample_stream.{h,cpp}); this is kept only so the
// not-yet-migrated recorder / SampleHolder / RT-reader callers (Tasks 3-4), which still call
// `clusters[i].getCluster(sample, i, ...)`, keep compiling unchanged. Deleted in Task 5. This
// deliberately ignores `this` (the SampleCluster entry) and uses only `clusterIndex`, which is safe
// because every remaining caller passes an index equal to its own entry's subscript (verified at
// migration time).
StreamedChunk* SampleCluster::getCluster(Sample* sample, uint32_t clusterIndex, int32_t loadInstruction,
                                         uint32_t priorityRating, Error* error) {
	return sample->stream().get_cluster(clusterIndex, loadInstruction, priorityRating, error);
}
