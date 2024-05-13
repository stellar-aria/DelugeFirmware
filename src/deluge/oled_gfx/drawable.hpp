#pragma once

namespace gfx {

class Canvas; // forward declaration

struct Drawable {
	virtual void drawTo(Canvas& canvas, bool on) const = 0;
};
} // namespace gfx
