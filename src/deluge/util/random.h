#pragma once
#include <cstdint>
#include <limits>
#include <random>

namespace deluge {

/** xoroshiro128++. Very fast, not-cryptographic random number generator.
 * Written in 2019 by David Blackman and Sebastiano Vigna (vigna@acm.org)
 * License: Public Domain */
class Xoroshiro128PlusPlus {
 public:
  using result_type = uint32_t;

  void Seed(uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3) {
    state_[0] = s0;
    state_[1] = s1;
	state_[2] = s2;
	state_[3] = s3;
    // A bad seed will give a bad first result, so shift the state
    operator()();
  }

  bool is_seeded() { return state_[0] || state_[1] || state_[2] || state_[3]; }

  uint32_t operator()() {
	const uint32_t result = rotl(state_[0] + state_[3], 7) + state_[0];
	const uint32_t t = state_[1] << 9;

	state_[2] ^= state_[0];
	state_[3] ^= state_[1];
	state_[1] ^= state_[2];
	state_[0] ^= state_[3];

	state_[2] ^= t;

	state_[3] = rotl(state_[3], 11);

	return result;
  }

  [[nodiscard]] static constexpr uint64_t min()  {
    return std::numeric_limits<result_type>::min();
  }

  [[nodiscard]] static constexpr uint64_t max()  {
    return std::numeric_limits<result_type>::max();
  }

 private:
  [[gnu::always_inline]] static constexpr uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
  }

  uint32_t state_[4] = {};
};

extern std::minstd_rand random;
}
