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

/// host_ws — a minimal RFC 6455 WebSocket server layer for host_link.
///
/// The browser panel (tools/deluge-web-sim in the deluge-sdk repo) cannot open a
/// raw TCP socket, so host_link speaks WebSocket when its target is `ws://…`. This
/// module is the transport glue: an HTTP Upgrade handshake plus a binary-frame
/// codec. One WebSocket binary message carries exactly one deluge-protocol frame
/// body (`[type][data]`, no length prefix — WebSocket already delimits messages).
///
/// The codec is split into pure buffer functions (encode/decode) so it composes
/// with host_link's non-blocking, cooperative read loop: host_link owns the socket
/// and its byte accumulator; this module never blocks except during the one-shot
/// handshake.

#ifndef HOST_WS_H
#define HOST_WS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> // ssize_t

#ifdef __cplusplus
extern "C" {
#endif

/// WebSocket opcodes we emit (with FIN set): binary data and pong.
#define HOST_WS_OP_BINARY 0x2u
#define HOST_WS_OP_PONG 0xAu

/// Compute the `Sec-WebSocket-Accept` value for a client key:
/// base64(SHA1(client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")).
/// `out_accept` must hold 29 bytes (28 base64 chars + NUL).
void host_ws_accept_key(const char* client_key, char out_accept[29]);

/// Perform the server handshake on a freshly accepted (still blocking) fd: read
/// the HTTP GET upgrade request, reply `101 Switching Protocols`. Returns 0 on
/// success, -1 on failure (malformed request, missing key, or write error).
int host_ws_accept(int fd);

/// Build one FIN frame with `opcode` carrying `payload[0..n)` (server→client, so
/// unmasked) into `out`. Returns the total frame length, or 0 if `cap` is too
/// small. Supports the 7-bit and 16-bit length forms (n must be < 65536).
size_t host_ws_encode(uint8_t* out, size_t cap, uint8_t opcode, const uint8_t* payload, size_t n);

/// Try to parse one frame from the front of `buf[0..buflen)`.
/// Returns the number of bytes consumed (> 0) when a whole frame was parsed,
/// 0 when more bytes are needed (nothing consumed), or -1 on a close frame /
/// protocol error (the caller should drop the connection).
///
/// On a data frame (binary/text/continuation) the unmasked payload is written to
/// `out[0..*out_len)` and `*is_ping` is false. On a ping frame the payload is
/// written to `out` and `*is_ping` is true (the caller should echo it as a pong).
/// Pong frames are consumed and reported as an empty (`*out_len == 0`) data frame.
ssize_t host_ws_decode(const uint8_t* buf, size_t buflen, uint8_t* out, size_t out_cap, size_t* out_len, int* is_ping);

#ifdef __cplusplus
}
#endif

#endif // HOST_WS_H
