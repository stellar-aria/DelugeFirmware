#pragma once

namespace gfx {
struct Size {
  int width = 0;
  int height = 0;

  constexpr Size() = default;

  constexpr Size(int width, int height) : width(width), height(height) {};

  bool operator==(const Size &b) const = default;
};
}
