#pragma once
#include "oled_gfx/drawable.hpp"
#include "point.hpp"

namespace gfx {
	struct LineSegment : Drawable{
		Point start;
		Point end;

		LineSegment(Point start, Point end) : start(start), end(end) {};
		virtual ~LineSegment() = default;

		void drawTo(Canvas &canvas, bool on) const override;
	};
}
