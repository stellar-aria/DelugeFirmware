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
/// state (ejected/disabled/uninitialised) stays owned by `AudioFileManager`; `pump()` reaches it
/// through a minimal accessor.
namespace deluge::audio::stream::loader {

/// @brief Pump the resource-manager loader queue: pop enqueued clusters (highest priority first) and
///        reconstruct each one, up to `max_num` per call.
///
/// Bails out immediately (without touching the card) if a card access is already underway, the audio
/// routine is locked, or the card is unavailable; in the card-unavailable case it optionally runs
/// `PlaybackHandler::slowRoutine()` first so pending user actions (undo/redo etc.) still get serviced
/// while the card is down.
///
/// @note Never called from the render thread's hot path. This is the cooperative, re-entrant pump
///       that both the FatFs `disk_read`/`disk_write` hooks and the various UI/engine idle points
///       call to keep streaming caught up.
///
/// @param max_num Maximum number of clusters to load in this call (keeps a single pump bounded).
/// @param may_process_user_actions If true, `PlaybackHandler::slowRoutine()` runs between each
///        cluster load (and on the card-unavailable early-out), giving pending user actions a chance
///        to run while the card isn't being read. Must stay false when called from a context where
///        that reentrancy isn't safe (e.g. deep inside the card-access routine).
void pump(int32_t max_num = 128, bool may_process_user_actions = false);

/// @brief Owner-mediated, coalesced entry point for `pump()` — the streaming call sites' seam.
///
/// Routes the cluster fill through the storage `Owner` (the worker fiber on Embassy), so that
/// at the async-SD ladder's final rung a mid-transfer read can suspend instead of parking the
/// executor. Single-flight coalesced (see `deluge::storage::Coalescer`): the high-frequency
/// streaming pumps collapse onto one owner fill at a time rather than flooding the fiber queue.
///
/// On legacy/host the owner runs the fill inline, so this is behaviourally identical to a direct
/// `pump(max_num, may_process_user_actions)`. Use this at the main-executor *streaming* sites;
/// offline stem-export / drain-all sites keep calling `pump()` directly (they must not dispatch
/// onto the owner from the audio interrupt-executor). See
/// docs/superpowers/specs/2026-07-17-async-sd-rung2-loader-design.md.
void request_pump(int32_t max_num = 128, bool may_process_user_actions = false);

/// @brief Whether any queued (and still-leased) cluster sits at the loader's lowest priority.
///
/// Used as the load-song yield gate: callers wait for the lowest-priority backlog to drain before
/// proceeding, so a burst of low-priority prefetch doesn't starve something more urgent.
/// @return true if the loader queue has at least one lowest-priority element.
[[nodiscard]] bool has_lowest_priority_queued();

} // namespace deluge::audio::stream::loader
