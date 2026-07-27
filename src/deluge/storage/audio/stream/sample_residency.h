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
enum class Error;

namespace deluge::audio::stream {

/// Non-leasing, non-blocking residency peek: the resident StreamedChunk for @p clusterIndex, or
/// nullptr if no Asset is defined yet or the index isn't resident. The single place that reaches the
/// sample's peek internals (it took over the former SampleStream::chunk_at(), now deleted), so callers
/// don't depend on the SampleStream method surface (when SampleStream later dissolves, only this body
/// changes, not the call sites). The returned chunk may be resident-but-not-yet-loaded; callers must
/// check `->loaded` before reading bytes.
[[nodiscard]] StreamedChunk* peek(const Sample& sample, uint32_t clusterIndex);

/// Intent-named prefetch: construct + lease the chunk for @p clusterIndex now and schedule its read,
/// never blocking on I/O (CLUSTER_ENQUEUE semantics). The single place the enqueue path reaches the
/// resource manager's lease internals (the residency dispatch, `acquire_cluster` in the .cpp). The
/// returned chunk is leased but may not yet be loaded; callers check `->loaded` before reading bytes.
[[nodiscard]] StreamedChunk* prefetch(Sample& sample, uint32_t clusterIndex);

/// General residency request forwarding a runtime @p loadInstruction. Its sole caller -- the head/
/// loop-start marker lookahead -- is polymorphic across CLUSTER_ENQUEUE / CLUSTER_LOAD_IMMEDIATELY /
/// CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE (the sample-preview path passes LOAD_IMMEDIATELY). Routes to the
/// residency dispatch (`acquire_cluster`) with the runtime instruction. The intent-named
/// prefetch()/load_now() wrappers are for the statically-known callers; this is the escape hatch for the
/// one runtime-variable caller.
[[nodiscard]] StreamedChunk* request(Sample& sample, uint32_t clusterIndex, int32_t loadInstruction);

/// Blocking load-now: acquire + materialize the chunk for @p clusterIndex, reading from the card on a
/// miss (may block on I/O -- the must-load-now contract). If @p error is non-null it is set on failure.
/// Routes to the residency dispatch (`acquire_cluster`) with CLUSTER_LOAD_IMMEDIATELY. NOT for the audio
/// ISR -- for load-time / UI paths permitted to block. Returned chunk is leased and (on success) loaded;
/// nullptr on failure.
[[nodiscard]] StreamedChunk* load_now(Sample& sample, uint32_t clusterIndex, Error* error = nullptr);

/// Cancel any pending async read of @p chunk by removing it from the loader queue. Used when the chunk's
/// on-disk data is no longer valid (file invalidation), so a queued fill must not run against stale data.
void dequeue(StreamedChunk& chunk);

} // namespace deluge::audio::stream
