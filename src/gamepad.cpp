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

namespace {

// The controller's outline in the 240 × 150 design box: shoulders, grips and the dip between them.
void BodyPath(gp::GraphicsPath& p) {
    // Left half from the top middle round to the bottom middle, then the same mirrored.
    const gp::PointF left[] = {
        {120, 35}, {100, 35}, {80, 29}, {62, 29},     // top edge to the left shoulder
        {44, 29}, {28, 40}, {22, 60},                 // shoulder
        {16, 82}, {16, 116}, {28, 134},               // outer side down the grip
        {36, 146}, {54, 148}, {64, 138},              // round grip bottom
        {74, 128}, {80, 113}, {96, 110},              // up the inside of the grip
        {106, 108}, {114, 108}, {120, 108},           // bottom middle
    };
    std::vector<gp::PointF> pts(std::begin(left), std::end(left));
    for (int i = (int)std::size(left) - 2; i >= 0; --i) pts.push_back({240 - left[i].X, left[i].Y});
    p.AddBeziers(pts.data(), (INT)pts.size());
    p.CloseFigure();
}

void Glow(gp::Graphics& g, float cx, float cy, float r, gp::Color c) {
    for (int i = 3; i >= 1; --i) {
        gp::SolidBrush b(gp::Color((BYTE)(c.GetA() * 0.16f), c.GetR(), c.GetG(), c.GetB()));
        const float rr = r + i * 2.2f;
        g.FillEllipse(&b, cx - rr, cy - rr, 2 * rr, 2 * rr);
    }
}

// A domed disc: lighter toward the top-left, like it catches the light.
void Dome(gp::Graphics& g, float cx, float cy, float r, gp::Color mid, gp::Color edge) {
    gp::GraphicsPath p;
    p.AddEllipse(cx - r, cy - r, 2 * r, 2 * r);
    gp::PathGradientBrush b(&p);
    b.SetCenterPoint(gp::PointF(cx - r * 0.3f, cy - r * 0.35f));
    b.SetCenterColor(mid);
    INT n = 1;
    b.SetSurroundColors(&edge, &n);
    g.FillPath(&b, &p);
}

}  // namespace

void DrawGamepad(Bitmap& frame, const PadState& s, PadCorner corner, float dpi) {
    if (!s.connected || frame.Width() < 32 || frame.Height() < 32) return;
    const PadLayout L = GamepadLayout(frame.Width(), frame.Height(), corner, dpi);
    gp::Bitmap gb(frame.Width(), frame.Height(), frame.Width() * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(frame.Bits()));
    gp::Graphics g(&gb);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    g.TranslateTransform(L.x, L.y);
    g.ScaleTransform(L.u, L.u);

    const auto on = [&](WORD b) { return (s.buttons & b) != 0; };
    const gp::Color lit = Rgba(255, 255, 255, 240), part = Rgba(34, 35, 40, 245), partEdge = Rgba(255, 255, 255, 34);
    gp::Pen partPen(partEdge, 1);

    // Triggers behind the shoulders: they fill from the bottom as they're pulled.
    for (int side = 0; side < 2; ++side) {
        const float v = side ? s.rt : s.lt, x = side ? 160.f : 50.f;
        gp::GraphicsPath p;
        RoundRect(p, x, 3, 30, 26, 10);
        gp::SolidBrush base(part);
        g.FillPath(&base, &p);
        if (v > 0) {
            g.SetClip(&p);
            gp::SolidBrush fill(lit);
            g.FillRectangle(&fill, x, 3 + 26 * (1 - v), 30.f, 26 * v);
            g.ResetClip();
        }
        g.DrawPath(&partPen, &p);
    }
    // Bumpers: bands that follow the curve of each shoulder.
    for (int side = 0; side < 2; ++side) {
        auto X = [side](float x) { return side ? 240 - x : x; };
        gp::GraphicsPath band;
        const gp::PointF pts[] = {{X(38), 38}, {X(44), 29}, {X(54), 25}, {X(66), 25}, {X(76), 25}, {X(86), 26}, {X(94), 28}};
        band.AddBeziers(pts, 7);
        const bool down = on(side ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER);
        gp::Pen rim(partEdge, 11);
        rim.SetStartCap(gp::LineCapRound);
        rim.SetEndCap(gp::LineCapRound);
        g.DrawPath(&rim, &band);
        gp::Pen fill(down ? lit : part, 9);
        fill.SetStartCap(gp::LineCapRound);
        fill.SetEndCap(gp::LineCapRound);
        g.DrawPath(&fill, &band);
    }
    // Body: a soft shadow, a top-lit fill and a thin rim.
    {
        gp::GraphicsPath body;
        BodyPath(body);
        for (int i = 3; i >= 1; --i) {
            gp::Matrix m;
            m.Translate(0, 1.5f * i);
            std::unique_ptr<gp::GraphicsPath> sh(body.Clone());
            sh->Transform(&m);
            gp::Pen spread(Rgba(0, 0, 0, 22), 2.5f * i);
            gp::SolidBrush dark(Rgba(0, 0, 0, 30));
            g.FillPath(&dark, sh.get());
            g.DrawPath(&spread, sh.get());
        }
        gp::LinearGradientBrush fill(gp::PointF(0, 28), gp::PointF(0, 148), Rgba(74, 76, 86, 240), Rgba(36, 37, 43, 240));
        g.FillPath(&fill, &body);
        gp::Pen rim(Rgba(255, 255, 255, 46), 1.2f);
        g.DrawPath(&rim, &body);
    }
    // Center: a dim home button and the View / Menu buttons.
    {
        gp::SolidBrush home(Rgba(255, 255, 255, 26));
        g.FillEllipse(&home, 112.f, 44.f, 16.f, 16.f);
        gp::Pen ring(Rgba(255, 255, 255, 60), 1.2f);
        g.DrawEllipse(&ring, 112.f, 44.f, 16.f, 16.f);
        for (int side = 0; side < 2; ++side) {
            const float cx = side ? 136.f : 104.f, cy = 72;
            const bool down = on(side ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_BACK);
            if (down) Glow(g, cx, cy, 5, lit);
            gp::SolidBrush b(down ? lit : part);
            g.FillEllipse(&b, cx - 5, cy - 5, 10.f, 10.f);
            g.DrawEllipse(&partPen, cx - 5, cy - 5, 10.f, 10.f);
            gp::Pen glyph(down ? Rgba(30, 30, 34) : Rgba(255, 255, 255, 140), 1);
            if (side) {  // ≡
                for (int k = -1; k <= 1; ++k) g.DrawLine(&glyph, cx - 2.5f, cy + k * 2.f, cx + 2.5f, cy + k * 2.f);
            } else {  // ⧉
                g.DrawRectangle(&glyph, cx - 2.8f, cy - 1.2f, 3.6f, 3.6f);
                g.DrawLine(&glyph, cx - 1.2f, cy - 2.8f, cx + 2.8f, cy - 2.8f);
                g.DrawLine(&glyph, cx + 2.8f, cy - 2.8f, cx + 2.8f, cy + 1.2f);
            }
        }
    }
    // Sticks: a recessed well and a domed cap that moves with the stick and lights when clicked.
    for (int side = 0; side < 2; ++side) {
        const float cx = side ? 146.f : kPadLeftStick[0], cy = side ? 98.f : kPadLeftStick[1];
        const float dx = (side ? s.rx : s.lx) * 8, dy = -(side ? s.ry : s.ly) * 8;
        Dome(g, cx, cy, 18, Rgba(14, 14, 17, 250), Rgba(28, 29, 34, 250));
        gp::Pen well(Rgba(255, 255, 255, 30), 1);
        g.DrawEllipse(&well, cx - 18, cy - 18, 36.f, 36.f);
        const bool click = on(side ? XINPUT_GAMEPAD_RIGHT_THUMB : XINPUT_GAMEPAD_LEFT_THUMB);
        const bool moved = dx * dx + dy * dy > 1;
        if (click) Glow(g, cx + dx, cy + dy, 12, lit);
        Dome(g, cx + dx, cy + dy, 12.5f, click ? Rgba(255, 255, 255) : moved ? Rgba(132, 134, 146) : Rgba(96, 98, 108),
             click ? Rgba(205, 208, 216) : Rgba(44, 45, 52));
        gp::Pen grip(click ? Rgba(0, 0, 0, 50) : Rgba(0, 0, 0, 90), 1.2f);
        g.DrawEllipse(&grip, cx + dx - 8, cy + dy - 8, 16.f, 16.f);
    }
    // D-pad: one cross; the pressed arm lights.
    {
        const float cx = 94, cy = 98, a = 6, len = 16;
        gp::GraphicsPath cross(gp::FillModeWinding);
        RoundRect(cross, cx - a, cy - len, 2 * a, 2 * len, 2.5f);
        RoundRect(cross, cx - len, cy - a, 2 * len, 2 * a, 2.5f);
        gp::SolidBrush base(part);
        g.FillPath(&base, &cross);
        std::unique_ptr<gp::GraphicsPath> rim(cross.Clone());
        rim->Outline(nullptr, 0.1f);
        g.DrawPath(&partPen, rim.get());
        const struct {
            WORD b;
            float x, y, w, h;
        } arms[] = {{XINPUT_GAMEPAD_DPAD_UP, cx - a, cy - len, 2 * a, len - a},
                    {XINPUT_GAMEPAD_DPAD_DOWN, cx - a, cy + a, 2 * a, len - a},
                    {XINPUT_GAMEPAD_DPAD_LEFT, cx - len, cy - a, len - a, 2 * a},
                    {XINPUT_GAMEPAD_DPAD_RIGHT, cx + a, cy - a, len - a, 2 * a}};
        gp::SolidBrush litB(lit);
        for (const auto& arm : arms)
            if (on(arm.b)) {
                gp::GraphicsPath p;
                RoundRect(p, arm.x + 0.8f, arm.y + 0.8f, arm.w - 1.6f, arm.h - 1.6f, 2);
                g.FillPath(&litB, &p);
            }
        gp::SolidBrush dimple(Rgba(0, 0, 0, 70));
        g.FillEllipse(&dimple, cx - 3, cy - 3, 6.f, 6.f);
    }
    // A, B, X and Y: dark and lettered in their color, or lit in their color with a glow.
    {
        gp::FontFamily family(L"Segoe UI");
        gp::Font font(&family, 10.5f, gp::FontStyleBold, gp::UnitPixel);
        gp::StringFormat center;
        center.SetAlignment(gp::StringAlignmentCenter);
        center.SetLineAlignment(gp::StringAlignmentCenter);
        const float cx = kPadA[0], cy = kPadA[1] - 15, d = 15, r = 9;
        const struct {
            WORD b;
            float x, y;
            const wchar_t* t;
            gp::Color c;
        } face[] = {{XINPUT_GAMEPAD_A, cx, cy + d, L"A", Rgba(108, 194, 74)},
                    {XINPUT_GAMEPAD_B, cx + d, cy, L"B", Rgba(232, 72, 64)},
                    {XINPUT_GAMEPAD_X, cx - d, cy, L"X", Rgba(64, 146, 228)},
                    {XINPUT_GAMEPAD_Y, cx, cy - d, L"Y", Rgba(244, 196, 48)}};
        for (const auto& f : face) {
            const bool down = on(f.b);
            if (down) Glow(g, f.x, f.y, r, f.c);
            const gp::Color hi = down ? Rgba((BYTE)std::min(255, f.c.GetR() + 60), (BYTE)std::min(255, f.c.GetG() + 60), (BYTE)std::min(255, f.c.GetB() + 60))
                                      : Rgba(52, 54, 62);
            Dome(g, f.x, f.y, r, hi, down ? f.c : Rgba(20, 21, 25));
            gp::Pen ring(down ? Rgba(255, 255, 255, 120) : gp::Color(110, f.c.GetR(), f.c.GetG(), f.c.GetB()), 1);
            g.DrawEllipse(&ring, f.x - r, f.y - r, 2 * r, 2 * r);
            gp::SolidBrush text(down ? Rgba(255, 255, 255) : f.c);
            g.DrawString(f.t, 1, &font, gp::RectF(f.x - r, f.y - r + 0.6f, 2 * r, 2 * r), &center, &text);
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
    s.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER;
    s.lt = 1;
    s.rt = 0.4f;
    s.lx = 0.7f;
    s.ly = 0.5f;
    auto pressed = frame();
    DrawGamepad(*pressed, s, PadCorner::BottomRight, 1);
    wchar_t dir[MAX_PATH];  // for eyeballing: run the tests with ATHER_SNAPSHOTS set to a folder
    if (GetEnvironmentVariableW(L"ATHER_SNAPSHOTS", dir, MAX_PATH)) {
        SavePng(*pressed, std::wstring(dir) + L"/gamepad.png");
        auto big = Bitmap::Create(1280, 720);
        std::fill(big->Bits(), big->Bits() + 1280 * 720, 0xFF3A5A78u);
        DrawGamepad(*big, s, PadCorner::TopLeft, 3);
        SavePng(*big, std::wstring(dir) + L"/gamepad-large.png");
    }
    const PadLayout L = GamepadLayout(640, 360, PadCorner::BottomRight, 1);
    const auto a0 = px(*idle, L.At(kPadA[0] - 5.5f, kPadA[1])), a1 = px(*pressed, L.At(kPadA[0] - 5.5f, kPadA[1]));
    CHECK(a1[1] > a1[0] + 60 && a1[1] > a1[2] + 60);  // green when pressed
    CHECK(a0[1] < 120);                               // dark when not
    const auto t0 = px(*idle, L.At(kPadLT[0], kPadLT[1])), t1 = px(*pressed, L.At(kPadLT[0], kPadLT[1]));
    CHECK(t1[0] > 220 && t0[0] < 120);  // the pulled trigger fills white
}

}  // namespace ather
