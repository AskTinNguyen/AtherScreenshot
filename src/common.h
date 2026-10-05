#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ather {

inline constexpr wchar_t kAppName[] = L"AtherScreenshot";

// Ather brand: warm near-black, off-white type, one electric-lime accent.
namespace theme {
inline constexpr COLORREF kBg = RGB(14, 14, 13);          // #0E0E0D page
inline constexpr COLORREF kSurface = RGB(26, 27, 25);     // #1A1B19 cards, bars
inline constexpr COLORREF kBgRaised = RGB(37, 38, 35);    // #252623 inputs, pills
inline constexpr COLORREF kSelected = RGB(42, 45, 33);    // lime-tinted selection
inline constexpr COLORREF kBorder = RGB(50, 52, 46);      // #32342E hairlines
inline constexpr COLORREF kText = RGB(242, 239, 232);     // #F2EFE8 warm off-white
inline constexpr COLORREF kTextDim = RGB(208, 206, 199);
inline constexpr COLORREF kMuted = RGB(150, 150, 142);    // #96968E captions
inline constexpr COLORREF kAccent = RGB(212, 255, 0);     // #D4FF00 Ather lime
inline constexpr COLORREF kAccentSoft = RGB(226, 255, 110);
inline constexpr COLORREF kOnAccent = RGB(14, 14, 13);    // text/knobs on lime
inline constexpr COLORREF kLogoGray = RGB(201, 204, 205); // the "A" of the A⁵ mark
}  // namespace theme

inline constexpr wchar_t kProductName[] = L"Ather Screenshot";

// 32bpp top-down BGRA DIB section. Pixels are readable from any thread.
class Bitmap {
public:
    static std::shared_ptr<Bitmap> Create(int w, int h);
    ~Bitmap();
    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    int Width() const { return w_; }
    int Height() const { return h_; }
    HBITMAP Handle() const { return hbm_; }
    uint32_t* Bits() const { return bits_; }
    uint32_t Pixel(int x, int y) const;
    std::shared_ptr<Bitmap> Crop(const RECT& r) const;  // r in bitmap coordinates, clamped
    void Pixelate(const RECT& r, int block);            // in place, r clamped

private:
    Bitmap() = default;
    HBITMAP hbm_ = nullptr;
    uint32_t* bits_ = nullptr;
    int w_ = 0, h_ = 0;
};
using BitmapPtr = std::shared_ptr<Bitmap>;

// Memory DC with a bitmap selected for its lifetime.
class MemDC {
public:
    explicit MemDC(HBITMAP bmp, HDC ref = nullptr);
    ~MemDC();
    MemDC(const MemDC&) = delete;
    MemDC& operator=(const MemDC&) = delete;
    operator HDC() const { return dc_; }

private:
    HDC dc_;
    HGDIOBJ old_;
};

// Marshal work onto the UI thread (the main window's message loop).
constexpr UINT WM_APP_RUN = WM_APP + 100;
void SetUiWindow(HWND hwnd);
// A message-only window that runs RunOnUi work on this thread. It outlives the main window, so work posted
// while the app shuts down (save completions, library scans, the recorder finishing) still runs.
// The running exe.
std::wstring SelfExePath();
HWND StartUiDispatcher();
void StopUiDispatcher(HWND h);
void RunOnUi(std::function<void()> fn);

inline int RectW(const RECT& r) { return r.right - r.left; }
inline int RectH(const RECT& r) { return r.bottom - r.top; }
inline int Px(float scale, int v) { return (int)(v * scale + 0.5f); }

RECT VirtualScreenRect();
RECT MonitorRectAt(POINT pt, bool workArea = false);
float DpiScaleAt(POINT pt);
bool IsOwnWindow(HWND hwnd);
std::wstring FileNameOf(const std::wstring& path);
// %APPDATA%\AtherScreenshot (created), or ATHER_SUPPORT_DIR when set: tests point it at a temp folder so
// they never touch the real settings or library.
std::wstring SupportFolder();

// Body text: Segoe UI Variable (Windows 11), Segoe UI elsewhere.
HFONT MakeFont(int px, int weight = FW_NORMAL, const wchar_t* face = nullptr);
// Brand headline face: condensed bold (Bahnschrift ships with Windows 10/11). Use for UPPERCASE titles.
HFONT MakeDisplayFont(int px);
// Eyebrow labels: Bahnschrift SemiBold, drawn letter-spaced with DrawSpacedText.
HFONT MakeEyebrowFont(int px);
// DrawText with extra spacing between characters (the artifact's "Ý NGHĨA BIỂU TƯỢNG" labels).
void DrawSpacedText(HDC dc, const std::wstring& text, RECT r, UINT flags, int extraPx);
std::wstring Upper(std::wstring s);
void FillSolid(HDC dc, const RECT& r, COLORREF c);
void FrameSolid(HDC dc, const RECT& r, COLORREF c, int width = 1);
void FillRounded(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border = CLR_INVALID);

// Popup chrome: no DWM show/hide animation, optional Win11 rounded corners.
void PrepareChromeless(HWND hwnd, bool rounded);
// Shows a window cloaked, paints it, then uncloaks: no blank-frame flash.
void ShowWithoutFlash(HWND hwnd, bool activate);
void ForceForeground(HWND hwnd);
// Keeps our transient UI (palette, toasts, recording chrome) out of every screenshot and recording.
// Test builds define ATHER_TEST_CAPTURABLE_UI so that UI can itself be screenshotted.
inline void ExcludeFromCapture(HWND hwnd) {
#ifndef ATHER_TEST_CAPTURABLE_UI
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
#else
    (void)hwnd;
#endif
}

}  // namespace ather
