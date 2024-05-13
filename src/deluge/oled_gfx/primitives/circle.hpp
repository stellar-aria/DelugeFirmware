#pragma once

#include "oled_gfx/drawable.hpp"
#include "point.hpp"

namespace gfx {
struct Circle : Drawable {
	Point origin;
	int radius;

	Circle(Point origin, int radius) : origin(origin), radius(radius) {};
	virtual ~Circle() = default;

	void drawTo(Canvas &canvas, bool on) const override;
};
} // namespace gfx
