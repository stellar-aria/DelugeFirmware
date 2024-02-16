#pragma once
#include "oled_gfx/canvas.hpp"
#include "oled_gfx/drawable.hpp"
#include "point.hpp"
#include "size.hpp"
#include <cmath>
#include <utility>

namespace gfx {
struct Rect : Drawable {
	Point origin;
	Size size;

public:
	enum class Edge { min_x, min_y, max_x, max_y };

	constexpr Rect() = default;
	constexpr Rect(Point origin, Size size) : origin(origin), size(size){};

	static constexpr Rect to(Point origin, Point end) { return {origin, {end.x - origin.x, end.y - origin.y}}; };

	// simple stuff
	[[nodiscard]] int height() const { return size.height; };
	[[nodiscard]] int width() const { return size.width; };

	[[nodiscard]] int minX() const { return std::min(origin.x, origin.x + size.height); };
	[[nodiscard]] int midX() const { return origin.x + size.width / 2; };
	[[nodiscard]] int maxX() const { return std::max(origin.x, origin.x + size.width); };

	[[nodiscard]] int minY() const { return std::min(origin.y, origin.y + size.width); };
	[[nodiscard]] int midY() const { return origin.y + size.height / 2; };
	[[nodiscard]] int maxY() const { return std::max(origin.y, origin.y + size.height); };

	// more complex stuff
	// inset_by(int dx, int dy)
	[[nodiscard]] Rect offsetBy(Size offset) const {
		return Rect{
		    Point{origin.x + offset.height, origin.y + offset.width},
		    Size{size.width, size.height},
		};
	};
	// Rect union(const Rect &r2) {};
	// Rect intersection(const Rect &r2){};

	[[nodiscard]] std::pair<Rect, Rect> divide(int at_distance, Edge from_edge) const {
		Point slice_origin;
		Point remainder_origin;
		Size slice_size;
		Size remainder_size;
		switch (from_edge) {
		case Edge::min_x:
			slice_origin = origin;
			remainder_origin = Point::at(origin.x + at_distance, origin.y);
			slice_size = Size(at_distance, size.height);
			remainder_size = Size(maxX() - at_distance, maxY());
			break;

		case Edge::min_y:
			break;
		case Edge::max_x:
			break;
		case Edge::max_y:
			break;
		}

		auto slice = Rect(slice_origin, slice_size);
		auto remainder = Rect(remainder_origin, remainder_size);
		return std::make_pair(slice, remainder);
	};

	// bool intersects(const Rect &r2) const {};
	// bool contains(const Point &p) const {};
	// bool contains(const Rect &r2) const {};
	[[nodiscard]] bool isEmpty() const { return (size.width <= 0 || size.height <= 0); };

	[[nodiscard]] bool equalTo(const Rect& r2) const { return (this->origin == r2.origin && this->size == r2.size); };

	bool operator==(const Rect& r2) const { return this->equalTo(r2); };
	void moveTo(const Point& p) { origin = p; };
	void center(const Point& p) { origin = Point::at(p.x - (size.width / 2), p.y - (size.height / 2)); }

	void drawTo(Canvas& canvas, bool on) const override {
		canvas.drawHorizontalLine(origin, size.width, on);
		canvas.drawVerticalLine(origin, size.height, on);
		canvas.drawHorizontalLine(Point::at(origin.x, origin.y + size.height), size.width, on);
		canvas.drawVerticalLine(Point::at(origin.x + size.width, origin.y), size.height, on);
	}
};
} // namespace gfx
