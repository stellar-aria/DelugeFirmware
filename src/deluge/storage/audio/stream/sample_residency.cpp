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

#include "storage/audio/stream/sample_residency.h"

#include "deluge_resource.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "storage/cluster/cluster.h"

namespace deluge::audio::stream {

StreamedChunk* peek(const Sample& sample, uint32_t clusterIndex) {
	const uint32_t assetId = sample.stream().resource_asset_id();
	if (assetId == DELUGE_RESOURCE_NO_ASSET) {
		return nullptr; // no Asset defined yet, so nothing can be resident
	}
	DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
	return reinterpret_cast<StreamedChunk*>(deluge_resource_peek(mgr, assetId, clusterIndex));
}

} // namespace deluge::audio::stream
