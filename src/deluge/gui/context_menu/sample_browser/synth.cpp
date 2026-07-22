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

#include "gui/context_menu/sample_browser/synth.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/ui/browser/sample_browser.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "processing/sound/sound.h"
#include "processing/source.h"
#include "storage/file_item.h"
#include "storage/owner.h"
#include "util/functions.h"
#include <cstdint>

namespace deluge::gui::context_menu::sample_browser {
Synth synth{};

namespace {
/// The dispatched op for Synth::acceptCurrentOption: runs the browser load fn for `option` (the
/// currentOption snapshotted at dispatch time, NOT re-read from `synth.currentOption` — on
/// Embassy this runs on the worker fiber after buttonAction() has returned and the menu is still
/// open, so the user could have turned the encoder and changed currentOption in the meantime).
/// On failure, closes just the ContextMenu (the browser fn already closes everything, including
/// this menu, on success via its own close()/exitAndNeverDeleteDrum()).
void runAcceptOp(void* ctx) {
	auto option = static_cast<int32_t>(reinterpret_cast<intptr_t>(ctx));
	bool success = false;
	switch (option) {
	case 0: // Multisamples
		success = sampleBrowser.importFolderAsMultisamples();
		break;
	case 1: // Basic
		success = sampleBrowser.claimCurrentFile(0, 0, 0);
		break;
	case 2: // Single-cycle
		success = sampleBrowser.claimCurrentFile(2, 2, 1);
		break;
	case 3:                                                // WaveTable
		success = sampleBrowser.claimCurrentFile(1, 1, 2); // Could probably also be 0,0,2
		break;
	default:
		__builtin_unreachable();
	}
	if (!success) {
		synth.close();
	}
}
} // namespace

char const* Synth::getTitle() {
	using enum l10n::String;
	return l10n::get(STRING_FOR_LOAD_FILES);
}

std::span<char const*> Synth::getOptions() {
	using enum l10n::String;
	static char const* options[] = {
	    l10n::get(STRING_FOR_MULTISAMPLES), //<
	    l10n::get(STRING_FOR_BASIC),        //<
	    l10n::get(STRING_FOR_SINGLE_CYCLE), //<
	    l10n::get(STRING_FOR_WAVETABLE),    //<
	};
	return {options, 4};
}

bool Synth::isCurrentOptionAvailable() {

	// Multisamples (load entire folder and auto-detect ranges). Will delete all previous Ranges.
	if (currentOption == 0) {
		return (soundEditor.currentSound->getSynthMode() != SynthMode::RINGMOD);
	}

	// Apart from that option, none of the other ones are valid if currently sitting on a folder-name.
	if (sampleBrowser.getCurrentFileItem()->isFolder) {
		return false;
	}

	switch (currentOption) {
	case 1:
		// "Basic" Sample - unavailable if ringmod.
		return (soundEditor.currentSound->getSynthMode() != SynthMode::RINGMOD);

	case 3:
		// WaveTable
		// No break - return true.

	case 2:
		// Single-cycle - available even if we're locked (because of there being other Ranges) to this happening either
		// in Sample or WaveTable mode. Although, the user could get an error after selecting the option - e.g. if
		// we're locked to WaveTable mode, they've selected single-cycle, and it realises it can't do this
		// combination of things because it's a stereo file.
		return true;

	default:
		__builtin_unreachable();
		return false;
	}
}

bool Synth::acceptCurrentOption() {
	// Dispatch onto the storage owner so pitch-detection (deep inside claimCurrentFile()/
	// importFolderAsMultisamples()) doesn't block the executor on SD reads (bug B6). The load is
	// now fire-and-forget, so this can't honestly report the eventual success/failure back to
	// ContextMenu::buttonAction() synchronously — always return true (never trigger its
	// synchronous close()) and do the close-on-failure ourselves inside the op once the real
	// result is known. Snapshot currentOption now: it must NOT be re-read inside the op (see
	// runAcceptOp's comment).
	deluge::storage::Owner::run_or_inline(&runAcceptOp, reinterpret_cast<void*>(static_cast<intptr_t>(currentOption)));
	return true;
}

ActionResult Synth::padAction(int32_t x, int32_t y, int32_t on) {
	return sampleBrowser.padAction(x, y, on);
}

bool Synth::canSeeViewUnderneath() {
	return sampleBrowser.canSeeViewUnderneath();
}
} // namespace deluge::gui::context_menu::sample_browser
