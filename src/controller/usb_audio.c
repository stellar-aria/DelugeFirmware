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
// 44.1kHz stereo 16-bit = 176.4 KB/s, we'll use 1ms frames = ~176 bytes per frame
// Round up to 192 bytes (48 samples * 2 channels * 2 bytes)
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_FRAME_SIZE 48 // samples per frame (approximately 1ms at 44.1kHz)

// Buffers for audio streaming
static int16_t audio_tx_buffer[AUDIO_FRAME_SIZE * 2]; // Stereo output to host (mic/line in)
static int16_t audio_rx_buffer[AUDIO_FRAME_SIZE * 2]; // Stereo input from host (speaker out)

// Audio state
static volatile bool audio_tx_ready = false;
static volatile bool audio_rx_ready = false;

void usb_audio_init(void) {
	memset(audio_tx_buffer, 0, sizeof(audio_tx_buffer));
	memset(audio_rx_buffer, 0, sizeof(audio_rx_buffer));
	audio_tx_ready = false;
	audio_rx_ready = false;
}

void usb_audio_task(void) {
	// Check if we have audio to send to host (from Deluge's ADC)
	if (audio_tx_ready && tud_audio_n_mounted(0)) {
		// Send audio samples to host (mic/line input)
		if (tud_audio_n_write(0, (uint8_t*)audio_tx_buffer, sizeof(audio_tx_buffer)) > 0) {
			audio_tx_ready = false;
		}
	}
}

// Called when host is ready to receive more audio data (mic/line input to host)
bool tud_audio_tx_done_pre_load_cb(uint8_t rhport, uint8_t itf, uint8_t ep_in, uint8_t cur_alt_setting) {
	(void)rhport;
	(void)itf;
	(void)ep_in;
	(void)cur_alt_setting;

	// Read audio from Deluge's ADC/SSI hardware
	// ssiRxBuffer contains stereo input at 44.1kHz (int32_t samples)
	int32_t* rxBuffer = getRxBufferStart();
	int32_t* rxCurrentPlace = (int32_t*)getRxBufferCurrentPlace();

	// Calculate how far behind we are in the circular buffer
	intptr_t samplesToRead = rxCurrentPlace - rxBuffer;
	if (samplesToRead < 0) {
		samplesToRead += (getRxBufferEnd() - getRxBufferStart());
	}

	// Read AUDIO_FRAME_SIZE stereo samples, converting from int32_t to int16_t
	// Limit to available samples to avoid reading too far ahead
	int32_t numSamples = AUDIO_FRAME_SIZE < (samplesToRead / 2) ? AUDIO_FRAME_SIZE : (samplesToRead / 2);
	if (numSamples > 0) {
		for (int i = 0; i < numSamples; i++) {
			// Convert from 32-bit to 16-bit (take upper 16 bits)
			audio_tx_buffer[i * 2] = (int16_t)(rxBuffer[i * 2] >> 16);         // Left
			audio_tx_buffer[i * 2 + 1] = (int16_t)(rxBuffer[i * 2 + 1] >> 16); // Right
		}
	}
	else {
		// No audio available, send silence
		memset(audio_tx_buffer, 0, sizeof(audio_tx_buffer));
	}

	return true;
}

// Called when audio data is received from host (speaker output from host)
bool tud_audio_rx_done_post_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out,
                                    uint8_t cur_alt_setting) {
	(void)rhport;
	(void)func_id;
	(void)ep_out;
	(void)cur_alt_setting;

	if (n_bytes_received > 0) {
		// Read audio data from USB
		uint16_t bytes_read = tud_audio_n_read(0, (uint8_t*)audio_rx_buffer, sizeof(audio_rx_buffer));

		if (bytes_read > 0) {
			// Send to Deluge's DAC/SSI audio output
			// ssiTxBuffer expects stereo int32_t samples at 44.1kHz
			int32_t* txBuffer = getTxBufferStart();
			int32_t* txCurrentPlace = (int32_t*)getTxBufferCurrentPlace();

			// Calculate number of stereo samples received
			int32_t numSamples = bytes_read / 4; // 4 bytes per stereo sample (2ch * 2bytes)
			if (numSamples > AUDIO_FRAME_SIZE) {
				numSamples = AUDIO_FRAME_SIZE;
			}

			// Write to TX buffer, converting from int16_t to int32_t
			// We write ahead of the current DMA position
			int32_t* writePos = txCurrentPlace;
			for (int i = 0; i < numSamples; i++) {
				// Convert from 16-bit to 32-bit (shift to upper 16 bits)
				writePos[i * 2] = (int32_t)audio_rx_buffer[i * 2] << 16;         // Left
				writePos[i * 2 + 1] = (int32_t)audio_rx_buffer[i * 2 + 1] << 16; // Right
			}

			audio_rx_ready = true;
		}
	}

	return true;
}

// TinyUSB audio callbacks
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const* p_request) {
	(void)rhport;
	(void)p_request;
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
	audio_control_request_t const* request = (audio_control_request_t const*)p_request;

	// Feature unit set requests (volume/mute)
	if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT && request->bRequest == AUDIO_CS_REQ_CUR) {
		if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
			// Accept mute changes but don't actually do anything yet
			return true;
		}
		else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
			// Accept volume changes but don't actually do anything yet
			return true;
		}
	}
	// Clock source set requests
	else if (request->bEntityID == UAC2_ENTITY_CLOCK && request->bRequest == AUDIO_CS_REQ_CUR) {
		if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
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
	audio_control_request_t const* request = (audio_control_request_t const*)p_request;

	TU_LOG2("  AUDIO get_req_entity: entity=%u, selector=%u, request=%u\r\n", request->bEntityID,
	        request->bControlSelector, request->bRequest);

	// Clock source requests
	if (request->bEntityID == UAC2_ENTITY_CLOCK) {
		if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_CUR) {
			// Return current sample rate (44.1kHz)
			audio_control_cur_4_t sample_rate = {.bCur = 44100};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sample_rate, sizeof(sample_rate));
		}
		else if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO_CS_REQ_RANGE) {
			// Return supported sample rate range (only 44.1kHz)
			audio_control_range_4_n_t(1) sample_rate_range = {.wNumSubRanges = 1, .subrange[0] = {44100, 44100, 0}};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &sample_rate_range,
			                                                  sizeof(sample_rate_range));
		}
		else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
			// Clock is always valid
			audio_control_cur_1_t clock_valid = {.bCur = 1};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &clock_valid, sizeof(clock_valid));
		}
	}
	// Feature unit requests (volume/mute)
	else if (request->bEntityID == UAC2_ENTITY_SPK_FEATURE_UNIT) {
		if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
			// Return mute state (not muted)
			audio_control_cur_1_t mute = {.bCur = 0};
			return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &mute, sizeof(mute));
		}
		else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
			if (request->bRequest == AUDIO_CS_REQ_RANGE) {
				// Volume range: -50dB to 0dB, 1dB steps
				audio_control_range_2_n_t(1)
				    volume_range = {.wNumSubRanges = 1, .subrange[0] = {.bMin = -50 * 256, .bMax = 0, .bRes = 256}};
				return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &volume_range,
				                                                  sizeof(volume_range));
			}
			else if (request->bRequest == AUDIO_CS_REQ_CUR) {
				// Current volume: 0dB (max)
				audio_control_cur_2_t volume = {.bCur = 0};
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
