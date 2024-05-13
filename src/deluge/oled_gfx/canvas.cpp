#include "canvas.hpp"
#include "deluge/hid/hid_sysex.h"
#include "drawable.hpp"

extern "C" {
#include "deluge/drivers/oled/oled.h"
}

namespace gfx {
void Canvas::setPixel(Point p, bool on) {
	if (p.x >= width || p.y >= height) {
		return;
	}

	// column-based wrapping iff height is greater than our repr's bitwidth
	if constexpr (height > pixel_repr_bits) {
		p.x = p.x + (p.y / pixel_repr_bits) * width;
		p.y = p.y % pixel_repr_bits;
	}

	// Draw in the right color
	if (on) {
		buffer_[p.x] |= 1 << p.y;
	}
	else {
		buffer_[p.x] &= ~(1 << p.y);
	}
	dirty();
}

bool Canvas::getPixel(Point p) {
	if (p.x >= width || p.y >= height) {
		return false;
	}

	if constexpr (height > pixel_repr_bits) {
		p.x = p.x + (p.y / pixel_repr_bits) * width;
		p.y = p.y % pixel_repr_bits;
	}

	return (buffer_[p.x] & (1 << p.y)) != 0;
}

void Canvas::drawHorizontalLine(Point p, size_t w, bool on) {
	pixel_repr_t mask = 1 << (p.y & 7); // 7 because uint8_t
	pixel_repr_t* const begin = &buffer_[p.x + (p.y / pixel_repr_bits) * width];

	for (pixel_repr_t* it = begin; it <= begin + w; ++it) {
		if (on) {
			*it |= mask;
		}
		else {
			*it &= ~mask;
		}
	}
	dirty();
}

void Canvas::drawVerticalLine(Point p, size_t h, bool on) {
	size_t first_row_offset = (p.y / pixel_repr_bits) * width;
	size_t last_row_offset = ((p.y + h) / pixel_repr_bits) * width;;

	pixel_repr_t first_row_mask = (0xFF << (p.y & 7));
	pixel_repr_t last_row_mask = (0xFF >> (7 - ((p.y + h) & 7)));

	// Single byte
	if (first_row_offset == last_row_offset) {
		pixel_repr_t mask = first_row_mask & last_row_mask;
		buffer_[first_row_offset + p.x] |= mask;
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
	buffer_[first_row_offset + p.x] |= first_row_mask;

	// Intermediate rows
	for (pixel_repr_t* it = &buffer_[first_row_offset + p.x + width]; it < &buffer_[last_row_offset + p.x]; it += width) {
		*it = ~pixel_repr_t{0};
	}

	// Last row
	buffer_[last_row_offset + p.x] |= last_row_mask;
	dirty();
}

void Canvas::write() {
	if (needs_update_) {
		enqueueSPITransfer(0, buffer_.data());
		// HIDSysex::sendDisplayIfChanged();
	}
	needs_update_ = false;
}

void Canvas::draw(const Drawable& drawable, bool on) {
	drawable.drawTo(*this, on);
}
} // namespace gfx
