#pragma once
#include "common.h"

namespace ather {

// Grabs a rectangle of the desktop (screen coordinates). Alpha is forced to 255.
BitmapPtr CaptureScreen(const RECT& screenRect, bool withCursor);

// Draws the current mouse cursor into `dc`, whose origin corresponds to `area.left/top` on screen.
void DrawCursorInto(HDC dc, const RECT& area);

// Visible window frame (without the invisible resize borders) in screen coordinates.
bool GetWindowFrame(HWND hwnd, RECT* out);

struct SnapTarget {
    RECT rect;
    HWND hwnd;  // null for monitors
};
// Snap targets for region selection: visible top-level windows in z-order, then monitors.
std::vector<SnapTarget> EnumSnapRects();

// Title of a top-level window and the file name of its process (without .exe).
std::wstring WindowTitle(HWND hwnd);
std::wstring WindowAppName(HWND hwnd);

}  // namespace ather
