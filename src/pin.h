#pragma once
#include "common.h"

namespace ather {

// Floating always-on-top image. `at` = screen rect it came from (pins exactly in place), or null to center on cursor.
// Drag to move, wheel to zoom, Ctrl+wheel for opacity, double-click / Esc / middle-click to close.
void PinImage(BitmapPtr img, const RECT* at);
void CloseAllPins();
int PinCount();

}  // namespace ather
