#include "storage/existence_policy.h"

namespace deluge::storage {

Presence presence_of(const std::expected<bool, deluge::io::Status>& present) {
	if (!present) {
		return Presence::Undeterminable;
	}
	return *present ? Presence::Present : Presence::Absent;
}

Bootstrap bootstrap_action(const std::expected<bool, deluge::io::Status>& present) {
	switch (presence_of(present)) {
	case Presence::Present:
		return Bootstrap::Load;
	case Presence::Absent:
		return Bootstrap::WriteDefaults;
	case Presence::Undeterminable:
		return Bootstrap::UseInMemoryOnly;
	}
	// Unreachable for a valid Presence; UseInMemoryOnly is the safe fallback (writes nothing).
	return Bootstrap::UseInMemoryOnly;
}

} // namespace deluge::storage
