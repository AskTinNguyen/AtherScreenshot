#include "common.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <algorithm>
#include <mutex>

namespace ather {

std::shared_ptr<Bitmap> Bitmap::Create(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) return nullptr;
    std::shared_ptr<Bitmap> b(new Bitmap());
    b->hbm_ = hbm;
    b->bits_ = static_cast<uint32_t*>(bits);
    b->w_ = w;
    b->h_ = h;
    return b;
}

Bitmap::~Bitmap() {
    if (hbm_) DeleteObject(hbm_);
}

namespace {

// Bitmaps dropped by CreateRecycled users, kept for the next one of their size (newest last).
struct Recycler {
    std::mutex mu;
    std::vector<std::pair<Bitmap*, ULONGLONG>> free;  // and when it was dropped
    size_t bytes = 0;
    static constexpr size_t kMaxBytes = 256u << 20;
    static constexpr ULONGLONG kMaxAgeMs = 3000;

    static size_t Size(const Bitmap* b) { return (size_t)b->Width() * b->Height() * 4; }
    // Frees the ones nobody took for a while; returns them so they are deleted outside the lock.
    std::vector<Bitmap*> Expire(ULONGLONG now, size_t keep) {
        std::vector<Bitmap*> out;
        while (!free.empty() && (bytes > keep || now - free.front().second > kMaxAgeMs)) {
            out.push_back(free.front().first);
            bytes -= Size(free.front().first);
            free.erase(free.begin());
        }
        return out;
    }
};

Recycler& TheRecycler() {
    static Recycler* r = new Recycler();  // never destroyed: bitmaps may come back during shutdown
    return *r;
}

}  // namespace

std::shared_ptr<Bitmap> Bitmap::CreateRecycled(int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;
    Recycler& r = TheRecycler();
    Bitmap* b = nullptr;
    std::vector<Bitmap*> old;
    {
        std::lock_guard l(r.mu);
        for (size_t i = r.free.size(); i-- > 0;)
            if (r.free[i].first->w_ == w && r.free[i].first->h_ == h) {
                b = r.free[i].first;
                r.bytes -= Recycler::Size(b);
                r.free.erase(r.free.begin() + (ptrdiff_t)i);
                break;
            }
        old = r.Expire(GetTickCount64(), Recycler::kMaxBytes);
    }
    for (Bitmap* o : old) delete o;
    if (!b) {
        auto fresh = Create(w, h);
        if (!fresh) return nullptr;
        b = new Bitmap();
        std::swap(b->hbm_, fresh->hbm_);
        std::swap(b->bits_, fresh->bits_);
        b->w_ = w;
        b->h_ = h;
    }
    return std::shared_ptr<Bitmap>(b, [](Bitmap* d) {
        Recycler& r = TheRecycler();
        std::vector<Bitmap*> old;
        {
            std::lock_guard l(r.mu);
            r.free.push_back({d, GetTickCount64()});
            r.bytes += Recycler::Size(d);
            old = r.Expire(GetTickCount64(), Recycler::kMaxBytes);
        }
        for (Bitmap* o : old) delete o;
    });
}

void Bitmap::ReleaseRecycled() {
    Recycler& r = TheRecycler();
    std::vector<Bitmap*> old;
    {
        std::lock_guard l(r.mu);
        old = r.Expire(GetTickCount64(), 0);
    }
    for (Bitmap* o : old) delete o;
}

uint32_t Bitmap::Pixel(int x, int y) const {
    x = std::clamp(x, 0, w_ - 1);
    y = std::clamp(y, 0, h_ - 1);
    return bits_[(size_t)y * w_ + x];
}

BitmapPtr Bitmap::Crop(const RECT& rc) const {
    RECT r{std::max<LONG>(rc.left, 0), std::max<LONG>(rc.top, 0), std::min<LONG>(rc.right, w_),
           std::min<LONG>(rc.bottom, h_)};
    int w = RectW(r), h = RectH(r);
    auto out = Create(w, h);
    if (!out) return nullptr;
    for (int y = 0; y < h; ++y)
        memcpy(out->bits_ + (size_t)y * w, bits_ + (size_t)(r.top + y) * w_ + r.left, (size_t)w * 4);
    return out;
}

void Bitmap::Pixelate(const RECT& rc, int block) {
    const int x0 = std::clamp<int>(rc.left, 0, w_), y0 = std::clamp<int>(rc.top, 0, h_);
    const int x1 = std::clamp<int>(rc.right, 0, w_), y1 = std::clamp<int>(rc.bottom, 0, h_);
    block = std::max(2, block);
    for (int by = y0; by < y1; by += block)
        for (int bx = x0; bx < x1; bx += block) {
            const int ex = std::min(bx + block, x1), ey = std::min(by + block, y1);
            uint64_t r = 0, g = 0, b = 0, n = 0;
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) {
                    const uint32_t p = bits_[(size_t)y * w_ + x];
                    r += (p >> 16) & 255;
                    g += (p >> 8) & 255;
                    b += p & 255;
                    ++n;
                }
            const uint32_t avg = 0xFF000000u | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
            for (int y = by; y < ey; ++y) std::fill_n(bits_ + (size_t)y * w_ + bx, ex - bx, avg);
        }
}

MemDC::MemDC(HBITMAP bmp, HDC ref) : dc_(CreateCompatibleDC(ref)), old_(SelectObject(dc_, bmp)) {}

MemDC::~MemDC() {
    SelectObject(dc_, old_);
    DeleteDC(dc_);
}

static HWND g_uiWindow = nullptr;

void SetUiWindow(HWND hwnd) { g_uiWindow = hwnd; }

static LRESULT CALLBACK DispatchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP_RUN) {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(l));
        (*fn)();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring SelfExePath() {
    wchar_t p[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
    return std::wstring(p, n);
}

HWND StartUiDispatcher() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DispatchProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AtherScreenshotDispatch";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (h) SetUiWindow(h);
    return h;
}

void StopUiDispatcher(HWND h) {
    if (!h) return;
    // Run what's still queued, then stop accepting work.
    MSG msg;
    while (PeekMessageW(&msg, h, WM_APP_RUN, WM_APP_RUN, PM_REMOVE)) DispatchMessageW(&msg);
    if (g_uiWindow == h) g_uiWindow = nullptr;
    DestroyWindow(h);
}

void RunOnUi(std::function<void()> fn) {
    auto* p = new std::function<void()>(std::move(fn));
    if (!g_uiWindow || !PostMessageW(g_uiWindow, WM_APP_RUN, 0, (LPARAM)p)) delete p;
}

RECT VirtualScreenRect() {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

RECT MonitorRectAt(POINT pt, bool workArea) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    return workArea ? mi.rcWork : mi.rcMonitor;
}

float DpiScaleAt(POINT pt) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy)))
        dx = 96;
    return dx / 96.0f;
}

bool IsOwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring SupportFolder() {
    std::wstring dir;
    const DWORD n = GetEnvironmentVariableW(L"ATHER_SUPPORT_DIR", nullptr, 0);
    if (n > 0) {
        // Set (tests): always honored, whatever its length, so a test can never fall back to the real folder.
        std::wstring over(n, L'\0');
        const DWORD got = GetEnvironmentVariableW(L"ATHER_SUPPORT_DIR", over.data(), n);
        over.resize(got < n ? got : 0);
        if (over.empty()) {  // set but unreadable: refuse rather than touch real data
            FatalAppExitW(0, L"ATHER_SUPPORT_DIR is set but can't be read.");
        }
        dir = over;
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
        CoTaskMemFree(p);
        dir += L"\\AtherScreenshot";
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

static bool FontInstalled(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
        *reinterpret_cast<bool*>(p) = true;
        return 0;
    }, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

static const wchar_t* BodyFace() {
    static const wchar_t* face = FontInstalled(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    return face;
}

HFONT MakeFont(int px, int weight, const wchar_t* face) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face ? face : BodyFace());
}

HFONT MakeDisplayFont(int px) {
    static const bool condensed = FontInstalled(L"Bahnschrift SemiBold Condensed");
    return condensed ? MakeFont(px, FW_NORMAL, L"Bahnschrift SemiBold Condensed") : MakeFont(px, FW_BOLD, L"Bahnschrift");
}

HFONT MakeEyebrowFont(int px) {
    static const bool semibold = FontInstalled(L"Bahnschrift SemiBold");
    return semibold ? MakeFont(px, FW_NORMAL, L"Bahnschrift SemiBold") : MakeFont(px, FW_BOLD);
}

void DrawSpacedText(HDC dc, const std::wstring& text, RECT r, UINT flags, int extraPx) {
    const int old = SetTextCharacterExtra(dc, extraPx);
    DrawTextW(dc, text.c_str(), (int)text.size(), &r, flags | DT_NOPREFIX);
    SetTextCharacterExtra(dc, old);
}

std::wstring Upper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), (DWORD)s.size());  // locale-aware (handles Vietnamese)
    return s;
}

void FillSolid(HDC dc, const RECT& r, COLORREF c) {
    COLORREF old = SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    SetBkColor(dc, old);
}

void FrameSolid(HDC dc, const RECT& r, COLORREF c, int w) {
    FillSolid(dc, {r.left, r.top, r.right, r.top + w}, c);
    FillSolid(dc, {r.left, r.bottom - w, r.right, r.bottom}, c);
    FillSolid(dc, {r.left, r.top + w, r.left + w, r.bottom - w}, c);
    FillSolid(dc, {r.right - w, r.top + w, r.right, r.bottom - w}, c);
}

void FillRounded(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? fill : border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void PrepareChromeless(HWND hwnd, bool rounded) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &on, sizeof(on));
    if (rounded) {
        DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
        COLORREF border = theme::kBorder;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }
}

void ForceForeground(HWND hwnd) {
    if (SetForegroundWindow(hwnd)) {
        SetFocus(hwnd);
        return;
    }
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD me = GetCurrentThreadId();
    AttachThreadInput(me, fgThread, TRUE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SetFocus(hwnd);
    AttachThreadInput(me, fgThread, FALSE);
}

void ShowWithoutFlash(HWND hwnd, bool activate) {
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindow(hwnd, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    cloak = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (activate) ForceForeground(hwnd);
}

}  // namespace ather
