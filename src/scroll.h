#pragma once
#include "common.h"

namespace ather {

// Captures `region` repeatedly while scrolling the window under it with the mouse wheel, and stitches
// the frames into one tall image. Sticky headers/footers are detected and kept once. Stops at the end
// of the content, on Esc, or after `maxFrames`. `done` runs on the UI thread.
void StartScrollingCapture(const RECT& region, int delayMs, int maxFrames,
                           std::function<void(BitmapPtr image, int frames, std::wstring error)> done);
bool ScrollingCaptureActive();

}  // namespace ather
