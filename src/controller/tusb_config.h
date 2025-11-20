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

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "usb_descriptors.h"

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------

// RZA1L MCU
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RZA1X
#endif

// RUSB1 specific configuration (required by TinyUSB RZA1 driver)
// Clock source: 0 = USB_X1 48MHz crystal, 1 = EXTAL 12MHz
#ifndef RUSB1_CLOCK_SOURCE
#define RUSB1_CLOCK_SOURCE 0 // Deluge uses USB_X1 48MHz
#endif

// Wait cycles for register access (must result in > 67ns wait time)
// With P1 bus @ 33.33MHz (30ns/cycle), need ≥67ns / 30ns = 2.23 cycles minimum
// Using 5 wait cycles (7 total access cycles) = 210ns >> 67ns (matches Deluge config)
#ifndef RUSB1_WAIT_CYCLES
#define RUSB1_WAIT_CYCLES 5
#endif

// No RTOS
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

// Debug level (0 = no debug, 1 = errors only, 2 = warnings, 3 = verbose)
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 1 // Reduced verbosity - only show errors
#endif

// Enable Device stack
#define CFG_TUD_ENABLED 1

// RHPort configuration
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUSB_RHPORT1_MODE 0

// RHPort number used for device
#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 0
#endif

// RHPort max operational speed - CHANGED TO HIGH-SPEED FOR UAC2
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#endif

#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

//------------- CLASS -------------//
#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 1 // Fixed: 512-byte endpoints for High-Speed BULK
#define CFG_TUD_AUDIO 1
#define CFG_TUD_VENDOR 0

// CDC FIFO size of TX and RX
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512

// CDC Endpoint transfer buffer size
// High-Speed USB requires 512 bytes for bulk endpoints
#define CFG_TUD_CDC_EP_BUFSIZE 512

// MIDI FIFO size of TX and RX
#define CFG_TUD_MIDI_RX_BUFSIZE 512
#define CFG_TUD_MIDI_TX_BUFSIZE 512

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// Audio descriptor length - now includes both speaker and microphone
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN TUD_AUDIO_HEADSET_STEREO_DESC_LEN

// Number of Standard AS Interface Descriptors - 2 (speaker + microphone, each with 2 format alternates)
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 3

// Size of control request buffer
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64

// Number of formats - dual format (16-bit and 24-bit)
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS 2

// Audio format type I specifications - 44.1kHz stereo
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE 44100
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX 2 // Stereo output (PC to Deluge)
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 2 // Stereo input (Deluge to PC)

// RX (output from PC): Format 1 - 16-bit (2 bytes per sample)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX 2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX 16

// RX (output from PC): Format 2 - 24-bit in 32-bit slots (4 bytes per sample)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_N_BYTES_PER_SAMPLE_RX 4
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_RX 24

// TX (input to PC): Format 1 - 16-bit (2 bytes per sample)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX 2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX 16

// TX (input to PC): Format 2 - 24-bit in 32-bit slots (4 bytes per sample)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_N_BYTES_PER_SAMPLE_TX 4
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_RESOLUTION_TX 24

// Enable endpoints
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 1 // Enable AC interrupt endpoint
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1 // Speaker output (PC to Deluge)
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1  // Microphone input (Deluge to PC)

// Calculate EP sizes for output (speaker) - use larger of the two formats
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT                                                                        \
	TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,                                        \
	                  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_EP_SZ_OUT                                                                        \
	TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,                                        \
	                  CFG_TUD_AUDIO_FUNC_1_FORMAT_2_N_BYTES_PER_SAMPLE_RX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT                                                                                 \
	TU_MAX(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT, CFG_TUD_AUDIO_FUNC_1_FORMAT_2_EP_SZ_OUT)

// Calculate EP sizes for input (microphone) - use larger of the two formats
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN                                                                         \
	TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,                                        \
	                  CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_2_EP_SZ_IN                                                                         \
	TUD_AUDIO_EP_SIZE(TUD_OPT_HIGH_SPEED, CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE,                                        \
	                  CFG_TUD_AUDIO_FUNC_1_FORMAT_2_N_BYTES_PER_SAMPLE_TX, CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_SZ_IN                                                                                  \
	TU_MAX(CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN, CFG_TUD_AUDIO_FUNC_1_FORMAT_2_EP_SZ_IN)

// Software buffers for High-Speed operation (32x EP size for 1ms = 8 HS frames)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (32 * CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX CFG_TUD_AUDIO_FUNC_1_EP_SZ_OUT
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (32 * CFG_TUD_AUDIO_FUNC_1_EP_SZ_IN)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX CFG_TUD_AUDIO_FUNC_1_EP_SZ_IN

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
