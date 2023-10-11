#include "canvas.hpp"
#include "deluge/hid/hid_sysex.h"

extern "C" {
#include "deluge/drivers/oled/oled.h"
}

namespace gfx {
void Canvas::SetPixel(size_t x, size_t y, bool on) {
	if (x >= width || y >= height) {
		return;
	}

	// column-based wrapping iff height is greater than our repr's bitwidth
	if constexpr (height > pixel_repr_bits) {
		x = x + (y / pixel_repr_bits) * width;
		y = y % pixel_repr_bits;
	}

	// Draw in the right color
	if (on) {
		buffer_[x] &= ~(1 << y);
	}
	else {
		buffer_[x] |= 1 << y;
	}
}

bool Canvas::GetPixel(size_t x, size_t y) {
	if (x >= width || y >= height) {
		return false;
	}

	if constexpr (height > pixel_repr_bits) {
		x = x + (y / pixel_repr_bits) * width;
		y = y % pixel_repr_bits;
	}

	return (buffer_[x] & (1 << y)) != 0;
}

void Canvas::DrawFastHLine(size_t x, size_t y, size_t w, bool on) {
	pixel_repr_t mask = 1 << (y & 7); // 7 because uint8_t
	pixel_repr_t* const begin = &buffer_[(y >> 3) + x];
	for (pixel_repr_t* it = begin; it <= begin + w; ++it) {
		if (on) {
			*it |= mask;
		}
		else {
			*it &= ~mask;
		}
	}
}

void Canvas::DrawFastVLine(size_t x, size_t y, size_t h, bool on) {
	size_t first_row_y = y >> 3;
	size_t last_row_y = (y + h) >> 3;

	pixel_repr_t first_row_mask = (0xFF << (y & 7));
	pixel_repr_t last_row_mask = (0xFF >> (7 - ((y + h) & 7)));

	// Single byte
	if (first_row_y == last_row_y) {
		pixel_repr_t mask = first_row_mask & last_row_mask;
		buffer_[first_row_y + x] |= mask;
		return;
	}

	/**
		 * Essentially here we're aiming for something like
		 *
		 * . x x x x
		 * . x x x x
		 * . x x x x
		 * x x x x .
		 * x x x x .
		 *
		 * Where "x" is newly set bits and '.' is unset bits
		 */

	// First row
	buffer_[first_row_y + x] |= first_row_mask;

	// Intermediate rows
	for (pixel_repr_t* it = &buffer_[first_row_y + x + width]; it < &buffer_[last_row_y + x]; it += width) {
		*it = ~pixel_repr_t{0};
	}

	// Last row
	buffer_[last_row_y + x] |= last_row_mask;
}

void Canvas::Write() {
	if (needs_update_) {
		enqueueSPITransfer(0, buffer_.data());
		HIDSysex::sendDisplayIfChanged();
	}
}
} // namespace gfx
