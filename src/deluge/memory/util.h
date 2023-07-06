#pragma once
#include <limits>
#include <cstddef>

namespace deluge::memory {
#if ALPHA_OR_BETA_VERSION
size_t closest_distance = std::numeric_limits<size_t>::max();
#endif

void checkStack(char const* caller) {
#if ALPHA_OR_BETA_VERSION
	uint8_t a;

	int distance = (int)&a - (INTERNAL_MEMORY_END - PROGRAM_STACK_MAX_SIZE);
	if (distance < closest_distance) {
		closest_distance = distance;

		Uart::print(distance);
		Uart::print(" free bytes in stack at ");
		Uart::println(caller);

		if (distance < 200) {
			Uart::println("COLLISION");
			numericDriver.freezeWithError("E338");
		}
	}
#endif
}
} // namespace deluge::memory
