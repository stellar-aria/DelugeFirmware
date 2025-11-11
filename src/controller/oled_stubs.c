/*
 * OLED stubs for controller mode
 * Provides definitions for OLED variables and functions needed by the low-level drivers
 */

#include "RZA1/uart/sio_char.h"
#include "definitions.h"
#include "usb_serial.h"
#include <stdint.h>

// OLED framebuffer: 128x64 pixels = 1024 bytes (8 rows of 128 bytes)
uint8_t oledMainImage[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS] = {{0}};

// CV engine callback - called when CV SPI transfer completes
// In controller mode, we don't need to do anything special
void cvSent(void) {
	// Stub - CV output complete callback
	// In the full Deluge, this manages the CV output queue
	// For controller mode, the low-level driver handles the queue
}
