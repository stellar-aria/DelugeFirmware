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

#include "controller.h"
#include "display_control.h"
#include "drivers/pic/pic.h"
#include "hardware_events.h"
#include "tusb.h"
#include "usb_audio.h"
#include "usb_midi.h"
#include "usb_serial.h"

extern "C" {
#include "RTT/SEGGER_RTT.h"
#include "RZA1/cpu_specific.h"
#include "RZA1/gpio/gpio.h"
#include "RZA1/intc/devdrv_intc.h"
#include "RZA1/oled/oled_low_level.h"
#include "RZA1/rspi/rspi.h"
#include "RZA1/ssi/ssi.h"
#include "RZA1/system/iobitmasks/usb_iobitmask.h"
#include "RZA1/system/iodefine.h"
#include "RZA1/uart/sio_char.h"
#include "definitions.h"
#include "drivers/ssi/ssi.h"
#include "drivers/uart/uart.h"
#include "portable/renesas/rusb1/dcd_rusb1.h"

// TinyUSB interrupt handler (from tinyusb)
void dcd_int_handler(uint8_t rhport);

// USB interrupt handler wrapper
void usb_interrupt_handler(uint32_t int_sense) {
	(void)int_sense;
	dcd_int_handler(0); // rhport 0
}

// External functions from OLED driver (oled_init.c is C)
extern void oledDMAInit(void);
extern void setupOLED(void);
extern void oledRoutine(void); // Process OLED transfers

// Timer utility function (used by oled_low_level.c)
uint32_t msToSlowTimerCount(uint32_t ms) {
	return ms * 33; // TIMER_SYSTEM_SLOW runs at 33kHz
}
}

// Simple delay in milliseconds (approximate)
static void delayMs(uint32_t ms) {
	volatile uint32_t count = ms * 10000;
	while (count > 0) {
		count--;
		__asm volatile("nop");
	}
}

int32_t controller_main(void) {
	// Initialize RTT for debug output
	SEGGER_RTT_ConfigUpBuffer(0, NULL, NULL, 0, SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);
	SEGGER_RTT_WriteString(0, "\n=== Deluge USB Controller Starting ===\n");

	// Assume OLED is always present in controller mode
	SEGGER_RTT_WriteString(0, "Configuring PIC32...\n");

	// Configure PIC32 for pad/button scanning
	PIC::enableOLED();               // Enable OLED (PIC command 247)
	PIC::setDebounce(5);             // Set debounce time to 20ms (value * 4)
	PIC::setRefreshTime(23);         // Set pad refresh time to 23ms
	PIC::setMinInterruptInterval(8); // Set min interrupt interval to 8ms
	PIC::setFlashLength(6);          // Set flash length to 6ms
	PIC::setUARTSpeed();             // Set UART speed to high speed (200kHz)
	PIC::flush();

	// Give PIC time to switch baud rates
	SEGGER_RTT_WriteString(0, "Waiting for PIC32 to switch to 200kHz...\n");
	delayMs(50);

	// Now switch OUR side to 200kHz to match the PIC
	PIC::setupForPads(); // This calls uartSetBaudRate(UART_CHANNEL_PIC, 200000)
	SEGGER_RTT_WriteString(0, "Host UART switched to 200kHz\n");

	// Request PIC firmware version and button states (important for PIC initialization)
	SEGGER_RTT_WriteString(0, "Requesting PIC firmware version and button states...\n");
	PIC::requestFirmwareVersion();
	PIC::resendButtonStates();
	PIC::flush();
	delayMs(50); // Give PIC time to process and respond

	// Drain any responses from PIC (firmware version, button states, etc.)
	int drain_count = 0;
	PIC::Response response;
	while ((response = PIC::read()) != PIC::Response::NONE && drain_count < 100) {
		drain_count++;
	}
	SEGGER_RTT_printf(0, "Drained %d bytes from PIC\n", drain_count);

	SEGGER_RTT_WriteString(0, "Setting up GPIO pins...\n");
	// GPIO setup for audio codec and peripherals
	setPinAsOutput(CODEC.port, CODEC.pin);
	setOutputState(CODEC.port, CODEC.pin, 1); // Enable codec

	setPinAsOutput(SPEAKER_ENABLE.port, SPEAKER_ENABLE.pin);
	setOutputState(SPEAKER_ENABLE.port, SPEAKER_ENABLE.pin, 0); // Speaker off initially

	// Battery and sync LEDs
	setOutputState(BATTERY_LED.port, BATTERY_LED.pin, 1); // Off (open-drain)
	setPinAsOutput(BATTERY_LED.port, BATTERY_LED.pin);

	setOutputState(SYNCED_LED.port, SYNCED_LED.pin, 0); // Off
	setPinAsOutput(SYNCED_LED.port, SYNCED_LED.pin);

	// Audio input detection pins
	setPinAsInput(HEADPHONE_DETECT.port, HEADPHONE_DETECT.pin);
	setPinAsInput(LINE_IN_DETECT.port, LINE_IN_DETECT.pin);
	setPinAsInput(MIC_DETECT.port, MIC_DETECT.pin);
	setPinAsInput(LINE_OUT_DETECT_L.port, LINE_OUT_DETECT_L.pin);
	setPinAsInput(LINE_OUT_DETECT_R.port, LINE_OUT_DETECT_R.pin);

	// Analog voltage sense
	setPinMux(VOLT_SENSE.port, VOLT_SENSE.pin, 1);

	SEGGER_RTT_WriteString(0, "Setting up SPI for CV/OLED...\n");
	// Setup SPI for CV/OLED (OLED always present, limited to 10MHz)
	R_RSPI_Create(SPI_CHANNEL_CV, 10000000, 0, 32);
	R_RSPI_Start(SPI_CHANNEL_CV);
	setPinMux(SPI_CLK.port, SPI_CLK.pin, 3);   // CLK
	setPinMux(SPI_MOSI.port, SPI_MOSI.pin, 3); // MOSI

	// OLED shares SPI - manually control SSL pin
	setOutputState(SPI_SSL.port, SPI_SSL.pin, 1);
	setPinAsOutput(SPI_SSL.port, SPI_SSL.pin);

	SEGGER_RTT_WriteString(0, "Initializing OLED...\n");
	setupSPIInterrupts();
	oledDMAInit();
	setupOLED();

	// Drain PIC responses from OLED init
	// The PIC echoes back: SET_DC_LOW, ENABLE_OLED, SELECT_OLED, SET_DC_HIGH (from init), DESELECT_OLED
	// Plus there may be many DESELECT_OLED responses (0xF9)
	SEGGER_RTT_WriteString(0, "Draining OLED init responses...\n");
	delayMs(10); // Let responses arrive
	int oled_drain = 0;
	PIC::Response oled_resp;
	while ((oled_resp = PIC::read()) != PIC::Response::NONE && oled_drain < 500) {
		oled_drain++;
	}
	SEGGER_RTT_printf(0, "Drained %d OLED responses\n", oled_drain);

	SEGGER_RTT_WriteString(0, "Setting up SSI audio...\n");
	// Setup audio output on SSI0 (44.1kHz, stereo)
	ssiInit(0, 1);

	SEGGER_RTT_WriteString(0, "Setting up USB...\n");
	// Enable USB0 module clock (STBCR7 bit 1 = 0)
	CPG.STBCR7 &= 0xFD;
	volatile uint8_t dummy_read = CPG.STBCR7; // Dummy read for write completion
	(void)dummy_read;

	// Register USB interrupt handler BEFORE initializing TinyUSB
	// TinyUSB will enable the interrupt via dcd_int_enable(), so we just register and set priority
	SEGGER_RTT_WriteString(0, "Registering USB interrupt handler...\n");
	R_INTC_Disable(INTC_ID_USBI0);
	R_INTC_RegistIntFunc(INTC_ID_USBI0, usb_interrupt_handler);
	R_INTC_SetPriority(INTC_ID_USBI0, 9);
	// Note: TinyUSB's tud_init() will call dcd_int_enable() to enable the interrupt
	SEGGER_RTT_WriteString(0, "USB interrupt handler registered (will be enabled by TinyUSB)\n");

	// Initialize TinyUSB device stack
	SEGGER_RTT_WriteString(0, "Initializing TinyUSB...\n");
	tud_init(0);

	// Connect to host (enables D+ pull-up)
	SEGGER_RTT_WriteString(0, "Connecting to USB host...\n");
	tud_connect();

	// CRITICAL: RZA1L quirk - interrupt enable registers only become writable after
	// VBUS detection and device state changes. Call tud_task() to process USB events,
	// then enable interrupts.
	for (int i = 0; i < 10; i++) {
		tud_task();
		delayMs(10);
	}

	// Now enable USB interrupts (registers are writable after VBUS detection)
	USB200.INTSTS0 = 0;
	USB200.INTENB0 = 0 | USB_INTENB0_VBSE // VBus interrupt
	                 | USB_INTENB0_BRDYE  // Buffer Ready
	                 | USB_INTENB0_BEMPE  // Buffer Empty
	                 | USB_INTENB0_DVSE   // Device State change
	                 | USB_INTENB0_CTRE   // Control Transfer Stage Transition
	                 | USB_INTENB0_RSME;  // Resume
	USB200.BEMPENB = 1;
	USB200.BRDYENB = 1;

	SEGGER_RTT_WriteString(0, "Initializing hardware event scanning...\n");
	// Initialize hardware event scanning (will configure PIC for pad reading)
	hardware_events_init();

	SEGGER_RTT_WriteString(0, "Initializing USB subsystems...\n");
	// Initialize USB serial protocol
	usb_serial_init(); // Disabled for audio-only testing

	// Initialize USB audio for audio I/O
	usb_audio_init();

	// Initialize USB MIDI for MIDI I/O
	usb_midi_init(); // Disabled for audio-only testing

	SEGGER_RTT_WriteString(0, "Clearing all LEDs and display...\n");

	// Clear all pad LEDs
	pad_led_clear_all();

	// Turn off all indicator LEDs
	for (int i = 0; i < 36; i++) {
		hardware_led_set(i, false);
	}

	// Ensure all LED commands are sent
	uartFlushIfNotSending(UART_ITEM_PIC);
	delayMs(50);

	// Clear OLED display
	display_clear();

	SEGGER_RTT_WriteString(0, "All outputs cleared\n");

	SEGGER_RTT_WriteString(0, "=== Controller initialization complete, entering main loop ===\n");

	// Main loop
	while (true) {
		// Process OLED transfer queue (handles select/deselect/DMA)
		oledRoutine();

		// Flush PIC UART if needed
		uartFlushIfNotSending(UART_ITEM_PIC);

		// Process USB tasks (handles non-interrupt USB events)
		tud_task();

		// Scan hardware for events
		hardware_events_scan();

		// Send any pending events over USB serial
		usb_serial_task();

		// Handle USB audio streaming
		usb_audio_task();

		// Handle USB MIDI I/O
		usb_midi_task();
	}

	return 0;
}

void controller_task(void) {
	// This can be called from main loop if needed
	tud_task();
	hardware_events_scan();
	usb_serial_task();
	usb_audio_task();
	usb_midi_task();
}

void midiAndGateTimerGoneOff(void) {
	// MIDI/Gate timer interrupt callback
	// This is called when a scheduled MIDI or gate output event needs to fire
	// The actual MIDI output will be sent through the USB serial protocol
	// TODO: Implement MIDI/Gate output scheduling and processing
}
