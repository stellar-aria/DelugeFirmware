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

#ifndef LIBDELUGE_STORAGE_OWNER_H
#define LIBDELUGE_STORAGE_OWNER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// True when the calling context is the storage owner (the only context permitted
/// to touch FatFS). Cooperative/host BSPs: always true (FatFS runs inline on the
/// caller). Embassy: true only on the worker fiber. Used by the migration-phase
/// diskio assert (rungs 1-4) to catch a stray non-owner FatFS caller before the
/// rung-5 yield flip would turn it into corruption.
bool deluge_storage_on_owner(void);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_STORAGE_OWNER_H
