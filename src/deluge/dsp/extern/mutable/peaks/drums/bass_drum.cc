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

#include "bass_drum.h"

#include "../../stmlib/utils/dsp.h"

#include <cassert>

namespace peaks {

using namespace stmlib;
using namespace skyline;

void BassDrum::Process(TriggerSignal gate_flags, std::span<int16_t> out) {
	assert(gate_flags.size() == out.size());

	for (size_t i = 0; i < out.size(); ++i) {
		auto& gate_flag = gate_flags[i];
		if (gate_flag.rising()) {
			pulse_up_.Trigger(12 * 32768 * 0.7);
			pulse_down_.Trigger(-19662 * 0.7);
			attack_fm_.Trigger(18000);
		}

		int32_t excitation = 0;
		excitation += pulse_up_.Process();
		excitation += !pulse_down_.done() ? 16384 : 0;
		excitation += pulse_down_.Process();
		attack_fm_.Process();
		resonator_.set_frequency(frequency_ + (attack_fm_.done() ? 0 : 17 << 7));

		int32_t resonator_output = (excitation >> 4) + resonator_.Process<Svf::Mode::BandPass>(excitation);
		lp_state_ += (resonator_output - lp_state_) * lp_coefficient_ >> 15;
		int32_t output = lp_state_;

		out[i] = stmlib::clip(output);
	}
}

} // namespace peaks
