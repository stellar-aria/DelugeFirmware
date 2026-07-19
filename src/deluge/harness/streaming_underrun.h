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

/// Host-only instrumentation for the streaming-underrun harness's PRIMARY signal: the audio
/// thread reaching a `clusters[0]->loaded == false` (or `clusters[1]`) check on a play-needed
/// path and either deferring (WAIT) or dropping (UNASSIGN) the voice as a result. Not a
/// fault, not a blocking wait — just the exact moment playback discovers the cluster it needs
/// isn't ready yet.
///
/// `DELUGE_HOST`-only (see `sim/CMakeLists.txt`'s `add_compile_definitions(DELUGE_HOST)`,
/// same guard `harness/streaming_scenario.h` uses): compiled into every x86 build off this
/// tree, never into the ARM device firmware. Every call site (`model/voice/voice_sample.cpp`,
/// `model/sample/sample_low_level_reader.cpp`) is itself wrapped in `#ifdef DELUGE_HOST` (not
/// just relying on these being no-ops), so a device/golden build doesn't even see the calls —
/// byte-for-byte unchanged from before this file existed.
#ifdef DELUGE_HOST

#include <cstdint>

namespace deluge::harness {

/// Count one WAIT-class underrun miss: `VoiceSample::attemptLateSampleStart` found
/// `clusters[0]` (or, having found `clusters[0]` loaded, `clusters[1]`) not yet loaded on a
/// play-needed path and is about to return `LateStartAttemptStatus::WAIT` (defer, come back
/// later) rather than starting playback. Called from exactly one place — the shared
/// fall-through just above the `WAIT` return — since both of the function's `loaded` checks
/// converge there.
void noteUnderrunWait();

/// Count one UNASSIGN-class underrun miss: a voice found the Cluster it needs isn't loaded on
/// a path that returns `false` all the way up to `Voice::render`'s `goto instantUnassign` — a
/// dropped voice, not just a deferral. Two call sites converge here, both the same severity:
///   - `VoiceSample::stopReadingFromCache`: the repitch-cache-stop path (only reached by
///     voices actively using the timestretch/repitch `SampleCache`).
///   - `SampleLowLevelReader::moveOnToNextCluster` (Task 7 addition): ordinary (non-cache,
///     non-time-stretch) forward playback crossing a Cluster boundary mid-stream and finding
///     the next Cluster not loaded — the common-case sustained-streaming underrun, and the one
///     most voices actually hit (most playback never uses the repitch cache at all). Without
///     this site the harness's counters could stay at zero even under genuine SD-latency
///     starvation of ordinary sustained streaming, because the miss would silently unassign
///     the voice via a wholly uninstrumented path.
void noteUnderrunUnassign();

} // namespace deluge::harness

extern "C" {

/// Total WAIT-class underrun misses observed since boot (see
/// `deluge::harness::noteUnderrunWait`). Monotonic — callers baseline-subtract across a
/// window, same pattern as `sd.rs`'s `on_fiber_reads`/`on_fiber_writes`.
uint64_t deluge_sim_underrun_wait_count();

/// Total UNASSIGN-class underrun misses observed since boot (see
/// `deluge::harness::noteUnderrunUnassign`).
uint64_t deluge_sim_underrun_unassign_count();

} // extern "C"

#endif // DELUGE_HOST
