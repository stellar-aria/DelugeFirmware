#include "storage/smsysex.h"
#include "fatfs/ff.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/oled.h"
#include "hid/hid_sysex.h"
#include "io/debug/log.h"
#include "io/debug/print.h"
#include "io/file.hpp"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "processing/engines/audio_engine.h"
#include "scheduler_api.h"
#include "storage/owner.h"
#include "sync/sd_access.h"
#include "util/containers.h"
#include "util/pack.h"
#include <cstring>

#define MAX_DIR_LINES 25

std::optional<deluge::io::Directory> sxDir;
uint32_t dirOffsetCounter;

JsonSerializer jWriter;
std::string activeDirName;
const size_t blockBufferMax = 1024;
const size_t sysexBufferMax = blockBufferMax + 256;
uint8_t* writeBlockBuffer = nullptr;
uint8_t* readBlockBuffer = nullptr;
const uint32_t MAX_OPEN_FILES = 4;

struct FILdata {
	std::string fName;
	uint32_t fileID;
	uint32_t LRUstamp = 0;
	uint32_t fSize = 0;
	uint32_t fPosition = 0; // file offset noted after last read/write operation.
	bool fileOpen = false;
	bool forWrite = false;
	std::optional<deluge::io::File> file;
};

int32_t FIDcounter = 1;
uint32_t LRUcounter = 1;

PLACE_SDRAM_BSS FILdata openFiles[MAX_OPEN_FILES];

namespace {

// Freezes the wire's "err" attribute at today's FRESULT-numeric values,
// independent of whatever backend implements file_io.h underneath — this is
// the permanent abstraction boundary between the companion protocol and
// deluge::io, not a migration shim. One accepted, documented collapse:
// Status::NOT_FOUND can't distinguish the original FR_NO_FILE from
// FR_NO_PATH (both already collapsed into DELUGE_ERR_NOT_FOUND at the
// boundary's original design); this always produces FR_NO_FILE.
// Status::UNSUPPORTED has no real FRESULT analog; FR_INVALID_PARAMETER is
// the closest fit.
FRESULT toWireFresult(deluge::io::Status status) {
	switch (status) {
	case deluge::io::Status::OK:
		return FRESULT::FR_OK;
	case deluge::io::Status::ERR:
	case deluge::io::Status::IO:
		return FRESULT::FR_DISK_ERR;
	case deluge::io::Status::PARAM:
	case deluge::io::Status::UNSUPPORTED:
		return FRESULT::FR_INVALID_PARAMETER;
	case deluge::io::Status::BUSY:
		return FRESULT::FR_LOCKED;
	case deluge::io::Status::TIMEOUT:
		return FRESULT::FR_TIMEOUT;
	case deluge::io::Status::NODEV:
		return FRESULT::FR_NOT_READY;
	case deluge::io::Status::NOT_FOUND:
		return FRESULT::FR_NO_FILE;
	case deluge::io::Status::EXISTS:
		return FRESULT::FR_EXIST;
	case deluge::io::Status::NO_SPACE:
		return FRESULT::FR_DENIED;
	case deluge::io::Status::NO_FILESYSTEM:
		return FRESULT::FR_NO_FILESYSTEM;
	case deluge::io::Status::WRITE_PROTECTED:
		return FRESULT::FR_WRITE_PROTECTED;
	case deluge::io::Status::NO_MEMORY:
		return FRESULT::FR_NOT_ENOUGH_CORE;
	}
	return FRESULT::FR_DISK_ERR; // unreachable while the switch above stays exhaustive
}

// The wire's "date"/"time" ints are already FAT DOS-packed WORDs (the
// companion app speaks FatFS's native format directly) -- this decodes them
// into the boundary's portable DelugeTimestamp. Returns nullopt when both
// are zero, matching every existing call site's "date != 0 || time != 0"
// gate for "no timestamp requested."
std::optional<DelugeTimestamp> timestampFromWireDateTime(uint32_t date, uint32_t time) {
	if (date == 0 && time == 0) {
		return std::nullopt;
	}
	DelugeTimestamp ts{};
	ts.year = static_cast<uint16_t>(1980 + ((date >> 9) & 0x7F));
	ts.month = static_cast<uint8_t>((date >> 5) & 0x0F);
	ts.day = static_cast<uint8_t>(date & 0x1F);
	ts.hour = static_cast<uint8_t>((time >> 11) & 0x1F);
	ts.minute = static_cast<uint8_t>((time >> 5) & 0x3F);
	ts.second = static_cast<uint8_t>((time & 0x1F) * 2);
	return ts;
}

// Inverse of timestampFromWireDateTime — packs a portable DelugeTimestamp
// back into the wire's FAT DOS-packed WORD shape for getDirEntries's reply.
WORD wireDateFromTimestamp(const DelugeTimestamp& ts) {
	return static_cast<WORD>(((ts.year - 1980) << 9) | (ts.month << 5) | ts.day);
}

WORD wireTimeFromTimestamp(const DelugeTimestamp& ts) {
	return static_cast<WORD>((ts.hour << 11) | (ts.minute << 5) | (ts.second / 2));
}

// Packs DelugeDirEntry's portable attribute flags back into the wire's raw
// FatFS fattrib byte shape.
BYTE wireAttribFromFlags(const DelugeDirEntry& entry) {
	BYTE attrib = 0;
	if (entry.is_read_only)
		attrib |= AM_RDO;
	if (entry.is_hidden)
		attrib |= AM_HID;
	if (entry.is_system)
		attrib |= AM_SYS;
	if (entry.is_directory)
		attrib |= AM_DIR;
	if (entry.is_archive)
		attrib |= AM_ARC;
	return attrib;
}

} // namespace

const int MaxSysExLength = 1024;

struct SysExDataEntry {
	MIDICable& cable;
	int32_t len;
	uint8_t data[sysexBufferMax]{};

	SysExDataEntry(MIDICable& forCable, int32_t newLen) : cable{forCable}, len{newLen} {}
};

deluge::deque<SysExDataEntry> SysExQ;

// The following constants assume that the messageID part ranges from 1 to SYSEX_MSGID_MAX
// and that SYSEX_MSGID_MAX is 1 less than a power of 2.
// It also assumes that MAX_SYSEX_SESSIONS is also 1 less than a power of 2.
const uint32_t MAX_SYSEX_SESSIONS = 15;
const uint32_t SYSEX_MSGID_MAX = 7;
const uint32_t SYSEX_MSGID_MASK = 0x07;
const uint32_t SYSEX_SESSION_MASK = 0x78;
const uint32_t SYSEX_SESSION_SHIFT = 3;

uint32_t session_mono_counter = 1;
uint32_t sessionLRU_array[MAX_SYSEX_SESSIONS + 1] = {0};

void smSysex::noteSessionIdUse(uint8_t msgId) {
	uint32_t sessionNum = (msgId & SYSEX_SESSION_MASK) >> SYSEX_SESSION_SHIFT;
	sessionLRU_array[sessionNum] = session_mono_counter++;
}

void smSysex::noteFileIdUse(FILdata* fp) {
	fp->LRUstamp = LRUcounter++;
}

// Returns the entry in the FILdata array for the given fid
FILdata* smSysex::entryForFID(uint32_t fid) {
	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (openFiles[i].fileID == fid) {
			return openFiles + i;
		}
	}
	return nullptr;
}

// Assign a FIL from our pool.
FILdata* smSysex::findEmptyFIL() {
	uint32_t LRUtime = 0xFFFFFFFF;
	uint32_t LRUindex = 0;

	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (!openFiles[i].fileOpen) {
			return openFiles + i;
		}
		else if (openFiles[i].LRUstamp < LRUtime) {
			LRUtime = openFiles[i].LRUstamp;
			LRUindex = i;
		}
	}
	// Close the abandoned file before we reuse the entry.
	FILdata* oldest = openFiles + LRUindex;
	closeFIL(oldest);
	return oldest;
}

void smSysex::startDirect(JsonSerializer& writer) {
	writer.reset();
	writer.setMemoryBased();
	uint8_t reply_hdr[7] = {0xf0, 0x00, 0x21, 0x7B, 0x01, SysEx::SysexCommands::Json, 0};
	writer.writeBlock(reply_hdr, sizeof(reply_hdr));
}

void smSysex::startReply(JsonSerializer& writer, JsonDeserializer& reader) {
	writer.reset();
	writer.setMemoryBased();
	uint8_t reply_hdr[7] = {0xf0, 0x00, 0x21, 0x7B, 0x01, SysEx::SysexCommands::JsonReply, reader.getReplySeqNum()};
	writer.writeBlock(reply_hdr, sizeof(reply_hdr));
}

void smSysex::sendMsg(MIDICable& cable, JsonSerializer& writer) {
	writer.writeByte(0xF7);

	char* bitz = writer.getBufferPtr();
	int32_t bw = writer.bytesWritten();
	cable.sendSysex((const uint8_t*)bitz, bw);
};

FILdata* smSysex::openFIL(const char* fPath, bool forWrite, FRESULT* eCode) {
	FILdata* fp = findEmptyFIL();
	fp->fName = fPath;
	fp->fileID = FIDcounter++;
	noteFileIdUse(fp);

	auto opened = deluge::io::File::open(fPath, forWrite ? DELUGE_FILE_WRITE_CREATE : DELUGE_FILE_READ);
	*eCode = toWireFresult(opened.has_value() ? deluge::io::Status::OK : opened.error());
	if (!opened.has_value()) {
		return nullptr;
	}
	fp->file = std::move(*opened);
	auto size = fp->file->size();
	fp->fSize = size.has_value() ? *size : 0;
	fp->fileOpen = true;
	fp->forWrite = forWrite;
	fp->fPosition = 0;
	return fp;
}

FRESULT smSysex::closeFIL(FILdata* fp) {
	if (fp == nullptr) {
		return FRESULT::FR_INVALID_OBJECT;
	}

	deluge::io::Status status = deluge::io::Status::OK;
	if (fp->file.has_value()) {
		auto result = fp->file->close();
		status = result.has_value() ? deluge::io::Status::OK : result.error();
		fp->file.reset();
	}
	fp->fileOpen = false;
	fp->forWrite = false;
	fp->fSize = 0;
	return toWireFresult(status);
}

// Fill in missing directories for the full path name given.
// Unless the last character in the path is a /, we assume the
// path given ends with a filename (which we ignore).
FRESULT smSysex::createPathDirectories(std::string& path, std::optional<DelugeTimestamp> timestamp) {
	if (path.size() > 256) {
		return FRESULT::FR_INVALID_PARAMETER;
	}

	char working[257];
	char pathPart[257];
	strcpy(working, path.c_str());
	int len = strlen(working);
	int lastSlash;
	for (lastSlash = len - 1; lastSlash >= 0; lastSlash--) {
		if (working[lastSlash] == '/')
			break;
	}
	if (lastSlash == 0) {
		return FRESULT::FR_INVALID_PARAMETER;
	}

	deluge::io::Status status = deluge::io::Status::OK;
	int jx = 1; // skip the leading slash.
	while (jx <= lastSlash) {
		if (working[jx] == '/') {
			working[jx] = 0;
			strcpy(pathPart, working);
			working[jx] = '/';
			if (strlen(pathPart)) {
				auto dir = deluge::io::Directory::open(pathPart);
				if (!dir.has_value() && dir.error() == deluge::io::Status::NOT_FOUND) {
					auto made = deluge::io::mkdir(pathPart);
					// preserving pre-existing quirk: a failed mkdir here does not
					// return early, it just leaves `status` set and the loop
					// continues to the next path segment.
					status = made.has_value() ? deluge::io::Status::OK : made.error();
					if (made.has_value() && timestamp.has_value()) {
						auto timed = deluge::io::set_time(pathPart, *timestamp);
						status = timed.has_value() ? deluge::io::Status::OK : timed.error();
					}
				}
				else if (!dir.has_value()) {
					return toWireFresult(dir.error());
				}
				// else: pathPart already exists as a directory; `dir` closes via
				// RAII when it goes out of scope at the end of this block.
			}
		}
		jx++;
	}
	return toWireFresult(status);
}

void smSysex::openFile(MIDICable& cable, JsonDeserializer& reader) {
	bool forWrite = false;
	std::string path;
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "write")) {
			forWrite = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}

	reader.match('}');
	bool pathCreateTried = false;
retry:
	FRESULT errCode;
	uint32_t fSize = 0;

	FILdata* fp = openFIL(path.c_str(), forWrite, &errCode);

	if (fp != nullptr) {
		fSize = fp->fSize;
	}
	if (forWrite && !pathCreateTried && errCode == FRESULT::FR_NO_FILE) { // was the path missing?
		createPathDirectories(path, timestampFromWireDateTime(date, time));
		pathCreateTried = true;
		goto retry;
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^open", false, true);
	jWriter.writeAttribute("fid", fp != nullptr ? fp->fileID : 0);
	jWriter.writeAttribute("size", fSize);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}

void smSysex::closeFile(MIDICable& cable, JsonDeserializer& reader) {
	int32_t fid = 0;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fd = entryForFID(fid);
	FRESULT errCode = closeFIL(fd);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^close", false, true);
	jWriter.writeAttribute("fid", (uint32_t)fid);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}

void smSysex::deleteFile(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string path;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!path.empty()) {
		D_PRINTLN(path.c_str());
		auto result = deluge::io::unlink(path);
		FRESULT errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^delete", false, true);
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

void smSysex::createDirectory(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string path;
	uint32_t date = 0;
	uint32_t time = 0;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!path.empty()) {
		D_PRINTLN(path.c_str());
		auto made = deluge::io::mkdir(path);
		deluge::io::Status status = made.has_value() ? deluge::io::Status::OK : made.error();
		auto timestamp = timestampFromWireDateTime(date, time);
		if (made.has_value() && timestamp.has_value()) {
			auto timed = deluge::io::set_time(path, *timestamp);
			status = timed.has_value() ? deluge::io::Status::OK : timed.error();
		}
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^mkdir", false, true);
		jWriter.writeAttribute("path", path.c_str());
		jWriter.writeAttribute("err", toWireFresult(status));
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

void smSysex::rename(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string fromName;
	std::string toName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "from")) {
			reader.readTagOrAttributeValueString(fromName);
		}
		else if (!strcmp(tagName, "to")) {
			reader.readTagOrAttributeValueString(toName);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	if (!fromName.empty() && !toName.empty()) {
		D_PRINTLN(fromName.c_str());
		D_PRINTLN(toName.c_str());
		auto result = deluge::io::rename(fromName, toName);
		FRESULT errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
		startReply(jWriter, reader);
		jWriter.writeOpeningTag("^rename", false, true);
		jWriter.writeAttribute("from", fromName.c_str());
		jWriter.writeAttribute("to", toName.c_str());
		jWriter.writeAttribute("err", errCode);
		jWriter.closeTag(true);
		sendMsg(cable, jWriter);
	}
}

// Returns a block of directory entries as a Json array.
void smSysex::getDirEntries(MIDICable& cable, JsonDeserializer& reader) {
	std::string path;
	path = "/";
	uint32_t lineOffset = 0;
	uint32_t linesWanted = 20;

	FRESULT errCode = FRESULT::FR_OK;
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "offset")) {
			lineOffset = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "lines")) {
			linesWanted = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');
	if (linesWanted > MAX_DIR_LINES)
		linesWanted = MAX_DIR_LINES;
	// We should pick up on path changes and out-of-order offset requests.

	if (lineOffset == 0 || activeDirName != path || lineOffset != dirOffsetCounter) {
		auto opened = deluge::io::Directory::open(path);
		if (!opened.has_value()) {
			errCode = toWireFresult(opened.error());
			goto errorFound;
		}
		sxDir = std::move(*opened);
		dirOffsetCounter = 0;
		activeDirName = path;
		if (lineOffset > 0) {
			for (uint32_t ix = 0; ix < lineOffset; ++ix) {
				auto entry = sxDir->read();
				if (!entry.has_value()) {
					errCode = toWireFresult(entry.error());
					break;
				}
				if (!entry->has_value()) {
					break;
				}
				dirOffsetCounter++;
			}
		}
	}
errorFound:;
	jWriter.reset();
	jWriter.setMemoryBased();
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^dir", false, true);
	jWriter.writeArrayStart("list", true, false);

	for (uint32_t ix = 0; ix < linesWanted && sxDir.has_value(); ++ix) {
		auto entry = sxDir->read();
		if (!entry.has_value() || !entry->has_value()) {
			break;
		}
		const DelugeDirEntry& fno = **entry;

		jWriter.writeOpeningTag(NULL, true);
		jWriter.writeAttribute("name", fno.name);
		jWriter.writeAttribute("size", fno.size);
		jWriter.writeAttribute("date", wireDateFromTimestamp(fno.modified_time));
		jWriter.writeAttribute("time", wireTimeFromTimestamp(fno.modified_time));

		// AM_RDO  0x01 Read only
		// AM_HID  0x02 Hidden
		// AM_SYS  0x04 System

		// AM_DIR  0x10 Directory
		// AM_ARC  0x20 Archive
		jWriter.writeAttribute("attr", wireAttribFromFlags(fno));

		jWriter.closeTag();
		dirOffsetCounter++;
	}
	jWriter.writeArrayEnding("list", true, false);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::readBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	int32_t fid = 0;

	auto repSN = reader.getReplySeqNum();
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "fid")) {
			fid = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FILdata* fp = entryForFID(fid);
	FRESULT errCode = FRESULT::FR_OK;

	if (fp == nullptr || !fp->fileOpen) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	uint8_t* srcAddr = (uint8_t*)addr;
	if (errCode == FRESULT::FR_OK) {
		if (!readBlockBuffer && fp) {
			readBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
		}

		if (readBlockBuffer && fp) {
			noteFileIdUse(fp);
			deluge::io::Status status = deluge::io::Status::OK;
			// If file position requested is not what we expect, seek to requested.
			if (fp->fPosition != addr) {
				auto seeked = fp->file->seek(addr);
				status = seeked.has_value() ? deluge::io::Status::OK : seeked.error();
			}
			if (status == deluge::io::Status::OK) {
				auto result = fp->file->read(std::span{reinterpret_cast<std::byte*>(readBlockBuffer), size});
				if (result.has_value()) {
					uint32_t actuallyRead = static_cast<uint32_t>(result->size());
					size = actuallyRead;
					srcAddr = readBlockBuffer;
					fp->fPosition = addr + actuallyRead;
				}
				else {
					status = result.error();
					size = 0;
				}
			}
			else {
				D_PRINTLN("lseek issue: %d", toWireFresult(status));
			}
			errCode = toWireFresult(status);
		}
	}
	else {
		size = 0;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^read", false, true);
	jWriter.writeAttribute("fid", fid);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	jWriter.writeByte(0); // spacer between Json and encoded block.

	uint8_t working[8];
	if (size == 0) {
		D_PRINTLN("Read size 0");
	}
	for (uint32_t ix = 0; ix < size; ix += 7) {
		int pktSize = 7;
		if (ix + pktSize > size) {
			pktSize = size - ix;
		}
		uint8_t hiBits = 0;
		uint8_t rotBit = 1;
		for (int i = 1; i <= pktSize; ++i) {
			working[i] = (*srcAddr) & 0x7F;
			if ((*srcAddr) & 0x80) {
				hiBits |= rotBit;
			}
			srcAddr++;
			rotBit <<= 1;
		}
		working[0] = hiBits;
		jWriter.writeBlock(working, pktSize + 1);
	}
	sendMsg(cable, jWriter);
}

void smSysex::writeBlock(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t fileId = 0;
	uint32_t addr = 0;
	uint32_t size = blockBufferMax;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "addr")) {
			addr = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "size")) {
			size = reader.readTagOrAttributeValueInt();
			if (size > blockBufferMax)
				size = blockBufferMax;
		}
		else if (!strcmp(tagName, "fid")) {
			fileId = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	if (!writeBlockBuffer) {
		writeBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
	}
	reader.match('}');
	reader.match('}'); // skip box too.

	char aChar;
	if (reader.peekChar(&aChar) && aChar != 0) {
		D_PRINTLN("Missing Separater error in writeBlock");
	}
	uint32_t decodedSize = decodeDataFromReader(reader, writeBlockBuffer, size);
	D_PRINTLN("Decoded block len: %d", decodedSize);

	FRESULT errCode = FRESULT::FR_OK;
	FILdata* fp = entryForFID(fileId);

	if (fp == nullptr || !fp->fileOpen) {
		errCode = FRESULT::FR_NOT_ENABLED;
	}
	if (writeBlockBuffer && (fp != nullptr) && fp->fileOpen) {
		deluge::io::Status status = deluge::io::Status::OK;
		if (addr != fp->fPosition) {
			auto seeked = fp->file->seek(addr);
			status = seeked.has_value() ? deluge::io::Status::OK : seeked.error();
		}
		if (status == deluge::io::Status::OK) {
			noteFileIdUse(fp);
			auto result = fp->file->write(std::span{reinterpret_cast<const std::byte*>(writeBlockBuffer), decodedSize});
			if (result.has_value()) {
				size = *result;
				fp->fPosition = addr + *result;
			}
			else {
				status = result.error();
				size = 0;
			}
		}
		errCode = toWireFresult(status);
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^write", false, true);
	jWriter.writeAttribute("fid", fileId);
	jWriter.writeAttribute("addr", addr);
	jWriter.writeAttribute("size", size);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);

	sendMsg(cable, jWriter);
}

void smSysex::updateTime(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	uint32_t date = 0;
	uint32_t time = 0;
	std::string path;

	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "path")) {
			reader.readTagOrAttributeValueString(path);
		}
		else if (!strcmp(tagName, "date")) {
			date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	FRESULT errCode;
	auto timestamp = timestampFromWireDateTime(date, time);
	if (!path.empty() && timestamp.has_value()) {
		auto result = deluge::io::set_time(path, *timestamp);
		errCode = toWireFresult(result.has_value() ? deluge::io::Status::OK : result.error());
	}
	else {
		errCode = FRESULT::FR_INVALID_PARAMETER;
	}
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^utime", false, true);
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

// A session ID or sid is a number used by clients to keep track of which messages belong to who.
void smSysex::assignSession(MIDICable& cable, JsonDeserializer& reader) {
	char const* tagName;
	std::string tag;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "tag")) {
			reader.readTagOrAttributeValueString(tag);
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	int sessionNum = 0;
	uint32_t minSeen = session_mono_counter;
	for (int i = 1; i <= MAX_SYSEX_SESSIONS; ++i) {
		if (sessionLRU_array[i] == 0) {
			sessionNum = i;
			break;
		}
		if (sessionLRU_array[i] < minSeen) {
			minSeen = sessionLRU_array[i];
			sessionNum = i;
		}
	}
	// Note the sessionNum as MRU to claim it.
	sessionLRU_array[sessionNum] = session_mono_counter++;

	startDirect(jWriter);
	jWriter.writeOpeningTag("^session", false, true);
	jWriter.writeAttribute("sid", sessionNum);
	jWriter.writeAttribute("tag", tag.c_str());
	jWriter.writeAttribute("midBase", sessionNum << SYSEX_SESSION_SHIFT);
	jWriter.writeAttribute("midMin", (sessionNum << SYSEX_SESSION_SHIFT) + 1);
	jWriter.writeAttribute("midMax", (sessionNum << SYSEX_SESSION_SHIFT) + SYSEX_MSGID_MAX);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::doPing(MIDICable& cable, JsonDeserializer& reader) {
	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^ping", false, true);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

uint32_t smSysex::decodeDataFromReader(JsonDeserializer& reader, uint8_t* dest, uint32_t destMax) {
	char zip = 0;
	if (!reader.readChar(&zip) || zip) // skip separator, fail if not there.
		return 0;
	uint32_t encodedSize = reader.bytesRemainingInBuffer() - 1; // don't count that 0xF7.
	uint32_t amount = unpack_7bit_to_8bit(dest, destMax, (uint8_t*)reader.GetCurrentAddressInBuffer(), encodedSize);
	return amount;
}

void smSysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}

	SysExDataEntry& de = SysExQ.emplace_back(cable, len);
	memcpy(de.data, data, len);
}

// --- Rung-4a: run each SysEx request's file ops on the storage owner --------------------------
// smSysex is a namespace (no instance state), so the single-flight guard and the op trampoline live
// at file scope. processFrontSysEx() is the parse+handle+reply+dequeue body; handleNextSysEx() is
// the per-tick trigger that dispatches it via deluge::storage::Owner::run so the FatFS work runs on
// the storage owner rather than the task stack.
namespace smSysex {
void processFrontSysEx();
}

namespace {
/// True while an owner op is processing SysExQ.front(). Prevents a later handleNextSysEx() tick from
/// dispatching a second op for the same (or next) entry before the first pops it. Set at dispatch,
/// cleared when the op completes. Synchronous, single executor thread (same contract as the queue).
bool g_sysex_op_in_flight = false;

/// Owner-op trampoline: process the front SysEx request, then release the single-flight guard.
void runSysexOp(void*) {
	smSysex::processFrontSysEx();
	g_sysex_op_in_flight = false;
}
} // namespace

void smSysex::handleNextSysEx() {

	if (SysExQ.empty()) {
		return;
	}
	if (g_sysex_op_in_flight) {
		return; // an op is already processing the front entry; it pops + clears the guard when done
	}
	if (deluge::sync::sd_busy()) {
		return;
	}

	// Dispatch the front request's parse+handle+reply onto the storage owner. Inline on legacy/host
	// (one request per tick, exactly as before); on Embassy the FatFS work leaves the task stack.
	g_sysex_op_in_flight = true;
	if (!deluge::storage::Owner::run(&runSysexOp, nullptr)) {
		g_sysex_op_in_flight = false; // owner queue full → the op won't run; retry next tick
	}
}

void smSysex::processFrontSysEx() {

	SysExDataEntry& de = SysExQ.front();

	char const* tagName;
	uint8_t msgSeqNum = de.data[1];
	noteSessionIdUse(msgSeqNum);
	JsonDeserializer parser(de.data + 2, de.len - 2);
	parser.setReplySeqNum(msgSeqNum);

	parser.match('{');
	while (*(tagName = parser.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "open")) {
			openFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "close")) {
			closeFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "dir")) {
			getDirEntries(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "read")) {
			readBlock(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "write")) {
			writeBlock(de.cable, parser);
			goto done; // Already skipped end.
		}
		else if (!strcmp(tagName, "delete")) {
			deleteFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "mkdir")) {
			createDirectory(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "rename")) {
			rename(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "copy")) {
			copyFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "move")) {
			moveFile(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "utime")) {
			updateTime(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "session")) {
			assignSession(de.cable, parser);
			goto done;
		}
		else if (!strcmp(tagName, "ping")) {
			doPing(de.cable, parser);
			goto done;
		}
		parser.exitTag();
	}
done:
	SysExQ.pop_front();
}

// Helper function to parse file operation parameters
bool smSysex::parseFileOpParams(JsonDeserializer& reader, FileOpParams& params) {
	char const* tagName;
	reader.match('{');
	while (*(tagName = reader.readNextTagOrAttributeName())) {
		if (!strcmp(tagName, "from")) {
			reader.readTagOrAttributeValueString(params.fromName);
		}
		else if (!strcmp(tagName, "to")) {
			reader.readTagOrAttributeValueString(params.toName);
		}
		else if (!strcmp(tagName, "date")) {
			params.date = reader.readTagOrAttributeValueInt();
		}
		else if (!strcmp(tagName, "time")) {
			params.time = reader.readTagOrAttributeValueInt();
		}
		else {
			reader.exitTag();
		}
	}
	reader.match('}');

	return !params.fromName.empty() && !params.toName.empty();
}

// Helper function to set file timestamp
void smSysex::setFileTimestamp(std::string_view path, uint32_t date, uint32_t time) {
	auto timestamp = timestampFromWireDateTime(date, time);
	if (timestamp.has_value()) {
		(void)deluge::io::set_time(path, *timestamp);
	}
}

// Helper function to perform file copy operation
FRESULT smSysex::performFileCopy(const FileOpParams& params) {
	D_PRINTLN(params.fromName.c_str());
	D_PRINTLN(params.toName.c_str());

	bool pathCreateTried = false;
	for (;;) {
		auto src = deluge::io::File::open(params.fromName, DELUGE_FILE_READ);
		if (!src.has_value()) {
			return toWireFresult(src.error());
		}

		auto dst = deluge::io::File::open(params.toName, DELUGE_FILE_WRITE_CREATE);
		if (!dst.has_value()) {
			if (dst.error() == deluge::io::Status::NOT_FOUND && !pathCreateTried) {
				// Don't set timestamps on directories - let them use current time
				std::string toNameCopy = params.toName;
				createPathDirectories(toNameCopy, std::nullopt);
				pathCreateTried = true;
				continue; // `src` closes via RAII at the end of this iteration
			}
			return toWireFresult(dst.error());
		}

		deluge::io::Status status = deluge::io::Status::OK;
		if (!readBlockBuffer) {
			readBlockBuffer = (uint8_t*)deluge::memory::alloc_sdram(blockBufferMax);
		}
		if (readBlockBuffer) {
			for (;;) {
				auto bytesRead = src->read(std::span{reinterpret_cast<std::byte*>(readBlockBuffer), blockBufferMax});
				if (!bytesRead.has_value()) {
					status = bytesRead.error();
					break;
				}
				if (bytesRead->empty()) {
					break;
				}
				auto bytesWritten = dst->write(*bytesRead);
				if (!bytesWritten.has_value()) {
					status = bytesWritten.error();
					break;
				}
				if (*bytesWritten != bytesRead->size()) {
					// preserving pre-existing quirk: a short write here is not
					// itself flagged as an error, `status` stays OK.
					break;
				}
				if (bytesRead->size() < blockBufferMax) {
					break;
				}
			}
		}
		else {
			status = deluge::io::Status::NO_MEMORY;
		}

		if (status == deluge::io::Status::OK && params.hasTimestamp()) {
			setFileTimestamp(params.toName, params.date, params.time);
		}
		return toWireFresult(status);
	}
}

void smSysex::copyFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	FRESULT errCode = performFileCopy(params);

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^copy", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}

void smSysex::moveFile(MIDICable& cable, JsonDeserializer& reader) {
	FileOpParams params;

	if (!parseFileOpParams(reader, params)) {
		return;
	}

	D_PRINTLN(params.fromName.c_str());
	D_PRINTLN(params.toName.c_str());

	// Try rename first (works if source and destination are on same filesystem)
	auto renamed = deluge::io::rename(params.fromName, params.toName);
	FRESULT errCode = toWireFresult(renamed.has_value() ? deluge::io::Status::OK : renamed.error());

	// If rename failed due to missing path, try creating directories
	if (!renamed.has_value() && renamed.error() == deluge::io::Status::NOT_FOUND) {
		// Don't set timestamps on directories - let them use current time
		std::string toNameCopy = params.toName;
		createPathDirectories(toNameCopy, std::nullopt);
		renamed = deluge::io::rename(params.fromName, params.toName);
		errCode = toWireFresult(renamed.has_value() ? deluge::io::Status::OK : renamed.error());
	}

	// If rename still fails (e.g., cross-filesystem move), fall back to copy+delete
	if (!renamed.has_value()) {
		// Use the shared copy function
		errCode = performFileCopy(params);

		// If copy was successful, delete the source file
		if (errCode == FRESULT::FR_OK) {
			auto deleted = deluge::io::unlink(params.fromName);

			// For move operation, both copy and delete must succeed
			if (!deleted.has_value()) {
				FRESULT deleteResult = toWireFresult(deleted.error());
				D_PRINTLN("Move: copy succeeded but delete failed: %d", deleteResult);
				// Clean up the destination file since move failed
				(void)deluge::io::unlink(params.toName);
				errCode = deleteResult;
			}
		}
	}
	else {
		// Rename was successful, set timestamp if provided
		if (params.hasTimestamp()) {
			setFileTimestamp(params.toName, params.date, params.time);
		}
	}

	startReply(jWriter, reader);
	jWriter.writeOpeningTag("^move", false, true);
	jWriter.writeAttribute("from", params.getFromPath());
	jWriter.writeAttribute("to", params.getToPath());
	jWriter.writeAttribute("err", errCode);
	jWriter.closeTag(true);
	sendMsg(cable, jWriter);
}
