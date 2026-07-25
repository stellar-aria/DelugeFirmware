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

/// @brief Host-only C-ABI diagnostic for SR3b Task 2's headline question (see
///        `.superpowers/sdd/sr3b-routing-spike.md`): CAN a still-recording sample be read back
///        through the same residency cursor real playback uses, on a target where the async Rust
///        loader is active (`host_app`)?
///
/// `deluge_harness_recorder_probe()` constructs a real `SampleRecorder`, feeds it a small amount of
/// deterministic audio (leaving it in `RecorderStatus::CAPTURING_DATA` — never finalized/closed, so
/// its Sample genuinely has no efatfs read handle, `SampleStream::efatfs_handle() == 0`, exactly the
/// "still-recording" condition `SampleStream::make_read_source()` branches on), then opens a
/// `DelugeSampleSource` cursor against that Sample's `SampleStream` and calls
/// `deluge_sample_region_acquire_ex(index=0)` — the exact same region-port entry point
/// `SampleLowLevelReader` uses for real playback (see `sample_low_level_reader.cpp`). The returned
/// `DelugeRegionState` (READY / LOADING / UNAVAILABLE) is the empirical, target-specific answer:
/// on the C-host sim (no async loader) the fiber `loader::pump()` drain reaches `RecordingReadSource`
/// and this resolves READY; on `host_app` (`async_streaming_loader` on by default) the Rust async
/// fill task owns the drain and reads via `efatfs_fs::read_at(handle=0, ...)`, which the spike's
/// static analysis predicts fails — this probe is what turns that prediction into a measurement.
///
/// `DELUGE_HOST`-only, same reach as `harness/streaming_scenario.h` (compiled into
/// deluge_host/deluge_render/deluge_loadcheck AND the Rust Embassy `host_app` build, never the ARM
/// device firmware). Leaks the `SampleRecorder`/`Sample` for the process's life — a one-shot
/// diagnostic run, not a long-lived service; acceptable here.
#ifdef DELUGE_HOST

#include <cstdint>

extern "C" {

/// @brief Run the whole probe in one call and return the resulting region state.
/// @param numChannels Recording channel count (1 or 2).
/// @param numFrames   Frames of deterministic audio to feed before probing (must be small enough to
///                    stay within the first cluster so `index=0` below is meaningful).
/// @param pumpDrainTicks How many times to call the fiber drain (`cardRoutine()`) BEFORE probing —
///                    lets the caller compare "probe immediately" vs. "probe after N drain ticks"
///                    (relevant since `async_streaming_loader`'s fill is a background task that may
///                    need scheduler ticks to run between the acquire call and a retry).
/// @return The `DelugeRegionState` for cluster index 0 (`DELUGE_REGION_READY` == 1,
///         `DELUGE_REGION_LOADING` == 2, `DELUGE_REGION_UNAVAILABLE` == 3), or 0 if the recorder
///         itself could not be set up (a harness failure, not a routing-divergence finding).
uint8_t deluge_harness_recorder_probe(uint8_t numChannels, uint32_t numFrames, uint32_t pumpDrainTicks);

/// @brief Re-query the SAME open cursor's state for index 0 without acquiring again — lets a caller
///        poll (e.g. after letting the host_app async executor run some more ticks) to see whether a
///        LOADING result ever resolves. Returns `DELUGE_REGION_UNAVAILABLE` (3) if no probe is open.
uint8_t deluge_harness_recorder_probe_poll();

/// @brief Tear down the probe's recorder/cursor (aborts the recording, closes the cursor). Safe to
///        call even if no probe is open.
void deluge_harness_recorder_probe_end();

} // extern "C"

#endif // DELUGE_HOST
