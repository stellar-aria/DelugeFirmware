#pragma once

#include "io/file.hpp" // reuses deluge::io::Status / to_status / to_deluge_status
#include "libdeluge/stream_io.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace deluge::io {

class Stream {
public:
	Stream(Stream&) = delete;
	Stream(Stream&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	Stream& operator=(Stream&) = delete;
	Stream& operator=(Stream&& other) noexcept {
		if (this != &other) {
			if (handle_) {
				// Route through close() (not deluge_stream_close directly) so the efatfs/C-FatFS
				// backend selector lives in exactly one place -- see stream.cpp's close(). Calling
				// deluge_stream_close directly on an efatfs-boxed handle would be a backend mismatch.
				(void)close();
			}
			handle_ = other.handle_;
			other.handle_ = nullptr;
		}
		return *this;
	}
	~Stream() {
		if (handle_) {
			(void)close();
		}
	}

	[[nodiscard]] static std::expected<Stream, Status> open(std::string_view path, DelugeStreamMode mode);
	std::expected<uint32_t, Status> write_at(uint32_t byte_offset, std::span<const std::byte> buffer);
	/// @brief Read up to @p dst.size() bytes at absolute @p byte_offset back through this stream's
	///        OWN open write context, bounded by its live (not yet flushed to disk) size.
	///
	/// R3: used by `SampleRecorder::alterFile()` (a positional read/write pass over its own open
	/// write context) and `finalizeRecordedFile()`'s header patch-back. (SR3b deleted this doc's
	/// other former caller, `RecordingReadSource` -- a still-recording sample's evicted-cluster
	/// read-back -- as a dead end on the real device; see storage/audio/stream/read_source.h.) Both
	/// backends implement it:
	/// `deluge_efatfs_stream_read_at_via` on efatfs, `deluge_stream_read_at` on C-FatFS. EOF-honest --
	/// the returned count may be short of @p dst.size().
	std::expected<uint32_t, Status> read_at_via(uint32_t byte_offset, std::span<std::byte> dst);
	std::expected<void, Status> truncate(uint32_t new_size);
	std::expected<uint32_t, Status> size();
	std::expected<uint32_t, Status> sector_of(uint32_t cluster_index);
	std::expected<void, Status> close();

private:
	explicit Stream(DelugeStream* handle) : handle_(handle) {}
	DelugeStream* handle_ = nullptr;
};

} // namespace deluge::io
