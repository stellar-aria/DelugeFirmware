// Copyright 2013 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Exponential decay excitation.

#pragma once

#include <cstdint>

namespace peaks {

class Excitation {
public:
	constexpr Excitation() = default;
	~Excitation() = default;

	Excitation(const Excitation&) = delete;
	const Excitation& operator=(const Excitation&) = delete;

	constexpr void set_delay(uint16_t delay) { delay_ = delay; }

	constexpr void set_decay(uint16_t decay) { decay_ = decay; }

	constexpr void Trigger(int32_t level) {
		level_ = level;
		counter_ = delay_ + 1;
	}

	constexpr bool done() { return counter_ == 0; }

	constexpr int32_t Process() {
		state_ = (state_ * decay_ >> 12);
		if (counter_ > 0) {
			--counter_;
			if (counter_ == 0) {
				state_ += level_ < 0 ? -level_ : level_;
			}
		}
		return level_ < 0 ? -state_ : state_;
	}

private:
	uint32_t delay_ = 0;
	uint32_t decay_ = 4093;
	int32_t counter_ = 0;
	int32_t state_ = 0;
	int32_t level_ = 0;
};

} // namespace peaks
