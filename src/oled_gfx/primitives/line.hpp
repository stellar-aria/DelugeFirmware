#pragma once
#include "line_segment.hpp"
#include "oled_gfx/drawable.hpp"
#include <array>
#include <cstddef>

namespace gfx {
template <size_t num_segments>
struct FiniteLine : Drawable {
	std::array<LineSegment, num_segments> segments;

	void drawTo(Canvas& canvas, bool on) const override {
		for (auto& segment : segments) {
			segment.drawTo(canvas, on);
		}
	}
};
} // namespace gfx
