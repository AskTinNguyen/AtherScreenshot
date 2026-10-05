#include "recorder.h"

#include <codecapi.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "audio.h"
#include "capture.h"
#include "inputviz.h"
#include "media.h"
#include "wgc.h"

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif


using Microsoft::WRL::ComPtr;

namespace ather {
namespace {

constexpr int64_t kTicksPerSecond = 10'000'000;  // Media Foundation 100 ns units

// ---- encoders ----

class Sink {
public:
    virtual ~Sink() = default;
    virtual HRESULT Begin(int w, int h, int fps, const std::wstring& path, bool audio) = 0;
    // t in 100 ns on the (pause-free) timeline. A failure ends the recording early but keeps what was written.
    virtual HRESULT Write(const uint32_t* px, int64_t t) = 0;
    virtual void WriteAudio(const int16_t*, uint32_t, int64_t) {}
    virtual HRESULT End(int64_t t) = 0;
};

class Mp4Sink : public Sink {
public:
    HRESULT Begin(int w, int h, int fps, const std::wstring& path, bool audio) override {
        frameDur_ = kTicksPerSecond / fps;
        return writer_.Begin(path, w, h, fps, audio ? AudioCapture::kRate : 0, AudioCapture::kChannels);
    }
    HRESULT Write(const uint32_t* px, int64_t t) override { return writer_.WriteFrame(px, t, frameDur_); }
    void WriteAudio(const int16_t* pcm, uint32_t frames, int64_t t) override { writer_.WriteAudio(pcm, frames, t); }
    HRESULT End(int64_t) override { return writer_.Finalize(); }

private:
    Mp4Writer writer_;
    int64_t frameDur_ = 0;
};

class GifSink : public Sink {
public:
    HRESULT Begin(int w, int h, int, const std::wstring& path, bool) override {
        w_ = w;
        h_ = h;
        path_ = path;
        worker_ = std::thread([this] { Work(); });
        return S_OK;
    }

    HRESULT Write(const uint32_t* px, int64_t t) override {
        const size_t n = (size_t)w_ * h_;
        if (!last_.empty() && memcmp(last_.data(), px, n * 4) == 0) return S_OK;  // unchanged: the previous frame just lasts longer
        std::lock_guard lock(m_);
        if (FAILED(hr_)) return hr_;  // the encoder gave up (disk full...): stop recording
        // Encoder behind: drop this frame. It doesn't become the "unchanged" reference, so if the screen then
        // stays still, the next identical frame is queued and the GIF ends on what was really shown.
        if (q_.size() >= 6) return S_OK;
        last_.assign(px, px + n);
        q_.push_back({last_, t});
        cv_.notify_one();
        return S_OK;
    }

    HRESULT End(int64_t t) override {
        {
            std::lock_guard lock(m_);
            ending_ = true;
            endT_ = t;
        }
        cv_.notify_one();
        worker_.join();
        return hr_;
    }

private:
    struct Frame {
        std::vector<uint32_t> px;
        int64_t t;
    };

    static int Cs(int64_t t) { return (int)(t / 100000); }  // GIF delays are in 1/100 s

    void Work() {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        GifWriter gif;
        hr_ = gif.Begin(path_, w_, h_);
        Frame pending;
        bool has = false;
        int64_t endT = 0;
        for (;;) {
            Frame f;
            {
                std::unique_lock lock(m_);
                cv_.wait(lock, [this] { return !q_.empty() || ending_; });
                if (q_.empty()) {
                    endT = endT_;
                    break;
                }
                f = std::move(q_.front());
                q_.pop_front();
            }
            if (has && SUCCEEDED(hr_)) hr_ = gif.Add(pending.px.data(), Cs(f.t) - Cs(pending.t));
            pending = std::move(f);
            has = true;
        }
        if (SUCCEEDED(hr_)) hr_ = has ? gif.Add(pending.px.data(), std::max(10, Cs(endT) - Cs(pending.t))) : E_FAIL;
        const HRESULT fin = gif.Finish();
        if (SUCCEEDED(hr_)) hr_ = fin;
        if (SUCCEEDED(co)) CoUninitialize();
    }

    int w_ = 0, h_ = 0;
    std::wstring path_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Frame> q_;
    bool ending_ = false;
    int64_t endT_ = 0;
    std::atomic<HRESULT> hr_{S_OK};
    std::vector<uint32_t> last_;
};

// ---- session ----

enum class State { Idle, Countdown, Recording, Finalizing };

struct Session {
    RecordOptions opt;
    int outW = 0, outH = 0;
    bool scaled = false;
    std::thread thread;
    std::atomic<bool> stop{false}, discard{false};
    RecClock clock;
    std::function<void(const RecordResult&)> done;
};

Session* g_session = nullptr;
State g_state = State::Idle;
int g_countdown = 0;
HWND g_border = nullptr, g_bar = nullptr, g_count = nullptr;

void Run(Session* s) {
    RecordResult res;
    res.format = s->opt.format;
    res.path = s->opt.path;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const RECT r = s->opt.rect;

    // Frame source: GPU (Windows.Graphics.Capture) when possible, GDI BitBlt as the fallback.
    std::unique_ptr<WgcSource> wgc;
    std::wstring srcErr;
    if (s->opt.window || (s->opt.gpuCapture && !s->scaled && WgcSource::Supported())) {
        wgc = std::make_unique<WgcSource>();
        bool ok = s->opt.window ? wgc->StartWindow(s->opt.window, s->outW, s->outH, s->opt.cursor, &srcErr)
                                : wgc->StartRegion(r, s->outW, s->outH, s->opt.cursor, &srcErr);
        if (!ok) wgc.reset();
    }
    HRESULT hr = S_OK;
    if (s->opt.window && !wgc) {
        hr = E_FAIL;
        res.error = srcErr.empty() ? L"This window can't be captured." : srcErr;
    }

    AudioCapture audio;
    bool withAudio = false;
    if (SUCCEEDED(hr) && s->opt.format == RecordFormat::Mp4 && (s->opt.systemAudio || s->opt.microphone)) {
        std::wstring aerr;
        withAudio = audio.Open(s->opt.systemAudio, s->opt.microphone, &aerr, &res.warning);
        if (!withAudio) res.warning += aerr;
    }

    std::unique_ptr<Sink> sink;
    if (s->opt.format == RecordFormat::Mp4) sink = std::make_unique<Mp4Sink>();
    else sink = std::make_unique<GifSink>();
    if (SUCCEEDED(hr)) hr = sink->Begin(s->outW, s->outH, s->opt.fps, s->opt.path, withAudio);

    int64_t lastT = 0;
    std::wstring stopReason;  // the recording ended by itself; the footage is still saved
    if (SUCCEEDED(hr)) {
        auto raw = Bitmap::Create(s->outW, s->outH);
        auto frame = Bitmap::Create(s->outW, s->outH);  // raw + click/key overlays
        const size_t bytes = (size_t)s->outW * s->outH * 4;
        const bool viz = s->opt.showClicks || s->opt.showKeys || s->opt.showGamepad;
        std::unique_ptr<GamepadPoller> pad;
        if (s->opt.showGamepad) pad = std::make_unique<GamepadPoller>();
        HDC screen = GetDC(nullptr);
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        if (wgc)  // the first GPU frame can take a moment; don't start the clock on a black frame
            for (int i = 0; i < 100 && !wgc->Grab(raw->Bits()); ++i) Sleep(10);
        const int64_t freq = s->clock.freq;
        const int64_t period = freq / s->opt.fps;
        s->clock.start = RecClock::Now();
        if (withAudio) audio.Run(&s->clock, [&sink](const int16_t* pcm, uint32_t n, int64_t t) { sink->WriteAudio(pcm, n, t); });
        int64_t next = RecClock::Now();
        {
            MemDC mem(raw->Handle(), screen);
            if (s->scaled) SetStretchBltMode(mem, COLORONCOLOR);
            while (!s->stop) {
                // A followed window that closes ends the recording; what was captured is kept.
                if (s->opt.window && !IsWindow(s->opt.window)) {
                    stopReason = L"The window was closed.";
                    break;
                }
                if (!s->clock.Paused()) {
                    if (wgc) {
                        wgc->Grab(raw->Bits());  // no new frame = screen unchanged, reuse the previous one
                    } else {
                        if (s->scaled)
                            StretchBlt(mem, 0, 0, s->outW, s->outH, screen, r.left, r.top, RectW(r), RectH(r),
                                       SRCCOPY | CAPTUREBLT);
                        else
                            BitBlt(mem, 0, 0, s->outW, s->outH, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
                        if (s->opt.cursor && !s->scaled) DrawCursorInto(mem, r);
                        GdiFlush();
                    }
                    const uint32_t* px = raw->Bits();
                    if (viz) {
                        memcpy(frame->Bits(), raw->Bits(), bytes);
                        POINT origin{r.left, r.top};
                        RECT wr;
                        if (s->opt.window && GetWindowFrame(s->opt.window, &wr)) origin = {wr.left, wr.top};
                        const float scale = s->scaled ? (float)s->outW / RectW(r) : 1.f;
                        DrawInputViz(*frame, origin, scale);
                        if (pad) DrawGamepad(*frame, pad->Take(), s->opt.gamepadCorner, DpiScaleAt({origin.x + 1, origin.y + 1}) * scale);
                        px = frame->Bits();
                    }
                    lastT = s->clock.ActiveTicks100ns(RecClock::Now());
                    if (HRESULT whr = sink->Write(px, lastT); FAILED(whr)) {  // keep what's written so far
                        wchar_t msg[96];
                        swprintf_s(msg, L"Couldn't write more frames (0x%08lX).", (unsigned long)whr);
                        stopReason = msg;
                        break;
                    }
                    if (!res.thumb) {
                        res.thumb = Bitmap::Create(s->outW, s->outH);
                        memcpy(res.thumb->Bits(), px, bytes);
                        for (size_t i = 0, n = bytes / 4; i < n; ++i) res.thumb->Bits()[i] |= 0xFF000000u;
                    }
                }
                // Next frame on a fixed grid; if we fell behind, skip ahead instead of bursting.
                next += period;
                const int64_t now = RecClock::Now();
                if (now >= next) next = now + period / 4;
                LARGE_INTEGER due;
                due.QuadPart = -((next - now) * kTicksPerSecond / freq);
                if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, 1000);
            }
            s->clock.Resume();
            lastT = s->clock.ActiveTicks100ns(RecClock::Now());
        }
        audio.Stop();
        if (timer) CloseHandle(timer);
        ReleaseDC(nullptr, screen);
        hr = sink->End(lastT);
    }
    audio.Stop();
    if (wgc) wgc->Stop();
    wgc.reset();
    sink.reset();
    if (SUCCEEDED(co)) CoUninitialize();

    res.seconds = lastT / (double)kTicksPerSecond;
    res.discarded = s->discard;
    if (s->discard || FAILED(hr)) {
        DeleteFileW(s->opt.path.c_str());
        if (FAILED(hr) && !s->discard && res.error.empty()) {
            wchar_t msg[96];
            swprintf_s(msg, L"The encoder failed (0x%08lX).", (unsigned long)hr);
            res.error = msg;
        }
    } else {
        res.ok = true;
        if (!stopReason.empty()) res.warning = L"Stopped early: " + stopReason + (res.warning.empty() ? L"" : L" " + res.warning);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(s->opt.path.c_str(), GetFileExInfoStandard, &fa))
            res.bytes = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    RunOnUi([s, res] {
        s->thread.join();
        auto done = std::move(s->done);
        delete s;
        g_session = nullptr;
        g_state = State::Idle;
        if (done) done(res);
    });
}

// ---- on-screen chrome: red frame, control bar and countdown, all invisible to capture ----

constexpr COLORREF kKey = RGB(255, 0, 254);
constexpr COLORREF kRed = RGB(255, 59, 48);
constexpr COLORREF kAmber = RGB(255, 184, 0);
RECT g_pauseBtn{}, g_stopBtn{}, g_discardBtn{};
int g_barHover = 0;  // 1 = pause, 2 = stop, 3 = discard
float g_barScale = 1;
HFONT g_barFont = nullptr, g_barIcon = nullptr, g_countFont = nullptr;

int BS(int v) { return Px(g_barScale, v); }

LRESULT CALLBACK BorderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillSolid(dc, rc, kKey);
            FrameSolid(dc, rc, IsRecordingPaused() ? kAmber : kRed, std::max(2, BS(2)));
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

void LayoutBar(const RECT& rc) {
    const int cy = rc.bottom / 2, bw = BS(30), bh = BS(26);
    g_discardBtn = {rc.right - BS(6) - bw, cy - bh / 2, rc.right - BS(6), cy + bh / 2};
    g_stopBtn = {g_discardBtn.left - BS(4) - BS(64), cy - bh / 2, g_discardBtn.left - BS(4), cy + bh / 2};
    g_pauseBtn = {g_stopBtn.left - BS(4) - bw, cy - bh / 2, g_stopBtn.left - BS(4), cy + bh / 2};
}

void PaintBar(HWND h, HDC hdc) {
    RECT rc;
    GetClientRect(h, &rc);
    LayoutBar(rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bb);
    FillSolid(dc, rc, theme::kSurface);
    SetBkMode(dc, TRANSPARENT);
    const int cy = rc.bottom / 2;
    const bool paused = IsRecordingPaused();
    double secs = 0;
    if (g_session && g_state == State::Recording && g_session->clock.start)
        secs = g_session->clock.Active(RecClock::Now()) / (double)g_session->clock.freq;
    const bool blink = paused || ((int)(secs * 2)) % 2 == 0;
    RECT dot{BS(14), cy - BS(5), BS(24), cy + BS(5)};
    FillRounded(dc, dot, BS(5), paused ? kAmber : (blink ? kRed : RGB(120, 40, 40)));
    wchar_t text[64];
    const wchar_t* fmt = g_session && g_session->opt.format == RecordFormat::Gif ? L"GIF" : L"MP4";
    if (g_state == State::Countdown) swprintf_s(text, L"Starting in %d", g_countdown);
    else swprintf_s(text, L"%s  %d:%02d", paused ? L"Paused" : fmt, (int)secs / 60, (int)secs % 60);
    HGDIOBJ of = SelectObject(dc, g_barFont);
    SetTextColor(dc, theme::kText);
    RECT tr{BS(32), 0, g_pauseBtn.left - BS(10), rc.bottom};  // never runs under the buttons
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    FillRounded(dc, g_stopBtn, BS(6), g_barHover == 2 ? RGB(255, 90, 80) : kRed);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L"Stop", -1, &g_stopBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_barIcon);
    if (g_barHover == 1) FillRounded(dc, g_pauseBtn, BS(6), theme::kBgRaised);
    SetTextColor(dc, paused ? kAmber : theme::kText);
    DrawTextW(dc, paused ? L"\xE768" : L"\xE769", 1, &g_pauseBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (g_barHover == 3) FillRounded(dc, g_discardBtn, BS(6), theme::kBgRaised);
    SetTextColor(dc, theme::kMuted);
    DrawTextW(dc, L"\xE711", 1, &g_discardBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bb);
    DeleteDC(dc);
}

LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hv = PtInRect(&g_pauseBtn, p) ? 1 : PtInRect(&g_stopBtn, p) ? 2 : PtInRect(&g_discardBtn, p) ? 3 : 0;
            if (hv != g_barHover) {
                g_barHover = hv;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_barHover = 0;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&g_pauseBtn, p)) TogglePauseRecording();
            else if (PtInRect(&g_stopBtn, p)) StopRecording(false);
            else if (PtInRect(&g_discardBtn, p)) StopRecording(true);
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, g_barHover ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintBar(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void BeginCapture();

LRESULT CALLBACK CountProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONUP: StopRecording(true); return 0;  // click the countdown to cancel
        case WM_TIMER:
            if (--g_countdown <= 0) {
                KillTimer(h, 1);
                BeginCapture();
            } else {
                InvalidateRect(h, nullptr, FALSE);
                if (g_bar) InvalidateRect(g_bar, nullptr, FALSE);
            }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillSolid(dc, rc, theme::kSurface);
            HGDIOBJ of = SelectObject(dc, g_countFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, theme::kAccent);
            wchar_t n[8];
            swprintf_s(n, L"%d", g_countdown);
            DrawTextW(dc, n, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            EndPaint(h, &ps);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

HWND MakePopup(const wchar_t* cls, WNDPROC proc, DWORD ex, int x, int y, int w, int h) {
    static std::vector<std::wstring> registered;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (std::find(registered.begin(), registered.end(), cls) == registered.end()) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = proc;
        wc.lpszClassName = cls;
        RegisterClassExW(&wc);
        registered.push_back(cls);
    }
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | ex, cls, L"", WS_POPUP, x, y, w, h,
                              nullptr, nullptr, inst, nullptr);
    ExcludeFromCapture(hw);
    return hw;
}

void CreateChrome(const Session& s) {
    RECT r = s.opt.rect;
    if (s.opt.window) GetWindowFrame(s.opt.window, &r);
    POINT center{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    g_barScale = DpiScaleAt(center);
    for (HFONT* f : {&g_barFont, &g_barIcon, &g_countFont})
        if (*f) DeleteObject(*f);
    g_barFont = MakeEyebrowFont(BS(14));
    g_barIcon = MakeFont(BS(12), FW_NORMAL, L"Segoe Fluent Icons");
    g_countFont = MakeDisplayFont(BS(72));

    const int b = std::max(2, BS(2));
    if (!s.opt.window) {  // a followed window moves around, so only fixed regions get a frame
        g_border = MakePopup(L"AtherScreenshotRecBorder", BorderProc, WS_EX_LAYERED | WS_EX_TRANSPARENT, r.left - b,
                             r.top - b, RectW(r) + 2 * b, RectH(r) + 2 * b);
        SetLayeredWindowAttributes(g_border, kKey, 0, LWA_COLORKEY);
        ShowWindow(g_border, SW_SHOWNOACTIVATE);
    }

    // Control bar sized to its content: dot + widest status text + Pause + Stop + Discard.
    // Below the region, else above, else inside the bottom edge.
    SIZE textSz{};
    {
        HDC mdc = GetDC(nullptr);
        HGDIOBJ of = SelectObject(mdc, g_barFont);
        const wchar_t widest[] = L"Paused  888:88";
        GetTextExtentPoint32W(mdc, widest, (int)wcslen(widest), &textSz);
        SelectObject(mdc, of);
        ReleaseDC(nullptr, mdc);
    }
    const int w = BS(32) + textSz.cx + BS(10) + BS(30) + BS(4) + BS(64) + BS(4) + BS(30) + BS(6), h = BS(38),
              gap = BS(8);
    RECT mon = MonitorRectAt({r.right - 1, r.bottom - 1});
    int x = std::clamp<int>(r.right - w, mon.left, std::max<int>(mon.left, mon.right - w));
    int y = r.bottom + b + gap;
    if (y + h > mon.bottom) y = r.top - b - gap - h;
    if (y < mon.top) y = r.bottom - h - gap;
    g_bar = MakePopup(L"AtherScreenshotRecBar", BarProc, 0, x, y, w, h);
    PrepareChromeless(g_bar, true);
    SetTimer(g_bar, 1, 250, nullptr);
    ShowWithoutFlash(g_bar, false);

    if (g_countdown > 0) {
        const int cs = BS(120);
        g_count = MakePopup(L"AtherScreenshotRecCount", CountProc, WS_EX_LAYERED, center.x - cs / 2, center.y - cs / 2,
                            cs, cs);
        SetLayeredWindowAttributes(g_count, 0, 225, LWA_ALPHA);
        HRGN round = CreateEllipticRgn(0, 0, cs + 1, cs + 1);
        SetWindowRgn(g_count, round, FALSE);
        SetTimer(g_count, 1, 1000, nullptr);
        ShowWithoutFlash(g_count, false);
    }
}

void DestroyChrome() {
    for (HWND* hw : {&g_border, &g_bar, &g_count})
        if (*hw) {
            DestroyWindow(*hw);
            *hw = nullptr;
        }
}

void BeginCapture() {
    if (!g_session) return;
    if (g_count) {
        DestroyWindow(g_count);
        g_count = nullptr;
        DwmFlush();  // make sure the countdown is off screen before the first frame
    }
    g_state = State::Recording;
    StartInputViz(g_session->opt.showClicks, g_session->opt.showKeys);
    g_session->thread = std::thread(Run, g_session);
}

}  // namespace

bool StartRecording(const RecordOptions& opt, std::function<void(const RecordResult&)> done, std::wstring* error) {
    if (g_session) {
        if (error) *error = L"A recording is already in progress.";
        return false;
    }
    auto* s = new Session();
    s->opt = opt;
    s->opt.fps = std::clamp(opt.fps, 1, 60);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s->clock.freq = f.QuadPart;
    if (opt.window) {
        SIZE sz = WgcWindowSize(opt.window);
        RECT wr{};
        if (sz.cx <= 0 && GetWindowFrame(opt.window, &wr)) sz = {RectW(wr), RectH(wr)};
        if (sz.cx < 16 || sz.cy < 16 || (opt.format == RecordFormat::Mp4 && (sz.cx > 4096 || sz.cy > 2304))) {
            if (error) *error = L"That window is too small or too large to record.";
            delete s;
            return false;
        }
        s->outW = sz.cx & ~1;
        s->outH = sz.cy & ~1;
    } else {
        int w = RectW(opt.rect), h = RectH(opt.rect);
        // H.264 encoders top out around 4096x2304; scale anything larger down to fit.
        double k = opt.format == RecordFormat::Mp4 ? std::min({1.0, 4096.0 / w, 2304.0 / h}) : 1.0;
        s->scaled = k < 1.0;
        s->outW = std::max(2, (int)(w * k)) & ~1;  // H.264 needs even dimensions
        s->outH = std::max(2, (int)(h * k)) & ~1;
        if (!s->scaled) {  // trim the odd pixel from the region itself so nothing is resampled
            s->opt.rect.right = s->opt.rect.left + s->outW;
            s->opt.rect.bottom = s->opt.rect.top + s->outH;
        }
    }
    s->done = [done](const RecordResult& r) {
        DestroyChrome();
        StopInputViz();
        if (done) done(r);
    };
    g_session = s;
    g_countdown = std::clamp(opt.countdownSeconds, 0, 10);
    g_state = State::Countdown;
    CreateChrome(*s);
    if (g_countdown == 0) BeginCapture();
    return true;
}

void StopRecording(bool discard) {
    if (!g_session) return;
    if (g_state == State::Countdown) {  // nothing captured yet: just cancel
        Session* s = g_session;
        g_session = nullptr;
        g_state = State::Idle;
        DestroyChrome();
        RecordResult res;
        res.discarded = true;
        res.format = s->opt.format;
        auto done = std::move(s->done);
        delete s;
        if (done) done(res);
        return;
    }
    if (g_state != State::Recording) return;
    g_session->discard = discard;
    g_session->stop = true;
    g_state = State::Finalizing;
    StopInputViz();
    DestroyChrome();
}

void TogglePauseRecording() {
    if (!g_session || g_state != State::Recording) return;
    if (g_session->clock.Paused()) g_session->clock.Resume();
    else g_session->clock.Pause();
    if (g_border) InvalidateRect(g_border, nullptr, FALSE);
    if (g_bar) InvalidateRect(g_bar, nullptr, FALSE);
}

bool IsRecording() { return g_state == State::Countdown || g_state == State::Recording; }
bool IsRecordingPaused() { return g_session && g_state == State::Recording && g_session->clock.Paused(); }
bool RecorderBusy() { return g_session != nullptr; }

}  // namespace ather
