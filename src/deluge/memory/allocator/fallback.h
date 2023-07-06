#pragma once
#include "memory/span.h"

namespace deluge::memory {

/**
 * @brief A std-compatible allocator for allocating to the internal RAM, and then SDRAM if the allocation fails
 *
 * @tparam T the Type to allocate
 */
template <typename T>
class fallback_allocator {
	using value_type = T;
	using size_type = size_t;
	using difference_type = ptrdiff_t;

	T* allocate(size_t n) { static_cast<T*>(memory::combined.malloc(n * sizeof(T))); }
	T* deallocate(T* ptr, size_t n) { memory::combined.free(ptr); }
};

template <typename T, typename U>
bool operator==(const fallback_allocator<T>& lhs, const fallback_allocator<U>& rhs) {
	return true;
}
} // namespace deluge::memory
