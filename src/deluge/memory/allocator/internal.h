#pragma once
#include "memory/heap.h"

namespace deluge::memory {

/**
 * @brief A std-compatible allocator for allocating to the internal RAM
 *
 * @tparam T the Type to allocate
 */
template <typename T>
class internal_allocator {
	using value_type = T;
	using size_type = size_t;
	using difference_type = ptrdiff_t;

	T* allocate(size_t n) { static_cast<T*>(memory::internal.malloc(n * sizeof(T))); }
	T* deallocate(T* ptr, size_t n) { memory::internal.free(ptr); }
};

template <typename T, typename U>
bool operator==(const internal_allocator<T>& lhs, const internal_allocator<U>& rhs) {
	return true;
}

} // namespace deluge::memory
