/*
 * Display and LED control for USB controller
 * Handles commands from host to control OLED display and pad LEDs
 */

#include "display_control.h"
#include "drivers/pic/pic.h"

extern "C" {
#include "RTT/SEGGER_RTT.h"
#include "RZA1/gpio/gpio.h"
#include "RZA1/oled/oled_low_level.h"
#include "RZA1/uart/sio_char.h"
#include "definitions.h"
#include "drivers/oled/oled.h"
#include "drivers/uart/uart.h"
#include <string.h>
}

// External OLED framebuffer (defined in oled_stubs.c)
extern "C" uint8_t oledMainImage[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS];

// Pad LED state tracking (16 columns x 8 rows, RGB)
// This matches Deluge's kDisplayWidth=16, kDisplayHeight=8
static RGB pad_led_state[16][8];

// Gate output GPIO pins (from cv_engine.h)
// Port 2: pins 7, 8, 9 (gates 0, 1, 2)
// Port 4: pin 0 (gate 3)
static constexpr uint8_t gatePort[] = {2, 2, 2, 4};
static constexpr uint8_t gatePin[] = {7, 8, 9, 0};

extern "C" {

void display_update(const uint8_t* buffer, uint16_t size) {
	// Validate size
	if (size > sizeof(oledMainImage)) {
		size = sizeof(oledMainImage);
	}

	// Copy framebuffer data
	memcpy(oledMainImage, buffer, size);

	// Drain any pending PIC responses first
	int drain_count = 0;
	while (PIC::read() != PIC::Response::NONE && drain_count < 200) {
		drain_count++;
	}

	// Select OLED via PIC
	PIC::selectOLED();
	PIC::flush();

	// Wait for PIC to confirm selection (response 248 = SELECT_OLED echoed back)
	PIC::Response response = PIC::read(50000); // Timeout in timer counts
	if (response != static_cast<PIC::Response>(248)) {
		SEGGER_RTT_printf(0, "WARNING: OLED select got response 0x%02X instead of 0xF8\n",
		                  static_cast<uint8_t>(response));
	}

	// Queue SPI transfer to OLED
	enqueueSPITransfer(0, (uint8_t const*)oledMainImage);

	// Deselect OLED
	PIC::deselectOLED();
	PIC::flush();
}

void display_clear() {
	// Clear framebuffer
	memset(oledMainImage, 0, sizeof(oledMainImage));

	// Queue the blank image for transfer to OLED
	// The oledRoutine() will handle select/deselect automatically
	enqueueSPITransfer(0, (uint8_t const*)oledMainImage);
}
void pad_led_set_rgb(uint8_t col, uint8_t row, uint8_t r, uint8_t g, uint8_t b) {
	// Deluge has 16 columns x 8 rows (includes main grid + sidebar)
	if (col >= 16 || row >= 8) {
		return;
	}

	// Update state
	pad_led_state[col][row].r = r;
	pad_led_state[col][row].g = g;
	pad_led_state[col][row].b = b;

	// PIC expects column pairs (2 columns at a time, 8 rows each = 16 RGB triplets)
	uint8_t column_pair_idx = col / 2; // 0-7 (8 column pairs)
	uint8_t base_col = column_pair_idx * 2;

	// Build array of 16 RGB values (first column's 8 rows, then second column's 8 rows)
	std::array<RGB, 16> colours;
	for (uint8_t row_idx = 0; row_idx < 8; row_idx++) {
		colours[row_idx] = pad_led_state[base_col][row_idx];
	}
	for (uint8_t row_idx = 0; row_idx < 8; row_idx++) {
		colours[8 + row_idx] = pad_led_state[base_col + 1][row_idx];
	}

	PIC::setColourForTwoColumns(column_pair_idx, colours);
	PIC::flush();
}

void pad_led_clear_all() {
	// Clear all 18 columns (16 main grid + 2 sidebar) by sending 9 column pairs
	std::array<RGB, 16> zero_colors{}; // All black
	for (uint8_t pair = 0; pair < 9; ++pair) {
		PIC::setColourForTwoColumns(pair, zero_colors);
	}
	PIC::flush();
	PIC::waitForFlush();
}

void hardware_led_set(uint8_t led_index, bool on) {
	// Map LED index (0-35) to PIC command (152-223)
	// OFF: 152-187, ON: 188-223
	if (led_index >= 36) {
		return;
	}

	if (on) {
		PIC::setLEDOn(led_index);
	}
	else {
		PIC::setLEDOff(led_index);
	}
	PIC::flush();
}
void cv_set(uint8_t channel, uint16_t value) {
	// Deluge has 4 CV output channels
	if (channel >= 4) {
		return;
	}

	// Queue CV message via SPI
	// CV DAC protocol: 4-bit channel + 12-bit value
	uint32_t message = (channel << 12) | (value & 0x0FFF);
	enqueueCVMessage(channel, message);
}

void gate_set(uint8_t channel, bool on) {
	// Deluge has 4 Gate output channels
	if (channel >= 4) {
		return;
	}

	// Gate outputs are direct GPIO pins
	// Note: setOutputState is inverted - sending true (1) turns the gate OFF
	// So we need to invert the 'on' value
	setOutputState(gatePort[channel], gatePin[channel], on ? 0 : 1);
}

} // extern "C"
