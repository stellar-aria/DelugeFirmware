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

/// One acquired, pinned, borrowed region of resident sample data.
typedef struct DelugeSampleRegion {
	void* payload_base;      ///< StreamedChunk payload().data() for the resident cluster (pinned)
	uint32_t region_index;   ///< cluster index this region corresponds to
	uint32_t resident_bytes; ///< valid payload bytes in this region (Cluster::size, or short for the last)
	uint64_t lease;          ///< opaque pin token; pass to deluge_sample_region_release
} DelugeSampleRegion;

/// Open a per-reader cursor. `stream_backing` identifies the sample's residency
/// (SR1: a `deluge::audio::stream::SampleStream*`; SR2: an opaque source id).
DelugeSampleSource* deluge_sample_source_open(void* stream_backing, DelugeSampleGeometry geometry);

/// Make the region containing cluster `index` resident-or-scheduled, pin it, and return it.
/// Non-blocking (CLUSTER_ENQUEUE semantics). Prefetches the next cluster in `direction`.
/// Returns false = NotReady: the region is not resident yet (a fetch was scheduled); `out` untouched.
/// `direction` is +1 (forward) or -1 (reverse) and selects which neighbour is prefetched.
bool deluge_sample_region_acquire(DelugeSampleSource* src, uint32_t index, int8_t direction, uint32_t priority,
                                  DelugeSampleRegion* out);

/// Drop a pin taken by acquire. Safe to call with a lease of 0 (no-op).
void deluge_sample_region_release(DelugeSampleSource* src, uint64_t lease);

/// Close the cursor, releasing any leases it still holds (current + prefetch).
void deluge_sample_source_close(DelugeSampleSource* src);

#ifdef __cplusplus
}
#endif
#endif // LIBDELUGE_SAMPLE_SOURCE_H
