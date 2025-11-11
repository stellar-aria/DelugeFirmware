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

// Event types
typedef enum {
	EVENT_TYPE_BUTTON_PRESS = 0x01,
	EVENT_TYPE_BUTTON_RELEASE = 0x02,
	EVENT_TYPE_ENCODER = 0x03,
	EVENT_TYPE_PAD_PRESS = 0x04,
	EVENT_TYPE_PAD_RELEASE = 0x05,
	EVENT_TYPE_PAD_VELOCITY = 0x06,
	EVENT_TYPE_MIDI_IN = 0x10,  // MIDI input from DIN ports
	EVENT_TYPE_MIDI_OUT = 0x11, // MIDI output to DIN ports
	EVENT_TYPE_CV_GATE = 0x20,  // CV/Gate output event
} EventType;

// Hardware event structure
typedef struct {
	EventType type;
	uint8_t id;         // Button/encoder/pad identifier or MIDI status byte
	uint16_t value;     // Value (encoder delta, velocity, MIDI data bytes, etc.)
	uint32_t timestamp; // Timestamp in milliseconds
} HardwareEvent;

// MIDI event helper functions
void hardware_events_push_midi_in(uint8_t status, uint8_t data1, uint8_t data2);
void hardware_events_push_midi_out(uint8_t status, uint8_t data1, uint8_t data2);

// Event queue
#define EVENT_QUEUE_SIZE 64

// Initialize hardware event system
void hardware_events_init(void);

// Scan hardware for events
void hardware_events_scan(void);

// Get next event from queue (returns false if queue empty)
bool hardware_events_pop(HardwareEvent* event);

// Get number of events in queue
uint32_t hardware_events_count(void);

#ifdef __cplusplus
}
#endif
