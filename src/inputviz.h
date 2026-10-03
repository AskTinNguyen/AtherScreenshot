#pragma once
#include "common.h"

namespace ather {

// Click ripples and a keystroke pill burned into recording frames.
// Start/Stop install low-level hooks and must run on the UI thread; Draw is thread-safe.
void StartInputViz(bool clicks, bool keys);
void StopInputViz();
// `origin` = screen position of the frame's top-left pixel, `scale` = frame pixels per screen pixel.
void DrawInputViz(Bitmap& frame, POINT origin, float scale);

}  // namespace ather
