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
// 808-style snare drum.

#include "snare_drum.h"
#include "../../stmlib/utils/dsp.h"
#include "util/random.h"
#include <cassert>
#include <cstdio>

namespace peaks {

using namespace stmlib;

void SnareDrum::Process(skyline::TriggerSignal triggers, std::span<int16_t> out) {
	assert(triggers.size() == out.size());

	for (size_t i = 0; i < out.size(); ++i) {
		auto trigger = triggers[i];
		if (trigger.rising()) {
			excitation_1_up_.Trigger(15 * 32768);
			excitation_1_down_.Trigger(-1 * 32768);
			excitation_2_.Trigger(13107);
			excitation_noise_.Trigger(snappy_);
		}

		int32_t excitation_1 = 0;
		excitation_1 += excitation_1_up_.Process();
		excitation_1 += excitation_1_down_.Process();
		excitation_1 += !excitation_1_down_.done() ? 2621 : 0;

		int32_t body_1 = body_1_.Process<Svf::Mode::BandPass>(excitation_1) + (excitation_1 >> 4);

		int32_t excitation_2 = 0;
		excitation_2 += excitation_2_.Process();
		excitation_2 += !excitation_2_.done() ? 13107 : 0;

		int32_t body_2 = body_2_.Process<Svf::Mode::BandPass>(excitation_2) + (excitation_2 >> 4);
		int32_t noise_sample = deluge::random();
		int32_t noise = noise_.Process<Svf::Mode::BandPass>(noise_sample);
		int32_t noise_envelope = excitation_noise_.Process();
		int32_t sd = 0;
    
		sd += body_1 * gain_1_ >> 15;
		sd += body_2 * gain_2_ >> 15;
		sd += noise_envelope * noise >> 15;
		out[i] = clip(sd);
	}
}

} // namespace peaks
