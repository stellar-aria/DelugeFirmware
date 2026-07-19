#pragma once

#include <bit>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace deluge::util {

/// @brief Growable, indexable container whose elements NEVER move once
///        constructed: `T` lives in fixed-size heap segments and growth appends
///        segments, so a `T&`/`T*` from `operator[]` stays valid across all later
///        growth.
///
/// This is the property `std::vector` cannot give (its backing array reallocates
/// and moves on growth). It exists for state shared across threads where one side
/// may grow the container while another holds a reference into it — see
/// `SampleStream::table_` and docs/dev/known-concurrency-bugs.md (B2).
///
/// @note Single-writer for structural mutation (`resize`). Concurrent readers may
///       hold references from `operator[]` across a grow. Shrinking destroys the
///       removed tail — same contract as `std::vector::resize` to a smaller size:
///       it must not race a reader of those tail elements.
///
/// @tparam T           Element type; default-constructible, nothrow-destructible;
///                     may be move-only.
/// @tparam SegmentSize Elements per segment; a compile-time power of two.
template <typename T, std::size_t SegmentSize = 256>
class SegmentedVector {
	static_assert((SegmentSize & (SegmentSize - 1)) == 0 && SegmentSize != 0,
	              "SegmentSize must be a non-zero power of two");

	struct Segment {
		alignas(T) std::byte storage[sizeof(T) * SegmentSize];
	};

	static constexpr std::size_t kMask = SegmentSize - 1;
	static constexpr std::size_t kShift = std::countr_zero(SegmentSize);
	// Deliberately `std::allocator`, not `deluge::memory::fast_allocator`: like
	// `spsc_ring.h`, this header stays dependency-free (no BSP-initialized-heap
	// requirement), so it links in the plain host CppSpec harness as well as
	// firmware/sim. A production call site that wants SRAM-preferred segment
	// storage can layer that in when it wires this container up.
	using SegAlloc = std::allocator<Segment>;

public:
	SegmentedVector() = default;
	SegmentedVector(const SegmentedVector&) = delete;
	SegmentedVector& operator=(const SegmentedVector&) = delete;

	SegmentedVector(SegmentedVector&& other) noexcept
	    : segments_{std::move(other.segments_)}, size_{std::exchange(other.size_, 0)} {}
	SegmentedVector& operator=(SegmentedVector&& other) noexcept {
		if (this != &other) {
			clear_and_free();
			segments_ = std::move(other.segments_);
			size_ = std::exchange(other.size_, 0);
		}
		return *this;
	}

	~SegmentedVector() { clear_and_free(); }

	[[nodiscard]] std::size_t size() const { return size_; }
	[[nodiscard]] bool empty() const { return size_ == 0; }

	[[nodiscard]] T& operator[](std::size_t i) { return *slot(i); }
	[[nodiscard]] const T& operator[](std::size_t i) const { return *slot(i); }

	/// @brief Resize to exactly @p n: grow default-constructs new tail elements,
	///        shrink destroys removed tail elements. Existing elements never move.
	void resize(std::size_t n) {
		while (size_ > n) {
			pop_back();
		}
		while (size_ < n) {
			if ((size_ >> kShift) >= segments_.size()) {
				segments_.push_back(alloc_segment());
			}
			std::construct_at(slot(size_));
			++size_;
		}
	}

private:
	[[nodiscard]] T* slot(std::size_t i) const {
		std::byte* base = segments_[i >> kShift]->storage;
		return std::launder(reinterpret_cast<T*>(base + (i & kMask) * sizeof(T)));
	}

	static Segment* alloc_segment() {
		SegAlloc a;
		return a.allocate(1); // raw, alignof(T)-aligned storage; Segment is trivial
	}
	static void free_segment(Segment* s) {
		SegAlloc a;
		a.deallocate(s, 1);
	}

	void pop_back() {
		--size_;
		std::destroy_at(slot(size_));
		if ((size_ & kMask) == 0) { // just emptied the trailing segment
			free_segment(segments_.back());
			segments_.pop_back();
		}
	}

	void clear_and_free() {
		while (size_ > 0) {
			pop_back();
		}
		segments_.clear();
	}

	std::vector<Segment*> segments_{};
	std::size_t size_ = 0;
};

} // namespace deluge::util
