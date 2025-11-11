#include "definitions.h"
#include <exception>

// Weak declaration for freezeWithError (may not be available in controller mode)
extern "C" void freezeWithError(char const*) __attribute__((weak));

[[noreturn]] void Terminate() noexcept {
	if (freezeWithError != nullptr) {
		freezeWithError("TERM");
	}
	// In controller mode, just loop forever
	while (true) {
		// Halt execution
	}
	__builtin_unreachable();
}

namespace __cxxabiv1 {
std::terminate_handler __terminate_handler = Terminate;
}
