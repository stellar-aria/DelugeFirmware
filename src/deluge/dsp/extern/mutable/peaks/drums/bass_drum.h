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
// 808-style bass drum.

#ifndef PEAKS_DRUMS_BASS_DRUM_H_
#define PEAKS_DRUMS_BASS_DRUM_H_

#include "../../../skyline/util/digital_signal.hpp"
#include "excitation.h"
#include "svf.h"
#include <cstdint>

namespace peaks {

class BassDrum {
public:
	constexpr BassDrum() {
		pulse_up_.set_delay(0);
		pulse_up_.set_decay(3340);

		pulse_down_.set_delay(1.0e-3 * 48000);
		pulse_down_.set_decay(3072);

		attack_fm_.set_delay(4.0e-3 * 48000);
		attack_fm_.set_decay(4093);

		resonator_.set_punch(32768);

		set_frequency(0);
		set_decay(32768);
		set_tone(32768);
		set_punch(65535);
	};

	~BassDrum() = default;
	BassDrum(const BassDrum&) = delete;
	const BassDrum& operator=(const BassDrum&) = delete;

	void Process(skyline::TriggerSignal, std::span<int16_t>);

	constexpr void Configure(uint16_t freq, uint16_t punch, uint16_t tone, uint16_t decay) {
		set_frequency(freq - 32768);
		set_punch(punch);
		set_tone(tone);
		set_decay(decay);
	}

	constexpr void set_frequency(int16_t frequency) { //<
		frequency_ = (31 << 7) + (static_cast<int32_t>(frequency) * 896 >> 15);
	}

	constexpr void set_decay(uint16_t decay) {
		uint32_t scaled;
		uint32_t squared;
		scaled = 65535 - decay;
		squared = scaled * scaled >> 16;
		scaled = squared * scaled >> 18;
		resonator_.set_resonance(32768 - 128 - scaled);
	}

	constexpr void set_tone(uint16_t tone) {
		uint32_t coefficient = tone;
		coefficient = coefficient * coefficient >> 16;
		lp_coefficient_ = 512 + (coefficient >> 2) * 3;
	}

	constexpr void set_punch(uint16_t punch) { //<
		resonator_.set_punch(punch * punch >> 16);
	}

private:
	Excitation pulse_up_;
	Excitation pulse_down_;
	Excitation attack_fm_;
	Svf resonator_;

	int32_t frequency_;
	int32_t lp_coefficient_;
	int32_t lp_state_ = 0;
};

} // namespace peaks

#endif // PEAKS_DRUMS_BASS_DRUM_H_
