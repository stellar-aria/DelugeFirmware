// Copyright 2023 Katherine Whitlock, Skyline Synths LLC
// Licensed under GPLv3

#pragma once
#include "RZA1/cpu_specific.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gfx {
// Stack-safe, high-speed representation of a Monochrome OLED display
class Canvas {
public:
	using pixel_t = bool;         // how a pixel is exposed to others
	using pixel_repr_t = uint8_t; // how pixels are represented, internally (as a block)

	constexpr static size_t width = OLED_MAIN_WIDTH_PIXELS;
	constexpr static size_t height = OLED_MAIN_HEIGHT_PIXELS;
	constexpr static size_t pixel_repr_bits = std::numeric_limits<pixel_repr_t>::digits;
	constexpr static size_t length = width * (height / pixel_repr_bits);

	Canvas() = default;
	~Canvas() = default;

	// Disallow copy and assignment
	Canvas(const Canvas&) = delete;
	const Canvas& operator=(const Canvas&) = delete;

	void Clear() { buffer_.fill(0); }

	void SetPixel(size_t x, size_t y, bool on);

	bool GetPixel(size_t x, size_t y);

	void DrawFastHLine(size_t x, size_t y, size_t w, bool on);

	void DrawFastVLine(size_t x, size_t y, size_t h, bool on);

	void Write();

private:
	std::array<pixel_repr_t, length> buffer_;
	bool needs_update_ = false;
};
} // namespace gfx
