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

#include "usb_midi.h"
#include "RZA1/cpu_specific.h"
#include "RZA1/uart/sio_char.h"
#include "drivers/uart/uart.h"
#include "tusb.h"

void usb_midi_init(void) {
	// MIDI UART is already initialized in main (uartInit(UART_ITEM_MIDI, 31250))
	// No additional initialization needed
}

void usb_midi_task(void) {
	// Handle incoming MIDI from USB and send to UART
	if (tud_midi_available()) {
		uint8_t packet[4];
		while (tud_midi_packet_read(packet)) {
			// USB MIDI packet format: [cable_number_and_code_index, byte0, byte1, byte2]
			// Extract MIDI bytes and send via UART
			uint8_t code_index = packet[0] & 0x0F;

			// Determine number of MIDI bytes based on code index
			int num_bytes = 0;
			switch (code_index) {
			case 0x02: // Two-byte System Common (e.g., MTC, Song Select)
			case 0x06: // System Exclusive End (single byte)
			case 0x0C: // Program Change
			case 0x0D: // Channel Pressure
				num_bytes = 2;
				break;
			case 0x03: // Three-byte System Common (e.g., SPP)
			case 0x04: // System Exclusive Start/Continue
			case 0x07: // System Exclusive End (three bytes)
			case 0x08: // Note Off
			case 0x09: // Note On
			case 0x0A: // Poly Aftertouch
			case 0x0B: // Control Change
			case 0x0E: // Pitch Bend
				num_bytes = 3;
				break;
			case 0x05: // Single-byte System Common (e.g., Cable Select, Tune Request)
			case 0x0F: // Single Byte (e.g., Clock, Start, Stop)
				num_bytes = 1;
				break;
			default:
				continue; // Invalid code index
			}

			// Send MIDI bytes to UART
			for (int i = 0; i < num_bytes; i++) {
				bufferMIDIUart(packet[1 + i]);
			}
			uartFlushIfNotSending(UART_ITEM_MIDI);
		}
	}

	// Check for MIDI data from UART and send to USB
	char midi_byte;
	uint8_t midi_packet[4] = {0};
	static uint8_t midi_buffer[3] = {0};
	static int midi_buffer_pos = 0;
	static uint8_t running_status = 0;

	while (uartGetChar(UART_ITEM_MIDI, &midi_byte)) {
		uint8_t byte = (uint8_t)midi_byte;

		// Handle status byte
		if (byte & 0x80) {
			// Status byte
			if (byte >= 0xF0) {
				// System message
				if (byte == 0xF8 || byte == 0xFA || byte == 0xFB || byte == 0xFC || byte == 0xFE || byte == 0xFF) {
					// Single-byte real-time message
					midi_packet[0] = 0x0F; // Single Byte
					midi_packet[1] = byte;
					tud_midi_packet_write(midi_packet);
					continue;
				}
				// Other system messages - clear running status
				running_status = 0;
			}
			else {
				// Channel message - update running status
				running_status = byte;
			}
			midi_buffer[0] = byte;
			midi_buffer_pos = 1;
		}
		else {
			// Data byte
			if (running_status == 0) {
				// No running status, ignore data byte
				continue;
			}
			if (midi_buffer_pos == 0) {
				// Running status - reuse last status byte
				midi_buffer[0] = running_status;
				midi_buffer_pos = 1;
			}
			midi_buffer[midi_buffer_pos++] = byte;
		}

		// Check if we have a complete message
		uint8_t status = midi_buffer[0];
		int required_bytes = 0;
		uint8_t code_index = 0;

		if ((status & 0xF0) == 0x80 || (status & 0xF0) == 0x90 || (status & 0xF0) == 0xA0 || (status & 0xF0) == 0xB0
		    || (status & 0xF0) == 0xE0) {
			// Note Off, Note On, Poly Aftertouch, Control Change, Pitch Bend
			required_bytes = 3;
			code_index = (status >> 4) & 0x0F;
		}
		else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
			// Program Change, Channel Pressure
			required_bytes = 2;
			code_index = (status >> 4) & 0x0F;
		}

		if (midi_buffer_pos >= required_bytes) {
			// Send complete message
			midi_packet[0] = code_index;
			midi_packet[1] = midi_buffer[0];
			midi_packet[2] = required_bytes >= 2 ? midi_buffer[1] : 0;
			midi_packet[3] = required_bytes >= 3 ? midi_buffer[2] : 0;
			tud_midi_packet_write(midi_packet);
			midi_buffer_pos = 0;
		}
	}
}

// TinyUSB MIDI callbacks
void tud_midi_rx_cb(uint8_t itf) {
	(void)itf;
	// MIDI data received from host - will be processed in usb_midi_task()
}
