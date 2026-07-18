/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
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
#include "sync/storage_op.h"

namespace {
// The permit state, owned entirely by this module. Was the file-scope global
// allowSomeUserActionsEvenWhenInCardRoutine (now deleted).
bool g_user_actions_permitted = false;
} // namespace

namespace deluge::sync {

StorageOp::StorageOp() : prev_permit_{g_user_actions_permitted} {
	g_user_actions_permitted = true;
}
StorageOp::~StorageOp() {
	g_user_actions_permitted = prev_permit_;
}

bool user_actions_permitted() {
	return g_user_actions_permitted;
}

} // namespace deluge::sync
