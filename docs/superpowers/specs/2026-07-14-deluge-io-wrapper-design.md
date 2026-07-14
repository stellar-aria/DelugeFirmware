# `deluge::io` — an idiomatic C++ wrapper over `file_io.h` — Design

**Date:** 2026-07-14
**Status:** Design (approved in brainstorming) → plan next
**Branch:** `feat/libdeluge-io-wrapper`, based off `next`

## 1. Goal

`include/libdeluge/file_io.h` (merged into `next`; design at
`docs/superpowers/specs/2026-07-14-libdeluge-file-io-boundary-design.md`) is a
C-ABI boundary — opaque handles, `DelugeStatus` returns, out-params — because
BSPs across languages (C, C++, Rust) must implement it. It was never meant to
be what *app* code calls directly. `src/fatfs/fatfs.hpp`'s `FatFS::File`/
`Directory` classes used to play that role (RAII, `std::expected`-returning
methods, object-oriented) sitting over raw FatFS; now that FatFS is
BSP-internal, that role is orphaned. Migrating app code straight from
`FatFS::File` to raw `deluge_file_open`/`DelugeStatus`/opaque-pointer calls
would be an ergonomics regression, not a lateral move.

This is a new, idiomatic C++23 layer, `deluge::io`, sitting directly over
`file_io.h` and playing the exact role `FatFS::File`/`Directory` played over
raw FatFS — mirroring its API shape closely (same method names, same
`std::expected`-returning pattern, same free-function `mkdir`/`unlink`/
`rename`) so call sites currently using the old wrapper migrate almost
mechanically. **Prerequisite for the call-site migration series** (the
five-plan roadmap in `docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md`)
— that series' Plan 1 (and every later plan) migrates app code to
`deluge::io`, not to raw `file_io.h` calls.

## 2. Location

`src/deluge/io/file.hpp` + `src/deluge/io/file.cpp`, namespace `deluge::io`.
Portable app code (`src/deluge/`, alongside the existing `io/midi/`,
`io/debug/`), not `src/fatfs/` — this is the app's own layer, not
vendored/BSP-adjacent code. Depends only on `include/libdeluge/file_io.h`;
knows nothing about FatFS.

## 3. API

### 3.1 `Status` — a real `enum class`, not the raw C enum

`DelugeStatus` is a plain C enum (implicit int conversions, no type safety) —
exactly what `FRESULT` was to `FatFS::Error`. Same fix, same precedent:

```cpp
enum class Status {
	OK, ERR, PARAM, BUSY, TIMEOUT, IO, NODEV, UNSUPPORTED,
	NOT_FOUND, EXISTS, NO_SPACE, NO_FILESYSTEM, WRITE_PROTECTED, NO_MEMORY,
};

Status to_status(DelugeStatus status); // 1:1 mapping, exhaustive switch
```

### 3.2 `File` and `Directory` — move-only RAII, `std::expected` methods

**Deliberately move-only from the outset** (deleted copy constructor/
assignment) — this closes off, by construction, both remaining findings from
the `file_io.h` boundary's final review: no way to repeat the move-unsound
RAII bug just fixed in `FatFS::File`/`Directory` (§4 of the boundary design
doc's fix), and no way for a future call site to reproduce the
`sample_recorder.cpp` copy-assignment bug found during that fix's review
(`src/deluge/model/sample/sample_recorder.cpp:481,1524`) — copying simply
won't compile against this wrapper.

```cpp
class File {
public:
	File(File&) = delete;
	File(File&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
	File& operator=(File&) = delete;
	File& operator=(File&& other) noexcept; // self-assignment-guarded (this != &other)
	~File() { if (handle_) deluge_file_close(handle_); }

	[[nodiscard]] static std::expected<File, Status> open(std::string_view path, DelugeFileOpenMode mode);
	std::expected<std::span<std::byte>, Status> read(std::span<std::byte> buffer);
	std::expected<uint32_t, Status> write(std::span<const std::byte> buffer);
	std::expected<void, Status> seek(uint32_t offset);
	std::expected<uint32_t, Status> size();
	std::expected<void, Status> close();

private:
	File() = default;
	explicit File(DelugeFile* handle) : handle_(handle) {}
	DelugeFile* handle_ = nullptr;
};

class Directory {
public:
	// same move-only shape as File

	[[nodiscard]] static std::expected<Directory, Status> open(std::string_view path);
	std::expected<std::optional<DelugeDirEntry>, Status> read(); // nullopt = end of directory
	std::expected<void, Status> close();

private:
	DelugeDir* handle_ = nullptr;
};

std::expected<void, Status> mkdir(std::string_view path);
std::expected<void, Status> unlink(std::string_view path);
std::expected<void, Status> rename(std::string_view old_path, std::string_view new_path);
```

`Directory::read()` returning `std::optional<DelugeDirEntry>` (nullopt at end
of directory) is more idiomatic than `file_io.h`'s C-shaped
`(DelugeDirEntry*, bool*)` out-param pair — the wrapper absorbs that
translation so callers get a normal "loop until nullopt" pattern.

**Path handling note (carried over from existing precedent, not a new
concern):** methods take `std::string_view` and pass `.data()` to the C-ABI's
`const char*` parameters, same as `FatFS::File::open` already does today.
This relies on call sites passing effectively-null-terminated views (string
literals, `.c_str()`-backed strings) — not enforced by the type system, but
an existing, working convention this wrapper deliberately doesn't change.

## 4. Testing — a real upgrade over what was possible for `file_io.h` itself

`file_io.h`'s own adapter (`src/fatfs/file_io.cpp`) couldn't get real I/O
round-trip test coverage (`FF_USE_MKFS=0` blocks synthesizing a mountable
filesystem host-side). `deluge::io` doesn't have that problem: it only calls
the twelve `deluge_file_*`/`deluge_dir_*` C-ABI functions declared in
`file_io.h` — a host test can link `deluge::io` against a **hand-written mock
implementation** of those twelve functions (an in-memory fake, not a real
filesystem) instead of the real FatFS-backed adapter, and get genuine
open→read→write→close round-trip coverage, error-path coverage, and
move-semantics coverage, entirely deterministic, no mounted filesystem
required. This mirrors the existing `mock_diskio.cpp`/`mock_display.cpp`/
`mock_print.cpp` pattern already used in `tests/32bit_unit_tests/mocks/` —
same technique, one layer up.

This plan's test suite: `Status` mapping (table-driven, all `DelugeStatus`
values), `File`/`Directory` move semantics (move leaves the source
handle-less; destructor on a moved-from object doesn't double-close — provable
directly against the mock, unlike the `file_io.h`-level fix which could only
be proven by code review), and full read/write/seek/mkdir/unlink/rename/
directory-iteration round-trips against the mock.

## 5. Effect on the migration roadmap

`docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md`'s Plan 1
(and Plans 2-4) target `deluge::io::File`/`Directory`/`mkdir`/`unlink`/
`rename`, not raw `file_io.h` calls — Plan 1's design doc needs a follow-up
amendment reflecting this once this wrapper lands. This doc's own scope ends
at the wrapper itself; it does not touch any of the ~65 existing call sites.
