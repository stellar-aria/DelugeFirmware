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
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include <cstddef>

SampleCluster::~SampleCluster() {
	if (cluster) {

#if ALPHA_OR_BETA_VERSION
		// No in-flight extra lease exists for manager-owned clusters, so the leftover-reason assertion
		// holds without any discount.
		uint32_t reasons = deluge::cluster::lease_count(cluster->resource_slot);
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
