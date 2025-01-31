#pragma once

#include <cstdint>
#include <optional>

#include "definitions_cxx.hpp"

#ifdef IN_UNIT_TESTS
#include "clip_mocks.h"
#include "song_mock.h"
#else
#include "deluge.h"
#include "model/clip/clip.h"
#include "model/clip/clip_array.h"
#include "model/song/song.h"
#endif

// This is an incomplete iterator, missing operator*() and operator->(): the templated iterators inherit
// from this and implement just those for minimal template bloat.
class ClipIteratorBase {
public:
	[[gnu::noinline]] ClipIteratorBase(ClipArray* array_, uint32_t index_, ClipArray* nextArray_,
	                                   std::optional<ClipType> clipType_);
	[[gnu::noinline]] ClipIteratorBase& operator++();
	[[gnu::noinline]] ClipIteratorBase operator++(int);
	/** Deletes the clip pointed to by the iterator and advances it. */
	[[gnu::noinline]] void deleteClip(InstrumentRemoval instrumentRemoval);
	[[gnu::noinline]] friend bool operator==(const ClipIteratorBase& a, const ClipIteratorBase& b) {
		return (a.array == b.array) && (a.index == b.index) && (a.clipType == b.clipType);
	}
	[[gnu::noinline]] friend bool operator!=(const ClipIteratorBase& a, const ClipIteratorBase& b) {
		return (a.array != b.array) || (a.index != b.index) || (a.clipType != b.clipType);
	}

private:
	[[gnu::noinline]] void next();

protected:
	ClipArray* array;
	ClipArray* nextArray;
	int32_t index;
	std::optional<ClipType> clipType;
};

template <typename ClipClass>
class ClipIterator : public ClipIteratorBase {
public:
	using ClipIteratorBase::ClipIteratorBase;
	ClipClass*& operator*() { return *(ClipClass**)array->getElementAddress(index); }
	ClipClass** operator->() { return (ClipClass**)array->getElementAddress(index); }
};

class ClipSet {
protected:
	ClipSet(ClipArray* first_, ClipArray* second_) : first(first_), second(second_) {}
	ClipArray* first;
	ClipArray* second;
};

class AllClips : ClipSet {
public:
	/** Constructs an iterable for all Clips in session and arranger. */
	static AllClips everywhere(Song* song);
	/** Constructs an iterable for all Clips in session. */
	static AllClips inSession(Song* song);
	/** Constructs an iterable for all Clips in arranger only. */
	static AllClips inArrangementOnly(Song* song);
	ClipIterator<Clip> begin();
	ClipIterator<Clip> end();

private:
	using ClipSet::ClipSet;
};

class InstrumentClips : ClipSet {
public:
	/** Constructs an iterable for all InstrumentClips in session and arranger. */
	static InstrumentClips everywhere(Song* song);
	ClipIterator<InstrumentClip> begin();
	ClipIterator<InstrumentClip> end();

private:
	using ClipSet::ClipSet;
};

class AudioClips : ClipSet {
public:
	/** Constructs an iterable for all AudioClips in session and arranger. */
	static AudioClips everywhere(Song* song);
	ClipIterator<AudioClip> begin();
	ClipIterator<AudioClip> end();

private:
	using ClipSet::ClipSet;
};
