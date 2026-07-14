# `deluge::io` Call-Site Migration, Tier 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the thirteen genuinely mechanical FatFS call sites across six files (the "settings-folder bootstrap" idiom in four files, plus two more standalone `f_mkdir` helpers found independent of their files' larger, gated problems) from raw FatFS calls to `deluge::io`.

**Architecture:** Pure call-site translation, no new behavior. Each site swaps `FRESULT`/`f_mkdir`/`f_rename`/`f_unlink` for `deluge::io::mkdir`/`rename`/`unlink` returning `std::expected<void, deluge::io::Status>`, preserving each site's exact existing tolerance/error-handling shape (there are three distinct shapes in this batch — see Global Constraints). One site needs a new small reverse-mapping helper in `deluge::io` (`Status` → `DelugeStatus`) so it can keep calling the existing `delugeStatusToError` translator.

**Tech Stack:** C++26, `std::expected`, the already-merged `deluge::io::mkdir`/`unlink`/`rename` (`src/deluge/io/file.hpp`).

**Spec:** `docs/superpowers/specs/2026-07-14-file-io-migration-1-design.md`. Roadmap context: `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md`.

## Global Constraints

- **No behavior change beyond the mechanical translation.** Every site's existing tolerance shape (EXISTS-tolerant / `FR_OK`-only-success / no-tolerance) is preserved exactly, including the pre-existing quirk in `browser.cpp:1753` where the failure path always returns a hardcoded `FR_NO_PATH`-derived error regardless of the real failure code. Do not "fix" this as part of the migration.
- **`mkdir`/`unlink`/`rename` are not `[[nodiscard]]`** (confirmed against `src/deluge/io/file.hpp:98-100`) — existing best-effort, discard-the-result call sites (`midi_device_manager.cpp:467`, `runtime_feature_settings.cpp:278`) need no `(void)` cast.
- **None of the six files currently `#include` a FatFS header directly** (`FRESULT`/`f_mkdir`/etc. arrive transitively via `storage_manager.h`) — each file needs `#include "io/file.hpp"` added, nothing removed.
- **This plan does not touch:** `sample_browser.cpp`, `instrument_clip_view.cpp`, `browser.cpp`'s main scan loop, `audio_file_manager.cpp`, `storage_manager.cpp`'s `FileReader`/`FileWriter`/`fileSystem.mount(...)`, or `smsysex.cpp` — all gated on separate, undesigned work tracked in `TODO.md` and the roadmap doc.
- **No golden-master coverage for this plan's call sites** (first-boot/upgrade-only code paths and directory-creation helpers, never on the audio-render path) — verification is build success + full existing spec/unit suite + code-level inspection, not golden bit-exactness.
- **This plan is `deluge::io`'s first real integration test against the shipping FatFS-backed adapter** (its own test suite only exercises a mock) — treat any on-device behavior surprise as potentially a `deluge::io`-level bug, not just a translation mistake.

---

## Task 1: `performance_view.cpp`

**Files:**
- Modify: `src/deluge/gui/views/performance_view.cpp:1866-1868` (and its `#include` block)

**Interfaces:**
- Consumes: `deluge::io::mkdir(std::string_view) -> std::expected<void, deluge::io::Status>`, `deluge::io::rename(std::string_view, std::string_view) -> std::expected<void, deluge::io::Status>` (both already merged, `src/deluge/io/file.hpp:98,100`).

No TDD cycle for this task — it's a pure, behavior-preserving call-site translation with no golden-harness coverage (see Global Constraints); verification is build success plus a manual read-through, not a synthetic failing test.

- [ ] **Step 1: Add the include**

Add near the top of `src/deluge/gui/views/performance_view.cpp`, alongside its other project includes:

```cpp
#include "io/file.hpp"
```

- [ ] **Step 2: Translate the call site**

Current code (`performance_view.cpp:1859-1879`):

```cpp
	FilePointer fp;
	// PerformanceView.XML
	bool success = StorageManager::fileExists(PERFORM_DEFAULTS_XML, &fp);
	if (!success) {
		// since we changed the file path for the PerformanceView.XML in c1.3, it's possible
		// that a PerformanceView file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		FRESULT result = f_mkdir(SETTINGS_FOLDER);
		if (result == FR_OK || result == FR_EXIST) {
			result = f_rename("PerformanceView.XML", PERFORM_DEFAULTS_XML);
			if (result == FR_OK) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(PERFORM_DEFAULTS_XML, &fp);
			}
		}
		if (!success) {
			loadDefaultLayout();
			return;
		}
	}
```

Replace with:

```cpp
	FilePointer fp;
	// PerformanceView.XML
	bool success = StorageManager::fileExists(PERFORM_DEFAULTS_XML, &fp);
	if (!success) {
		// since we changed the file path for the PerformanceView.XML in c1.3, it's possible
		// that a PerformanceView file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		auto result = deluge::io::mkdir(SETTINGS_FOLDER);
		if (result.has_value() || result.error() == deluge::io::Status::EXISTS) {
			auto renamed = deluge::io::rename("PerformanceView.XML", PERFORM_DEFAULTS_XML);
			if (renamed.has_value()) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(PERFORM_DEFAULTS_XML, &fp);
			}
		}
		if (!success) {
			loadDefaultLayout();
			return;
		}
	}
```

- [ ] **Step 3: Build and verify**

```bash
./dbt build Debug
```

Expected: clean build. Confirm no other reference to `FRESULT`/`f_mkdir`/`f_rename` remains in this file:

```bash
grep -n "FRESULT\|f_mkdir\|f_rename\|f_unlink" src/deluge/gui/views/performance_view.cpp
```

Expected: no output.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/gui/views/performance_view.cpp
git commit -m "migrate performance_view.cpp's settings-folder bootstrap to deluge::io"
```

---

## Task 2: `midi_follow.cpp`

**Files:**
- Modify: `src/deluge/io/midi/midi_follow.cpp:1669` (and its `#include` block)

**Interfaces:**
- Consumes: `deluge::io::mkdir` (same as Task 1).

- [ ] **Step 1: Add the include**

```cpp
#include "io/file.hpp"
```

- [ ] **Step 2: Translate the call site**

Current code (`midi_follow.cpp:1664-1676`):

```cpp
	FilePointer fp;
	// MIDIFollow.XML
	bool success = StorageManager::fileExists(MIDI_FOLLOW_XML, &fp);
	if (!success) {
		// if file doesn't exist, lets make SETTINGS folder if it doesn't already exist
		FRESULT result = f_mkdir(SETTINGS_FOLDER);
		if (result == FR_OK || result == FR_EXIST) {
			// folder eixsts now, write defaults
			writeDefaultsToFile();
			successfullyReadDefaultsFromFile = true;
			return;
		}
	}
```

Replace with:

```cpp
	FilePointer fp;
	// MIDIFollow.XML
	bool success = StorageManager::fileExists(MIDI_FOLLOW_XML, &fp);
	if (!success) {
		// if file doesn't exist, lets make SETTINGS folder if it doesn't already exist
		auto result = deluge::io::mkdir(SETTINGS_FOLDER);
		if (result.has_value() || result.error() == deluge::io::Status::EXISTS) {
			// folder eixsts now, write defaults
			writeDefaultsToFile();
			successfullyReadDefaultsFromFile = true;
			return;
		}
	}
```

(Preserve the pre-existing "eixsts" typo in the comment verbatim — not this migration's concern to fix.)

- [ ] **Step 3: Build and verify**

```bash
./dbt build Debug
grep -n "FRESULT\|f_mkdir\|f_rename\|f_unlink" src/deluge/io/midi/midi_follow.cpp
```

Expected: clean build, no output from the grep.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/io/midi/midi_follow.cpp
git commit -m "migrate midi_follow.cpp's settings-folder bootstrap to deluge::io"
```

---

## Task 3: `midi_device_manager.cpp`

**Files:**
- Modify: `src/deluge/io/midi/midi_device_manager.cpp:467`, `:518-520` (and its `#include` block)

**Interfaces:**
- Consumes: `deluge::io::mkdir`, `deluge::io::rename`, `deluge::io::unlink` (all already merged).

- [ ] **Step 1: Add the include**

This file already includes `"libdeluge/midi_io.h"` (line 32) — add `"io/file.hpp"` alongside it:

```cpp
#include "io/file.hpp"
#include "libdeluge/midi_io.h" // deluge_midi_init/_usb_port/_write/_write_space, deluge_midi_poll_usb_host_event
```

- [ ] **Step 2: Translate the standalone unlink site**

Current code (`midi_device_manager.cpp:465-469`):

```cpp
	if (!anyWorthWritting) {
		// If still here, nothing worth writing. Delete the file if there was one.
		f_unlink(MIDI_DEVICES_XML); // May give error, but no real consequence from that.
		return;
	}
```

Replace with:

```cpp
	if (!anyWorthWritting) {
		// If still here, nothing worth writing. Delete the file if there was one.
		deluge::io::unlink(MIDI_DEVICES_XML); // May give error, but no real consequence from that.
		return;
	}
```

- [ ] **Step 3: Translate the mkdir+rename site**

Current code (`midi_device_manager.cpp:512-529`):

```cpp
	FilePointer fp;
	bool success = StorageManager::fileExists(MIDI_DEVICES_XML, &fp);
	if (!success) {
		// since we changed the file path for the MIDIDevices.XML in c1.3, it's possible
		// that a MIDIDevice file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		FRESULT result = f_mkdir(SETTINGS_FOLDER);
		if (result == FR_OK || result == FR_EXIST) {
			result = f_rename("MIDIDevices.XML", MIDI_DEVICES_XML);
			if (result == FR_OK) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(MIDI_DEVICES_XML, &fp);
			}
		}
		if (!success) {
			return;
		}
```

Replace with:

```cpp
	FilePointer fp;
	bool success = StorageManager::fileExists(MIDI_DEVICES_XML, &fp);
	if (!success) {
		// since we changed the file path for the MIDIDevices.XML in c1.3, it's possible
		// that a MIDIDevice file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		auto result = deluge::io::mkdir(SETTINGS_FOLDER);
		if (result.has_value() || result.error() == deluge::io::Status::EXISTS) {
			auto renamed = deluge::io::rename("MIDIDevices.XML", MIDI_DEVICES_XML);
			if (renamed.has_value()) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(MIDI_DEVICES_XML, &fp);
			}
		}
		if (!success) {
			return;
		}
```

- [ ] **Step 4: Build and verify**

```bash
./dbt build Debug
grep -n "FRESULT\|f_mkdir\|f_rename\|f_unlink" src/deluge/io/midi/midi_device_manager.cpp
```

Expected: clean build, no output from the grep.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/io/midi/midi_device_manager.cpp
git commit -m "migrate midi_device_manager.cpp's FatFS call sites to deluge::io"
```

---

## Task 4: `runtime_feature_settings.cpp`

**Files:**
- Modify: `src/deluge/model/settings/runtime_feature_settings.cpp:207-209`, `:278` (and its `#include` block)

**Interfaces:**
- Consumes: `deluge::io::mkdir`, `deluge::io::rename`, `deluge::io::unlink` (all already merged).

- [ ] **Step 1: Add the include**

```cpp
#include "io/file.hpp"
```

- [ ] **Step 2: Translate the mkdir+rename site**

Current code (`runtime_feature_settings.cpp:199-219`):

```cpp
void RuntimeFeatureSettings::readSettingsFromFile() {
	FilePointer fp;
	// CommunityFeatures.XML
	bool success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
	if (!success) {
		// since we changed the file path for the CommunityFeatures.XML in c1.3, it's possible
		// that a CommunityFeatures file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		FRESULT result = f_mkdir(SETTINGS_FOLDER);
		if (result == FR_OK || result == FR_EXIST) {
			result = f_rename("CommunityFeatures.XML", RUNTIME_FEATURE_SETTINGS_FILE);
			if (result == FR_OK) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
			}
		}
		if (!success) {
			return;
		}
	}
```

Replace with:

```cpp
void RuntimeFeatureSettings::readSettingsFromFile() {
	FilePointer fp;
	// CommunityFeatures.XML
	bool success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
	if (!success) {
		// since we changed the file path for the CommunityFeatures.XML in c1.3, it's possible
		// that a CommunityFeatures file may exists in the root of the SD card
		// if so, let's move it to the new SETTINGS folder (but first make sure folder exists)
		auto result = deluge::io::mkdir(SETTINGS_FOLDER);
		if (result.has_value() || result.error() == deluge::io::Status::EXISTS) {
			auto renamed = deluge::io::rename("CommunityFeatures.XML", RUNTIME_FEATURE_SETTINGS_FILE);
			if (renamed.has_value()) {
				// this means we moved it
				// now let's open it
				success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE, &fp);
			}
		}
		if (!success) {
			return;
		}
	}
```

- [ ] **Step 3: Translate the standalone unlink site**

Current code (`runtime_feature_settings.cpp:277-280`):

```cpp
void RuntimeFeatureSettings::writeSettingsToFile() {
	f_unlink(RUNTIME_FEATURE_SETTINGS_FILE); // May give error, but no real consequence from that.

	Error error = StorageManager::createXMLFile(RUNTIME_FEATURE_SETTINGS_FILE, smSerializer, true);
```

Replace with:

```cpp
void RuntimeFeatureSettings::writeSettingsToFile() {
	deluge::io::unlink(RUNTIME_FEATURE_SETTINGS_FILE); // May give error, but no real consequence from that.

	Error error = StorageManager::createXMLFile(RUNTIME_FEATURE_SETTINGS_FILE, smSerializer, true);
```

- [ ] **Step 4: Build and verify**

```bash
./dbt build Debug
grep -n "FRESULT\|f_mkdir\|f_rename\|f_unlink" src/deluge/model/settings/runtime_feature_settings.cpp
```

Expected: clean build, no output from the grep.

- [ ] **Step 5: Commit**

```bash
git add src/deluge/model/settings/runtime_feature_settings.cpp
git commit -m "migrate runtime_feature_settings.cpp's FatFS call sites to deluge::io"
```

---

## Task 5: `browser.cpp`'s three standalone `f_mkdir` sites + the `Status`→`DelugeStatus` reverse mapping

**Files:**
- Modify: `src/deluge/io/file.hpp`, `src/deluge/io/file.cpp` (new reverse-mapping helper)
- Modify: `src/deluge/gui/ui/browser/browser.cpp:530-538`, `:1729-1732`, `:1753-1756` (and its `#include` block)
- Test: `tests/spec_io/status_spec.cpp` (extend with a case for the new helper)

**Interfaces:**
- Produces: `namespace deluge::io { DelugeStatus to_deluge_status(Status status); }` — the reverse of the already-merged `to_status(DelugeStatus) -> Status`.
- Consumes: `deluge::io::mkdir`; `delugeStatusToError(DelugeStatus) -> Error` (already merged, `src/deluge/util/functions.h`).

This task has a genuine small TDD-able piece (the new reverse-mapping helper) alongside pure call-site translation for the other two sites.

- [ ] **Step 1: Write the failing test for the reverse mapping**

Add to `tests/spec_io/status_spec.cpp` (a new `it` block inside the existing `describe status(...)` block):

```cpp
	it("maps Status::EXISTS back to DELUGE_ERR_EXISTS (the reverse of to_status)", _ {
		expect(deluge::io::to_deluge_status(deluge::io::Status::EXISTS)).to_equal(DELUGE_ERR_EXISTS);
	});
	it("maps Status::NOT_FOUND back to DELUGE_ERR_NOT_FOUND", _ {
		expect(deluge::io::to_deluge_status(deluge::io::Status::NOT_FOUND)).to_equal(DELUGE_ERR_NOT_FOUND);
	});
	it("maps Status::OK back to DELUGE_OK", _ {
		expect(deluge::io::to_deluge_status(deluge::io::Status::OK)).to_equal(DELUGE_OK);
	});
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build build-tests --target io_specs
```

Expected: FAIL — `to_deluge_status` doesn't exist yet.

- [ ] **Step 3: Implement the reverse mapping**

Add to `src/deluge/io/file.hpp` (immediately after `to_status`'s declaration):

```cpp
Status to_status(DelugeStatus status);
DelugeStatus to_deluge_status(Status status);
```

Add to `src/deluge/io/file.cpp` (immediately after `to_status`'s implementation):

```cpp
DelugeStatus to_deluge_status(Status status) {
	switch (status) {
	case Status::OK:
		return DELUGE_OK;
	case Status::ERR:
		return DELUGE_ERR;
	case Status::PARAM:
		return DELUGE_ERR_PARAM;
	case Status::BUSY:
		return DELUGE_ERR_BUSY;
	case Status::TIMEOUT:
		return DELUGE_ERR_TIMEOUT;
	case Status::IO:
		return DELUGE_ERR_IO;
	case Status::NODEV:
		return DELUGE_ERR_NODEV;
	case Status::UNSUPPORTED:
		return DELUGE_ERR_UNSUPPORTED;
	case Status::NOT_FOUND:
		return DELUGE_ERR_NOT_FOUND;
	case Status::EXISTS:
		return DELUGE_ERR_EXISTS;
	case Status::NO_SPACE:
		return DELUGE_ERR_NO_SPACE;
	case Status::NO_FILESYSTEM:
		return DELUGE_ERR_NO_FILESYSTEM;
	case Status::WRITE_PROTECTED:
		return DELUGE_ERR_WRITE_PROTECTED;
	case Status::NO_MEMORY:
		return DELUGE_ERR_NO_MEMORY;
	}
	return DELUGE_ERR; // unreachable while the switch above stays exhaustive
}
```

- [ ] **Step 4: Run to verify it passes**

```bash
cmake --build build-tests --target io_specs && ctest --test-dir build-tests -R status --output-on-failure
```

Expected: PASS (8/8 — the original 5 `to_status` cases plus 3 new `to_deluge_status` cases).

- [ ] **Step 5: Add the include to `browser.cpp`**

```cpp
#include "io/file.hpp"
```

- [ ] **Step 6: Translate `browser.cpp:530-538`**

Current code:

```cpp
					FRESULT result = f_mkdir(defaultDirToAlsoTry);
					if (result == FR_OK) {
						triedCreatingFolder = true;
						goto tryReadingItems;
					}
					else {
						return fresultToDelugeErrorCode(result);
					}
```

Replace with:

```cpp
					auto result = deluge::io::mkdir(defaultDirToAlsoTry);
					if (result.has_value()) {
						triedCreatingFolder = true;
						goto tryReadingItems;
					}
					else {
						return delugeStatusToError(deluge::io::to_deluge_status(result.error()));
					}
```

- [ ] **Step 7: Translate `browser.cpp:1729-1732`**

Current code:

```cpp
	FRESULT result = f_mkdir(newDirPath.c_str());
	if (result) {
		return Error::SD_CARD;
	}
```

Replace with:

```cpp
	auto result = deluge::io::mkdir(newDirPath.c_str());
	if (!result.has_value()) {
		return Error::SD_CARD;
	}
```

- [ ] **Step 8: Translate `browser.cpp:1753-1756`**

Current code (inside `Browser::createFoldersRecursiveIfNotExists`'s per-character loop):

```cpp
			FRESULT result = f_mkdir(tempPath);
			if (result != FR_OK && result != FR_EXIST) {
				return fresultToDelugeErrorCode(FR_NO_PATH);
			}
```

Replace with:

```cpp
			auto result = deluge::io::mkdir(tempPath);
			if (!result.has_value() && result.error() != deluge::io::Status::EXISTS) {
				return delugeStatusToError(DELUGE_ERR_NOT_FOUND); // hardcoded, preserving today's exact (pre-existing) quirk
			}
```

- [ ] **Step 9: Build and verify**

```bash
./dbt build Debug
grep -n "f_mkdir(defaultDirToAlsoTry)\|f_mkdir(newDirPath\|f_mkdir(tempPath)" src/deluge/gui/ui/browser/browser.cpp
```

Expected: clean build, no output from the grep (confirms all three sites moved — `browser.cpp` still has other, unmigrated FatFS usage in its main scan loop, so a blanket `grep FRESULT` here would show unrelated hits; check only for the three specific patterns just migrated).

- [ ] **Step 10: Commit**

```bash
git add src/deluge/io/file.hpp src/deluge/io/file.cpp tests/spec_io/status_spec.cpp src/deluge/gui/ui/browser/browser.cpp
git commit -m "deluge::io: add Status->DelugeStatus reverse mapping; migrate browser.cpp's 3 standalone mkdir sites"
```

---

## Task 6: `storage_manager.cpp`'s `buildPathToFile`

**Files:**
- Modify: `src/deluge/storage/storage_manager.cpp:764-795` (and its `#include` block, if not already present)

**Interfaces:**
- Consumes: `deluge::io::mkdir`.

- [ ] **Step 1: Confirm/add the include**

`storage_manager.cpp` likely already includes FatFS headers transitively via `storage_manager.h`; check whether `"io/file.hpp"` is already present (it may have been pulled in by other, unrelated already-landed work) before adding it — add it only if missing:

```bash
grep -n '#include "io/file.hpp"' src/deluge/storage/storage_manager.cpp
```

If no output, add it near the top alongside the file's other project includes.

- [ ] **Step 2: Translate `buildPathToFile`**

Current code (`storage_manager.cpp:764-795`):

```cpp
bool StorageManager::buildPathToFile(const char* fileName) {

	FRESULT res;
	etl::string<255> s_container;
	s_container.append(fileName);
	char* s = s_container.data();
	int i = strlen(s);

	while (i > 0 && s[i - 1] != '/') // Find decomposition point
		i--;

	if (i > 0) // Move to '/'
		i--;

	if (i > 0) {
		s[i] = 0; // replace '/' with NUL

		res = f_mkdir(s);

		if (res == FR_NO_PATH) {
			// try the next folder in the path
			if (buildPathToFile(s)) {
				// if that worked, try again
				res = f_mkdir(s);
			}
		}

		if (res == FR_OK || res == FR_EXIST)
			return true;
	}
	return false;
}
```

Replace with:

```cpp
bool StorageManager::buildPathToFile(const char* fileName) {

	etl::string<255> s_container;
	s_container.append(fileName);
	char* s = s_container.data();
	int i = strlen(s);

	while (i > 0 && s[i - 1] != '/') // Find decomposition point
		i--;

	if (i > 0) // Move to '/'
		i--;

	if (i > 0) {
		s[i] = 0; // replace '/' with NUL

		auto res = deluge::io::mkdir(s);

		if (!res.has_value() && res.error() == deluge::io::Status::NOT_FOUND) {
			// try the next folder in the path
			if (buildPathToFile(s)) {
				// if that worked, try again
				res = deluge::io::mkdir(s);
			}
		}

		if (res.has_value() || res.error() == deluge::io::Status::EXISTS)
			return true;
	}
	return false;
}
```

Note: the retry branch's condition changed from `res == FR_NO_PATH` to `res.error() == deluge::io::Status::NOT_FOUND` — `DELUGE_ERR_NOT_FOUND` (and its `deluge::io::Status::NOT_FOUND` counterpart) is the boundary's general "path doesn't exist" code, covering both FatFS's `FR_NO_FILE` and `FR_NO_PATH` (see the file_io.h boundary design doc §5) — this is the same collapsing the whole migration series already accepts everywhere else a `NOT_FOUND`-shaped FatFS code appears, not a new decision specific to this site.

- [ ] **Step 3: Build and verify**

```bash
./dbt build Debug
grep -n "FRESULT\|f_mkdir\|f_rename\|f_unlink" src/deluge/storage/storage_manager.cpp
```

Expected: clean build. The grep will still show unrelated hits (`storage_manager.cpp` has plenty of FatFS usage outside `buildPathToFile` — `FileReader`/`FileWriter`'s raw `FIL` members, `fileSystem.mount`, etc. — all out of scope for this plan, per Global Constraints). Confirm specifically that `buildPathToFile`'s body (lines ~764-793) no longer contains `FRESULT`/`f_mkdir`.

- [ ] **Step 4: Commit**

```bash
git add src/deluge/storage/storage_manager.cpp
git commit -m "migrate storage_manager.cpp's buildPathToFile to deluge::io"
```

---

## Task 7: Full regression check

**Files:** none (verification only).

- [ ] **Step 1: Full rza1 build**

```bash
./dbt build Debug
```

Expected: builds clean.

- [ ] **Step 2: Full host/sim build + golden-master check**

```bash
cmake --build build-sim-cpp --target deluge_host deluge_render deluge_loadcheck
NO_BUILD=1 scripts/golden_mixdown.sh check
```

Expected: bit-exact against the stored golden — none of this plan's call sites are on the render path, so nothing should move.

- [ ] **Step 3: Full spec suite**

```bash
cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
```

Expected: all suites pass, including `status_spec` (now 8/8 with the new reverse-mapping cases) and every other `tests/spec_io/`/`tests/spec/` suite unaffected.

- [ ] **Step 4: Confirm no call site was missed**

```bash
grep -n "f_mkdir(SETTINGS_FOLDER)\|f_rename(\"PerformanceView\|f_rename(\"MIDIDevices\|f_rename(\"CommunityFeatures\|f_mkdir(defaultDirToAlsoTry)\|f_mkdir(newDirPath\|f_mkdir(tempPath)" \
  src/deluge/gui/views/performance_view.cpp \
  src/deluge/io/midi/midi_follow.cpp \
  src/deluge/io/midi/midi_device_manager.cpp \
  src/deluge/model/settings/runtime_feature_settings.cpp \
  src/deluge/gui/ui/browser/browser.cpp \
  src/deluge/storage/storage_manager.cpp
```

Expected: no output (every one of the 13 targeted call sites has moved; this plan deliberately leaves other, unrelated FatFS usage in `browser.cpp`/`storage_manager.cpp`/`midi_device_manager.cpp` untouched — the grep patterns above are scoped exactly to the sites this plan targets, not a blanket FatFS check on these files).

- [ ] **Step 5: Commit** (only if any step above required a fix; otherwise this task is verification-only and produces no diff)

---

## Self-Review

**Spec coverage:** design doc §2 (all 6 files / 13 sites, including the two non-EXISTS-tolerant `browser.cpp` sub-idioms and the hardcoded-error quirk) → Tasks 1-6. §3 testing approach → Task 7. §4 out-of-scope items are correctly absent from every task (no task touches `sample_browser.cpp`, `instrument_clip_view.cpp`, `browser.cpp`'s scan loop, `audio_file_manager.cpp`, `storage_manager.cpp`'s `FileReader`/`FileWriter`/mount calls, or `smsysex.cpp`).

**Placeholder scan:** every step has complete code or an exact command with expected output. Task 6's `NOT_FOUND`-collapses-`FR_NO_PATH`-and-`FR_NO_FILE` note is a documented, deliberate boundary-design fact (already established in the `file_io.h` boundary doc), not a guess.

**Type consistency:** `deluge::io::mkdir`/`rename`/`unlink` (already merged) used identically across Tasks 1-6. `to_deluge_status` (introduced Task 5) is used only by Task 5's `browser.cpp:530` site and is the only new symbol this plan produces. `delugeStatusToError` (already merged, from the `file_io.h` boundary plan) is consumed by Task 5's two `Error`-returning sites (`:530`, `:1753`) — Task 6's `buildPathToFile` returns `bool`, not `Error`, so it never calls it, matching the original code's own shape.
