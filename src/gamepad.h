#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "common.h"
#include <xinput.h>

namespace ather {

// A game controller drawn into a corner of recording frames, like OBS's Input Overlay: sticks, triggers,
// bumpers, D-pad, A/B/X/Y and View/Menu, lit as they are used. XInput controllers (Xbox and compatible
// pads; PlayStation pads through Steam Input or DS4Windows). Nothing is drawn while no controller is connected.

struct PadState {
    bool connected = false;
    WORD buttons = 0;              // XINPUT_GAMEPAD_* bits
    float lx = 0, ly = 0, rx = 0, ry = 0;  // sticks, -1…1, y up, dead zone removed
    float lt = 0, rt = 0;          // triggers, 0…1
};

enum class PadCorner { TopLeft, TopRight, BottomLeft, BottomRight };
PadCorner ParsePadCorner(const std::wstring& s);  // "topleft"… (any case); bottom right otherwise

// Holds every press seen between frames; recordings also keep buttons/triggers readable for a minimum time.
class PadLatch {
public:
    explicit PadLatch(DWORD minVisibleMs = 0) : minVisibleMs_(minVisibleMs) {}
    void Feed(const PadState& s);
    PadState Take();  // what to draw now; then starts over from the current state
    void DiscardHistory();  // keep live controls, discard between-frame presses and visibility holds

private:
    PadState cur_, held_;
    DWORD minVisibleMs_;
    ULONGLONG buttonUntil_[16]{};
    ULONGLONG ltUntil_ = 0, rtUntil_ = 0;
    float ltPeak_ = 0, rtPeak_ = 0;
};

// Polls connected controllers about 250 times a second and follows the most recently used one.
class GamepadPoller {
public:
    // Optional native reader lets integration tests drive the same polling/conversion path without a driver.
    using StateReader = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    explicit GamepadPoller(StateReader reader = nullptr);
    ~GamepadPoller();
    PadState Take();
    void DiscardHistory();  // used at pause/resume boundaries

private:
    void Run();
    StateReader reader_;
    std::mutex m_;
    PadLatch latch_{120};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Where the overlay goes: (x, y) is the top-left of its 240 × 172 design box, `u` pixels per design unit.
struct PadLayout {
    float x = 0, y = 0, u = 1;
    POINT At(float dx, float dy) const { return {(LONG)(x + dx * u), (LONG)(y + dy * u)}; }
};
PadLayout GamepadLayout(int frameW, int frameH, PadCorner corner, float dpi);
// Design-box positions of a few controls (used by the drawing and the tests).
constexpr float kPadA[2] = {196.7f, 67.1f}, kPadLeftStick[2] = {43, 49}, kPadLT[2] = {46, -2};

// Draws the controller onto opaque BGRA frame pixels at `opacity` (0…1). Does nothing for a disconnected pad.
void DrawGamepad(Bitmap& frame, const PadState& s, PadCorner corner, float dpi, float opacity = 1);

}  // namespace ather
