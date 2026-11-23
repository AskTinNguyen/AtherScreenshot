#include "bugvoice.h"
#include "selftest.h"
#include <algorithm>
#include <format>
#include <mutex>
#include <memory>
#include "output.h"

namespace ather {
BugVoiceLabels DescribeBugVoice(const BugVoiceStatus& s, double now) {
    BugVoiceLabels l;
    l.visible = s.phase != BugVoicePhase::Off || s.bugOpen || s.saving || s.controllerEnabled;
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
            default: l.title = s.phase == BugVoicePhase::Off ? L"CONTROLLER INPUT" : L"BUG VOICE READY"; break;
        }
        l.action = L"Start a bug: " + key;
    }
    l.input = (s.microphone.empty() ? L"Microphone: identifying input…" : s.microphone)
        + (s.language.empty() ? L"" : L" · " + s.language);
    if (s.phase == BugVoicePhase::Off) l.input = L"Microphone: not listening";
    if (s.phase == BugVoicePhase::Listening && s.levelAt > 0 && now - s.levelAt <= 0.8)
        l.meter = std::clamp(s.audioLevel, 0, 100);
    l.microphoneSignal = s.phase == BugVoicePhase::Listening
        ? (l.meter > 0 ? std::format(L"MIC LIVE  {}%", l.meter) : L"MIC quiet · 0%")
        : L"MIC not listening";
    if (s.controllerEnabled) {
        l.controllerConnection = s.controller.connected
            ? std::format(L"CONTROLLER connected · Xbox / XInput slot {}", s.controller.slot + 1)
            : L"CONTROLLER not connected · Waiting for Xbox / XInput";
        if (!s.controller.connected) l.controllerInput = L"Check USB / Bluetooth or Steam Input visibility";
        else if (PadHasInput(s.controller)) l.controllerInput = L"Receiving: " + PadInputText(s.controller);
        else if (s.controllerInputAt > 0) l.controllerInput = std::format(L"Idle · Last input {:.0f}s ago", std::max(0.0, now - s.controllerInputAt));
        else l.controllerInput = L"Connected · No input received yet · Press a button";
    }
    if (s.phase == BugVoicePhase::Listening && s.speech && now - s.signalAt < 2 && !s.rejected) {
        l.activity = L"Recognizing speech…";
        if (!s.lastHeard.empty()) l.activity += L" · Last heard: “" + s.lastHeard + L"”";
    } else if (!s.lastHeard.empty()) {
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
constexpr int kPanelWidth = 420, kPanelHeight = 300;
std::mutex statusMutex;
BugVoiceStatus status;
HWND panel = nullptr;
std::unique_ptr<GamepadPoller> controllerMonitor;
double lastControllerInput = 0;
int lastControllerSlot = -1;

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
    text(l.microphoneSignal, {16, 105, 404, 124}, 14, l.meter ? RGB(96, 230, 150) : theme::kMuted);
    FillSolid(dc, {16, 128, 404, 136}, theme::kBgRaised);
    if (l.meter) FillSolid(dc, {16, 128, 16 + 388 * l.meter / 100, 136}, RGB(96, 230, 150));
    text(l.activity, {16, 145, 404, 186}, 13, theme::kTextDim, true);
    text(l.action, {16, 190, 404, 211}, 13, theme::kText);
    if (s.controllerEnabled) {
        FillSolid(dc, {16, 220, 404, 221}, theme::kBgRaised);
        text(l.controllerConnection, {16, 231, 404, 253}, 13,
             s.controller.connected ? RGB(96, 230, 150) : RGB(255, 180, 96));
        text(l.controllerInput, {16, 259, 404, 293}, 13,
             PadHasInput(s.controller) ? RGB(96, 230, 150) : theme::kTextDim, true);
    }
}

LRESULT CALLBACK PanelProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            InvalidateRect(h, nullptr, FALSE); return 0;
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
    auto current = status;
    current.controllerEnabled = controllerMonitor != nullptr;
    if (controllerMonitor) {
        current.controller = controllerMonitor->Take();
        if (!current.controller.connected || current.controller.slot != lastControllerSlot) lastControllerInput = 0;
        lastControllerSlot = current.controller.slot;
        if (PadHasInput(current.controller)) lastControllerInput = GetTickCount64() / 1000.0;
        current.controllerInputAt = lastControllerInput;
    }
    return current;
}
void ConfigureGamepadIndicator(bool enabled) {
    {
        std::lock_guard lock(statusMutex);
        if (enabled && !controllerMonitor) controllerMonitor = std::make_unique<GamepadPoller>();
        else if (!enabled) { controllerMonitor.reset(); lastControllerInput = 0; lastControllerSlot = -1; }
    }
    UpdateBugVoiceStatus(CurrentBugVoiceStatus());
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
    if (!DescribeBugVoice(CurrentBugVoiceStatus(), GetTickCount64() / 1000.0).visible) {
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
        SetTimer(panel, 1, 80, nullptr);
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
    s.lastHeard = L"bug"; s.speech = true; s.signalAt = 102;
    CHECK(DescribeBugVoice(s, 102).activity.find(L"Recognizing speech") != std::wstring::npos);
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

ATHER_TEST(bug_voice_input_indicators_distinguish_connection_signal_and_idle) {
    BugVoiceStatus s;
    s.controllerEnabled = true;
    auto l = DescribeBugVoice(s, 100);
    CHECK(l.visible);
    CHECK(l.controllerConnection.find(L"not connected") != std::wstring::npos);
    s.controller.connected = true; s.controller.slot = 1;
    l = DescribeBugVoice(s, 100);
    CHECK(l.controllerConnection.find(L"2") != std::wstring::npos);
    CHECK(l.controllerInput.find(L"No input received") != std::wstring::npos);
    s.controller.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B;
    s.controllerInputAt = 100;
    l = DescribeBugVoice(s, 100);
    CHECK(l.controllerInput.find(L"A + B") != std::wstring::npos);
    s.controller.buttons = 0;
    CHECK(DescribeBugVoice(s, 102).controllerInput.find(L"2s") != std::wstring::npos);
    s.phase = BugVoicePhase::Listening; s.levelAt = 102; s.audioLevel = 63;
    CHECK(DescribeBugVoice(s, 102).microphoneSignal.find(L"63%") != std::wstring::npos);
    CHECK(DescribeBugVoice(s, 104).microphoneSignal.find(L"quiet") != std::wstring::npos);
    wchar_t artifacts[32768]{};
    if (GetEnvironmentVariableW(L"ATHER_INPUT_HUD_DEMO_DIR", artifacts, (DWORD)std::size(artifacts))) {
        s.bugOpen = true; s.bugStarted = 90; s.hotkey = L"Ctrl+Alt+B";
        s.controller.buttons = XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B;
        s.speech = true; s.signalAt = 102; s.lastHeard = L"bug";
        SavePng(*BugVoicePanelSnapshot(s, 102), std::wstring(artifacts) + L"\\input-hud-live.png");
        s.controller = {}; s.audioLevel = 0;
        SavePng(*BugVoicePanelSnapshot(s, 112), std::wstring(artifacts) + L"\\input-hud-disconnected.png");
    }
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

ATHER_TEST(bug_voice_controller_indicator_stays_visible_with_voice_off) {
    UpdateBugVoiceStatus({});
    const HWND foreground = GetForegroundWindow();
    ConfigureGamepadIndicator(true);
    CHECK(panel && IsWindowVisible(panel));
    CHECK(CurrentBugVoiceStatus().controllerEnabled);
    CHECK(DescribeBugVoice(CurrentBugVoiceStatus(), GetTickCount64() / 1000.0).microphoneSignal == L"MIC not listening");
    UpdateBugVoiceStatus({});  // a voice-off update must not dismiss the controller indicator
    CHECK(panel && IsWindowVisible(panel));
    CHECK(GetForegroundWindow() == foreground);
    ConfigureGamepadIndicator(false);
    CHECK(!CurrentBugVoiceStatus().controllerEnabled);
    CHECK(panel == nullptr);
}
}  // namespace ather
