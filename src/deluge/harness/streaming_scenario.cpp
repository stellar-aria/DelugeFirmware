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

#include "harness/streaming_scenario.h"

#ifdef DELUGE_HOST

#include "definitions_cxx.hpp"
#include "gui/ui/audio_recorder.h"
#include "gui/ui/load/load_song_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/arranger_view.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/owner.h"

extern "C" {

bool deluge_scenario_begin_song_load(const char* full_path) {
	if (currentSong == nullptr) {
		return false;
	}
	currentSong->setSongFullPath(full_path);
	return openUI(&loadSongUI);
}

bool deluge_scenario_song_listing_in_progress() {
	return LoadSongUI::isListingInProgress();
}

bool deluge_scenario_commit_song_load() {
	return deluge::storage::Owner::run([](void*) { loadSongUI.performLoad(); }, nullptr);
}

bool deluge_scenario_song_load_in_progress() {
	return loadSongUI.isLoadingSong();
}

void deluge_scenario_start_playback() {
	changeRootUI(&arrangerView);
	playbackHandler.playButtonPressed(kInternalButtonPressLatency);
}

bool deluge_scenario_playback_active() {
	return playbackHandler.isEitherClockActive();
}

bool deluge_scenario_start_recording() {
	return audioRecorder.beginOutputRecording();
}

} // extern "C"

#endif // DELUGE_HOST
