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

#ifndef LIBDELUGE_SAMPLE_SOURCE_H
#define LIBDELUGE_SAMPLE_SOURCE_H
#include "types.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/// Per-reader streaming cursor over one sample's residency. Opaque; owned by the backend.
typedef struct DelugeSampleSource DelugeSampleSource;

/// Immutable per-sample audio geometry, parsed once at load (above the port).
typedef struct DelugeSampleGeometry {
	uint32_t audio_data_start_bytes;  ///< byte offset of audio data within the file
	uint64_t audio_data_length_bytes; ///< length of the audio data, bytes
	uint32_t cluster_size_bytes;      ///< Cluster::size
	uint8_t byte_depth;               ///< bytes per channel-sample
	uint8_t num_channels;
	uint8_t raw_data_format; ///< RawDataFormat, opaque to the port
} DelugeSampleGeometry;

/// Residency outcome of a region query. Distinguishes the two outcomes the plain boolean
/// `deluge_sample_region_acquire` collapses into `false`: "not loaded YET" (worth waiting for) and
/// "could not be reserved at all" (nothing is coming — give up).
///
/// Each state carries a LEASE POLICY, which is part of the contract, not an implementation detail.
/// Numbered from 1 (not 0) so `if (state)` cannot be misread as a boolean — always compare against
/// a named constant.
typedef enum DelugeRegionState {
	/// Resident AND loaded. `out` is filled and the source holds the pin backing it; the caller may
	/// read `payload_base` until it releases the lease, closes, or acquires a different region.
	DELUGE_REGION_READY = 1,
	/// Reserved, leased and scheduled, but the data has not landed yet. `out` is NOT filled.
	/// The source RETAINS the lease on the region across the call, so the background fill keeps
	/// progressing (and the region cannot be stolen) while the caller defers and retries. Retrying
	/// the same index is idempotent — it does not accumulate leases. The retained lease is dropped
	/// by the next acquire on this source or by `deluge_sample_source_close`.
	DELUGE_REGION_LOADING,
	/// The region could not be reserved or constructed at all (out of RAM / nothing stealable / out
	/// of range / null source). `out` is NOT filled and NOTHING is left leased for it — no fill is
	/// in flight, so retrying gains the caller nothing.
	DELUGE_REGION_UNAVAILABLE,
} DelugeRegionState;

/// One acquired, pinned, borrowed region of resident sample data.
typedef struct DelugeSampleRegion {
	void* payload_base;      ///< StreamedChunk payload().data() for the resident cluster (pinned)
	uint32_t region_index;   ///< cluster index this region corresponds to
	uint32_t resident_bytes; ///< valid payload bytes in this region (Cluster::size, or short for the last)
	uint64_t lease;          ///< opaque pin token; retain/release the independent pin with it
} DelugeSampleRegion;

/// Open a per-reader cursor. `stream_backing` identifies the sample's residency (currently a
/// `deluge::audio::stream::SampleStream*`; an opaque source id once the backend moves to Rust).
DelugeSampleSource* deluge_sample_source_open(void* stream_backing, DelugeSampleGeometry geometry);

/// Make the region containing cluster `index` resident-or-scheduled, pin it, and report which.
/// Non-blocking (CLUSTER_ENQUEUE semantics), allocation-free. On DELUGE_REGION_READY the next
/// cluster in `direction` is also prefetched; `direction` is +1 (forward) or -1 (reverse).
///
/// `out` is filled only on DELUGE_REGION_READY, and only then does the source release the region it
/// previously handed out (so the caller need not release before re-acquiring). The other two states
/// leave the standing current region alone — a deferring caller keeps reading what it already has.
/// See DelugeRegionState for each state's lease policy; in particular DELUGE_REGION_LOADING keeps
/// the region leased so the fill continues across the caller's defer/retry cycle.
///
/// If `index` is exactly the standing prefetched neighbour, that lease is PROMOTED into this call's
/// result rather than re-fetched — which, on a LOADING outcome, empties the prefetch slot and moves
/// the reservation to `pending` (see deluge_sample_region_state: querying `index` afterwards still
/// finds it, now via `pending` instead of `prefetch`, so the query stays truthful).
DelugeRegionState deluge_sample_region_acquire_ex(DelugeSampleSource* src, uint32_t index, int8_t direction,
                                                  uint32_t priority, DelugeSampleRegion* out);

/// Boolean form of deluge_sample_region_acquire_ex: true == DELUGE_REGION_READY. Callers that
/// cannot act on the LOADING/UNAVAILABLE distinction (both are simply "NotReady") use this.
bool deluge_sample_region_acquire(DelugeSampleSource* src, uint32_t index, int8_t direction, uint32_t priority,
                                  DelugeSampleRegion* out);

/// Residency of `index` as tracked by this cursor RIGHT NOW, WITHOUT acquiring anything (no lease
/// taken, no fetch scheduled, no state changed). This consults every reservation the cursor is
/// currently holding -- `current`, the standing `prefetch`, and any retained `pending` LOADING
/// reservation from a previous acquire_ex call -- and reports on whichever of them is tracking
/// `index`, by matching that reservation's OWN cluster index (never by assuming `index` is "the
/// neighbour" or "the last call's subject").
///
/// Because the match is by index, this cannot conflate two different clusters the way an unindexed
/// query would: after an acquire_ex(0) READY followed by an acquire_ex(5) LOADING (index 5 jumped
/// ahead of the standing prefetch), deluge_sample_region_state(src, 6) still reports on the TRUE
/// neighbour (index 6, tracked by `prefetch` if in range, else UNAVAILABLE) — it does not fall back
/// to reporting index 5's LOADING state just because index 5 is what the cursor most recently
/// resolved.
///
/// Returns:
///   * DELUGE_REGION_READY   -- `index` is tracked by this cursor and its data has landed.
///   * DELUGE_REGION_LOADING -- `index` is tracked by this cursor (as `pending` or `prefetch`) but
///                              has not landed yet; a fill is in flight for it.
///   * DELUGE_REGION_UNAVAILABLE -- this cursor has NOTHING in flight or resident for `index` right
///                              now (it isn't `current`, `pending`, or `prefetch`). This means only
///                              "ask me again after a different acquire_ex call, if you want to know
///                              about this index" — it is NOT a claim that `index` can never load;
///                              a fresh acquire_ex(index, ...) may still succeed. `src == nullptr`
///                              also reports UNAVAILABLE.
DelugeRegionState deluge_sample_region_state(const DelugeSampleSource* src, uint32_t index);

/// Take/drop an INDEPENDENT pin on the region's chunk, keyed on the opaque `lease` token alone (no
/// cursor). Separate from the cursor's current/prefetch/pending pins (those are managed by acquire/close).
/// `lease == 0` is a no-op in both directions. The token comes from a READY `DelugeSampleRegion::lease`.
void deluge_sample_region_retain(uint64_t lease);
void deluge_sample_region_release(uint64_t lease);

/// Close the cursor, releasing any leases it still holds (current + prefetch).
void deluge_sample_source_close(DelugeSampleSource* src);

#ifdef __cplusplus
}
#endif
#endif // LIBDELUGE_SAMPLE_SOURCE_H
