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
// 808-style HH.
//

#include "high_hat.h"
#include "../../stmlib/utils/dsp.h"
#include "util/random.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>

namespace peaks {

using namespace stmlib;
using namespace skyline;

static std::bernoulli_distribution dist{0.5};

void HighHat::Process(TriggerSignal triggers, std::span<int16_t> out) {
	assert(triggers.size() == out.size());

	for (size_t i = 0; i < out.size(); ++i) {
		auto trigger = triggers[i];

		if (trigger.rising()) { // && (open_ || (!open_ && !(gate_flag & GATE_FLAG_AUXILIARY_RISING)))) {

			// randomise parameters
			// frequency
			bool freq_up = dist(deluge::random);
			int32_t randomised_frequency = freq_up ? (last_frequency_ + (frequency_randomness_ >> 2))
			                                       : (last_frequency_ - (frequency_randomness_ >> 2));

			// Check if we haven't walked out-of-bounds, and if so, reverse direction on last step
			if (randomised_frequency < 0 || randomised_frequency > std::numeric_limits<uint16_t>::max()) {
				// flip the direction
				freq_up = !freq_up;
				randomised_frequency = freq_up ? (last_frequency_ + (frequency_randomness_ >> 2))
				                               : (last_frequency_ - (frequency_randomness_ >> 2));
			}

			// constrain randomised frequency - probably not necessary
			randomised_frequency = std::clamp<int32_t>(randomised_frequency, 0, std::numeric_limits<uint16_t>::max());

			// set new random frequency
			set_frequency(randomised_frequency);
			last_frequency_ = randomised_frequency;

			// decay
			bool decay_up = dist(deluge::random);
			int32_t randomised_decay = decay_up ? (last_decay_ + (decay_randomness_ >> 2)) //<
			                                    : (last_decay_ - (decay_randomness_ >> 2));

			// Check if we haven't walked out-of-bounds, and if so, reverse direction on last step
			if (randomised_decay < 0 || randomised_decay > std::numeric_limits<uint16_t>::max()) {
				// flip the direction
				decay_up = !decay_up;
				randomised_decay = decay_up ? (last_decay_ + (decay_randomness_ >> 2)) //<
				                            : (last_decay_ - (decay_randomness_ >> 2));
			}

			// constrain randomised frequency - probably not necessary
			randomised_decay = std::clamp<int32_t>(randomised_decay, 0, std::numeric_limits<uint16_t>::max());

			// set new random decay
			set_decay(randomised_decay);
			last_decay_ = randomised_decay;

			// Hit it!
			vca_envelope_.Trigger(0x8000 * 15);
		}

		phase_[0] += 48318382; // 0x2E147AE
		phase_[1] += 71582788; // 0x4444444
		phase_[2] += 37044092; // 0x2353F7C
		phase_[3] += 54313440; // 0x33CC1E0
		phase_[4] += 66214079; // 0x3F258BF
		phase_[5] += 93952409; // 0x5999999

		int16_t noise = 0;
		noise += phase_[0] >> 31;
		noise += phase_[1] >> 31;
		noise += phase_[2] >> 31;
		noise += phase_[3] >> 31;
		noise += phase_[4] >> 31;
		noise += phase_[5] >> 31;
		noise <<= 12;

		// Run the SVF at the double of the original sample rate for stability.
		int32_t filtered_noise = 0;
		filtered_noise += noise_.Process<Svf::Mode::BandPass>(noise);
		// filtered_noise += noise_.Process(noise);

		// The 808-style VCA amplifies only the positive section of the signal.
    filtered_noise = std::clamp<int32_t>(filtered_noise, 0, std::numeric_limits<int16_t>::max());

		int32_t envelope = vca_envelope_.Process() >> 4;
		int32_t vca_noise = envelope * filtered_noise >> 14;
		out[i] = clip(vca_noise);

		// int32_t hh = 0;
		// hh += vca_coloration_.Process<SVF_MODE_HP>(vca_noise);
		// hh += vca_coloration_.Process<SVF_MODE_HP>(vca_noise);
		// hh <<= 1;
		// CLIP(hh);
		// *out++ = hh;
	}
}

} // namespace peaks
