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
// Resources definitions.
//
// Automatically generated with:
// make resources


#pragma once


#include <cstdint>


namespace peaks {

using ResourceId = uint8_t;

extern const char* string_table[];

extern const uint16_t* lookup_table_table[];

extern const uint32_t* lookup_table_32_table[];

extern const uint16_t lut_raised_cosine[];
extern const uint16_t lut_svf_cutoff[];
extern const uint16_t lut_svf_damp[];
extern const uint16_t lut_svf_scale[];
extern const uint16_t lut_env_expo[];
extern const uint32_t lut_env_increments[];
extern const uint32_t lut_oscillator_increments[];
extern const int16_t wav_overdrive[];
extern const int16_t wav_sine[];

#define STR_DUMMY 0  // dummy
#define LUT_RAISED_COSINE 5
#define LUT_RAISED_COSINE_SIZE 257
#define LUT_SVF_CUTOFF 6
#define LUT_SVF_CUTOFF_SIZE 257
#define LUT_SVF_DAMP 7
#define LUT_SVF_DAMP_SIZE 257
#define LUT_SVF_SCALE 8
#define LUT_SVF_SCALE_SIZE 257

}  // namespace peaks
