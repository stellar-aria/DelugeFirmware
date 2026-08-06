/*
 * Copyright © 2026 Synthstrom Audible Limited
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

/// Host-sim platform backend: the non-boundary symbols the portable app expects
/// from its environment but which are NOT part of the <libdeluge/...> contract —
/// on the SoC they come from RZA1/diskio.c, RZA1/usb/, the firmware src/main.c,
/// and the linker. Here they are inert host stubs sufficient to link and to run
/// headless (no SD card, no USB). The memory-map boundary symbols live in
/// host_bsp.c alongside the rest of memory.h.

#include "board_config.h"           // TRIGGER_CLOCK_INPUT_NUM_TIMES_STORED
#include "libdeluge/block_device.h" // DelugeStatus, DELUGE_ERR_*
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

// ===========================================================================
// SD card presence + block device (RZA1/diskio.c on target). On host the "SD
// card" is a plain directory named by env DELUGE_SD_ROOT: task-context file
// I/O (song load, sample streaming, stem/recorder writes) goes through
// host_efatfs_passthrough.cpp, which opens files under that same directory —
// so deluge_block_ready/deluge_block_init report presence directly from
// DELUGE_SD_ROOT, agreeing with what the passthrough actually serves. The
// plain sector-addressed block device below (deluge_block_read/_write) has no
// reader left on host: it only ever fed C-FatFS's ff.c, which this BSP no
// longer compiles. Kept as no-op stubs so audio_file_manager.cpp's
// disk_read/disk_write FatFs-porting shim (now itself unreferenced, pending
// src/fatfs's own retirement) still links.
// ===========================================================================

// True iff DELUGE_SD_ROOT is set and names an existing directory.
static bool host_sd_root_present(void) {
	const char* root = getenv("DELUGE_SD_ROOT");
	if (root == NULL || root[0] == '\0') {
		return false;
	}
	struct stat st;
	return stat(root, &st) == 0 && S_ISDIR(st.st_mode);
}

// Set while the SD routine is mid-access on target; nothing toggles it on host
// (synchronous I/O, no reentrancy), but it is read app-wide.
uint8_t currentlyAccessingCard = 0;

// block_device.h — the app's native (non-FatFS) card-detect/init entry points.
// deluge_block_ready/deluge_block_init live here (rather than host_bsp.c,
// where the rest of the block_device.h stubs are) because they need this
// file's DELUGE_SD_ROOT presence check, mirroring the passthrough it agrees with.
DelugeStatus deluge_block_init(uint8_t unit) {
	(void)unit;
	return host_sd_root_present() ? DELUGE_OK : DELUGE_ERR_NODEV;
}

bool deluge_block_ready(uint8_t unit) {
	(void)unit;
	return host_sd_root_present();
}

DelugeStatus deluge_block_read(uint8_t unit, uint8_t* dst, uint32_t sector, uint32_t count) {
	(void)unit;
	(void)dst;
	(void)sector;
	(void)count;
	return DELUGE_ERR_NODEV;
}

DelugeStatus deluge_block_write(uint8_t unit, const uint8_t* src, uint32_t sector, uint32_t count) {
	(void)unit;
	(void)src;
	(void)sector;
	(void)count;
	return DELUGE_ERR_NODEV;
}

// ===========================================================================
// USB host/peripheral control (RZA1/usb/ on target). Headless host → inert.
// ===========================================================================

uint8_t anythingInitiallyAttachedAsUSBHost = 0;

void openUSBHost(void) {
}
void closeUSBHost(void) {
}
void openUSBPeripheral(void) {
}

// ===========================================================================
// Trigger-clock input edge buffer (defined in firmware src/main.c, filled from
// the GPIO ISR on target). No external clock input on host → stays empty.
// ===========================================================================

uint32_t triggerClockRisingEdgeTimes[TRIGGER_CLOCK_INPUT_NUM_TIMES_STORED];
uint32_t triggerClockRisingEdgesReceived = 0;
uint32_t triggerClockRisingEdgesProcessed = 0;

// ===========================================================================
// eyalroz printf sink. Route to stdout so D_PRINTLN / debug output is visible.
// ===========================================================================

void putchar_(char c) {
	putchar((unsigned char)c);
}

// ===========================================================================
// arm-linux reference build only (sim/arm-linux-toolchain.cmake). On __arm__ the app takes code
// paths that reference symbols normally provided by bsp/rza1 or hand-asm: the fault handler (its
// FREEZE_WITH_ERROR macro reads LR/SP and calls fault_handler_print_freeze_pointers) and the FM
// synth's neon_fm_kernel. Neither is on the audio path the WAV-diff harness exercises (error path;
// default patch is subtractive, not FM), so stub them. On x86 (__arm__ undefined) these paths are
// not taken and the symbols are not referenced.
// ===========================================================================
#if defined(__arm__)
void fault_handler_print_freeze_pointers(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	(void)a;
	(void)b;
	(void)c;
	(void)d;
}
void neon_fm_kernel(const int32_t* in, const int32_t* busin, int32_t* out, int count, int32_t phase0, int32_t freq,
                    int32_t gain1, int32_t dgain) {
	(void)in;
	(void)busin;
	(void)out;
	(void)count;
	(void)phase0;
	(void)freq;
	(void)gain1;
	(void)dgain;
}
#endif
