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

#include "usb_serial.h"
#include "display_control.h"
#include "tusb.h"
#include <string.h>

extern "C" {
#include "RTT/SEGGER_RTT.h"
}

// Message buffer for receiving
#define RX_BUFFER_SIZE 1024
static uint8_t rx_buffer[RX_BUFFER_SIZE];
static uint32_t rx_buffer_pos = 0;

// Message buffer for sending
#define TX_BUFFER_SIZE 256
static uint8_t tx_buffer[TX_BUFFER_SIZE];

// Simple message framing: [length:2][type:1][data:N]
// Length includes type byte and data, but not the length field itself

static void send_message(uint8_t type, const uint8_t* data, uint16_t data_len) {
	if (!tud_cdc_connected()) {
		return;
	}

	uint16_t total_len = 1 + data_len; // type + data

	// Build message in tx_buffer
	tx_buffer[0] = total_len & 0xFF;
	tx_buffer[1] = (total_len >> 8) & 0xFF;
	tx_buffer[2] = type;

	if (data_len > 0 && data != NULL) {
		memcpy(&tx_buffer[3], data, data_len);
	}

	// Send via TinyUSB CDC
	tud_cdc_write(tx_buffer, 3 + data_len);
	tud_cdc_write_flush();
}

static void process_incoming_message(uint8_t type, const uint8_t* data, uint16_t data_len) {
	switch (type) {
	case MSG_TO_UPDATE_DISPLAY:
		if (data_len > 0) {
			display_update(data, data_len);
		}
		break;

	case MSG_TO_CLEAR_DISPLAY:
		display_clear();
		break;

	case MSG_TO_SET_PAD_RGB:
		if (data_len >= 5) {
			uint8_t col = data[0];
			uint8_t row = data[1];
			uint8_t r = data[2];
			uint8_t g = data[3];
			uint8_t b = data[4];
			pad_led_set_rgb(col, row, r, g, b);
		}
		break;

	case MSG_TO_CLEAR_ALL_PADS:
		pad_led_clear_all();
		break;

	case MSG_TO_SET_LED:
		if (data_len >= 2) {
			uint8_t led_index = data[0];
			bool on = data[1] != 0;
			hardware_led_set(led_index, on);
		}
		break;

	case MSG_TO_SET_CV:
		if (data_len >= 3) {
			uint8_t channel = data[0];
			uint16_t value = (data[1] << 8) | data[2];
			cv_set(channel, value);
		}
		break;

	case MSG_TO_SET_GATE:
		if (data_len >= 2) {
			uint8_t channel = data[0];
			bool on = data[1] != 0;
			gate_set(channel, on);
		}
		break;

	case MSG_TO_GET_VERSION:
		usb_serial_send_version();
		break;

	case MSG_TO_PING:
		usb_serial_send_pong();
		break;

	default:
		// Unknown message type
		break;
	}
}

void usb_serial_init(void) {
	// USB initialization is done in main
	rx_buffer_pos = 0;
}

void usb_serial_task(void) {
	if (!tud_cdc_connected()) {
		return;
	}

	// Read available data
	uint32_t available = tud_cdc_available();
	if (available == 0) {
		return;
	}

	// Read into buffer
	uint32_t space = RX_BUFFER_SIZE - rx_buffer_pos;
	if (space > 0) {
		uint32_t to_read = (available < space) ? available : space;
		uint32_t read = tud_cdc_read(&rx_buffer[rx_buffer_pos], to_read);
		rx_buffer_pos += read;
	}

	// Process complete messages
	while (rx_buffer_pos >= 3) { // Minimum: 2-byte length + 1-byte type
		uint16_t msg_len = rx_buffer[0] | (rx_buffer[1] << 8);
		uint16_t total_len = 2 + msg_len; // length field + message

		if (rx_buffer_pos >= total_len) {
			// Complete message available
			uint8_t type = rx_buffer[2];
			uint16_t data_len = msg_len - 1; // Subtract type byte
			const uint8_t* data = (data_len > 0) ? &rx_buffer[3] : NULL;

			process_incoming_message(type, data, data_len);

			// Remove processed message from buffer
			if (rx_buffer_pos > total_len) {
				memmove(rx_buffer, &rx_buffer[total_len], rx_buffer_pos - total_len);
				rx_buffer_pos -= total_len;
			}
			else {
				rx_buffer_pos = 0;
			}
		}
		else {
			// Incomplete message, wait for more data
			break;
		}
	}
}

bool usb_serial_is_connected(void) {
	return tud_cdc_connected();
}

// Send message implementations
void usb_serial_send_pad_pressed(uint8_t col, uint8_t row) {
	uint8_t data[2] = {col, row};
	send_message(MSG_FROM_PAD_PRESSED, data, 2);
}

void usb_serial_send_pad_released(uint8_t col, uint8_t row) {
	uint8_t data[2] = {col, row};
	send_message(MSG_FROM_PAD_RELEASED, data, 2);
}

void usb_serial_send_button_pressed(uint8_t button_id) {
	send_message(MSG_FROM_BUTTON_PRESSED, &button_id, 1);
}

void usb_serial_send_button_released(uint8_t button_id) {
	send_message(MSG_FROM_BUTTON_RELEASED, &button_id, 1);
}

void usb_serial_send_encoder_rotated(uint8_t encoder_id, int8_t delta) {
	uint8_t data[2] = {encoder_id, (uint8_t)delta};
	send_message(MSG_FROM_ENCODER_ROTATED, data, 2);
}

void usb_serial_send_encoder_pressed(uint8_t encoder_id) {
	send_message(MSG_FROM_ENCODER_PRESSED, &encoder_id, 1);
}

void usb_serial_send_encoder_released(uint8_t encoder_id) {
	send_message(MSG_FROM_ENCODER_RELEASED, &encoder_id, 1);
}

void usb_serial_send_version(void) {
	uint8_t data[3] = {USB_SERIAL_VERSION_MAJOR, USB_SERIAL_VERSION_MINOR, USB_SERIAL_VERSION_PATCH};
	send_message(MSG_FROM_VERSION, data, 3);
}

void usb_serial_send_pong(void) {
	send_message(MSG_FROM_PONG, NULL, 0);
}

void usb_serial_send_ready(void) {
	send_message(MSG_FROM_READY, NULL, 0);
}

void usb_serial_send_error(const char* error_msg) {
	uint16_t len = strlen(error_msg);
	if (len > 255)
		len = 255;
	send_message(MSG_FROM_ERROR, (const uint8_t*)error_msg, len);
}

// TinyUSB callbacks for debugging
extern "C" {

// Invoked when device is mounted
void tud_mount_cb(void) {
	SEGGER_RTT_WriteString(0, "*** USB MOUNTED - Device enumerated successfully! ***\n");
}

// Invoked when device is unmounted
void tud_umount_cb(void) {
	SEGGER_RTT_WriteString(0, "*** USB UNMOUNTED ***\n");
}

// Invoked when USB bus is suspended
void tud_suspend_cb(bool remote_wakeup_en) {
	(void)remote_wakeup_en;
	SEGGER_RTT_WriteString(0, "*** USB SUSPENDED ***\n");
}

// Invoked when USB bus is resumed
void tud_resume_cb(void) {
	SEGGER_RTT_WriteString(0, "*** USB RESUMED ***\n");
}

} // extern "C"
