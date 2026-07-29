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
#include "fatfs/ff.h"
#include "gui/ui/audio_recorder.h"
#include "gui/ui/load/load_song_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/arranger_view.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/stem_export/stem_export.h"
#include "storage/owner.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <strings.h> // strcasecmp
#include <sys/stat.h>
#include <unistd.h>

namespace {

/// Set false at the start of every deluge_scenario_start_stem_export() dispatch,
/// true only once the dispatched worker-fiber closure itself returns. See
/// deluge_scenario_stem_export_done()'s header doc for why this can't just read
/// `stemExport.processStarted` directly.
bool g_stem_export_dispatch_finished = false;

/// Same start/finished-latch shape as g_stem_export_dispatch_finished, for
/// deluge_scenario_start_song_load()/deluge_scenario_song_load_begin_done()
/// below.
bool g_song_load_begin_finished = false;
bool g_song_load_begin_result = false;
std::string g_song_load_begin_path;

} // namespace

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

void deluge_scenario_start_song_load(const char* full_path) {
	// deluge_scenario_begin_song_load() (above) is called SYNCHRONOUSLY by its
	// existing callers (e.g. the streaming-underrun harness's scenario.rs), which
	// is fine there because THEY only ever reach it from a context that either IS
	// the worker fiber already or never touches the newer storage paths this one
	// does. This scenario's own caller is neither: it runs on a plain Rust task,
	// off the worker fiber, and `openUI(&loadSongUI)`'s opened() chain
	// (LoadSongUI::opened -> FavouritesManager::loadFavouritesBank ->
	// StorageManager::fileExists) does a real storage read. Under `sim_latency`
	// (on by default for golden_vt_render) that read needs a `Timer` fired by a
	// SEPARATE Embassy task (`sim_latency::pump`) to ever resolve — off-fiber it
	// runs through a non-yielding `embassy_futures::block_on` that can never let
	// that task get polled, livelocking the whole process. So dispatch
	// deluge_scenario_begin_song_load() itself onto the worker fiber (same
	// pattern as deluge_scenario_start_stem_export() above), rather than calling
	// it directly. deluge_scenario_begin_song_load() is UNCHANGED and still
	// callable directly by any context that's actually safe doing so.
	g_song_load_begin_finished = false;
	g_song_load_begin_result = false;
	g_song_load_begin_path = full_path;
	bool dispatched = deluge::storage::Owner::run(
	    [](void*) {
		    g_song_load_begin_result = deluge_scenario_begin_song_load(g_song_load_begin_path.c_str());
		    g_song_load_begin_finished = true;
	    },
	    nullptr);
	if (!dispatched) {
		// Owner queue rejected the dispatch — nothing will ever flip the latch.
		g_song_load_begin_result = false;
		g_song_load_begin_finished = true;
	}
}

bool deluge_scenario_song_load_begin_done() {
	return g_song_load_begin_finished;
}

bool deluge_scenario_song_load_begin_ok() {
	return g_song_load_begin_result;
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

uint32_t deluge_scenario_debug_ui_mode() {
	return currentUIMode;
}

uint32_t deluge_scenario_debug_load_state() {
	// bit 0: isLoadingSong() itself (the OR of the two conditions below).
	// bit 1: getCurrentUI() == &loadSongUI (the UI object hasn't transitioned away yet).
	uint32_t bits = 0;
	if (loadSongUI.isLoadingSong()) {
		bits |= 1u;
	}
	if (getCurrentUI() == &loadSongUI) {
		bits |= 2u;
	}
	return bits;
}

void deluge_scenario_start_stem_export(int32_t mode) {
	StemExportType stemMode = static_cast<StemExportType>(mode);
	stemExport.renderOffline = true;
	stemExport.exportToSilence = true;
	stemExport.exportMixdown = (stemMode == StemExportType::MIXDOWN);
	// A MIXDOWN is the full master output, so it must include the song-level FX — see
	// host_render_main.cpp's deluge_render_driver() (the field-for-field reference this
	// mirrors) for the mechanical reason: the offline render path only feeds a recorder
	// whose channel is OFFLINE_OUTPUT, which is selected only when includeSongFX is set.
	if (stemMode == StemExportType::MIXDOWN) {
		stemExport.includeSongFX = true;
	}
	g_stem_export_dispatch_finished = false;
	// Dispatch the whole export onto the storage-owner worker fiber, exactly as
	// host_render_main.cpp's deluge_render_driver does — StemExport::startStemExportProcess
	// itself performs this identical Owner::run dispatch of runStemExportProcess();
	// it's reproduced here (rather than calling startStemExportProcess() directly)
	// only so this wrapper can flip g_stem_export_dispatch_finished once the closure
	// returns, without racing stemExport.processStarted (see the header doc).
	deluge::storage::Owner::run(
	    [](void* p) {
		    stemExport.runStemExportProcess(static_cast<StemExportType>(reinterpret_cast<uintptr_t>(p)));
		    g_stem_export_dispatch_finished = true;
	    },
	    reinterpret_cast<void*>(static_cast<uintptr_t>(mode)));
}

bool deluge_scenario_stem_export_done() {
	return g_stem_export_dispatch_finished;
}

void deluge_scenario_copy_stems_out(const char* out_dir) {
	const char* folder = stemExport.lastFolderNameForStemExport.c_str();
	if (folder == nullptr || folder[0] == '\0') {
		fprintf(stderr, "[golden-scenario] no stem folder recorded; nothing to copy\n");
		return;
	}

	mkdir(out_dir, 0755); // best-effort; ignore EEXIST

	DIR dir;
	if (f_opendir(&dir, folder) != FR_OK) {
		fprintf(stderr, "[golden-scenario] cannot open stem folder '%s' in image\n", folder);
		return;
	}

	static char buf[65536];
	int count = 0;
	FILINFO fno;
	while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
		if (fno.fattrib & AM_DIR) {
			continue;
		}
		size_t len = strlen(fno.fname);
		if (len < 4 || strcasecmp(fno.fname + len - 4, ".WAV") != 0) {
			continue;
		}

		char src[600];
		char dst[700];
		snprintf(src, sizeof src, "%s/%s", folder, fno.fname);
		snprintf(dst, sizeof dst, "%s/%s", out_dir, fno.fname);

		FIL fil;
		if (f_open(&fil, src, FA_READ) != FR_OK) {
			fprintf(stderr, "[golden-scenario] cannot read '%s'\n", src);
			continue;
		}
		int ofd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (ofd < 0) {
			fprintf(stderr, "[golden-scenario] cannot create '%s'\n", dst);
			f_close(&fil);
			continue;
		}
		UINT br = 0;
		while (f_read(&fil, buf, sizeof buf, &br) == FR_OK && br > 0) {
			size_t off = 0;
			while (off < br) {
				ssize_t w = write(ofd, buf + off, br - off);
				if (w <= 0) {
					break;
				}
				off += (size_t)w;
			}
		}
		close(ofd);
		f_close(&fil);
		count++;
		fprintf(stderr, "[golden-scenario]   %s\n", dst);
	}
	f_closedir(&dir);
	fprintf(stderr, "[golden-scenario] copied %d stem(s) to %s\n", count, out_dir);
}

} // extern "C"

#endif // DELUGE_HOST
