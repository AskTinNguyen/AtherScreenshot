#include "scroll.h"

#include <algorithm>
#include <cmath>

#include "capture.h"

namespace ather {
namespace {

using RowHashes = std::vector<uint64_t>;

RowHashes HashRows(const Bitmap& b) {
    RowHashes h(b.Height());
    for (int y = 0; y < b.Height(); ++y) {
        uint64_t v = 1469598103934665603ull;  // FNV-1a over the row
        const uint32_t* row = b.Bits() + (size_t)y * b.Width();
        for (int x = 0; x < b.Width(); ++x) v = (v ^ (row[x] & 0x00FFFFFFu)) * 1099511628211ull;
        h[y] = v;
    }
    return h;
}

struct Match {
    int offset = 0;     // how far the content moved up between the two frames
    double score = 0;   // fraction of overlapping rows that matched
};

// Finds how far content scrolled between `a` (before) and `b` (after), ignoring `top` sticky rows
// and `bottom` sticky rows. Rows of constant colour match everything, so they don't count as evidence.
Match FindOffset(const RowHashes& a, const RowHashes& b, int top, int bottom) {
    const int h = (int)a.size(), lo = top, hi = h - bottom;  // scrolling band [lo, hi)
    Match best;
    int bestHits = 0;
    for (int d = 1; d < hi - lo - 24; ++d) {
        int hits = 0, n = 0;
        for (int y = lo; y + d < hi; ++y) {
            if (a[y + d] == a[std::min(y + d + 1, hi - 1)] && b[y] == b[std::min(y + 1, hi - 1)]) continue;  // flat area
            ++n;
            if (a[y + d] == b[y]) ++hits;
        }
        if (n < 12) continue;
        const double score = (double)hits / n;
        if (hits > bestHits && score >= 0.6) {
            bestHits = hits;
            best = {d, score};
        }
    }
    return best;
}

void Wheel(int notches) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(-WHEEL_DELTA * notches);
    SendInput(1, &in, sizeof(in));
}

struct Session {
    RECT region{};
    int delayMs = 400, maxFrames = 60;
    std::function<void(BitmapPtr, int, std::wstring)> done;
    POINT savedCursor{};
    std::vector<BitmapPtr> frames;
    std::vector<int> offsets;  // offsets[i] = scroll between frames[i] and frames[i+1]
    RowHashes lastHashes;
    int top = -1, bottom = -1;  // sticky header / footer rows, found on the first scroll
    int notches = 3;
    int stalls = 0;
    HWND timerWnd = nullptr;
};

Session* g_scroll = nullptr;
constexpr UINT_PTR kTimer = 0xA7;

void Finish(std::wstring err);

BitmapPtr Stitch(const Session& s) {
    const BitmapPtr& f0 = s.frames[0];
    const int w = f0->Width(), h = f0->Height(), top = std::max(0, s.top), bottom = std::max(0, s.bottom);
    int total = h;
    for (int d : s.offsets) total += d;
    auto out = Bitmap::Create(w, total);
    if (!out) return nullptr;
    auto copyRows = [&](const Bitmap& src, int srcY, int dstY, int rows) {
        if (rows > 0) memcpy(out->Bits() + (size_t)dstY * w, src.Bits() + (size_t)srcY * w, (size_t)rows * w * 4);
    };
    // First frame without its footer, then the newly revealed rows of each frame, then the footer once.
    copyRows(*f0, 0, 0, h - bottom);
    int y = h - bottom;
    for (size_t i = 0; i < s.offsets.size(); ++i) {
        const Bitmap& f = *s.frames[i + 1];
        const int d = s.offsets[i];
        copyRows(f, h - bottom - d, y, d);
        y += d;
    }
    copyRows(*s.frames.back(), h - bottom, y, bottom);
    (void)top;
    return out;
}

void Step() {
    Session& s = *g_scroll;
    if (GetAsyncKeyState(VK_ESCAPE) < 0) return Finish(L"");
    auto frame = CaptureScreen(s.region, false);
    if (!frame) return Finish(L"Capture failed.");
    RowHashes hashes = HashRows(*frame);
    const int h = frame->Height();
    if (s.top < 0) {  // sticky rows: identical at the same position in both frames, from the edges inward
        int t = 0, b = 0;
        while (t < h / 3 && hashes[t] == s.lastHashes[t]) ++t;
        while (b < h / 3 && hashes[h - 1 - b] == s.lastHashes[h - 1 - b]) ++b;
        if (t + b >= h - 24) {  // everything identical: nothing scrolled
            if (++s.stalls >= 2) return Finish(L"");
            Wheel(s.notches);
            SetTimer(s.timerWnd, kTimer, s.delayMs, nullptr);
            return;
        }
        s.top = t;
        s.bottom = b;
    }
    const Match m = FindOffset(s.lastHashes, hashes, s.top, s.bottom);
    if (m.offset == 0) {
        if (++s.stalls >= 2) return Finish(L"");  // reached the end (or the page stopped moving)
    } else {
        s.stalls = 0;
        // Aim for ~60% of the band per step: enough overlap for a reliable match, few frames.
        const int band = h - s.top - s.bottom;
        const double perNotch = (double)m.offset / s.notches;
        if (perNotch > 0) s.notches = std::clamp((int)std::lround(band * 0.6 / perNotch), 1, 15);
        s.frames.push_back(frame);
        s.offsets.push_back(m.offset);
        s.lastHashes = std::move(hashes);
    }
    if ((int)s.frames.size() >= s.maxFrames) return Finish(L"");
    Wheel(s.notches);
    SetTimer(s.timerWnd, kTimer, s.delayMs, nullptr);
}

LRESULT CALLBACK TimerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == kTimer) {
        KillTimer(h, kTimer);
        if (g_scroll) Step();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Finish(std::wstring err) {
    Session* s = g_scroll;
    g_scroll = nullptr;
    KillTimer(s->timerWnd, kTimer);
    DestroyWindow(s->timerWnd);
    SetCursorPos(s->savedCursor.x, s->savedCursor.y);
    BitmapPtr img = err.empty() && !s->frames.empty() ? Stitch(*s) : nullptr;
    auto done = std::move(s->done);
    const int n = (int)s->frames.size();
    delete s;
    if (done) done(img, n, err);
}

}  // namespace

void StartScrollingCapture(const RECT& region, int delayMs, int maxFrames,
                           std::function<void(BitmapPtr, int, std::wstring)> done) {
    if (g_scroll) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = TimerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AtherScreenshotScroll";
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* s = new Session();
    s->region = region;
    s->delayMs = delayMs;
    s->maxFrames = std::max(2, maxFrames);
    s->done = std::move(done);
    s->timerWnd = CreateWindowExW(0, L"AtherScreenshotScroll", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    GetCursorPos(&s->savedCursor);
    g_scroll = s;
    auto first = CaptureScreen(region, false);
    if (!first) return Finish(L"Capture failed.");
    s->frames.push_back(first);
    s->lastHashes = HashRows(*first);
    // The wheel goes to whatever is under the cursor: park it in the middle of the region.
    SetCursorPos((region.left + region.right) / 2, (region.top + region.bottom) / 2);
    Wheel(s->notches);
    SetTimer(s->timerWnd, kTimer, delayMs, nullptr);
}

bool ScrollingCaptureActive() { return g_scroll != nullptr; }

}  // namespace ather
