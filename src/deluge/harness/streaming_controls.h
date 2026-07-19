/*
 * Copyright © 2026 Synthstrom Audible Limited
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

/// @brief Host-only sim test controls for the streaming-underrun harness.
///
/// Each knob lets the harness force a rung-5 streaming mechanism off — HIGH-priority loader
/// dispatch, or cooperative recorder-drain yield — so a scenario can be run once with the
/// mechanism on and once with it off, to check whether the mechanism is actually the thing
/// reducing underruns. Both knobs default `false` (production behaviour, mechanisms fully
/// enabled); only `lens1_vt_sim`'s `main.rs` calls the setters below, driven by the
/// `LENS1_FORCE_NORMAL_PRIORITY`/`LENS1_DISABLE_RECORDER_YIELD` env vars.
///
/// @note `DELUGE_HOST`-only (same guard as `streaming_underrun.h`/`streaming_scenario.h`):
///       compiled into every x86 build off this tree, never into the ARM device firmware. Every
///       call site (`storage/owner.cpp`, `processing/engines/audio_engine.cpp`) reads the flag
///       through an `#ifdef DELUGE_HOST`-guarded expression too, so a device/golden build's
///       control flow is completely untouched — this header doesn't even exist to it beyond an
///       empty translation unit contribution.
#ifdef DELUGE_HOST

#include <cstdint>

namespace deluge::harness {

/// @brief Whether `storage::Coalescer::request()` demotes what would be a HIGH-priority
///        (`Owner::run_priority`) dispatch to NORMAL (`Owner::run`) instead.
///
/// Sim-only test control (see file-level comment). Read at `storage/owner.cpp`'s
/// `Coalescer::request()`.
/// @return `true` if HIGH-priority dispatch is currently forced down to NORMAL.
bool simForceNormalPriority();

/// @brief Set whether HIGH-priority loader dispatch is forced down to NORMAL. See
///        simForceNormalPriority().
/// @param enabled `true` to force NORMAL-priority dispatch; `false` for normal (HIGH-priority)
///                behaviour.
void setSimForceNormalPriority(bool enabled);

/// @brief Whether `audio_engine::doRecorderCardRoutines()`'s cooperative yield-to-HIGH-priority
///        check (`deluge_worker_higher_priority_waiting()`) is ignored, so the recorder drain
///        runs to completion every dispatch instead of breaking early for a queued streaming
///        read.
///
/// Sim-only test control (see file-level comment).
/// @return `true` if the cooperative yield check is currently disabled.
bool simRecorderYieldDisabled();

/// @brief Set whether the recorder-drain cooperative yield check is disabled. See
///        simRecorderYieldDisabled().
/// @param disabled `true` to skip the yield check (drain-to-completion); `false` for normal
///                 behaviour.
void setSimRecorderYieldDisabled(bool disabled);

} // namespace deluge::harness

extern "C" {

/// @brief C-linkage wrapper for deluge::harness::setSimForceNormalPriority(), called from
///        `lens1_vt_sim`'s `main.rs`, never from production code.
/// @param enabled Forwarded to deluge::harness::setSimForceNormalPriority().
void deluge_sim_set_force_normal_priority(bool enabled);

/// @brief C-linkage wrapper for deluge::harness::setSimRecorderYieldDisabled(), called from
///        `lens1_vt_sim`'s `main.rs`, never from production code.
/// @param disabled Forwarded to deluge::harness::setSimRecorderYieldDisabled().
void deluge_sim_set_recorder_yield_disabled(bool disabled);

} // extern "C"

#endif // DELUGE_HOST
