/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#pragma once

#include "definitions_cxx.hpp" // Error, ClusterLoad (CLUSTER_ENQUEUE et al.)
#include "io/stream.hpp"
#include "libdeluge/stream_io.h" // DelugeStreamMode
#include "storage/audio/stream/read_source.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

class Sample;
class SampleCluster;
struct StreamedChunk;

// The audio-stream module's per-Sample orchestrator (design spec §4/§8 step 3). This is a
// COEXISTENCE migration (Phase 4, Task 1 of the plan): SampleStream owns the read-stream handle,
// the resource-manager Asset + its materialize/construct/evict callbacks, and the ReadSource
// selection. The cluster residency table (Sample::clusters) stays on Sample until a later task
// internalizes it here (see docs/superpowers/plans/2026-07-15-audio-stream-phase4-sample-stream.md).
namespace deluge::audio::stream {

class SampleStream {
public:
	explicit SampleStream(Sample& sample) : sample_{sample} {}

	// Owned member holding a back-reference plus an owned stream/Asset -- not copyable or movable.
	SampleStream(const SampleStream&) = delete;
	SampleStream& operator=(const SampleStream&) = delete;
	SampleStream(SampleStream&&) = delete;
	SampleStream& operator=(SampleStream&&) = delete;

	// Defensive: if ~Sample's explicit ordered stream_.release_asset() call already ran (the normal
	// path -- see sample.cpp's ~Sample), this is a no-op. Do NOT rely on this for correctness: the
	// asset must be released BEFORE the residency table's entries destruct, which member-destruction
	// order alone cannot guarantee here (see the release_asset() doc comment).
	~SampleStream() { release_asset(); }

	/// Lazily define this Sample's resource-manager Asset (its SAMPLE clusters are the Asset's
	/// Chunks: materialize = readClusterData, on_evict = drop + ~StreamedChunk). Returns the asset
	/// id. The manager is the sole SDRAM evictor, so this never returns NO_ASSET -- a missing
	/// manager / full asset table is fatal (FREEZE), no legacy fallback.
	uint32_t ensure_resource_asset();
	[[nodiscard]] uint32_t resource_asset_id() const { return resource_asset_id_; }

	/// Releases the Asset (if defined), evicting every resident cluster via the manager's on_evict
	/// callback first -- this nulls the Sample's clusters[i].cluster entries. Idempotent (a no-op if
	/// already released / never defined). ~Sample calls this explicitly, at the very top of its body,
	/// BEFORE the cluster table (still owned by Sample this task) destructs, so on_evict always finds
	/// live SampleCluster entries to null. That ordering is load-bearing and is NOT guaranteed by
	/// ~SampleStream's own (defensive) call to this, since Sample::stream_ and Sample::clusters are
	/// both plain data members and destruct in declaration order regardless of which is declared
	/// first -- the explicit call in ~Sample's body is what guarantees correctness.
	void release_asset();

	/// Opens the read-stream handle used by every subsequent readClusterData call for this Sample's
	/// lifetime (AudioFileManager::buildAudioFileFromCard, DELUGE_STREAM_READ mode), and -- since the
	/// residency table is still owned by Sample this task -- populates each of the first
	/// `num_clusters` entries' sdAddress from the newly-opened stream (best-effort; only meaningful on
	/// FatFS-family backends). Returns false, leaving the stream disengaged, if the underlying open
	/// fails; the caller maps that to Error::FILE_NOT_FOUND, matching prior behavior.
	bool open_read_stream(std::string_view path, DelugeStreamMode mode, uint32_t num_clusters);
	[[nodiscard]] bool has_read_stream() const { return read_stream_.has_value(); }

	/// Selects the read source from this Sample's backing state: an open read stream (normal,
	/// loaded-from-card sample) -> StreamReadSource; otherwise (a recording still being written) ->
	/// BlockReadSource. This is the single place the block-vs-stream decision is made -- no caller
	/// branches on it.
	[[nodiscard]] std::unique_ptr<ReadSource> make_read_source() const;

	// === Cluster residency dispatch + table accessors (Phase 4, Task 2; design plan DD3) ========
	// Additive over the still-Sample-owned residency table (`Sample::clusters`) during the
	// COEXISTENCE migration -- SampleCluster::getCluster forwards here so not-yet-migrated callers
	// (the recorder, Task 3; SampleHolder + the RT reader, Task 4) keep compiling unchanged. Bodies
	// live in the .cpp: they need `Sample` complete (only forward-declared above, since `sample.h`
	// includes this header).

	/// The getCluster dispatch (moved verbatim from the former SampleCluster::getCluster,
	/// sample_cluster.cpp:63-146, rebased onto `sample_.clusters[index]`): CLUSTER_DONT_LOAD
	/// pins/constructs without I/O (recorder write target, held dirty); CLUSTER_ENQUEUE constructs +
	/// leases and schedules an async read on the loader (the audio thread never blocks); CLUSTER_
	/// LOAD_IMMEDIATELY[_OR_ENQUEUE] acquires (materializing on a miss, which may block on I/O) and
	/// reads synchronously on an unloaded hit. Adds a manager lease on every non-null return. `error`,
	/// if non-null, is set to Error::NONE up front and only overwritten on a failure path (matches the
	/// original's out-param contract; `std::expected` is out of scope here -- see plan DD4).
	StreamedChunk* get_cluster(uint32_t index, int32_t load_instruction = CLUSTER_ENQUEUE,
	                           uint32_t priority_rating = 0xFFFFFFFF, Error* error = nullptr);

	/// Resident chunk pointer for `index`, no lease taken (stitch neighbor edges, fillPercCache,
	/// crossfade sampling, ALPHA bug-checks). nullptr if not currently resident.
	[[nodiscard]] StreamedChunk* chunk_at(uint32_t index) const;

	/// The raw table entry (waveform min/max cache, recorder re-fetch-after-write).
	[[nodiscard]] SampleCluster& entry(uint32_t index);
	[[nodiscard]] const SampleCluster& entry(uint32_t index) const;

	[[nodiscard]] uint32_t sd_address_at(uint32_t index) const;
	void set_sd_address_at(uint32_t index, uint32_t sector);

	[[nodiscard]] size_t num_clusters() const;
	void resize(size_t n);
	/// Erases every entry from `index` to the end (recorder table shrink on record-stop/truncate).
	void erase_from(size_t index);

	/// ALPHA debug bug-check: FREEZEs if the entry at `index` still holds a manager lease. Every call
	/// site is currently commented out (see sample.cpp/sample_recorder.cpp); kept for parity with the
	/// former SampleCluster::ensureNoReason, deletion deferred to Task 5 (plan DD4).
	void ensure_no_reason(uint32_t index);

private:
	// === Resource-manager Source for SAMPLE clusters =========================================
	// These are the materialize / on_evict callbacks the manager calls for a Sample's Asset. A
	// Chunk's backing is a uniform slab slot; materialize placement-news a StreamedChunk into it and
	// fills it via the Step-1 reader; on_evict drops the Sample's pointer and destructs the
	// StreamedChunk (the manager frees the slot). The Chunk records its chunk-slot handle
	// (resource_slot) here -- the manager already leased the slot before calling us (leases >= 1), so
	// slot_of(dest) is valid and deluge::cluster::lease_count(resource_slot) reports the reason count
	// immediately. Bodies are a verbatim move from the former file-static clusterMaterialize /
	// clusterConstruct / clusterEvict in sample.cpp -- `owner` is still the Sample* (not the
	// SampleStream*), so the callback contract with the resource manager (defined in
	// ensure_resource_asset() below, which passes `&sample_` as owner) is unchanged.
	static bool cluster_materialize(void* ctx, void* owner, uint32_t index, void* dest, size_t len);
	// Async construct (the manager's `request`/prefetch path): init the StreamedChunk object but do
	// NOT read the data -- an external loader (loadAnyEnqueuedClusters -> readClusterData) fills it
	// later, so the audio thread never blocks on I/O. Mirror of cluster_materialize minus the read;
	// the Sample's pointer is set immediately so the requester holds a valid (loaded==false) chunk.
	static void cluster_construct(void* ctx, void* owner, uint32_t index, void* dest);
	static void cluster_evict(void* ctx, void* owner, uint32_t index);

	Sample& sample_;
	// DELUGE_RESOURCE_NO_ASSET until defined on first stream. 0xFFFFFFFF is kept as a literal here
	// (rather than pulling in deluge_resource.h) since this header is reached transitively by every
	// includer of sample.h.
	uint32_t resource_asset_id_ = 0xFFFFFFFFu;
	// Opened once by AudioFileManager::buildAudioFileFromCard, used by readClusterData for every
	// cluster read thereafter; closed (via the optional's destructor) when this SampleStream is
	// destructed. Disengaged for a Sample that isn't backed by a stream_io.h read (e.g. one still
	// being recorded). `mutable`: make_read_source() is logically const (it doesn't change which
	// source a caller would observe), but StreamReadSource needs a mutable Stream& to read through.
	mutable std::optional<deluge::io::Stream> read_stream_;
};

} // namespace deluge::audio::stream
