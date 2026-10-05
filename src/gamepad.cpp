#include "gamepad.h"

#include <objidl.h>
#include <xinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "output.h"
#include "selftest.h"

#pragma comment(lib, "xinput")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr float kBoxW = 240, kBoxH = 150;

// Stick position with a round dead zone, rescaled so the edge of the dead zone is 0 and full tilt is 1.
void Stick(SHORT x, SHORT y, int dead, float* ox, float* oy) {
    const float fx = x / 32767.f, fy = y / 32767.f;
    const float mag = std::min(1.f, std::sqrt(fx * fx + fy * fy)), d = dead / 32767.f;
    if (mag <= d) {
        *ox = *oy = 0;
        return;
    }
    const float k = (mag - d) / (1 - d) / mag;
    *ox = std::clamp(fx * k, -1.f, 1.f);
    *oy = std::clamp(fy * k, -1.f, 1.f);
}

float Trigger(BYTE v) {
    return v <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 0.f : (v - XINPUT_GAMEPAD_TRIGGER_THRESHOLD) / (255.f - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
}

PadState FromXInput(const XINPUT_GAMEPAD& g) {
    PadState s;
    s.connected = true;
    s.buttons = g.wButtons;
    Stick(g.sThumbLX, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &s.lx, &s.ly);
    Stick(g.sThumbRX, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &s.rx, &s.ry);
    s.lt = Trigger(g.bLeftTrigger);
    s.rt = Trigger(g.bRightTrigger);
    return s;
}

gp::Color Rgba(BYTE r, BYTE g, BYTE b, BYTE a = 255) { return gp::Color(a, r, g, b); }

void RoundRect(gp::GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::min({r, w / 2, h / 2});
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

}  // namespace

PadCorner ParsePadCorner(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"topleft") == 0) return PadCorner::TopLeft;
    if (_wcsicmp(s.c_str(), L"topright") == 0) return PadCorner::TopRight;
    if (_wcsicmp(s.c_str(), L"bottomleft") == 0) return PadCorner::BottomLeft;
    return PadCorner::BottomRight;
}

void PadLatch::Feed(const PadState& s) {
    cur_ = s;
    held_.connected = held_.connected || s.connected;
    held_.buttons |= s.buttons;
    held_.lt = std::max(held_.lt, s.lt);
    held_.rt = std::max(held_.rt, s.rt);
    // A stick flick between two frames shows at its widest.
    if (s.lx * s.lx + s.ly * s.ly >= held_.lx * held_.lx + held_.ly * held_.ly) held_.lx = s.lx, held_.ly = s.ly;
    if (s.rx * s.rx + s.ry * s.ry >= held_.rx * held_.rx + held_.ry * held_.ry) held_.rx = s.rx, held_.ry = s.ry;
}

PadState PadLatch::Take() {
    PadState out = held_;
    out.connected = cur_.connected;
    held_ = cur_;
    return out;
}

GamepadPoller::GamepadPoller() : thread_([this] { Run(); }) {}

GamepadPoller::~GamepadPoller() {
    stop_ = true;
    thread_.join();
}

PadState GamepadPoller::Take() {
    std::lock_guard lock(m_);
    return latch_.Take();
}

void GamepadPoller::Run() {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    int slot = -1;
    ULONGLONG lastScan = 0;
    while (!stop_) {
        PadState s;
        // Asking an empty slot is slow, so look for a controller once a second and then only read that one.
        if (slot < 0 && GetTickCount64() - lastScan >= 1000) {
            lastScan = GetTickCount64();
            for (DWORD i = 0; i < XUSER_MAX_COUNT && slot < 0; ++i) {
                XINPUT_STATE x{};
                if (XInputGetState(i, &x) == ERROR_SUCCESS) slot = (int)i;
            }
        }
        if (slot >= 0) {
            XINPUT_STATE x{};
            if (XInputGetState((DWORD)slot, &x) == ERROR_SUCCESS) s = FromXInput(x.Gamepad);
            else slot = -1;
        }
        {
            std::lock_guard lock(m_);
            latch_.Feed(s);
        }
        LARGE_INTEGER due;
        due.QuadPart = slot >= 0 ? -40000 : -1000000;  // 4 ms while a controller is on, 100 ms otherwise
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
        else Sleep(slot >= 0 ? 4 : 100);
    }
    if (timer) CloseHandle(timer);
}

PadLayout GamepadLayout(int frameW, int frameH, PadCorner corner, float dpi) {
    // About 220 px wide on a 100% screen, but never more than a third of the frame across or half of it down.
    const float w = std::min({220 * dpi, frameW * 0.33f, frameH * 0.5f * kBoxW / kBoxH});
    const float u = w / kBoxW, h = kBoxH * u;
    const float m = std::min(16 * dpi, std::min(frameW, frameH) * 0.03f);
    PadLayout l;
    l.u = u;
    l.x = corner == PadCorner::TopLeft || corner == PadCorner::BottomLeft ? m : frameW - w - m;
    l.y = corner == PadCorner::TopLeft || corner == PadCorner::TopRight ? m : frameH - h - m;
    return l;
}

void DrawGamepad(Bitmap& frame, const PadState& s, PadCorner corner, float dpi) {
    if (!s.connected || frame.Width() < 32 || frame.Height() < 32) return;
    const PadLayout L = GamepadLayout(frame.Width(), frame.Height(), corner, dpi);
    gp::Bitmap gb(frame.Width(), frame.Height(), frame.Width() * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(frame.Bits()));
    gp::Graphics g(&gb);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
    g.TranslateTransform(L.x, L.y);
    g.ScaleTransform(L.u, L.u);

    const auto on = [&](WORD b) { return (s.buttons & b) != 0; };
    const gp::Color lit = Rgba(255, 255, 255, 235), idle = Rgba(0, 0, 0, 120), edge = Rgba(255, 255, 255, 80);
    gp::Pen edgePen(edge, 1.5f);
    gp::SolidBrush idleBrush(idle), litBrush(lit);

    // Triggers: filled as far as they are pulled.
    for (int side = 0; side < 2; ++side) {
        const float v = side ? s.rt : s.lt, x = side ? 160.f : 40.f;
        gp::GraphicsPath p;
        RoundRect(p, x, 4, 40, 14, 6);
        g.FillPath(&idleBrush, &p);
        if (v > 0) {
            g.SetClip(&p);
            g.FillRectangle(&litBrush, x, 4.f, 40 * v, 14.f);
            g.ResetClip();
        }
        g.DrawPath(&edgePen, &p);
    }
    // Bumpers.
    for (int side = 0; side < 2; ++side) {
        gp::GraphicsPath p;
        RoundRect(p, side ? 154.f : 34.f, 22, 52, 11, 5.5f);
        g.FillPath(on(side ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER) ? &litBrush : &idleBrush, &p);
        g.DrawPath(&edgePen, &p);
    }
    // Body: one shape (winding fill) so the overlapping grips don't darken where they meet.
    {
        gp::GraphicsPath body(gp::FillModeWinding);
        RoundRect(body, 22, 36, 196, 74, 34);
        body.AddEllipse(24.f, 72.f, 64.f, 76.f);
        body.AddEllipse(152.f, 72.f, 64.f, 76.f);
        gp::SolidBrush fill(Rgba(30, 30, 36, 205));
        g.FillPath(&fill, &body);
        std::unique_ptr<gp::GraphicsPath> rim(body.Clone());
        rim->Outline(nullptr, 0.1f);  // just the outside edge, not where the grips overlap
        g.DrawPath(&edgePen, rim.get());
    }
    // Sticks: the cap moves with the stick and lights when clicked.
    for (int side = 0; side < 2; ++side) {
        const float cx = side ? 148.f : kPadLeftStick[0], cy = side ? 104.f : kPadLeftStick[1];
        const float dx = (side ? s.rx : s.lx) * 9, dy = -(side ? s.ry : s.ly) * 9;
        gp::SolidBrush base(Rgba(0, 0, 0, 150));
        g.FillEllipse(&base, cx - 19, cy - 19, 38.f, 38.f);
        g.DrawEllipse(&edgePen, cx - 19, cy - 19, 38.f, 38.f);
        const bool click = on(side ? XINPUT_GAMEPAD_RIGHT_THUMB : XINPUT_GAMEPAD_LEFT_THUMB);
        const bool moved = dx * dx + dy * dy > 1;
        gp::SolidBrush cap(click ? lit : moved ? Rgba(150, 150, 165, 240) : Rgba(85, 85, 96, 240));
        g.FillEllipse(&cap, cx + dx - 12, cy + dy - 12, 24.f, 24.f);
    }
    // D-pad.
    {
        const float cx = 92, cy = 104, a = 5, len = 13;
        const struct {
            WORD b;
            float x, y, w, h;
        } arms[] = {{XINPUT_GAMEPAD_DPAD_UP, cx - a, cy - a - len, 2 * a, len},
                    {XINPUT_GAMEPAD_DPAD_DOWN, cx - a, cy + a, 2 * a, len},
                    {XINPUT_GAMEPAD_DPAD_LEFT, cx - a - len, cy - a, len, 2 * a},
                    {XINPUT_GAMEPAD_DPAD_RIGHT, cx + a, cy - a, len, 2 * a}};
        gp::SolidBrush mid(Rgba(0, 0, 0, 120));
        g.FillRectangle(&mid, cx - a, cy - a, 2 * a, 2 * a);
        for (const auto& arm : arms) {
            gp::GraphicsPath p;
            RoundRect(p, arm.x, arm.y, arm.w, arm.h, 2.5f);
            g.FillPath(on(arm.b) ? &litBrush : &idleBrush, &p);
            g.DrawPath(&edgePen, &p);
        }
    }
    // View and Menu.
    for (int side = 0; side < 2; ++side) {
        gp::GraphicsPath p;
        RoundRect(p, side ? 128.f : 100.f, 59, 12, 7, 3.5f);
        g.FillPath(on(side ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_BACK) ? &litBrush : &idleBrush, &p);
        g.DrawPath(&edgePen, &p);
    }
    // A, B, X and Y in their usual colors.
    {
        gp::FontFamily family(L"Segoe UI");
        gp::Font font(&family, 10, gp::FontStyleBold, gp::UnitPixel);
        gp::StringFormat center;
        center.SetAlignment(gp::StringAlignmentCenter);
        center.SetLineAlignment(gp::StringAlignmentCenter);
        const float cx = kPadA[0], cy = kPadA[1] - 15, d = 15, r = 8.5f;
        const struct {
            WORD b;
            float x, y;
            const wchar_t* t;
            gp::Color c;
        } face[] = {{XINPUT_GAMEPAD_A, cx, cy + d, L"A", Rgba(108, 194, 74)},
                    {XINPUT_GAMEPAD_B, cx + d, cy, L"B", Rgba(226, 70, 63)},
                    {XINPUT_GAMEPAD_X, cx - d, cy, L"X", Rgba(61, 143, 224)},
                    {XINPUT_GAMEPAD_Y, cx, cy - d, L"Y", Rgba(242, 193, 46)}};
        for (const auto& f : face) {
            const bool down = on(f.b);
            gp::SolidBrush fill(down ? f.c : idle);
            g.FillEllipse(&fill, f.x - r, f.y - r, 2 * r, 2 * r);
            gp::Pen ring(down ? lit : gp::Color(150, f.c.GetR(), f.c.GetG(), f.c.GetB()), 1.5f);
            g.DrawEllipse(&ring, f.x - r, f.y - r, 2 * r, 2 * r);
            gp::SolidBrush text(down ? Rgba(255, 255, 255) : f.c);
            g.DrawString(f.t, 1, &font, gp::RectF(f.x - r, f.y - r + 0.5f, 2 * r, 2 * r), &center, &text);
        }
    }
}

// ---- tests ----

ATHER_TEST(gamepad_latch_keeps_a_tap_between_frames) {
    PadLatch l;
    PadState a;
    a.connected = true;
    a.buttons = XINPUT_GAMEPAD_A;
    PadState none;
    none.connected = true;
    l.Feed(a);
    l.Feed(none);  // released before the frame was drawn
    CHECK((l.Take().buttons & XINPUT_GAMEPAD_A) != 0);
    CHECK(l.Take().buttons == 0);  // shown once, then gone
    PadState t = none;
    t.lt = 0.8f;
    l.Feed(t);
    l.Feed(none);
    CHECK_NEAR(l.Take().lt, 0.8, 1e-6);
    l.Feed(PadState{});  // unplugged
    CHECK(!l.Take().connected);
}

ATHER_TEST(gamepad_stick_dead_zone_and_layout) {
    float x, y;
    Stick(3000, -3000, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &x, &y);
    CHECK(x == 0 && y == 0);
    Stick(32767, 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &x, &y);
    CHECK_NEAR(x, 1, 1e-4);
    CHECK_EQ(ParsePadCorner(L"TopLeft"), PadCorner::TopLeft);
    CHECK_EQ(ParsePadCorner(L"nonsense"), PadCorner::BottomRight);
    const PadLayout big = GamepadLayout(1920, 1080, PadCorner::BottomRight, 1);
    CHECK_NEAR(big.u * 240, 220, 0.01);
    CHECK(big.x + 240 * big.u <= 1920 && big.y + 150 * big.u <= 1080 && big.x > 1500 && big.y > 800);
    const PadLayout tiny = GamepadLayout(300, 120, PadCorner::TopLeft, 2);  // never covers most of a small frame
    CHECK(tiny.u * 240 <= 100 && tiny.u * 150 <= 60.01 && tiny.x < 10 && tiny.y < 10);
}

ATHER_TEST(gamepad_draws_pressed_buttons_lit) {
    auto px = [](Bitmap& b, POINT p) {
        const uint32_t c = b.Bits()[(size_t)p.y * b.Width() + p.x];
        return std::array<int, 3>{(int)(c >> 16 & 255), (int)(c >> 8 & 255), (int)(c & 255)};
    };
    auto frame = [] {
        auto b = Bitmap::Create(640, 360);
        std::fill(b->Bits(), b->Bits() + 640 * 360, 0xFF808080u);
        return b;
    };
    PadState s;
    auto off = frame();
    DrawGamepad(*off, s, PadCorner::BottomRight, 1);  // nothing connected: untouched
    CHECK(std::all_of(off->Bits(), off->Bits() + 640 * 360, [](uint32_t c) { return c == 0xFF808080u; }));
    s.connected = true;
    auto idle = frame();
    DrawGamepad(*idle, s, PadCorner::BottomRight, 1);
    s.buttons = XINPUT_GAMEPAD_A;
    s.lt = 1;
    auto pressed = frame();
    DrawGamepad(*pressed, s, PadCorner::BottomRight, 1);
    wchar_t dir[MAX_PATH];  // for eyeballing: run the tests with ATHER_SNAPSHOTS set to a folder
    if (GetEnvironmentVariableW(L"ATHER_SNAPSHOTS", dir, MAX_PATH)) SavePng(*pressed, std::wstring(dir) + L"/gamepad.png");
    const PadLayout L = GamepadLayout(640, 360, PadCorner::BottomRight, 1);
    const auto a0 = px(*idle, L.At(kPadA[0] - 5.5f, kPadA[1])), a1 = px(*pressed, L.At(kPadA[0] - 5.5f, kPadA[1]));
    CHECK(a1[1] > a1[0] + 60 && a1[1] > a1[2] + 60);  // green when pressed
    CHECK(a0[1] < 120);                               // dark when not
    const auto t0 = px(*idle, L.At(kPadLT[0], kPadLT[1])), t1 = px(*pressed, L.At(kPadLT[0], kPadLT[1]));
    CHECK(t1[0] > 220 && t0[0] < 120);  // the pulled trigger fills white
}

}  // namespace ather
