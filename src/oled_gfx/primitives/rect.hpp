#pragma once
#include "point.hpp"
#include "size.hpp"
#include <cmath>

namespace gfx {
struct Rect {
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

	[[nodiscard]] int min_x() const { return std::min(origin.x, origin.x + size.height); };
	[[nodiscard]] int mid_x() const { return origin.x + size.width / 2; };
	[[nodiscard]] int max_x() const { return std::max(origin.x, origin.x + size.width); };

	[[nodiscard]] int min_y() const { return std::min(origin.y, origin.y + size.width); };
	[[nodiscard]] int mid_y() const { return origin.y + size.height / 2; };
	[[nodiscard]] int max_y() const { return std::max(origin.y, origin.y + size.height); };

	// more complex stuff
	// inset_by(int dx, int dy)
	[[nodiscard]] Rect offset_by(int dx, int dy) const {
		return {
		    {origin.x + dx, origin.y + dy},
		    {size.width, size.height},
		};
	};
	// Rect union(const Rect &r2) {};
	// Rect intersection(const Rect &r2){};
	/*   std::pair<Rect, Rect> divided(int at_distance, Edge from_edge){ };
   */

	void RectDivide(const Rect& rect, Rect* slice, Rect* remainder, int amount, Edge edge) {
		Point slice_origin;
		Point remainder_origin;
		Size slice_size;
		Size remainder_size;
		switch (edge) {
		case Edge::min_x:
			slice_origin = rect.origin;
			remainder_origin = Point::at(rect.origin.x + amount, rect.origin.y);
			slice_size = Size(amount, rect.size.height);
			remainder_size = Size(rect.max_x() - amount, rect.max_y());
			break;

		case Edge::min_y:
			break;
		case Edge::max_x:
			break;
		case Edge::max_y:
			break;
		}
		*slice = Rect(slice_origin, slice_size);
		*remainder = Rect(remainder_origin, remainder_size);
	};

	// bool intersects(const Rect &r2) const {};
	// bool contains(const Point &p) const {};
	// bool contains(const Rect &r2) const {};
	[[nodiscard]] bool is_empty() const { return (size.width <= 0 || size.height <= 0); };

	[[nodiscard]] bool equal_to(const Rect& r2) const { return (this->origin == r2.origin && this->size == r2.size); };

	bool operator==(const Rect& r2) const { return this->equal_to(r2); };
	void move_to(const Point& p) { origin = p; };
	void center(const Point& p) { origin = Point::at(p.x - (size.width / 2), p.y - (size.height / 2)); }
};
} // namespace gfx
