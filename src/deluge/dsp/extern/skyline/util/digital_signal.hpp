// Copyright (c) 2023 Katherine Whitlock [Skyline Synths LLC]
// Licensed under GPL for the DelugeFirmware project

#pragma once
#include <span>
#include <type_traits>

namespace skyline {
namespace digital {
enum State {
	LOW,
	HIGH,
	RISING,
	FALLING,
};

class Component {
public:
	Component() = default;

	template <typename T>
	Component(T state) {
		if constexpr (std::is_same_v<State, T>) {
			state_ = state;
		}
		else {
			state_ = bool(state) ? HIGH : LOW;
		}
	};

	[[nodiscard]] Component Process(bool new_state) const {
		using enum State;
		if (this->high()) {  // currently HIGH
			if (new_state) { // now HIGH
				return {HIGH};
			}
			return {FALLING}; // now LOW
		}
		else {               // currently LOW
			if (new_state) { // now HIGH
				return {RISING};
			}
			return {LOW}; // now low
		}
	}

	[[nodiscard]] bool high() const { return state_ == State::HIGH || state_ == State::RISING; }

	[[nodiscard]] bool rising() const { return state_ == State::RISING; }

	[[nodiscard]] bool low() const { return state_ == State::LOW || state_ == State::FALLING; }

	[[nodiscard]] bool falling() const { return state_ == State::FALLING; }

	operator bool() const { return this->high(); }

private:
	State state_ = State::LOW;
};
} // namespace digital

using DigitalSignal = std::span<skyline::digital::Component>;
using GateSignal = DigitalSignal;
using ClockSignal = DigitalSignal;
using TriggerSignal = DigitalSignal;
} // namespace skyline
