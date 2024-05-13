#include "line_segment.hpp"
#include "oled_gfx/canvas.hpp"
#include <cmath>

namespace gfx {

void drawLineHigh(Canvas& canvas, Point start, Point end, bool on) {
	int dx = end.x - start.x;
	int dy = end.y - start.y;
	int xi = 1;
	if (dx < 0) {
		xi = -1;
		dx = -dx;
	}
	int D = (2 * dy) - dx;
	int x = start.x;

	for (int y = start.x; y < end.y; ++y) {
		canvas.setPixel({x, y}, on);
		if (D > 0) {
			x = x + xi;
			D = D + (2 * (dy - dx));
		}
		else {
			D = D + 2 * dx;
		}
	}
}

void drawLineLow(Canvas& canvas, Point start, Point end, bool on) {
	int dx = end.x - start.x;
	int dy = end.y - start.y;
	int yi = 1;
	if (dy < 0) {
		yi = -1;
		dy = -dy;
	}
	int D = (2 * dy) - dx;
	int y = start.y;

	for (int x = start.x; x < end.x; ++x) {
		canvas.setPixel({x, y}, on);
		if (D > 0) {
			y = y + yi;
			D = D + (2 * (dy - dx));
		}
		else {
			D = D + 2 * dy;
		}
	}
}

void LineSegment::drawTo(Canvas& canvas, bool on) const {
	// Fast vertical
	if (start.x == end.x) {
		if (start.y >= end.y) {
			canvas.drawVerticalLine(end, start.y - end.y, on);
		}
		else {
			canvas.drawVerticalLine(start, end.y - start.y, on);
		}
	}

	// Fast horizontal
	else if (start.y == end.y) {
		if (start.x >= end.x) {
			canvas.drawHorizontalLine(end, start.x - end.x, on);
		}
		else {
			canvas.drawHorizontalLine(start, end.x - start.x, on);
		}
	}

	// Generic drawing algorithm
	else if (std::abs(end.y - start.y) < std::abs(end.x - start.x)) {
		if (start.x > end.x) {
			drawLineLow(canvas, end, start, on);
		}
		else {
			drawLineLow(canvas, start, end, on);
		}
	}
	else {
		if (start.y > end.y) {
			drawLineHigh(canvas, end, start, on);
		}
		else {
			drawLineHigh(canvas, start, end, on);
		}
	}
}
} // namespace gfx
