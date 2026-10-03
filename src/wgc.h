#pragma once
#include <memory>

#include "common.h"

namespace ather {

// GPU frame source built on Windows.Graphics.Capture. Either follows one window wherever it moves
// (even when covered), or captures a monitor and crops a region out of it on the GPU.
// All calls must come from the same (MTA) thread.
class WgcSource {
public:
    WgcSource();
    ~WgcSource();
    static bool Supported();
    // Window mode: output is outW x outH; a larger window is cropped, a smaller one padded with black.
    bool StartWindow(HWND hwnd, int outW, int outH, bool cursor, std::wstring* error);
    // Region mode: `region` must lie inside a single monitor (screen coordinates).
    bool StartRegion(const RECT& region, int outW, int outH, bool cursor, std::wstring* error);
    // Copies the newest frame into `dst` (outW*outH BGRA). Returns false if no new frame arrived
    // since the last call; `dst` is then left untouched (the screen didn't change).
    bool Grab(uint32_t* dst);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Size of the window as Windows.Graphics.Capture sees it (0,0 on failure).
SIZE WgcWindowSize(HWND hwnd);

}  // namespace ather
