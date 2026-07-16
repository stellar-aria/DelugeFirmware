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

/// @brief The audio-stream module's SD-card cluster loader: pumps the resource-manager loader queue.
///
/// A stateless set of free functions over the single `deluge_resource` manager instance — there is
/// nothing to own besides the queue itself (which lives in the resource manager). Card-lifecycle
/// state (ejected/disabled/uninitialised) and the legacy `loadCluster`/`clusterBeingLoaded` reentrancy
/// sentinel stay owned by `AudioFileManager` for now (Task 4 retires the sentinel); `pump()` reaches
/// them through two minimal accessors so the guard behaviour is reproduced exactly.
namespace deluge::audio::stream::loader {

/// @brief Pump the resource-manager loader queue: pop enqueued clusters (highest priority first) and
///        reconstruct each one, up to `max_num` per call.
///
/// Bails out immediately (without touching the card) if a card access is already underway, a legacy
/// cluster is mid-conversion, the audio routine is locked, or the card is unavailable — in the
/// card-unavailable case it optionally runs `PlaybackHandler::slowRoutine()` first so pending user
/// actions (undo/redo etc.) still get serviced while the card is down. Never called from the render
/// thread's hot path; this is the cooperative, re-entrant pump that both the FatFs `disk_read`/
/// `disk_write` hooks and the various UI/engine idle points call to keep streaming caught up.
///
/// @param max_num Maximum number of clusters to load in this call (keeps a single pump bounded).
/// @param may_process_user_actions If true, `PlaybackHandler::slowRoutine()` runs between each
///        cluster load (and on the card-unavailable early-out), giving pending user actions a chance
///        to run while the card isn't being read. Must stay false when called from a context where
///        that reentrancy isn't safe (e.g. deep inside the card-access routine).
void pump(int32_t max_num = 128, bool may_process_user_actions = false);

/// @brief Whether any queued (and still-leased) cluster sits at the loader's lowest priority.
///
/// Used as the load-song yield gate: callers wait for the lowest-priority backlog to drain before
/// proceeding, so a burst of low-priority prefetch doesn't starve something more urgent.
/// @return true if the loader queue has at least one lowest-priority element.
[[nodiscard]] bool has_lowest_priority_queued();

} // namespace deluge::audio::stream::loader
