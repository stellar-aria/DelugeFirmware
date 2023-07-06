#include "span.h"

namespace deluge::memory {

Span<2> combined{{&internal, &SDRAM}};

template <size_t n>
void* Span<n>::malloc(size_t size) {
	void* ptr = nullptr;
	for (Heap* heap : heaps_) {
		// Try allocating using a heap
		ptr = heap->malloc(size);

		// If we receive a valid pointer (i.e. not a allocation error), return it
		if (ptr != nullptr) {
			return ptr;
		}
		// Otherwise, continue while trying the next heap
	}

	// if none of the allocations worked, return a nullptr to indicate error
	return nullptr;
}

template <size_t n>
void Span<n>::free(void* ptr) {
	for (Heap* heap : heaps_) {    // iterate through all our heaps
		if (heap->contains(ptr)) { // if the heap contains our pointer
			heap->free(ptr);       // free it
			break;                 // and stop iterating
		}
	}
}

} // namespace deluge::memory
