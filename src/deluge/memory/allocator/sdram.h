#pragma once
#include "memory/heap.h"

namespace deluge::memory {

/**
 * @brief A std-compatible allocator for allocating to the SDRAM
 *
 * @tparam T the Type to allocate
 */
template <typename T>
class sdram_allocator {
	using value_type = T;
	using size_type = size_t;
	using difference_type = ptrdiff_t;

	T* allocate(size_t n) { static_cast<T*>(memory::SDRAM.malloc(n * sizeof(T))); }
	T* deallocate(T* ptr, size_t n) { memory::SDRAM.free(ptr); }
};

template <typename T, typename U>
bool operator==(const sdram_allocator<T>& lhs, const sdram_allocator<U>& rhs) {
	return true;
}
} // namespace deluge::memory
