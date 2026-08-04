#include "io/stream.hpp"

#include "libdeluge/streaming_fill.h" // deluge_streaming_efatfs_active

#include <cstdint>

namespace deluge::io {

namespace {

// Mirrors file.cpp's box_file_handle boxing (see its doc for the full rationale) -- the efatfs
// backend's persistent stream-write `u32` handle is boxed into the opaque `DelugeStream*`,
// `handle + 1` so slot 0 doesn't box to null (Stream's RAII `close()` guards on
// `handle_ != nullptr`).
DelugeStream* box_stream_handle(uint32_t handle) {
	return reinterpret_cast<DelugeStream*>(static_cast<uintptr_t>(handle) + 1);
}
uint32_t unbox_stream_handle(DelugeStream* handle) {
	return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle) - 1);
}

} // namespace

std::expected<Stream, Status> Stream::open(std::string_view path, DelugeStreamMode mode) {
	if (deluge_streaming_efatfs_active()) {
		// One persistent write context for the whole recording -- every subsequent
		// write_at/read_at_via/close on this Stream reuses it; no per-call reopen.
		uint32_t handle = 0;
		if (!deluge_efatfs_stream_open(path.data(), static_cast<uint8_t>(mode), &handle)) {
			return std::unexpected(Status::ERR);
		}
		return Stream(box_stream_handle(handle));
	}
	DelugeStream* handle = nullptr;
	DelugeStatus status = deluge_stream_open(path.data(), mode, &handle);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return Stream(handle);
}

std::expected<uint32_t, Status> Stream::write_at(uint32_t byte_offset, std::span<const std::byte> buffer) {
	uint32_t out_written = 0;
	if (deluge_streaming_efatfs_active()) {
		// No-flush write -- see stream_io.h's `deluge_efatfs_stream_write_at` doc. The persisted
		// size/mtime edit is flushed to disk on `close()` (or an explicit mid-recording flush, not
		// exercised by this port).
		if (!deluge_efatfs_stream_write_at(unbox_stream_handle(handle_), byte_offset, buffer.data(),
		                                   static_cast<uint32_t>(buffer.size()), &out_written)) {
			return std::unexpected(Status::ERR);
		}
		return out_written;
	}
	DelugeStatus status =
	    deluge_stream_write_at(handle_, byte_offset, buffer.data(), static_cast<uint32_t>(buffer.size()), &out_written);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_written;
}

std::expected<uint32_t, Status> Stream::read_at_via(uint32_t byte_offset, std::span<std::byte> dst) {
	uint32_t out_read = 0;
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_stream_read_at_via(unbox_stream_handle(handle_), byte_offset, dst.data(),
		                                      static_cast<uint32_t>(dst.size()), &out_read)) {
			return std::unexpected(Status::ERR);
		}
		return out_read;
	}
	DelugeStatus status =
	    deluge_stream_read_at(handle_, byte_offset, dst.data(), static_cast<uint32_t>(dst.size()), &out_read);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_read;
}

std::expected<void, Status> Stream::truncate(uint32_t new_size) {
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_stream_truncate(unbox_stream_handle(handle_), new_size)) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_stream_truncate(handle_, new_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

std::expected<uint32_t, Status> Stream::size() {
	uint32_t out_size = 0;
	if (deluge_streaming_efatfs_active()) {
		if (!deluge_efatfs_stream_size(unbox_stream_handle(handle_), &out_size)) {
			return std::unexpected(Status::ERR);
		}
		return out_size;
	}
	DelugeStatus status = deluge_stream_size(handle_, &out_size);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_size;
}

std::expected<uint32_t, Status> Stream::sector_of(uint32_t cluster_index) {
	uint32_t out_sector = 0;
	if (deluge_streaming_efatfs_active()) {
		// The efatfs backend has no `sector_of` accessor: `SampleRecorder::writeCluster` resolves the
		// write-side "most recently written cluster" a different way, and this function's only
		// remaining caller (AudioFileManager's read-mode cold-path "did this file move on the
		// reinserted card" identity check, audio_file_manager.cpp) opens fresh, for reading, with
		// nothing written -- there is nothing such an accessor could resolve for it anyway (it could
		// only ever answer for a context's just-completed write). Fail explicitly rather than
		// resolving a sector: `handle_` on an efatfs-opened Stream is a boxed opaque `u32`, not a real
		// `DelugeStream*`, so it must never reach `deluge_stream_sector_of` below.
		return std::unexpected(Status::UNSUPPORTED);
	}
	DelugeStatus status = deluge_stream_sector_of(handle_, cluster_index, &out_sector);
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return out_sector;
}

std::expected<void, Status> Stream::close() {
	if (deluge_streaming_efatfs_active()) {
		bool ok = deluge_efatfs_stream_close(unbox_stream_handle(handle_));
		handle_ = nullptr; // matters even on error: don't let the destructor double-close
		if (!ok) {
			return std::unexpected(Status::ERR);
		}
		return {};
	}
	DelugeStatus status = deluge_stream_close(handle_);
	handle_ = nullptr; // matters even on error: don't let the destructor double-close
	if (status != DELUGE_OK) {
		return std::unexpected(to_status(status));
	}
	return {};
}

} // namespace deluge::io
