#include "inputviz.h"

#include <objidl.h>

#include <algorithm>
#include <deque>
#include <mutex>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr ULONGLONG kClickMs = 550, kKeyMs = 1600;

struct Click {
    POINT pt;
    ULONGLONG t;
    bool right;
};

std::mutex g_mu;
std::deque<Click> g_clicks;
std::wstring g_keys;  // what the pill shows
ULONGLONG g_keysT = 0;
bool g_typing = false;  // pill currently shows plain typed text (append) vs. a shortcut (replace)
HHOOK g_mouseHook = nullptr, g_keyHook = nullptr;

LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && (w == WM_LBUTTONDOWN || w == WM_RBUTTONDOWN || w == WM_MBUTTONDOWN)) {
        auto* m = reinterpret_cast<MSLLHOOKSTRUCT*>(l);
        std::lock_guard lock(g_mu);
        g_clicks.push_back({m->pt, GetTickCount64(), w == WM_RBUTTONDOWN});
        while (g_clicks.size() > 16) g_clicks.pop_front();
    }
    return CallNextHookEx(nullptr, code, w, l);
}

std::wstring KeyName(const KBDLLHOOKSTRUCT* k) {
    switch (k->vkCode) {
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_LEFT: return L"←";
        case VK_UP: return L"↑";
        case VK_RIGHT: return L"→";
        case VK_DOWN: return L"↓";
        case VK_SNAPSHOT: return L"PrtSc";
    }
    wchar_t name[64] = {};
    LONG lp = (LONG)(k->scanCode << 16) | ((k->flags & LLKHF_EXTENDED) ? (1 << 24) : 0);
    if (GetKeyNameTextW(lp, name, 64) <= 0) swprintf_s(name, L"0x%02X", k->vkCode);
    return name;
}

LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        const DWORD vk = k->vkCode;
        const bool isMod = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
                           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
                           vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
        if (!isMod) {
            const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = GetAsyncKeyState(VK_MENU) < 0,
                       win = GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0,
                       shift = GetAsyncKeyState(VK_SHIFT) < 0;
            const bool plain = !ctrl && !alt && !win && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'));
            std::lock_guard lock(g_mu);
            const ULONGLONG now = GetTickCount64();
            if (plain) {
                if (!g_typing || now - g_keysT > kKeyMs) g_keys.clear();
                wchar_t c = (wchar_t)vk;
                if (vk >= 'A' && vk <= 'Z' && !shift) c = (wchar_t)towlower(c);
                g_keys += c;
                if (g_keys.size() > 24) g_keys.erase(0, g_keys.size() - 24);
                g_typing = true;
            } else if (vk == VK_SPACE && g_typing && !ctrl && !alt && !win && now - g_keysT < kKeyMs) {
                g_keys += L' ';
            } else {
                std::wstring s;
                if (ctrl) s += L"Ctrl + ";
                if (alt) s += L"Alt + ";
                if (shift) s += L"Shift + ";
                if (win) s += L"Win + ";
                g_keys = s + KeyName(k);
                g_typing = false;
            }
            g_keysT = now;
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

}  // namespace

void StartInputViz(bool clicks, bool keys) {
    StopInputViz();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (clicks) g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, inst, 0);
    if (keys) g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, inst, 0);
}

void StopInputViz() {
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyHook) UnhookWindowsHookEx(g_keyHook);
    g_mouseHook = g_keyHook = nullptr;
    std::lock_guard lock(g_mu);
    g_clicks.clear();
    g_keys.clear();
}

void DrawInputViz(Bitmap& frame, POINT origin, float scale) {
    std::vector<Click> clicks;
    std::wstring keys;
    ULONGLONG keysT;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard lock(g_mu);
        for (const auto& c : g_clicks)
            if (now - c.t < kClickMs) clicks.push_back(c);
        keys = g_keys;
        keysT = g_keysT;
    }
    const bool showKeys = !keys.empty() && now - keysT < kKeyMs;
    if (clicks.empty() && !showKeys) return;

    const float dpi = DpiScaleAt({origin.x + 1, origin.y + 1}) * scale;
    gp::Bitmap gb(frame.Width(), frame.Height(), frame.Width() * 4, PixelFormat32bppRGB,
                  reinterpret_cast<BYTE*>(frame.Bits()));
    gp::Graphics g(&gb);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    for (const auto& c : clicks) {
        const float a = (now - c.t) / (float)kClickMs;
        const float r = (10 + 26 * a) * dpi;
        const float x = (c.pt.x - origin.x) * scale, y = (c.pt.y - origin.y) * scale;
        const BYTE alpha = (BYTE)(230 * (1 - a));
        const gp::Color col = c.right ? gp::Color(alpha, 255, 149, 0) : gp::Color(alpha, 124, 108, 255);
        gp::SolidBrush fill(gp::Color((BYTE)(alpha * 0.35f), col.GetR(), col.GetG(), col.GetB()));
        gp::Pen ring(col, 3 * dpi);
        g.FillEllipse(&fill, x - r, y - r, 2 * r, 2 * r);
        g.DrawEllipse(&ring, x - r, y - r, 2 * r, 2 * r);
    }
    if (showKeys) {
        const float age = (float)(now - keysT), fade = age > kKeyMs - 300 ? (kKeyMs - age) / 300.f : 1.f;
        gp::Font font(L"Segoe UI", 22 * dpi, gp::FontStyleBold, gp::UnitPixel);
        gp::RectF tb;
        g.MeasureString(keys.c_str(), (INT)keys.size(), &font, gp::PointF(0, 0), &tb);
        const float padX = 16 * dpi, padY = 8 * dpi;
        const float w = tb.Width + 2 * padX, h = tb.Height + 2 * padY;
        const float x = (frame.Width() - w) / 2, y = frame.Height() - h - 28 * dpi;
        gp::GraphicsPath path;
        const float rr = h / 2;
        path.AddArc(x, y, 2 * rr, h, 90, 180);
        path.AddArc(x + w - 2 * rr, y, 2 * rr, h, 270, 180);
        path.CloseFigure();
        gp::SolidBrush bg(gp::Color((BYTE)(215 * fade), 20, 20, 28));
        gp::SolidBrush fg(gp::Color((BYTE)(255 * fade), 255, 255, 255));
        g.FillPath(&bg, &path);
        g.DrawString(keys.c_str(), (INT)keys.size(), &font, gp::PointF(x + padX, y + padY), &fg);
    }
}

}  // namespace ather
