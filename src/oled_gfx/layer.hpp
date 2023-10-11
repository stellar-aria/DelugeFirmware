#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace gfx {
template <size_t width, size_t height>
class Layer {
public:
	enum class Pixel : uint8_t {
		Transparent = 0,
		Black = 1,
		White = 2,
	};
	Layer(std::initializer_list<uint8_t> pixels) {
		for (size_t i = 0; i < pixels.size(); ++i) {
			uint8_t val = *(pixels.begin() + i);
			pixels_[i] = Pixel{val};
		}
	}

	constexpr Pixel& at(size_t x, size_t y) { return pixels_[x + (y * width)]; }

private:
	std::array<Pixel, width * height> pixels_;
};
} // namespace gfx
