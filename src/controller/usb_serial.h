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

// Protocol version
#define USB_SERIAL_VERSION_MAJOR 1
#define USB_SERIAL_VERSION_MINOR 0
#define USB_SERIAL_VERSION_PATCH 0

// Message types FROM Deluge TO Host
typedef enum {
	MSG_FROM_PAD_PRESSED = 0x01,
	MSG_FROM_PAD_RELEASED = 0x02,
	MSG_FROM_BUTTON_PRESSED = 0x03,
	MSG_FROM_BUTTON_RELEASED = 0x04,
	MSG_FROM_ENCODER_ROTATED = 0x05,
	MSG_FROM_ENCODER_PRESSED = 0x06,
	MSG_FROM_ENCODER_RELEASED = 0x07,
	MSG_FROM_VERSION = 0x10,
	MSG_FROM_PONG = 0x11,
	MSG_FROM_READY = 0x12,
	MSG_FROM_ERROR = 0x13,
} MessageFromDelugeType;

// Message types TO Deluge FROM Host
typedef enum {
	MSG_TO_UPDATE_DISPLAY = 0x20,
	MSG_TO_CLEAR_DISPLAY = 0x21,
	MSG_TO_SET_PAD_RGB = 0x22,
	MSG_TO_CLEAR_ALL_PADS = 0x23,
	MSG_TO_SET_LED = 0x24,
	MSG_TO_SET_CV = 0x25,
	MSG_TO_SET_GATE = 0x26,
	MSG_TO_GET_VERSION = 0x30,
	MSG_TO_PING = 0x31,
} MessageToDelugeType;

// Initialize USB serial protocol
void usb_serial_init(void);

// Process USB serial tasks (send/receive messages)
void usb_serial_task(void);

// Check if USB is connected and ready
bool usb_serial_is_connected(void);

// Send messages FROM Deluge TO Host
void usb_serial_send_pad_pressed(uint8_t col, uint8_t row);
void usb_serial_send_pad_released(uint8_t col, uint8_t row);
void usb_serial_send_button_pressed(uint8_t button_id);
void usb_serial_send_button_released(uint8_t button_id);
void usb_serial_send_encoder_rotated(uint8_t encoder_id, int8_t delta);
void usb_serial_send_encoder_pressed(uint8_t encoder_id);
void usb_serial_send_encoder_released(uint8_t encoder_id);
void usb_serial_send_version(void);
void usb_serial_send_pong(void);
void usb_serial_send_ready(void);
void usb_serial_send_error(const char* error_msg);

#ifdef __cplusplus
}
#endif
