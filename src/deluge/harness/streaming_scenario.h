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

/// Host-only C-ABI bridge for the streaming-underrun harness (the Rust/Embassy `host_app`
/// BSP, `src/bsp/rust/src/scenario.rs`). Lets the harness drive a REAL song load, REAL
/// playback, and a REAL concurrent output recording headlessly — the exact application
/// entry points a user's LOAD/PLAY/RECORD button presses reach (LoadSongUI,
/// PlaybackHandler, AudioRecorder), minus the button/HID layer. None of it changes their
/// behaviour; every function is a thin, direct call into (or dispatch onto the storage
/// owner of) existing production code.
///
/// `DELUGE_HOST`-only (see sim/CMakeLists.txt's `add_compile_definitions(DELUGE_HOST)`):
/// compiled into every x86 build off this tree (deluge_host/deluge_render/deluge_loadcheck,
/// and the Rust Embassy `host_app`), never into the ARM device firmware. Unreachable — and
/// so behaviourally inert — from deluge_host/deluge_render/deluge_loadcheck's own drivers;
/// only the Rust harness calls these.
///
/// ## Why two-phase song load
/// On the Embassy BSP, `Browser::beginListing` (which `openUI(&loadSongUI)` triggers)
/// dispatches the directory listing onto the single storage-owner fiber ASYNCHRONOUSLY —
/// unlike the legacy/cooperative host_bsp (`host_render_main.cpp`'s `deluge_render_driver`),
/// where every `Owner::run` dispatch happens to run inline. So the harness must poll for the
/// listing to finish before committing the load, exactly as a real LOAD-button UI flow does
/// (open browser → listing completes asynchronously → user presses LOAD → the load itself
/// dispatches as a SEPARATE fiber job). See `deluge_scenario_begin_song_load` /
/// `deluge_scenario_song_listing_in_progress` / `deluge_scenario_commit_song_load` below.
#ifdef DELUGE_HOST

#include <cstdint>

extern "C" {

/// Point `currentSong` at `full_path` (e.g. `"SONGS/Cordae.XML"` — same shape
/// `Song::setSongFullPath` expects) and open the load browser (mirrors
/// `host_render_main.cpp`'s headless load sequence). Only DISPATCHES the async directory
/// listing (`Browser::beginListing`) onto the storage-owner fiber — poll
/// `deluge_scenario_song_listing_in_progress()` before calling
/// `deluge_scenario_commit_song_load()`. Returns false only if `currentSong` isn't set yet
/// (boot hasn't reached `deluge_boot()`'s `setupBlankSong()`).
bool deluge_scenario_begin_song_load(const char* full_path);

/// True while the async listing dispatched by `deluge_scenario_begin_song_load` is still
/// in flight.
bool deluge_scenario_song_listing_in_progress();

/// Commit the load: dispatch `LoadSongUI::performLoad()` onto the storage-owner fiber —
/// mirrors `LoadSongUI::enterKeyPress()`'s non-folder branch (same dispatch, same
/// callback), minus its `LASTOPENED` settings-file write (irrelevant to the harness).
/// `performLoad()` itself yields internally (waiting for essential-sample clusters); poll
/// `deluge_scenario_song_load_in_progress()` for completion. Returns false if the owner
/// queue rejected the dispatch (never actually ran).
bool deluge_scenario_commit_song_load();

/// True while a load committed by `deluge_scenario_commit_song_load` is still running
/// (`LoadSongUI::isLoadingSong()`).
bool deluge_scenario_song_load_in_progress();

/// Switch to the arrangement view and press PLAY (`PlaybackHandler::playButtonPressed`,
/// internal-clock branch) — starts real-time arrangement playback of the current song,
/// which is what drives the real sample-streaming loader.
void deluge_scenario_start_playback();

/// True while playback (either clock) is active (`PlaybackHandler::isEitherClockActive`).
bool deluge_scenario_playback_active();

/// Start a real-time recording of the master output to a WAV file on the card
/// (`AudioRecorder::beginOutputRecording` — the same mechanism a RECORD button press with
/// nothing armed starts). Runs concurrently with playback/streaming.
bool deluge_scenario_start_recording();

/// Debug-only: the raw `currentUIMode` bitmask (`gui/ui/ui.h`/`ui.cpp`'s `currentUIMode`
/// global). Lets a harness stuck waiting on `deluge_scenario_song_load_in_progress()`
/// distinguish WHICH phase of `LoadSongUI::performLoad()` it's parked in (e.g.
/// `UI_MODE_LOADING_SONG_ESSENTIAL_SAMPLES` vs. `..._UNESSENTIAL_SAMPLES_ARMED`) without a
/// debugger attach. Not part of the scenario's normal control flow.
uint32_t deluge_scenario_debug_ui_mode();

/// Debug-only: bit 0 = `LoadSongUI::isLoadingSong()` itself; bit 1 =
/// `getCurrentUI() == &loadSongUI` (whether the UI has transitioned away from the load
/// screen yet). Distinguishes "still genuinely loading" from "the UI object just hasn't
/// been reassigned yet" while a harness is stuck waiting for load completion.
uint32_t deluge_scenario_debug_load_state();

} // extern "C"

#endif // DELUGE_HOST
