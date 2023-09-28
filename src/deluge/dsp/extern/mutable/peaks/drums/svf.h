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
// SVF used for modeling the bridged T-networks.

#ifndef PEAKS_DRUMS_SVF_H_
#define PEAKS_DRUMS_SVF_H_

#include <cstdint>
#include <limits>

#include "../../stmlib/utils/dsp.h"
#include "resources.h"

namespace peaks {

class Svf {
public:
	enum class Mode { LowPass, BandPass, HighPass };

	constexpr Svf() = default;
	~Svf() = default;

	// No copy/assign
	Svf(const Svf&) = delete;
	const Svf& operator=(const Svf&) = delete;

	constexpr void set_frequency(int16_t frequency) {
		dirty_ = dirty_ || (frequency_ != frequency);
		frequency_ = frequency;
	}

	constexpr void set_resonance(int16_t resonance) {
		resonance_ = resonance;
		dirty_ = true;
	}

	constexpr void set_punch(uint16_t punch) { punch_ = (static_cast<uint32_t>(punch) * punch) >> 24; }

	template <Mode mode>
	constexpr int32_t Process(int32_t in) {
		if (dirty_) {
			f_ = stmlib::Interpolate824(lut_svf_cutoff, frequency_ << 17);
			damp_ = stmlib::Interpolate824(lut_svf_damp, resonance_ << 17);
			dirty_ = false;
		}

		int32_t f = f_;

		int32_t damp = damp_;
		if (punch_) {
			int32_t punch_signal = lp_ > 4096 ? lp_ : 2048;
			f += ((punch_signal >> 4) * punch_) >> 9;
			damp += ((punch_signal - 2048) >> 3);
		}

		int32_t notch = in - (bp_ * damp >> 15);
		lp_ += f * bp_ >> 15;
		lp_ = stmlib::clip(lp_);

		int32_t hp = notch - lp_;
		bp_ += f * hp >> 15;
		bp_ = stmlib::clip(bp_);

		return mode == Mode::BandPass ? bp_ : (mode == Mode::HighPass ? hp : lp_);
	}

private:
	bool dirty_ = true;

	int16_t frequency_ = 33 << 7;
	int16_t resonance_ = std::numeric_limits<int16_t>::max() / 2;

	int32_t punch_ = 0;
	int32_t f_ = 0;
	int32_t damp_ = 0;

	int32_t lp_ = 0;
	int32_t bp_ = 0;

	Mode mode_ = Mode::LowPass;
};

} // namespace peaks

#endif // PEAKS_DRUMS_SVF_H_
