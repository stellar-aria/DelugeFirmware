#pragma once

namespace gfx {
	struct Point {
		int x = 0;
		int y = 0;

		static constexpr Point at(int x, int y) { return Point{x, y}; }
		static constexpr Point origin() { return {0,0}; }

		bool operator==(const Point&) const = default;
	};
}
