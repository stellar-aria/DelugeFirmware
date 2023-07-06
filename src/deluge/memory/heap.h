#pragma once
#include <cstddef>
#include <umm_malloc/src/umm_malloc.h>

namespace deluge::memory {
class Heap {
public:
	Heap(const size_t start_address, const size_t end_address)
	    : start_addr_{start_address}, end_addr_{end_address}, config_{} {
		umm_multi_init_heap(&config_, reinterpret_cast<void*>(start_address), end_address - start_address);
	}

	void* malloc(size_t size) { return umm_multi_malloc(&config_, size); }

	void* calloc(size_t num, size_t size) { return umm_multi_calloc(&config_, num, size); }

	void* realloc(void* ptr, size_t size) { return umm_multi_realloc(&config_, ptr, size); }

	void free(void* ptr) { umm_multi_free(&config_, ptr); }

	[[nodiscard]] size_t size() const { return end_addr_ - start_addr_; }

	[[nodiscard]] bool contains(const size_t address) const { return address >= start_addr_ && address < end_addr_; }

	[[nodiscard]] bool contains(const void* ptr) const { return contains(reinterpret_cast<size_t>(ptr)); }

	/**
	 * @brief Compare two heaps to see if they are the same (start and end are same)
	 *
	 * @param o The heap to compare against
	 * @return true The two heaps cover the same area of memory
	 * @return false The two heaps do not cover the same area of memory
	 */
	bool operator==(const Heap& o) const { return start_addr_ == o.start_addr_ && end_addr_ == o.end_addr_; }

private:
	size_t start_addr_;
	size_t end_addr_;
	umm_heap config_;
};

extern Heap internal;
extern Heap SDRAM;

} // namespace deluge::memory
