/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "gui/ui/ui.h"
#include "hid/button.h"
#include "storage/latest_wins.h"

#define SLICER_MODE_REGION 0
#define SLICER_MODE_MANUAL 1
#define MAX_MANUAL_SLICES 64

struct SliceItem {
	int32_t startPos;
	int32_t transpose;
};

class Slicer final : public UI {
public:
	Slicer() { oledShowsUIUnderneath = true; }

	void focusRegained() override;
	bool canSeeViewUnderneath() override { return false; }
	void selectEncoderAction(int8_t offset) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;

	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) override;
	void graphicsRoutine() override;
	ActionResult horizontalEncoderAction(int32_t offset) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;

	void stopAnyPreviewing();
	/// @brief Trigger a pad-hold/tap audition: snapshot the slice/on-off request and dispatch it
	///        (coalesced latest-wins) onto the storage owner.
	///
	/// The load (when the underlying sample isn't already resident) and the audition note both
	/// happen inside the dispatched op, never here.
	/// @param startPoint Sample-frame position where the audition slice starts.
	/// @param endPoint   Sample-frame position where the audition slice ends; -1 leaves the
	///                   current end position unchanged.
	/// @param transpose  Transpose to apply to the audition note.
	/// @param on         Nonzero to start/continue the audition, zero to silence it.
	void preview(int64_t startPoint, int64_t endPoint, int32_t transpose, int32_t on);

	int32_t numManualSlice{};
	int32_t currentSlice{};
	int32_t slicerMode{};
	SliceItem manualSlicePoints[MAX_MANUAL_SLICES]{};

	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;

	int16_t numClips{};

	// ui
	UIType getUIType() override { return UIType::SLICER; }

private:
	// 7SEG Only
	void redraw();

	/// @brief Create the sliced drums/clips, loading the underlying sample if it isn't already
	///        resident.
	///
	/// @param sliceCount How many slices/drums to create — `numClips` (REGION) or
	///                   `numManualSlice` (MANUAL) at commit time, passed explicitly (not read off
	///                   `this->numClips`) so a concurrent encoder turn during the dispatched op's
	///                   SD-yield can't desync the slice math from `commitSlice`'s paint loop. See
	///                   `SliceCommitTarget`.
	void doSlice(int32_t sliceCount);

	/// @brief A pad-hold/tap audition request, snapshotted at trigger time.
	///
	/// All fields are plain values (no pointers into live UI state) — `startPoint`/`endPoint`/
	/// `transpose` are already read out of `manualSlicePoints[]` by `padAction` before this is
	/// built, and `on` is the press/release flag for this event.
	struct PreviewTarget {
		int64_t startPoint;
		int64_t endPoint;
		int32_t transpose;
		int32_t on;
	};

	/// @brief The dispatched op: loads the sliced sample if it isn't already resident and
	///        sounds/silences the audition note for the coalescer's current target.
	///
	/// Re-dispatches itself if a newer target arrived while it ran. Runs on the storage owner
	/// (inline on legacy/host).
	/// @param self The `Slicer` this op is running for.
	static void runPreviewOp(void* self);
	/// @brief The load-then-audition body for a single preview target.
	/// @param target The audition request to act on.
	void previewForTarget(const PreviewTarget& target);

	deluge::storage::LatestWins<PreviewTarget> previewCoalescer_{};

	/// @brief Snapshot of the SELECT_ENC "commit slice" gesture, captured in `buttonAction()` at
	///        dispatch time.
	///
	/// Captured before the dispatched op's storage-owner yield inside `doSlice()`'s `loadFile()`
	/// calls. `sliceCount` is `numClips` (REGION mode) or `numManualSlice` (MANUAL mode) at press
	/// time; `isManual`/`manualPoints` are what `commitSlice()`'s MANUAL-mode per-drum
	/// start/end/transpose paint loop needs. All plain data — no pointers into
	/// `manualSlicePoints[]`/`numManualSlice`/`numClips` survive past dispatch, so a concurrent
	/// encoder turn or pad press during the op's yield can't desync what gets sliced from what
	/// gets painted onto the drums `doSlice()` creates.
	struct SliceCommitTarget {
		bool isManual;
		int32_t sliceCount;
		SliceItem manualPoints[MAX_MANUAL_SLICES];
	};

	/// @brief The dispatched op for `buttonAction()`'s SELECT_ENC "commit slice" gesture.
	///
	/// Runs `doSlice()` (which loads the sliced sample(s) if not already resident, off the
	/// executor) and, for MANUAL mode, the per-drum paint loop. The two run together because the
	/// loop indexes drums `doSlice()` creates, so it must run after `doSlice()` completes, and
	/// `doSlice()`'s own load can yield partway through. Reads only `self->pendingSliceTarget_`
	/// (the dispatch-time snapshot), never live UI members.
	/// @param self The `Slicer` this op is running for.
	static void runDoSliceOp(void* self);
	/// @brief The commit body for `buttonAction()`'s SELECT_ENC case.
	///
	/// Resets `currentUIMode` back to `UI_MODE_NONE` on every exit path (success or `doSlice()`'s
	/// internal load-failure branch) to release the gate `buttonAction()` closes before dispatch.
	/// @param target The dispatch-time snapshot of the commit-slice gesture.
	void commitSlice(const SliceCommitTarget& target);

	SliceCommitTarget pendingSliceTarget_{};
};

extern Slicer slicer;
