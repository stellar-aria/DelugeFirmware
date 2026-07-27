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

#include <cstdint>

class Sample;
struct StreamedChunk;

namespace deluge::audio::stream {

/// Non-leasing, non-blocking residency peek: the resident StreamedChunk for @p clusterIndex, or
/// nullptr if no Asset is defined yet or the index isn't resident. Byte-for-byte the logic
/// SampleStream::chunk_at() has today -- the single place that reaches the sample's peek internals,
/// so callers stop depending on the SampleStream method surface (when SampleStream later dissolves,
/// only this body changes, not the call sites). The returned chunk may be resident-but-not-yet-loaded;
/// callers must check `->loaded` before reading bytes.
[[nodiscard]] StreamedChunk* peek(const Sample& sample, uint32_t clusterIndex);

/// Intent-named prefetch: construct + lease the chunk for @p clusterIndex now and schedule its read,
/// never blocking on I/O (CLUSTER_ENQUEUE semantics). Byte-for-byte SampleStream::get_cluster(index,
/// CLUSTER_ENQUEUE) -- the single place the enqueue path reaches SampleStream's lease internals. The
/// returned chunk is leased but may not yet be loaded; callers check `->loaded` before reading bytes.
[[nodiscard]] StreamedChunk* prefetch(Sample& sample, uint32_t clusterIndex);

/// General residency request forwarding a runtime @p loadInstruction. Its sole caller -- the head/
/// loop-start marker lookahead -- is polymorphic across CLUSTER_ENQUEUE / CLUSTER_LOAD_IMMEDIATELY /
/// CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE (the sample-preview path passes LOAD_IMMEDIATELY). Byte-for-byte
/// SampleStream::get_cluster(index, loadInstruction). The intent-named prefetch()/load_now() wrappers
/// are for the statically-known callers; this is the escape hatch for the one runtime-variable caller.
[[nodiscard]] StreamedChunk* request(Sample& sample, uint32_t clusterIndex, int32_t loadInstruction);

} // namespace deluge::audio::stream
