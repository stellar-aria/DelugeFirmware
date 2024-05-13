#pragma once
#include <array>
#include <cstdint>

namespace gfx::icon {

constexpr std::array<uint8_t, 8> folder = {
    0x78, //       x x x x
    0x94, //     x   x     x
    0x8F, // x x x x       x
    0x81, // x             x
    0x81, // x             x
    0x81, // x             x
    0x81, // x             x
    0xFF, // x x x x x x x x
};

constexpr std::array<uint8_t, 8> wave = {
    0x00,
    0x02, //   x
    0x22, //   x       x
    0x76, //   x x   x x x
    0xFF, // x x x x x x x x
    0x76, //   x x   x x x
    0x22, //   x       x
    0x02, //   x
};

constexpr std::array<uint8_t, 8> song = {
    0x80, //              x
    0xF0, //        x x x x
    0x70, //        x x x
    0x10, //        x
    0x1C, //    x x x
    0x1E, //  x x x x
    0x1E, //  x x x x
    0x0C, //    x x
};

constexpr std::array<uint8_t, 8> synth = {
    0x00,
    0x49, // x     x     x
    0x49, // x     x     x
    0x49, // x     x     x
    0x49, // x     x     x
    0x6B, // x x   x   x x
    0x6B, // x x   x   x x
    0x6B, // x x   x   x x
};

constexpr std::array<uint8_t, 8> kit = {
    0x3C, //     x x x x
    0x42, //   x         x
    0x81, // x             x
    0xC3, // x x         x x
    0xBD, // x   x x x x   x
    0xA5, // x   x     x   x
    0x66, //   x x     x x
    0x3C, //     x x x x
};

constexpr std::array<uint8_t, 8> downArrow = {
    0x04, //     x
    0x04, //     x
    0x04, //     x
    0x04, //     x
    0x15, // x   x   x
    0x0E, //   x x x
    0x04, //     x
};

constexpr std::array<uint8_t, 5> rightArrow = {
    0x01, // x
    0x02, //   x
    0x07, // x x x
    0x02, //   x
    0x01, // x
};

constexpr std::array<uint8_t, 5> rightArrowNoFill = {
    0x01, // x
    0x02, //   x
    0x04, //     x
    0x02, //   x
    0x01, // x
};
} // namespace gfx::icon
