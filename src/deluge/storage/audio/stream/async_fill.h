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

#pragma once

#include "libdeluge/streaming_fill.h" // StreamingFillDescriptor (the C-ABI descriptor type)

struct StreamedChunk;

namespace deluge::audio::stream {

/// @brief C++ entry point behind `deluge_streaming_begin_fill`: resolve @p cluster's destination
///        buffer and physical sector range.
///
/// Pure lookup + arithmetic (the last-cluster short-read sector-count calc and the cluster's
/// byte offset within the file) — touches no SD hardware, no FatFS. The extern "C"
/// `deluge_streaming_begin_fill` in async_fill.cpp is a thin `void*`-casting wrapper over this.
/// @param cluster The chunk to resolve (already leased/resident, not yet loaded).
/// @return The fill descriptor; `ok == false` on a geometry error (skip the read; do not call
///         finish_fill()).
StreamingFillDescriptor begin_fill(StreamedChunk& cluster);

/// @brief C++ entry point behind `deluge_streaming_finish_fill`: convert, stitch, and publish
///        @p cluster after its sectors have been read.
///
/// Runs `StreamedChunk::convert_data_if_necessary()`, stitches the boundary bytes against any
/// already-loaded neighbouring clusters (`deluge::audio::stream::stitch_boundaries`), marks the
/// chunk loaded, and publishes readiness to the resource manager. The extern "C"
/// `deluge_streaming_finish_fill` in async_fill.cpp is a thin `void*`-casting wrapper over this.
/// In `ALPHA_OR_BETA_VERSION` builds it also carries the "i040" lease-count checkpoint,
/// positioned right after `convert_data_if_necessary()` (which cooperatively yields; see the
/// comment at its call site) and before the stitch step.
/// @param cluster The chunk that was just read (or attempted).
/// @param read_ok Whether the read that followed begin_fill() succeeded.
/// @return `true` if the cluster was successfully converted, stitched, and published; `false` if
///         @p read_ok was `false` (the cluster is left unloaded).
bool finish_fill(StreamedChunk& cluster, bool read_ok);

} // namespace deluge::audio::stream
