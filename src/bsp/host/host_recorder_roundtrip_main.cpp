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

/// deluge_recorder_roundtrip — headless byte-exact round-trip oracle for `SampleRecorder`.
///
/// The oracle itself (the geometry matrix + alterFile characterization + the two SR3b probes) lives
/// in the `DELUGE_HOST` harness scenario `deluge_scenario_run_recorder_roundtrip()`
/// (harness/recorder_roundtrip_scenario.cpp), so it can run BOTH here on the cooperative C-host BSP
/// AND on the Rust/Embassy host BSP (golden_vt_render's `GOLDEN_SCENARIO=recorder_roundtrip` mode).
/// This file is just the C-host driver shell: format an empty FAT image + a POSIX SD-root, boot the
/// full host-sim app (same `deluge_platform_init()`/`deluge_main()` sequence as
/// `deluge_render`/`deluge_loadcheck`), run the scenario synchronously on the cooperative host
/// driver, and exit with its failure count.
///
/// Usage: deluge_recorder_roundtrip   (no arguments; runs the whole geometry matrix, deterministic)
/// Exit code 0 on all cases passing, 1 otherwise. No mtools/project dependency — the disk image is
/// freshly `mformat`-ed empty (we only ever WRITE new files, never read a pre-seeded project).

#include "harness/recorder_roundtrip_scenario.h"

#include "OSLikeStuff/scheduler_api.h" // TaskHandle
#include "libdeluge/system.h"          // deluge_platform_init

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

extern "C" int32_t deluge_main(void);
extern TaskHandle startupConditionalTask;

namespace {

char g_temp_image[512] = {0};
char g_sd_root[512] = {0};

void cleanup_temp_image() {
	if (g_temp_image[0] != '\0') {
		unlink(g_temp_image);
		g_temp_image[0] = '\0';
	}
}

void cleanup_sd_root() {
	if (g_sd_root[0] != '\0') {
		char cmd[600];
		snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_sd_root);
		if (system(cmd) != 0) {
			// Best-effort cleanup; not a test failure.
		}
		g_sd_root[0] = '\0';
	}
}

// This binary's streaming read path (open_read_stream(), via host_efatfs_passthrough.cpp) is backed
// by a plain POSIX DELUGE_SD_ROOT directory, separate from the mounted FAT image (DELUGE_SD_IMAGE)
// the recorder writes into -- set one up so the finalized-multicluster probe can mirror a finalized
// file there and open a genuine streaming-read cursor on it. (The Embassy renderer reads efatfs on
// the same image it wrote to, so it leaves DELUGE_SD_ROOT unset and the probe skips the mirror.)
bool make_sd_root(char* out_path, size_t out_size) {
	char tmpl[] = "/tmp/deluge_recorder_roundtrip_sdroot_XXXXXX";
	char* dir = mkdtemp(tmpl);
	if (dir == nullptr) {
		perror("[recorder_roundtrip] mkdtemp");
		return false;
	}
	snprintf(out_path, out_size, "%s", dir);
	return true;
}

bool format_empty_image(char* out_path, size_t out_size) {
	char tmpl[] = "/tmp/deluge_recorder_roundtrip_XXXXXX.img";
	int fd = mkstemps(tmpl, 4);
	if (fd < 0) {
		perror("[recorder_roundtrip] mkstemps");
		return false;
	}
	close(fd);
	snprintf(out_path, out_size, "%s", tmpl);

	// Same geometry as host_loadcheck_main.cpp / host_render_main.cpp: >= 2.5 GB sparse image so it
	// formats as a valid FAT32 with 32 KB clusters (the geometry the firmware's FatFS/efatfs expects).
	const long long bytes = 2560LL << 20;
	char cmd[512];
	snprintf(cmd, sizeof cmd, "truncate -s %lld '%s'", bytes, out_path);
	if (system(cmd) != 0) {
		fprintf(stderr, "[recorder_roundtrip] truncate failed\n");
		return false;
	}
	snprintf(cmd, sizeof cmd, "mformat -i '%s' -F -c 64 ::", out_path);
	if (system(cmd) != 0) {
		fprintf(stderr, "[recorder_roundtrip] mformat failed (is mtools installed?)\n");
		return false;
	}
	return true;
}

void deluge_recorder_roundtrip_driver() {
	static bool started = false;
	if (started) {
		return;
	}
	started = true;

	int32_t failures = deluge_scenario_run_recorder_roundtrip();
	fflush(nullptr);
	quick_exit(failures == 0 ? 0 : 1);
}

} // namespace

int main(int, char**) {
	if (!format_empty_image(g_temp_image, sizeof g_temp_image)) {
		return 1;
	}
	at_quick_exit(cleanup_temp_image);
	atexit(cleanup_temp_image);
	setenv("DELUGE_SD_IMAGE", g_temp_image, 1);

	if (!make_sd_root(g_sd_root, sizeof g_sd_root)) {
		return 1;
	}
	at_quick_exit(cleanup_sd_root);
	atexit(cleanup_sd_root);
	setenv("DELUGE_SD_ROOT", g_sd_root, 1);

	if (getenv("DELUGE_HOST_DETERMINISTIC") == nullptr) {
		setenv("DELUGE_HOST_DETERMINISTIC", "1", 1);
	}
	if (getenv("DELUGE_HOST_AUDIO") == nullptr) {
		setenv("DELUGE_HOST_AUDIO", "off", 1);
	}

	startupConditionalTask = deluge_recorder_roundtrip_driver; // override before deluge_main registers it

	deluge_platform_init();
	deluge_main(); // never returns; deluge_recorder_roundtrip_driver quick_exit()s

	cleanup_temp_image();
	cleanup_sd_root();
	return 0;
}
