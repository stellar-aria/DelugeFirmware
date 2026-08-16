/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "gui/ui/load/load_ui.h"
#include "hid/button.h"
#include "storage/owner.h"

class LoadSongUI final : public LoadUI {
public:
	LoadSongUI();
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult timerCallback() override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	void graphicsRoutine() override {}
	void scrollFinished() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	bool opened() override;
	void selectEncoderAction(int8_t offset) override;
	void queueLoadNextSongIfAvailable(int8_t offset);
	void performLoad();
	void displayLoopsRemainingPopup();
	bool isLoadingSong();

	bool deletedPartsOfOldSong{};

	// ui
	UIType getUIType() override { return UIType::LOAD_SONG; }
	bool canDisplayFavourites() override { return true; }

protected:
	void displayText(bool blinkImmediately = false) override;
	void enterKeyPress() override;
	void folderContentsReady(int32_t entryDirection) override;
	void currentFileChanged(int32_t movementDirection) override;
	void exitAction() override;
	void onBrowserOpened() override;

private:
	/// @brief Render the highlighted song's saved pad layout onto the grid.
	///
	/// Reads the song file, so it must run on the storage owner. Called from the scroll path (the
	/// interaction tier), where an off-owner read is refused outright, so this dispatches onto the owner
	/// via @ref previewCoalescer_ and returns; the render happens in @ref drawSongPreviewImpl.
	/// @param toStore true to render into PadLEDs::imageStore (behind a scroll), false for the live image.
	/// @return true if the render happened inline (we were already on the owner); false if it was
	///         dispatched, in which case `imageStore` is NOT yet filled and a scroll that reads it must
	///         wait for @ref previewTrampoline.
	[[nodiscard]] bool drawSongPreview(bool toStore = true);

	/// @brief The actual preview read + render. ONLY valid on the storage owner.
	/// @param toStore As @ref drawSongPreview.
	void drawSongPreviewImpl(bool toStore);

	/// @brief `Owner::run` trampoline for @ref drawSongPreviewImpl, using @ref previewToStore_.
	static void previewTrampoline(void* self);

	void displayArmedPopup();

	/// Coalesces scroll-driven preview requests: fast scrolling would otherwise overrun the owner's
	/// 4-deep queue, and only the newest preview is worth rendering anyway.
	deluge::storage::Coalescer previewCoalescer_{};

	/// `toStore` argument for the dispatched @ref drawSongPreviewImpl (the trampoline takes no args).
	bool previewToStore_{true};

	/// Set on every request, cleared as the render begins. Still set when the render finishes means the
	/// selection moved again mid-read and the coalescer dropped that request, so a catch-up is dispatched.
	bool previewPending_{false};

	/// @brief Scroll direction owed to a preview whose render was dispatched, or 0 for none.
	///
	/// The scroll-in reads `PadLEDs::imageStore`, so it cannot start until the render has actually filled
	/// it. When @ref drawSongPreview dispatches instead of rendering inline, the direction is parked here
	/// and @ref previewTrampoline starts the scroll once the fill is done. Starting it eagerly is what
	/// left the first two columns showing the PREVIOUS entry: `setupScroll` ticks once itself and
	/// `currentFileChanged` ticked again, so two columns were copied out of an unfilled store.
	int32_t pendingScrollDirection_{0};

	/// @brief Set up and start the scroll-in from `imageStore`. Call only once it is filled.
	void beginPreviewScrollIn(int32_t movementDirection);

	bool performingLoad;
	bool scrollingIntoSlot{};
	bool qwertyCurrentlyDrawnOnscreen;
	void doQueueLoadNextSongIfAvailable(int8_t offset);
	// int32_t findNextFile(int32_t offset);
	void exitThisUI();
	void exitActionWithError();
	void performLoadFixedSM();
};
extern LoadSongUI loadSongUI;

extern char loopsRemainingText[];
