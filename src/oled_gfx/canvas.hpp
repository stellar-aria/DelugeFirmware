// Copyright 2023 Katherine Whitlock, Skyline Synths LLC
// Licensed under GPLv3

#pragma once
#include "RZA1/cpu_specific.h"
#include "primitives/point.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace gfx {
class Drawable;
// Stack-safe, high-speed representation of a Monochrome OLED display
class Canvas {
public:
	using pixel_t = bool;         // how a pixel is exposed to others
	using pixel_repr_t = uint8_t; // how pixels are represented, internally (as a block)

	constexpr static size_t width = OLED_MAIN_WIDTH_PIXELS;
	constexpr static size_t height = OLED_MAIN_HEIGHT_PIXELS;
	constexpr static size_t pixel_repr_bits = std::numeric_limits<pixel_repr_t>::digits;
	constexpr static size_t length = width * (height / pixel_repr_bits);

	constexpr Canvas() = default;
	~Canvas() = default;

	Canvas& operator=(const Canvas& o) {
		std::memcpy(buffer_.data(), o.buffer_.data(), length * sizeof(pixel_repr_t));
		needs_update_ = true;
		return *this;
	};

	/**
	 * @brief Clears the pixel buffer
	 *
	 */
	void clear() {
		buffer_.fill(0);
		dirty();
	}

	/**
	 * @brief Turn on/off a pixel
	 *
	 * @param p The pixel's position
	 * @param on Whether it should be on or off
	 */
	void setPixel(Point p, bool on);

	/**
	 * @brief Get the state of a pixel
	 *
	 * @param p The pixel's position
	 * @return true The pixel is on
	 * @return false The pixel is off
	 */
	bool getPixel(Point p);

	/**
	 * @brief Draw a horizontal line (fast, left to right)
	 *
	 * @param p The starting point of the line
	 * @param w The width of the line
	 * @param on The pixel state to set
	 */
	void drawHorizontalLine(Point p, size_t w, bool on);

	/**
	 * @brief Draw a horizontal line (fast, top to bottom)
	 *
	 * @param p The starting point of the line
	 * @param h The height of the line
	 * @param on The pixel state to set
	 *
	 * @note Will wrap and overflow to next column if height is greater than remaining screen height
	 */
	void drawVerticalLine(Point p, size_t h, bool on);

	/**
	 * @brief Draw an object on the canvas
	 *
	 * @param drawable The object to draw
	 * @param on The pixel state for the object
	 */
	void draw(const Drawable& drawable, bool on = true);

	/**
	 * @brief Write the Canvas to the SPI device
	 * @note This will not do anything if the display does not need updating, and is safe to call every refresh
	 */
	void write();

	/**
	 * @brief Checks if the canvas needs to be output.
	 *
	 * @return true
	 * @return false
	 */
	bool needs_update() { return needs_update_; }

	/**
	 * @brief Mark the canvas as dirty so that it will be written the next time write() is called
	 *
	 */
	void dirty() { needs_update_ = true; }

private:
	std::array<pixel_repr_t, length> buffer_{0};
	bool needs_update_ = false;
};
} // namespace gfx
