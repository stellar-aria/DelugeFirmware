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

#include "hardware_events.h"

extern "C" {
#include "RZA1/cpu_specific.h"
#include "RZA1/mtu/mtu.h"
#include "RZA1/oled/oled_low_level.h" // For oledWaitingForMessage and callback
#include "RZA1/system/r_typedefs.h"
#include "RZA1/uart/sio_char.h"
#include "definitions.h"
#include "drivers/uart/uart.h"
#include <string.h>
}

// Event queue for hardware events
static HardwareEvent event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t queue_head = 0;
static volatile uint32_t queue_tail = 0;

// PIC32 response codes (matches PIC firmware)
#define PIC_RESPONSE_NO_PRESSES 254
#define PIC_RESPONSE_PAD_OFF 252
#define PIC_PAD_BUTTON_MESSAGES_END 180

static bool push_event(const HardwareEvent* event) {
	uint32_t next_head = (queue_head + 1) % EVENT_QUEUE_SIZE;
	if (next_head == queue_tail) {
		return false; // Queue full
	}
	memcpy(&event_queue[queue_head], event, sizeof(HardwareEvent));
	queue_head = next_head;
	return true;
}

void hardware_events_init(void) {
	queue_head = 0;
	queue_tail = 0;

	// Request PIC32 to resend all button states
	uint8_t msg = 22; // RESEND_BUTTON_STATES command
	char buf[1];
	buf[0] = msg;
	uartPrint(buf);
	uartFlushIfNotSending(UART_ITEM_PIC);
}

void hardware_events_scan(void) {
	// Read from PIC32 UART
	char value_char;
	while (uartGetChar(UART_ITEM_PIC, &value_char) != 0) { // Returns 1 when data available, 0 when empty
		uint8_t value = (uint8_t)value_char;
		HardwareEvent event;
		event.timestamp = *TCNT[TIMER_SYSTEM_SLOW];

		// Check if this is an OLED-related response
		if (oledWaitingForMessage != 256 && value == (uint8_t)oledWaitingForMessage) {
			// PIC responded to OLED select/deselect command
			oledLowLevelTimerCallback();
			continue;
		}

		if (value == PIC_RESPONSE_NO_PRESSES) {
			// No buttons/pads currently pressed
			continue;
		}
		else if (value == PIC_RESPONSE_PAD_OFF) {
			// Pad released - need to read which pad
			if (uartGetChar(UART_ITEM_PIC, &value_char) == 0) {
				value = (uint8_t)value_char;
				event.type = EVENT_TYPE_PAD_RELEASE;
				event.id = value; // Pad position (x + y*16)
				event.value = 0;  // Zero velocity
				push_event(&event);
			}
		}
		else if (value < PIC_PAD_BUTTON_MESSAGES_END) {
			// Button or pad press
			if (value < 128) {
				// Pad press - need velocity
				if (uartGetChar(UART_ITEM_PIC, &value_char) == 0) {
					uint8_t velocity = (uint8_t)value_char;
					event.type = EVENT_TYPE_PAD_PRESS;
					event.id = value; // Pad position (x + y*16)
					event.value = velocity;
					push_event(&event);
				}
			}
			else {
				// Button press/release
				event.type = ((value & 0x80) != 0) ? EVENT_TYPE_BUTTON_RELEASE : EVENT_TYPE_BUTTON_PRESS;
				event.id = value & 0x7F; // Button ID
				event.value = 0;
				push_event(&event);
			}
		}
		else {
			// Encoder or other event
			if (uartGetChar(UART_ITEM_PIC, &value_char) == 0) {
				int8_t delta = (int8_t)value_char;
				event.type = EVENT_TYPE_ENCODER;
				event.id = value - 180;                 // Encoder ID
				event.value = (uint16_t)(int16_t)delta; // Delta as signed
				push_event(&event);
			}
		}
	}
}

bool hardware_events_pop(HardwareEvent* event) {
	if (queue_head == queue_tail) {
		return false;
	}
	memcpy(event, &event_queue[queue_tail], sizeof(HardwareEvent));
	queue_tail = (queue_tail + 1) % EVENT_QUEUE_SIZE;
	return true;
}

uint32_t hardware_events_count(void) {
	if (queue_head >= queue_tail) {
		return queue_head - queue_tail;
	}
	return EVENT_QUEUE_SIZE - queue_tail + queue_head;
}

void hardware_events_push_midi_in(uint8_t status, uint8_t data1, uint8_t data2) {
	HardwareEvent event;
	event.type = EVENT_TYPE_MIDI_IN;
	event.id = status;
	event.value = (data1 << 8) | data2;
	event.timestamp = *TCNT[TIMER_SYSTEM_SLOW];
	push_event(&event);
}

void hardware_events_push_midi_out(uint8_t status, uint8_t data1, uint8_t data2) {
	HardwareEvent event;
	event.type = EVENT_TYPE_MIDI_OUT;
	event.id = status;
	event.value = (data1 << 8) | data2;
	event.timestamp = *TCNT[TIMER_SYSTEM_SLOW];
	push_event(&event);
}
