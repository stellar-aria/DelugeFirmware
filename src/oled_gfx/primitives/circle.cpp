
#include "circle.hpp"
#include "oled_gfx/canvas.hpp"
namespace gfx {

void drawCircleSegments(Point c, Point p, Canvas& canvas, bool on) {
	canvas.setPixel(Point{c.x + p.x, c.y + p.y}, on);
	canvas.setPixel(Point{c.x - p.x, c.y + p.y}, on);
	canvas.setPixel(Point{c.x + p.x, c.y - p.y}, on);
	canvas.setPixel(Point{c.x - p.x, c.y - p.y}, on);
	canvas.setPixel(Point{c.x + p.y, c.y + p.x}, on);
	canvas.setPixel(Point{c.x - p.y, c.y + p.x}, on);
	canvas.setPixel(Point{c.x + p.y, c.y - p.x}, on);
	canvas.setPixel(Point{c.x - p.y, c.y - p.x}, on);
}

// Bresenham's circle drawing algorithm
void Circle::drawTo(Canvas& canvas, bool on) const {
	int x = 0;
	int y = radius;
	int d = 3 - 2 * radius;
	while (x <= y) {
		drawCircleSegments(this->origin, Point{x, y}, canvas, on);

		if (d > 0) {
			--y;
			d = d + 4 * (x - y) + 10;
		}
		else {
			d = d + 4 * x + 6;
		}
		++x;
	}
}

} // namespace gfx
