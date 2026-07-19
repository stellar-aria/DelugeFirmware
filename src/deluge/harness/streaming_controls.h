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

/// Host-only, Lens-1-only sim knobs for the streaming-underrun harness's NEGATIVE CONTROL B
/// (Task 8, `.superpowers/sdd/task-8-brief.md`): does the rung-5 streaming machinery
/// (HIGH-priority loader dispatch, cooperative recorder-drain yield) actually reduce
/// underruns, or is the sim not exercising it? Both knobs default OFF (production/current
/// behaviour); only `lens1_vt_sim`'s own `main.rs` ever calls the setters below, driven by
/// `LENS1_FORCE_NORMAL_PRIORITY`/`LENS1_DISABLE_RECORDER_YIELD` env vars, to run the SAME
/// scenario with a mechanism forced off for comparison against the mechanism-on baseline.
///
/// `DELUGE_HOST`-only (same guard as `streaming_underrun.h`/`streaming_scenario.h`): compiled
/// into every x86 build off this tree, never into the ARM device firmware. Every call site
/// (`storage/owner.cpp`, `processing/engines/audio_engine.cpp`) reads the flag through an
/// `#ifdef DELUGE_HOST`-guarded expression too, so a device/golden build's control flow is
/// completely untouched — this header doesn't even exist to it beyond an empty translation
/// unit contribution.
#ifdef DELUGE_HOST

#include <cstdint>

namespace deluge::harness {

/// When true, `storage::Coalescer::request()` demotes what would be a HIGH-priority
/// (`Owner::run_priority`) dispatch to NORMAL (`Owner::run`) instead — the "priority OFF"
/// side of negative control B. Read at `storage/owner.cpp`'s `Coalescer::request()`.
bool simForceNormalPriority();
void setSimForceNormalPriority(bool enabled);

/// When true, `audio_engine::doRecorderCardRoutines()`'s cooperative yield-to-HIGH-priority
/// check (`deluge_worker_higher_priority_waiting()`) is ignored — the recorder drain runs to
/// completion every dispatch instead of breaking early for a queued streaming read — the
/// "cooperative yield OFF" side of negative control B.
bool simRecorderYieldDisabled();
void setSimRecorderYieldDisabled(bool disabled);

} // namespace deluge::harness

extern "C" {

/// Lens-1 harness setter for [`deluge::harness::setSimForceNormalPriority`] — called from
/// `lens1_vt_sim`'s `main.rs`, never from production code.
void deluge_sim_set_force_normal_priority(bool enabled);

/// Lens-1 harness setter for [`deluge::harness::setSimRecorderYieldDisabled`] — called from
/// `lens1_vt_sim`'s `main.rs`, never from production code.
void deluge_sim_set_recorder_yield_disabled(bool disabled);

} // extern "C"

#endif // DELUGE_HOST
