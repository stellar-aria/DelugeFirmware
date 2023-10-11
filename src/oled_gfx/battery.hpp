#pragma once
#include "oled_gfx/layer.hpp"
#include "primitives/point.hpp"
#include <array>
#include <cstdint>

namespace gfx {
class BatteryIndicator {

	Layer<8, 4> Render() {
		return Layer<8, 4>{{
		    2, 2, 2, 2, 2, 2, 2, 0, // x x x x x x x
		    2, 1, 1, 1, 1, 1, 2, 2, // x           x x
		    2, 1, 1, 1, 1, 1, 2, 2, // x           x x
		    2, 2, 2, 2, 2, 2, 2, 0, // x x x x x x x
		}};
	}
};
} // namespace gfx
