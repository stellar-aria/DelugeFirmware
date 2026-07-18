/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <optional>

namespace deluge::storage {

/// @brief Single-flight dispatch with latest-wins supersession.
///
/// A high-frequency producer (e.g. sample preview on every cursor move) records a
/// desired target; at most one dispatch runs at a time, and when it completes the
/// *latest* requested target (if it changed meanwhile) is dispatched next — so the
/// result converges on where the user actually landed, never a stale intermediate.
/// Contrast `Coalescer`, which drops new requests while one is in flight (correct for
/// demand-collapsing, wrong for "show the newest").
///
/// Not thread-safe: all calls are expected from the single executor-thread context
/// that also runs the dispatched op (same contract as `Coalescer`).
template <typename T>
class LatestWins {
public:
	/// Record a desired target. Returns true if the caller should dispatch now
	/// (nothing was in flight); false if an op is already running — it will pick up
	/// this target on completion.
	bool request(const T& target) {
		if (in_flight_) {
			queued_ = target;
			return false;
		}
		in_flight_ = true;
		current_ = target;
		queued_.reset();
		return true;
	}

	/// Signal that the in-flight op finished. Returns the next target to dispatch if
	/// the desired target changed while running (latest-wins re-dispatch), or nullopt
	/// if nothing is pending (goes idle).
	std::optional<T> complete() {
		if (queued_.has_value()) {
			current_ = *queued_;
			queued_.reset();
			return current_; // stays in_flight_ — caller re-dispatches current_
		}
		in_flight_ = false;
		return std::nullopt;
	}

	[[nodiscard]] bool in_flight() const { return in_flight_; }
	[[nodiscard]] const T& current() const { return current_; }

private:
	bool in_flight_ = false;
	T current_{};
	std::optional<T> queued_{};
};

} // namespace deluge::storage
