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

#include "usb_audio.h"
#include "drivers/ssi/ssi.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include <string.h>

#if CFG_TUD_AUDIO

// Audio buffers for USB transfer
// 44.1kHz stereo 24-bit in 32-bit slots = 352.8 KB/s
// We'll use 1ms frames = ~353 bytes per frame
// 44.1 samples * 2 channels * 4 bytes = ~353 bytes
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_FRAME_SIZE 44 // samples per frame (approximately 1ms at 44.1kHz)

// Buffers for audio streaming - 24-bit in 32-bit slots (int32_t)
static int32_t audio_rx_buffer[AUDIO_FRAME_SIZE * 2]; // Stereo input from host (speaker out)

// Audio state
static volatile bool audio_tx_ready = false;
static volatile bool audio_rx_ready = false;

void usb_audio_init(void) {
	memset(audio_rx_buffer, 0, sizeof(audio_rx_buffer));
	audio_rx_ready = false;
}

void usb_audio_task(void) {
	// Speaker-only mode - no TX (microphone) functionality
	// Only handle RX (playback from host to Deluge speakers)
}

// TX callback - disabled for speaker-only mode
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
	(void)rhport;
	(void)itf;
	(void)ep_in;
	(void)cur_alt_setting;

	// Speaker-only mode - no microphone TX
	return true;
}

// Called when audio data is received from host (speaker output from host)
bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting) {
	(void)rhport;
	(void)func_id;
	(void)ep_out;

	if (n_bytes_received > 0) {
		TU_LOG2("  RX audio: %u bytes, alt=%u\r\n", n_bytes_received, cur_alt_setting);

		// Read audio data from USB
		uint16_t bytes_read = tud_audio_n_read(0, (uint8_t*)audio_rx_buffer, sizeof(audio_rx_buffer));

		if (bytes_read > 0) {
			TU_LOG2("  Read %u bytes from USB audio (alt %u)\r\n", bytes_read, cur_alt_setting);

			// Send to Deluge's DAC/SSI audio output
			// ssiTxBuffer expects stereo int32_t samples (24-bit in 32-bit slots) at 44.1kHz
			int32_t* txBuffer = getTxBufferStart();
			int32_t* txCurrentPlace = (int32_t*)getTxBufferCurrentPlace();
			int32_t* txBufferEnd = getTxBufferEnd();

			int32_t numValues = 0;

			// cur_alt_setting: 0=idle, 1=16-bit, 2=24-bit
			if (cur_alt_setting == 1) {
				// 16-bit format: convert to 24-bit in 32-bit slots
				int16_t* samples_16 = (int16_t*)audio_rx_buffer;
				numValues = bytes_read / 2; // Number of 16-bit samples (L+R)
				if (numValues > AUDIO_FRAME_SIZE * 2) {
					numValues = AUDIO_FRAME_SIZE * 2;
				}

				// Convert 16-bit to 24-bit (shift left by 8 bits)
				for (int i = 0; i < numValues; i++) {
					audio_rx_buffer[i] = ((int32_t)samples_16[i]) << 8;
				}
			}
			else if (cur_alt_setting == 2) {
				// 24-bit in 32-bit slots: use directly
				numValues = bytes_read / 4; // Number of int32_t values (L+R samples)
				if (numValues > AUDIO_FRAME_SIZE * 2) {
					numValues = AUDIO_FRAME_SIZE * 2;
				}
			}
			else {
				// Alternate 0 is idle, shouldn't receive data
				TU_LOG1("  Received data on idle alternate setting!\r\n");
				return true;
			}

			TU_LOG2("  Writing %ld int32 values to audio buffer\r\n", numValues);

			// Copy 24-bit audio directly to TX buffer
			// Audio comes as 24-bit in 32-bit slots, SSI expects same format
			// We need to write ahead of the current DMA position to avoid glitches
			int32_t bufferSizeValues = txBufferEnd - txBuffer;
			int32_t currentOffset = txCurrentPlace - txBuffer;
			int32_t writeOffset = currentOffset + 128; // Write 128 values (64 stereo samples) ahead
			if (writeOffset >= bufferSizeValues) {
				writeOffset -= bufferSizeValues;
			}

			// Copy samples with circular buffer wraparound
			int32_t* writePos = txBuffer + writeOffset;
			for (int i = 0; i < numValues; i++) {
				*writePos = audio_rx_buffer[i];
				writePos++;

				// Wrap around circular buffer
				if (writePos >= txBufferEnd) {
					writePos = txBuffer;
				}
			}

			audio_rx_ready = true;
		}
		else {
			TU_LOG2("  Failed to read audio data\r\n");
		}
	}

	return true;
}

// TinyUSB audio callbacks
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	(void)rhport;

	// Extract interface and alternate setting from the request
	uint8_t const itf = tu_u16_low(p_request->wIndex);
	uint8_t const alt = tu_u16_low(p_request->wValue);

	TU_LOG2("  AUDIO set_itf: itf=%u, alt=%u\r\n", itf, alt);

	// When Windows selects alternate setting 1, it's ready to stream audio
	// Alternate setting 0 is idle (no streaming)
	if (itf == ITF_NUM_AUDIO_STREAMING_SPK) {
		if (alt == 1) {
			TU_LOG2("  Audio streaming enabled (alt=1)\r\n");
		}
		else {
			TU_LOG2("  Audio streaming disabled (alt=0)\r\n");
		}
	}

	return true;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	(void)rhport;
	(void)p_request;
	return true;
}

// Audio control callbacks (volume, mute, etc.)
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff) {
	(void)rhport;
	(void)p_request;
	(void)pBuff;
	return true;
}

bool tud_audio_set_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff) {
	(void)rhport;
	(void)p_request;
	(void)pBuff;
	return true;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request, uint8_t* pBuff) {
	(void)rhport;
	audio20_control_request_t const* request = (audio20_control_request_t const*)p_request;

	// Feature unit set requests (volume/mute)
	if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT && request->bRequest == AUDIO20_CS_REQ_CUR) {
		if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE) {
			// Accept mute changes but don't actually do anything yet
			return true;
		}
		else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
			// Accept volume changes but don't actually do anything yet
			return true;
		}
	}
	// Clock source set requests
	else if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bRequest == AUDIO20_CS_REQ_CUR) {
		if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ) {
			// Accept sample rate changes but we only support 44.1kHz
			return true;
		}
	}

	return false; // Request not handled
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	(void)rhport;
	(void)p_request;
	return true;
}

bool tud_audio_get_req_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	(void)rhport;
	(void)p_request;
	return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	audio20_control_request_t const* request = (audio20_control_request_t const*)p_request;

	TU_LOG2("  AUDIO get_req_entity: entity=%u, selector=%u, request=%u\r\n", request->bEntityID,
	        request->bControlSelector, request->bRequest);

	// Clock source requests
	if (request->bEntityID == UAC2_ENTITY_CLOCK) {
		if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_CUR) {
			// Return current sample rate (44.1kHz)
			audio20_control_cur_4_t sample_rate = {.bCur = 44100};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sample_rate, sizeof(sample_rate));
		}
		else if (request->bControlSelector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_RANGE) {
			// Return supported sample rate range (only 44.1kHz)
			audio20_control_range_4_n_t(1) sample_rate_range = {.wNumSubRanges = 1, .subrange[0] = {44100, 44100, 0}};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sample_rate_range,
			                                                  sizeof(sample_rate_range));
		}
		else if (request->bControlSelector == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR) {
			// Clock is always valid
			audio20_control_cur_1_t clock_valid = {.bCur = 1};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &clock_valid, sizeof(clock_valid));
		}
	}
	// Feature unit requests (volume/mute)
	else if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
		if (request->bControlSelector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR) {
			// Return mute state (not muted)
			audio20_control_cur_1_t mute = {.bCur = 0};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
		}
		else if (request->bControlSelector == AUDIO20_FU_CTRL_VOLUME) {
			if (request->bRequest == AUDIO20_CS_REQ_RANGE) {
				// Volume range: -50dB to 0dB, 1dB steps
				audio20_control_range_2_n_t(1)
				    volume_range = {.wNumSubRanges = 1, .subrange[0] = {.bMin = -50 * 256, .bMax = 0, .bRes = 256}};
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume_range,
				                                                  sizeof(volume_range));
			}
			else if (request->bRequest == AUDIO20_CS_REQ_CUR) {
				// Current volume: 0dB (max)
				audio20_control_cur_2_t volume = {.bCur = 0};
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume, sizeof(volume));
			}
		}
	}

	TU_LOG1("  AUDIO get_req_entity NOT HANDLED: entity=%u, selector=%u, request=%u\r\n", request->bEntityID,
	        request->bControlSelector, request->bRequest);
	return false; // Request not handled
}

#else // CFG_TUD_AUDIO == 0

// Stub implementations when audio is disabled
void usb_audio_init(void) {
	// Do nothing
}

void usb_audio_task(void) {
	// Do nothing
}

#endif // CFG_TUD_AUDIO
