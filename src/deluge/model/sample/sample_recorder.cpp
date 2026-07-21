/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
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

#include "model/sample/sample_recorder.h"
#include "definitions.h"
#include "definitions_cxx.hpp"
#include "gui/ui/browser/sample_browser.h"
#include "gui/ui/root_ui.h"
#include "gui/ui_timer_manager.h"
#include "io/file.hpp"
#include "io/stream.hpp"
#include "libdeluge/control_surface.h"
#include "libdeluge/file_io.h"
#include "libdeluge/stream_io.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include "model/sample/sample.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/stem_export/stem_export.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "util/exceptions.h"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include <algorithm>
#include <new>

#include "deluge_resource.h" // mark_dirty: hold recording clusters un-evictable until flushed

extern "C" {
#include "fatfs/diskio.h"
}

#define MAX_FILE_SIZE_MAGNITUDE 32

SampleRecorder::~SampleRecorder() {
	D_PRINTLN("~SampleRecorder()");
	if (sample != nullptr) {
		detachSample();
	}
	// Only SPECIFIC_OUTPUT recorders track a source Output; MIX/OFFLINE_OUTPUT stem
	// recorders leave outputRecordingFrom null. The deref was benign on the MCU (addr 0
	// mapped) but faults on the host.
	if (outputRecordingFrom != nullptr) {
		outputRecordingFrom->removeRecorder();
	}
}

// This can be called when this SampleRecorder is destructed routinely - or earlier if we've aborted and the sample file
// is being deleted IMPORTANT!!!! You have to set sample to NULL after calling this, if not destructing
void SampleRecorder::detachSample() {

	// If we were holding onto the reasons for the first couple of Clusters, release them now
	if (keepingReasonsForFirstClusters) {
		int32_t numClustersToRemoveFor =
		    std::min(kNumClustersLoadedAhead, static_cast<int32_t>(sample->stream().num_clusters()));
		numClustersToRemoveFor = std::min(numClustersToRemoveFor, firstUnwrittenClusterIndex);

		for (int32_t l = 0; l < numClustersToRemoveFor; l++) {
			StreamedChunk* cluster = sample->stream().chunk_at(l);

			// Some bug-hunting
			if (!cluster->num_reasons_held_by_sample_recorder) {
				FREEZE_WITH_ERROR("E345");
			}
			cluster->num_reasons_held_by_sample_recorder--;

			deluge::cluster::remove_reason(*cluster, "E257");
		}
	}

	int32_t removeForClustersUntilIndex = currentRecordClusterIndex;
	if (currentRecordCluster) {
		removeForClustersUntilIndex++; // If there's a currentRecordCluster (usually will be if aborting), need to
		                               // remove its "reason" too
	}

	while (firstUnwrittenClusterIndex < removeForClustersUntilIndex) {
		StreamedChunk* cluster = sample->stream().chunk_at(firstUnwrittenClusterIndex);

		if (!cluster) {
			FREEZE_WITH_ERROR("E363");
		}

		// Some bug-hunting
		if (!cluster->num_reasons_held_by_sample_recorder) {
			FREEZE_WITH_ERROR("E346");
		}
		cluster->num_reasons_held_by_sample_recorder--;

		deluge::cluster::remove_reason(*cluster, "E249");
		firstUnwrittenClusterIndex++;
	}

	sample->removeReason("E400");
}

// config stuff
Error SampleRecorder::setup(int32_t newNumChannels, AudioInputChannel newMode, bool newKeepingReasons,
                            bool shouldRecordExtraMargins, AudioRecordingFolder newFolderID, int32_t buttonPressLatency,
                            Output* outputRecordingFrom_, RecorderConfig config) {

	outputRecordingFrom = outputRecordingFrom_;
	keepingReasonsForFirstClusters = newKeepingReasons;
	recordingExtraMargins = shouldRecordExtraMargins;
	folderID = newFolderID;

	// Didn't seem to make a difference forcing this into local RAM
	void* sample_memory = deluge::memory::alloc_external(sizeof(Sample), 16);
	if (sample_memory == nullptr) {
		return Error::INSUFFICIENT_RAM;
	}

	sample = new (sample_memory) Sample;
	audioFileManager.adoptAudioFileObject(sample); // resource-manager evictable object (before addReason)
	sample->addReason(); // Must call this so it's protected from stealing, before we call initialize().
	Error error = sample->initialize(1);
	if (error != Error::NONE) {
gotError:
		audioFileManager.destroyAudioFileObject(*sample); // ~Sample + free (routed through the manager if adopted)
		return error;
	}

	currentRecordCluster = sample->stream().get_cluster(0, CLUSTER_DONT_LOAD); // Adds a "reason" to it, too
	if (!currentRecordCluster) {
		error = Error::INSUFFICIENT_RAM;
		goto gotError;
	}

	// Bug hunting - newly gotten Cluster
	if (currentRecordCluster->num_reasons_held_by_sample_recorder) {
		FREEZE_WITH_ERROR("E360");
	}
	currentRecordCluster->num_reasons_held_by_sample_recorder++;

	// Give the sample some stuff
	sample->audioDataStartPosBytes = recordingExtraMargins ? 112 : 44;
	sample->byteDepth = 3;
	sample->numChannels = newNumChannels;
	sample->lengthInSamples = 0x8FFFFFFFFFFFFFFF;
	sample->audioDataLengthBytes = 0x8FFFFFFFFFFFFFFF; // If you ever change this value, update the check for it in
	                                                   // SampleStream::read_cluster_data()
	sample->sampleRate = kSampleRate;
	sample->workOutBitMask();

	currentRecordCluster->loaded =
	    true; // I think this is ok - mark it as loaded even though we're yet to record into it

	pointerHeldElsewhere = true;
	mode = newMode;
	currentRecordClusterIndex = 0;

	numSamplesToRunBeforeBeginningCapturing = numSamplesExtraToCaptureAtEndSyncingWise =
	    (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION) ? kAudioRecordLagCompensation : 0;

	// Apart from the MIX option, all other audio sources are fed to us during the "outputting" routine. Occasionally,
	// there'll be some more of that going to happen for the previous render, so we have to compensate for that
	if (mode != AudioInputChannel::MIX) {
		numSamplesToRunBeforeBeginningCapturing += AudioEngine::getNumSamplesLeftToOutputFromPreviousRender();
	}

	// External sources
	if (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION) {

		sourcePos = (int32_t*)AudioEngine::inputRingPos;

		numSamplesToRunBeforeBeginningCapturing -=
		    buttonPressLatency; // Compensate for button press latency. We only do this for external sources

		// If doing extra margins...
		if (recordingExtraMargins) {

			// Everything will be fine, so long as the button press latency we compensated for isn't, like, as big as
			// the RX buffer
			sample->fileLoopStartSamples =
			    SSI_RX_BUFFER_NUM_SAMPLES - (SSI_TX_BUFFER_NUM_SAMPLES << 1) + numSamplesToRunBeforeBeginningCapturing;
			numSamplesToRunBeforeBeginningCapturing = 0;

			sourcePos += (SSI_TX_BUFFER_NUM_SAMPLES
			              << (NUM_MONO_INPUT_CHANNELS_MAGNITUDE + 1)); // I think the +1 was just because it needs to
			                                                           // move two tx buffers' length for some reason...
			if (sourcePos >= AudioEngine::inputRingEnd()) {
				sourcePos -= SSI_RX_BUFFER_NUM_SAMPLES << NUM_MONO_INPUT_CHANNELS_MAGNITUDE;
			}
		}

		// Or if not doing extra margins...
		else {

			// If the button press latency we're compensating for is more than the audio latency, we have to adjust
			// stuff, to grab some audio from just back in time
			if (numSamplesToRunBeforeBeginningCapturing < 0) {

				sourcePos +=
				    numSamplesToRunBeforeBeginningCapturing * NUM_MONO_INPUT_CHANNELS; // This might be negative!
				if (sourcePos < AudioEngine::inputRingStart()) {
					sourcePos += SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS;
				}

				numSamplesToRunBeforeBeginningCapturing = 0;
			}
		}
	}

	// Set some other stuff up

	recordPeakL = recordPeakR = recordPeakLMinusR = 0;
	recordingClippedRecently = false;

	recordSumL = 0;
	recordSumR = 0;
	recordSumLPlusR = 0;
	recordSumLMinusR = 0;

	recordMax = -2147483648;
	recordMin = 2147483647;

	writePos = reinterpret_cast<char*>(currentRecordCluster->payload().data());
	clusterEndPos = reinterpret_cast<char*>(currentRecordCluster->payload().data() + Cluster::size);

	numSamplesBeenRunning = 0;
	numSamplesCaptured = 0;

	capturedTooMuch = false;

	recordingNumChannels = newNumChannels;
	int32_t byteDepth = 3;
	int32_t lengthSec =
	    5; // Mark it as 5 seconds long initially. We'll update that later when we know how long it actually is
	int32_t lengthSamples = lengthSec * sample->sampleRate;
	audioDataLengthBytesAsWrittenToFile = lengthSamples * 3 * recordingNumChannels;

	setRecordingThreshold(config);

	// Riff chunk -------------------------------------------------------
	writeInt32(&writePos, 0x46464952);                                                               // "RIFF"
	writeInt32(&writePos, audioDataLengthBytesAsWrittenToFile + sample->audioDataStartPosBytes - 8); // Chunk size
	writeInt32(&writePos, 0x45564157);                                                               // "WAVE"

	// Format chunk --------------------------------------------------------
	writeInt32(&writePos, 0x20746d66);                                            // "fmt "
	writeInt32(&writePos, 16);                                                    // Chunk size
	writeInt16(&writePos, 0x0001);                                                // Format - PCM
	writeInt16(&writePos, recordingNumChannels);                                  // Num channels
	writeInt32(&writePos, sample->sampleRate);                                    // Sample rate
	writeInt32(&writePos, sample->sampleRate * recordingNumChannels * byteDepth); // Data rate
	writeInt16(&writePos, recordingNumChannels * byteDepth);                      // Data block size
	writeInt16(&writePos, byteDepth * 8);                                         // Bits per sample

	if (recordingExtraMargins) {

		loopEndSampleAsWrittenToFile = lengthSamples;

		// Sample chunk ------------------------------------------------------
		writeInt32(&writePos, 0x6c706d73); // "smpl"
		writeInt32(&writePos, 60);         // Chunk size
		writeInt32(&writePos, 0);          // Manufacturer - 0 means none
		writeInt32(&writePos, 0);          // Product - 0 means none
		writeInt32(&writePos, (1000000000 + (sample->sampleRate >> 1)) / sample->sampleRate); // Nanoseconds per sample
		writeInt32(&writePos, 0); // MIDI note - 0 conventionally seems to mean none
		writeInt32(&writePos, 0); // MIDI pitch fraction
		writeInt32(&writePos, 0); // SMPTE format - 0 means none / no offset
		writeInt32(&writePos, 0); // SMPTE offset
		writeInt32(&writePos, 1); // Number of loops
		writeInt32(&writePos, 0); // Number of additional sampler data bytes

		// Loop definition ----------------------------------------------------
		writeInt32(&writePos, 0);                            // Cue point ID
		writeInt32(&writePos, 0);                            // Type - 0 means loop forward
		writeInt32(&writePos, sample->fileLoopStartSamples); // Start point
		writeInt32(&writePos, loopEndSampleAsWrittenToFile); // End point
		writeInt32(&writePos, 0);                            // Loop point sample fraction
		writeInt32(&writePos, 0);                            // Play count - 0 means continuous
	}

	// Data chunk ------------------------------------------------------
	writeInt32(&writePos, 0x61746164);                          // "data"
	writeInt32(&writePos, audioDataLengthBytesAsWrittenToFile); // Chunk size

	return Error::NONE;
}

void SampleRecorder::setRecordingThreshold(RecorderConfig config) {
	if (config.neverUseThreshold) {
		D_PRINTLN("ignoring threshold");
	}
	else {
		D_PRINTLN("following settings");
	}
	// don't use threshold recording if we're resampling internal input
	if (config.neverUseThreshold || currentSong->thresholdRecordingMode == ThresholdRecordingMode::OFF
	    || mode >= AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION) {
		startValueThreshold = 0;
		thresholdRecording = false;
		minThresholdMargin = 0;
	}
	else {
		// max possible sample value = 1 << kBitDepth = 16777216

		// these thresholds were determined through iterative testing to find
		// the right sample value to start recording audio input based on different input
		// levels

		switch (currentSong->thresholdRecordingMode) {
		// used for input sources with low input volume
		case ThresholdRecordingMode::LOW:
			startValueThreshold = 11.5;
			break;

		// used for input sources with medium-high input volume
		case ThresholdRecordingMode::MEDIUM:
			startValueThreshold = 14.5;
			break;

		// used for input sources with high input volume (good for microphones with gain)
		case ThresholdRecordingMode::HIGH:
			startValueThreshold = 17.5;
			break;

		default:
			break;
		}

		thresholdRecording = true;
		minThresholdMargin = std::min<uint32_t>(sample->fileLoopStartSamples, 256);
	}
}

// Beware! This could get called during card routine - e.g. if user stopped playback. So we'll just store a changed
// status, then do the descrutcion and file deletion when we know we're out of the card routine. Also, this gets called
// in audio routine! So don't do anything drastic.
void SampleRecorder::abort() {
	status = RecorderStatus::ABORTED; // Note: it may already equal this!
}

// Returns error if one occurred just now - not if one was already noted before
Error SampleRecorder::cardRoutine() {

	// If aborted, delete the file.
	if (status == RecorderStatus::ABORTED) {

aborted:
		if (sample != nullptr) { // This might get called multiple times, so check we haven't already detached it.

			// Note: if this abort() is due to a song-swap (loading a different song),
			// then samples is about to be searched for temp ones to delete, and we'll need to have deleted it here
			// before that trips over us. Previously caused an E281. So, for that to happen,
			// SampleManager::deleteAnyTempRecordedSamplesFromMemory() (indirectly) calls us here first.

			detachSample(); // Does not set sample to NULL - we do that below

#if ALPHA_OR_BETA_VERSION
			// It should be impossible that anyone else still holds a "reason" to this Sample, as we can only be
			// "aborted" before AudioClip::finishLinearRecording() is called, and it's only then at the AudioClip
			// becomes a "reason".
			if (sample->isProjectReferenced()) {
				FREEZE_WITH_ERROR("E282");
			}
#endif

			if (haveAddedSampleToArray) { // We only add it to the array when the file is created.
				audioFileManager.deleteUnusedAudioFileFromMemoryIndexUnknown(*sample);
			}

			sample = nullptr; // So we don't try to detach it again when we're destructed
		}

		// Delete the file if one was created
		if (!filePathCreated.empty()) {

			deluge_file_invalidate_cache();
			FRESULT result = f_unlink(filePathCreated.c_str());

			// If this was the most recent recording in this category, tick the counter backwards - so long as
			// either the delete was successful or it was for an AudioClip, which means the file is in the TEMP folder
			// and can be overwritten anyway
			if (result == FR_OK || folderID == AudioRecordingFolder::CLIPS) {
				if (audioFileManager.highestUsedAudioRecordingNumber[util::to_underlying(folderID)]
				    == audioFileNumber) {
					audioFileManager.highestUsedAudioRecordingNumber[util::to_underlying(folderID)]--;
					D_PRINTLN("ticked file counter backwards");
				}
			}
			filePathCreated.clear();
		}

		// Normally we now just await deletion - except if a pointer is still being held elsewhere - which I think can
		// only happen from AudioRecorder. Or if the abort comes from a failure within this class and the AudioClip
		// hasn't realised yet?
		if (!pointerHeldElsewhere) {
			status = RecorderStatus::AWAITING_DELETION;
		}
		return Error::NONE;
	}

	if (status >= RecorderStatus::COMPLETE) {
		return Error::NONE;
	}

	Error error = Error::NONE;

	if (!hadCardError) {

		// If file not created yet, do that
		if (filePathCreated.empty()) {

			error = StorageManager::initSD();
			if (error != Error::NONE) {
				goto gotError;
			}

			// Check there's space on the card
			error = StorageManager::checkSpaceOnCard();
			if (error != Error::NONE) {
				goto gotError;
			}

			std::string filePath;
			std::string tempFilePathForRecording;

			// Note: we couldn't pass the actual Sample pointer into this function, cos the Sample might get destructed
			// during the card access! (Though probably not anymore right?)
			// Recording could finish or abort during this!
			if (stemExport.processStarted) {
				error = stemExport.getUnusedStemRecordingFilePath(&filePath, folderID);
			}
			else {
				const char* name;
				if (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION && !AudioEngine::lineInPluggedIn) {
					if (AudioEngine::micPluggedIn) {
						name = "ExtMic";
					}
					else {
						name = "IntMic";
					}
				}
				else if (mode == AudioInputChannel::SPECIFIC_OUTPUT) {
					if (outputRecordingFrom) {
						name = outputRecordingFrom->name.c_str();
					}
				}
				else {
					name = inputChannelToString(mode);
				}
				error = audioFileManager.getUnusedAudioRecordingFilePath(filePath, &tempFilePathForRecording, folderID,
				                                                         &audioFileNumber, name, &currentSong->name);
			}
			if (status == RecorderStatus::ABORTED) {
				goto aborted; // In case aborted during
			}
			if (error != Error::NONE) {
				goto gotError;
			}

			bool mayOverwrite = true;

			// Now store our own copy of the actually (possibly temp) filename
			if (!tempFilePathForRecording.empty()) {
				filePathCreated = tempFilePathForRecording.c_str(); // Can't fail!
			}
			else {
				filePathCreated = filePath.c_str(); // Can't fail!
				mayOverwrite = false;
			}

			// Recording could finish or abort during this!
			DelugeStreamMode streamOpenMode =
			    mayOverwrite ? DELUGE_STREAM_WRITE_CREATE : DELUGE_STREAM_WRITE_CREATE_NEW;
			bool triedCreatingRecordingFolder = false;

tryOpenRecordingStream:
			// Bypasses the Tier 2 locator cache (same as the raw FatFS open createFileRaw used to do)
			// -- must invalidate it, or a stale cached locator for this exact path (from an earlier
			// directory listing) could survive a mayOverwrite create-always reopen and feed a later
			// cache-hit read the pre-overwrite file's stale sclust/objsize.
			deluge_file_invalidate_cache();
			auto openedStream = deluge::io::Stream::open(filePathCreated, streamOpenMode);
			if (!openedStream && openedStream.error() == deluge::io::Status::NOT_FOUND) {
				// deluge::io::Stream::open (unlike the old createFileRaw, and unlike the portable
				// deluge::io layer's createFile()) does no folder-creation retry of its own -- but the
				// containing folder legitimately might not exist yet (AudioClip's CLIPS/TEMP
				// subfolder, or a per-song recording subfolder under RECORD/RESAMPLE -- see
				// AudioFileManager::getUnusedAudioRecordingFilePath). Replicate createFileRaw's
				// create-parent-and-retry behaviour here, one folder level at a time.
				if (triedCreatingRecordingFolder) {
					filePathCreated.clear();
					goto gotError;
				}
				triedCreatingRecordingFolder = true;

				std::string folderPath = filePathCreated;

cutRecordingFolderPathAndTryCreating:
				size_t slashPos = folderPath.find_last_of('/');
				if (slashPos == std::string::npos) {
					filePathCreated.clear();
					goto gotError;
				}
				folderPath.resize(slashPos);

				auto madeDir = deluge::io::mkdir(folderPath.c_str());
				if (madeDir) {
					goto tryOpenRecordingStream;
				}
				else if (madeDir.error() == deluge::io::Status::NOT_FOUND) {
					triedCreatingRecordingFolder = false; // Let it try multiple levels again
					goto cutRecordingFolderPathAndTryCreating;
				}
				else {
					filePathCreated.clear();
					goto gotError;
				}
			}
			if (!openedStream) {
				filePathCreated.clear();
				goto gotError;
			}
			else {
				this->file = std::move(openedStream.value());
			}

			if (status == RecorderStatus::ABORTED) {
				goto aborted; // In case aborted during
			}

			// Ok, the Sample still exists.
			sample->filePath = filePath;                                 // Can't fail!
			sample->tempFilePathForRecording = tempFilePathForRecording; // Can't fail!

			auto inserted = audioFileManager.audioFiles.insertElement(sample);
			if (!inserted) {
				error = inserted.error();
				goto gotError;
			}

			haveAddedSampleToArray = true;
		}

		// Might want to write just one cluster
		if (firstUnwrittenClusterIndex < currentRecordClusterIndex) {
			error = writeOneCompletedCluster();

			if (error != Error::NONE) {
gotError:
				hadCardError = true;
			}

			else {
				// If more clusters still to write, come back later to do them
				if (true || firstUnwrittenClusterIndex < currentRecordClusterIndex) {
					goto allDoneForNow;
				}
			}
		}
	}

	// If we've actually finished recording...
	if (status == RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
		if (!hadCardError) {
			error = finalizeRecordedFile();
			if (error != Error::NONE) {
				hadCardError = true;
				error = Error::SD_CARD;
			}
		}

		if (reachedMaxFileSize) {
			if (autoDeleteWhenDone) {
				abort();
			}
			else {
				status = RecorderStatus::COMPLETE;
			}
			error = Error::MAX_FILE_SIZE_REACHED;
		}
		else {
			status = autoDeleteWhenDone ? RecorderStatus::AWAITING_DELETION : RecorderStatus::COMPLETE;
		}
	}

allDoneForNow:
	return error;
}

Error SampleRecorder::writeAnyCompletedClusters() {
	while (firstUnwrittenClusterIndex < currentRecordClusterIndex) {

		Error error = writeOneCompletedCluster();

		// If there was an error, we can only return now after removing that reason, because we'd already incremented
		// firstUnwrittenClusterIndex, and we can't leave that incremented without removing the reason
		if (error != Error::NONE) {
			return error;
		}
	}

	return Error::NONE;
}

Error SampleRecorder::writeOneCompletedCluster() {
	int32_t writingClusterIndex = firstUnwrittenClusterIndex;

#if ALPHA_OR_BETA_VERSION
	// Trying to pin down E347 which Leo got, below
	StreamedChunk* cluster = sample->stream().chunk_at(writingClusterIndex);
	if (!cluster->num_reasons_held_by_sample_recorder) {
		FREEZE_WITH_ERROR("E374");
	}
#endif

	firstUnwrittenClusterIndex++; // Have to increment this before writing, cos while writing, the audio routine will be
	                              // called, and we need to be counting this cluster as "written", as in too late for it
	                              // to be modified (by writing a final length to it)

	Error error = writeCluster(writingClusterIndex, Cluster::size);

	// We no longer have a reason to require this Cluster to be kept in memory
	if (!keepingReasonsForFirstClusters || writingClusterIndex >= kNumClustersLoadedAhead) {
		StreamedChunk* cluster = sample->stream().chunk_at(writingClusterIndex);

		// Some bug-hunting
		if (!cluster->num_reasons_held_by_sample_recorder) {
			// Leo got!!! And Vinz, and keyman. May be solved now that fixed so detachSample() doesn't get called during
			// card routine.
			FREEZE_WITH_ERROR("E347");
		}
		cluster->num_reasons_held_by_sample_recorder--;

		deluge::cluster::remove_reason(*cluster, "E015");
	}

	// If there was an error, we can only return now after removing that reason, because we'd already incremented
	// firstUnwrittenClusterIndex, and we can't leave that incremented without removing the reason
	return error;
}

Error SampleRecorder::finalizeRecordedFile() {

	if (ALPHA_OR_BETA_VERSION && (status == RecorderStatus::ABORTED || hadCardError)) {
		FREEZE_WITH_ERROR("E273");
	}

	D_PRINTLN("finalizing");

	// In the very rare case where we've already got between 1 and 5 bytes overhanging the end of our current cluster,
	// we need to allocate a new one right now
	int32_t bytesTilClusterEnd = clusterEndPos - writePos;
	if (bytesTilClusterEnd < 0) {
		Error error = createNextCluster();

		if (error == Error::MAX_FILE_SIZE_REACHED) {
		} // So incredibly unlikely. But no real problem - we maybe just lose a byte or two

		else if (error != Error::NONE) {
			return error;
		}
		else { // No error
			// Having just created a new cluster, there'll be one more completed one to write
			error = writeAnyCompletedClusters();
			if (error != Error::NONE) {
				return error;
			}
		}
	}

	// And we probably need to write some of the final cluster(s) to file. (If it's NULL, it means that it couldn't be
	// created, cos or RAM or file size limit.)
	if (currentRecordCluster) {

		int32_t bytesToWrite = writePos - reinterpret_cast<char*>(currentRecordCluster->payload().data());
		if (bytesToWrite > 0) { // Will always be true
			Error error = writeCluster(currentRecordClusterIndex, bytesToWrite);
			if (error != Error::NONE) {
				return error;
			}
		}

		firstUnwrittenClusterIndex++;

		// Having incremented firstUnwrittenClusterIndex, we need to remove the "reason" for that final cluster.
		// Normally that happens in writeAnyCompletedClusters(), but well this cluster wasn't "complete" so we're doing
		// the whole thing here instead
		if (!keepingReasonsForFirstClusters || currentRecordClusterIndex >= kNumClustersLoadedAhead) {

			// Some bug-hunting
			if (!currentRecordCluster->num_reasons_held_by_sample_recorder) {
				FREEZE_WITH_ERROR("E348");
			}
			currentRecordCluster->num_reasons_held_by_sample_recorder--;

			deluge::cluster::remove_reason(*currentRecordCluster, "E047");
		}
		currentRecordClusterIndex++;    // We've finished with that cluster
		currentRecordCluster = nullptr; // But currentRecordClusterIndex now refers to a cluster that'll never exist
	}

	uint32_t idealFileSizeBeforeAction = sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
	uint32_t dataLengthBeforeAction = sample->audioDataLengthBytes;

	// Figure out what processing needs to happen on the recorded audio
	MonitoringAction action = MonitoringAction::NONE;
	int32_t lshiftAmount = 0;

	if (allowFileAlterationAfter
	    && idealFileSizeBeforeAction <= 67108864) { // Arbitrarily, don't alter files bigger than 64MB
		if (recordingNumChannels == 1) {
			action = MonitoringAction::NONE;
		}
		else {
			// If R is really quiet or is nearly identical to L, delete R
			if (inputHasNoRightChannel() || recordSumLMinusR < (recordSumL >> 6)) {
				D_PRINTLN("removing right channel");
				action = MonitoringAction::REMOVE_RIGHT_CHANNEL;
			}

			// Or, if R is the differential signal of L, do that
			else if (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION && AudioEngine::lineInPluggedIn
			         && inputLooksDifferential()) {
				D_PRINTLN("subtracting right channel");
				action = MonitoringAction::SUBTRACT_RIGHT_CHANNEL;
			}

			else {
				D_PRINTLN("keeping right channel");
				action = MonitoringAction::NONE;
			}
		}

		uint32_t maxPeak;
		if (action == MonitoringAction::SUBTRACT_RIGHT_CHANNEL) {
			maxPeak = -1 - recordPeakLMinusR;
		}
		else {
			maxPeak = -1 - std::min(recordPeakL, recordPeakR);
		}

		if (allowNormalization) {
			for (lshiftAmount = 0; ((uint32_t)2147483648 >> (lshiftAmount + 1)) > maxPeak; lshiftAmount++) {}
		}
	}
	uint32_t dataLengthAfterAction =
	    action != MonitoringAction::NONE ? (dataLengthBeforeAction >> 1) : dataLengthBeforeAction;

	// TODO: in a perfect world, where we're not deleting a channel, we'd go backwards from the last Cluster, because
	// that's the most likely to still be in memory

	// If some processing of the recorded audio data needs to happen...
	if (lshiftAmount || action != MonitoringAction::NONE) {

		auto closeResult = this->file->close();
		this->file.reset();
		if (!closeResult) {
			return Error::SD_CARD;
		}

		Error error = alterFile(action, lshiftAmount, idealFileSizeBeforeAction, dataLengthAfterAction);
		if (error != Error::NONE) {
			return error;
		}
	}

	// Or if no action or shifting was required...
	else {

		// If we made the file too long, because we then compensated for button latency and are throwing away the last
		// little bit, then truncate it
		if (capturedTooMuch) {
			D_PRINTLN("truncating");
			uint32_t correctLength =
			    sample->audioDataStartPosBytes
			    + sample->audioDataLengthBytes; // These were written to in totalSampleLengthNowKnown().
			Error error = truncateFileDownToSize(correctLength);
		}

		auto closeResult = this->file->close();
		this->file.reset();
		if (!closeResult) {
			return Error::SD_CARD;
		}

		// If the actual audio data length we ended up with is not the same as was written in the headers in the first
		// cluster (very likely; various reasons)
		if (sample->audioDataLengthBytes != audioDataLengthBytesAsWrittenToFile
		    || (recordingExtraMargins && sample->fileLoopEndSamples != loopEndSampleAsWrittenToFile)) {

			// Update data length as written in first cluster
			SampleCluster& firstSampleCluster = sample->stream().entry(0);
			StreamedChunk* cluster =
			    sample->stream().get_cluster(0, CLUSTER_LOAD_IMMEDIATELY); // Remember, this adds a "reason"
			if (cluster) {

				// Bug hunting - newly gotten Cluster
				cluster->num_reasons_held_by_sample_recorder++;

				// Do a last-ditch check that the SD address doesn't look invalid
				if (firstSampleCluster.sdAddress == 0) {
					FREEZE_WITH_ERROR("E268");
				}
				if ((firstSampleCluster.sdAddress - fileSystem.database) & (fileSystem.csize - 1)) {
					FREEZE_WITH_ERROR("E269");
				}

				audioDataLengthBytesAsWrittenToFile = sample->audioDataLengthBytes;
				loopEndSampleAsWrittenToFile = sample->fileLoopEndSamples;
				updateDataLengthInFirstCluster(cluster);

				// Write just that one first sector back to the card
				disk_write(0, (BYTE*)cluster->payload().data(), firstSampleCluster.sdAddress, 1);

				// If that failed, well, that's a shame, but we don't need to do anything

				// Some bug-hunting
				if (!cluster->num_reasons_held_by_sample_recorder) {
					FREEZE_WITH_ERROR("E349");
				}
				cluster->num_reasons_held_by_sample_recorder--;

				deluge::cluster::remove_reason(*cluster, "E026");
			}
		}
	}

	sample->numChannels = (action != MonitoringAction::NONE || recordingNumChannels == 1) ? 1 : 2;
	sample->lengthInSamples = dataLengthAfterAction / (sample->byteDepth * sample->numChannels);
	sample->audioDataLengthBytes =
	    sample->lengthInSamples
	    * (sample->byteDepth
	       * sample->numChannels); // Ensure whole number of samples (surely it already would be though?)

	if (sample->tempFilePathForRecording.empty()) {
		sampleBrowser.lastFilePathLoaded = sample->filePath;
	}

	return Error::NONE;
}

void SampleRecorder::updateDataLengthInFirstCluster(StreamedChunk* cluster) {
	uint32_t data32;

	// Write top-level RIFF chunk size
	*(uint32_t*)(cluster->payload().data() + 4) =
	    audioDataLengthBytesAsWrittenToFile + sample->audioDataStartPosBytes - 8;

	// Write data chunk size
	*(uint32_t*)(cluster->payload().data() + (sample->audioDataStartPosBytes - 4)) =
	    audioDataLengthBytesAsWrittenToFile;

	if (recordingExtraMargins) {
		// Write loop end point
		*(uint32_t*)(cluster->payload().data() + 92) = loopEndSampleAsWrittenToFile;
	}
}

extern int32_t pendingGlobalMIDICommandNumClustersWritten;

// You'll want to remove the "reason" after calling this
Error SampleRecorder::writeCluster(int32_t clusterIndex, size_t numBytes) {
	// D_PRINTLN("writeCluster");

	SampleCluster* sampleCluster = &sample->stream().entry(clusterIndex);

	uint32_t byteOffset = static_cast<uint32_t>(clusterIndex) << Cluster::size_magnitude;
	auto writeResult =
	    file->write_at(byteOffset, std::span<const std::byte>(sampleCluster->cluster->payload().data(), numBytes));
	if (!writeResult || *writeResult != numBytes) {
		return Error::SD_CARD;
	}

	// MUST re-get this - while writing above, the audio routine is being called, and that could
	// allocate new SampleClusters and move them around!
	sampleCluster = &sample->stream().entry(clusterIndex);

	// Grab the SD address, for later
	uint32_t sector = 0;
	auto sectorResult = file->sector_of(static_cast<uint32_t>(clusterIndex));
	if (sectorResult) {
		sector = *sectorResult;
	}
	sampleCluster->sdAddress = sector;

	// Now flushed to the card with its sdAddress recorded, this cluster is reconstructable like any
	// sample cluster (materialize re-reads it) — so clear dirty, letting the manager evict + reload
	// it under pressure. (Until now it was held dirty so the unflushed audio could not be evicted.)
	if (sampleCluster->cluster != nullptr) {
		DelugeResource* mgr = GeneralMemoryAllocator::get().resourceManager();
		if (mgr != nullptr) {
			deluge_resource_mark_dirty(mgr, sampleCluster->cluster, false);
		}
	}
	return Error::NONE;
}

Error SampleRecorder::createNextCluster() {

	StreamedChunk* oldRecordCluster =
	    currentRecordCluster; // Cos we're gonna set that to NULL just below here, but still
	                          // want to be able to access the old one a bit further down

	currentRecordClusterIndex++; // Mark record-cluster we were on as finished

	currentRecordCluster = nullptr; // Note that we haven't yet created our next record-cluster - we'll do that below
	                                // if no error first; and if there is an error and we don't create one, this has to
	                                // remain NULL to indicate that we never created one

	// If this new cluster would actually put us past the 4GB limit...
	if (currentRecordClusterIndex >= (1 << (MAX_FILE_SIZE_MAGNITUDE - Cluster::size_magnitude))) {

		// See if we actually already had any bytes to write into that new cluster we can't have...
		int32_t bytesTilClusterEnd = clusterEndPos - writePos;
		if (bytesTilClusterEnd < 0) {
			numSamplesCaptured--;
			writePos -= (int32_t)recordingNumChannels * 3;
		}

		totalSampleLengthNowKnown(numSamplesCaptured, numSamplesCaptured);

		reachedMaxFileSize = true;
		return Error::MAX_FILE_SIZE_REACHED;
	}

	// We need to allocate our next Cluster
	try {
		sample->stream().resize(sample->stream().num_clusters() + 1);
	} catch (deluge::exception&) {
		return Error::INSUFFICIENT_RAM;
	}

	currentRecordCluster = sample->stream().get_cluster(currentRecordClusterIndex, CLUSTER_DONT_LOAD);

	// If couldn't allocate cluster (would normally only happen if no SD card present so recording only to RAM)
	if (!currentRecordCluster) {
		D_PRINTLN("SampleRecorder::createNextCluster() fail");
		return Error::INSUFFICIENT_RAM;
	}

	// Bug hunting - newly gotten Cluster
	if (currentRecordCluster->num_reasons_held_by_sample_recorder) {
		FREEZE_WITH_ERROR("E362");
	}
	currentRecordCluster->num_reasons_held_by_sample_recorder++;

	// Copy those extra bytes from the end of the old record cluster to the start of the new cluster
	memcpy(currentRecordCluster->payload().data(), oldRecordCluster->payload().data() + Cluster::size,
	       5); // 5 is the max number of bytes we could have overshot

	int32_t bytesOvershot = writePos - clusterEndPos;

	currentRecordCluster->loaded =
	    true; // I think this is ok - mark it as loaded even though we're yet to record into it

	writePos = (char*)(currentRecordCluster->payload().data() + bytesOvershot);
	clusterEndPos = (char*)(currentRecordCluster->payload().data() + Cluster::size);

	return Error::NONE;
}

// Gets called when we've captured all the samples of audio that we wanted - either as a direct result of user
// action, or after being fed a few more samples to make up for latency.
void SampleRecorder::finishCapturing() {
	status = RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING;

	// A freshly recorded sample now has a final length and needs its waveform overview pre-scanned. Re-arm
	// the background scan, which may have gone idle after all previously-loaded samples were scanned (#4460).
	// (The scan skips clusters the recorder is still writing and retries them once its reasons are released.)
	audioFileManager.overviewScanAllDone = false;

	if (getRootUI()) {
		getRootUI()->sampleNeedsReRendering(sample);
	}
	if (outputRecordingFrom) {
		outputRecordingFrom->removeRecorder();
	}
}

// Only call this after checking that status < RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING
// Watch out - this could be called during SD writing - including during cardRoutine() for this class!
void SampleRecorder::feedAudio(std::span<StereoSample> input, bool applyGain, uint8_t gainToApply) {
	int32_t numSamples = input.size();
	do {
		int32_t numSamplesThisCycle = input.size();

		// If haven't actually started recording yet cos we're compensating for lag...
		if (numSamplesBeenRunning < (uint32_t)numSamplesToRunBeforeBeginningCapturing) {
			int32_t numSamplesTilBeginRecording = numSamplesToRunBeforeBeginningCapturing - numSamplesBeenRunning;
			numSamplesThisCycle = std::min(numSamplesThisCycle, numSamplesTilBeginRecording);
		}

		// Or, if properly recording...
		else {
			int32_t samplesLeft;

			if (status == RecorderStatus::CAPTURING_DATA_WAITING_TO_STOP) {

				samplesLeft = sample->lengthInSamples - numSamplesCaptured;
				if (samplesLeft <= 0) {
doFinishCapturing:
					finishCapturing();
					return;
				}

				numSamplesThisCycle = std::min(numSamplesThisCycle, samplesLeft);
			}
			if (ALPHA_OR_BETA_VERSION && numSamplesThisCycle <= 0) {
				FREEZE_WITH_ERROR("bbbb");
			}

			int32_t bytesPerSample = recordingNumChannels * 3;
			int32_t bytesWeWantToWrite = numSamplesThisCycle * bytesPerSample;

			int32_t bytesTilClusterEnd = clusterEndPos - writePos;

			// If we need a new cluster right now...
			if (bytesTilClusterEnd <= 0) {
				Error error = createNextCluster();
				if (error == Error::MAX_FILE_SIZE_REACHED) {
					goto doFinishCapturing;
				}
				else if (error != Error::NONE) { // RAM error
					D_PRINTLN("couldn't allocate RAM");
					abort();
					return;
				}

				bytesTilClusterEnd = clusterEndPos - writePos; // Recalculate it
			}

			if (bytesTilClusterEnd <= bytesWeWantToWrite - bytesPerSample) {
				int32_t samplesTilClusterEnd =
				    (static_cast<uint16_t>(bytesTilClusterEnd - 1) / static_cast<uint8_t>(bytesPerSample))
				    + 1; // Rounds up
				numSamplesThisCycle = std::min(numSamplesThisCycle, samplesTilClusterEnd);
			}

			if (ALPHA_OR_BETA_VERSION && numSamplesThisCycle <= 0) {
				FREEZE_WITH_ERROR("aaaa");
			}

			std::byte* __restrict__ writePosNow = reinterpret_cast<std::byte*>(writePos);
			// Balanced input. For this, we skip a bunch of stat-grabbing, cos we know this is just for AudioClips.
			// We also know that applyGain is false - that's just for the MIX option
			if (mode == AudioInputChannel::BALANCED) {
				for (StereoSample sample : input.first(numSamplesThisCycle)) {
					q31_t rxBalanced = (sample.l / 2) - (sample.r / 2);

					// Copy the last 24 bits (lower 3 bytes) to writePosNow
					writePosNow = std::copy_n(&reinterpret_cast<std::byte*>(&rxBalanced)[1], 3, writePosNow);
				}
			}

			// Or, all other, non-balanced input types
			else {
				for (auto [rxL, rxR] : input.first(numSamplesThisCycle)) {
					if (applyGain) {
						rxL = lshiftAndSaturateUnknown(rxL, gainToApply);
					}

					// Copy the last 24 bits (lower 3 bytes) to writePosNow
					writePosNow = std::copy_n(&reinterpret_cast<std::byte*>(&rxL)[1], 3, writePosNow);

					recordMax = std::max(rxL, recordMax);
					recordMin = std::min(rxL, recordMin);

					q31_t absL = (rxL >= 0) ? rxL : -1 - rxL;
					recordSumL += absL;

					if (rxL < recordPeakL) {
						recordPeakL = rxL;
					}
					else if (-rxL < recordPeakL) {
						recordPeakL = -rxL;
					}
					if (rxL == std::numeric_limits<q31_t>::max() || rxL == std::numeric_limits<q31_t>::min()) {
						recordingClippedRecently = true;
					}

					// recording stereo
					if (recordingNumChannels == 2) {
						if (applyGain) {
							rxR = lshiftAndSaturateUnknown(rxR, gainToApply);
						}

						// Copy the last 24 bits (lower 3 bytes) to writePosNow
						writePosNow = std::copy_n(&reinterpret_cast<std::byte*>(&rxR)[1], 3, writePosNow);

						recordMax = std::max(rxR, recordMax);
						recordMin = std::min(rxR, recordMin);

						recordSumR += (rxR >= 0) ? rxR : -1 - rxR;

						int32_t lPlusR = (rxL >> 1) + (rxR >> 1);
						recordSumLPlusR += (lPlusR >= 0) ? lPlusR : -1 - lPlusR;

						int32_t lMinusR = (rxL >> 1) - (rxR >> 1);
						recordSumLMinusR += (lMinusR >= 0) ? lMinusR : -1 - lMinusR;

						if (rxR < recordPeakR) {
							recordPeakR = rxR;
						}
						else if (-rxR < recordPeakR) {
							recordPeakR = -rxR;
						}
						if (rxR == std::numeric_limits<q31_t>::max() || rxR == std::numeric_limits<q31_t>::min()) {
							recordingClippedRecently = true;
						}

						if (lMinusR < recordPeakLMinusR) {
							recordPeakLMinusR = lMinusR;
						}
						else if (-lMinusR < recordPeakLMinusR) {
							recordPeakLMinusR = -lMinusR;
						}
					}
				}
			}
			writePos = reinterpret_cast<char*>(writePosNow);
			numSamplesCaptured += numSamplesThisCycle;

			// if we're threshold recording and didn't detect audio in previous cycles
			// check if there's any audio in this cycle
			if (thresholdRecording) [[unlikely]] {
				size_t samples_to_copy = minThresholdMargin;
				StereoFloatSample approxRMSLevel = envelopeFollower.calcApproxRMS(input);
				if (std::max(approxRMSLevel.l, approxRMSLevel.r) > startValueThreshold) {
					thresholdRecording = false;
					samples_to_copy += numSamplesThisCycle;
				}

				// is this overkill? yes, we could  let the cluster fill up before doing the copy
				// is there a downside? no - threshold recording only occurs when a single audio clip is being recorded
				// or the standalone sampler is being used so whatever this is simple
				if (numSamplesCaptured > minThresholdMargin) {
					auto* endpos = (char*)writePosNow;
					ptrdiff_t num_bytes = samples_to_copy * 3 * recordingNumChannels;
					char* audio_start_pos = endpos - num_bytes;
					char* cluster_start_pos = reinterpret_cast<char*>(currentRecordCluster->payload().data()
					                                                  + sample->audioDataStartPosBytes);
					if (audio_start_pos > cluster_start_pos) {
						memcpy(cluster_start_pos, audio_start_pos, num_bytes);
						writePos = cluster_start_pos + num_bytes;
						numSamplesCaptured = samples_to_copy;
					}
				}
			}

			// update our input view to exclude the chunk we just processed
			input = input.subspan(numSamplesThisCycle);
		}

		numSamplesBeenRunning += numSamplesThisCycle;

		numSamples -= numSamplesThisCycle;
	} while (numSamples > 0);
}

void SampleRecorder::endSyncedRecording(int32_t buttonLatencyForTempolessRecording) {
#if ALPHA_OR_BETA_VERSION
	if (status == RecorderStatus::CAPTURING_DATA_WAITING_TO_STOP) {
		FREEZE_WITH_ERROR("E272");
	}
	else if (status == RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
		FREEZE_WITH_ERROR("E288");
	}
	else if (status == RecorderStatus::COMPLETE) {
		FREEZE_WITH_ERROR("E289");
	}
	else if (status == RecorderStatus::ABORTED) {
		FREEZE_WITH_ERROR("E290");
	}
	else if (status == RecorderStatus::AWAITING_DELETION) {
		FREEZE_WITH_ERROR("E291");
	}
#endif

	if (numSamplesCaptured) {
		int32_t numMoreSamplesTilEndLoopPoint =
		    numSamplesExtraToCaptureAtEndSyncingWise - buttonLatencyForTempolessRecording;
		int32_t numMoreSamplesToCapture = numMoreSamplesTilEndLoopPoint;

		D_PRINTLN("buttonLatencyForTempolessRecording:  %d", buttonLatencyForTempolessRecording);

		if (recordingExtraMargins) {
			numMoreSamplesToCapture += kAudioClipMarginSizePostEnd; // Means we also have an audioClip
		}

		uint32_t loopEndPointSamples = numSamplesCaptured + numMoreSamplesTilEndLoopPoint;

		totalSampleLengthNowKnown(numSamplesCaptured + numMoreSamplesToCapture, loopEndPointSamples);

		if (numMoreSamplesToCapture <= 0) {
			if (numMoreSamplesToCapture < 0) {
				capturedTooMuch = true;
				D_PRINTLN("captured too much.");
			}
			finishCapturing();
		}
		else {
			status = RecorderStatus::CAPTURING_DATA_WAITING_TO_STOP;
		}
	}
	else {
		// if we haven't captured any samples (which can happen with threshold recording), abort
		abort();
	}
}

void SampleRecorder::totalSampleLengthNowKnown(uint32_t totalLengthSamples, uint32_t loopEndPointSamples) {

	sample->lengthInSamples = totalLengthSamples;
	sample->audioDataLengthBytes = totalLengthSamples * sample->byteDepth * sample->numChannels;
	// when stem recording is done, we want to update the sample loop end position in the sample holder
	// so that that loop end marker is available right away if you want to load that sample into a kit
	if (stemExport.writeLoopEndPos()) {
		sample->fileLoopEndSamples = stemExport.loopEndPointInSamplesForAudioFile;
	}
	else {
		sample->fileLoopEndSamples = loopEndPointSamples;
	}

	// If we haven't written the first cluster yet, quick - update it with the actual length
	if (firstUnwrittenClusterIndex == 0) {
		StreamedChunk* cluster =
		    sample->stream().chunk_at(0); // It should still be there, cos it hasn't been written to card yet
		if (ALPHA_OR_BETA_VERSION && !cluster) {
			FREEZE_WITH_ERROR("E274");
		}

		audioDataLengthBytesAsWrittenToFile = sample->audioDataLengthBytes;
		loopEndSampleAsWrittenToFile = sample->fileLoopEndSamples; // Even if we're not actually writing loop points
		                                                           // to the file, this is harmless
		updateDataLengthInFirstCluster(cluster);
	}
}

bool SampleRecorder::inputLooksDifferential() {
	return (recordSumLPlusR < (recordSumL >> 4));
}

bool SampleRecorder::inputHasNoRightChannel() {
	return (recordSumR < (recordSumL >> 6));
}

// Only call this if currentRecordCluster points to a real cluster
void SampleRecorder::setExtraBytesOnPreviousCluster(StreamedChunk* currentCluster, int32_t currentClusterIndex) {
	if (currentClusterIndex <= 0) {
		return;
	}

	StreamedChunk* prevCluster = sample->stream().chunk_at(currentClusterIndex - 1);

	// It might have since been deallocated, which is just fine. But if not...
	if (prevCluster) {
		memcpy(prevCluster->payload().data() + Cluster::size, currentCluster->payload().data(), 5);
	}
}

Error SampleRecorder::alterFile(MonitoringAction action, int32_t lshiftAmount, uint32_t idealFileSizeBeforeAction,
                                uint64_t dataLengthAfterAction) {

	D_PRINTLN("altering file");
	int32_t currentReadClusterIndex = 0;
	int32_t currentWriteClusterIndex = 0;

	StreamedChunk* currentReadCluster =
	    sample->stream().get_cluster(0, CLUSTER_LOAD_IMMEDIATELY); // Remember, this adds a "reason"
	if (!currentReadCluster) {
		return Error::SD_CARD;
	}

	// Bug hunting - newly gotten Cluster
	currentReadCluster->num_reasons_held_by_sample_recorder++;

	int32_t numClustersBeforeAction = ((idealFileSizeBeforeAction - 1) >> Cluster::size_magnitude) + 1; // Rounds up
	if (ALPHA_OR_BETA_VERSION && numClustersBeforeAction > static_cast<int32_t>(sample->stream().num_clusters())) {
		FREEZE_WITH_ERROR("E286");
	}

	StreamedChunk* nextReadCluster = nullptr;

	if (numClustersBeforeAction >= 2) {
		nextReadCluster = sample->stream().get_cluster(1, CLUSTER_LOAD_IMMEDIATELY); // Remember, this adds a "reason"
		if (!nextReadCluster) {

			// Some bug-hunting
			if (!currentReadCluster->num_reasons_held_by_sample_recorder) {
				FREEZE_WITH_ERROR("E350");
			}
			currentReadCluster->num_reasons_held_by_sample_recorder--;

			deluge::cluster::remove_reason(*currentReadCluster, "E017");
			return Error::SD_CARD;
		}

		// Bug hunting - newly gotten Cluster
		nextReadCluster->num_reasons_held_by_sample_recorder++;
	}

	StreamedChunk* currentWriteCluster =
	    sample->stream().get_cluster(0, CLUSTER_DONT_LOAD); // Remember, this adds a "reason"
	// That one can't fail, fortunately, cos we already grabbed Cluster 0 above, so it exists

	// Bug hunting - newly gotten Cluster
	currentWriteCluster->num_reasons_held_by_sample_recorder++;

	uint32_t data32;
	uint16_t data16;

	audioDataLengthBytesAsWrittenToFile = dataLengthAfterAction;
	loopEndSampleAsWrittenToFile = sample->fileLoopEndSamples;
	updateDataLengthInFirstCluster(currentWriteCluster);

	if (action != MonitoringAction::NONE) {
		// Write num channels
		data16 = 1;
		memcpy(currentWriteCluster->payload().data() + 22, &data16, 2);

		// Data rate
		data32 = kSampleRate * 1 * 3;
		memcpy(currentWriteCluster->payload().data() + 28, &data32, 4);

		// Data block size
		data16 = 1 * 3;
		memcpy(currentWriteCluster->payload().data() + 32, &data16, 2);
	}

	char* readPos = reinterpret_cast<char*>(currentReadCluster->payload().data() + sample->audioDataStartPosBytes);
	char* writePos = reinterpret_cast<char*>(currentWriteCluster->payload().data() + sample->audioDataStartPosBytes);

	uint32_t bytesFinalCluster = idealFileSizeBeforeAction & (Cluster::size - 1);
	if (bytesFinalCluster == 0) {
		bytesFinalCluster = Cluster::size;
	}

	uint32_t count = 0;

	// TODO: this is really inefficient - checks a bunch of stuff for every single audio sample. Should check in
	// advance how many samples we can process at a time

	while (true) {

		if (!(count & 0b11111111)) { // 10x 1's seems to work ok. So we go down to 8 to be sure
			AudioEngine::routineWithClusterLoading();

			uiTimerManager.routine();

			deluge_control_flush();
		}

		count++;

		int32_t* input = (int32_t*)(readPos - 1);
		readPos += 3;
		int32_t value = *input & 0xFFFFFF00;

		if (action == MonitoringAction::SUBTRACT_RIGHT_CHANNEL) {
			input = (int32_t*)(readPos - 1);
			readPos += 3;
			value = (value >> 1) - ((int32_t)(*input & 0xFFFFFF00) >> 1);
		}

		else if (action == MonitoringAction::REMOVE_RIGHT_CHANNEL) {
			readPos += 3;
		}
		int32_t processed = value << lshiftAmount;

		char* processedPos = (char*)&processed + 1;
		*(writePos++) = *(processedPos++);
		*(writePos++) = *(processedPos++);
		*(writePos++) = *(processedPos++);

		// If need to advance write-head past the end of a cluster, then we'll write that current cluster to disk
		// and carry on
		int32_t writeOvershot =
		    writePos - reinterpret_cast<char*>(currentWriteCluster->payload().data() + Cluster::size);
		if (writeOvershot >= 0) {

			// If reached very end of file, break
			if (currentWriteClusterIndex == numClustersBeforeAction - 1) {
				break;
			}

			D_PRINTLN("write advance");

			currentWriteCluster->loaded = true; // I don't think this is necessary anymore

			uint32_t sdAddress = sample->stream().sd_address_at(currentWriteClusterIndex);

			// Do a last-ditch check that the SD address doesn't look invalid
			if (sdAddress == 0) {
				FREEZE_WITH_ERROR("E268");
			}
			if ((sdAddress - fileSystem.database) & (fileSystem.csize - 1)) {
				FREEZE_WITH_ERROR("E275");
			}

			// Write the Cluster we just finished processing to card
			DRESULT result = disk_write(0, (BYTE*)currentWriteCluster->payload().data(), sdAddress, Cluster::size >> 9);

			// Grab any overshot / extra bytes from the end of the Cluster we just finished...
			uint8_t extraBytes[5]; // 5 is the max number of bytes we could have overshot
			if (writeOvershot) {
				memcpy(extraBytes, currentWriteCluster->payload().data() + Cluster::size, writeOvershot);
			}

			// And from the Cluster we just finished, give the Cluster *before that* the extra bytes from its start
			setExtraBytesOnPreviousCluster(currentWriteCluster, currentWriteClusterIndex);

			// We don't need that old Cluster anymore

			// Some bug-hunting
			if (!currentWriteCluster->num_reasons_held_by_sample_recorder) {
				FREEZE_WITH_ERROR("E351");
			}
			currentWriteCluster->num_reasons_held_by_sample_recorder--;

			deluge::cluster::remove_reason(*currentWriteCluster, "E023");
			currentWriteCluster = nullptr;

			// If write operation failed, now's the time to get out
			if (result) {
writeFailed:
				// Before we get out, remove "reasons" from the clusters we've been reading from

				// Some bug-hunting
				if (!currentReadCluster->num_reasons_held_by_sample_recorder) {
					FREEZE_WITH_ERROR("E352");
				}
				currentReadCluster->num_reasons_held_by_sample_recorder--;

				deluge::cluster::remove_reason(*currentReadCluster, "E024");

				if (nextReadCluster) {
					// Some bug-hunting
					if (!nextReadCluster->num_reasons_held_by_sample_recorder) {
						FREEZE_WITH_ERROR("E353");
					}
					nextReadCluster->num_reasons_held_by_sample_recorder--;

					deluge::cluster::remove_reason(*nextReadCluster, "E025");
				}
				return Error::SD_CARD;
			}

			// Ok, move on and start thinking about the next Cluster now
			currentWriteClusterIndex++;

			// Get the new / next Cluster, but don't insist on actually reading from the card, cos we're gonna
			// overwrite it with new data anyway
			currentWriteCluster = sample->stream().get_cluster(currentWriteClusterIndex,
			                                                   CLUSTER_DONT_LOAD); // Remember, this adds a "reason"

			// That could only fail if no RAM, but juuuust in case...
			if (!currentWriteCluster) {
				goto writeFailed;
			}

			// Bug hunting - newly gotten Cluster
			currentWriteCluster->num_reasons_held_by_sample_recorder++;

			// Ok, and those extra bytes that we grabbed from the end of the previous Cluster - paste them into the
			// beginning of the new current Cluster
			if (writeOvershot) {
				memcpy(currentWriteCluster->payload().data(), extraBytes, writeOvershot);
			}

			// And get ready to write to the new current Cluster - from the next sample, which might not be
			// perfectly aligned to the Cluster start
			writePos = reinterpret_cast<char*>(currentWriteCluster->payload().data() + writeOvershot);
		}

		// If we're in the final read-Cluster and reached the end, then all that's left to do is flush out what we
		// have left to write (max 1 cluster), and get out.
		if (currentReadClusterIndex == numClustersBeforeAction - 1
		    && readPos >= reinterpret_cast<char*>(currentReadCluster->payload().data() + bytesFinalCluster)) {
			break;
		}

		// Advance read-head. We read one Cluster ahead, so we can access its "extra bytes"
		if (readPos >= reinterpret_cast<char*>(currentReadCluster->payload().data() + Cluster::size)) {

			D_PRINTLN("read advance");

			int32_t overshot = readPos - reinterpret_cast<char*>(currentReadCluster->payload().data() + Cluster::size);

			// Some bug-hunting
			if (!currentReadCluster->num_reasons_held_by_sample_recorder) {
				FREEZE_WITH_ERROR("E354");
			}
			currentReadCluster->num_reasons_held_by_sample_recorder--;

			deluge::cluster::remove_reason(*currentReadCluster, "E020");
			currentReadClusterIndex++;
			currentReadCluster = nextReadCluster;

			// If there are further read Clusters...
			if (currentReadClusterIndex < numClustersBeforeAction - 1) {
				nextReadCluster = sample->stream().get_cluster(
				    currentReadClusterIndex + 1, CLUSTER_LOAD_IMMEDIATELY); // Remember, this adds a "reason"

				// If that failed, remove other reasons and get out
				if (!nextReadCluster) {

					// Some bug-hunting
					if (!currentReadCluster->num_reasons_held_by_sample_recorder) {
						FREEZE_WITH_ERROR("E355");
					}
					currentReadCluster->num_reasons_held_by_sample_recorder--;

					deluge::cluster::remove_reason(*currentReadCluster, "E021");

					// Some bug-hunting
					if (!currentWriteCluster->num_reasons_held_by_sample_recorder) {
						FREEZE_WITH_ERROR("E356");
					}
					currentWriteCluster->num_reasons_held_by_sample_recorder--;

					deluge::cluster::remove_reason(*currentWriteCluster, "E022");
					currentWriteCluster = nullptr;
					return Error::SD_CARD;
				}

				// Bug hunting - newly gotten Cluster
				nextReadCluster->num_reasons_held_by_sample_recorder++;
			}
			else { // Not sure these are strictly necessary...
				nextReadCluster = nullptr;
			}

			readPos = reinterpret_cast<char*>(currentReadCluster->payload().data() + overshot);
		}
	}

	// We got to the end, so wrap everything up

	// Some bug-hunting
	if (!currentReadCluster->num_reasons_held_by_sample_recorder) {
		FREEZE_WITH_ERROR("E357");
	}
	currentReadCluster->num_reasons_held_by_sample_recorder--;

	deluge::cluster::remove_reason(*currentReadCluster, "E018");
	// We know that finishedAlteringFile must be NULL

	currentWriteCluster->loaded = true;

	uint32_t bytesToWriteFinalCluster = writePos - reinterpret_cast<char*>(currentWriteCluster->payload().data());

	if (bytesToWriteFinalCluster) { // If there is in fact anything to flush out to the file / card...

		// And from this final Cluster, give the Cluster *before that* the extra bytes from its start
		setExtraBytesOnPreviousCluster(currentWriteCluster, currentWriteClusterIndex);

		uint32_t numSectorsToWrite = ((bytesToWriteFinalCluster - 1) >> 9) + 1;
		if (numSectorsToWrite > (Cluster::size >> 9)) {
			FREEZE_WITH_ERROR("E239");
		}

		uint32_t sdAddress = sample->stream().sd_address_at(currentWriteClusterIndex);

		// Do a last-ditch check that the SD address doesn't look invalid
		if (sdAddress == 0) {
			FREEZE_WITH_ERROR("E268");
		}
		if ((sdAddress - fileSystem.database) & (fileSystem.csize - 1)) {
			FREEZE_WITH_ERROR("E276");
		}

		DRESULT result = disk_write(0, (BYTE*)currentWriteCluster->payload().data(), sdAddress, numSectorsToWrite);

		// Some bug-hunting
		if (!currentWriteCluster->num_reasons_held_by_sample_recorder) {
			FREEZE_WITH_ERROR("E358");
		}
		currentWriteCluster->num_reasons_held_by_sample_recorder--;

		deluge::cluster::remove_reason(*currentWriteCluster, "E019");
		currentWriteCluster = nullptr;

		// If writing disk failed, above, we've now removed that "reason", so we can get out
		if (result) {
			return Error::SD_CARD;
		}

		if (action != MonitoringAction::NONE || capturedTooMuch) {

			deluge_file_invalidate_cache();
			auto reopenedStream = deluge::io::Stream::open(sample->filePath, DELUGE_STREAM_WRITE_APPEND);
			if (!reopenedStream) {
				return Error::SD_CARD;
			}
			this->file = std::move(reopenedStream.value());

			Error error = truncateFileDownToSize(dataLengthAfterAction + sample->audioDataStartPosBytes);
			if (error != Error::NONE) {
				return error;
			}

			auto closeResult = this->file->close();
			this->file.reset();
			if (!closeResult) {
				return Error::SD_CARD;
			}
		}
	}
	else { // Or if there was nothing further to write (very rare)...

		// Some bug-hunting
		if (!currentWriteCluster->num_reasons_held_by_sample_recorder) {
			FREEZE_WITH_ERROR("E359");
		}
		currentWriteCluster->num_reasons_held_by_sample_recorder--;

		deluge::cluster::remove_reason(*currentWriteCluster, "E238");
		currentWriteCluster = nullptr;
	}

	return Error::NONE;
}

// You must still have the file open when you call this
Error SampleRecorder::truncateFileDownToSize(uint32_t newFileSize) {

	// Update the Sample object to indicate the correct size. Do this before we risk errors below

	uint64_t numClustersAfterAction = ((newFileSize - 1) >> Cluster::size_magnitude) + 1;

	if (numClustersAfterAction < sample->stream().num_clusters()) {
		sample->stream().erase_from(numClustersAfterAction);
	}

	auto truncateResult = file->truncate(newFileSize);
	if (!truncateResult) {
		return Error::SD_CARD;
	}

	return Error::NONE;
}
