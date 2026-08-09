#pragma once

#include "io/file.hpp"
#include <expected>

namespace deluge::storage {

/// @brief A three-state answer to "does this path exist?", suitable for exhaustive switching.
/// @note Unknown is NEVER absence. Handling it is mandatory at every call site — switch on this
///       enum without a default so -Wswitch catches a missing case.
enum class Presence {
	Present,        ///< the path is there
	Absent,         ///< the path is known not to be there (Status::NOT_FOUND)
	Undeterminable, ///< existence could not be determined (e.g. Status::BUSY off-owner)
};

/// @brief Classify an existence result for exhaustive handling.
/// @param present result of `StorageManager::fileExists` / `deluge::io::presence_from_open`.
Presence presence_of(const std::expected<bool, deluge::io::Status>& present);

/// @brief What a settings-file bootstrap path should do about a settings file.
enum class Bootstrap {
	Load,            ///< the file is there: parse it
	WriteDefaults,   ///< the file is known absent: it is safe to create and write defaults
	UseInMemoryOnly, ///< existence unknown: use in-memory defaults and WRITE NOTHING
};

/// @brief The one rule all settings-bootstrap readers share.
/// @param present result of the existence check on the settings file.
/// @return `UseInMemoryOnly` whenever existence is undeterminable — never write over a file whose
///         absence is unconfirmed.
Bootstrap bootstrap_action(const std::expected<bool, deluge::io::Status>& present);

} // namespace deluge::storage
