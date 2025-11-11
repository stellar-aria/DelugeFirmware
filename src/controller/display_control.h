/*
 * Copyright © 2025 Synthstrom Audible Limited
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

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void display_init(void);
void display_update(const uint8_t* buffer, uint16_t size);
void display_clear(void);
bool display_has_oled(void);

void pad_led_set_rgb(uint8_t col, uint8_t row, uint8_t r, uint8_t g, uint8_t b);
void pad_led_clear_all(void);

void hardware_led_set(uint8_t led_index, bool on);

void cv_set(uint8_t channel, uint16_t value);
void gate_set(uint8_t channel, bool on);

#ifdef __cplusplus
}
#endif
