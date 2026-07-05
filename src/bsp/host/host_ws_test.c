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

// Standalone unit test for host_ws (no socket needed):
//   cc -Isrc/bsp/host src/bsp/host/host_ws.c src/bsp/host/host_ws_test.c -o /tmp/ws_test && /tmp/ws_test

#include "host_ws.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
	// 1. RFC 6455 §1.3 canonical accept-key example.
	char accept[29];
	host_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept);
	assert(strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);

	// 2. Encode a small server binary frame (unmasked): FIN|binary, len, payload.
	uint8_t body[3] = {0x24, 35, 1}; // a SetLed deluge-protocol body
	uint8_t frame[16];
	size_t fl = host_ws_encode(frame, sizeof(frame), HOST_WS_OP_BINARY, body, sizeof(body));
	assert(fl == 5);
	assert(frame[0] == 0x82); // FIN + binary
	assert(frame[1] == 3);    // len, mask bit clear
	assert(frame[2] == 0x24 && frame[3] == 35 && frame[4] == 1);

	// 3. Decode a masked client binary frame (as a browser would send).
	uint8_t mask[4] = {0x10, 0x20, 0x30, 0x40};
	uint8_t payload[3] = {0x01, 5, 2}; // PadPressed
	uint8_t client[2 + 4 + 3];
	client[0] = 0x82;                // FIN + binary
	client[1] = (uint8_t)(0x80 | 3); // masked, len 3
	memcpy(client + 2, mask, 4);
	for (int i = 0; i < 3; i++) {
		client[6 + i] = (uint8_t)(payload[i] ^ mask[i & 3]);
	}
	uint8_t out[8];
	size_t out_len = 0;
	int is_ping = 0;
	ssize_t consumed = host_ws_decode(client, sizeof(client), out, sizeof(out), &out_len, &is_ping);
	assert(consumed == (ssize_t)sizeof(client));
	assert(!is_ping);
	assert(out_len == 3);
	assert(out[0] == 0x01 && out[1] == 5 && out[2] == 2);

	// 4. A truncated frame is reported as "need more bytes" (0 consumed).
	assert(host_ws_decode(client, 4, out, sizeof(out), &out_len, &is_ping) == 0);

	// 5. A close frame (opcode 0x8) returns -1.
	uint8_t close_frame[2] = {0x88, 0x00};
	assert(host_ws_decode(close_frame, sizeof(close_frame), out, sizeof(out), &out_len, &is_ping) == -1);

	printf("ok\n");
	return 0;
}
