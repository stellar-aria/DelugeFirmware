#pragma once
#include <cstdint>
#include <concepts>

namespace stmlib {
template <std::integral T>
[[nodiscard]] constexpr T clip(T x) {
    return (x < -32767) ? -32767 : (x > 32767) ? 32767 : x;
}

constexpr int16_t Mix(int16_t a, int16_t b, uint16_t balance) {
  return (a * (65535 - balance) + b * balance) >> 16;
}

constexpr uint16_t Mix(uint16_t a, uint16_t b, uint16_t balance) {
  return (a * (65535 - balance) + b * balance) >> 16;
}


constexpr int16_t Interpolate824(const int16_t* table, uint32_t phase) {
	int32_t a = table[phase >> 24];
	int32_t b = table[(phase >> 24) + 1];
	return a + ((b - a) * static_cast<int32_t>((phase >> 8) & 0xffff) >> 16);
}

constexpr uint16_t Interpolate824(const uint16_t* table, uint32_t phase) {
	uint32_t a = table[phase >> 24];
	uint32_t b = table[(phase >> 24) + 1];
	return a + ((b - a) * static_cast<uint32_t>((phase >> 8) & 0xffff) >> 16);
}

constexpr int16_t Interpolate824(const uint8_t* table, uint32_t phase) {
	int32_t a = table[phase >> 24];
	int32_t b = table[(phase >> 24) + 1];
	return (a << 8) + ((b - a) * static_cast<int32_t>(phase & 0xffffff) >> 16) - 32768;
}

constexpr int16_t Interpolate1022(const int16_t* table, uint32_t phase) {
  int32_t a = table[phase >> 22];
  int32_t b = table[(phase >> 22) + 1];
  return a + ((b - a) * static_cast<int32_t>((phase >> 6) & 0xffff) >> 16);

}
}
