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

#include "libdeluge/worker.h"

namespace deluge::storage {

/// @brief The single storage owner: routes FatFS-touching operations through
/// one serialized context so they never re-enter non-reentrant FatFS.
///
/// On Embassy the op runs on the worker fiber (so its `yield()`s / mid-transfer
/// suspends don't stall the caller); on legacy/host it runs inline on the caller's
/// stack. Fire-and-forget: `run` returns immediately (Embassy) / on completion
/// (inline). Post-op work must live inside the op. `await` (a blocking, result-
/// returning variant) is a later rung; this seam is `run`-only.
///
/// This is the one enforced entry point for the single-owner discipline the
/// async-SD reentrancy proof (tests/fatfs_stress/RESULTS.md) showed is
/// load-bearing. See docs/superpowers/specs/2026-07-17-async-sd-staging-ladder-roadmap.md.
struct Owner {
	/// Queue `fn(ctx)` on the owner. C-ABI-shaped for existing call sites.
	static void run(void (*fn)(void*), void* ctx);
};

} // namespace deluge::storage
