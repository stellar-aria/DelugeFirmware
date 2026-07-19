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

#include "harness/streaming_controls.h"

#ifdef DELUGE_HOST

#include <atomic>

namespace deluge::harness {

namespace {
// Relaxed atomics: plain sim-config flags read/written from the same single-threaded host
// executor (Lens 1) or the main+fiber contexts (Lens 2) — same rationale as
// `streaming_underrun.cpp`'s counters, no ordering requirement against other memory ops.
std::atomic<bool> forceNormalPriority{false};
std::atomic<bool> recorderYieldDisabled{false};
} // namespace

bool simForceNormalPriority() {
	return forceNormalPriority.load(std::memory_order_relaxed);
}

void setSimForceNormalPriority(bool enabled) {
	forceNormalPriority.store(enabled, std::memory_order_relaxed);
}

bool simRecorderYieldDisabled() {
	return recorderYieldDisabled.load(std::memory_order_relaxed);
}

void setSimRecorderYieldDisabled(bool disabled) {
	recorderYieldDisabled.store(disabled, std::memory_order_relaxed);
}

} // namespace deluge::harness

extern "C" {

void deluge_sim_set_force_normal_priority(bool enabled) {
	deluge::harness::setSimForceNormalPriority(enabled);
}

void deluge_sim_set_recorder_yield_disabled(bool disabled) {
	deluge::harness::setSimRecorderYieldDisabled(disabled);
}

} // extern "C"

#endif // DELUGE_HOST
