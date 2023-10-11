#pragma once
#include "line_segment.hpp"
#include <array>
#include <cstddef>

namespace gfx {
template <size_t num_segments>
struct FiniteLine {
	std::array<LineSegment, num_segments> segments;
};
}
