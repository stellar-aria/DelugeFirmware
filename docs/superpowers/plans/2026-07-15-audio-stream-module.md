# Audio Stream Module — Implementation Plan (Phase 0–1: foundation + ReadSource seam)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `deluge::audio::stream` module and route the sample cluster read through an explicit `ReadSource` seam (`StreamReadSource` for normal playback, `BlockReadSource` for recorder read-back), replacing the buried `if (readStream_) … else deluge_block_read` branch in `readClusterData`.

**Architecture:** A new in-tree C++23 module under `src/deluge/storage/audio/stream/`. This plan delivers the read seam only: an abstract `ReadSource` with two concrete impls and a selection helper, consumed by `readClusterData`. The residency engine (`deluge_resource`, Rust) and the I/O backend (`stream_io.h` via `deluge::io::Stream`) are unchanged — this only reshapes what sits between them. The de-overloading of `Cluster`, the pure reconstruction core, `SampleStream`, and loader consolidation are later phases (see Roadmap).

**Tech Stack:** C++23 (module targets `CXX_STANDARD 26` in specs, per the repo test convention); `deluge::io::Stream` (RAII over `stream_io.h`); `deluge_block_read` / `deluge_block_sd_unit` (`block_device.h`); CppSpec unit specs (mirroring `tests/spec_io/`); golden-master render gate (`scripts/golden_mixdown.sh`).

## Global Constraints

Copied verbatim from the design spec (`docs/superpowers/specs/2026-07-15-audio-stream-module-design.md`); every task inherits these:

- **Idiomatic modern C++23**, in-tree — not a copy of the legacy style.
- **I/O via `deluge::io::Stream`**, never the raw `deluge_stream_*` C ABI directly; the C ABI is the portability boundary *beneath* the wrapper.
- **Residency via the `deluge_resource` callback / resident-chunk-pointer seam**; **no new C ABI** introduced in the middle. A thin C++ facade over `deluge_resource` is explicitly out of scope.
- **FAT-cluster-sized, DMA-aligned transfers preserved** — the physical model is unchanged.
- **RT reader keeps reading resident bytes by pointer; the render thread never does I/O.** This plan does not touch the RT read path.
- **Keep chunk / manager structs byte-stable.** Struct-size changes shift heap addresses and can flip goldens deterministically with zero behavior change — run the `padsweep` layout-invariance guard on any task that could change a struct's size.
- **Gate:** pure code-moves must stay golden **bit-exact**; behavior-changing steps are ear-check + hardware gated (none in this plan — Phase 0–1 is behavior-neutral).

---

## File Structure

- `src/deluge/storage/audio/stream/read_source.h` — the `ReadSource` interface + `StreamReadSource`, `BlockReadSource` declarations, and the `makeReadSource(Sample&)` selection helper declaration. One responsibility: the read seam's public surface.
- `src/deluge/storage/audio/stream/read_source.cpp` — the two impls + the selection helper.
- `src/deluge/storage/audio/audio_file_manager.cpp` — modify `readClusterData` (the read branch at ~988–1011) to consume a `ReadSource`.
- `src/deluge/CMakeLists.txt` — add the new `.cpp` to the firmware sources.
- `tests/spec_audio_stream/CMakeLists.txt` — CppSpec driver (mirror `tests/spec_io/CMakeLists.txt`).
- `tests/spec_audio_stream/mock_read_source.h` — an in-memory `ReadSource` for specs.
- `tests/spec_audio_stream/read_source_spec.cpp` — the specs.
- `tests/CMakeLists.txt` — `add_subdirectory(spec_audio_stream)`.

---

## Task 0: Module skeleton + build wiring

**Files:**
- Create: `src/deluge/storage/audio/stream/read_source.h`
- Create: `src/deluge/storage/audio/stream/read_source.cpp`
- Modify: `src/deluge/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: the `deluge::audio::stream` namespace and a compiling, linkable translation unit later tasks extend.

- [ ] **Step 1: Create the header with the namespace and a placeholder free function**

`src/deluge/storage/audio/stream/read_source.h`:
```cpp
#pragma once

// The audio-stream module's read seam. See docs/superpowers/specs/2026-07-15-audio-stream-module-design.md §6.
namespace deluge::audio::stream {

// Sanity anchor for Task 0; removed in Task 1 when the real interface lands.
bool module_linked();

} // namespace deluge::audio::stream
```

- [ ] **Step 2: Create the source file**

`src/deluge/storage/audio/stream/read_source.cpp`:
```cpp
#include "storage/audio/stream/read_source.h"

namespace deluge::audio::stream {

bool module_linked() {
	return true;
}

} // namespace deluge::audio::stream
```

- [ ] **Step 3: Add the source to the firmware build**

In `src/deluge/CMakeLists.txt`, add `storage/audio/stream/read_source.cpp` to the target's source list, following the existing pattern for `storage/audio/*.cpp` entries (place it next to `storage/audio/audio_file_manager.cpp`).

- [ ] **Step 4: Build the firmware to verify it compiles and links**

Run: `dbt build Debug`
Expected: build succeeds, no new warnings.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/storage/audio/stream/read_source.h src/deluge/storage/audio/stream/read_source.cpp src/deluge/CMakeLists.txt
git commit -m "feat(audio-stream): scaffold deluge::audio::stream module"
```

---

## Task 1: `ReadSource` interface + `StreamReadSource` + `BlockReadSource`

**Files:**
- Modify: `src/deluge/storage/audio/stream/read_source.h`
- Modify: `src/deluge/storage/audio/stream/read_source.cpp`

**Interfaces:**
- Consumes: `deluge::io::Stream` (`src/deluge/io/stream.hpp`), `deluge_block_read`/`deluge_block_sd_unit` (`include/libdeluge/block_device.h`), `DelugeStatus` (`include/libdeluge/types.h`), `Sample` (`model/sample/sample.h`).
- Produces:
  - `class deluge::audio::stream::ReadSource` with `virtual std::expected<uint32_t, DelugeStatus> read(uint32_t clusterIndex, std::span<std::byte> dst) = 0;`
  - `class StreamReadSource : public ReadSource` — ctor `StreamReadSource(deluge::io::Stream& stream, uint8_t clusterSizeMagnitude)`.
  - `class BlockReadSource : public ReadSource` — ctor `BlockReadSource(const Sample& sample)`.
  - `makeReadSource` is added in Task 3 (not here).

- [ ] **Step 1: Replace the header with the real interface + the two impls' declarations**

`src/deluge/storage/audio/stream/read_source.h`:
```cpp
#pragma once

#include "io/stream.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

extern "C" {
#include "libdeluge/types.h" // DelugeStatus
}

class Sample;

// The audio-stream module's read seam. See design spec §6/§7. A ReadSource pulls one FAT-cluster-sized
// block of a sample's on-card bytes into a caller buffer. Two impls, both first-class: StreamReadSource
// (normal playback, over deluge::io::Stream) and BlockReadSource (recorder read-back of a mid-write file,
// by physical sector address). The reconstruction core (a later phase) reads through this and stays pure.
namespace deluge::audio::stream {

class ReadSource {
public:
	virtual ~ReadSource() = default;

	// Read exactly dst.size() bytes for cluster `clusterIndex` (byte offset = clusterIndex << magnitude,
	// or physical sector, depending on the impl). Returns bytes read on success, or a DelugeStatus error.
	virtual std::expected<uint32_t, DelugeStatus> read(uint32_t clusterIndex, std::span<std::byte> dst) = 0;
};

// Normal playback / load path: reads via deluge::io::Stream::read_at at a cluster-aligned byte offset.
class StreamReadSource final : public ReadSource {
public:
	StreamReadSource(deluge::io::Stream& stream, uint8_t clusterSizeMagnitude)
	    : stream_{stream}, clusterSizeMagnitude_{clusterSizeMagnitude} {}

	std::expected<uint32_t, DelugeStatus> read(uint32_t clusterIndex, std::span<std::byte> dst) override;

private:
	deluge::io::Stream& stream_;
	uint8_t clusterSizeMagnitude_;
};

// Recorder read-back path: the sample has no open read stream (it's still being written), so read the
// physically-written sectors directly by the recorder-maintained per-cluster sdAddress. See §7.
class BlockReadSource final : public ReadSource {
public:
	explicit BlockReadSource(const Sample& sample) : sample_{sample} {}

	std::expected<uint32_t, DelugeStatus> read(uint32_t clusterIndex, std::span<std::byte> dst) override;

private:
	const Sample& sample_;
};

} // namespace deluge::audio::stream
```

- [ ] **Step 2: Replace the source with the two impls**

`src/deluge/storage/audio/stream/read_source.cpp`:
```cpp
#include "storage/audio/stream/read_source.h"
#include "model/sample/sample.h"

extern "C" {
#include "libdeluge/block_device.h"
}

namespace deluge::audio::stream {

std::expected<uint32_t, DelugeStatus> StreamReadSource::read(uint32_t clusterIndex, std::span<std::byte> dst) {
	auto result = stream_.read_at(clusterIndex << clusterSizeMagnitude_, dst);
	if (!result) {
		return std::unexpected(deluge::io::to_deluge_status(result.error()));
	}
	return static_cast<uint32_t>(result->size());
}

std::expected<uint32_t, DelugeStatus> BlockReadSource::read(uint32_t clusterIndex, std::span<std::byte> dst) {
	uint32_t numSectors = static_cast<uint32_t>(dst.size()) >> 9;
	DelugeStatus status = deluge_block_read(deluge_block_sd_unit(), reinterpret_cast<uint8_t*>(dst.data()),
	                                        sample_.clusters[clusterIndex].sdAddress, numSectors);
	if (status != DELUGE_OK) {
		return std::unexpected(status);
	}
	return static_cast<uint32_t>(numSectors) * 512u;
}

} // namespace deluge::audio::stream
```

- [ ] **Step 3: Build the firmware**

Run: `dbt build Debug`
Expected: build succeeds. (Verifies the interface compiles against the real `deluge::io::Stream`, `Sample`, and `block_device.h`.)

- [ ] **Step 4: Commit**

```bash
git add src/deluge/storage/audio/stream/read_source.h src/deluge/storage/audio/stream/read_source.cpp
git commit -m "feat(audio-stream): ReadSource interface + Stream/Block impls"
```

---

## Task 2: CppSpec harness + `MockReadSource` (specs stand up)

**Files:**
- Create: `tests/spec_audio_stream/CMakeLists.txt`
- Create: `tests/spec_audio_stream/mock_read_source.h`
- Create: `tests/spec_audio_stream/read_source_spec.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ReadSource` (Task 1).
- Produces: `deluge::audio::stream::MockReadSource` — ctor takes `std::vector<std::vector<std::byte>> clusters` (bytes per cluster index); `read()` copies the requested cluster into `dst`, returns `dst.size()`, or `DELUGE_ERR_PARAM` if the index is out of range.

- [ ] **Step 1: Write the failing test**

`tests/spec_audio_stream/read_source_spec.cpp` (mirror `tests/spec_io/status_spec.cpp`'s structure exactly — the quoted `cppspec.hpp` include, the `describe <var>("desc", $ {...})` form, and the trailing `CPPSPEC_SPEC(<var>)` registration where `<var>` matches the `<var>_spec.cpp` filename convention):
```cpp
// tests/spec_audio_stream/read_source_spec.cpp
#include "mock_read_source.h"

#include "cppspec.hpp"

#include <array>
#include <cstddef>

using namespace deluge::audio::stream;

// clang-format off
describe read_source("ReadSource (mock)", $ {
	it("returns the requested cluster's bytes", _ {
		MockReadSource src{{{std::byte{1}, std::byte{2}}, {std::byte{3}, std::byte{4}}}};
		std::array<std::byte, 2> buf{};
		auto n = src.read(1, buf);
		expect(n.has_value()).to_equal(true);
		expect(n.value()).to_equal(2u);
		expect(std::to_integer<int>(buf[0])).to_equal(3);
		expect(std::to_integer<int>(buf[1])).to_equal(4);
	});

	it("errors on an out-of-range cluster index", _ {
		MockReadSource src{{{std::byte{1}}}};
		std::array<std::byte, 1> buf{};
		auto n = src.read(5, buf);
		expect(n.has_value()).to_equal(false);
	});
});

CPPSPEC_SPEC(read_source)
```

- [ ] **Step 2: Write the mock**

`tests/spec_audio_stream/mock_read_source.h`:
```cpp
#pragma once

#include "storage/audio/stream/read_source.h"
#include <algorithm>
#include <vector>

namespace deluge::audio::stream {

// In-memory ReadSource for specs: one byte vector per cluster index. No card, no FatFS.
class MockReadSource final : public ReadSource {
public:
	explicit MockReadSource(std::vector<std::vector<std::byte>> clusters) : clusters_{std::move(clusters)} {}

	std::expected<uint32_t, DelugeStatus> read(uint32_t clusterIndex, std::span<std::byte> dst) override {
		if (clusterIndex >= clusters_.size()) {
			return std::unexpected(DELUGE_ERR_PARAM);
		}
		const auto& src = clusters_[clusterIndex];
		uint32_t n = static_cast<uint32_t>(std::min(dst.size(), src.size()));
		std::copy_n(src.begin(), n, dst.begin());
		return n;
	}

private:
	std::vector<std::vector<std::byte>> clusters_;
};

} // namespace deluge::audio::stream
```

- [ ] **Step 3: Wire the CppSpec driver**

`tests/spec_audio_stream/CMakeLists.txt` (mirror `tests/spec_io/CMakeLists.txt` — same `FetchContent`/`create_specs_driver` shape; this spec needs no production `.cpp` because `MockReadSource` is header-only and `read_source_spec.cpp` doesn't link `StreamReadSource`/`BlockReadSource`):
```cmake
# tests/spec_audio_stream/CMakeLists.txt
#
# deluge::audio::stream ReadSource specs. Exercises the seam via an in-memory MockReadSource
# (header-only) — no card, no FatFS, no resource manager.
include(FetchContent)
FetchContent_Declare(CppSpec
  URL https://github.com/toroidal-code/cppspec/archive/refs/heads/main.tar.gz
)
FetchContent_MakeAvailable(CppSpec)

function(create_specs_driver driver_name spec_dir)
  file(GLOB_RECURSE spec_sources RELATIVE ${spec_dir} ${spec_dir}/*_spec.cpp)
  create_test_sourcelist(specs ${driver_name}.cpp ${spec_sources})
  add_executable(${driver_name} ${specs})
  target_link_libraries(${driver_name} PRIVATE c++spec)
  target_include_directories(${driver_name} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ../../include      # libdeluge/types.h
    ../../src/deluge   # storage/audio/stream/read_source.h, io/stream.hpp
  )
  set_target_properties(${driver_name} PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES)
  foreach(spec IN LISTS spec_sources)
    cmake_path(GET spec STEM LAST_ONLY spec_name)
    add_test(NAME ${spec_name} COMMAND ${driver_name} ${spec_name} --verbose)
  endforeach()
endfunction()

create_specs_driver(audio_stream_specs ${CMAKE_CURRENT_LIST_DIR})
```

In `tests/CMakeLists.txt`, add `add_subdirectory(spec_audio_stream)` next to `add_subdirectory(spec_io)` (line ~89).

- [ ] **Step 4: Configure + build the specs, run to verify they fail then pass**

Build the test tree the same way the existing `spec_io` specs are built in this repo (the tests are configured via `tests/CMakeLists.txt`), then:

Run: `ctest -R read_source_spec --output-on-failure`
Expected: PASS (2 assertions green). If the driver won't compile because `read` signature drifted from Task 1, that's the intended fail-first signal — reconcile against Task 1's `Produces` block.

- [ ] **Step 5: Commit**

```bash
git add tests/spec_audio_stream tests/CMakeLists.txt
git commit -m "test(audio-stream): CppSpec harness + MockReadSource"
```

---

## Task 3: `makeReadSource` selection helper + spec

**Files:**
- Modify: `src/deluge/storage/audio/stream/read_source.h`
- Modify: `src/deluge/storage/audio/stream/read_source.cpp`
- Modify: `tests/spec_audio_stream/read_source_spec.cpp`

**Interfaces:**
- Consumes: `ReadSource`, `StreamReadSource`, `BlockReadSource` (Task 1); `Sample::readStream_` (`std::optional<deluge::io::Stream>`), `Cluster::size_magnitude`.
- Produces: `std::unique_ptr<ReadSource> deluge::audio::stream::makeReadSource(Sample& sample)` — returns a `StreamReadSource` if `sample.readStream_.has_value()`, else a `BlockReadSource`. **This is where source selection lives; no caller branches on block-vs-stream.** (In Phase 3 this ownership migrates onto `SampleStream`; the rule is unchanged.)

- [ ] **Step 1: Declare `makeReadSource` in the header**

Add to `read_source.h` inside the namespace, after `BlockReadSource`:
```cpp
#include <memory>
// ...
// Selects the read source from the sample's backing state: an open read stream (normal, loaded-from-card
// sample) -> StreamReadSource; otherwise (a recording still being written) -> BlockReadSource. This is the
// single place the block-vs-stream decision is made — no caller branches on it.
std::unique_ptr<ReadSource> makeReadSource(Sample& sample);
```

- [ ] **Step 2: Implement it**

Add to `read_source.cpp`:
```cpp
#include "storage/cluster/cluster.h" // Cluster::size_magnitude
#include <memory>

// ... inside namespace deluge::audio::stream ...

std::unique_ptr<ReadSource> makeReadSource(Sample& sample) {
	if (sample.readStream_.has_value()) {
		return std::make_unique<StreamReadSource>(sample.readStream_.value(),
		                                          static_cast<uint8_t>(Cluster::size_magnitude));
	}
	return std::make_unique<BlockReadSource>(sample);
}
```

- [ ] **Step 3: Build the firmware**

Run: `dbt build Debug`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/storage/audio/stream/read_source.h src/deluge/storage/audio/stream/read_source.cpp
git commit -m "feat(audio-stream): makeReadSource selects Stream vs Block from sample state"
```

---

## Task 4: Route `readClusterData`'s read through `ReadSource`

**Files:**
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp` (the read branch, ~988–1011)

**Interfaces:**
- Consumes: `makeReadSource(Sample&)` (Task 3).
- Produces: `readClusterData` with the `if (readStream_) … else deluge_block_read` branch replaced by a single `ReadSource` read. Behavior-identical.

- [ ] **Step 1: Replace the read branch**

In `readClusterData` (`audio_file_manager.cpp`), the current block is (lines ~988–1011):
```cpp
	uint32_t bytesRequested = static_cast<uint32_t>(numSectors) * 512u;
	uint32_t bytesRead = 0;
	DelugeStatus status;
	if (sample->readStream_.has_value()) {
		auto readResult = sample->readStream_->read_at(
		    static_cast<uint32_t>(clusterIndex) << Cluster::size_magnitude,
		    std::span<std::byte>(reinterpret_cast<std::byte*>(cluster.data), bytesRequested));
		if (readResult) {
			bytesRead = static_cast<uint32_t>(readResult->size());
			status = DELUGE_OK;
		}
		else {
			status = deluge::io::to_deluge_status(readResult.error());
		}
	}
	else {
		// ... comment ...
		status = deluge_block_read(deluge_block_sd_unit(), reinterpret_cast<uint8_t*>(cluster.data),
		                           sample->clusters[cluster.clusterIndex].sdAddress, static_cast<uint32_t>(numSectors));
	}
```

Replace it with:
```cpp
	uint32_t bytesRequested = static_cast<uint32_t>(numSectors) * 512u;
	uint32_t bytesRead = 0;
	DelugeStatus status;
	{
		// Read seam: SampleStream owns source selection (Stream for a loaded sample, Block for a
		// still-being-written recording). See storage/audio/stream/read_source.h and design §6/§7.
		auto source = deluge::audio::stream::makeReadSource(*sample);
		auto readResult = source->read(static_cast<uint32_t>(clusterIndex),
		                               std::span<std::byte>(reinterpret_cast<std::byte*>(cluster.data), bytesRequested));
		if (readResult) {
			bytesRead = readResult.value();
			status = DELUGE_OK;
		}
		else {
			status = readResult.error();
		}
	}
```

Add `#include "storage/audio/stream/read_source.h"` to the file's includes if not already present. Leave everything else in `readClusterData` (sector math, ALPHA checks, `convertDataIfNecessary`, the boundary stitch, `mark_ready`) exactly as-is.

- [ ] **Step 2: Build the firmware**

Run: `dbt build Debug`
Expected: build succeeds, no warnings.

- [ ] **Step 3: Golden bit-exact gate (Cordae MIXDOWN)**

Run: `scripts/golden_mixdown.sh check`
Expected: `PASS` — bit-exact against the golden. (This is a pure read-mechanism swap; the bytes read and everything downstream are identical.)

- [ ] **Step 4: Layout-invariance guard**

Run: `scripts/golden_mixdown.sh padsweep`
Expected: all pad layouts render bit-identical to each other. (Confirms no struct-size/layout coupling was introduced.)

- [ ] **Step 5: Repeat the golden gate on the recorder-exercising and eviction fixtures**

The Cordae MIXDOWN does not exercise `BlockReadSource` (no mid-record read-back) or heavy eviction. Re-run the check against the `highsiderr` and `icoustic` fixtures via the harness's fixture selector (see `scripts/golden_mixdown.sh` header — set the fixture the same way the resource-manager work did).
Expected: both `PASS` (or, if a fixture legitimately exercises the recorder read-back and diverges, capture A/B WAVs for Kate's ear-check per the design's gate — but a pure read-mechanism swap should stay bit-exact).

- [ ] **Step 6: Commit**

```bash
git add src/deluge/storage/audio/audio_file_manager.cpp
git commit -m "refactor(audio-stream): route readClusterData's read through the ReadSource seam"
```

---

## Roadmap — remaining phases (each planned as it lands)

These are recorded here from the design spec's §8 migration sequence; each becomes its own detailed plan once its predecessor lands, because each phase's surgery depends on the realized shape of the previous one. Each phase is independently golden-gated working software.

- **Phase 2 — extract the pure reconstruction core.** Move `convertDataIfNecessary` (cluster.cpp:60–153) and the boundary stitch (audio_file_manager.cpp:1048–1227) into a pure `reconstruct()` in the module, reading through `ReadSource` (Task 4) and taking neighbor edge spans + a `convertToNative` functor + a yield callback (for the current mid-loop `AudioEngine::runRoutine()` cooperative yield). **This is the delicate part** (goto-laden, format-specific, neighbor-coupled). Its plan must derive real per-format test vectors by reading `Sample::convertToNative` — do not fabricate them. Gate: NATIVE-passthrough + boundary specs, plus golden bit-exact.
- **Phase 3 — split `ComputedChunk` out of `Cluster`.** Introduce `StreamedChunk` / `ComputedChunk`; repoint `SampleCache` + perc onto `ComputedChunk`; shed SAMPLE-only fields from the computed path. Watch struct byte-stability (padsweep). Gate: golden bit-exact.
- **Phase 4 — introduce `SampleStream`.** Migrate the residency table + `getCluster` dispatch + stream handle off `Sample`/`SampleCluster`; `SampleHolder` and the RT reader lease through it; `makeReadSource` ownership moves here. Gate: golden bit-exact.
- **Phase 5 — consolidate the `loader` pump** into the module; retire the `AudioFileManager` streaming methods. Gate: ear-check + hardware (behavior-touching).
- **Follow-on A** — decouple `ComputedChunk` sizing from FAT-derived `Cluster::size`.
- **Follow-on B** — `RecordingBacking`: hide physical addressing (`sdAddress`/`sector_of`) from `SampleRecorder`.
