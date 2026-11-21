#include "gamepad.h"
#include "output.h"
#include "recorder.h"
#include "selftest.h"
#include "videoio.h"

#include <array>
#include <algorithm>
#include <mutex>

namespace ather {
namespace {

// Simulated native XInput responses, not a synthetic PadState or a replacement renderer.
// Only tests pass this reader; normal recordings always call Windows XInputGetState.
std::mutex g_inputMutex;
std::array<XINPUT_STATE, XUSER_MAX_COUNT> g_input{};
std::array<bool, XUSER_MAX_COUNT> g_connected{};

DWORD WINAPI SimulatedXInput(DWORD slot, XINPUT_STATE* state) {
    std::lock_guard lock(g_inputMutex);
    if (slot >= XUSER_MAX_COUNT || !g_connected[slot]) return ERROR_DEVICE_NOT_CONNECTED;
    *state = g_input[slot];
    return ERROR_SUCCESS;
}

void ResetInput() {
    std::lock_guard lock(g_inputMutex);
    g_input = {};
    g_connected = {};
}

void Input(DWORD slot, XINPUT_GAMEPAD pad = {}, bool connected = true) {
    std::lock_guard lock(g_inputMutex);
    g_connected[slot] = connected;
    g_input[slot].Gamepad = pad;
    ++g_input[slot].dwPacketNumber;
}

bool WaitPad(GamepadPoller& poller, const std::function<bool(const PadState&)>& ready, DWORD timeout = 400) {
    const ULONGLONG end = GetTickCount64() + timeout;
    do {
        if (ready(poller.Take())) return true;
        Sleep(2);
    } while (GetTickCount64() < end);
    return false;
}

void Pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

void PumpFor(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    do { Pump(); Sleep(2); } while (GetTickCount64() < end);
}

void DrawSimulationBackdrop(Bitmap& frame, const wchar_t* stage) {
    MemDC mem(frame.Handle());
    HDC dc = mem;
    RECT rc{0, 0, frame.Width(), frame.Height()};
    FillSolid(dc, rc, RGB(35, 48, 66));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(244, 246, 250));
    HFONT font = MakeFont(25, FW_SEMIBOLD);
    HGDIOBJ old = SelectObject(dc, font);
    RECT title{28, 24, 770, 72};
    DrawTextW(dc, L"SIMULATED CONTROLLER INPUT", -1, &title, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(font);
    font = MakeFont(18);
    old = SelectObject(dc, font);
    RECT detail{28, 80, 770, 170};
    DrawTextW(dc, L"Injected backdrop + native XInput responses. Real poller,\nrecorder, controller renderer and H.264 encoder.\nSlot 0 stays connected and idle. Slot 1 supplies the actions.", -1, &detail, DT_LEFT);
    RECT action{28, 205, 490, 290};
    SetTextColor(dc, RGB(255, 203, 122));
    DrawTextW(dc, stage, -1, &action, DT_LEFT | DT_WORDBREAK);
    RECT footer{28, 330, 460, 420};
    SetTextColor(dc, RGB(192, 204, 219));
    DrawTextW(dc, L"Simulation verifies the app pipeline.\nIt does not verify a physical controller\nor explain the owner's hardware setup.", -1, &footer, DT_LEFT);
    SelectObject(dc, old);
    DeleteObject(font);
    GdiFlush();
}

struct DemoAction {
    const wchar_t* name;
    XINPUT_GAMEPAD input{};
    float x, y;  // region in the production controller's design coordinates
    int radius = 5;
    bool tap = false;
    double start = 0, released = 0, end = 0;
    int changedFrames = 0, releasedFrames = 0;
};

// A substantial color change in a control's own patch, compared with an encoded idle frame.
// Checking decoded pixels also catches a missing recorder hookup or stale renderer cache.
int ChangedPixels(const Bitmap& idle, const Bitmap& frame, POINT p, int radius) {
    int changed = 0;
    for (int y = std::max(0L, p.y - radius); y <= p.y + radius && y < frame.Height(); ++y)
        for (int x = std::max(0L, p.x - radius); x <= p.x + radius && x < frame.Width(); ++x) {
            const uint32_t a = idle.Bits()[(size_t)y * idle.Width() + x], b = frame.Bits()[(size_t)y * frame.Width() + x];
            int diff = 0;
            for (int shift : {0, 8, 16}) diff += std::abs((int)((a >> shift) & 255) - (int)((b >> shift) & 255));
            if (diff > 65) ++changed;
        }
    return changed;
}

}  // namespace

ATHER_TEST(gamepad_poller_uses_active_second_controller_instead_of_idle_first) {
    ResetInput();
    Input(0);
    XINPUT_GAMEPAD active{};
    active.wButtons = XINPUT_GAMEPAD_A;
    Input(1, active);
    GamepadPoller poller(SimulatedXInput);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected && (s.buttons & XINPUT_GAMEPAD_A); }));
}

ATHER_TEST(gamepad_poller_short_tap_remains_readable_after_first_frame) {
    ResetInput();
    Input(0);
    GamepadPoller poller(SimulatedXInput);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected; }));
    XINPUT_GAMEPAD active{};
    active.wButtons = XINPUT_GAMEPAD_A;
    Input(0, active);
    CHECK(WaitPad(poller, [](const PadState& s) { return (s.buttons & XINPUT_GAMEPAD_A) != 0; }));
    Input(0);  // release as soon as the poller saw the tap
    Sleep(20);
    poller.Take();  // the next video frame drains the old one-frame latch too
    Sleep(50);
    CHECK((poller.Take().buttons & XINPUT_GAMEPAD_A) != 0);
    Sleep(140);
    CHECK_EQ(poller.Take().buttons, 0);
}

ATHER_TEST(gamepad_poller_follows_deliberate_activity_and_ignores_drift) {
    ResetInput();
    Input(0);
    Input(1);
    GamepadPoller poller(SimulatedXInput);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected; }));
    XINPUT_GAMEPAD active{};
    active.wButtons = XINPUT_GAMEPAD_B;
    active.bRightTrigger = 255;
    active.sThumbRX = 32767;
    Input(1, active);
    CHECK(WaitPad(poller, [](const PadState& s) { return (s.buttons & XINPUT_GAMEPAD_B) && s.rt > 0.99f && s.rx > 0.99f; }));
    XINPUT_GAMEPAD noise{};
    noise.sThumbLX = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 500;
    Input(0, noise);
    PumpFor(50);
    CHECK((poller.Take().buttons & XINPUT_GAMEPAD_B) != 0);
    XINPUT_GAMEPAD other{};
    other.wButtons = XINPUT_GAMEPAD_X;
    Input(0, other);
    CHECK(WaitPad(poller, [](const PadState& s) { return (s.buttons & XINPUT_GAMEPAD_X) != 0; }));
    const PadState switched = poller.Take();
    CHECK((switched.buttons & XINPUT_GAMEPAD_B) == 0 && switched.rt == 0);
    Input(0, {}, false);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected && (s.buttons & XINPUT_GAMEPAD_B); }));
    Input(1, {}, false);
    CHECK(WaitPad(poller, [](const PadState& s) { return !s.connected && s.buttons == 0 && s.rt == 0; }));
    Input(2, other);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected && (s.buttons & XINPUT_GAMEPAD_X); }, 1600));
}

ATHER_TEST(gamepad_poller_recognizes_gradual_analog_activity_on_second_pad) {
    ResetInput();
    Input(0);
    Input(1);
    GamepadPoller poller(SimulatedXInput);
    CHECK(WaitPad(poller, [](const PadState& s) { return s.connected; }));
    XINPUT_GAMEPAD active{};
    for (int v = XINPUT_GAMEPAD_TRIGGER_THRESHOLD + 1; v <= 180; ++v) {
        active.bLeftTrigger = (BYTE)v;
        Input(1, active);
        Sleep(4);
    }
    CHECK(WaitPad(poller, [](const PadState& s) { return s.lt > 0.6f; }));
}

ATHER_TEST(gamepad_recording_encodes_simulated_controller_actions) {
    ResetInput();
    Input(0);  // the regression: an idle first controller remains connected for the whole recording
    Input(1);
    std::vector<DemoAction> actions;
    const auto button = [&](const wchar_t* name, WORD b, float x, float y, int radius = 5, bool tap = false) {
        DemoAction a{name, {}, x, y, radius, tap};
        a.input.wButtons = b;
        actions.push_back(a);
    };
    button(L"A", XINPUT_GAMEPAD_A, 191.2f, 67.1f);
    button(L"B", XINPUT_GAMEPAD_B, 209.8f, 48.5f);
    button(L"X", XINPUT_GAMEPAD_X, 172.6f, 48.5f);
    button(L"Y", XINPUT_GAMEPAD_Y, 191.2f, 29.9f);
    button(L"D-pad up", XINPUT_GAMEPAD_DPAD_UP, 82.6f, 74);
    button(L"D-pad down", XINPUT_GAMEPAD_DPAD_DOWN, 82.6f, 106);
    button(L"D-pad left", XINPUT_GAMEPAD_DPAD_LEFT, 66, 90.6f);
    button(L"D-pad right", XINPUT_GAMEPAD_DPAD_RIGHT, 99, 90.6f);
    button(L"Left bumper", XINPUT_GAMEPAD_LEFT_SHOULDER, 50, 5, 3);
    button(L"Right bumper", XINPUT_GAMEPAD_RIGHT_SHOULDER, 190, 5, 3);
    button(L"View", XINPUT_GAMEPAD_BACK, 84.6f, 26.8f);
    button(L"Menu", XINPUT_GAMEPAD_START, 154.4f, 26.8f);
    button(L"Left stick click", XINPUT_GAMEPAD_LEFT_THUMB, 43, 49, 20);
    button(L"Right stick click", XINPUT_GAMEPAD_RIGHT_THUMB, 157.8f, 89.6f, 20);
    DemoAction lt{L"Left trigger", {}, 46, -2};
    lt.input.bLeftTrigger = 255;
    actions.push_back(lt);
    DemoAction rt{L"Right trigger", {}, 186, -2};
    rt.input.bRightTrigger = 255;
    actions.push_back(rt);
    DemoAction ls{L"Left stick movement", {}, 43, 49, 20};
    ls.input.sThumbLX = 32767;
    ls.input.sThumbLY = 22000;
    actions.push_back(ls);
    DemoAction rs{L"Right stick movement", {}, 157.8f, 89.6f, 20};
    rs.input.sThumbRX = -32767;
    rs.input.sThumbRY = -22000;
    actions.push_back(rs);
    button(L"12 ms A tap (highlight held briefly for readability)", XINPUT_GAMEPAD_A, 191.2f, 67.1f, 5, true);
    DemoAction tapLt = lt;
    tapLt.name = L"12 ms left trigger tap";
    tapLt.tap = true;
    actions.push_back(tapLt);

    wchar_t artifactDir[2048]{};
    const bool keep = GetEnvironmentVariableW(L"ATHER_CONTROLLER_DEMO_DIR", artifactDir, (DWORD)std::size(artifactDir)) > 0;
    const std::wstring dir = keep ? artifactDir : test::TempDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\controller-simulated-input.mp4";
    HWND dispatcher = StartUiDispatcher();
    CHECK(dispatcher != nullptr);
    RecordOptions opt;
    opt.rect = {40, 80, 840, 530};
    opt.path = path;
    opt.gpuCapture = false;
    opt.cursor = false;
    opt.showGamepad = true;
    opt.gamepadReader = SimulatedXInput;
    std::atomic<int> stage{-1};
    BitmapPtr backdrop;
    int paintedStage = -2;
    opt.frameSource = [&](Bitmap& raw) {
        const int i = stage.load();
        if (!backdrop || i != paintedStage) {
            backdrop = Bitmap::Create(raw.Width(), raw.Height());
            DrawSimulationBackdrop(*backdrop, i < 0 ? L"Connected and idle" :
                i < (int)actions.size() ? actions[i].name : L"Simulation complete");
            paintedStage = i;
        }
        // The recorder owns a capture DC selecting raw's bitmap. Draw text into our own bitmap,
        // then copy pixels; one HBITMAP cannot be selected into two memory DCs at the same time.
        memcpy(raw.Bits(), backdrop->Bits(), (size_t)raw.Width() * raw.Height() * 4);
    };
    opt.fps = 30;
    bool done = false;
    RecordResult result;
    std::wstring error;
    const bool started = StartRecording(opt, [&](const RecordResult& r) { result = r; done = true; }, &error);
    CHECK(started);
    if (started) {
        // Wait for a real encoded idle frame, rather than timing from encoder initialization.
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (RecordingSeconds() < 0.35 && GetTickCount64() < deadline) PumpFor(5);
        CHECK(RecordingSeconds() >= 0.35);
        for (size_t i = 0; i < actions.size(); ++i) {
            auto& action = actions[i];
            stage = (int)i;
            action.start = RecordingSeconds();
            Input(1, action.input);
            PumpFor(action.tap ? 12 : (keep ? 450 : 180));
            Input(1);
            action.released = RecordingSeconds();
            PumpFor(270);  // drains readable holds and leaves decoded idle frames before the next action
            action.end = RecordingSeconds();
        }
        stage = (int)actions.size();
        PumpFor(350);
        StopRecording(false);
        while (!done) PumpFor(5);  // finalization callback is marshaled through the real UI dispatcher
        CHECK(result.ok);
        CHECK(result.bytes > 0);
    }
    StopUiDispatcher(dispatcher);
    if (!started || !result.ok) return;

    VideoReader reader;
    CHECK(reader.Open(path));
    BitmapPtr frame, idle, finalFrame;
    double t = 0;
    const float dpi = DpiScaleAt({41, 81});
    const PadLayout layout = GamepadLayout(800, 450, opt.gamepadCorner, dpi);
    while (reader.Read(&frame, &t)) {
        finalFrame = frame;
        if (!idle && t >= 0.15) {
            idle = frame;
            const uint32_t bg = idle->Pixel(5, 5);
            CHECK(std::abs((int)(bg >> 16 & 255) - 35) <= 5 &&
                  std::abs((int)(bg >> 8 & 255) - 48) <= 5 && std::abs((int)(bg & 255) - 66) <= 5);
            if (keep) SavePng(*idle, dir + L"\\controller-idle-decoded.png");
        }
        if (!idle) continue;
        for (size_t i = 0; i < actions.size(); ++i) {
            auto& action = actions[i];
            // Tap tests must appear across multiple frames, including after the physical tap ended.
            const double end = action.tap ? action.start + 0.115 : action.end - 0.15;
            if (t < action.start + 0.02 || t > action.end - 0.01) continue;
            const int changes = ChangedPixels(*idle, *frame, layout.At(action.x, action.y), std::max(2, (int)(action.radius * layout.u)));
            if (t >= action.released + 0.18 && changes < 8) ++action.releasedFrames;
            if (t <= end && changes >= 8) {
                ++action.changedFrames;
                if (keep && action.changedFrames == 1 && i == 0) SavePng(*frame, dir + L"\\controller-a-decoded.png");
                if (keep && action.changedFrames == 1 && action.tap && i == actions.size() - 2)
                    SavePng(*frame, dir + L"\\controller-short-tap-decoded.png");
            }
        }
    }
    CHECK(idle != nullptr);
    if (keep && finalFrame) SavePng(*finalFrame, dir + L"\\controller-released-decoded.png");
    for (const auto& action : actions) {
        const int count = WideCharToMultiByte(CP_UTF8, 0, action.name, -1, nullptr, 0, nullptr, nullptr);
        std::string name((size_t)count, '\0');
        WideCharToMultiByte(CP_UTF8, 0, action.name, -1, name.data(), count, nullptr, nullptr);
        name.pop_back();
        test::Out("  decoded " + name + ": " + std::to_string(action.changedFrames) + " highlighted, " +
                  std::to_string(action.releasedFrames) + " released frames\n");
        test::Note(name);
        CHECK(action.changedFrames >= (action.tap ? 2 : 1));
        CHECK(action.releasedFrames >= 1);
    }
}

ATHER_TEST(gamepad_recording_drops_paused_taps_but_keeps_live_held_controls) {
    ResetInput();
    Input(0);
    HWND dispatcher = StartUiDispatcher();
    CHECK(dispatcher != nullptr);
    RecordOptions opt;
    opt.rect = {40, 80, 840, 530};
    opt.path = test::TempDir() + L"\\paused-controller.mp4";
    opt.gpuCapture = false;
    opt.cursor = false;
    opt.showGamepad = true;
    opt.gamepadReader = SimulatedXInput;
    opt.frameSource = [](Bitmap& raw) {
        std::fill_n(raw.Bits(), (size_t)raw.Width() * raw.Height(), 0xFF233042u);
    };
    opt.fps = 60;
    bool done = false;
    RecordResult result;
    std::wstring error;
    const bool started = StartRecording(opt, [&](const RecordResult& r) { result = r; done = true; }, &error);
    CHECK(started);
    if (!started) { StopUiDispatcher(dispatcher); return; }
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (RecordingSeconds() < 0.2 && GetTickCount64() < deadline) PumpFor(5);
    CHECK(RecordingSeconds() >= 0.2);
    XINPUT_GAMEPAD active{};
    active.wButtons = XINPUT_GAMEPAD_A;
    active.bLeftTrigger = 255;
    TogglePauseRecording();
    PumpFor(50);  // let the real capture loop observe the pause
    Input(0, active);
    PumpFor(12);
    Input(0);
    PumpFor(12);  // release is sampled, but remains inside the visibility hold
    const double releasedResume = RecordingSeconds();
    TogglePauseRecording();
    PumpFor(220);
    TogglePauseRecording();
    Input(0, active);  // controls still physically held at resume must remain visible
    PumpFor(25);
    const double heldResume = RecordingSeconds();
    TogglePauseRecording();
    PumpFor(180);
    Input(0);
    PumpFor(230);
    StopRecording(false);
    while (!done) PumpFor(5);
    StopUiDispatcher(dispatcher);
    CHECK(result.ok);
    if (!result.ok) return;
    VideoReader reader;
    CHECK(reader.Open(opt.path));
    BitmapPtr frame, idle;
    double t = 0;
    int staleButton = 0, staleTrigger = 0, heldButton = 0, heldTrigger = 0, releasedSamples = 0;
    const PadLayout layout = GamepadLayout(800, 450, opt.gamepadCorner, DpiScaleAt({41, 81}));
    const auto lit = [&](const Bitmap& f, float x, float y) {
        return ChangedPixels(*idle, f, layout.At(x, y), 4) >= 8;
    };
    while (reader.Read(&frame, &t)) {
        if (!idle && t >= 0.1) idle = frame;
        if (!idle) continue;
        if (t >= releasedResume + 0.02 && t <= releasedResume + 0.09) {
            ++releasedSamples;
            staleButton += lit(*frame, 191.2f, 67.1f);
            staleTrigger += lit(*frame, 46, -2);
        }
        if (t >= heldResume + 0.02 && t <= heldResume + 0.12) {
            heldButton += lit(*frame, 191.2f, 67.1f);
            heldTrigger += lit(*frame, 46, -2);
        }
    }
    test::Out("  pause/resume: " + std::to_string(releasedSamples) + " released samples, " +
        std::to_string(staleButton) + " stale button, " + std::to_string(staleTrigger) + " stale trigger; " +
        std::to_string(heldButton) + " held button, " + std::to_string(heldTrigger) + " held trigger frames\n");
    CHECK(releasedSamples >= 1);
    CHECK_EQ(staleButton, 0);
    CHECK_EQ(staleTrigger, 0);
    CHECK(heldButton >= 1 && heldTrigger >= 1);
}

}  // namespace ather
