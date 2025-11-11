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
	uint16_t intsts = USB200.INTSTS0;
	SEGGER_RTT_printf(0, "[USB_IRQ] INTSTS0=0x%04X\n", intsts);
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

	SEGGER_RTT_WriteString(0, "Setting up SSI audio...\n");
	// Setup audio output on SSI0 (44.1kHz, stereo)
	ssiInit(0, 1);

	SEGGER_RTT_WriteString(0, "Setting up USB...\n");
	// Enable USB0 module clock (STBCR7 bit 1 = 0)
	CPG.STBCR7 &= 0xFD;
	volatile uint8_t dummy_read = CPG.STBCR7; // Dummy read for write completion
	(void)dummy_read;

	SEGGER_RTT_WriteString(0, "Initializing TinyUSB...\n");
	// Initialize USB device stack (this will configure SUSPMODE, clocks, and USB registers)
	tusb_init();

	// NOTE: Pipe configuration is now handled automatically by the DCD driver
	// in dcd_edpt_open() when the host issues Set Configuration

	// CRITICAL: Register the interrupt handler AFTER hardware initialization (like Deluge does)
	// TinyUSB's dcd_int_enable() only enables the GIC interrupt, but doesn't
	// register the C function handler. Without this, the GIC will set pending
	// bits but there's no handler to call!
	SEGGER_RTT_WriteString(0, "Registering USB interrupt handler...\n");

	// Note: We're NOT clearing DMARS here because it would break OLED DMA transfers
	// The USB interrupt (ID 73) should not be configured as a DMA source by default

	R_INTC_Disable(INTC_ID_USBI0); // Disable first to clear any stale state
	R_INTC_RegistIntFunc(INTC_ID_USBI0, usb_interrupt_handler);
	R_INTC_SetPriority(INTC_ID_USBI0, 5);

	// CRITICAL: Clear any pending interrupt status BEFORE enabling GIC interrupt
	// The USB module may have pending DVST from initialization, and if the interrupt
	// line is already asserted, enabling the GIC won't help (level-sensitive)
	USB200.INTSTS0 = 0; // Clear all interrupt status bits
	delayMs(1);         // Give hardware time to de-assert interrupt line

	R_INTC_Enable(INTC_ID_USBI0);
	SEGGER_RTT_WriteString(0, "USB interrupt handler registered and enabled\n");

	// Give USB hardware time to stabilize (critical for enumeration)
	// USB PHY needs ~10ms to power up and stabilize
	delayMs(50);

	USB200.INTENB0 = 0x0000 | (1 << 15) // VBSE - VBus interrupt
	                 | (1 << 9)         // BRDYE - Buffer Ready
	                 | (1 << 8)         // BEMPE - Buffer Empty
	                 | (1 << 4)         // DVSE - Device State change
	                 | (1 << 0);        // CTRE - Control Transfer Stage Transition
	USB200.BEMPENB = 1;                 // Enable buffer empty interrupt for pipe 0
	USB200.BRDYENB = 1;                 // Enable buffer ready interrupt for pipe 0
	SEGGER_RTT_WriteString(0, "USB device connected and interrupts enabled\n");

	// Debug: Check USB register states after initialization
	SEGGER_RTT_printf(0, "Post-init USB registers:\n");
	SEGGER_RTT_printf(0, "  SYSCFG0  = 0x%04X\n", USB200.SYSCFG0);
	SEGGER_RTT_printf(0, "  INTSTS0  = 0x%04X (DVST pending!)\n", USB200.INTSTS0);
	SEGGER_RTT_printf(0, "  INTENB0  = 0x%04X\n", USB200.INTENB0);
	SEGGER_RTT_printf(0, "  BEMPENB  = 0x%04X\n", USB200.BEMPENB);
	SEGGER_RTT_printf(0, "  BRDYENB  = 0x%04X\n", USB200.BRDYENB);
	SEGGER_RTT_printf(0, "  DVSTCTR0 = 0x%04X\n", USB200.DVSTCTR0);

	// Manually trigger USB interrupt handler to process pending DVST
	SEGGER_RTT_WriteString(0, "Manually calling USB interrupt handler to clear pending DVST...\n");
	usb_interrupt_handler(0);

	// Call tud_task a few times to let TinyUSB process any events
	SEGGER_RTT_WriteString(0, "Polling USB stack...\n");
	for (int i = 0; i < 10; i++) {
		tud_task();
		delayMs(10);
	}
	SEGGER_RTT_printf(0, "After polling - INTSTS0 = 0x%04X\n", USB200.INTSTS0);

	// Debug: Check if GIC interrupt is actually enabled
	volatile uint32_t* icdiser = (volatile uint32_t*)&INTC.ICDISER0;
	uint32_t gic_enabled = icdiser[73 >> 5] & (1u << (73 & 0x1F));
	SEGGER_RTT_printf(0, "GIC ICDISER for IRQ73: 0x%08lX (bit %d = %lu)\n", icdiser[73 >> 5], (73 & 0x1F),
	                  gic_enabled ? 1UL : 0UL);

	// Check if interrupt is pending
	volatile uint32_t* icdispr = (volatile uint32_t*)&INTC.ICDISPR0;
	uint32_t gic_pending = icdispr[73 >> 5] & (1u << (73 & 0x1F));
	SEGGER_RTT_printf(0, "GIC ICDISPR for IRQ73: 0x%08lX (pending = %lu)\n", icdispr[73 >> 5], gic_pending ? 1UL : 0UL);

	SEGGER_RTT_WriteString(0, "Initializing hardware event scanning...\n");
	// Initialize hardware event scanning (will configure PIC for pad reading)
	hardware_events_init();

	SEGGER_RTT_WriteString(0, "Initializing USB subsystems...\n");
	// Initialize USB serial protocol
	// usb_serial_init();  // Disabled for audio-only testing

	// Initialize USB audio for audio I/O
	usb_audio_init();

	// Initialize USB MIDI for MIDI I/O
	// usb_midi_init();  // Disabled for audio-only testing

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

	uint32_t loop_count = 0;
	uint32_t last_printed_intsts = USB200.INTSTS0;
	// Main loop
	while (true) {
		// Debug: Print loop count every 100000 iterations or when INTSTS0 changes significantly
		// Ignore DVST bit (0x2000) changes as they're too frequent
		uint16_t current_intsts = USB200.INTSTS0;
		uint16_t current_intsts_masked = current_intsts & ~0x2000; // Ignore DVST bit
		uint16_t last_intsts_masked = last_printed_intsts & ~0x2000;

		if ((loop_count % 100000) == 0 || current_intsts_masked != last_intsts_masked) {
			SEGGER_RTT_printf(0, "Loop %lu, INTSTS0=0x%04X\n", loop_count, current_intsts);
			last_printed_intsts = current_intsts;
		}
		loop_count++;

		// Process OLED transfer queue (handles select/deselect/DMA)
		oledRoutine();

		// Flush PIC UART if needed
		uartFlushIfNotSending(UART_ITEM_PIC);

		// Process USB tasks - this should handle events but it's not working
		tud_task();

		// WORKAROUND: Manually check for USB interrupts and call handler
		// This is needed because interrupts aren't working and tud_task() doesn't poll the hardware
		// Check INTSTS0 against INTENB0, BRDYENB, and BEMPENB
		uint16_t intsts = USB200.INTSTS0;
		uint16_t intenb = USB200.INTENB0;
		uint16_t brdyenb = USB200.BRDYENB;
		uint16_t bempenb = USB200.BEMPENB;

		// Check if any enabled interrupt is pending
		bool has_interrupt = false;
		if (intsts & intenb)
			has_interrupt = true; // Standard interrupts
		if ((intsts & 0x0080) && brdyenb)
			has_interrupt = true; // BRDY (bit 7)
		if ((intsts & 0x0010) && bempenb)
			has_interrupt = true; // BEMP (bit 4)

		if (has_interrupt) {
			dcd_int_handler(0);
		}

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
	usb_serial_task(); // Disabled for audio-only testing
	usb_audio_task();
	usb_midi_task(); // Disabled for audio-only testing
}

void midiAndGateTimerGoneOff(void) {
	// MIDI/Gate timer interrupt callback
	// This is called when a scheduled MIDI or gate output event needs to fire
	// The actual MIDI output will be sent through the USB serial protocol
	// TODO: Implement MIDI/Gate output scheduling and processing
}
