#include "toast.h"

#include <algorithm>

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotToast";
constexpr UINT_PTR kHideTimer = 1, kFadeTimer = 2;
constexpr int kWidth = 340, kPad = 12, kMaxThumbH = 190, kMargin = 16;
constexpr int kOpaque = 248;

struct State {
    HWND hwnd = nullptr;
    std::wstring title, body;
    HBITMAP thumb = nullptr;
    int tw = 0, th = 0;
    std::function<void()> onClick;
    int alpha = kOpaque;
    float scale = 1, fontScale = 0;
    HFONT fTitle = nullptr, fBody = nullptr;
    RECT work{};
    uint64_t token = 0;
} g;

int S(int v) { return Px(g.scale, v); }

void EnsureFonts() {
    if (g.fontScale == g.scale) return;
    if (g.fTitle) DeleteObject(g.fTitle);
    if (g.fBody) DeleteObject(g.fBody);
    g.fTitle = MakeDisplayFont(S(21));  // condensed uppercase headline, Ather style
    g.fBody = MakeFont(S(12));
    g.fontScale = g.scale;
}

int TextHeight(HFONT f, const std::wstring& t, int width, bool wrap) {
    if (t.empty()) return 0;
    HDC dc = GetDC(nullptr);
    HGDIOBJ o = SelectObject(dc, f);
    RECT r{0, 0, width, 0};
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX | (wrap ? DT_WORDBREAK : DT_SINGLELINE));
    SelectObject(dc, o);
    ReleaseDC(nullptr, dc);
    return r.bottom;
}

void FreeThumb() {
    if (g.thumb) DeleteObject(g.thumb);
    g.thumb = nullptr;
}

void BuildThumb(const BitmapPtr& img) {
    FreeThumb();
    if (!img) return;
    int maxW = S(kWidth) - 2 * S(kPad), maxH = S(kMaxThumbH);
    double sc = std::min({1.0, (double)maxW / img->Width(), (double)maxH / img->Height()});
    g.tw = std::max(1, (int)(img->Width() * sc));
    g.th = std::max(1, (int)(img->Height() * sc));
    HDC screen = GetDC(nullptr);
    g.thumb = CreateCompatibleBitmap(screen, g.tw, g.th);
    {
        MemDC d(g.thumb, screen), s(img->Handle(), screen);
        SetStretchBltMode(d, sc < 1 ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(d, 0, 0, nullptr);
        StretchBlt(d, 0, 0, g.tw, g.th, s, 0, 0, img->Width(), img->Height(), SRCCOPY);
    }
    ReleaseDC(nullptr, screen);
}

void Layout() {
    int w = S(kWidth), inner = w - 2 * S(kPad);
    int h = S(kPad);
    if (g.thumb) h += g.th + S(10);
    h += TextHeight(g.fTitle, g.title, inner, false);
    if (!g.body.empty()) h += S(3) + TextHeight(g.fBody, g.body, inner, true);
    h += S(kPad);
    SetWindowPos(g.hwnd, HWND_TOPMOST, g.work.right - S(kMargin) - w, g.work.bottom - S(kMargin) - h, w, h,
                 SWP_NOACTIVATE);
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bb);
    FillSolid(dc, rc, theme::kSurface);
    FillSolid(dc, {0, 0, std::max(2, S(3)), rc.bottom}, theme::kAccent);  // lime edge
    SetBkMode(dc, TRANSPARENT);
    int x = S(kPad), y = S(kPad), inner = rc.right - 2 * S(kPad);
    if (g.thumb) {
        int tx = (rc.right - g.tw) / 2;
        MemDC t(g.thumb, hdc);
        BitBlt(dc, tx, y, g.tw, g.th, t, 0, 0, SRCCOPY);
        FrameSolid(dc, {tx - 1, y - 1, tx + g.tw + 1, y + g.th + 1}, theme::kBorder);
        y += g.th + S(10);
    }
    HGDIOBJ oldFont = SelectObject(dc, g.fTitle);
    SetTextColor(dc, theme::kText);
    int th = TextHeight(g.fTitle, g.title, inner, false);
    RECT tr{x, y, x + inner, y + th};
    DrawTextW(dc, g.title.c_str(), -1, &tr, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    y += th + S(3);
    if (!g.body.empty()) {
        SelectObject(dc, g.fBody);
        SetTextColor(dc, theme::kMuted);
        RECT br{x, y, x + inner, rc.bottom - S(kPad) + S(2)};
        DrawTextW(dc, g.body.c_str(), -1, &br, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bb);
    DeleteDC(dc);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP: {
            auto cb = g.onClick;
            HideToast();
            if (cb) cb();
            return 0;
        }
        case WM_RBUTTONUP: HideToast(); return 0;
        case WM_TIMER:
            if (w == kHideTimer) {
                POINT pt;
                RECT wr;
                GetCursorPos(&pt);
                GetWindowRect(h, &wr);
                if (PtInRect(&wr, pt)) return 0;  // hovering: keep it up
                KillTimer(h, kHideTimer);
                SetTimer(h, kFadeTimer, 15, nullptr);
            } else if (w == kFadeTimer) {
                g.alpha -= 28;
                if (g.alpha <= 0) HideToast();
                else SetLayeredWindowAttributes(h, 0, (BYTE)g.alpha, LWA_ALPHA);
            }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureWindow() {
    if (g.hwnd) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP,
                             0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    PrepareChromeless(g.hwnd, true);
    ExcludeFromCapture(g.hwnd);
}

}  // namespace

uint64_t ShowToast(const std::wstring& title, const std::wstring& body, BitmapPtr thumb,
                   std::function<void()> onClick, int durationMs) {
    EnsureWindow();
    POINT pt;
    GetCursorPos(&pt);
    g.scale = DpiScaleAt(pt);
    g.work = MonitorRectAt(pt, true);
    EnsureFonts();
    g.title = Upper(title);
    g.body = body;
    g.onClick = std::move(onClick);
    BuildThumb(thumb);
    Layout();
    g.alpha = kOpaque;
    SetLayeredWindowAttributes(g.hwnd, 0, (BYTE)g.alpha, LWA_ALPHA);
    KillTimer(g.hwnd, kFadeTimer);
    SetTimer(g.hwnd, kHideTimer, durationMs, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
    if (IsWindowVisible(g.hwnd)) UpdateWindow(g.hwnd);
    else ShowWithoutFlash(g.hwnd, false);
    return ++g.token;
}

void UpdateToastBody(uint64_t token, const std::wstring& body) {
    if (!g.hwnd || token != g.token || !IsWindowVisible(g.hwnd)) return;
    g.body = body;
    Layout();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

bool HideToast() {
    if (!g.hwnd || !IsWindowVisible(g.hwnd)) return false;
    KillTimer(g.hwnd, kHideTimer);
    KillTimer(g.hwnd, kFadeTimer);
    ShowWindow(g.hwnd, SW_HIDE);
    FreeThumb();
    g.onClick = nullptr;
    return true;
}

}  // namespace ather
