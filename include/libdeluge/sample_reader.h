/*
 * Copyright © 2014-2026 Synthstrom Audible Limited
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

/// libdeluge/sample_reader.h — the sample range-reader C-ABI (U1).
///
/// A zero-copy streaming reader-handle (plus a stateless copy convenience) over a sample's source
/// residency, for non-voice consumers that read raw-PCM frame RANGES (pitch detect, waveform
/// overview, crossfade averages, perc-cache, wavetable build, hop search) without reaching
/// `StreamedChunk` internals the way they do today. This is the non-voice twin of the voice region
/// port (`sample_source.h`'s `DelugeSampleSource`/`DelugeSampleRegion`): where that port hands the
/// voice a cluster-indexed region to read itself, this one hides clusters/leases/convert/boundaries
/// entirely behind a plain frame-cursor, since non-voice callers have no reason to know a cluster
/// boundary exists.
///
/// Coexists with the existing facade (`peek`/`prefetch`/`load_now`/`request`/`dequeue`, declared in
/// `deluge::audio::stream`) — nothing is deleted or migrated by this header landing. No consumer
/// uses this API yet (that is U2); U1 delivers the reader and proves it byte-identical to the
/// facade's own reads via a differential (see the U1 design doc).
#ifndef LIBDELUGE_SAMPLE_READER_H
#define LIBDELUGE_SAMPLE_READER_H

#include "libdeluge/sample_source.h" // DelugeSampleGeometry
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A per-reader zero-copy streaming cursor over one sample's source residency. Opaque; owned by the
/// backend. Hides clusters/leases/convert/boundaries behind a plain frame-cursor contract: open at a
/// start frame and direction, then repeatedly `window()`/`advance()` to stream, or `seek()` to
/// reposition at random.
typedef struct DelugeSampleReader DelugeSampleReader;

/// Read-intent hint, steering how a reader shares the source residency cache with everything else
/// reading the same sample — in particular the playing voice's own warm cluster set.
///
/// Fixed underlying type (`: uint8_t`, C23 + C++11): this enum crosses the FFI BY VALUE (`open`'s
/// parameter), so its width must be pinned explicitly rather than left to the C++ build's default
/// `int` enum width — mirrors `DelugeRegionState` in `sample_source.h` (see that enum's doc for the
/// fuller rationale, including why every OTHER libdeluge enum stays unfixed).
typedef enum DelugeReadHint : uint8_t {
	/// Share the source residency cache: warm clusters the voice already holds are free hits, and
	/// this reader's own reads are cached exactly like the voice's.
	DELUGE_READ_CACHED = 0,
	/// One-shot whole-sample scan (waveform pre-scan, wavetable build): do NOT evict or pollute the
	/// voice's warm working set for a read that will never repeat.
	DELUGE_READ_SCAN = 1,
} DelugeReadHint;

/// A contiguous run of valid, already-converted native-format frames at a reader's cursor
/// (zero-copy: `frames` points directly into the pinned, resident backing — no data is copied to
/// produce this window).
typedef struct DelugeFrameWindow {
	const void* frames;   ///< pinned, already-converted native-format frames at the cursor (zero-copy)
	uint32_t frame_count; ///< valid frames in this window; for deluge_sample_reader_window, 0 == end-of-audio
	                      ///< (or a hard read failure — see deluge_sample_reader_ok). For deluge_sample_peek,
	                      ///< residency is signalled by `frames` (null == not resident); a non-null `frames`
	                      ///< with 0 here is a valid resident position on the cluster's last partial frame.
} DelugeFrameWindow;

/// Open a reader over `source_id`'s sample residency (the resource-manager Asset id the facade
/// already defines for this sample — see `deluge_streaming_define_asset`; the voice port and this
/// one share the same residency per sample), positioned at `start_frame` and reading in `direction`
/// (+1 forward, -1 reverse). `hint` steers eviction pressure — see `DelugeReadHint`.
DelugeSampleReader* deluge_sample_reader_open(uint32_t source_id, uint64_t start_frame, int8_t direction,
                                              DelugeReadHint hint);

/// The contiguous run of valid, already-converted frames at the cursor. BLOCKS to make the data
/// resident (must-load-now). Self-pins the window's backing while the window is live, so it cannot
/// be evicted mid-read. `frame_count == 0` means end-of-audio (or a hard read failure — see
/// `deluge_sample_reader_ok`).
DelugeFrameWindow deluge_sample_reader_window(DelugeSampleReader* reader);

/// Advance the cursor `frames` frames (in the reader's own `direction`). Crosses cluster boundaries
/// internally: releases the old pin, acquires and pins the next. The next `deluge_sample_reader_window`
/// call reflects the new position.
void deluge_sample_reader_advance(DelugeSampleReader* reader, uint32_t frames);

/// Random-access reposition to `frame` (for the both-directions hop search); like `open()` without
/// re-allocating a reader. Releases any pin the reader currently holds — the next `window()` re-pins
/// at the new position.
void deluge_sample_reader_seek(DelugeSampleReader* reader, uint64_t frame);

/// True unless the last `deluge_sample_reader_window` call failed to make its data resident (card
/// error / out of memory) — distinct from ordinary end-of-audio, which is reported via
/// `DelugeFrameWindow::frame_count == 0` instead.
bool deluge_sample_reader_ok(const DelugeSampleReader* reader);

/// Release the reader and any pin it still holds.
void deluge_sample_reader_close(DelugeSampleReader* reader);

/// Stateless, passive resident-peek: the resident, already-converted frames at `start_frame` of
/// `source_id`'s residency, as a zero-copy run to the CONTAINING CLUSTER's own boundary in
/// `direction`. Takes NO lease, bumps NO recency, triggers NO load on a miss, and prefetches NO
/// neighbouring cluster — safe to call from the audio render thread.
///
/// Residency is signalled by the `frames` pointer, NOT `frame_count` (this makes it the exact
/// equivalent of the C++ facade `peek` it replaces): `frames == NULL` iff there is no valid
/// resident position here — not resident, OR resident but not yet ready (a
/// `deluge_sample_reader_open`+`window()` would block to fill it; this never does). A non-NULL
/// `frames` with `frame_count == 0` means "resident and ready, but the cursor is on the last
/// PARTIAL frame" — read that frame via the pointer with your own byte bounds; do NOT treat a 0
/// count as not-resident.
///
/// Within-cluster only: unlike `deluge_sample_reader_window`, this does NOT serve a
/// boundary-straddling frame via the stitched trailing slack (that serve is proven safe only for
/// a sequential reader's own access pattern; a peek is random-access and takes no pin at all) — a
/// frame whose bytes cross into the next cluster is simply excluded from the run.
///
/// `frames` always points AT `start_frame` itself, in both directions. For `direction == +1`
/// `frame_count` extends toward HIGHER addresses (forward, up to the cluster's own last resident
/// frame); for `-1` it extends toward LOWER addresses (backward, down to the cluster's own first
/// frame) — the caller walks DOWN from `frames` in that case, not up from some earlier start.
DelugeFrameWindow deluge_sample_peek(uint32_t source_id, uint64_t start_frame, int8_t direction);

/// A per-reservation passive lookahead handle. Opaque; owned by the backend. The active twin of
/// `deluge_sample_peek`: where a peek is a single, transient, non-pinning glance, a reservation
/// pins a small forward- or backward-facing WINDOW of cluster residency (holding one real lease per
/// covered cluster) for as long as it stays open — the same shape `kNumClustersLoadedAhead`'s
/// existing lookahead pins already give the timestretch/loop-point paths, generalized behind this
/// handle. `deluge_sample_reserve_move` (a later task) slides the window; `deluge_sample_reserve_close`
/// releases every lease it still holds.
typedef struct DelugeSampleReservation DelugeSampleReservation;

/// How a reservation's covered clusters are loaded when opened (or, in a later task, moved).
///
/// Fixed underlying type (`: uint8_t`, C23 + C++11): this enum crosses the FFI BY VALUE
/// (`deluge_sample_reserve_open`'s parameter), so its width must be pinned explicitly rather than
/// left to the C++ build's default `int` enum width — mirrors `DelugeReadHint` above (see that
/// enum's doc for the fuller rationale).
typedef enum DelugeLoadMode : uint8_t {
	/// Reserve the covered clusters and enqueue them for the async loader; never blocks.
	DELUGE_LOAD_ENQUEUE = 0,
	/// Materialize the covered clusters synchronously before returning.
	DELUGE_LOAD_NOW = 1,
	/// Prefer a synchronous load, falling back to enqueueing under memory pressure.
	DELUGE_LOAD_NOW_OR_ENQUEUE = 2,
} DelugeLoadMode;

/// Open a passive lookahead reservation over `source_id`'s sample residency, pinning up to a fixed
/// small depth of clusters starting at the cluster containing `marker_frame`, walking in `direction`.
/// @param marker_frame a sample-frame index (frame 0 == the sample's first audio-data frame).
/// @param direction +1 forward, -1 reverse.
/// @param load_mode how the covered clusters are loaded — see `DelugeLoadMode`.
DelugeSampleReservation* deluge_sample_reserve_open(uint32_t source_id, uint64_t marker_frame, int8_t direction,
                                                    DelugeLoadMode load_mode);

/// Re-anchor `res` to the cluster containing `marker_frame`, walking in `direction` exactly as
/// `deluge_sample_reserve_open` does.
///
/// Guarded: if `marker_frame` maps to the SAME cluster `res` is already anchored on, this is a
/// no-op — no lease is released or acquired, avoiding per-render-tick lease churn while a marker
/// drifts within its current cluster. Otherwise every lease `res` currently holds is released
/// FIRST, then the covered window is rebuilt from the new head exactly as `open` builds it
/// (release-old-then-acquire-new, matching the C++ recipe this ports — load order matters for
/// eviction-recency fidelity).
/// @param marker_frame a sample-frame index (frame 0 == the sample's first audio-data frame).
/// @param direction +1 forward, -1 reverse.
/// @param load_mode how the covered clusters are (re)loaded — see `DelugeLoadMode`.
void deluge_sample_reserve_move(DelugeSampleReservation* res, uint64_t marker_frame, int8_t direction,
                                DelugeLoadMode load_mode);

/// Release `res` and every lease it still holds.
void deluge_sample_reserve_close(DelugeSampleReservation* res);

/// Convenience for cold one-shots: copy `[start_frame, start_frame + num_frames)` native-format
/// frames of `source_id`'s sample into `dest`. Blocking. Internally an open → window/copy loop →
/// close (one path, not a second implementation) — equivalent to driving the handle API by hand with
/// `DELUGE_READ_CACHED`. `dest_bytes` bounds the write (never writes past it).
/// @return frames actually written (short at end-of-audio).
uint32_t deluge_sample_read(uint32_t source_id, uint64_t start_frame, uint32_t num_frames, void* dest,
                            size_t dest_bytes);

#ifdef __cplusplus
}
#endif
#endif // LIBDELUGE_SAMPLE_READER_H
