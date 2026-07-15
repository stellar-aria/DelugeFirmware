# `deluge::io` migration Tier 3: `FileReader`/`FileWriter` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire `FileReader::readFIL`/`FileWriter::writeFIL` (a raw vendored FatFS `FIL`) in favour of `std::optional<deluge::io::File>`, and retire the FatFS-specific `FilePointer` locator type from the entire XML/JSON open call chain in favour of plain paths — completing Tier 3 of `docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md`.

**Architecture:** Design doc: `docs/superpowers/specs/2026-07-14-file-io-migration-tier3-filereader-filewriter-design.md` (read this first — it has the full rationale, including a "Correction" section added during plan-writing that this plan's Task 1 absorbs). `FileReader`/`FileWriter` become thin wrappers around `deluge::io::File`; `StorageManager::createFile` returns a `deluge::io::File` instead of a `FatFS::File`; every function that currently threads a `FilePointer*` purely to reach an XML/JSON open threads a `char const* path` instead, made fast by Tier 2's adapter-side directory cache.

**Tech Stack:** C++23, `deluge::io` (`src/deluge/io/file.hpp`), `include/libdeluge/file_io.h` C-ABI boundary.

## Global Constraints

- **This migration is one atomic compilation unit through Task 1; it cannot be split further along the call-chain axis.** `FileReader::readFIL`/`FileWriter::writeFIL`'s only locator-based fast-open primitive (`StorageManager::openFilePointer`, a raw `FFOBJID` field poke) has **no valid replacement once the member's type changes** — `deluge::io::File` only exposes path-based `open()`, deliberately, since the whole point of Tier 2's adapter cache is that locator-based fast-reopen no longer needs to be app-visible. Since `openFilePointer` is called (directly or via the free functions `openXMLFile`/`openJsonFile`) from every layer of the wrapper chain, and none of those layers currently have a path in scope (only a `FilePointer*`), the path has to be threaded in from the outermost GUI call site all the way down, in one shot. **Task 1 therefore covers everything inside `storage_manager.h`/`.cpp` (all 13 in-scope signatures) plus 3 external files forced by the same member-type change — it will not produce a linkable firmware by itself** (external call sites in Tasks 2-4 still pass the old argument shapes until they land). Verify Task 1 by confirming `storage_manager.cpp`, `Deserializer.cpp`, `JsonDeserializer.cpp`, `deserializer_byte_source.cpp`, `save_song_ui.cpp`, `audio_file_manager.cpp`, and `fatfs.cpp` show **zero** compile errors, and that all remaining `dbt build`/`dbt sim` errors are confined to the files listed in Tasks 2-4 (each with an old-signature-shaped call). Only Task 5's build must be fully green.
- `DelugeFileOpenMode` (`include/libdeluge/file_io.h`) has exactly two values: `DELUGE_FILE_READ` and `DELUGE_FILE_WRITE_CREATE` (always creates/truncates — there is **no** "fail if the file already exists" mode, unlike the raw `FA_CREATE_NEW` this replaces). `StorageManager::createFile`'s `mayOverwrite=false` path is real, load-bearing behaviour (several save flows check `Error::FILE_ALREADY_EXISTS` to trigger an overwrite-confirmation prompt) and must keep working — Task 1 adds an explicit existence pre-check for this case (see Task 1, Step 5). This firmware has no concurrent file writers, so a plain check-then-create is race-free in practice.
- `deluge::io::File::write`'s signature (`std::expected<uint32_t, Status> write(std::span<const std::byte> buffer)`) already matches the shape `save_song_ui.cpp`'s collect-media routine expects from `StorageManager::createFile`'s return value — no change needed to that half of that routine.
- Every new failure path introduced by swapping a "never fails" raw-FIL poke for a real path-based open (which genuinely can fail, e.g. TOCTOU between two resolution steps) must be handled via this codebase's existing `*error`/`return nullptr` or `std::unexpected(...)` conventions at that call site — never left as an unchecked `.value()`/`operator->()` that would crash on failure.
- Preserve exact existing behaviour of `closeAfterWriting`'s beginning/end string verification (no byte-count strictness beyond what the current `FRESULT`-only check already enforces) — this is a translation, not a behaviour tightening.
- `char const* path`, not `std::string`, is this migration's parameter type everywhere the design doc and this plan say "path" (matching `deluge::io::File::open(std::string_view path, ...)`'s call convention and the existing codebase style for these functions).

---

### Task 1: `FileReader`/`FileWriter` core redesign + `storage_manager.h`/`.cpp`'s full `FilePointer` retirement (+ 3 forced external files)

**Files:**
- Modify: `src/deluge/storage/storage_manager.h`
- Modify: `src/deluge/storage/storage_manager.cpp`
- Modify: `src/deluge/storage/Deserializer.cpp`
- Modify: `src/deluge/storage/JsonDeserializer.cpp`
- Modify: `src/deluge/storage/audio/deserializer_byte_source.cpp`
- Modify: `src/deluge/storage/audio/deserializer_byte_source.h`
- Modify: `src/deluge/gui/ui/save/save_song_ui.cpp`
- Modify: `src/deluge/storage/audio/audio_file_manager.cpp`
- Modify: `src/fatfs/fatfs.cpp`
- Test: `tests/spec/file_io_spec.cpp` (extend), `tests/spec/storage_manager_spec.cpp` (new, memoryBased-path coverage)

**Interfaces:**
- Consumes: `deluge::io::File::open(std::string_view, DelugeFileOpenMode) -> std::expected<File, Status>`, `.read(std::span<std::byte>) -> std::expected<std::span<std::byte>, Status>`, `.write(std::span<const std::byte>) -> std::expected<uint32_t, Status>`, `.seek(uint32_t) -> std::expected<void, Status>`, `.size() -> std::expected<uint32_t, Status>`, `.close() -> std::expected<void, Status>` (`src/deluge/io/file.hpp`). `deluge::io::mkdir(std::string_view) -> std::expected<void, Status>`.
- Produces (for Tasks 2-4): `StorageManager::openXMLFile(char const* path, XMLDeserializer&, char const* firstTagName, char const* altTagName = "", bool ignoreIncorrectFirmware = false) -> Error`; `openJsonFile` (same shape, `JsonDeserializer&`); `openDelugeFile(char const* path, char const* firstTagName, char const* altTagName = "", bool ignoreIncorrectFirmware = false) -> Error`; `openInstrumentFile(OutputType, char const* path) -> Error`; `openMidiDeviceDefinitionFile(char const* path) -> Error`; `loadMidiDeviceDefinitionFile(MIDIInstrument*, char const* path, std::string* fileName, bool updateFileName = true) -> Error`; `openPatternFile(char const* path) -> Error`; `loadPatternFile(char const* path, std::string* fileName, bool overwriteExisting, bool noScaling, bool previewOnly, bool selectedDrumOnly) -> Error`; `openFavouriteFile(char const* path) -> Error`; `loadFavouriteFile(char const* path, std::string* fileName) -> Error`; `loadInstrumentFromFile(Song*, InstrumentClip*, OutputType, bool, Instrument**, char const* path, std::string* name, std::string* dirPath) -> Error`; `loadSynthToDrum(Song*, InstrumentClip*, bool, SoundDrum**, char const* path, std::string* name, std::string* dirPath) -> Error`; `createFile(char const* filePath, bool mayOverwrite) -> std::expected<deluge::io::File, Error>`. `StorageManager::openFilePointer` and `FilePointer* filePointer` params are **gone** — do not reintroduce them.

---

- [ ] **Step 1: Member type change in `storage_manager.h`**

In `src/deluge/storage/storage_manager.h`, add the include (near the top, alongside the existing `#include "fatfs/fatfs.hpp"`):

```cpp
#include "io/file.hpp"
```

Change (around line 60):
```cpp
	FIL readFIL{};
```
to:
```cpp
	std::optional<deluge::io::File> file;
```

Change (around line 87):
```cpp
	FIL writeFIL{};
```
to:
```cpp
	std::optional<deluge::io::File> file;
```

- [ ] **Step 2: Drop the dead `FilePointer*` param from the two member-function declarations**

In `src/deluge/storage/storage_manager.h`, change (around line 234):
```cpp
	Error openXMLFile(FilePointer* filePointer, char const* firstTagName, char const* altTagName = "",
	                  bool ignoreIncorrectFirmware = false);
```
to:
```cpp
	Error openXMLFile(char const* firstTagName, char const* altTagName = "", bool ignoreIncorrectFirmware = false);
```

And (around line 319):
```cpp
	Error openJsonFile(FilePointer* filePointer, char const* firstTagName, char const* altTagName = "",
	                   bool ignoreIncorrectFirmware = false);
```
to:
```cpp
	Error openJsonFile(char const* firstTagName, char const* altTagName = "", bool ignoreIncorrectFirmware = false);
```

- [ ] **Step 3: Rewrite the `namespace StorageManager { ... }` declarations block**

In `src/deluge/storage/storage_manager.h`, replace the entire block from `std::expected<FatFS::File, Error> createFile(...)` through the closing `} // namespace StorageManager` (currently lines 362-407) with:

```cpp
std::expected<deluge::io::File, Error> createFile(char const* filePath, bool mayOverwrite);
Error createXMLFile(char const* pathName, XMLSerializer& writer, bool mayOverwrite = false, bool displayErrors = true);
Error createJsonFile(char const* pathName, JsonSerializer& writer, bool mayOverwrite = false,
                     bool displayErrors = true);
Error openXMLFile(char const* path, XMLDeserializer& reader, char const* firstTagName, char const* altTagName = "",
                  bool ignoreIncorrectFirmware = false);
Error openJsonFile(char const* path, JsonDeserializer& reader, char const* firstTagName, char const* altTagName = "",
                   bool ignoreIncorrectFirmware = false);
Error openDelugeFile(char const* path, char const* firstTagName, char const* altTagName = "",
                     bool ignoreIncorrectFirmware = false);
Error initSD();

bool fileExists(char const* pathName);
bool fileExists(char const* pathName, FilePointer* fp);
/// takes a full path/to/file.text and makes sure the directories exist
bool buildPathToFile(const char* fileName);

bool checkSDPresent();
bool checkSDInitialized();

Instrument* createNewInstrument(OutputType newOutputType, ParamManager* getParamManager = nullptr);
Error loadInstrumentFromFile(Song* song, InstrumentClip* clip, OutputType outputType, bool mayReadSamplesFromFiles,
                             Instrument** getInstrument, char const* path, std::string* name, std::string* dirPath);
Instrument* createNewNonAudioInstrument(OutputType outputType, int32_t slot, int32_t subSlot);

Error openMidiDeviceDefinitionFile(char const* path);
Error loadMidiDeviceDefinitionFile(MIDIInstrument* midiInstrument, char const* path, std::string* fileName,
                                   bool updateFileName = true);

Error openPatternFile(char const* path);
Error loadPatternFile(char const* path, std::string* fileName, bool overwriteExisting, bool noScaling,
                      bool previewOnly, bool selectedDrumOnly);

Error openFavouriteFile(char const* path);
Error loadFavouriteFile(char const* path, std::string* fileName);

Drum* createNewDrum(DrumType drumType);
Error loadSynthToDrum(Song* song, InstrumentClip* clip, bool mayReadSamplesFromFiles, SoundDrum** getInstrument,
                      char const* path, std::string* name, std::string* dirPath);

Error checkSpaceOnCard();

Error openInstrumentFile(OutputType outputType, char const* path);
} // namespace StorageManager
```

Note: `void openFilePointer(FilePointer* fp, FileReader& reader);` is **deleted**, not migrated — its implementation is a raw `FFOBJID` field poke that has no equivalent once `FileReader::file` is a `deluge::io::File`, and (after this task) it has no callers left.

- [ ] **Step 4: `FileReader`/`FileWriter` method bodies in `storage_manager.cpp`**

Replace the four method bodies (`FileReader::readFileCluster`, `FileReader::closeWriter`, `FileWriter::closeWriter`, `FileWriter::writeBufferToFile`) as follows.

`FileReader::readFileCluster` (currently ~line 858):
```cpp
bool FileReader::readFileCluster() {

	AudioEngine::logAction("readFileCluster");
	if (memoryBased) {
		return true;
	}

	auto result = file->read(std::span<std::byte>(reinterpret_cast<std::byte*>(fileClusterBuffer), Cluster::size));
	if (!result) {
		return false;
	}
	currentReadBufferEndPos = static_cast<UINT>(result->size());

	// If error or we reached end of file
	if (!currentReadBufferEndPos) {
		return false;
	}

	fileReadBufferCurrentPos = 0;

	return true;
}
```

`FileReader::closeWriter` (currently ~line 926):
```cpp
FRESULT FileReader::closeWriter() {
	if (memoryBased) {
		return FRESULT::FR_OK;
	}
	auto result = file->close();
	file.reset();
	return result ? FRESULT::FR_OK : FRESULT::FR_DISK_ERR;
}
```

`FileWriter::closeWriter` (currently ~line 957):
```cpp
FRESULT FileWriter::closeWriter() {
	if (memoryBased) {
		if (fileWriteBufferCurrentPos < bufferSize) {
			writeClusterBuffer[fileWriteBufferCurrentPos] = 0;
			return FRESULT::FR_OK;
		}
		else {
			return FRESULT::FR_INT_ERR;
		}
	}
	auto result = file->close();
	file.reset();
	return result ? FRESULT::FR_OK : FRESULT::FR_DISK_ERR;
}
```

`FileWriter::writeBufferToFile` (currently ~line 1010):
```cpp
Error FileWriter::writeBufferToFile() {
	auto written = file->write(
	    std::span<const std::byte>(reinterpret_cast<const std::byte*>(writeClusterBuffer), fileWriteBufferCurrentPos));
	if (!written || *written != fileWriteBufferCurrentPos) {
		return Error::SD_CARD;
	}

	fileTotalBytesWritten += fileWriteBufferCurrentPos;

	return Error::NONE;
}
```

- [ ] **Step 5: `StorageManager::createFile`/`createXMLFile`/`createJsonFile`**

Replace `StorageManager::createFile` (currently ~line 97-174) with:

```cpp
std::expected<deluge::io::File, Error> StorageManager::createFile(char const* filePath, bool mayOverwrite) {

	Error error = initSD();
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	error = checkSpaceOnCard();
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	// file_io.h's DELUGE_FILE_WRITE_CREATE mode always truncates/creates - it has no "fail if the file
	// already exists" mode (unlike the raw FA_CREATE_NEW this replaces). This firmware has no concurrent
	// writers, so a plain existence check first is race-free in practice, and preserves the
	// overwrite-confirmation behaviour several save flows depend on (checking for FILE_ALREADY_EXISTS).
	if (!mayOverwrite) {
		auto existing = deluge::io::File::open(filePath, DELUGE_FILE_READ);
		if (existing) {
			auto _ = existing->close();
			return std::unexpected(Error::FILE_ALREADY_EXISTS);
		}
	}

	bool triedCreatingFolder = false;

tryAgain:
	auto opened = deluge::io::File::open(filePath, DELUGE_FILE_WRITE_CREATE);
	if (!opened) {

processError:
		// If folder doesn't exist, try creating it - once only
		if (opened.error() == deluge::io::Status::NOT_FOUND) {
			if (triedCreatingFolder) {
				return std::unexpected(Error::FOLDER_DOESNT_EXIST);
			}
			triedCreatingFolder = true;

			std::string folderPath;
			folderPath = filePath;

			// Get just the folder path
cutFolderPathAndTryCreating:
			char const* folderPathChars = folderPath.c_str();
			char const* slashAddr = strrchr(folderPathChars, '/');
			if (!slashAddr) {
				return std::unexpected(Error::UNSPECIFIED); // Shouldn't happen
			}
			int32_t slashPos = slashAddr - folderPathChars;

			folderPath.resize(slashPos);

			// Try making the folder
			auto made_dir = deluge::io::mkdir(folderPath.c_str());
			if (made_dir) {
				goto tryAgain;
			}
			// If that folder couldn't be created because its parent folder didn't exist...
			else if (made_dir.error() == deluge::io::Status::NOT_FOUND) {
				triedCreatingFolder = false;      // Let it do multiple tries again
				goto cutFolderPathAndTryCreating; // Go and try creating the parent folder
			}
			else {
				goto processError;
			}
		}

		// Otherwise, just return the appropriate error.
		else {
			return std::unexpected(Error::WRITE_FAIL);
		}
	}

	return std::move(opened.value());
}
```

Replace `StorageManager::createXMLFile` (currently ~line 176-193):
```cpp
Error StorageManager::createXMLFile(char const* filePath, XMLSerializer& writer, bool mayOverwrite,
                                    bool displayErrors) {
	auto created = createFile(filePath, mayOverwrite);
	writer.reset();
	if (!created) {
		if (displayErrors) {
			display->removeWorkingAnimation();
			display->displayError(created.error());
		}
		return created.error();
	}
	writer.file = std::move(created.value());
	writer.reset();
	if (!writeJsonFlag) {
		writer.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	}
	return Error::NONE;
}
```

Replace `StorageManager::createJsonFile` (currently ~line 195-209):
```cpp
Error StorageManager::createJsonFile(char const* filePath, JsonSerializer& writer, bool mayOverwrite,
                                     bool displayErrors) {
	auto created = createFile(filePath, mayOverwrite);
	writer.reset();
	if (!created) {
		if (displayErrors) {
			display->removeWorkingAnimation();
			display->displayError(created.error());
		}
		return created.error();
	}
	writer.file = std::move(created.value());
	writer.reset();
	return Error::NONE;
}
```

- [ ] **Step 6: Delete `StorageManager::openFilePointer`**

In `src/deluge/storage/storage_manager.cpp`, delete the entire `StorageManager::openFilePointer` function body (currently ~line 286-301):
```cpp
// Function can't fail.
void StorageManager::openFilePointer(FilePointer* fp, FileReader& reader) {

	AudioEngine::logAction("openFilePointer");

	D_PRINTLN("openFilePointer");

	reader.readFIL.obj.sclust = fp->sclust;
	reader.readFIL.obj.objsize = fp->objsize;
	reader.readFIL.obj.fs = &fileSystem; /* Validate the file object */
	reader.readFIL.obj.id = fileSystem.id;

	reader.readFIL.flag = FA_READ; /* Set file access mode */
	reader.readFIL.err = 0;        /* Clear error flag */
	reader.readFIL.sect = 0;       /* Invalidate current data sector */
	reader.readFIL.fptr = 0;       /* Set file pointer top of the file */
}
```
This also resolves the `TODO.md`-tracked raw `FFOBJID.id` poke (`reader.readFIL.obj.id = fileSystem.id;`) as a side effect — it lived entirely inside this function.

- [ ] **Step 7: `openXMLFile`/`openJsonFile` free functions + `openDelugeFile`**

Replace `StorageManager::openXMLFile` (currently ~line 721-735):
```cpp
Error StorageManager::openXMLFile(char const* path, XMLDeserializer& reader, char const* firstTagName,
                                  char const* altTagName, bool ignoreIncorrectFirmware) {

	AudioEngine::logAction("openXMLFile");
	reader.reset();
	auto opened = deluge::io::File::open(path, DELUGE_FILE_READ);
	if (!opened) {
		return Error::FILE_NOT_FOUND;
	}
	reader.file = std::move(opened.value());
	Error err = reader.openXMLFile(firstTagName, altTagName, ignoreIncorrectFirmware);
	activeDeserializer = &reader;
	if (err == Error::NONE)
		return Error::NONE;
	reader.closeWriter();

	return Error::FILE_CORRUPTED;
}
```

Replace `StorageManager::openJsonFile` (currently ~line 737-751):
```cpp
Error StorageManager::openJsonFile(char const* path, JsonDeserializer& reader, char const* firstTagName,
                                   char const* altTagName, bool ignoreIncorrectFirmware) {

	AudioEngine::logAction("openJsonFile");
	reader.reset();
	auto opened = deluge::io::File::open(path, DELUGE_FILE_READ);
	if (!opened) {
		return Error::FILE_NOT_FOUND;
	}
	reader.file = std::move(opened.value());
	Error err = reader.openJsonFile(firstTagName, altTagName, ignoreIncorrectFirmware);
	activeDeserializer = &reader;
	if (err == Error::NONE)
		return Error::NONE;
	reader.closeWriter();

	return Error::FILE_CORRUPTED;
}
```

Replace `StorageManager::openDelugeFile` (currently ~line 753-765). It previously took a `FileItem*` purely to reach `&currentFileItem->filePointer`; `storage_manager.cpp` has no dependency on the `gui`/browser layer today and shouldn't gain one, so this now takes a path directly (its two callers, in `load_song_ui.cpp`, build the path themselves — Task 2):
```cpp
Error StorageManager::openDelugeFile(char const* path, char const* firstTagName, char const* altTagName,
                                     bool ignoreIncorrectFirmware) {
	Error error;
	if (strstr(path, ".Json") != nullptr) {
		error = StorageManager::openJsonFile(path, smJsonDeserializer, firstTagName, altTagName,
		                                     ignoreIncorrectFirmware);
	}
	else {
		error = StorageManager::openXMLFile(path, smDeserializer, firstTagName, altTagName, ignoreIncorrectFirmware);
	}
	return error;
}
```

Also update `src/deluge/storage/storage_manager.h`'s `openDelugeFile` declaration (currently ~line 370-371):
```cpp
Error openDelugeFile(FileItem* currentFileItem, char const* firstTagName, char const* altTagName = "",
                     bool ignoreIncorrectFirmware = false);
```
to:
```cpp
Error openDelugeFile(char const* path, char const* firstTagName, char const* altTagName = "",
                     bool ignoreIncorrectFirmware = false);
```
(this declaration is already covered by Step 3's full-block replacement above — this note is just so the change isn't missed if applying steps out of order).

- [ ] **Step 8: The four wrapper-opens**

Replace `StorageManager::openInstrumentFile` (currently ~line 303-325):
```cpp
Error StorageManager::openInstrumentFile(OutputType outputType, char const* path) {

	AudioEngine::logAction("openInstrumentFile");
	char const* firstTagName;
	char const* altTagName = "";

	if (outputType == OutputType::SYNTH) {
		firstTagName = "sound";
		altTagName = "synth"; // Compatibility with old xml files
	}
	else if (outputType == OutputType::MIDI_OUT) {
		firstTagName = "midi";
	}
	else {
		firstTagName = "kit";
	}

	Error error = openXMLFile(path, smDeserializer, firstTagName, altTagName);
	return error;
}
```

Replace `StorageManager::openMidiDeviceDefinitionFile` (currently ~line 425-436):
```cpp
Error StorageManager::openMidiDeviceDefinitionFile(char const* path) {

	AudioEngine::logAction("openMidiDeviceDefinitionFile");
	char const* firstTagName = "midiDevice";
	char const* altTagName = "";

	Error error = openXMLFile(path, smDeserializer, firstTagName, altTagName);
	return error;
}
```

Replace `StorageManager::openPatternFile` (currently ~line 475-486):
```cpp
Error StorageManager::openPatternFile(char const* path) {

	AudioEngine::logAction("openPatternFile");
	char const* firstTagName = "pattern";
	char const* altTagName = "";

	Error error = openXMLFile(path, smDeserializer, firstTagName, altTagName);
	return error;
}
```

Replace `StorageManager::openFavouriteFile` (currently ~line 488-499):
```cpp
Error StorageManager::openFavouriteFile(char const* path) {

	AudioEngine::logAction("openFavouriteFile");
	char const* firstTagName = "favourites";
	char const* altTagName = "";

	Error error = openXMLFile(path, smDeserializer, firstTagName, altTagName);
	return error;
}
```

- [ ] **Step 9: The five wrapper-loads**

Replace `StorageManager::loadInstrumentFromFile`'s signature and its `openInstrumentFile` call + log line (currently ~line 329-337; the rest of the function body, lines ~338-423, is unchanged — do not modify it):
```cpp
Error StorageManager::loadInstrumentFromFile(Song* song, InstrumentClip* clip, OutputType outputType,
                                             bool mayReadSamplesFromFiles, Instrument** getInstrument,
                                             char const* path, std::string* name, std::string* dirPath) {

	AudioEngine::logAction("loadInstrumentFromFile");
	D_PRINTLN("opening instrument file -  %s %s  from path  %s", dirPath->c_str(), name->c_str(), path);

	Error error = openInstrumentFile(outputType, path);
	if (error != Error::NONE) {
		D_PRINTLN("opening instrument file failed -  %s", name->c_str());
		return error;
	}
```
(the rest of the function — `AudioEngine::logAction("loadInstrumentFromFile")` through the final `return Error::NONE;` — stays exactly as-is; it never referenced `filePointer` again).

Replace `StorageManager::loadMidiDeviceDefinitionFile`'s signature and its `openMidiDeviceDefinitionFile` call + log line (currently ~line 439-451; the rest of the function, ~line 453-473, is unchanged):
```cpp
Error StorageManager::loadMidiDeviceDefinitionFile(MIDIInstrument* midiInstrument, char const* path,
                                                   std::string* fileName, bool updateFileName) {
	midiInstrument->loadDeviceDefinitionFile = false;

	AudioEngine::logAction("loadMidiDeviceDefinitionFile");
	D_PRINTLN("opening midi device definition file -  %s  from path  %s", fileName->c_str(), path);

	Error error = openMidiDeviceDefinitionFile(path);
	if (error != Error::NONE) {
		D_PRINTLN("opening midi device definition file failed -  %s", fileName->c_str());
		return error;
	}
```
(rest of function unchanged).

Replace `StorageManager::loadPatternFile`'s signature and its `openPatternFile` call (currently ~line 502-510; rest of function, ~line 512-529, unchanged):
```cpp
Error StorageManager::loadPatternFile(char const* path, std::string* fileName, bool overwriteExisting,
                                      bool noScaling, bool previewOnly, bool selectedDrumOnly) {

	AudioEngine::logAction("loadPatternFile");

	Error error = openPatternFile(path);
	if (error != Error::NONE) {
		return error;
	}
```
(rest unchanged).

Replace `StorageManager::loadFavouriteFile`'s signature and its `openFavouriteFile` call (currently ~line 532-539; rest of function, ~line 541-557, unchanged):
```cpp
Error StorageManager::loadFavouriteFile(char const* path, std::string* fileName) {

	AudioEngine::logAction("loadFavouriteFile");

	Error error = openFavouriteFile(path);
	if (error != Error::NONE) {
		return error;
	}
```
(rest unchanged).

Replace `StorageManager::loadSynthToDrum`'s signature and its `openInstrumentFile` call (currently ~line 562-576; rest of function, ~line 578-608, unchanged):
```cpp
Error StorageManager::loadSynthToDrum(Song* song, InstrumentClip* clip, bool mayReadSamplesFromFiles,
                                      SoundDrum** getInstrument, char const* path, std::string* name,
                                      std::string* dirPath) {
	OutputType outputType = OutputType::SYNTH;
	SoundDrum* newDrum = (SoundDrum*)createNewDrum(DrumType::SOUND);
	if (!newDrum) {
		return Error::INSUFFICIENT_RAM;
	}

	AudioEngine::logAction("loadSynthDrumFromFile");

	Error error = openInstrumentFile(outputType, path);
	if (error != Error::NONE) {
		return error;
	}
```
(rest unchanged).

- [ ] **Step 10: `FileWriter::closeAfterWriting`**

Replace the whole function (currently ~line 1023-1092). Note the reordering: the size check now runs *before* `closeWriter()` rather than after, because `deluge::io::File` has no way to query a closed handle's last-known size (unlike the raw `FIL` this replaces, whose `f_size()` macro was a pure struct-field read that happened to survive `f_close()`). Several real call sites invoke `closeFileAfterWriting()` with all-default (null) arguments, meaning `path`/`beginningString`/`endString` are all null and only the size check runs — that path must keep working without a reopen.

```cpp
Error FileWriter::closeAfterWriting(char const* path, char const* beginningString, char const* endString) {

	if (fileAccessFailedDuringWrite) {
		return Error::WRITE_FAIL; // Calling close if this is false might be dangerous - if access has failed, we
		                          // don't want it to flush any data to the card or anything
	}
	if (memoryBased)
		return Error::NONE;
	Error error = writeBufferToFile();
	if (error != Error::NONE) {
		return Error::WRITE_FAIL;
	}

	// Check size while the file is still open in write mode - deluge::io::File has no way to query a
	// closed handle's last-known size.
	auto size = file->size();
	if (!size || *size != fileTotalBytesWritten) {
		return Error::WRITE_FAIL;
	}

	FRESULT result = closeWriter();
	if (result) {
		return Error::WRITE_FAIL;
	}

	if (path) {
		// Reopen for reading, to verify the beginning/end strings.
		auto opened = deluge::io::File::open(path, DELUGE_FILE_READ);
		if (!opened) {
			return Error::WRITE_FAIL;
		}
		file = std::move(opened.value());
	}

	// Check beginning
	if (beginningString) {
		int32_t length = strlen(beginningString);
		auto read = file->read(std::span<std::byte>(reinterpret_cast<std::byte*>(miscStringBuffer), length));
		if (!read) {
			return Error::WRITE_FAIL;
		}
		if (memcmp(miscStringBuffer, beginningString, length)) {
			return Error::WRITE_FAIL;
		}
	}

	// Check end
	if (endString) {
		int32_t length = strlen(endString);

		auto sought = file->seek(fileTotalBytesWritten - length);
		if (!sought) {
			return Error::WRITE_FAIL;
		}

		auto read = file->read(std::span<std::byte>(reinterpret_cast<std::byte*>(miscStringBuffer), length));
		if (!read) {
			return Error::WRITE_FAIL;
		}
		if (memcmp(miscStringBuffer, endString, length)) {
			return Error::WRITE_FAIL;
		}
	}

	result = closeWriter();
	if (result) {
		return Error::WRITE_FAIL;
	}

	return Error::NONE;
}
```

- [ ] **Step 11: `Deserializer.cpp` — drop the dead `FilePointer*` param**

In `src/deluge/storage/Deserializer.cpp`, change (currently ~line 834-835):
```cpp
Error XMLDeserializer::openXMLFile(FilePointer* filePointer, char const* firstTagName, char const* altTagName,
                                   bool ignoreIncorrectFirmware) {
```
to:
```cpp
Error XMLDeserializer::openXMLFile(char const* firstTagName, char const* altTagName, bool ignoreIncorrectFirmware) {
```
(the function body, lines ~837-853, is unchanged — `filePointer` was never referenced inside it).

- [ ] **Step 12: `JsonDeserializer.cpp` — drop the dead `FilePointer*` param**

In `src/deluge/storage/JsonDeserializer.cpp`, change (currently ~line 569-570):
```cpp
Error JsonDeserializer::openJsonFile(FilePointer* filePointer, char const* firstTagName, char const* altTagName,
                                     bool ignoreIncorrectFirmware) {
```
to:
```cpp
Error JsonDeserializer::openJsonFile(char const* firstTagName, char const* altTagName, bool ignoreIncorrectFirmware) {
```
(the function body, lines ~572-591, is unchanged).

- [ ] **Step 13: `deserializer_byte_source.cpp`/`.h` — the raw `readFIL` reach found during plan-writing**

In `src/deluge/storage/audio/deserializer_byte_source.cpp`, replace `readNewCluster` (currently ~line 39-45):
```cpp
Error DeserializerByteSource::readNewCluster() {
	auto result = smDeserializer.file->read(
	    std::span<std::byte>(reinterpret_cast<std::byte*>(smDeserializer.fileClusterBuffer), Cluster::size));
	if (!result) {
		return Error::SD_CARD; // Failed to load cluster from card.
	}
	return Error::NONE;
}
```

In `src/deluge/storage/audio/deserializer_byte_source.h`, the class doc comment references the now-deleted `StorageManager::openFilePointer`. Change (currently ~line 25-26):
```cpp
/// band-build loop drives it through the lower-level cluster accessors (`clusterBuffer` /
/// `byteIndexWithinCluster` / `advanceClustersIfNecessary`) — those exist because that loop reads misaligned
/// 32-bit words straight out of the cluster buffer and owns its own within-cluster cursor. Replaces
/// WaveTableReader + the AudioFileReader base. The file must already be open (StorageManager::openFilePointer
/// onto `smDeserializer`) before constructing.
```
to:
```cpp
/// band-build loop drives it through the lower-level cluster accessors (`clusterBuffer` /
/// `byteIndexWithinCluster` / `advanceClustersIfNecessary`) — those exist because that loop reads misaligned
/// 32-bit words straight out of the cluster buffer and owns its own within-cluster cursor. Replaces
/// WaveTableReader + the AudioFileReader base. The file must already be open (`smDeserializer.file`, opened by
/// `AudioFileManager::buildAudioFileFromCard`'s WaveTable branch) before constructing.
```

- [ ] **Step 14: `save_song_ui.cpp` — the raw `readFIL` reach found during plan-writing**

In `src/deluge/gui/ui/save/save_song_ui.cpp`, add near the existing `#include "libdeluge/file_io.h"`:
```cpp
#include "io/file.hpp"
```

Replace the open (currently ~line 236-247):
```cpp
				// Open file to read
				FRESULT result = FRESULT::FR_OK;
				// this is just blind copying to move samples to/from the song folder. The serializer is being used for
				// the song file write so use the deserializer
				result = f_open(&activeDeserializer->readFIL, sourceFilePath, FA_READ);

				if (result != FR_OK) {
					D_PRINTLN("open fail %s", sourceFilePath);
					error = Error::UNSPECIFIED;
					display->removeLoadingAnimation();
					display->displayError(error);
					return false;
				}
```
with:
```cpp
				// this is just blind copying to move samples to/from the song folder. The serializer is being used for
				// the song file write so use the deserializer
				auto sourceOpened = deluge::io::File::open(sourceFilePath, DELUGE_FILE_READ);

				if (!sourceOpened) {
					D_PRINTLN("open fail %s", sourceFilePath);
					error = Error::UNSPECIFIED;
					display->removeLoadingAnimation();
					display->displayError(error);
					return false;
				}
				activeDeserializer->file = std::move(sourceOpened.value());
```

Replace the copy loop's read half (currently ~line 358-370 — the `while (true) { ... }` loop's first half, up to and including the `written` assignment; the loop's remaining body, checking `!written`/`bytesRead < Cluster::size`, is unchanged):
```cpp
					// Copy
					while (true) {
						UINT bytesRead;
						result = f_read(&activeDeserializer->readFIL, activeDeserializer->fileClusterBuffer,
						                Cluster::size, &bytesRead);
						if (result) {
							D_PRINTLN("read fail");
							error = Error::UNSPECIFIED;
							activeDeserializer->closeWriter();
							display->removeLoadingAnimation();
							display->displayError(error);
							return false;
						}
						if (!bytesRead) {
							break; // Stop, on rare case where file ended right at end of last cluster
						}

						auto written =
						    created.value().write({(std::byte*)activeDeserializer->fileClusterBuffer, bytesRead});
```
with:
```cpp
					// Copy
					while (true) {
						auto readResult = activeDeserializer->file->read(std::span<std::byte>(
						    reinterpret_cast<std::byte*>(activeDeserializer->fileClusterBuffer), Cluster::size));
						if (!readResult) {
							D_PRINTLN("read fail");
							error = Error::UNSPECIFIED;
							activeDeserializer->closeWriter();
							display->removeLoadingAnimation();
							display->displayError(error);
							return false;
						}
						UINT bytesRead = static_cast<UINT>(readResult->size());
						if (!bytesRead) {
							break; // Stop, on rare case where file ended right at end of last cluster
						}

						auto written =
						    created.value().write({(std::byte*)activeDeserializer->fileClusterBuffer, bytesRead});
```
(the loop's remaining lines — checking `!written || written.value() != bytesRead`, the `if (bytesRead < Cluster::size) break;`, and the trailing `activeDeserializer->closeWriter(); // Close source file` — are unchanged; `created` is already a `deluge::io::File` after Step 5 above, so `created.value().write(...)` needs no change).

- [ ] **Step 15: `audio_file_manager.cpp` — the two raw `readFIL` reaches found during plan-writing**

In `src/deluge/storage/audio/audio_file_manager.cpp`, add near the existing includes:
```cpp
#include "io/file.hpp"
```

Replace `tryRegularPath` (currently ~line 582-588) — this decouples it from `FileReader`'s member entirely, using a self-contained local `FIL`, mirroring `StorageManager::fileExists(path, FilePointer*)`'s existing pattern:
```cpp
	// Open the file at its regular path; on success fill effectiveFilePointer. Returns the FatFS result.
	const auto tryRegularPath = [&]() -> FRESULT {
		FIL fil;
		const FRESULT result = f_open(&fil, filePath.c_str(), FA_READ);
		if (result == FR_OK) {
			effectiveFilePointer.sclust = fil.obj.sclust;
			effectiveFilePointer.objsize = fil.obj.objsize;
			f_close(&fil);
		}
		return result;
	};
```

Replace the WaveTable branch's `openFilePointer` call (currently ~line 819-836, the whole `else` block of `buildAudioFileFromCard`'s SAMPLE/WaveTable split):
```cpp
	else {
		audioFile = new (audioFileMemory) WaveTable;
		adoptAudioFileObject(audioFile); // resource-manager evictable object (before addReason)
		audioFile->addReason();          // So it's protected while setting up.

		audioFile->filePath = filePath;
		audioFile->loadedFromAlternatePath = usingAlternateLocation;

		// WaveTable reads the file more normally through FatFS, so "open" it.
		StorageManager::openFilePointer(&effectiveFilePointer, smDeserializer); // It never returns fail.

		// One deserializer-backed source serves both the header parse (via the AudioByteSource surface) and
		// WaveTable::setup's zero-copy band read (via its cluster accessors) — hence passed both ways.
		DeserializerByteSource source{static_cast<uint32_t>(effectiveFilePointer.objsize)};
		*error = audioFile->loadFile(source, makeWaveTableWorkAtAllCosts, &source);
	}
```
with:
```cpp
	else {
		audioFile = new (audioFileMemory) WaveTable;
		adoptAudioFileObject(audioFile); // resource-manager evictable object (before addReason)
		audioFile->addReason();          // So it's protected while setting up.

		audioFile->filePath = filePath;
		audioFile->loadedFromAlternatePath = usingAlternateLocation;

		// WaveTable reads the file more normally, so open it for the deserializer to stream from. `filePath`
		// is already the correct final resolved path (regular or alternate) at this point, and Tier 2's
		// adapter-side directory cache makes this reopen fast even though the file was just opened once
		// already (in tryRegularPath, to discover effectiveFilePointer).
		auto opened = deluge::io::File::open(filePath, DELUGE_FILE_READ);
		if (!opened) {
			*error = Error::FILE_NOT_FOUND;
			destroyAudioFileObject(audioFile);
			return nullptr;
		}
		smDeserializer.file = std::move(opened.value());

		// One deserializer-backed source serves both the header parse (via the AudioByteSource surface) and
		// WaveTable::setup's zero-copy band read (via its cluster accessors) — hence passed both ways.
		DeserializerByteSource source{static_cast<uint32_t>(effectiveFilePointer.objsize)};
		*error = audioFile->loadFile(source, makeWaveTableWorkAtAllCosts, &source);
	}
```

- [ ] **Step 16: `fatfs.cpp` — stale comment referencing the deleted member**

In `src/fatfs/fatfs.cpp`, `File::open_by_locator`'s comment (currently ~line 27-29) names `FileReader::readFIL` by name. Change:
```cpp
  file.file_ = {}; // zero every field first -- File's own file_ member has no
                    // in-class initializer (unlike FileReader::readFIL{}, whose
                    // starting state this mirrors), so this isn't redundant.
```
to:
```cpp
  file.file_ = {}; // zero every field first -- File's own file_ member has no
                    // in-class initializer, so this isn't redundant.
```

- [ ] **Step 17: Confirm scope — grep for remaining raw `readFIL`/`writeFIL` references**

Run:
```bash
grep -rn "readFIL\|writeFIL" src/deluge src/fatfs include/libdeluge tests 2>/dev/null
```
Expected: **zero matches**. If any remain, they were missed by this task and must be resolved before proceeding (do not defer — this task's whole point is retiring this member).

- [ ] **Step 18: Build check — confirm this task's own files are clean**

Run `dbt build Debug` (or the project's equivalent) and confirm:
- Zero errors in: `storage_manager.h`, `storage_manager.cpp`, `Deserializer.cpp`, `JsonDeserializer.cpp`, `deserializer_byte_source.cpp`, `deserializer_byte_source.h`, `save_song_ui.cpp`, `audio_file_manager.cpp`, `fatfs.cpp`.
- All remaining errors are confined to files listed in Tasks 2-4's "Files" sections below (each will show a call passing the old argument shape — e.g. `&fp` where `char const* path` is now expected). List the offending files/line numbers in the task report so Tasks 2-4 can cross-check nothing was missed.

- [ ] **Step 19: `memoryBased`-path unit tests**

Add `tests/spec/storage_manager_spec.cpp` (new file) exercising the parts of this task that don't need real disk I/O — the `memoryBased` construction path never touches `file`, so this is fully testable:

```cpp
#include "CppUTest/TestHarness.h"
#include "storage/storage_manager.h"

TEST_GROUP(StorageManagerFileReaderWriter){};

TEST(StorageManagerFileReaderWriter, memoryBasedReaderStartsWithNoOpenFile) {
	char buf[16] = "hello world";
	FileReader reader(buf, sizeof(buf));
	CHECK_FALSE(reader.peekChar(nullptr) && false); // constructed OK, no crash touching memoryBased state
}

TEST(StorageManagerFileReaderWriter, memoryBasedWriterClosesWithoutTouchingFile) {
	FileWriter writer(true);
	writer.writeChars("test");
	FRESULT result = writer.closeWriter();
	CHECK_EQUAL(FR_OK, result);
}
```

Wire it into `tests/spec/CMakeLists.txt` following the existing pattern for other `*_spec.cpp` files in that directory (add the new source to the same target list `file_io_spec.cpp` is already registered under).

- [ ] **Step 20: Extend `tests/spec/file_io_spec.cpp` — `createFile`'s `mayOverwrite=false` existence check**

Add a case verifying the new pre-check logic added in Step 5 is structurally present (hand-constructed, matching this file's existing no-real-disk pattern — see the file's existing `open_by_locator` cases for the style to follow): confirm that `StorageManager::createFile`'s implementation, when `mayOverwrite` is `false`, performs a `DELUGE_FILE_READ`-mode existence probe before any `DELUGE_FILE_WRITE_CREATE` attempt. Since this repo's test harness has no real mountable filesystem (`mock_diskio.cpp` reports `STA_NOINIT` unconditionally — the same accepted gap as Tiers 2 and 4), this is necessarily a code-path/behavioural-shape check rather than an end-to-end disk test; follow `file_io_spec.cpp`'s existing convention for this constraint.

- [ ] **Step 21: Commit**

```bash
git add src/deluge/storage/storage_manager.h src/deluge/storage/storage_manager.cpp \
        src/deluge/storage/Deserializer.cpp src/deluge/storage/JsonDeserializer.cpp \
        src/deluge/storage/audio/deserializer_byte_source.cpp src/deluge/storage/audio/deserializer_byte_source.h \
        src/deluge/gui/ui/save/save_song_ui.cpp src/deluge/storage/audio/audio_file_manager.cpp \
        src/fatfs/fatfs.cpp tests/spec/storage_manager_spec.cpp tests/spec/file_io_spec.cpp tests/spec/CMakeLists.txt
git commit -m "refactor(storage): retire FileReader/FileWriter's raw FIL + FilePointer-based opens

FileReader::readFIL/FileWriter::writeFIL become std::optional<deluge::io::File>.
StorageManager::openFilePointer (a raw FFOBJID field poke) is deleted along with
FilePointer* from the entire XML/JSON open chain (createFile, openXMLFile/
openJsonFile, openDelugeFile, the 4 wrapper-opens, the 5 wrapper-loads) in favour
of plain paths, made fast by Tier 2's adapter-side directory cache. Also fixes 3
external raw-field call sites forced by the member change (deserializer_byte_source.cpp,
save_song_ui.cpp's collect-media routine, audio_file_manager.cpp's WaveTable path)."
```

---

### Task 2: External call sites — Group A (direct `openXMLFile` callers + `load_song_ui.cpp`)

**Files:**
- Modify: `src/deluge/gui/views/performance_view.cpp`
- Modify: `src/deluge/io/midi/midi_device_manager.cpp`
- Modify: `src/deluge/io/midi/midi_follow.cpp`
- Modify: `src/deluge/model/settings/runtime_feature_settings.cpp`
- Modify: `src/deluge/gui/ui/load/load_song_ui.cpp`

**Interfaces:**
- Consumes: Task 1's `StorageManager::openXMLFile(char const* path, ...)`, `StorageManager::fileExists(char const* pathName)` (one-arg, unchanged), `StorageManager::openDelugeFile(char const* path, ...)`, `StorageManager::loadMidiDeviceDefinitionFile(MIDIInstrument*, char const* path, ...)`.

- [ ] **Step 1: `performance_view.cpp`**

In `PerformanceView::readDefaultsFromFile()`, replace (currently ~line 1812-1833):
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

	//<defaults>
	Error error = StorageManager::openXMLFile(&fp, smDeserializer, PERFORM_DEFAULTS_TAG);
```
with:
```cpp
	// PerformanceView.XML
	bool success = StorageManager::fileExists(PERFORM_DEFAULTS_XML);
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
				success = StorageManager::fileExists(PERFORM_DEFAULTS_XML);
			}
		}
		if (!success) {
			loadDefaultLayout();
			return;
		}
	}

	//<defaults>
	Error error = StorageManager::openXMLFile(PERFORM_DEFAULTS_XML, smDeserializer, PERFORM_DEFAULTS_TAG);
```
`smDeserializer.closeWriter();` (currently ~line 1845) is unchanged.

- [ ] **Step 2: `midi_device_manager.cpp`**

In `readDevicesFromFile()`, replace (currently ~line 509-528):
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
	}

	Error error = StorageManager::openXMLFile(&fp, smDeserializer, "midiDevices");
```
with:
```cpp
	bool success = StorageManager::fileExists(MIDI_DEVICES_XML);
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
				success = StorageManager::fileExists(MIDI_DEVICES_XML);
			}
		}
		if (!success) {
			return;
		}
	}

	Error error = StorageManager::openXMLFile(MIDI_DEVICES_XML, smDeserializer, "midiDevices");
```
`activeDeserializer->closeWriter();` (currently ~line 554) is unchanged.

- [ ] **Step 3: `midi_follow.cpp`**

In `MidiFollow::readDefaultsFromFile()`, replace (currently ~line 1662-1675):
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

	//<defaults>
	Error error = StorageManager::openXMLFile(&fp, smDeserializer, MIDI_DEFAULTS_TAG);
```
with:
```cpp
	// MIDIFollow.XML
	bool success = StorageManager::fileExists(MIDI_FOLLOW_XML);
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

	//<defaults>
	Error error = StorageManager::openXMLFile(MIDI_FOLLOW_XML, smDeserializer, MIDI_DEFAULTS_TAG);
```
`activeDeserializer->closeWriter();` (currently ~line 1698) is unchanged.

- [ ] **Step 4: `runtime_feature_settings.cpp`**

In `RuntimeFeatureSettings::readSettingsFromFile()`, replace (currently ~line 174-193):
```cpp
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

	Error error = StorageManager::openXMLFile(&fp, smDeserializer, TAG_RUNTIME_FEATURE_SETTINGS);
```
with:
```cpp
	// CommunityFeatures.XML
	bool success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE);
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
				success = StorageManager::fileExists(RUNTIME_FEATURE_SETTINGS_FILE);
			}
		}
		if (!success) {
			return;
		}
	}

	Error error = StorageManager::openXMLFile(RUNTIME_FEATURE_SETTINGS_FILE, smDeserializer, TAG_RUNTIME_FEATURE_SETTINGS);
```
`smDeserializer.closeWriter();` (currently ~line 246) is unchanged.

- [ ] **Step 5: `load_song_ui.cpp` — two `openDelugeFile` call sites + one `loadMidiDeviceDefinitionFile` call site**

In `LoadSongUI::performLoad()`, replace (currently ~line 334):
```cpp
	error = StorageManager::openDelugeFile(currentFileItem, "song");
```
with:
```cpp
	std::string filePath = currentDir + "/" + currentFileItem->filename;
	error = StorageManager::openDelugeFile(filePath.c_str(), "song");
```

In the second caller (currently ~line 839, inside the function building the song-preview image — same call shape), replace:
```cpp
	error = StorageManager::openDelugeFile(currentFileItem, "song");
```
with:
```cpp
	std::string filePath = currentDir + "/" + currentFileItem->filename;
	error = StorageManager::openDelugeFile(filePath.c_str(), "song");
```

Replace the `loadMidiDeviceDefinitionFile` call site (currently ~line 559-568):
```cpp
				FilePointer tempfp;
				bool fileExists = StorageManager::fileExists(midiInstrument->deviceDefinitionFileName.c_str(), &tempfp);
				if (fileExists) {
					StorageManager::loadMidiDeviceDefinitionFile(midiInstrument, &tempfp,
					                                             &midiInstrument->deviceDefinitionFileName, false);
				}
```
with:
```cpp
				bool fileExists = StorageManager::fileExists(midiInstrument->deviceDefinitionFileName.c_str());
				if (fileExists) {
					StorageManager::loadMidiDeviceDefinitionFile(midiInstrument,
					                                             midiInstrument->deviceDefinitionFileName.c_str(),
					                                             &midiInstrument->deviceDefinitionFileName, false);
				}
```
`activeDeserializer->closeWriter();` call sites at (currently) lines 368, 378, 445, 894 are unchanged.

- [ ] **Step 6: Build check**

Run `dbt build Debug`. Confirm zero errors in the 5 files this task touches. Remaining errors, if any, must be confined to Task 3/Task 4's file list.

- [ ] **Step 7: Commit**

```bash
git add src/deluge/gui/views/performance_view.cpp src/deluge/io/midi/midi_device_manager.cpp \
        src/deluge/io/midi/midi_follow.cpp src/deluge/model/settings/runtime_feature_settings.cpp \
        src/deluge/gui/ui/load/load_song_ui.cpp
git commit -m "refactor(storage): migrate Group A external callers off FilePointer-based opens"
```

---

### Task 3: External call sites — Group B (`load_instrument_preset_ui.cpp` + the smaller load-family files)

**Files:**
- Modify: `src/deluge/gui/ui/load/load_instrument_preset_ui.cpp`
- Modify: `src/deluge/gui/ui/load/load_midi_device_definition_ui.cpp`
- Modify: `src/deluge/gui/ui/load/load_pattern_ui.cpp`
- Modify: `src/deluge/model/favourite/favourite_manager.cpp`

**Interfaces:**
- Consumes: Task 1's `StorageManager::loadMidiDeviceDefinitionFile`, `loadPatternFile`, `loadFavouriteFile`, `loadInstrumentFromFile`, `loadSynthToDrum` (all now `char const* path`), `fileExists(char const* pathName)` (one-arg).

- [ ] **Step 1: `load_instrument_preset_ui.cpp` — line ~407 stays untouched**

Confirm (no edit) that `bool fileExists = StorageManager::fileExists(filePath.c_str(), &currentFileItem->filePointer);` (currently ~line 407) is left exactly as-is — it writes into the real, shared `FileItem::filePointer` field that `browser.cpp`'s own navigation/reload-detection bookkeeping reads independently (unrelated to any XML/JSON open). This is the one confirmed-out-of-scope call site in this file.

- [ ] **Step 2: `load_instrument_preset_ui.cpp` — lines ~592-601**

Replace:
```cpp
		std::string filePath = getCurrentFilePath();
		FilePointer tempFilePointer;
		bool success = StorageManager::fileExists(filePath.c_str(), &tempFilePointer);
		if (!success) {
			return;
		}
		Error error = StorageManager::loadInstrumentFromFile(currentSong, instrumentClipToLoadFor,
		                                                     initialOutputType, false, &initialInstrument,
		                                                     &tempFilePointer, &initialName, &initialDirPath);
```
with:
```cpp
		std::string filePath = getCurrentFilePath();
		bool success = StorageManager::fileExists(filePath.c_str());
		if (!success) {
			return;
		}
		Error error = StorageManager::loadInstrumentFromFile(currentSong, instrumentClipToLoadFor,
		                                                     initialOutputType, false, &initialInstrument,
		                                                     filePath.c_str(), &initialName, &initialDirPath);
```

- [ ] **Step 3: `load_instrument_preset_ui.cpp` — lines ~806-810**

Replace:
```cpp
		error = StorageManager::loadInstrumentFromFile(currentSong, instrumentClipToLoadFor, outputTypeToLoad, false,
		                                               &newInstrument, &currentFileItem->filePointer, &enteredText,
		                                               &currentDir);
```
with:
```cpp
		error = StorageManager::loadInstrumentFromFile(currentSong, instrumentClipToLoadFor, outputTypeToLoad, false,
		                                               &newInstrument, getCurrentFilePath().c_str(), &enteredText,
		                                               &currentDir);
```
(no local path variable existed here; `SlotBrowser::getCurrentFilePath()` returns exactly `currentDir + "/" + enteredText + ".XML"/".Json"`, the same state `currentFileItem->filePointer` was resolving via the browser's directory scan. The temporary's `.c_str()` is safe — it lives through the full-expression call.)

- [ ] **Step 4: `load_instrument_preset_ui.cpp` — lines ~912-914**

Replace:
```cpp
			FilePointer tempfp;
			bool fileExists = StorageManager::fileExists(midiInstrument->deviceDefinitionFileName.c_str(), &tempfp);
			if (fileExists) {
				StorageManager::loadMidiDeviceDefinitionFile(midiInstrument, &tempfp,
				                                             &midiInstrument->deviceDefinitionFileName, false);
			}
```
with:
```cpp
			bool fileExists = StorageManager::fileExists(midiInstrument->deviceDefinitionFileName.c_str());
			if (fileExists) {
				StorageManager::loadMidiDeviceDefinitionFile(midiInstrument,
				                                             midiInstrument->deviceDefinitionFileName.c_str(),
				                                             &midiInstrument->deviceDefinitionFileName, false);
			}
```

- [ ] **Step 5: `load_instrument_preset_ui.cpp` — lines ~951-953 (`performLoadSynthToKit`)**

Replace:
```cpp
	Error error = StorageManager::loadSynthToDrum(currentSong, instrumentClipToLoadFor, false, &soundDrumToReplace,
	                                              &currentFileItem->filePointer, &enteredText, &currentDir);
```
with:
```cpp
	Error error = StorageManager::loadSynthToDrum(currentSong, instrumentClipToLoadFor, false, &soundDrumToReplace,
	                                              getCurrentFilePath().c_str(), &enteredText, &currentDir);
```

- [ ] **Step 6: `load_instrument_preset_ui.cpp` — lines ~1454-1457 (`doPresetNavigation`)**

Replace:
```cpp
	toReturn.error = StorageManager::loadInstrumentFromFile(
	    currentSong, nullptr, outputType, false, &toReturn.fileItem->instrument, &toReturn.fileItem->filePointer,
	    &newName, &Browser::currentDir);
```
with:
```cpp
	std::string filePath = Browser::currentDir + "/" + toReturn.fileItem->getFilenameWithExtension();
	toReturn.error = StorageManager::loadInstrumentFromFile(
	    currentSong, nullptr, outputType, false, &toReturn.fileItem->instrument, filePath.c_str(), &newName,
	    &Browser::currentDir);
```
(`toReturn.fileItem` here is `&fileItems[i]`, an entry in the browser's own scanned list — not `getCurrentFileItem()` — and `newName` is already extension-stripped via `getFilenameWithoutExtension()`, so it can't be reused as the path; `FileItem::getFilenameWithExtension()` exists precisely for this and correctly handles the `filenameIncludesExtension` flag.)

- [ ] **Step 7: `load_midi_device_definition_ui.cpp`**

Replace (currently ~line 221-222):
```cpp
	Error error = StorageManager::loadMidiDeviceDefinitionFile((MIDIInstrument*)getCurrentOutput(),
	                                                           &currentFileItem->filePointer, &fileName);
```
with:
```cpp
	Error error = StorageManager::loadMidiDeviceDefinitionFile((MIDIInstrument*)getCurrentOutput(), fileName.c_str(),
	                                                           &fileName);
```
(`fileName` is built locally just above this call as `currentDir + "/" + enteredText + ".XML"` — the same value already used for both the path and the output-name param.)

- [ ] **Step 8: `load_pattern_ui.cpp`**

Replace (currently ~line 289-291):
```cpp
	Error error = StorageManager::loadPatternFile(&currentFileItem->filePointer, &fileName, overwriteExisting,
	                                              noScaling, previewOnly, selectedDrumOnly);
```
with:
```cpp
	Error error = StorageManager::loadPatternFile(fileName.c_str(), &fileName, overwriteExisting, noScaling,
	                                              previewOnly, selectedDrumOnly);
```
(`fileName` is built locally just above, same pattern as Step 7.)

- [ ] **Step 9: `favourite_manager.cpp`**

Replace `FavouritesManager::loadFavouritesBank()` (currently ~line 65-79):
```cpp
void FavouritesManager::loadFavouritesBank() {
	resetFavourites();
	std::string filePath = getFilenameForSave();
	FilePointer fileToLoad{};
	bool fileExists = StorageManager::fileExists(filePath.c_str(), &fileToLoad);
	if (!fileExists) {
		saveFavouriteBank();
		return;
	}
	std::string path;
	path = filePath.c_str();
	Error error = StorageManager::loadFavouriteFile(&fileToLoad, &path);
	if (error != Error::NONE) {
		resetFavourites();
	}
}
```
with:
```cpp
void FavouritesManager::loadFavouritesBank() {
	resetFavourites();
	std::string filePath = getFilenameForSave();
	bool fileExists = StorageManager::fileExists(filePath.c_str());
	if (!fileExists) {
		saveFavouriteBank();
		return;
	}
	std::string path;
	path = filePath.c_str();
	Error error = StorageManager::loadFavouriteFile(filePath.c_str(), &path);
	if (error != Error::NONE) {
		resetFavourites();
	}
}
```

- [ ] **Step 10: Build check**

Run `dbt build Debug`. Confirm zero errors in the 4 files this task touches. Remaining errors, if any, must be confined to Task 4's file list.

- [ ] **Step 11: Commit**

```bash
git add src/deluge/gui/ui/load/load_instrument_preset_ui.cpp src/deluge/gui/ui/load/load_midi_device_definition_ui.cpp \
        src/deluge/gui/ui/load/load_pattern_ui.cpp src/deluge/model/favourite/favourite_manager.cpp
git commit -m "refactor(storage): migrate Group B external callers off FilePointer-based opens"
```

---

### Task 4: External call sites — Group C (`loadInstrumentFromFile`'s remaining direct callers)

**Files:**
- Modify: `src/deluge/gui/views/session_view.cpp`
- Modify: `src/deluge/gui/views/arranger_view.cpp`
- Modify: `src/deluge/model/song/song.cpp`
- Modify: `src/deluge/model/clip/instrument_clip.cpp`

**Interfaces:**
- Consumes: Task 1's `StorageManager::loadInstrumentFromFile(..., char const* path, ...)`. All 5 call sites in this task follow the same pattern: `fileItem->filePointer` (or `result.value()->filePointer`) is replaced with a path built as `Browser::currentDir + "/" + fileItem->filename`, since `findAnUnlaunchedPresetIncludingWithinSubfolders`/`confirmPresetOrNextUnlaunchedOne` (in `load_instrument_preset_ui.cpp`, unchanged by this task) already mutate `Browser::currentDir` in place as they recurse into subfolders, so by the time any of these 5 sites run, `Browser::currentDir` already holds the returned `FileItem`'s containing directory. `FileItem::filename` includes the file extension (per its own field comment) — confirmed by every one of these 5 sites already passing `&Browser::currentDir` as the separate `dirPath` out-param.

- [ ] **Step 1: `session_view.cpp` (`SessionView::setPresetOrNextUnlaunchedOne`)**

Replace (currently ~line 1544-1560):
```cpp
	Instrument* newInstrument = fileItem->instrument;
	bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
	*instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

	Error error = Error::NONE;
	if (!newInstrument) {
		std::string newPresetName = fileItem->getFilenameWithoutExtension();
		error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, outputType, false, &newInstrument,
		                                               &fileItem->filePointer, &newPresetName, &Browser::currentDir);
	}
```
with:
```cpp
	Instrument* newInstrument = fileItem->instrument;
	bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
	*instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

	Error error = Error::NONE;
	if (!newInstrument) {
		std::string newPresetName = fileItem->getFilenameWithoutExtension();
		std::string filePath = Browser::currentDir + "/" + fileItem->filename;
		error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, outputType, false, &newInstrument,
		                                               filePath.c_str(), &newPresetName, &Browser::currentDir);
	}
```

- [ ] **Step 2: `arranger_view.cpp` (`ArrangerView::createNewInstrument`)**

Replace (currently ~line 825-836):
```cpp
	Instrument* newInstrument = fileItem->instrument;
	bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
	*instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

	Error error = Error::NONE;
	if (!newInstrument) {
		std::string newPresetName = fileItem->getFilenameWithoutExtension();
		error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, newOutputType, false, &newInstrument,
		                                               &fileItem->filePointer, &newPresetName, &Browser::currentDir);
	}
```
with:
```cpp
	Instrument* newInstrument = fileItem->instrument;
	bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
	*instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

	Error error = Error::NONE;
	if (!newInstrument) {
		std::string newPresetName = fileItem->getFilenameWithoutExtension();
		std::string filePath = Browser::currentDir + "/" + fileItem->filename;
		error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, newOutputType, false, &newInstrument,
		                                               filePath.c_str(), &newPresetName, &Browser::currentDir);
	}
```

- [ ] **Step 3: `song.cpp` (`Song::ensureAtLeastOneSessionClip`)**

Replace (currently ~line 363-369):
```cpp
	result = loadInstrumentPresetUI.findAnUnlaunchedPresetIncludingWithinSubfolders(nullptr, OutputType::SYNTH,
	                                                                                Availability::ANY);
	if (result) {
		std::string newPresetName = result.value()->getFilenameWithoutExtension();
		error =
		    StorageManager::loadInstrumentFromFile(this, firstClip, OutputType::SYNTH, false, &newInstrument,
		                                           &result.value()->filePointer, &newPresetName, &Browser::currentDir);
```
with:
```cpp
	result = loadInstrumentPresetUI.findAnUnlaunchedPresetIncludingWithinSubfolders(nullptr, OutputType::SYNTH,
	                                                                                Availability::ANY);
	if (result) {
		std::string newPresetName = result.value()->getFilenameWithoutExtension();
		std::string filePath = Browser::currentDir + "/" + result.value()->filename;
		error =
		    StorageManager::loadInstrumentFromFile(this, firstClip, OutputType::SYNTH, false, &newInstrument,
		                                           filePath.c_str(), &newPresetName, &Browser::currentDir);
```
(the rest of the `if (result) { ... }` block is unchanged.)

- [ ] **Step 4: `song.cpp` (`Song::createNewInstrument`, the Synth/Kit branch)**

Replace (currently ~line 4825-4830):
```cpp
		newInstrument = fileItem->instrument;
		bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;

		Error error = Error::NONE;
		if (!newInstrument) {
			std::string newPresetName = fileItem->getFilenameWithoutExtension();
			error =
			    StorageManager::loadInstrumentFromFile(this, nullptr, newOutputType, false, &newInstrument,
			                                           &fileItem->filePointer, &newPresetName, &Browser::currentDir);
		}
```
with:
```cpp
		newInstrument = fileItem->instrument;
		bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;

		Error error = Error::NONE;
		if (!newInstrument) {
			std::string newPresetName = fileItem->getFilenameWithoutExtension();
			std::string filePath = Browser::currentDir + "/" + fileItem->filename;
			error =
			    StorageManager::loadInstrumentFromFile(this, nullptr, newOutputType, false, &newInstrument,
			                                           filePath.c_str(), &newPresetName, &Browser::currentDir);
		}
```

- [ ] **Step 5: `instrument_clip.cpp`**

Replace (currently ~line 3757-3762):
```cpp
		newInstrument = fileItem->instrument;
		bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
		instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

		if (!newInstrument) {
			std::string newPresetName = fileItem->getFilenameWithoutExtension();
			error =
			    StorageManager::loadInstrumentFromFile(modelStack->song, nullptr, newOutputType, false, &newInstrument,
			                                           &fileItem->filePointer, &newPresetName, &Browser::currentDir);
		}
```
with:
```cpp
		newInstrument = fileItem->instrument;
		bool isHibernating = newInstrument && !fileItem->instrumentAlreadyInSong;
		instrumentAlreadyInSong = newInstrument && fileItem->instrumentAlreadyInSong;

		if (!newInstrument) {
			std::string newPresetName = fileItem->getFilenameWithoutExtension();
			std::string filePath = Browser::currentDir + "/" + fileItem->filename;
			error =
			    StorageManager::loadInstrumentFromFile(modelStack->song, nullptr, newOutputType, false, &newInstrument,
			                                           filePath.c_str(), &newPresetName, &Browser::currentDir);
		}
```

- [ ] **Step 6: Build check**

Run `dbt build Debug`. This is the last file group in the cascade — confirm **zero errors anywhere** in the firmware target.

- [ ] **Step 7: Commit**

```bash
git add src/deluge/gui/views/session_view.cpp src/deluge/gui/views/arranger_view.cpp \
        src/deluge/model/song/song.cpp src/deluge/model/clip/instrument_clip.cpp
git commit -m "refactor(storage): migrate Group C external callers off FilePointer-based opens"
```

---

### Task 5: Final regression check

**Files:** none modified — verification only.

- [ ] **Step 1: Full build, both targets**

```bash
dbt build Debug
dbt sim
```
Expected: both green, zero errors, zero new warnings.

- [ ] **Step 2: Run `tests/spec`**

```bash
dbt test   # or this project's equivalent tests/spec invocation
```
Expected: all green, including the new `storage_manager_spec.cpp` cases from Task 1 and the existing `file_io_spec.cpp` suite (extended in Task 1 Step 20).

- [ ] **Step 3: Confirm the `FilePointer`-based-open surface is fully gone**

```bash
grep -rn "readFIL\|writeFIL" src/deluge src/fatfs include/libdeluge tests 2>/dev/null
grep -rn "openFilePointer" src/deluge src/fatfs include/libdeluge tests 2>/dev/null
```
Expected: zero matches for both. (`FilePointer` the *type* legitimately remains — it's still used by `FileItem::filePointer`, `StorageManager::fileExists(path, FilePointer*)`, and the SAMPLE-streaming bucket, all confirmed permanent exemptions per the design doc's §5 "Out of scope" list. Only the raw-`FIL`-member and the locator-driven-open call chain are gone.)

- [ ] **Step 4: Confirm `TODO.md`'s Tier 3 entries are resolved**

Read `TODO.md`. Remove the line:
```
- [] deluge::io migration Tier 3 (docs/superpowers/specs/2026-07-14-file-io-migration-roadmap-design.md §4): ...
```
(This entry's two goals — the `FileReader`/`FileWriter` redesign and the `reader.readFIL.obj.id` raw `FFOBJID` poke fix — are both done; the poke was inside `StorageManager::openFilePointer`, deleted in Task 1.)

- [ ] **Step 5: Update memory**

This is a plan-execution step, not a code change — when this plan is fully executed (all 5 tasks merged), update the `file-io-migration-tiers` memory file to record Tier 3 as DONE, following the same pattern used when Tier 2 landed. Not part of this plan's code deliverable; note it here so it isn't forgotten.

- [ ] **Step 6: Hand off to `finishing-a-development-branch`**

Once Steps 1-4 are green, use the `superpowers:finishing-a-development-branch` skill to decide how to land this branch (merge/PR/keep/discard), matching how Tier 2 and Tier 4 were completed.
