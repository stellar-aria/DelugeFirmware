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

// Debug level (2 = more verbose, 3 = most verbose)
#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 2 // Re-enable to debug enumeration failure
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

// RHPort max operational speed
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
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
#define CFG_TUD_MIDI 0  // Disabled temporarily to test CDC only
#define CFG_TUD_AUDIO 0 // Disabled temporarily to simplify enumeration
#define CFG_TUD_VENDOR 0

// CDC FIFO size of TX and RX
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512

// CDC Endpoint transfer buffer size, more is faster
#define CFG_TUD_CDC_EP_BUFSIZE 64

// MIDI FIFO size of TX and RX
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 128

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// Audio descriptor length
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN TUD_AUDIO_HEADSET_STEREO_DESC_LEN

// Number of Standard AS Interface Descriptors - 2 (one for output, one for input)
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT 2

// Size of control request buffer
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ 64

// Number of formats
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS 1

// Audio format type I specifications - 44.1kHz, 16-bit stereo
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE 44100
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX 2 // Stereo input (mic/line in)
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX 2 // Stereo output

// 16bit in 16bit slots
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX 2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX 16
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX 2
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX 16

// EP and buffer size - for isochronous EP's, the buffer and EP size are equal
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN                                                                         \
	TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX,       \
	                  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN * 4)
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN

#define CFG_TUD_AUDIO_ENABLE_EP_OUT 1
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT                                                                        \
	TUD_AUDIO_EP_SIZE(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE, CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,       \
	                  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ (CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT * 2)
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
