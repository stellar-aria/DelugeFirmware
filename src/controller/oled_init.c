/*
 * Simple OLED initialization for USB controller
 */

#include "RZA1/uart/sio_char.h"
#include "definitions.h"
#include "drivers/oled/oled.h"
#include "drivers/rspi/rspi.h"
#include "drivers/uart/uart.h"

// Simple delay in milliseconds
static void delayMs(uint32_t ms) {
	// Very approximate delay using busy loop
	// TODO: use timer for accurate delay
	volatile uint32_t count = ms * 10000;
	while (count > 0) {
		count--;
		__asm volatile("nop");
	}
}

void setupOLED(void) {
	// Set up 8-bit SPI mode for OLED
	RSPI(SPI_CHANNEL_OLED_MAIN).SPDCR = 0x20u;               // 8-bit
	RSPI(SPI_CHANNEL_OLED_MAIN).SPCMD0 = 0b0000011100000010; // 8-bit
	RSPI(SPI_CHANNEL_OLED_MAIN).SPBFCR.BYTE = 0b01100000;

	// Send PIC commands to set up OLED - DC low for commands, enable, then select
	bufferPICUart(250); // SET_DC_LOW
	bufferPICUart(247); // ENABLE_OLED
	bufferPICUart(248); // SELECT_OLED
	uartFlushIfNotSending(UART_ITEM_PIC);

	delayMs(5);

	// Initialize OLED main display
	oledMainInit();

	// Deselect OLED
	bufferPICUart(249); // DESELECT_OLED
	uartFlushIfNotSending(UART_ITEM_PIC);
}
