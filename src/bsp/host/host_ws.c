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

/// host_ws — minimal RFC 6455 server transport. See host_ws.h.

#define _POSIX_C_SOURCE 200809L

#include "host_ws.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <unistd.h>

// ── SHA-1 (public-domain style, compact) ─────────────────────────────────────

typedef struct {
	uint32_t state[5];
	uint64_t count; // message length in bytes
	uint8_t buffer[64];
} sha1_ctx;

static uint32_t sha1_rol(uint32_t v, int b) {
	return (v << b) | (v >> (32 - b));
}

static void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
	uint32_t w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) | ((uint32_t)block[i * 4 + 2] << 8)
		       | ((uint32_t)block[i * 4 + 3]);
	}
	for (int i = 16; i < 80; i++) {
		w[i] = sha1_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	}
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
	for (int i = 0; i < 80; i++) {
		uint32_t f, k;
		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5A827999;
		}
		else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1;
		}
		else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDC;
		}
		else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6;
		}
		uint32_t tmp = sha1_rol(a, 5) + f + e + k + w[i];
		e = d;
		d = c;
		c = sha1_rol(b, 30);
		b = a;
		a = tmp;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
}

static void sha1_init(sha1_ctx* ctx) {
	ctx->state[0] = 0x67452301;
	ctx->state[1] = 0xEFCDAB89;
	ctx->state[2] = 0x98BADCFE;
	ctx->state[3] = 0x10325476;
	ctx->state[4] = 0xC3D2E1F0;
	ctx->count = 0;
}

static void sha1_update(sha1_ctx* ctx, const uint8_t* data, size_t len) {
	size_t have = (size_t)(ctx->count % 64);
	ctx->count += len;
	while (len > 0) {
		size_t take = 64 - have;
		if (take > len) {
			take = len;
		}
		memcpy(ctx->buffer + have, data, take);
		have += take;
		data += take;
		len -= take;
		if (have == 64) {
			sha1_transform(ctx->state, ctx->buffer);
			have = 0;
		}
	}
}

static void sha1_final(sha1_ctx* ctx, uint8_t out[20]) {
	uint64_t bits = ctx->count * 8;
	uint8_t pad = 0x80;
	sha1_update(ctx, &pad, 1);
	uint8_t zero = 0;
	while ((ctx->count % 64) != 56) {
		sha1_update(ctx, &zero, 1);
	}
	uint8_t lenbuf[8];
	for (int i = 0; i < 8; i++) {
		lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
	}
	sha1_update(ctx, lenbuf, 8);
	for (int i = 0; i < 5; i++) {
		out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
	}
}

// ── base64 encode ────────────────────────────────────────────────────────────

static void base64_encode(const uint8_t* in, size_t n, char* out) {
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < n; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16;
		int rem = (int)(n - i);
		if (rem > 1) {
			v |= (uint32_t)in[i + 1] << 8;
		}
		if (rem > 2) {
			v |= (uint32_t)in[i + 2];
		}
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		out[o++] = (rem > 1) ? tbl[(v >> 6) & 0x3F] : '=';
		out[o++] = (rem > 2) ? tbl[v & 0x3F] : '=';
	}
	out[o] = '\0';
}

// ── handshake ────────────────────────────────────────────────────────────────

void host_ws_accept_key(const char* client_key, char out_accept[29]) {
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	sha1_ctx ctx;
	sha1_init(&ctx);
	sha1_update(&ctx, (const uint8_t*)client_key, strlen(client_key));
	sha1_update(&ctx, (const uint8_t*)guid, strlen(guid));
	uint8_t digest[20];
	sha1_final(&ctx, digest);
	base64_encode(digest, 20, out_accept);
}

// Case-insensitively find a header value in the request; copies it (trimmed) into
// `out`. Returns 1 on success, 0 if the header is absent.
static int find_header(const char* req, const char* name, char* out, size_t out_cap) {
	size_t namelen = strlen(name);
	const char* p = req;
	while (*p) {
		// At the start of a line, compare the header name case-insensitively.
		if (strncasecmp(p, name, namelen) == 0 && p[namelen] == ':') {
			const char* v = p + namelen + 1;
			while (*v == ' ' || *v == '\t') {
				v++;
			}
			const char* end = v;
			while (*end && *end != '\r' && *end != '\n') {
				end++;
			}
			size_t len = (size_t)(end - v);
			if (len >= out_cap) {
				len = out_cap - 1;
			}
			memcpy(out, v, len);
			out[len] = '\0';
			return 1;
		}
		// Advance to the next line.
		while (*p && *p != '\n') {
			p++;
		}
		if (*p == '\n') {
			p++;
		}
	}
	return 0;
}

int host_ws_accept(int fd) {
	char req[2048];
	size_t len = 0;
	// Read (blocking) until we see the end of the HTTP headers.
	while (len < sizeof(req) - 1) {
		ssize_t r = read(fd, req + len, sizeof(req) - 1 - len);
		if (r > 0) {
			len += (size_t)r;
			req[len] = '\0';
			if (strstr(req, "\r\n\r\n")) {
				break;
			}
			continue;
		}
		if (r < 0 && errno == EINTR) {
			continue;
		}
		return -1; // EOF or error before the request completed
	}
	req[len] = '\0';

	char key[128];
	if (!find_header(req, "Sec-WebSocket-Key", key, sizeof(key))) {
		return -1;
	}
	char accept[29];
	host_ws_accept_key(key, accept);

	char resp[256];
	int n = snprintf(resp, sizeof(resp),
	                 "HTTP/1.1 101 Switching Protocols\r\n"
	                 "Upgrade: websocket\r\n"
	                 "Connection: Upgrade\r\n"
	                 "Sec-WebSocket-Accept: %s\r\n\r\n",
	                 accept);
	if (n <= 0 || (size_t)n >= sizeof(resp)) {
		return -1;
	}
	size_t off = 0;
	while (off < (size_t)n) {
		ssize_t w = write(fd, resp + off, (size_t)n - off);
		if (w > 0) {
			off += (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		return -1;
	}
	return 0;
}

// ── frame codec ──────────────────────────────────────────────────────────────

size_t host_ws_encode(uint8_t* out, size_t cap, uint8_t opcode, const uint8_t* payload, size_t n) {
	if (n > 0xFFFF) {
		return 0; // 64-bit length form unused here
	}
	size_t header = (n < 126) ? 2u : 4u;
	if (cap < header + n) {
		return 0;
	}
	out[0] = (uint8_t)(0x80u | (opcode & 0x0Fu)); // FIN + opcode
	if (n < 126) {
		out[1] = (uint8_t)n; // server→client: mask bit 0
	}
	else {
		out[1] = 126;
		out[2] = (uint8_t)(n >> 8);
		out[3] = (uint8_t)(n & 0xFF);
	}
	if (n > 0) {
		memcpy(out + header, payload, n);
	}
	return header + n;
}

ssize_t host_ws_decode(const uint8_t* buf, size_t buflen, uint8_t* out, size_t out_cap, size_t* out_len, int* is_ping) {
	*out_len = 0;
	*is_ping = 0;
	if (buflen < 2) {
		return 0; // need at least the 2-byte header
	}
	uint8_t b0 = buf[0];
	uint8_t b1 = buf[1];
	uint8_t opcode = b0 & 0x0Fu;
	int masked = (b1 & 0x80u) != 0;
	uint64_t plen = b1 & 0x7Fu;

	size_t pos = 2;
	if (plen == 126) {
		if (buflen < 4) {
			return 0;
		}
		plen = ((uint64_t)buf[2] << 8) | buf[3];
		pos = 4;
	}
	else if (plen == 127) {
		if (buflen < 10) {
			return 0;
		}
		plen = 0;
		for (int i = 0; i < 8; i++) {
			plen = (plen << 8) | buf[2 + i];
		}
		pos = 10;
	}

	uint8_t mask[4] = {0, 0, 0, 0};
	if (masked) {
		if (buflen < pos + 4) {
			return 0;
		}
		memcpy(mask, buf + pos, 4);
		pos += 4;
	}

	if (buflen < pos + plen) {
		return 0; // payload not fully arrived
	}

	if (opcode == 0x8u) {
		return -1; // close
	}

	// Copy + unmask the payload (truncating to out_cap defensively).
	size_t copy = (plen < out_cap) ? (size_t)plen : out_cap;
	for (size_t i = 0; i < copy; i++) {
		out[i] = masked ? (uint8_t)(buf[pos + i] ^ mask[i & 3]) : buf[pos + i];
	}
	*out_len = copy;

	if (opcode == 0x9u) {
		*is_ping = 1; // caller echoes as pong
	}
	else if (opcode == 0xAu) {
		*out_len = 0; // pong: consume, deliver nothing
	}
	// binary (0x2) / text (0x1) / continuation (0x0): deliver as data.

	return (ssize_t)(pos + plen);
}
