#include "capture.h"

#include <dwmapi.h>

namespace ather {

void DrawCursorInto(HDC dc, const RECT& area) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || ci.flags != CURSOR_SHOWING) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    int x = ci.ptScreenPos.x - (int)ii.xHotspot - area.left;
    int y = ci.ptScreenPos.y - (int)ii.yHotspot - area.top;
    DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

BitmapPtr CaptureScreen(const RECT& r, bool withCursor) {
    int w = RectW(r), h = RectH(r);
    auto bmp = Bitmap::Create(w, h);
    if (!bmp) return nullptr;
    HDC screen = GetDC(nullptr);
    {
        MemDC mem(bmp->Handle(), screen);
        BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
        if (withCursor) DrawCursorInto(mem, r);
    }
    ReleaseDC(nullptr, screen);
    GdiFlush();
    uint32_t* p = bmp->Bits();
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) p[i] |= 0xFF000000u;
    return bmp;
}

bool GetWindowFrame(HWND hwnd, RECT* out) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out, sizeof(*out))) && RectW(*out) > 0)
        return true;
    return GetWindowRect(hwnd, out) != 0;
}

namespace {
struct SnapCtx {
    std::vector<SnapTarget> rects;
    RECT virt;
    DWORD pid;
};

BOOL CALLBACK EnumWin(HWND h, LPARAM lp) {
    auto& c = *reinterpret_cast<SnapCtx*>(lp);
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == c.pid) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) return TRUE;
    wchar_t cls[32];
    GetClassNameW(h, cls, 32);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;
    RECT r, clipped;
    if (!GetWindowFrame(h, &r) || !IntersectRect(&clipped, &r, &c.virt)) return TRUE;
    if (RectW(clipped) < 8 || RectH(clipped) < 8) return TRUE;
    c.rects.push_back({clipped, h});
    return TRUE;
}

BOOL CALLBACK EnumMon(HMONITOR, HDC, LPRECT r, LPARAM lp) {
    reinterpret_cast<SnapCtx*>(lp)->rects.push_back({*r, nullptr});
    return TRUE;
}
}  // namespace

std::wstring WindowTitle(HWND hwnd) {
    wchar_t t[256] = {};
    if (hwnd) GetWindowTextW(hwnd, t, 256);
    return t;
}

std::wstring WindowAppName(HWND hwnd) {
    DWORD pid = 0;
    if (!hwnd || !GetWindowThreadProcessId(hwnd, &pid)) return L"";
    std::wstring name;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            name = FileNameOf(path);
            if (size_t dot = name.find_last_of(L'.'); dot != std::wstring::npos) name.erase(dot);
        }
        CloseHandle(p);
    }
    return name;
}

std::vector<SnapTarget> EnumSnapRects() {
    SnapCtx ctx{{}, VirtualScreenRect(), GetCurrentProcessId()};
    ctx.rects.reserve(64);
    EnumWindows(EnumWin, (LPARAM)&ctx);
    EnumDisplayMonitors(nullptr, nullptr, EnumMon, (LPARAM)&ctx);
    return ctx.rects;
}

}  // namespace ather
