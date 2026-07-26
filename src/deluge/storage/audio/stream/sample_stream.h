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

#include "definitions_cxx.hpp"                    // Error, ClusterLoad (CLUSTER_ENQUEUE et al.)
#include "memory/fast_allocator.h"                // deluge::memory::fast_allocator
#include "model/sample/sample_cluster.h"          // SampleCluster, the residency table's element type
#include "storage/audio/stream/chunk_residency.h" // deluge_streaming_define_asset() + the chunk Source callbacks
#include "storage/audio/stream/read_source.h"
#include "util/segmented_vector.h" // deluge::SegmentedVector
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

class Sample;
struct StreamedChunk;

namespace deluge::audio::stream {

/// @brief Per-`Sample` orchestrator for streaming a sample's audio off the SD card in FAT-cluster-sized
///        chunks.
///
/// Every `Sample` owns exactly one `SampleStream` (as a member). It is the sole owner of that sample's
/// streaming state:
///   - the **`table_` vector**: one `SampleCluster` entry per cluster, holding the entry's waveform
///     min/max overview cache. Its `StreamedChunk*` field is now a DEAD residency mirror — residency
///     lives entirely in the resource manager, reached via chunk_at()/get_cluster() → the manager
///     peek; the field + the vector itself are retired in a later slice (SR3e);
///   - the open **efatfs read handle** used to pull cluster bytes off the card (R1's streaming read
///     path; see `efatfs_handle_`);
///   - the sample's **resource-manager Asset** id cache (`resource_asset_id_`); the Asset's
///     *definition* + the materialize / construct callbacks the manager invokes now live in
///     `chunk_residency.cpp` (`deluge_streaming_define_asset()`), not here (eviction needs no callback —
///     the manager frees the trivially-destructible slab chunk itself);
///   - **read-source selection** — the single place a cluster read is issued from (make_read_source()).
///
/// Callers obtain a cluster through get_cluster() (which takes a manager lease) or peek at a resident
/// one through chunk_at(); no caller indexes the table directly, and none branches on how a cluster's
/// bytes are read.
///
/// @note **Real-time contract.** The audio render thread never calls into `SampleStream` per sample —
///       it reads already-resident chunk bytes by pointer from its own lookahead array. It only touches
///       this class at cluster-boundary crossings, to enqueue the next cluster, and get_cluster() with
///       CLUSTER_ENQUEUE never blocks on I/O.
///
/// @warning Non-copyable and non-movable: it holds a back-reference to its owning `Sample` and owns the
///          read stream and Asset. This transitively makes `Sample` non-movable.
class SampleStream {
public:
	/// @param sample The owning sample; the reference is retained for this object's whole lifetime.
	explicit SampleStream(Sample& sample) : sample_{sample} {}

	SampleStream(const SampleStream&) = delete;
	SampleStream& operator=(const SampleStream&) = delete;
	SampleStream(SampleStream&&) = delete;
	SampleStream& operator=(SampleStream&&) = delete;

	/// Releases the Asset as a defensive backstop; a no-op on the normal path, where `~Sample` has
	/// already released it (see release_asset() for why that explicit, earlier release is required).
	~SampleStream() { release_asset(); }

	/// @return The owning `Sample` (the back-reference this stream was constructed with). Used by the
	///         region-port cursor bridge (`deluge_sample_stream_asset_id()`, sample_stream.cpp) to reach
	///         `deluge_streaming_define_asset()`, which takes a `Sample*` rather than a `SampleStream*`.
	[[nodiscard]] Sample& sample() { return sample_; }

	/// @name Resource-manager Asset lifecycle
	/// @{

	/// @return This sample's Asset id, or DELUGE_RESOURCE_NO_ASSET if not yet defined.
	[[nodiscard]] uint32_t resource_asset_id() const { return resource_asset_id_; }

	/// @brief Set this sample's cached Asset id. Storage only -- the id cache stays a `SampleStream`
	///        member for SR3c even though the asset-*definition* logic that assigns it has moved to
	///        `deluge_streaming_define_asset()` (chunk_residency.cpp), which reads/writes it via this
	///        setter and the getter above (through `sample->stream()`).
	void set_resource_asset_id(uint32_t id) { resource_asset_id_ = id; }

	/// @brief Release the Asset, freeing every resident cluster's backing first.
	///
	/// Releasing frees the manager's slab slot for each resident Chunk directly — there is no evict
	/// callback (a `StreamedChunk` is a trivially-destructible POD in the slab). Idempotent (a no-op if
	/// the Asset was never defined or is already released).
	/// @warning Release before the `Sample`/`SampleStream` is destroyed so the manager frees this
	///          asset's resident backings rather than orphaning them; `~Sample` calls it explicitly.
	///          `~Sample` therefore calls this explicitly, before any `Sample` member (including this
	///          object, and hence `table_`) is destructed. That ordering is load-bearing and is why the
	///          call is not left to `~SampleStream` alone.
	void release_asset();

	/// @}
	/// @name Read stream
	/// @{

	/// @brief Open the efatfs read handle used for every subsequent cluster read.
	///
	/// Opens the handle once (typically from `AudioFileManager::buildAudioFileFromCard`) for the rest of
	/// the sample's life. R1: efatfs is the streaming read path outright — there is no C-FatFS fallback.
	/// @param path Path to open.
	/// @return `true` on success; `false` if the open failed, leaving the stream disengaged (callers map
	///         this to `Error::FILE_NOT_FOUND`).
	bool open_read_stream(std::string_view path);

	/// @brief Build this sample's read source.
	///
	/// The single point a cluster read is issued from; no caller branches on it.
	/// @return An `EfatfsReadSource` over the efatfs read handle (0 if none is open yet -- e.g. a
	///         still-recording sample, which has no reader at all; see the .cpp for why a handle-0
	///         read is safe and simply fails).
	[[nodiscard]] std::unique_ptr<ReadSource> make_read_source();

	/// @}
	/// @name Cluster residency
	/// @{

	/// @brief Reconstruct @p cluster's data: read its sectors from the read source, convert if the
	///        sample's raw format isn't native, and stitch in the neighbouring clusters' boundary bytes.
	///
	/// The pure per-cluster reconstruction primitive underneath get_cluster() and the resource-manager
	/// materialize callback (cluster_materialize()) — no orchestration (leasing, the loading queue) here.
	/// @warning Must be called on the cluster's OWN sample's stream, i.e. on `cluster.sample->stream()`
	///          (`this == &cluster.sample->stream()`) — make_read_source(), chunk_at() and num_clusters()
	///          are called on `*this` below to reach `cluster.sample`'s read source and residency table
	///          (for the neighbour-edge stitch), not some other sample's. All callers uphold this.
	/// @param cluster           The chunk to reconstruct (already leased/resident, not yet loaded).
	/// @param min_reasons_after ALPHA/BETA-only: the expected post-call lease-count floor, checked by the
	///                          freeze sanity-checks below (unused in a release build).
	/// @return `true` if the cluster was successfully read and stitched; `false` on a read failure (the
	///         cluster is left unloaded).
	bool read_cluster_data(StreamedChunk& cluster, [[maybe_unused]] int32_t min_reasons_after);

	/// @brief Ensure cluster @p index is resident (or scheduled to load) and return it, taking a lease.
	///
	/// The dispatch always takes a manager lease on a non-null return; the caller is responsible for
	/// releasing it. Behaviour depends on @p load_instruction:
	///   - CLUSTER_DONT_LOAD — pin or construct the cluster without any I/O and hold it *dirty* (the
	///     recorder / convert write target); the manager will not evict the unflushed data until it is
	///     written to the card.
	///   - CLUSTER_ENQUEUE — construct and lease immediately with no I/O, then schedule the read on the
	///     background loader. Returns at once; the returned chunk may still be unloaded. Never blocks —
	///     this is the real-time-safe path.
	///   - CLUSTER_LOAD_IMMEDIATELY / CLUSTER_LOAD_IMMEDIATELY_OR_ENQUEUE — acquire the cluster,
	///     materializing it on a miss (which may block on I/O), and read a prefetched-but-unloaded hit
	///     synchronously. On a read failure the `_OR_ENQUEUE` form falls back to the loader; the plain
	///     form returns `nullptr`, leaving the cluster resident and leased so the caller can retry.
	/// @param index           Cluster index within the sample.
	/// @param load_instruction One of the `CLUSTER_*` load modes above.
	/// @param priority_rating  Loader priority; used only when the read is enqueued.
	/// @param error            If non-null, set to `Error::NONE` on entry and overwritten only on a
	///                         failure path.
	/// @return The resident (or scheduled) chunk, leased; `nullptr` on failure.
	StreamedChunk* get_cluster(uint32_t index, int32_t load_instruction = CLUSTER_ENQUEUE,
	                           uint32_t priority_rating = 0xFFFFFFFF, Error* error = nullptr);

	/// @brief Peek at the resident chunk for @p index without taking a lease.
	///
	/// For read-only, non-owning uses (stitching neighbouring cluster edges, perc-cache fills, crossfade
	/// sampling, debug checks).
	/// @return The resident chunk, or `nullptr` if @p index is not currently resident.
	[[nodiscard]] StreamedChunk* chunk_at(uint32_t index) const;

	/// @brief Access the raw table entry for @p index (its waveform min/max cache).
	[[nodiscard]] SampleCluster& entry(uint32_t index);
	/// @copydoc entry(uint32_t)
	[[nodiscard]] const SampleCluster& entry(uint32_t index) const;

	/// @return This stream's embedded-fatfs file handle, or 0 if none is open (the flag-off C-FatFS
	///         path, or a stream not yet opened via the efatfs read path). Set by open_read_stream()
	///         under the `efatfs_streaming` build; consulted by begin_fill() to route the read.
	[[nodiscard]] uint32_t efatfs_handle() const { return efatfs_handle_; }

	/// @return The number of entries in the residency table.
	[[nodiscard]] size_t num_clusters() const;

	/// @brief Resize the residency table to @p n entries (grows the table as a recording extends).
	void resize(size_t n);

	/// @brief Reserve the residency table's segment-pointer index for up to @p num_clusters entries.
	///
	/// Reserves index capacity only (allocates no cluster entries) so subsequent growth up to
	/// @p num_clusters will not reallocate the segment-pointer index. Required BEFORE concurrent
	/// (recording) growth: the recorder's audio thread grows the table via resize() while the fiber
	/// reads it via chunk_at(); the `SegmentedVector` keeps element addresses stable, but its pointer
	/// index must be pre-reserved to the final capacity, single-threaded, to stay stable under that
	/// concurrent growth (see docs/dev/known-concurrency-bugs.md B2).
	void reserve(size_t num_clusters);

	/// @brief Erase every entry from @p index to the end (shrinks the table on record-stop / truncate).
	void erase_from(size_t index);

	/// @}

	/// @brief Register (or refresh) this asset's streaming fill-context with the resource manager.
	///
	/// A no-op if the Asset isn't defined yet (`resource_asset_id_ == DELUGE_RESOURCE_NO_ASSET`) or
	/// there is no manager. Called from `deluge_streaming_define_asset()` (chunk_residency.cpp) right
	/// after the Asset is defined, and again from open_read_stream() in case the efatfs handle becomes
	/// known only afterwards (see their call sites for why both are needed). Kept as a `SampleStream`
	/// method (not relocated alongside the asset-definition core) because it is stream/geometry-coupled
	/// -- it reads `efatfs_handle_` directly -- and open_read_stream() needs to call it too.
	void register_fill_context();

private:
	Sample& sample_;

	/// This sample's Asset id, DELUGE_RESOURCE_NO_ASSET until defined on first use. The sentinel is a
	/// literal (rather than including `deluge_resource.h`) because this header is pulled in transitively
	/// by every includer of `sample.h`.
	uint32_t resource_asset_id_ = 0xFFFFFFFFu;

	/// This stream's embedded-fatfs file handle (0 = none open). Defaults to 0 so the flag-off
	/// C-FatFS sector path is unaffected; the `efatfs_streaming` read path (Task 6) sets it in
	/// open_read_stream() via deluge_efatfs_open() and clears it on close.
	uint32_t efatfs_handle_ = 0;

	/// The cluster residency table: one passive `SampleCluster` per cluster of the file. This is the
	/// sole owner of the table. A stable-address `SegmentedVector` (not a `std::vector`) so that growth
	/// during recording never moves existing entries under a concurrent reader on another thread
	/// (see docs/dev/known-concurrency-bugs.md, B2). The third template argument pins segment storage to
	/// the fast/SRAM-preferred heap (`fast_allocator`), matching the retired `fast_vector`'s placement —
	/// do not drop it back to the `std::allocator` default, which would move the table to SDRAM.
	/// @warning `~SampleStream` destructs `table_` only after `~Sample`'s explicit release_asset() has
	///          nulled every entry's `cluster` pointer; see release_asset().
	deluge::SegmentedVector<SampleCluster, 256, deluge::memory::fast_allocator> table_{};
};

} // namespace deluge::audio::stream
