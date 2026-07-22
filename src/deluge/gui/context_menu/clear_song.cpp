/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
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

#include "gui/context_menu/clear_song.h"
#include "gui/l10n/l10n.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/led/indicator_leds.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "playback/mode/arrangement.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/owner.h"

extern void setUIForLoadedSong(Song* song);
extern void deleteOldSongBeforeLoadingNew();
namespace deluge::gui::context_menu {
ClearSong clearSong{};

namespace {
/// The dispatched op for ClearSong::acceptCurrentOption(): runs "Create New Song" (default
/// synth preset via ensureAtLeastOneSessionClip(), then loadAllSamples()) on the storage
/// worker so it doesn't block the executor (bug B6). Unlike the Synth/Kit sample-browser
/// context menus, there is no failure branch to mirror here: the pre-existing synchronous
/// code already discarded ensureAtLeastOneSessionClip()'s bool and always returned true, so
/// this is the same unconditional sequence, just moved off the executor.
///
/// acceptCurrentOption() sets currentUIMode to a LOADING sentinel (outside
/// ContextMenu::buttonAndPadActionUIModes) before dispatching, and this op only resets it to
/// UI_MODE_NONE at the very end. Why that matters: nullifyUIs() below drops numUIsOpen to 0
/// for this op's whole in-flight window (same as LoadSongUI::performLoad()), during which
/// getCurrentUI() falls back to lastUIBeforeNullifying — the (now-stale) clearSong singleton
/// (ui.cpp's documented "ugly work-around to stop everything breaking"). buttons.cpp routes
/// button presses to getCurrentUI()->buttonAction() unconditionally. Without the mode gate, a
/// BACK press in that window would reach ContextMenu::buttonAction() -> close() ->
/// closeUI(&clearSong), whose loop never runs with numUIsOpen==0 and then indexes
/// uiNavigationHierarchy[-2] (OOB/UB); a repeat SELECT_ENC would re-dispatch this op
/// mid-flight. The mode gate (isUIModeWithinRange() returns false) makes both no-ops instead.
void runAcceptOp(void*) {
	if (playbackHandler.playbackState
	    && (playbackHandler.isInternalClockActive() || currentPlaybackMode == &arrangement)) {

		playbackHandler.endPlayback();
	}

	actionLogger.deleteAllLogs();

	nullifyUIs();
	if (!playbackHandler.isEitherClockActive()) {
		deleteOldSongBeforeLoadingNew();
	}
	else {
		AudioEngine::songSwapAboutToHappen();
	}

	void* songMemory = deluge::memory::alloc_fast(sizeof(Song)); // TODO: error checking
	preLoadedSong = new (songMemory) Song();
	preLoadedSong->paramManager.setupUnpatched(); // TODO: error checking
	GlobalEffectable::initParams(&preLoadedSong->paramManager);
	preLoadedSong->setupDefault();

	Song* toDelete = currentSong;

	preLoadedSong->ensureAtLeastOneSessionClip(); // Will load a synth preset from SD card

	playbackHandler.doSongSwap(playbackHandler.isEitherClockActive());
	if (toDelete) {
		void* toDealloc = dynamic_cast<void*>(toDelete);
		toDelete->~Song();
		delugeDealloc(toDealloc);
	}

	audioFileManager.deleteAnyTempRecordedSamplesFromMemory();

	// If for some reason the default synth preset included a sample which needs loading, and somehow there wasn't
	// enough RAM to load it before, do it now.
	currentSong->loadAllSamples();

	setUIForLoadedSong(currentSong);
	currentUIMode = UI_MODE_NONE;

	display->removeWorkingAnimation();
}
} // namespace

char const* ClearSong::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_CLEAR_SONG_QMARK);
}

std::span<char const*> ClearSong::getOptions() {
	using enum l10n::String;
	static char const* options[] = {l10n::get(STRING_FOR_OK)};
	return {options, 1};
}

void ClearSong::focusRegained() {
	ContextMenu::focusRegained();

	// TODO: Switch a bunch of LEDs off (?)

	indicator_leds::setLedState(IndicatorLED::SAVE, false);
	indicator_leds::setLedState(IndicatorLED::SYNTH, false);
	indicator_leds::setLedState(IndicatorLED::KIT, false);

	indicator_leds::setLedState(IndicatorLED::CROSS_SCREEN_EDIT, false);
	indicator_leds::setLedState(IndicatorLED::CLIP_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::SESSION_VIEW, false);
	indicator_leds::setLedState(IndicatorLED::SCALE_MODE, false);

	indicator_leds::blinkLed(IndicatorLED::LOAD);
	indicator_leds::blinkLed(IndicatorLED::BACK);
}

bool ClearSong::acceptCurrentOption() {
	// Dispatch onto the storage owner so the SD-card work inside
	// ensureAtLeastOneSessionClip()/loadAllSamples() (deep inside runAcceptOp) doesn't block
	// the executor (bug B6). Fire-and-forget, so this can't honestly report the eventual
	// success/failure back to ContextMenu::buttonAction() synchronously — always return true
	// (never trigger its synchronous close()); runAcceptOp() does the whole sequence itself,
	// including the UI teardown (there is no failure branch to preserve — see runAcceptOp()'s
	// comment).
	//
	// Close the buttonAndPadActionUIModes gate here, before dispatch, not inside the op: this
	// must happen before any yield, with no window where a BACK/SELECT_ENC press could reach
	// ContextMenu::buttonAction() while currentUIMode still permits it. See runAcceptOp()'s
	// comment for what that would corrupt.
	currentUIMode = UI_MODE_LOADING_SONG_ESSENTIAL_SAMPLES;
	if (!deluge::storage::Owner::run_or_inline(&runAcceptOp, nullptr)) {
		// Dropped dispatch (Embassy worker ring full): runAcceptOp never runs, and it is the
		// only thing that resets currentUIMode — leaving the UI permanently gated (reboot to
		// recover). Release the gate here so the menu stays usable, matching the drop-reset in
		// Slicer::doSlice / InstrumentClipView's randomize dispatch.
		currentUIMode = UI_MODE_NONE;
	}
	return true;
}
} // namespace deluge::gui::context_menu
