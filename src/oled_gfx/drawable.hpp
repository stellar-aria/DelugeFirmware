#pragma once

namespace gfx {

class Canvas; // forward declaration

class Drawable {
public:
	virtual void drawTo(Canvas& canvas, bool on) const = 0;
};
} // namespace gfx
