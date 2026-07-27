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
#include "storage/audio/stream/chunk_residency.h" // deluge_streaming_define_asset() + the chunk Source callbacks
#include "storage/audio/stream/read_source.h"
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
///   - residency itself, which lives entirely in the resource manager and is reached via the
///     `deluge::audio::stream` facade's prefetch()/load_now()/request() (leasing dispatch) or its
///     peek() free function (non-leasing peek) → the manager peek/acquire (SR3e retired the former
///     app-side residency-table mirror; there is nothing left on `SampleStream` for a caller to index
///     directly);
///   - the open **efatfs read handle** used to pull cluster bytes off the card (R1's streaming read
///     path; see `efatfs_handle_`);
///   - the sample's **resource-manager Asset** id cache (`resource_asset_id_`); the Asset's
///     *definition* + the materialize / construct callbacks the manager invokes now live in
///     `chunk_residency.cpp` (`deluge_streaming_define_asset()`), not here (eviction needs no callback —
///     the manager frees the trivially-destructible slab chunk itself);
///   - **read-source selection** — the single place a cluster read is issued from (make_read_source()).
///
/// Callers obtain a cluster through the `deluge::audio::stream` facade's prefetch()/load_now()/request()
/// (which take a manager lease) or peek at a resident one through its peek() free function (no lease);
/// none branches on how a cluster's bytes are read.
///
/// @note **Real-time contract.** The audio render thread never calls into `SampleStream` per sample —
///       it reads already-resident chunk bytes by pointer from its own lookahead array. It only touches
///       this class at cluster-boundary crossings, to enqueue the next cluster, and the facade's
///       prefetch() (CLUSTER_ENQUEUE) never blocks on I/O.
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
	///          asset's resident backings rather than orphaning them; `~Sample` calls it explicitly,
	///          before any `Sample` member is destructed, rather than leaving it to `~SampleStream`
	///          alone.
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
	/// The pure per-cluster reconstruction primitive underneath the residency dispatch and the
	/// resource-manager materialize callback (cluster_materialize()) — no orchestration (leasing, the
	/// loading queue) here.
	/// @warning Must be called on the cluster's OWN sample's stream, i.e. on `cluster.sample->stream()`
	///          (`this == &cluster.sample->stream()`) — make_read_source() is called on `*this` below,
	///          and the finish-fill step it hands off to peeks `cluster.sample`'s resident neighbour
	///          clusters (for the neighbour-edge stitch), not some other sample's. All callers uphold
	///          this.
	/// @param cluster           The chunk to reconstruct (already leased/resident, not yet loaded).
	/// @param min_reasons_after ALPHA/BETA-only: the expected post-call lease-count floor, checked by the
	///                          freeze sanity-checks below (unused in a release build).
	/// @return `true` if the cluster was successfully read and stitched; `false` on a read failure (the
	///         cluster is left unloaded).
	bool read_cluster_data(StreamedChunk& cluster, [[maybe_unused]] int32_t min_reasons_after);

	/// @return This stream's embedded-fatfs file handle, or 0 if none is open (the flag-off C-FatFS
	///         path, or a stream not yet opened via the efatfs read path). Set by open_read_stream()
	///         under the `efatfs_streaming` build; consulted by begin_fill() to route the read.
	[[nodiscard]] uint32_t efatfs_handle() const { return efatfs_handle_; }

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
};

} // namespace deluge::audio::stream
