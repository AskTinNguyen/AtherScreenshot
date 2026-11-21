#include "bugvoice.h"
#include "selftest.h"
#include <algorithm>
#include <format>
#include <mutex>

namespace ather {
BugVoiceLabels DescribeBugVoice(const BugVoiceStatus& s, double now) {
    BugVoiceLabels l;
    l.visible = s.phase != BugVoicePhase::Off || s.bugOpen || s.saving;
    if (!l.visible) return l;
    const std::wstring key = s.hotkey.empty() ? L"the Bug report shortcut" : s.hotkey;
    switch (s.phase) {
        case BugVoicePhase::Off: l.detail = L"Voice commands off"; break;
        case BugVoicePhase::Starting: l.detail = L"Starting microphone and speech recognition"; break;
        case BugVoicePhase::Listening: l.detail = L"Listening for “bug” / “stop”"; break;
        case BugVoicePhase::Reconnecting: l.detail = L"Reconnecting microphone"; break;
        case BugVoicePhase::Unavailable: l.detail = L"Voice unavailable"; break;
    }
    if (!s.message.empty()) l.detail += L" · " + s.message;
    if (s.phase == BugVoicePhase::Listening && !s.lastHeard.empty()
        && now - std::max(s.signalAt, s.startedAt) >= 8)
        l.detail += L" · No microphone signal; check input";
    if (s.bugOpen) {
        const int elapsed = (int)std::clamp(now - s.bugStarted, 0.0, 5999.0);
        l.title = std::format(L"BUG STARTED  {:02}:{:02}", elapsed / 60, elapsed % 60);
        l.action = L"Stop and save: " + key + (s.phase == BugVoicePhase::Listening ? L" or say “stop”" : L"");
    } else if (s.saving) {
        l.title = L"SAVING BUG…";
        l.action = L"Your bug report is being saved";
    } else {
        switch (s.phase) {
            case BugVoicePhase::Starting: l.title = L"VOICE STARTING"; break;
            case BugVoicePhase::Reconnecting: l.title = L"VOICE RECONNECTING"; break;
            case BugVoicePhase::Unavailable: l.title = L"VOICE UNAVAILABLE"; break;
            default: l.title = L"BUG VOICE READY"; break;
        }
        l.action = L"Start a bug: " + key;
    }
    l.input = (s.microphone.empty() ? L"Microphone: identifying input…" : s.microphone)
        + (s.language.empty() ? L"" : L" · " + s.language);
    if (s.phase == BugVoicePhase::Off) l.input = L"Microphone: not listening";
    if (s.phase == BugVoicePhase::Listening && s.levelAt > 0 && now - s.levelAt <= 0.8)
        l.meter = std::clamp(s.audioLevel, 0, 100);
    if (!s.lastHeard.empty()) {
        l.activity = (s.rejected ? L"Not accepted: “" : L"Heard: “") + s.lastHeard + L"”";
        if (s.rejected) l.activity += L" · Repeat command or use " + key;
    } else if (s.phase == BugVoicePhase::Listening) {
        if (s.speech && now - s.signalAt < 2) l.activity = L"Recognizing speech…";
        else if (now - std::max(s.signalAt, s.startedAt) >= 8)
            l.activity = L"No microphone signal · Check input or use " + key;
        else l.activity = L"Speak clearly; pause after each command";
    } else l.activity = L"The " + key + L" shortcut still works";
    return l;
}

namespace {
constexpr wchar_t kPanelClass[] = L"AtherBugVoicePanel";
constexpr int kPanelWidth = 420, kPanelHeight = 212;
std::mutex statusMutex;
BugVoiceStatus status;
HWND panel = nullptr;

void PaintPanel(HDC dc, const BugVoiceStatus& s, double now) {
    const auto l = DescribeBugVoice(s, now);
    FillSolid(dc, {0, 0, kPanelWidth, kPanelHeight}, theme::kSurface);
    const COLORREF accent = s.bugOpen ? RGB(255, 122, 100) : theme::kAccent;
    FillSolid(dc, {0, 0, 4, kPanelHeight}, accent);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const std::wstring& value, RECT r, int size, COLORREF color, bool wrap = false) {
        HFONT font = MakeFont(size, size >= 18 ? FW_SEMIBOLD : FW_NORMAL);
        HGDIOBJ old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &r, DT_NOPREFIX | DT_END_ELLIPSIS | (wrap ? DT_WORDBREAK : DT_SINGLELINE));
        SelectObject(dc, old);
        DeleteObject(font);
    };
    text(l.title, {16, 12, 404, 40}, 21, accent);
    text(l.detail, {16, 44, 404, 80}, 13, theme::kText, true);
    text(l.input, {16, 82, 404, 102}, 12, theme::kMuted);
    FillSolid(dc, {16, 108, 404, 114}, theme::kBgRaised);
    if (l.meter) FillSolid(dc, {16, 108, 16 + 388 * l.meter / 100, 114}, theme::kAccent);
    text(l.activity, {16, 125, 404, 171}, 13, theme::kTextDim, true);
    text(l.action, {16, 183, 404, 204}, 13, theme::kText);
}

LRESULT CALLBACK PanelProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            auto image = BugVoicePanelSnapshot(CurrentBugVoiceStatus(), GetTickCount64() / 1000.0);
            if (image) {
                MemDC source(image->Handle(), dc);
                RECT r; GetClientRect(h, &r);
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchBlt(dc, 0, 0, r.right, r.bottom, source, 0, 0, image->Width(), image->Height(), SRCCOPY);
            }
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, message, w, l);
}
}  // namespace

BugVoiceStatus CurrentBugVoiceStatus() {
    std::lock_guard lock(statusMutex);
    return status;
}
void HideBugVoicePanel() {
    if (panel) {
        KillTimer(panel, 1);
        DestroyWindow(panel);
        panel = nullptr;
    }
}
void UpdateBugVoiceStatus(const BugVoiceStatus& s) {
    {
        std::lock_guard lock(statusMutex);
        status = s;
    }
    if (!DescribeBugVoice(s, GetTickCount64() / 1000.0).visible) {
        HideBugVoicePanel();
        return;
    }
    if (!panel) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = PanelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kPanelClass;
        RegisterClassExW(&wc);
        // Layered + transparent passes mouse input to other processes as well; HTTRANSPARENT alone
        // only forwards hit testing between windows on the same thread.
        panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT, kPanelClass,
                               L"Bug voice status", WS_POPUP, 0, 0, kPanelWidth, kPanelHeight,
                               nullptr, nullptr, wc.hInstance, nullptr);
        if (!panel) return;
        SetLayeredWindowAttributes(panel, 0, 255, LWA_ALPHA);
        PrepareChromeless(panel, true);
        ExcludeFromCapture(panel);
        POINT pt; GetCursorPos(&pt);
        const RECT work = MonitorRectAt(pt, true);
        const float scale = DpiScaleAt(pt);
        const int width = Px(scale, kPanelWidth), height = Px(scale, kPanelHeight);
        SetWindowPos(panel, HWND_TOPMOST, work.right - width - Px(scale, 16), work.top + Px(scale, 16),
                     width, height, SWP_NOACTIVATE);
        SetTimer(panel, 1, 200, nullptr);
        ShowWindow(panel, SW_SHOWNOACTIVATE);
    }
    InvalidateRect(panel, nullptr, FALSE);
}
BitmapPtr BugVoicePanelSnapshot(const BugVoiceStatus& s, double now) {
    auto image = Bitmap::Create(kPanelWidth, kPanelHeight);
    if (image) {
        MemDC dc(image->Handle());
        PaintPanel(dc, s, now);
        GdiFlush();
        for (int i = 0; i < image->Width() * image->Height(); ++i) image->Bits()[i] |= 0xff000000;
    }
    return image;
}

ATHER_TEST(bug_voice_status_explains_silence_and_rejections) {
    BugVoiceStatus s;
    CHECK(!DescribeBugVoice(s, 100).visible);
    s.phase = BugVoicePhase::Listening;
    s.startedAt = 80;
    s.microphone = L"Test microphone";
    s.language = L"English (US)";
    s.hotkey = L"Ctrl+Alt+B";
    auto l = DescribeBugVoice(s, 100);
    CHECK(l.visible);
    CHECK(l.input.find(s.microphone) != std::wstring::npos);
    CHECK(l.input.find(s.language) != std::wstring::npos);
    CHECK(l.activity.find(L"No microphone signal") != std::wstring::npos);
    s.audioLevel = 70; s.levelAt = s.signalAt = 100;
    CHECK(DescribeBugVoice(s, 100).meter == 70);
    CHECK(DescribeBugVoice(s, 102).meter == 0);
    s.lastHeard = L"bug start"; s.heardAt = 100; s.rejected = true;
    l = DescribeBugVoice(s, 102);
    CHECK(l.activity.find(L"bug start") != std::wstring::npos);
    CHECK(l.activity.find(L"Repeat") != std::wstring::npos);
    l = DescribeBugVoice(s, 120);
    CHECK(l.activity.find(L"bug start") != std::wstring::npos);
    CHECK(l.detail.find(L"No microphone signal") != std::wstring::npos);
}

ATHER_TEST(bug_voice_status_keeps_open_bug_and_failures_visible) {
    BugVoiceStatus s;
    s.bugOpen = true; s.bugStarted = 90; s.hotkey = L"Ctrl+Alt+B";
    auto l = DescribeBugVoice(s, 165);
    CHECK(l.visible && l.title == L"BUG STARTED  01:15");
    CHECK(l.action.find(s.hotkey) != std::wstring::npos);
    s.microphone = L"Previously selected microphone";
    CHECK(DescribeBugVoice(s, 165).input == L"Microphone: not listening");
    s.phase = BugVoicePhase::Reconnecting;
    s.message = L"Input stream ended";
    l = DescribeBugVoice(s, 166);
    CHECK(l.title.find(L"BUG STARTED") != std::wstring::npos);
    CHECK(l.detail.find(L"Reconnecting") != std::wstring::npos);
    CHECK(l.detail.find(s.message) != std::wstring::npos);
    s.bugOpen = false; s.phase = BugVoicePhase::Unavailable;
    CHECK(DescribeBugVoice(s, 166).title == L"VOICE UNAVAILABLE");
    s.phase = BugVoicePhase::Off; s.saving = true;
    CHECK(DescribeBugVoice(s, 166).visible);
    CHECK(DescribeBugVoice(s, 166).input == L"Microphone: not listening");
}

ATHER_TEST(bug_voice_panel_preserves_focus_and_uses_production_painter) {
    const HWND foreground = GetForegroundWindow();
    const HWND active = GetActiveWindow();
    BugVoiceStatus s; s.phase = BugVoicePhase::Listening;
    s.microphone = L"Synthetic microphone"; s.language = L"English (US)";
    UpdateBugVoiceStatus(s);
    HWND testPanel = panel;  // the test process's HUD, independent of an already running app's HUD
    CHECK(testPanel && IsWindowVisible(testPanel));
    DWORD panelProcess = 0;
    GetWindowThreadProcessId(testPanel, &panelProcess);
    CHECK_EQ(panelProcess, GetCurrentProcessId());
    if (testPanel) {
        const auto style = GetWindowLongPtrW(testPanel, GWL_EXSTYLE);
        CHECK((style & WS_EX_NOACTIVATE) != 0);
        CHECK((style & WS_EX_TOOLWINDOW) != 0);
        CHECK((style & WS_EX_TOPMOST) != 0);
        CHECK((style & WS_EX_LAYERED) != 0);
        CHECK((style & WS_EX_TRANSPARENT) != 0);
        CHECK(SendMessageW(testPanel, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);
        CHECK(SendMessageW(testPanel, WM_NCHITTEST, 0, 0) == HTTRANSPARENT);
        SendMessageW(testPanel, WM_TIMER, 1, 0);
        UpdateWindow(testPanel);
#ifndef ATHER_TEST_CAPTURABLE_UI
        DWORD affinity = 0;
        CHECK(GetWindowDisplayAffinity(testPanel, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE);
#endif
    }
    CHECK(GetForegroundWindow() == foreground);
    CHECK(GetActiveWindow() == active);
    CHECK(CurrentBugVoiceStatus().microphone == s.microphone);
    auto image = BugVoicePanelSnapshot(s, 100);
    CHECK(image && image->Width() >= 340 && image->Height() >= 140);
    HideBugVoicePanel();
    CHECK(!IsWindow(testPanel));
    CHECK(panel == nullptr);
    UpdateBugVoiceStatus(s);
    CHECK(panel != nullptr && IsWindow(panel));
    testPanel = panel;
    UpdateBugVoiceStatus({});
    CHECK(!IsWindow(testPanel));
    CHECK(panel == nullptr);
}
}  // namespace ather
