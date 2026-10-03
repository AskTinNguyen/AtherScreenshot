#pragma once
#include "common.h"

namespace ather {

// Ruler: drag to measure (W × H, length, angle; Shift snaps), drag again for a new measurement,
// C copies the measurement, Esc / Enter / right-click closes.
enum class OverlayMode { Region, ColorPick, Ruler };

struct OverlayOptions {
    bool crosshair = true;
    bool magnifier = true;
};

struct OverlayResult {
    bool ok = false;
    RECT rect{};        // Region: selection in screen coordinates
    HWND window = nullptr;  // Region: the window that was clicked (snap), if any
    COLORREF color{};  // ColorPick
};

// Fullscreen selector over a frozen screenshot of `virt` (screen coords).
// Click = snap to window/monitor under cursor, drag = free region, Space = monitor,
// Ctrl+A = everything, arrows nudge cursor (Shift = 10px), Esc / right-click = cancel.
bool ShowOverlay(OverlayMode mode, BitmapPtr shot, const RECT& virt, const OverlayOptions& opt,
                 std::function<void(const OverlayResult&)> done);
bool OverlayActive();

}  // namespace ather
