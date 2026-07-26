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

#pragma once

#include <cstddef>
#include <cstdint>

class Sample;

/// @file
/// Resource-manager Source callbacks for SAMPLE (streamed) chunks, plus the asset-*definition* core,
/// relocated out of `SampleStream` into their own translation unit. `owner` is always the `Sample*`
/// registered by `deluge_streaming_define_asset()`; each callback reaches that sample's `SampleStream`
/// via `sample->stream()` to update the residency table (`table_`) and, for materialize, to read the
/// cluster's data (`read_cluster_data()`). See sample_stream.h's "Cluster residency" section for the
/// broader contract these implement.

extern "C" {

/// @brief Lazily define @p sample's resource-manager Asset, whose Chunks are its SAMPLE clusters.
///
/// Idempotent: returns the existing id (cached on `sample->stream()`) on later calls. The manager is
/// the sole SDRAM evictor, so a missing manager or an exhausted asset table is fatal (`FREEZE`) --
/// there is no legacy fallback, hence this never returns `DELUGE_RESOURCE_NO_ASSET`.
/// @return The Asset id.
uint32_t deluge_streaming_define_asset(Sample* sample);

/// @brief Prefetch counterpart to deluge_streaming_chunk_materialize(): construct the `StreamedChunk`
///        into @p dest (the manager backing) but do not read it (`loaded` stays false), so the audio
///        thread never blocks — the background loader fills it later.
void deluge_streaming_chunk_construct(void* ctx, void* owner, uint32_t index, void* dest);

/// @brief Reconstruct cluster @p index synchronously: placement-new a `StreamedChunk` into @p dest
///        (the manager backing) and read its data. On read failure the chunk is destructed and the
///        slot freed.
/// @return `true` if the cluster was materialized.
bool deluge_streaming_chunk_materialize(void* ctx, void* owner, uint32_t index, void* dest, size_t len);

} // extern "C"
