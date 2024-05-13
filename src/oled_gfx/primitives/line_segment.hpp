#pragma once
#include "oled_gfx/drawable.hpp"
#include "point.hpp"

namespace gfx {
	struct LineSegment : Drawable{
		Point start;
		Point end;

		void drawTo(Canvas &canvas, bool on) const override;
	};
}
