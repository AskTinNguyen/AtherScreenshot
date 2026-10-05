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

constexpr float kBoxW = 240, kBoxH = 172;

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

// Everything below is measured from the reference photo (a white modern controller, 485 px wide there),
// scaled to the 240-unit-wide design box: 1 px ≈ 0.495 units.

// The shell: flat top, rounded shoulders, near-straight sides and round grips with a dip between them.
void BodyPath(gp::GraphicsPath& p) {
    const gp::PointF left[] = {
        {120, 6},   {100, 6},   {80, 5},    {62, 7},    // top edge
        {36, 9},    {14, 18},   {6, 42},                // round shoulder
        {0, 62},    {0, 100},   {3, 132},               // the side, widest halfway down
        {6, 160},   {28, 170},  {46, 164},              // round grip bottom
        {60, 158},  {68, 144},  {80, 134},              // up the inside of the grip
        {94, 126},  {106, 124}, {120, 124},             // bottom middle
    };
    std::vector<gp::PointF> pts(std::begin(left), std::end(left));
    for (int i = (int)std::size(left) - 2; i >= 0; --i) pts.push_back({240 - left[i].X, left[i].Y});
    p.AddBeziers(pts.data(), (INT)pts.size());
    p.CloseFigure();
}

void Glow(gp::Graphics& g, float cx, float cy, float r, gp::Color c, float strength = 1) {
    for (int i = 5; i >= 1; --i) {
        gp::SolidBrush b(gp::Color((BYTE)std::min(255.f, c.GetA() * 0.09f * strength), c.GetR(), c.GetG(), c.GetB()));
        const float rr = r + i * 1.5f;
        g.FillEllipse(&b, cx - rr, cy - rr, 2 * rr, 2 * rr);
    }
}

// A disc lit from the top left: `mid` where the light hits, `edge` toward the rim. Swapped, it reads as a dip.
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

void Ring(gp::Graphics& g, float cx, float cy, float r, float width, gp::Color c) {
    gp::Pen pen(c, width);
    g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
}

const gp::Color kOrange = Rgba(255, 160, 46), kOrangeHot = Rgba(255, 214, 150);
const gp::Color kSeam = Rgba(0, 0, 0, 40);

// A small white button with a gray rim, orange when pressed.
void WhiteButton(gp::Graphics& g, float cx, float cy, float r, bool down) {
    gp::SolidBrush shadow(Rgba(0, 0, 0, 22));
    g.FillEllipse(&shadow, cx - r + 0.4f, cy - r + 0.9f, 2 * r, 2 * r);
    if (down) Glow(g, cx, cy, r, kOrange);
    Dome(g, cx, cy, r, down ? Rgba(255, 205, 140) : Rgba(255, 255, 255), down ? kOrange : Rgba(222, 223, 227));
    Ring(g, cx, cy, r, 0.7f, kSeam);
}

// The whole controller in design units (240 × 172).
void PaintPad(gp::Graphics& g, const PadState& s) {
    const auto on = [&](WORD b) { return (s.buttons & b) != 0; };

    // Triggers sit behind the controller, out of sight; pulled, an orange tab rises behind the shoulder.
    for (int side = 0; side < 2; ++side) {
        const float v = side ? s.rt : s.lt, x = side ? 172.f : 32.f;
        if (v <= 0) continue;
        gp::GraphicsPath p;
        RoundRect(p, x, 6 - 10 * v, 36, 12, 5);
        gp::LinearGradientBrush fill(gp::PointF(0, -4), gp::PointF(0, 8), Rgba(255, 196, 120), kOrange);
        g.FillPath(&fill, &p);
        gp::Pen seam(Rgba(0, 0, 0, 50), 0.8f);
        g.DrawPath(&seam, &p);
    }
    // Bumpers: a thin white band along each shoulder, just above the shell.
    for (int side = 0; side < 2; ++side) {
        auto X = [side](float x) { return side ? 240 - x : x; };
        const bool down = on(side ? XINPUT_GAMEPAD_RIGHT_SHOULDER : XINPUT_GAMEPAD_LEFT_SHOULDER);
        gp::GraphicsPath band;
        const gp::PointF pts[] = {{X(22), 13}, {X(30), 8}, {X(42), 5}, {X(54), 4.4f}, {X(62), 4}, {X(70), 4}, {X(78), 4.6f}};
        band.AddBeziers(pts, 7);
        gp::Pen seam(Rgba(0, 0, 0, 46), 8.4f), fill(down ? kOrange : Rgba(246, 246, 248), 7);
        for (gp::Pen* pen : {&seam, &fill}) {
            pen->SetStartCap(gp::LineCapRound);
            pen->SetEndCap(gp::LineCapRound);
            g.DrawPath(pen, &band);
        }
    }
    // Shell: a soft shadow under it, white in the middle shading to light gray at the edges.
    gp::GraphicsPath body;
    BodyPath(body);
    for (int i = 3; i >= 1; --i) {  // a light shadow, so it still separates from a white video
        gp::Matrix m;
        m.Translate(0, 1.2f * i);
        std::unique_ptr<gp::GraphicsPath> sh(body.Clone());
        sh->Transform(&m);
        gp::Pen spread(Rgba(0, 0, 0, 12), 2.2f * i);
        g.DrawPath(&spread, sh.get());
    }
    {
        gp::LinearGradientBrush fill(gp::PointF(0, 6), gp::PointF(0, 170), Rgba(255, 255, 255), Rgba(238, 238, 241));
        g.FillPath(&fill, &body);
        // Soft shading just inside the edge, like the curve of the shell turning away.
        g.SetClip(&body);
        for (const auto& [w, a] : {std::pair{14.f, 9}, {9.f, 10}, {5.f, 12}, {2.4f, 16}}) {
            gp::Pen in(Rgba(150, 152, 160, (BYTE)a), w);
            in.SetLineJoin(gp::LineJoinRound);
            g.DrawPath(&in, &body);
        }
        g.ResetClip();
        gp::Pen rim(Rgba(0, 0, 0, 48), 0.9f);
        g.DrawPath(&rim, &body);
    }

    // Raised mounds around the sticks, and the D-pad's round plate.
    for (const auto& c : {gp::PointF(43, 49), gp::PointF(157.8f, 89.6f)}) {
        gp::SolidBrush shade(Rgba(0, 0, 0, 8));
        g.FillEllipse(&shade, c.X - 26.f, c.Y - 25.f, 53.f, 53.f);
        Dome(g, c.X, c.Y, 26.5f, Rgba(255, 255, 255), Rgba(240, 240, 243));
    }
    const float dpx = 82.6f, dpy = 90.6f;
    Dome(g, dpx, dpy, 28.5f, Rgba(222, 223, 227), Rgba(243, 243, 245));  // darker where the light comes from: a dip
    Ring(g, dpx, dpy, 28.5f, 0.8f, Rgba(0, 0, 0, 30));

    // The middle: View, home, Menu; two small buttons with a dot; a pill; three status lights.
    WhiteButton(g, 84.6f, 26.8f, 6.4f, on(XINPUT_GAMEPAD_BACK));
    WhiteButton(g, 154.4f, 26.8f, 6.4f, on(XINPUT_GAMEPAD_START));
    {
        gp::Pen icon(Rgba(120, 121, 128), 0.9f);
        icon.SetStartCap(gp::LineCapRound);
        icon.SetEndCap(gp::LineCapRound);
        const bool v = on(XINPUT_GAMEPAD_BACK), m = on(XINPUT_GAMEPAD_START);
        gp::Pen iconV(v ? Rgba(255, 255, 255) : Rgba(120, 121, 128), 0.9f), iconM(m ? Rgba(255, 255, 255) : Rgba(120, 121, 128), 0.9f);
        g.DrawLine(&iconV, 82.4f, 26.8f, 86.8f, 26.8f);  // ‹ with a stem
        g.DrawLine(&iconV, 82.4f, 26.8f, 84.2f, 25.f);
        g.DrawLine(&iconV, 82.4f, 26.8f, 84.2f, 28.6f);
        g.DrawLine(&iconM, 152.2f, 26.8f, 156.6f, 26.8f);  // stem with a ›
        g.DrawLine(&iconM, 156.6f, 26.8f, 154.8f, 25.f);
        g.DrawLine(&iconM, 156.6f, 26.8f, 154.8f, 28.6f);
    }
    WhiteButton(g, 119.7f, 26.8f, 7.9f, false);  // home
    {
        gp::GraphicsPath logo;
        RoundRect(logo, 117.2f, 24.3f, 5, 5, 1.2f);
        gp::Pen pen(Rgba(140, 141, 148), 0.8f);
        g.DrawPath(&pen, &logo);
        g.DrawLine(&pen, 119.7f, 24.3f, 119.7f, 29.3f);
        g.DrawLine(&pen, 117.2f, 26.8f, 122.2f, 26.8f);
        Ring(g, 119.7f, 26.8f, 5.8f, 0.6f, Rgba(0, 0, 0, 30));
    }
    WhiteButton(g, 101.4f, 47.5f, 6.0f, false);
    WhiteButton(g, 138.5f, 47.5f, 6.0f, false);
    {
        gp::Pen pen(Rgba(140, 141, 148), 0.8f);
        g.DrawArc(&pen, 99.f, 45.1f, 4.8f, 4.8f, 200, 250);  // a small circular arrow
        g.DrawLine(&pen, 136.5f, 49.5f, 140.5f, 45.5f);      // a small slash
        gp::SolidBrush dot(Rgba(70, 71, 78));
        g.FillEllipse(&dot, 118.5f, 46.3f, 2.4f, 2.4f);
    }
    {
        gp::GraphicsPath pill;
        RoundRect(pill, 112.8f, 65.4f, 13.9f, 6.9f, 3.45f);
        gp::SolidBrush shadow(Rgba(0, 0, 0, 20));
        g.FillEllipse(&shadow, 112.8f, 66.4f, 13.9f, 6.9f);
        gp::LinearGradientBrush b(gp::PointF(0, 65.4f), gp::PointF(0, 72.3f), Rgba(255, 255, 255), Rgba(224, 225, 229));
        g.FillPath(&b, &pill);
        gp::Pen seam(kSeam, 0.7f);
        g.DrawPath(&seam, &pill);
        gp::SolidBrush led(Rgba(96, 97, 104));
        for (float y : {84.7f, 90.6f, 97.f}) g.FillEllipse(&led, 118.7f, y - 1, 2.f, 2.f);
    }

    // Sticks: an orange ring fixed in the shell (glowing), a dark gap, and a black cap that tilts with the stick.
    for (int side = 0; side < 2; ++side) {
        const float cx = side ? 157.8f : kPadLeftStick[0], cy = side ? 89.6f : kPadLeftStick[1];
        const bool click = on(side ? XINPUT_GAMEPAD_RIGHT_THUMB : XINPUT_GAMEPAD_LEFT_THUMB);
        Glow(g, cx, cy, 18.6f, kOrange, click ? 2.2f : 0.8f);
        Ring(g, cx, cy, 16.6f, 4.2f, click ? Rgba(255, 238, 205) : kOrange);
        Ring(g, cx, cy, 15.2f, 1.0f, click ? Rgba(255, 255, 255) : kOrangeHot);  // the bright inner edge of the light
        gp::SolidBrush gap(Rgba(38, 38, 42));
        g.FillEllipse(&gap, cx - 14.4f, cy - 14.4f, 28.8f, 28.8f);
        const float x = cx + (side ? s.rx : s.lx) * 4.5f, y = cy - (side ? s.ry : s.ly) * 4.5f;
        gp::SolidBrush capShadow(Rgba(0, 0, 0, 70));
        g.FillEllipse(&capShadow, x - 12.9f, y - 12.2f, 25.8f, 25.8f);
        Dome(g, x, y, 12.9f, Rgba(76, 76, 80), Rgba(12, 12, 14));
        Ring(g, x, y, 9.4f, 1.0f, Rgba(0, 0, 0, 110));  // the dished top
        gp::Pen shine(Rgba(255, 255, 255, 40), 1.0f);
        g.DrawArc(&shine, x - 11.f, y - 11.f, 22.f, 22.f, 200, 70);
    }

    // D-pad: black and glossy, with arrow notches and a raised middle; the pressed arm lights orange.
    {
        const float cx = dpx, cy = dpy, a = 8, len = 22.5f;
        gp::GraphicsPath cross(gp::FillModeWinding);
        RoundRect(cross, cx - a, cy - len, 2 * a, 2 * len, 3.2f);
        RoundRect(cross, cx - len, cy - a, 2 * len, 2 * a, 3.2f);
        std::unique_ptr<gp::GraphicsPath> rim(cross.Clone());
        rim->Outline(nullptr, 0.1f);
        gp::Matrix down;
        down.Translate(0.6f, 1.4f);
        std::unique_ptr<gp::GraphicsPath> shadow(rim->Clone());
        shadow->Transform(&down);
        gp::SolidBrush sh(Rgba(0, 0, 0, 50));
        g.FillPath(&sh, shadow.get());
        gp::LinearGradientBrush black(gp::PointF(0, cy - len), gp::PointF(0, cy + len), Rgba(56, 56, 60), Rgba(16, 16, 18));
        g.FillPath(&black, rim.get());
        const struct {
            WORD b;
            float x, y, w, h;
            int dir;  // 0 up, 1 down, 2 left, 3 right
        } arms[] = {{XINPUT_GAMEPAD_DPAD_UP, cx - a, cy - len, 2 * a, len - a, 0},
                    {XINPUT_GAMEPAD_DPAD_DOWN, cx - a, cy + a, 2 * a, len - a, 1},
                    {XINPUT_GAMEPAD_DPAD_LEFT, cx - len, cy - a, len - a, 2 * a, 2},
                    {XINPUT_GAMEPAD_DPAD_RIGHT, cx + a, cy - a, len - a, 2 * a, 3}};
        for (const auto& arm : arms) {
            const bool pressed = on(arm.b);
            if (pressed) {
                gp::GraphicsPath p;
                RoundRect(p, arm.x + 0.6f, arm.y + 0.6f, arm.w - 1.2f, arm.h - 1.2f, 2.6f);
                gp::SolidBrush o(kOrange);
                g.FillPath(&o, &p);
            }
            const float mx = arm.x + arm.w / 2, my = arm.y + arm.h / 2, t = 3.6f;
            gp::PointF tri[3];
            if (arm.dir == 0) tri[0] = {mx, my - t}, tri[1] = {mx - t, my + t * 0.55f}, tri[2] = {mx + t, my + t * 0.55f};
            else if (arm.dir == 1) tri[0] = {mx, my + t}, tri[1] = {mx - t, my - t * 0.55f}, tri[2] = {mx + t, my - t * 0.55f};
            else if (arm.dir == 2) tri[0] = {mx - t, my}, tri[1] = {mx + t * 0.55f, my - t}, tri[2] = {mx + t * 0.55f, my + t};
            else tri[0] = {mx + t, my}, tri[1] = {mx - t * 0.55f, my - t}, tri[2] = {mx - t * 0.55f, my + t};
            gp::SolidBrush notch(pressed ? Rgba(255, 255, 255) : Rgba(8, 8, 10));
            g.FillPolygon(&notch, tri, 3);
            gp::Pen lip(pressed ? Rgba(255, 255, 255, 0) : Rgba(255, 255, 255, 34), 0.6f);
            g.DrawLine(&lip, tri[1], tri[2]);
        }
        gp::Pen edge(Rgba(255, 255, 255, 30), 0.7f);
        g.DrawPath(&edge, rim.get());
        Dome(g, cx, cy, 5.2f, Rgba(60, 60, 64), Rgba(14, 14, 16));
        Ring(g, cx, cy, 5.2f, 0.7f, Rgba(0, 0, 0, 160));
    }

    // A, B, X and Y: glossy black with letters in their colors; pressed, they fill with their color and glow.
    {
        gp::FontFamily family(L"Segoe UI Semibold");
        gp::Font font(&family, 10.5f, gp::FontStyleRegular, gp::UnitPixel);
        gp::StringFormat center;
        center.SetAlignment(gp::StringAlignmentCenter);
        center.SetLineAlignment(gp::StringAlignmentCenter);
        const float cx = kPadA[0], cy = kPadA[1] - 18.6f, d = 18.6f, r = 8.9f;
        const struct {
            WORD b;
            float x, y;
            const wchar_t* t;
            gp::Color c;
        } face[] = {{XINPUT_GAMEPAD_A, cx, cy + d, L"A", Rgba(46, 184, 74)},
                    {XINPUT_GAMEPAD_B, cx + d, cy, L"B", Rgba(232, 44, 56)},
                    {XINPUT_GAMEPAD_X, cx - d, cy, L"X", Rgba(42, 132, 242)},
                    {XINPUT_GAMEPAD_Y, cx, cy - d, L"Y", Rgba(244, 204, 20)}};
        for (const auto& f : face) {
            const bool pressed = on(f.b);
            gp::SolidBrush shadow(Rgba(0, 0, 0, 45));
            g.FillEllipse(&shadow, f.x - r + 0.4f, f.y - r + 1.1f, 2 * r, 2 * r);
            if (pressed) Glow(g, f.x, f.y, r, f.c, 2);
            const gp::Color hi = pressed ? Rgba((BYTE)std::min(255, f.c.GetR() + 70), (BYTE)std::min(255, f.c.GetG() + 70), (BYTE)std::min(255, f.c.GetB() + 70))
                                         : Rgba(74, 74, 80);
            Dome(g, f.x, f.y, r, hi, pressed ? f.c : Rgba(6, 6, 8));
            gp::SolidBrush gloss(Rgba(255, 255, 255, 46));
            g.FillEllipse(&gloss, f.x - r * 0.55f, f.y - r * 0.85f, r * 1.1f, r * 0.55f);
            gp::SolidBrush text(pressed ? Rgba(255, 255, 255) : f.c);
            g.DrawString(f.t, 1, &font, gp::RectF(f.x - r, f.y - r + 0.4f, 2 * r, 2 * r), &center, &text);
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
    const int w = (int)std::ceil(240 * L.u + 2 * pad) + 1, h = (int)std::ceil(kBoxH * L.u + 2 * pad) + 1;
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
    CHECK(big.x + 240 * big.u <= 1920 && big.y + kBoxH * big.u <= 1080 && big.x > 1500 && big.y > 800);
    const PadLayout tiny = GamepadLayout(300, 120, PadCorner::TopLeft, 2);  // never covers most of a small frame
    CHECK(tiny.u * 240 <= 100 && tiny.u * kBoxH <= 60.01 && tiny.x < 10 && tiny.y < 10);
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
        auto idle2 = Bitmap::Create(1500, 720);  // at the reference photo's size, for comparing
        std::fill(idle2->Bits(), idle2->Bits() + 1500 * 720, 0xFFFAFAFAu);
        PadState rest;
        rest.connected = true;
        DrawGamepad(*idle2, rest, PadCorner::TopLeft, 2.3f);
        SavePng(*idle2, std::wstring(dir) + L"/gamepad-idle.png");
    }
    const PadLayout L = GamepadLayout(640, 360, PadCorner::BottomRight, 1);
    const auto a0 = px(*idle, L.At(kPadA[0] - 5.5f, kPadA[1])), a1 = px(*pressed, L.At(kPadA[0] - 5.5f, kPadA[1]));
    CHECK(a1[1] > a1[0] + 60 && a1[1] > a1[2] + 60);  // green when pressed
    CHECK(a0[1] < 120);                               // dark when not
    const auto t0 = px(*idle, L.At(kPadLT[0], kPadLT[1])), t1 = px(*pressed, L.At(kPadLT[0], kPadLT[1]));
    CHECK(t1[0] > 220 && t1[1] > 120 && t1[1] < 200 && t1[2] < 110);  // the pulled trigger fills orange
    CHECK(t0[0] == 128 && t0[2] == 128);                               // and isn't drawn when not
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
