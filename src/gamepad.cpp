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

// The controller's outline in the 240 × 150 design box: broad shoulders, rounded grips, a dip between them.
void BodyPath(gp::GraphicsPath& p) {
    // Left half from the top middle round to the bottom middle, then the same mirrored.
    const gp::PointF left[] = {
        {120, 30}, {96, 30}, {70, 26}, {54, 28},      // top edge to the left shoulder
        {34, 30}, {20, 42}, {17, 62},                 // shoulder
        {13, 86}, {16, 118}, {30, 136},               // outer side down the grip
        {40, 149}, {60, 149}, {70, 138},              // round grip bottom
        {78, 128}, {84, 118}, {100, 116},             // up the inside of the grip
        {108, 115}, {114, 115}, {120, 115},           // bottom middle
    };
    std::vector<gp::PointF> pts(std::begin(left), std::end(left));
    for (int i = (int)std::size(left) - 2; i >= 0; --i) pts.push_back({240 - left[i].X, left[i].Y});
    p.AddBeziers(pts.data(), (INT)pts.size());
    p.CloseFigure();
}

void Glow(gp::Graphics& g, float cx, float cy, float r, gp::Color c, float strength = 1) {
    for (int i = 4; i >= 1; --i) {
        gp::SolidBrush b(gp::Color((BYTE)std::min(255.f, c.GetA() * 0.13f * strength), c.GetR(), c.GetG(), c.GetB()));
        const float rr = r + i * 1.8f;
        g.FillEllipse(&b, cx - rr, cy - rr, 2 * rr, 2 * rr);
    }
}

// A domed disc: `mid` where the light hits (up and to the left), `edge` toward the rim.
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

void Shine(gp::Graphics& g, float cx, float cy, float r) {  // the glossy highlight on a black button
    gp::SolidBrush b(Rgba(255, 255, 255, 40));
    g.FillEllipse(&b, cx - r * 0.6f, cy - r * 0.82f, r * 1.2f, r * 0.62f);
}

const gp::Color kOrange = Rgba(255, 158, 44);

// The whole controller in design units (240 × 150).
void PaintPad(gp::Graphics& g, const PadState& s) {
    const auto on = [&](WORD b) { return (s.buttons & b) != 0; };
    const gp::Color white = Rgba(246, 246, 248), whiteEdge = Rgba(0, 0, 0, 48);
    gp::Pen whitePen(whiteEdge, 1);

    // Triggers behind the shoulders: they fill orange from the top as they're pulled (the bottom is hidden).
    for (int side = 0; side < 2; ++side) {
        const float v = side ? s.rt : s.lt, x = side ? 162.f : 48.f;
        gp::GraphicsPath p;
        RoundRect(p, x, 3, 30, 26, 10);
        gp::LinearGradientBrush base(gp::PointF(0, 3), gp::PointF(0, 29), Rgba(236, 236, 239), Rgba(206, 207, 212));
        g.FillPath(&base, &p);
        if (v > 0) {
            g.SetClip(&p);
            gp::SolidBrush fill(kOrange);
            g.FillRectangle(&fill, x, 3.f, 30.f, 3 + 20 * v);
            g.ResetClip();
        }
        g.DrawPath(&whitePen, &p);
    }
    // Bumpers: bands that follow the curve of each shoulder.
    for (int side = 0; side < 2; ++side) {
        auto X = [side](float x) { return side ? 240 - x : x; };
        gp::GraphicsPath band;
        const gp::PointF pts[] = {{X(32), 38}, {X(40), 28}, {X(52), 23}, {X(66), 23}, {X(76), 23}, {X(86), 24}, {X(94), 26}};
        band.AddBeziers(pts, 7);
        const bool down = on(side ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER);
        gp::Pen rim(whiteEdge, 12);
        rim.SetStartCap(gp::LineCapRound);
        rim.SetEndCap(gp::LineCapRound);
        g.DrawPath(&rim, &band);
        gp::Pen fill(down ? kOrange : Rgba(240, 240, 243), 10);
        fill.SetStartCap(gp::LineCapRound);
        fill.SetEndCap(gp::LineCapRound);
        g.DrawPath(&fill, &band);
    }
    // Body: a soft shadow, a white top-lit shell and a fine edge (so it stands out on light videos too).
    {
        gp::GraphicsPath body;
        BodyPath(body);
        for (int i = 3; i >= 1; --i) {
            gp::Matrix m;
            m.Translate(0, 1.6f * i);
            std::unique_ptr<gp::GraphicsPath> sh(body.Clone());
            sh->Transform(&m);
            gp::Pen spread(Rgba(0, 0, 0, 24), 2.6f * i);
            gp::SolidBrush dark(Rgba(0, 0, 0, 30));
            g.FillPath(&dark, sh.get());
            g.DrawPath(&spread, sh.get());
        }
        gp::LinearGradientBrush fill(gp::PointF(0, 26), gp::PointF(0, 150), Rgba(253, 253, 254), Rgba(218, 219, 224));
        g.FillPath(&fill, &body);
        gp::Pen rim(Rgba(0, 0, 0, 60), 1.1f);
        g.DrawPath(&rim, &body);
    }
    // Middle: home, View and Menu as small white buttons; View and Menu light orange when pressed.
    {
        Dome(g, 120, 44, 7.5f, Rgba(255, 255, 255), Rgba(226, 227, 231));
        g.DrawEllipse(&whitePen, 112.5f, 36.5f, 15.f, 15.f);
        gp::Pen logo(Rgba(0, 0, 0, 70), 1.1f);
        g.DrawEllipse(&logo, 117.f, 41.f, 6.f, 6.f);
        for (int side = 0; side < 2; ++side) {
            const float cx = side ? 137.f : 103.f, cy = 62;
            const bool down = on(side ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_BACK);
            if (down) Glow(g, cx, cy, 5.5f, kOrange);
            Dome(g, cx, cy, 5.5f, down ? Rgba(255, 196, 120) : Rgba(255, 255, 255), down ? kOrange : Rgba(224, 225, 229));
            g.DrawEllipse(&whitePen, cx - 5.5f, cy - 5.5f, 11.f, 11.f);
            gp::Pen glyph(down ? Rgba(255, 255, 255) : Rgba(0, 0, 0, 110), 1);
            if (side) {  // ≡
                for (int k = -1; k <= 1; ++k) g.DrawLine(&glyph, cx - 2.5f, cy + k * 2.f, cx + 2.5f, cy + k * 2.f);
            } else {  // ⧉
                g.DrawRectangle(&glyph, cx - 2.8f, cy - 1.2f, 3.6f, 3.6f);
                g.DrawLine(&glyph, cx - 1.2f, cy - 2.8f, cx + 2.8f, cy - 2.8f);
                g.DrawLine(&glyph, cx + 2.8f, cy - 2.8f, cx + 2.8f, cy + 1.2f);
            }
        }
        gp::SolidBrush dot(Rgba(0, 0, 0, 60));
        for (int k = 0; k < 3; ++k) g.FillEllipse(&dot, 119.f, 80.f + k * 5.f, 2.f, 2.f);
    }
    // Sticks: a light recess, and a black cap in a glowing orange ring that moves with the stick.
    for (int side = 0; side < 2; ++side) {
        const float cx = side ? 148.f : kPadLeftStick[0], cy = side ? 100.f : kPadLeftStick[1];
        const float dx = (side ? s.rx : s.lx) * 7, dy = -(side ? s.ry : s.ly) * 7;
        Dome(g, cx, cy, 21, Rgba(212, 213, 218), Rgba(244, 244, 246));  // darker where the light comes from: a dip
        gp::Pen well(Rgba(0, 0, 0, 34), 1);
        g.DrawEllipse(&well, cx - 21, cy - 21, 42.f, 42.f);
        const bool click = on(side ? XINPUT_GAMEPAD_RIGHT_THUMB : XINPUT_GAMEPAD_LEFT_THUMB);
        const float x = cx + dx, y = cy + dy;
        Glow(g, x, y, 15, kOrange, click ? 2.2f : 1);
        gp::Pen ring(click ? Rgba(255, 236, 200) : kOrange, 3.4f);
        g.DrawEllipse(&ring, x - 14.5f, y - 14.5f, 29.f, 29.f);
        Dome(g, x, y, 12.8f, Rgba(82, 82, 88), Rgba(16, 16, 19));
        gp::Pen dish(Rgba(0, 0, 0, 120), 1.2f);
        g.DrawEllipse(&dish, x - 8.5f, y - 8.5f, 17.f, 17.f);
    }
    // D-pad: a black cross with arrows, on a light round plate; the pressed arm lights orange.
    {
        const float cx = 92, cy = 100, a = 6.5f, len = 17;
        Dome(g, cx, cy, 23, Rgba(214, 215, 220), Rgba(245, 245, 247));
        gp::Pen platePen(Rgba(0, 0, 0, 30), 1);
        g.DrawEllipse(&platePen, cx - 23, cy - 23, 46.f, 46.f);
        gp::GraphicsPath cross(gp::FillModeWinding);
        RoundRect(cross, cx - a, cy - len, 2 * a, 2 * len, 2.5f);
        RoundRect(cross, cx - len, cy - a, 2 * len, 2 * a, 2.5f);
        gp::LinearGradientBrush black(gp::PointF(0, cy - len), gp::PointF(0, cy + len), Rgba(52, 52, 58), Rgba(14, 14, 17));
        g.FillPath(&black, &cross);
        const struct {
            WORD b;
            float x, y, w, h;
            int dir;  // 0 up, 1 down, 2 left, 3 right
        } arms[] = {{XINPUT_GAMEPAD_DPAD_UP, cx - a, cy - len, 2 * a, len - a, 0},
                    {XINPUT_GAMEPAD_DPAD_DOWN, cx - a, cy + a, 2 * a, len - a, 1},
                    {XINPUT_GAMEPAD_DPAD_LEFT, cx - len, cy - a, len - a, 2 * a, 2},
                    {XINPUT_GAMEPAD_DPAD_RIGHT, cx + a, cy - a, len - a, 2 * a, 3}};
        for (const auto& arm : arms) {
            const bool down = on(arm.b);
            if (down) {
                gp::GraphicsPath p;
                RoundRect(p, arm.x + 0.7f, arm.y + 0.7f, arm.w - 1.4f, arm.h - 1.4f, 2);
                gp::SolidBrush o(kOrange);
                g.FillPath(&o, &p);
            }
            const float mx = arm.x + arm.w / 2, my = arm.y + arm.h / 2, t = 2.6f;
            gp::PointF tri[3];
            if (arm.dir == 0) tri[0] = {mx, my - t}, tri[1] = {mx - t, my + t * 0.6f}, tri[2] = {mx + t, my + t * 0.6f};
            else if (arm.dir == 1) tri[0] = {mx, my + t}, tri[1] = {mx - t, my - t * 0.6f}, tri[2] = {mx + t, my - t * 0.6f};
            else if (arm.dir == 2) tri[0] = {mx - t, my}, tri[1] = {mx + t * 0.6f, my - t}, tri[2] = {mx + t * 0.6f, my + t};
            else tri[0] = {mx + t, my}, tri[1] = {mx - t * 0.6f, my - t}, tri[2] = {mx - t * 0.6f, my + t};
            gp::SolidBrush arrow(down ? Rgba(255, 255, 255) : Rgba(255, 255, 255, 46));
            g.FillPolygon(&arrow, tri, 3);
        }
        gp::Pen shine(Rgba(255, 255, 255, 36), 1);
        std::unique_ptr<gp::GraphicsPath> rim(cross.Clone());
        rim->Outline(nullptr, 0.1f);
        g.DrawPath(&shine, rim.get());
        gp::SolidBrush dimple(Rgba(0, 0, 0, 90));
        g.FillEllipse(&dimple, cx - 3.5f, cy - 3.5f, 7.f, 7.f);
    }
    // A, B, X and Y: glossy black with colored letters; pressed, they fill with their color and glow.
    {
        gp::FontFamily family(L"Segoe UI");
        gp::Font font(&family, 11, gp::FontStyleBold, gp::UnitPixel);
        gp::StringFormat center;
        center.SetAlignment(gp::StringAlignmentCenter);
        center.SetLineAlignment(gp::StringAlignmentCenter);
        const float cx = kPadA[0], cy = kPadA[1] - 15, d = 15, r = 9.5f;
        const struct {
            WORD b;
            float x, y;
            const wchar_t* t;
            gp::Color c;
        } face[] = {{XINPUT_GAMEPAD_A, cx, cy + d, L"A", Rgba(64, 190, 92)},
                    {XINPUT_GAMEPAD_B, cx + d, cy, L"B", Rgba(236, 62, 66)},
                    {XINPUT_GAMEPAD_X, cx - d, cy, L"X", Rgba(48, 140, 236)},
                    {XINPUT_GAMEPAD_Y, cx, cy - d, L"Y", Rgba(250, 204, 36)}};
        for (const auto& f : face) {
            const bool down = on(f.b);
            if (down) Glow(g, f.x, f.y, r, f.c, 1.6f);
            const gp::Color hi = down ? Rgba((BYTE)std::min(255, f.c.GetR() + 70), (BYTE)std::min(255, f.c.GetG() + 70), (BYTE)std::min(255, f.c.GetB() + 70))
                                      : Rgba(70, 70, 76);
            Dome(g, f.x, f.y, r, hi, down ? f.c : Rgba(10, 10, 12));
            Shine(g, f.x, f.y, r);
            gp::SolidBrush text(down ? Rgba(255, 255, 255) : f.c);
            g.DrawString(f.t, 1, &font, gp::RectF(f.x - r, f.y - r + 0.6f, 2 * r, 2 * r), &center, &text);
        }
    }
}

}  // namespace

void DrawGamepad(Bitmap& frame, const PadState& s, PadCorner corner, float dpi, float opacity) {
    opacity = std::clamp(opacity, 0.f, 1.f);
    if (!s.connected || opacity < 0.01f || frame.Width() < 32 || frame.Height() < 32) return;
    const PadLayout L = GamepadLayout(frame.Width(), frame.Height(), corner, dpi);
    // Drawn on its own layer (room for the shadow and glows), then blended in at the chosen opacity, so
    // overlapping parts fade together instead of showing through each other.
    const float pad = 10 * L.u;
    const int ox = (int)std::floor(L.x - pad), oy = (int)std::floor(L.y - pad);
    const int w = (int)std::ceil(240 * L.u + 2 * pad) + 1, h = (int)std::ceil(150 * L.u + 2 * pad) + 1;
    auto layer = Bitmap::Create(w, h);
    if (!layer) return;
    std::fill(layer->Bits(), layer->Bits() + (size_t)w * h, 0u);
    {
        gp::Bitmap gb(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(layer->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.TranslateTransform(L.x - ox, L.y - oy);
        g.ScaleTransform(L.u, L.u);
        PaintPad(g, s);
    }
    const int k = (int)std::lround(opacity * 256);
    for (int y = std::max(0, -oy); y < h && oy + y < frame.Height(); ++y) {
        const uint32_t* src = layer->Bits() + (size_t)y * w;
        uint32_t* dst = frame.Bits() + (size_t)(oy + y) * frame.Width() + ox;
        for (int x = std::max(0, -ox); x < w && ox + x < frame.Width(); ++x) {
            const uint32_t p = src[x];
            if (!p) continue;
            const uint32_t a = ((p >> 24) * k) >> 8, inv = 255 - a, d = dst[x];
            auto ch = [&](int shift) {
                const uint32_t sc = (((p >> shift) & 255) * k) >> 8, dc = (d >> shift) & 255;
                return std::min<uint32_t>(255, sc + (dc * inv + 127) / 255) << shift;
            };
            dst[x] = 0xFF000000u | ch(16) | ch(8) | ch(0);
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
        DrawGamepad(*big, s, PadCorner::TopRight, 3, 0.6f);  // the opacity setting at 60%
        SavePng(*big, std::wstring(dir) + L"/gamepad-large.png");
    }
    const PadLayout L = GamepadLayout(640, 360, PadCorner::BottomRight, 1);
    const auto a0 = px(*idle, L.At(kPadA[0] - 5.5f, kPadA[1])), a1 = px(*pressed, L.At(kPadA[0] - 5.5f, kPadA[1]));
    CHECK(a1[1] > a1[0] + 60 && a1[1] > a1[2] + 60);  // green when pressed
    CHECK(a0[1] < 120);                               // dark when not
    const auto t0 = px(*idle, L.At(kPadLT[0], kPadLT[1])), t1 = px(*pressed, L.At(kPadLT[0], kPadLT[1]));
    CHECK(t1[0] > 220 && t1[1] > 120 && t1[1] < 200 && t1[2] < 110);  // the pulled trigger fills orange
    CHECK(t0[2] > 190);                                                // and is light when not
    // Half opacity lands halfway between the video and the controller.
    auto half = frame();
    DrawGamepad(*half, s, PadCorner::BottomRight, 1, 0.5f);
    const auto a2 = px(*half, L.At(kPadA[0] - 5.5f, kPadA[1]));
    CHECK(std::abs(a2[1] - (a1[1] + 128) / 2) <= 12);
    auto none = frame();
    DrawGamepad(*none, s, PadCorner::BottomRight, 1, 0);
    CHECK(std::all_of(none->Bits(), none->Bits() + 640 * 360, [](uint32_t c) { return c == 0xFF808080u; }));
}

}  // namespace ather
