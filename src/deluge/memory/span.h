#pragma once
#include "heap.h"

#include <array>


namespace deluge::memory {
/**
 * @brief A Span bridges multiple Heaps and attempts allocating to them in order
 *
 * WARNING: Do NOT put the same Heap in a Span multiple times.
 */
template <size_t n>
class Span {
public:
	Span(std::initializer_list<Heap*> heaps) : heaps_{heaps} {}

	void* malloc(size_t size);
	void free(void* ptr);
private:
	std::array<Heap*, n> heaps_;
};

extern Span<2> combined;
} // namespace deluge::memory
