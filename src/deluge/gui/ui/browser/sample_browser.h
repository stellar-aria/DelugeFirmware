/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include "definitions_cxx.hpp"
#include "gui/ui/browser/browser.h"
#include "hid/button.h"
#include "storage/latest_wins.h"
#include <string>

extern "C" {

#include "fatfs/ff.h"

FRESULT f_readdir_get_filepointer(DIR* dp,      /* Pointer to the open directory object */
                                  FILINFO* fno, /* Pointer to file information to return */
                                  FilePointer* filePointer);
}

class SoundDrum;
class Source;
class Sample;
class MenuItem;

namespace deluge::gui::menu_item {
class HorizontalMenu;
}

class SampleBrowser final : public Browser {
public:
	SampleBrowser();
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	bool opened() override;
	void focusRegained() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	ActionResult horizontalEncoderAction(int32_t offset) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	bool canSeeViewUnderneath() override;
	Error claimAudioFileForInstrument(bool makeWaveTableWorkAtAllCosts = false);
	Error claimAudioFileForAudioClip();
	void scrollFinished() override;
	bool importFolderAsKit();
	bool importFolderAsMultisamples();
	ActionResult timerCallback() override;
	bool claimCurrentFile(int32_t mayDoPitchDetection = 1, int32_t mayDoSingleCycle = 1, int32_t mayDoWaveTable = 1,
	                      bool loadWithoutExiting = false); // 0 means no. 1 means auto. 2 means yes definitely
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea = true) override;
	void exitAndNeverDeleteDrum();

	std::string lastFilePathLoaded{};

	// menus to open when a sample file is selected
	deluge::gui::menu_item::HorizontalMenu* parentMenuHeadingTo{nullptr};
	MenuItem* menuItemHeadingTo{nullptr};

	// ui
	UIType getUIType() override { return UIType::SAMPLE_BROWSER; }
	bool canDisplayFavourites() override { return true; }

protected:
	void enterKeyPress() override;
	void exitAction() override;
	ActionResult backButtonAction() override;
	void folderContentsReady(int32_t entryDirection) override;
	void currentFileChanged(int32_t movementDirection) override;
	void onBrowserOpened() override;

private:
	void displayCurrentFilename();

	/// A snapshot of the file to preview, captured at trigger time so the dispatched op
	/// loads the file the user pointed at even if the selection moves on before it runs
	/// (fast cursor-scroll under async dispatch).
	struct PreviewTarget {
		std::string path;
		FilePointer filePointer;
		int32_t movementDirection;
	};

	/// Trigger: snapshot the current file and dispatch its preview load+render onto the
	/// storage owner (coalesced latest-wins). Folder/no-file just clears the preview.
	void previewIfPossible(int32_t movementDirection = 1);
	/// The dispatched op: load + render the coalescer's current target, then re-dispatch
	/// if a newer target arrived while it ran. Runs on the storage owner (inline on
	/// legacy/host). `self` is the SampleBrowser.
	static void runPreviewOp(void* self);
	/// Load the target sample and render its waveform (the body previously inline in
	/// previewIfPossible); clears the preview display if nothing loaded.
	void renderPreviewForTarget(const PreviewTarget& target);
	/// Tear down any on-screen sample preview (shared by the no-file path and a failed load).
	void clearPreviewDisplay(int32_t movementDirection);

	deluge::storage::LatestWins<PreviewTarget> previewCoalescer_{};

	void audioFileIsNowSet();
	bool canImportWholeKit();
	bool loadAllSamplesInFolder(bool detectPitch, int32_t* getNumSamples, Sample*** getSortArea,
	                            bool* getDoingSingleCycle = nullptr, int32_t* getNumCharsInPrefix = nullptr);
	std::string getCurrentFilePath() override;
	void drawKeysOverWaveform();
	void autoDetectSideChainSending(SoundDrum* drum, Source* source, char const* fileName);
	void possiblySetUpBlinking();

	bool autoLoadEnabled{};

	bool currentlyShowingSamplePreview{};

	bool qwertyCurrentlyDrawnOnscreen; // This will linger as true even when qwertyVisible has been set to false
};

extern SampleBrowser sampleBrowser;
