
#include <stdlib.h>

// Weak declarations for optional memory allocator (may not be available in controller mode)
extern void* delugeAlloc(unsigned int requiredSize, bool mayUseOnChipRam) __attribute__((weak));
extern void delugeDealloc(void* address) __attribute__((weak));

void* malloc(size_t size) {
	if (delugeAlloc != NULL) {
		return delugeAlloc(size, false);
	}
	// In controller mode without allocator, return NULL
	return NULL;
}

void free(void* ptr) {
	if (delugeDealloc != NULL && ptr != NULL) {
		delugeDealloc(ptr);
	}
}
