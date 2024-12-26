/*
 * Copyright © 2017-2023 Synthstrom Audible Limited
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

#include "storage/audio/audio_file_manager.h"
#include "definitions.h"
#include "definitions_cxx.hpp"
#include "extern.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "io/debug/log.h"
#include "io/midi/midi_device_manager.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cache.h"
#include "model/sample/sample_reader.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file.h"
#include "storage/cluster/cluster.h"
#include "storage/storage_manager.h"
#include "storage/wave_table/wave_table.h"
#include "storage/wave_table/wave_table_reader.h"
#include "util/containers.h"
#include "util/exceptions.h"
#include "util/try.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <ranges>

extern "C" {
#include "fatfs/diskio.h"
#include "fatfs/ff.h"

DWORD get_fat_from_fs(                      /* 0xFFFFFFFF:Disk error, 1:Internal error, 2..0x7FFFFFFF:Cluster status */
                      FATFS* fs, DWORD clst /* Cluster number to get the value */
);

LBA_t clst2sect(           /* !=0:Sector number, 0:Failed (invalid cluster#) */
                FATFS* fs, /* Filesystem object */
                DWORD clst /* Cluster# to be converted */
);

DRESULT disk_read_without_streaming_first(BYTE pdrv, BYTE* buff, DWORD sector, UINT count);

extern uint8_t currentlyAccessingCard;
}

AudioFileManager audioFileManager{};

AudioFileManager::AudioFileManager() {
	highestUsedAudioRecordingNumber.fill(-1);
	highestUsedAudioRecordingNumberNeedsReChecking.set();
}

void AudioFileManager::firstCardRead() {
	if (cardReadOnce) {
		cardReinserted();
	}
	else {
		init();
	}
}

void AudioFileManager::init() {

	clusterBeingLoaded = nullptr;

	Error error = StorageManager::initSD();
	if (error == Error::NONE) {
		Cluster::setSize(fileSystem.csize * 512);

		D_PRINTLN("Cluster::size  %d clusterSizeMagnitude  %d", Cluster::size, Cluster::size_magnitude);
		cardEjected = false;
		cardReadOnce = true;
	}

	else {
		Cluster::setSize(Cluster::kSizeFAT16Max);
		cardEjected = true;
	}

	clusterSizeAtBoot = Cluster::size;
}

void AudioFileManager::cardReinserted() {

	cardDisabled = false;
	for (int32_t i = 0; i < kNumAudioRecordingFolders; i++) {
		highestUsedAudioRecordingNumberNeedsReChecking[i] = true;
	}

	// If cluster size has increased, we're in trouble
	if (fileSystem.csize * 512 > Cluster::size) {

		// But, if it's still not as big as it was when we booted up, that's still manageable
		if (fileSystem.csize * 512 <= clusterSizeAtBoot) {
			goto clusterSizeChangedButItsOk;
		}

		D_PRINTLN("cluster size increased and we're in trouble");
		cardDisabled = true;
		display->displayPopup(deluge::l10n::get(deluge::l10n::String::STRING_FOR_REBOOT_TO_USE_THIS_SD_CARD));
	}

	// If cluster size decreased, we have to stop all current samples from ever sounding again. Pretty big trouble
	// really...
	else if (fileSystem.csize * 512 < Cluster::size) {

clusterSizeChangedButItsOk:
		D_PRINTLN("cluster size changed, and smaller than original so it's ok");
		AudioEngine::unassignAllVoices(); // Will also stop synth voices - too bad.

		// If any files aren't currently used, take this opportunity to remove them from memory
		releaseAllUnused();

		// Otherwise, mark all remaining samples as unplayable
		for (Sample* sample : sampleFiles | std::views::values) {
			sample->unplayable = true;
		}

		// That was all a pain, but now we can update the cluster size
		Cluster::setSize(fileSystem.csize * 512);
	}

	// Or if cluster size stayed the same...
	else {
		// If any files aren't currently used, take this opportunity to remove them from memory
		releaseAllUnused();

		// Go through every Sample in memory
		for (Sample* thisSample : sampleFiles | std::views::values) {
			// Check the Sample's file still exists
			FIL sampleFile;
			char const* filePath = thisSample->tempFilePathForRecording.get();
			if (!*filePath) {
				filePath = thisSample->filePath.get();
			}

			FRESULT result = f_open(&sampleFile, filePath, FA_READ);
			if (result != FR_OK) {
				D_PRINTLN("couldn't open file");
				thisSample->markAsUnloadable();
				continue;
			}

			uint32_t firstSector = clst2sect(&fileSystem, sampleFile.obj.sclust);

			f_close(&sampleFile);

			// If address of first sector remained unchanged, we can be sure enough that the file hasn't been
			// changed
			if (firstSector == thisSample->clusters.getElement(0)->sdAddress) {}

			// Otherwise
			else {
				thisSample->markAsUnloadable();
				continue;
			}

			// Or if we're still here, the file's fine - who knows, maybe it's even fine again after it wasn't
			// for a while (e.g. if the user temporarily had a different card inserted)
			thisSample->unloadable = false;
		}
	}

	MIDIDeviceManager::readDevicesFromFile(); // Hopefully we can do this now. It'll only happen if it
	                                          // wasn't able to do it before.
}

// Call this after deleting the current (or in other words previous) Song from memory - meaning there won't be any
// further reason we'd ever move these temp samples into the permanent sample folder, meaning we don't want them in
// memory listed with their would-be real permanent filenames. Also, we won't be needing to play them back again. You
// must not call this during the card or audio routines.
void AudioFileManager::deleteAnyTempRecordedSamplesFromMemory() {

	// Also though, in case any of these Samples were still being recorded before the Song-delete, we need to make sure
	// SampleRecorder::cardRoutine() gets called first to "detach" the Sample from the recorder. So, do this:
	AudioEngine::doRecorderCardRoutines();

	for (Sample* sample : sampleFiles | std::views::values) {
		// If it's a temp-recorded one
		if (!sample->tempFilePathForRecording.isEmpty()) {

			// if (ALPHA_OR_BETA_VERSION && audioFile->numReasons) FREEZE_WITH_ERROR("E281"); // It definitely
			// shouldn't still have any reasons
			//  No - it could still have a reason - the reason of its SampleRecorder. Scenario where this happened
			//  was: recording AudioClip (instance) into Arranger when loading a new song, first causes Arranger
			//  playback to switch to Session playback, which causes finishLinearRecording() on AudioClip, so when
			//  song-swap does happen, the AudioClip no longer has a recorder, so the recorder doesn't clear stuff,
			//  and it's still not quite yet finalized the file, so still holds the "reason" to the Sample.
			//  TODO: although the Sample doesn't store a pointer to the SampleRecorder, we could easily search for
			//  it - and delete it and its "reason"?

			// We know Sample belonged to an AudioClip originally because only those ones can be TEMP
			highestUsedAudioRecordingNumberNeedsReChecking[util::to_underlying(AudioRecordingFolder::CLIPS)] = true;

			// We may have deleted several, so do make sure we go and re-check from 0
			highestUsedAudioRecordingNumber[util::to_underlying(AudioRecordingFolder::CLIPS)] = -1;

			releaseFile(*sample);
		}
	}
}

// Oi, don't even think about modifying this to take a Sample* pointer - cos the whole Sample could get deleted during
// the card access.
Error AudioFileManager::getUnusedAudioRecordingFilePath(String& filePath, String* tempFilePathForRecording,
                                                        AudioRecordingFolder folder, uint32_t* getNumber,
                                                        const char* channelName, String* songName) {
	const auto folderID = util::to_underlying(folder);

	Error error = StorageManager::initSD();
	if (error != Error::NONE) {
		return error;
	}
	// this caches the last used numbers in the main folders to avoid repeated reads. This is probably good there since
	// some people have hundreds of recordings, but it's unnecessary for song specific folders since the number of
	// recordings will be much smaller
	if (highestUsedAudioRecordingNumberNeedsReChecking[folderID]) {

		FRESULT result = f_opendir(&staticDIR, audioRecordingFolderNames[folderID]);
		if (result == FR_OK) {

			while (true) {
				loadAnyEnqueuedClusters();
				FRESULT result = f_readdir(&staticDIR, &staticFNO); /* Read a directory item */
				if (__builtin_expect(result != FR_OK, 0)) {
					return Error::SD_CARD;
				}

				if (__builtin_expect((*(uint32_t*)staticFNO.altname & 0x00FFFFFF) == 0x00434552, 1)) { // "REC"
					if (*(uint32_t*)&staticFNO.altname[8] == 0x5641572E) {                             // ".WAV"

						int32_t thisSlot = memToUIntOrError(&staticFNO.altname[3], &staticFNO.altname[8]);
						if (thisSlot == -1) {
							continue;
						}

						if (thisSlot > highestUsedAudioRecordingNumber[folderID]) {
							highestUsedAudioRecordingNumber[folderID] = thisSlot;
						}
					}
				}
				else if (!staticFNO.altname[0]) {
					break; /* Break on end of dir */
				}
			}
			// f_closedir(&staticDIR);
		}

		highestUsedAudioRecordingNumberNeedsReChecking[folderID] = false;
	}

	highestUsedAudioRecordingNumber[folderID]++;

	D_PRINTLN("new file: --------------  %d", highestUsedAudioRecordingNumber[folderID]);

	error = filePath.set(audioRecordingFolderNames[folderID]);
	if (error != Error::NONE) {
		return error;
	}

	bool doingTempFolder = (folder == AudioRecordingFolder::CLIPS);
	if (doingTempFolder) {
		error = tempFilePathForRecording->set(audioRecordingFolderNames[folderID]);
		if (error != Error::NONE) {
			return error;
		}
		error = tempFilePathForRecording->concatenate("/TEMP");
		if (error != Error::NONE) {
			return error;
		}
	}

	// default to putting it in the main folder if the song isn't named
	if (songName->isEmpty()) {
		error = filePath.concatenate("/REC");
		if (error != Error::NONE) {
			return error;
		}
		error = filePath.concatenateInt(highestUsedAudioRecordingNumber[folderID], 5);
		if (error != Error::NONE) {
			return error;
		}
		error = filePath.concatenate(".WAV");
		if (error != Error::NONE) {
			return error;
		}

		if (doingTempFolder) {
			error = tempFilePathForRecording->concatenate(&filePath.get()[strlen(audioRecordingFolderNames[folderID])]);
			if (error != Error::NONE) {
				return error;
			}
		}
	}
	// otherwise file it under the song name
	else {
		char namedPath[255]{0};
		char tempPath[255]{0};
		int i = 0;
		bool changed = true;
		// iterate through the main and temp folders until we find a path that's free in both
		while (changed) {
			changed = false;
			snprintf(namedPath, sizeof(namedPath), "%s/%s/%s_%03d.wav", filePath.get(), songName->get(), channelName,
			         i);
			while (StorageManager::fileExists(namedPath)) {
				snprintf(namedPath, sizeof(namedPath), "%s/%s/%s_%03d.wav", filePath.get(), songName->get(),
				         channelName, i);
				i++;
				changed = true;
			}
			if (doingTempFolder) {
				snprintf(tempPath, sizeof(tempPath), "%s/%s/%s_%03d.wav", tempFilePathForRecording->get(),
				         songName->get(), channelName, i);

				while (StorageManager::fileExists(tempPath)) {
					snprintf(tempPath, sizeof(tempPath), "%s/%s/%s_%03d.wav", tempFilePathForRecording->get(),
					         songName->get(), channelName, i);
					i++;
					changed = true;
				}
			}
		}
		error = filePath.set(namedPath);
		if (error != Error::NONE) {
			return error;
		}
		if (doingTempFolder) {
			error = tempFilePathForRecording->set(tempPath);
			if (error != Error::NONE) {
				return error;
			}
		}
	}

	*getNumber = highestUsedAudioRecordingNumber[folderID];

	return Error::NONE;
}

// Returns false if exists but can't be deleted
bool AudioFileManager::releaseSampleAtFilePath(String& filePath) {
	if (auto search = sampleFiles.find(&filePath); search != sampleFiles.end()) {
		Sample& sample = *search->second;

		// Ok, it's in memory. Can we delete it - is it unused?
		if (sample.numReasonsToBeLoaded > 0) {
			return false; // Alert - not only is it in memory, but it also can't be deleted
		}

		// Ok, it's unused. Delete it.
		releaseFile(sample);
	}
	return true; // We're fine - it got deleted
}

///@brief release unused sapmles and wavetables
void AudioFileManager::releaseAllUnused() {
	for (Sample* sample : sampleFiles | std::views::values) {
		if (sample->numReasonsToBeLoaded <= 0) {
			releaseFile(*sample);
		}
	}

	for (WaveTable* wavetable : wavetableFiles | std::views::values) {
		if (wavetable->numReasonsToBeLoaded <= 0) {
			releaseFile(*wavetable);
		}
	}
}

void AudioFileManager::releaseFile(Sample& sample) {
	sampleFiles.erase(&sample.filePath);
	sample.~Sample();
	delugeDealloc(&sample);
}

void AudioFileManager::releaseFile(WaveTable& wavetable) {
	wavetableFiles.erase(&wavetable.filePath);
	wavetable.~WaveTable();
	delugeDealloc(&wavetable);
}

std::expected<void, Error> AudioFileManager::setupAlternateAudioFileDir(String& newPath, char const* rootDir,
                                                                        String& songFilenameWithoutExtension) {

	Error error = newPath.set(rootDir);
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	error = newPath.concatenate("/");
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	error = newPath.concatenate(&songFilenameWithoutExtension);
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	return std::expected<void, Error>{};
}

std::expected<void, Error> AudioFileManager::setupAlternateAudioFilePath(String& newPath, int32_t dirPathLength,
                                                                         String& oldPath) {
	Error error = newPath.concatenateAtPos(&oldPath.get()[8], dirPathLength); // The [8] skips us past "SAMPLES/"
	if (error != Error::NONE) {
		return std::unexpected(error);
	}

	int32_t pos = dirPathLength;

	while (true) {
		char const* newPathChars = newPath.get();
		char const* slashAddress = strchr(&newPathChars[pos], '/');
		if (!slashAddress) {
			break;
		}
		int32_t slashPos = (uint32_t)slashAddress - (uint32_t)newPathChars;
		Error error = newPath.setChar('_', slashPos);
		if (error != Error::NONE) {
			return std::unexpected(error);
		}
		pos = slashPos + 1;
	}

	return std::expected<void, Error>{};
}

std::expected<AudioFile*, Error> //
AudioFileManager::getAudioFileFromFilename(String& filePath, bool mayReadCard, FilePointer* suppliedFilePointer,
                                           AudioFileType type, bool makeWaveTableWorkAtAllCosts) {
	Error error = Error::NONE;
	String backedUpFilePath;

	std::optional<AudioFile*> foundAudioFile{};

	// See if it's already in memory.
	if (type == AudioFileType::SAMPLE) {
		if (auto search = sampleFiles.find(&filePath); search != sampleFiles.end()) {
			return search->second;
		}
		// If we want Sample but got Wavetable, can't convert, so we'll have to load from file after all. Reset
		// filePath if needed (pretty unlikely scenario).
		if (wavetableFiles.contains(&filePath)) {
			if (!backedUpFilePath.isEmpty()) {
				filePath.set(&backedUpFilePath);
			}
			goto tryLoadingFromCard;
		}
	}
	else if (type == AudioFileType::WAVETABLE) {
		if (auto search = wavetableFiles.find(&filePath); search != wavetableFiles.end()) {
			return search->second;
		}

		// If we want WaveTable but got Sample, we can convert. (Otherwise, we can't.)
		if (auto search = sampleFiles.find(&filePath); search != sampleFiles.end()) {
			Sample* foundSample = search->second;

			// Stereo files can never be WaveTables
			if (foundSample->numChannels != 1) {
				return std::unexpected(Error::FILE_NOT_LOADABLE_AS_WAVETABLE_BECAUSE_STEREO);
			}

			// And if the user isn't insisting, then some other signs show that we probably don't want to load this
			// as a WaveTable
			if (!makeWaveTableWorkAtAllCosts) {
				if (isAiffFilename(foundSample->filePath.get())) {
					return std::unexpected(Error::FILE_NOT_LOADABLE_AS_WAVETABLE);
				}

				// If this isn't actually a wavetable-specifying file or at least a wavetable-looking length, and
				// the user isn't insisting, then opt not to do it.
				if (!foundSample->fileExplicitlySpecifiesSelfAsWaveTable && foundSample->lengthInSamples & 2047) {
					return std::unexpected(Error::FILE_NOT_LOADABLE_AS_WAVETABLE);
				}
			}

			void* waveTableMemory = GeneralMemoryAllocator::get().allocStealable(sizeof(WaveTable));
			if (!waveTableMemory) {
				return std::unexpected(Error::INSUFFICIENT_RAM);
			}

			auto* newWaveTable = new (waveTableMemory) WaveTable;

			newWaveTable->addReason(); // So it's protected while setting up.
			foundSample->addReason();

			error = newWaveTable->setup(foundSample);
			if (error != Error::NONE) {
				newWaveTable->~WaveTable();
				delugeDealloc(waveTableMemory);
				return std::unexpected(error);
			}

			try {
				wavetableFiles[&newWaveTable->filePath] = newWaveTable;
			} catch (deluge::exception e) {
				if (e == deluge::exception::BAD_ALLOC) {
					newWaveTable->~WaveTable();
					delugeDealloc(waveTableMemory);
					return std::unexpected(Error::INSUFFICIENT_RAM);
				}
				freezeWithError("EXAM");
			}
			newWaveTable->removeReason("E397");
			foundSample->removeReason("E398");
			return newWaveTable;
		}
	}

	// If we didn't find the audio file in memory...
	else {

		// If we're loading a preset (not a Song, and not just browsing audio files), we should search in memory for
		// the alternate path
		if (alternateLoadDirStatus == AlternateLoadDirStatus::MIGHT_EXIST
		    || alternateLoadDirStatus == AlternateLoadDirStatus::DOES_EXIST) {
			if (thingTypeBeingLoaded != ThingType::SONG) {
				String alternateFilePath;
				alternateFilePath.set(&alternateAudioFileLoadPath);
				error = alternateFilePath.concatenate("/");
				if (error != Error::NONE) {
					goto tryLoadingFromCard;
				}
				char const* fileName = getFileNameFromEndOfPath(filePath.get());
				error = alternateFilePath.concatenate(fileName);
				if (error != Error::NONE) {
					goto tryLoadingFromCard;
				}

				std::optional<AudioFile*> foundAudioFile{};
				if (type == AudioFileType::WAVETABLE) {
					if (auto search = wavetableFiles.find(&alternateFilePath); search != wavetableFiles.end()) {
						foundAudioFile = search->second;
					}
				}
				else if (type == AudioFileType::SAMPLE) {
					if (auto search = sampleFiles.find(&alternateFilePath); search != sampleFiles.end()) {
						foundAudioFile = search->second;
					}
				}

				if (foundAudioFile) {
					// Tiny bit cheeky, but we're going to update the file path actually stored in the AudioFile to
					// reflect this alternate location, which will no longer be considered alternate.
					backedUpFilePath.set(&filePath); // First we back up the original filePath.
					filePath.set(&alternateFilePath);
					return foundAudioFile.value();
				}
			}
		}
	}

tryLoadingFromCard:
	// Otherwise, try and load it in
	if (!mayReadCard) {
		return nullptr;
	}

	if (cardDisabled) {
		return std::unexpected(Error::SD_CARD);
	}

	// Deactivated this because it stuff up the sampleI we already found
	/*
	if (!ensureEnoughMemoryForOneMoreSample()) {
	    *error = Error::INSUFFICIENT_RAM;
	    return NULL;
	}
	*/

	String usingAlternateLocation;

	FilePointer effectiveFilePointer;

	// If we got given a FilePointer, it's easy
	if (suppliedFilePointer) {
		effectiveFilePointer = *suppliedFilePointer;
	}

	// Otherwise go on the filePath
	else {

		bool alreadyTriedRegular = false;
		FRESULT result;

		// If we know the alternate load directory actually exists, then we should try that first, cos there's a
		// high chance the file is in there
		if (alternateLoadDirStatus == AlternateLoadDirStatus::DOES_EXIST) {

tryAlternateDoesExist:
			String proposedFileName;
			char const* proposedFileNamePointer;
			bool alreadyTriedSecondAlternate;

			// We'll first try the long file name, which contains all the folder names from the original path. This
			// is how collect-media saves look - for Songs. But, if that original path didn't begin with "SAMPLES/",
			// we can't do that.
			if (memcasecmp(filePath.get(), "SAMPLES/", 8)) {
				goto tryNextAlternate;
			}

			// This is to generate just the name of the file - not an entire path with folders -
			// despite the function being called ...Path.
			D_TRY(setupAlternateAudioFilePath(proposedFileName, 0, filePath));

			alreadyTriedSecondAlternate = false;

tryAnAlternate:
			proposedFileNamePointer = proposedFileName.get();
			result = create_name(&alternateLoadDir, &proposedFileNamePointer);
			if (result != FR_OK) {
				goto alternateFailed; // Can only fail if filename too weird.
			}

			result = dir_find(&alternateLoadDir);

			if (result != FR_OK) {
alternateFailed:
				if (!alreadyTriedSecondAlternate) {
tryNextAlternate:
					// Next up we'll try looking for just the filename that the original file had, without any added
					// folder names. This allows users to copy files into folders for instruments more easily, and
					// have them load.
					alreadyTriedSecondAlternate = true;
					char const* fileName = getFileNameFromEndOfPath(filePath.get());
					error = proposedFileName.set(fileName);
					if (error != Error::NONE) {
						return std::unexpected(error);
					}
					goto tryAnAlternate;
				}
				if (alreadyTriedRegular) {
					goto notFound;
				}
				else {
					goto tryRegular;
				}
			}

			// Ok, found file - in the alternate location.
			effectiveFilePointer.sclust = ld_clust(&fileSystem, alternateLoadDir.dir);
			effectiveFilePointer.objsize = ld_dword(alternateLoadDir.dir + DIR_FileSize);

			usingAlternateLocation.set(&alternateAudioFileLoadPath);
			error = usingAlternateLocation.concatenate("/");
			if (error != Error::NONE) {
				return std::unexpected(error);
			}
			error = usingAlternateLocation.concatenate(&proposedFileName);
			if (error != Error::NONE) {
				return std::unexpected(error);
			}

			if (thingTypeBeingLoaded == ThingType::SYNTH || thingTypeBeingLoaded == ThingType::KIT) {
				// Special rule for loading presets with files in their dedicated "alternate" folder: must update
				// the AudioFile's filePath to point to that alternate location - and then treat them as normal (not
				// alternate).
				filePath.set(&usingAlternateLocation);
				usingAlternateLocation.clear();
			}
		}

		// Otherwise, try the regular file path
		else {
tryRegular:
			result = f_open(&smDeserializer.readFIL, filePath.get(), FA_READ);

			// If that didn't work, try the alternate load directory, if we didn't already and it potentially exists
			if (result != FR_OK) {

				if (alternateLoadDirStatus == AlternateLoadDirStatus::MIGHT_EXIST) {

					result = f_opendir(&alternateLoadDir, alternateAudioFileLoadPath.get());
					if (result != FR_OK) {
						alternateLoadDirStatus = AlternateLoadDirStatus::NOT_FOUND;
						goto notFound;
					}

					alternateLoadDirStatus = AlternateLoadDirStatus::DOES_EXIST;

					alreadyTriedRegular = true;
					goto tryAlternateDoesExist;
				}

notFound:
				return std::unexpected(Error::FILE_UNREADABLE);
			}

			// Ok, found file.
			effectiveFilePointer.sclust = activeDeserializer->readFIL.obj.sclust;
			effectiveFilePointer.objsize = activeDeserializer->readFIL.obj.objsize;
		}
	}

	// 0-byte files not allowed.
	if (!effectiveFilePointer.objsize) {
		return std::unexpected(Error::FILE_CORRUPTED);
	}

	// Files bigger than 1GB not allowed
	else if (effectiveFilePointer.objsize > kMaxFileSize) {
		return std::unexpected(Error::FILE_TOO_BIG);
	}

	uint32_t numClusters = ((effectiveFilePointer.objsize - 1) >> Cluster::size_magnitude) + 1;

	int32_t memorySizeNeeded = (type == AudioFileType::SAMPLE) ? sizeof(Sample) : sizeof(WaveTable);

	void* audioFileMemory = GeneralMemoryAllocator::get().allocStealable(memorySizeNeeded);
	if (!audioFileMemory) {
ramError:
		return std::unexpected(Error::INSUFFICIENT_RAM);
	}

	char readerMemory[sizeof(SampleReader)];
	AudioFileReader* reader;

	AudioFile* audioFile;
	if (type == AudioFileType::SAMPLE) {
		audioFile = new (audioFileMemory) Sample;
		audioFile->addReason(); // So it's protected while setting up. Must do this before calling initialize().
		error = static_cast<Sample*>(audioFile)->initialize(numClusters);
		if (error != Error::NONE) { // Very rare, only if not enough RAM
			audioFile->~AudioFile();
			delugeDealloc(audioFileMemory);
			return std::unexpected(error);
		}

		reader = new (readerMemory) SampleReader;
	}
	else {
		audioFile = new (audioFileMemory) WaveTable;
		audioFile->addReason(); // So it's protected while setting up.
		reader = new (readerMemory) WaveTableReader;
	}

	audioFile->filePath.set(&filePath);
	audioFile->loadedFromAlternatePath.set(&usingAlternateLocation);

	reader->currentClusterIndex = -1;
	reader->audioFile = audioFile;
	reader->fileSize = effectiveFilePointer.objsize;
	reader->byteIndexWithinCluster = Cluster::size;

	// If Sample, we go directly to god-mode and get the cluster addresses.
	if (type == AudioFileType::SAMPLE) {

		// Store the address of each of the file's clusters.
		uint32_t currentClusterIndex = 0;
		uint32_t currentSDCluster =
		    effectiveFilePointer.sclust; // Start with first cluster, whose address we already got.

		while (true) {

			((Sample*)audioFile)->clusters.getElement(currentClusterIndex)->sdAddress =
			    clst2sect(&fileSystem, currentSDCluster);

			currentClusterIndex++;
			if (currentClusterIndex >= numClusters) {
				break;
			}

			currentSDCluster = get_fat_from_fs(&fileSystem, currentSDCluster);

			if (currentSDCluster == 0xFFFFFFFF || currentSDCluster < 2) {
				break;
			}
		}

		// if (!suppliedFilePointer) f_close(&fileSystemStuff.currentFile);

		((SampleReader*)reader)->currentCluster = nullptr;
	}

	// Or if WaveTable, we're going to read the file more normally through FatFS, so we want to "open" it.
	else {
		StorageManager::openFilePointer(&effectiveFilePointer, smDeserializer); // It never returns fail.
	}

	// Read top-level RIFF headers
	uint32_t topHeader[3];
	error = reader->readBytes((char*)topHeader, 3 * 4);
	if (error != Error::NONE) {
		goto ensureSafeThenCheckError;
	}

	if (topHeader[0] == 0x46464952       // "RIFF"
	    && topHeader[2] == 0x45564157) { // "WAVE"
		error = audioFile->loadWAVE(*reader, makeWaveTableWorkAtAllCosts);
	}
	else if (topHeader[0] == 0x4D524F46       // "FORM"
	         && topHeader[2] == 0x46464941) { // "AIFF"
		error = audioFile->loadAIFF(*reader, makeWaveTableWorkAtAllCosts);
	}
	else {
		error = Error::FILE_UNSUPPORTED;
	}

ensureSafeThenCheckError:
	if (type == AudioFileType::SAMPLE) {
		auto& sampleReader = static_cast<SampleReader&>(*reader);
		if (sampleReader.currentCluster != nullptr) {
			removeReasonFromCluster(*sampleReader.currentCluster, "E030");
		}
	}

	if (error != Error::NONE) {
audioFileError:
		audioFile->~AudioFile(); // Have to call this! This removes the pointers back to the Sample / SampleClusters
		                         // from any Clusters.
		delugeDealloc(audioFileMemory);
		return std::unexpected(error);
	}

	try {
		if (audioFile->type == AudioFileType::SAMPLE) {
			sampleFiles[&audioFile->filePath] = static_cast<Sample*>(audioFile);
		}
		else if (audioFile->type == AudioFileType::WAVETABLE) {
			wavetableFiles[&audioFile->filePath] = static_cast<WaveTable*>(audioFile);
		}

	} catch (deluge::exception e) {
		if (e == deluge::exception::BAD_ALLOC) {
			audioFile->~AudioFile(); // Have to call this! This removes the pointers back to the Sample / SampleClusters
			                         // from any Clusters.
			delugeDealloc(audioFileMemory);
			return std::unexpected(Error::INSUFFICIENT_RAM);
		}
		freezeWithError("EXAM");
	}

	audioFile->finalizeAfterLoad(effectiveFilePointer.objsize);

	audioFile->removeReason("E399");

	return audioFile;
}

#define REPORT_LOAD_TIME 0

bool AudioFileManager::loadCluster(Cluster& cluster, int32_t minNumReasonsAfter) {

	if (currentlyAccessingCard) {
		return false; // Could happen if we're trying to render a waveform but we're actually already inside the SD
		              // routine
	}

	// I don't think these should happen...
	if (clusterBeingLoaded != nullptr) {
		return false;
	}

	if (AudioEngine::audioRoutineLocked) {
		return false;
	}

	clusterBeingLoaded = &cluster;
	minNumReasonsForClusterBeingLoaded = minNumReasonsAfter + 1;

	Sample* sample = cluster.sample;

	if (cluster.type != Cluster::Type::SAMPLE) {
		FREEZE_WITH_ERROR("E205"); // Chris F got this, so gonna leave checking in release build
	}

#if ALPHA_OR_BETA_VERSION
	if (cluster.numReasonsToBeLoaded <= 0) {
		// Ok, I think we know there's at least 1 reason at the point this function's called, because
		FREEZE_WITH_ERROR("E204");
	}
	// it'd only be in the loading queue if it had a "reason".
	if (!sample) {
		FREEZE_WITH_ERROR("E206");
	}
#endif

	// So that it can't accidentally hit 0 reasons while we're loading it,
	// cos then it might get deallocated.
	cluster.addReason();

	if (false) {
getOutEarly:
		clusterBeingLoaded = nullptr;
		removeReasonFromCluster(cluster, "E033");
		return false;
	}

	int32_t clusterIndex = cluster.clusterIndex;

	int32_t numSectors = Cluster::size >> 9;

	// If this is the last Cluster, and we do know what the audio data length is...
	if (sample->audioDataLengthBytes && sample->audioDataLengthBytes != 0x8FFFFFFFFFFFFFFF) {
		uint32_t audioDataEndPosBytes = sample->audioDataLengthBytes + sample->audioDataStartPosBytes;
		uint32_t startByteThisCluster = clusterIndex << Cluster::size_magnitude;
		int32_t bytesToRead = audioDataEndPosBytes - startByteThisCluster;
		if (bytesToRead <= 0) {
			D_PRINTLN("fail thing"); // Shouldn't really still happen
			goto getOutEarly;
		}
		if (bytesToRead < Cluster::size) {
			numSectors = ((bytesToRead - 1) >> 9) + 1;
		}
		// Otherwise, just leave it at the normal number of sectors
	}

#if ALPHA_OR_BETA_VERSION
	if ((uint32_t)cluster.data & 0b11) {
		D_PRINTLN("SD read address misaligned by  %d", (int32_t)((uint32_t)cluster.data & 0b11));
	}
#endif

	AudioEngine::logAction("loadCluster");

#if REPORT_LOAD_TIME
	uint16_t startTime = MTU2.TCNT_0;
#endif

#if ALPHA_OR_BETA_VERSION
	if (cluster.type != Cluster::Type::SAMPLE) {
		FREEZE_WITH_ERROR("i023"); // Happened to me while thrash testing with reduced RAM
	}

	if (cluster.numReasonsToBeLoaded < minNumReasonsAfter + 1) {
		FREEZE_WITH_ERROR("i039"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	DRESULT result = disk_read_without_streaming_first(
	    SD_PORT, (BYTE*)cluster.data, sample->clusters.getElement(cluster.clusterIndex)->sdAddress, numSectors);

#if REPORT_LOAD_TIME
	uint16_t endTime = MTU2.TCNT_0;
	uint16_t duration = endTime - startTime;
	int32_t uSec = timerCountToUS(duration);
	if (uSec > 7000) {
		D_PRINTLN(uSec);
	}
#endif

#if ALPHA_OR_BETA_VERSION
	if (cluster.type != Cluster::Type::SAMPLE) {
		FREEZE_WITH_ERROR("E207");
	}
	if (cluster.sample == nullptr) {
		FREEZE_WITH_ERROR("E208");
	}

	if (cluster.numReasonsToBeLoaded < minNumReasonsAfter + 1) {
		FREEZE_WITH_ERROR("i038"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	// If that failed, get out
	if (result != 0u) {
		goto getOutEarly;
	}

	cluster.convertDataIfNecessary();

#if ALPHA_OR_BETA_VERSION
	if (cluster.numReasonsToBeLoaded < minNumReasonsAfter + 1) {
		FREEZE_WITH_ERROR("i040"); // It's +1 because we haven't removed this function's "reason" yet.
	}
#endif

	int32_t misalignment = sample->audioDataStartPosBytes & 0b11;

	// Give extra bytes to previous Cluster
	if (clusterIndex > 0) {
		Cluster* prevCluster = sample->clusters.getElement(cluster.clusterIndex - 1)->cluster;

		if (prevCluster && prevCluster->loaded) {

			// We first copy our first 7 bytes from here to the end of the prev Cluster...
			memcpy(&prevCluster->data[Cluster::size], cluster.data, 7);

			// If 24-bit wrong-endian data...
			if (sample->rawDataFormat == RawDataFormat::ENDIANNESS_WRONG_24) {

				// If we hadn't previously written the "extra" bytes to the end of the prev Cluster and converted
				// them, do so now...
				if (!prevCluster->extraBytesAtEndConverted) {

					uint32_t bytesBeforeStartOfCluster = clusterIndex * Cluster::size - sample->audioDataStartPosBytes;
					int32_t bytesUnconvertedBeforeCluster = bytesBeforeStartOfCluster % 3;
					if (bytesUnconvertedBeforeCluster) {

						// There'll be one word in there which hasn't yet been converted. Do it now. (We've probably
						// just copied over the next one and a bit, which already was converted)
						int32_t startPos = Cluster::size - bytesUnconvertedBeforeCluster;
						uint8_t* thisNumber = (uint8_t*)&prevCluster->data[startPos];

						uint8_t temp = thisNumber[0];
						thisNumber[0] = thisNumber[2];
						thisNumber[2] = temp;

						// And now, copy 2 bytes back to this Cluster (that's the maximum that the float could have
						// been overhanging the boundary)
						memcpy(cluster.data, &prevCluster->data[Cluster::size], 2);
					}

					prevCluster->extraBytesAtEndConverted = true;
				}
			}

			// Or, all other types of raw data conversion
			else if (sample->rawDataFormat != RawDataFormat::NATIVE) {

				// If we haven't previously written the "extra" bytes to the end of the prev Cluster and converted
				// them, do so now...
				if (!prevCluster->extraBytesAtEndConverted) {

					// If misaligned from the 4-byte boundary
					if (misalignment) {

						// There'll be one word in there which hasn't yet been converted. Do it now. (We've probably
						// also just moved over the next one too, which already was converted)
						int32_t startPos = Cluster::size - 4 + misalignment;
						auto& thisNumber = reinterpret_cast<int32_t&>(prevCluster->data[startPos]);
						thisNumber = sample->convertToNative(thisNumber);

						// And now, copy 3 bytes back to this Cluster (that's the maximum that the float could have
						// been overhanging the boundary)
						memcpy(cluster.data, &prevCluster->data[Cluster::size], 3);
					}

					prevCluster->extraBytesAtEndConverted = true;
				}
			}

			cluster.extraBytesAtStartConverted = true;
		}
	}

	// Grab extra bytes from next Cluster
	if (clusterIndex < sample->clusters.getNumElements() - 1) {
		Cluster* nextCluster = sample->clusters.getElement(cluster.clusterIndex + 1)->cluster;

		if (nextCluster && nextCluster->loaded) {

			// If 24-bit wrong-endian data...
			if (sample->rawDataFormat == RawDataFormat::ENDIANNESS_WRONG_24) {

				uint32_t bytesBeforeStartOfNextCluster =
				    (clusterIndex + 1) * Cluster::size - sample->audioDataStartPosBytes;
				int32_t bytesUnconvertedBeforeNextCluster = bytesBeforeStartOfNextCluster % 3;

				// If one word missed conversion...
				if (bytesUnconvertedBeforeNextCluster) {

					// If we had't previously converted the first couple of bytes of the next Cluster...
					if (!nextCluster->extraBytesAtStartConverted) {

						// We first copy the next Cluster first 7 bytes to the end of this Cluster
						memcpy(&cluster.data[Cluster::size], nextCluster->data, 7);
					}

					// Or, if we *had* previously converted the first bytes of the next Cluster...
					else {

						// Grab the unconverted bytes back from where we backed them up to
						memcpy(&cluster.data[Cluster::size], nextCluster->firstThreeBytesPreDataConversion, 2);
					}

					// There'll be one word in there which hasn't yet been converted. Do it now. (We've probably
					// just copied over the next one and a bit, which already was converted)
					uint8_t* thisNumber = (uint8_t*)&cluster.data[Cluster::size - bytesUnconvertedBeforeNextCluster];

					uint8_t temp = thisNumber[0];
					thisNumber[0] = thisNumber[2];
					thisNumber[2] = temp;

					// If we had't previously converted the first couple of bytes of the next Cluster, do so now...
					if (!nextCluster->extraBytesAtStartConverted) {
						nextCluster->extraBytesAtStartConverted = true;

						// And now, copy 2 bytes back to the next Cluster (that's the maximum that the 24-bit
						// int32_t could have been overhanging the boundary)
						memcpy(nextCluster->data, &cluster.data[Cluster::size], 2);
					}

					// Or, if we *had* previously converted the first bytes of the next Cluster...
					else {
						goto copy7ToMe;
					}
				}

				// Or if no words missed conversion
				else {
					goto copy7ToMe;
				}
			}

			// Or, all other types of raw data conversion
			else if (sample->rawDataFormat != RawDataFormat::NATIVE) {

				// If one word missed conversion...
				if (misalignment) {
					int32_t startPos = Cluster::size - 4 + misalignment;
					auto& thisNumber = reinterpret_cast<int32_t&>(cluster.data[startPos]);

					// If we had't previously converted the first couple of bytes of the next Cluster, do so now...
					if (!nextCluster->extraBytesAtStartConverted) {

						// We first copy the next Cluster first 7 bytes to the end of this Cluster
						memcpy(&cluster.data[Cluster::size], nextCluster->data, 7);

						// There'll be one word in there which hasn't yet been converted from float. Do it now
						thisNumber = sample->convertToNative(thisNumber);

						// And now, copy 3 bytes back to the next Cluster (that's the maximum that the float could
						// have been overhanging the boundary)
						memcpy(nextCluster->data, &cluster.data[Cluster::size], 3);

						nextCluster->extraBytesAtStartConverted = true;
					}

					// Or, if we *had* previously converted the first bytes of the next Cluster...
					else {

						// Grab the unconverted bytes back from where we backed them up to
						memcpy(&cluster.data[Cluster::size], nextCluster->firstThreeBytesPreDataConversion, 3);

						// There'll be one word in there which hasn't yet been converted from float. Do it now
						thisNumber = sample->convertToNative(thisNumber);

						// And now just copy the converted-from-float first bytes from the next Cluster to the end
						// of this one
						goto copy7ToMe;
					}
				}
				else {
					goto copy7ToMe;
				}
			}

			else {
copy7ToMe:
				// We copy the next Cluster's first 7 bytes to the end of this Cluster
				memcpy(&cluster.data[Cluster::size], nextCluster->data, 7);
			}

			cluster.extraBytesAtEndConverted = true;
		}
	}

	cluster.loaded = true;

	clusterBeingLoaded = nullptr;
	removeReasonFromCluster(cluster, "E034");

#if ALPHA_OR_BETA_VERSION
	if (cluster.numReasonsToBeLoaded < minNumReasonsAfter) {
		FREEZE_WITH_ERROR("i037");
	}
	if (cluster.sample->clusters.getElement(cluster.clusterIndex)->cluster != &cluster) {
		FREEZE_WITH_ERROR("E438");
	}
#endif

	return true;
}

// Only needs calling a couple times per second. Must be called outside of the audio / SD-reading routine
// Call this repeatedly so SD card is re-initialized on re-insert before we actually urgently need audio from it
void AudioFileManager::slowRoutine() {

	// If we know the card's been ejected...
	if (cardEjected && !sdRoutineLock) {
		Error error = StorageManager::initSD();
		if (error == Error::NONE) {
			cardEjected = false;
		}
	}

	// NOTE: (Kate) There was dead code here referencing things that no longer
	// exist (NUM_LOADED_SAMPLE_CHUNK_ALLOCATION_QUEUES, availableClusterQueues)
	// It has been removed.
	// see
	// https://github.com/SynthstromAudible/DelugeFirmware/blob/866a71d0394e259a5b3db9d4fde605511bd1c67d/src/deluge/storage/audio/audio_file_manager.cpp#L1238
	// for a copy if ever needed
}

#define REPORT_AWAY_TIME 0

#if REPORT_AWAY_TIME
uint16_t timeLastFinish;
#endif

void AudioFileManager::loadAnyEnqueuedClusters(int32_t maxNum, bool mayProcessUserActionsBetween) {

	if (currentlyAccessingCard) {
		return;
	}
	if (clusterBeingLoaded) {
		return; // One might be having stuff done to it, like having its data converted, but not actually reading
		        // the card right now
	}
	if (AudioEngine::audioRoutineLocked) {
		return; // Not sure if this should be neccesary?
	}

	// Cannot call any functions in here which will read the SD card, other than loadCluster(), otherwise that'll
	// re-call this function!

	if (cardEjected || cardDisabled) {

performActionsAndGetOut:
		if (mayProcessUserActionsBetween) {
			playbackHandler.slowRoutine();
		}
		return;
	}

	if (!StorageManager::checkSDInitialized()) {
		goto performActionsAndGetOut; // In case the card somehow died
	}

	int32_t count = 0;

#if REPORT_AWAY_TIME
	uint16_t startTime = MTU2.TCNT_0;
	uint16_t awayTime = startTime - timeLastFinish;
	int32_t uSecAway = timerCountToUS(awayTime);
	if (uSecAway > 1000) {
		D_PRINTLN("away  %d", uSecAway);
	}
#endif

	while (true) {

		// We now have an opportunity, since we're not reading the card, to process any pending user actions like
		// undo / redo.
		if (mayProcessUserActionsBetween) {
			playbackHandler.slowRoutine();
		}

		// pop clusters until we get one that has reasonsToBeLoaded
		// this prevents loading clusters that have been quickly culled after they were enqueued
		Cluster* cluster = nullptr;
		while (!loadingQueue.empty()) {
			cluster = &loadingQueue.front();
			loadingQueue.pop();
			if (cluster->numReasonsToBeLoaded > 0) {
				break;
			}
			cluster->destroy();
		}

		// no more clusters to load, so exit
		if (cluster == nullptr) {
			return;
		}

		// cluster has at least 1 "reason". If it didn't, it would have been removed from the load-queue

		// Do the actual loading
		if (cluster->type != Cluster::Type::SAMPLE) {
			FREEZE_WITH_ERROR("E235"); // Cos Chris F got an E205
		}

		allowSomeUserActionsEvenWhenInCardRoutine = true; // Sorry!!
		bool success = loadCluster(*cluster);
		allowSomeUserActionsEvenWhenInCardRoutine = false;

		// If that didn't work, presumably because the SD card got ejected...
		if (!success) {
			D_PRINTLN("load Cluster fail");

			// If the Cluster is now down to 0 reasons (i.e. it lost a reason while being loaded), then it's already
			// been made "available" and we don't have a problem
			if (!cluster->numReasonsToBeLoaded) {}

			// Otherwise, there are still "reasons" waiting for this Cluster to become loaded, so we need to put it
			// back in the loading queue. Presumably it won't actually get loaded for a while - only when the user
			// re-inserts the card
			else {

				if (cluster->type != Cluster::Type::SAMPLE) {
					FREEZE_WITH_ERROR("E237"); // Cos Chris F got an E205
				}

				// TODO: If that fails, it'll just get awkwardly forgotten about
				loadingQueue.push(*cluster, 0xFFFFFFFF); // lowest priority

				// Also, return now. Normally we stay here til there's nothing left in the load-queue, but now that
				// would leave us in an infinite loop!
				break;
			}
		}

		count++;
		if (count >= maxNum) {
			break; // Keep things sane?
		}
	}

#if REPORT_AWAY_TIME
	timeLastFinish = MTU2.TCNT_0;
#endif
}

void AudioFileManager::removeReasonFromCluster(Cluster& cluster, char const* errorCode, bool deletingSong) {
	cluster.numReasonsToBeLoaded--;

	if (&cluster == clusterBeingLoaded && cluster.numReasonsToBeLoaded < minNumReasonsForClusterBeingLoaded) {
		FREEZE_WITH_ERROR("E041"); // Sven got this!
	}

	// If it's now zero, it's become available
	if (cluster.numReasonsToBeLoaded == 0) {

		// Bug hunting
		if (ALPHA_OR_BETA_VERSION && cluster.numReasonsHeldBySampleRecorder) {
			FREEZE_WITH_ERROR("E364");
		}

		// If it's still in the load queue, remove it from there. (We know that it isn't in the process of being
		// loaded right now because that would have added a "reason", so we wouldn't be here.) also do this on song
		// swap
		if (loadingQueue.erase(cluster) || deletingSong) {

			// Tell its Cluster to forget it exists
			cluster.sample->clusters.getElement(cluster.clusterIndex)->cluster = nullptr;

			delete &cluster; // It contains nothing, so completely recycle it
		}

		else {
			// It contains data we may want at some future point, so file it away
			GeneralMemoryAllocator::get().putStealableInAppropriateQueue(&cluster);
		}

		//*cluster->getAnyReasonsPointer() = ANY_REASONS_NO;
	}

	else if (cluster.numReasonsToBeLoaded < 0) {
#if ALPHA_OR_BETA_VERSION
		if (cluster.sample != nullptr) { // "Should" always be true...
			D_PRINTLN("reason remains on cluster of sample:  %d", cluster.sample->filePath.get());
		}
		FREEZE_WITH_ERROR(errorCode);
#else
		display->displayPopup(errorCode);  // For non testers, just display the error code without freezing
		cluster->numReasonsToBeLoaded = 0; // Save it from crashing or anything
#endif
	}
}

bool AudioFileManager::loadingQueueHasAnyLowestPriorityElements() {
	return loadingQueue.hasAnyLowestPriority();
}

// Caller must also set alternateAudioFileLoadPath.
void AudioFileManager::thingBeginningLoading(ThingType newThingType) {
	alternateLoadDirStatus = AlternateLoadDirStatus::MIGHT_EXIST;
	thingTypeBeingLoaded = newThingType;
}

void AudioFileManager::thingFinishedLoading() {
	alternateAudioFileLoadPath.clear();
	alternateLoadDirStatus = AlternateLoadDirStatus::NONE_SET;
	thingTypeBeingLoaded = ThingType::NONE;
}
