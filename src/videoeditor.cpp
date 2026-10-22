#include "videoeditor.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <mfmediaengine.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#define SECURITY_WIN32
#include <security.h>
#pragma comment(lib, "secur32")

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>

#include <map>
#include <mutex>
#include <optional>
#include <set>

#include <thread>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "annot.h"
#include "compress.h"
#include "json.h"
#include "library.h"
#include "media.h"
#include "output.h"
#include "palette.h"
#include "selftest.h"
#include "textdraw.h"
#include "toast.h"
#include "videoedit.h"
#include "videoio.h"

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotVideoEditor";
constexpr wchar_t kIconFace[] = L"Segoe Fluent Icons";
constexpr UINT WM_ENGINE = WM_APP + 40, WM_THUMBS = WM_APP + 41, WM_TRANSCRIBED = WM_APP + 42, WM_SAVED = WM_APP + 43, WM_FETCHED = WM_APP + 44,
               WM_FRAMES = WM_APP + 45, WM_STRIP = WM_APP + 46, WM_OVERLAYS = WM_APP + 47;
enum : UINT_PTR { kTimerFrame = 1 };
enum : int { kField1 = 200, kField2 };

const wchar_t* const kQuickEmoji[] = {L"✅", L"❌", L"⚠️", L"\U0001F449", L"\U0001F440", L"\U0001F4A1",
                                      L"\U0001F389", L"\U0001F525", L"⭐", L"❤️", L"\U0001F44D", L"\U0001F914"};
const wchar_t* const kAspects[] = {L"Free", L"16:9", L"4:3", L"1:1", L"9:16"};
const double kAspectValues[] = {0, 16.0 / 9, 4.0 / 3, 1, 9.0 / 16};

std::wstring g_folder;
HICON g_icon = nullptr;
std::wstring g_noteAuthor;  // remembered in settings; empty: Windows' display name
std::function<void(const std::wstring&)> g_rememberAuthor;

// Who you are on this PC: the display name of the Windows account ("Tin Nguyen"), else the user name.
std::wstring WindowsDisplayName() {
    wchar_t buf[256];
    ULONG n = (ULONG)std::size(buf);
    if (GetUserNameExW(NameDisplay, buf, &n) && n > 0 && buf[0]) return buf;
    DWORD m = (DWORD)std::size(buf);
    if (GetUserNameW(buf, &m) && buf[0]) return buf;
    return L"Reviewer";
}
std::wstring NoteAuthor() { return g_noteAuthor.empty() ? WindowsDisplayName() : g_noteAuthor; }
bool WriteFileUtf8(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    return ok;
}
// A small file's bytes (at most 16 MB).
bool ReadFileUtf8(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart <= (16 << 20);
    if (ok) {
        out->resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out->empty() || (ReadFile(f, out->data(), (DWORD)out->size(), &got, nullptr) && got == out->size());
    }
    CloseHandle(f);
    return ok;
}

// A note's author typed in the editor becomes the name on new notes, kept in settings.
void RememberAuthor(const std::wstring& name) {
    g_noteAuthor = name;
    if (g_rememberAuthor) g_rememberAuthor(name);
}

gp::Color A(COLORREF c, BYTE a = 255) { return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundPath(gp::GraphicsPath& p, float x, float y, float w, float h, float r) {
    r = std::max(0.f, std::min({r, w / 2, h / 2}));
    if (r < 0.5f) {
        p.AddRectangle(gp::RectF(x, y, w, h));
        return;
    }
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

void FillRR(gp::Graphics& g, const RECT& r, float rad, gp::Color c) {
    gp::GraphicsPath p;
    RoundPath(p, (float)r.left, (float)r.top, (float)RectW(r), (float)RectH(r), rad);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
}

SIZE Measure(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, o);
    return sz;
}

void Text(HDC dc, HFONT f, const std::wstring& t, RECT r, COLORREF c, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, flags | DT_NOPREFIX);
    SelectObject(dc, o);
}

std::wstring Clock(double t) {
    const double s = std::max(0.0, t);
    wchar_t b[32];
    swprintf_s(b, L"%d:%02d.%d", (int)s / 60, (int)s % 60, (int)std::fmod(s * 10, 10));
    return b;
}

std::wstring SpeedLabel(double v) {
    wchar_t b[16];
    swprintf_s(b, L"%g×", v);
    return b;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---------- popup menus ----------

struct MenuItem {
    std::wstring label;
    std::function<void()> run;
    bool checked = false, enabled = true, separator = false;
    std::optional<COLORREF> swatch;
    std::vector<MenuItem> sub;
    static MenuItem Sep() {
        MenuItem m;
        m.separator = true;
        return m;
    }
    static MenuItem Header(const std::wstring& t) {
        MenuItem m;
        m.label = t;
        m.enabled = false;
        return m;
    }
};

HBITMAP SwatchBitmap(COLORREF c) {
    static std::map<COLORREF, HBITMAP> cache;
    if (auto it = cache.find(c); it != cache.end()) return it->second;
    const int n = 14;
    auto b = Bitmap::Create(n, n);
    std::fill_n(b->Bits(), n * n, 0u);
    {
        gp::Bitmap gb(n, n, n * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(b->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::SolidBrush br(A(c));
        g.FillEllipse(&br, 1.f, 1.f, n - 2.f, n - 2.f);
        gp::Pen pen(gp::Color(90, 128, 128, 128), 1);
        g.DrawEllipse(&pen, 1.f, 1.f, n - 2.f, n - 2.f);
    }
    // The menu keeps the HBITMAP, so this one is never freed (eight colors at most).
    HDC sdc = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP h = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, sdc);
    if (h) memcpy(bits, b->Bits(), (size_t)n * n * 4);
    cache[c] = h;
    return h;
}

HMENU BuildMenu(const std::vector<MenuItem>& items, std::vector<const MenuItem*>& flat) {
    HMENU m = CreatePopupMenu();
    for (const auto& it : items) {
        if (it.separator) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_STRING | MIIM_STATE | MIIM_ID;
        mi.dwTypeData = const_cast<wchar_t*>(it.label.c_str());
        mi.fState = (it.checked ? MFS_CHECKED : 0) | (it.enabled ? 0 : MFS_DISABLED);
        if (!it.sub.empty()) {
            mi.fMask |= MIIM_SUBMENU;
            mi.hSubMenu = BuildMenu(it.sub, flat);
        }
        flat.push_back(&it);
        mi.wID = (UINT)flat.size();
        if (it.swatch) {
            mi.fMask |= MIIM_BITMAP;
            mi.hbmpItem = SwatchBitmap(*it.swatch);
        }
        InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
    }
    return m;
}

void RunMenu(HWND owner, const std::vector<MenuItem>& items, POINT at) {
    std::vector<const MenuItem*> flat;
    HMENU m = BuildMenu(items, flat);
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, at.x, at.y, 0, owner, nullptr);
    DestroyMenu(m);
    if (cmd >= 1 && cmd <= flat.size() && flat[cmd - 1]->run) flat[cmd - 1]->run();
}

// ---------- frames for the paused preview ----------

// Decodes the frame at a time on a worker thread. It keeps one reader open, so stepping forward is cheap, and the
// last frames decoded on the way to an exact request (a step), so stepping back is too.
class FrameFetcher {
public:
    FrameFetcher(Sequence seq, HWND hwnd) : seq_(std::move(seq)), hwnd_(hwnd), worker_([this] { Work(); }) {}
    ~FrameFetcher() {
        {
            std::lock_guard l(mu_);
            quit_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }
    // The frame at `t`: the first one from `tol` seconds before it on (by default half a frame, so the nearest one).
    void Request(double t, double tol = -1) {
        {
            std::lock_guard l(mu_);
            want_ = t;
            tol_ = tol;
        }
        cv_.notify_one();
    }
    struct Result {
        BitmapPtr frame;
        double t;
    };

private:
    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SequenceReader r;
        const bool ok = r.Open(seq_);
        const double fps = r.Fps() > 1 ? r.Fps() : 30;
        // Frames read since the last seek, oldest first, with their times. The newest few keep their pictures
        // (unconverted until shown), within ~150 MB: 25 frames of 1080p.
        std::deque<std::pair<VideoFrame, double>> got;
        const size_t keep = std::clamp<size_t>((size_t)(150e6 / std::max(1.0, 5.5 * r.Size().cx * r.Size().cy)), 4, 60);
        for (;;) {
            double t, tol;
            {
                std::unique_lock l(mu_);
                cv_.wait(l, [&] { return quit_ || want_ >= 0; });
                if (quit_) break;
                t = want_;
                tol = tol_;
                want_ = -1;
            }
            if (!ok) continue;
            const bool exact = tol >= 0;
            const double from = t - (exact ? tol : 0.5 / fps);  // the frame shown is the first one from here on
            const std::pair<VideoFrame, double>* hit = nullptr;
            if (!got.empty() && got.front().second <= from + 1e-9)  // read already, unless it's still to come
                for (const auto& g : got)
                    if (g.second >= from) {
                        hit = &g;
                        break;
                    }
            if (!hit) {
                if (got.empty() || from < got.front().second || t - got.back().second >= 1.0) {  // stepping forward reads on; anything else seeks
                    r.Seek(t);
                    got.clear();
                }
                // Only frames that may be shown come with their pictures: the one asked for, and before a step the
                // ones a step back would show.
                const double pictures = exact ? from - (double)keep / fps : from;
                VideoFrame f;
                double ft = 0;
                while (!quit_ && (got.empty() || got.back().second < from) && r.ReadFrame(&f, &ft, pictures)) {
                    got.emplace_back(f, ft);
                    while (got.size() > keep || (got.size() > 1 && !got.front().first.HasPicture())) got.pop_front();
                }
                if (!got.empty()) hit = &got.back();
            }
            BitmapPtr fitted = hit ? hit->first.Bgra() : nullptr;  // sequence-sized, like the playing frames
            if (!fitted) continue;
            auto* res = new Result{fitted, hit->second};
            if (!PostMessageW(hwnd_, WM_FETCHED, 0, (LPARAM)res)) delete res;
        }
        CoUninitialize();
    }

    Sequence seq_;
    HWND hwnd_;
    std::mutex mu_;
    std::condition_variable cv_;
    double want_ = -1, tol_ = -1;
    std::atomic<bool> quit_{false};  // also checked mid-decode, so replacing the fetcher never waits long
    std::thread worker_;
};

// The overlay videos' pictures for the preview: decodes each overlay's frame at the time asked for (the newest request
// replacing the rest) on a worker thread and posts them back as WM_OVERLAYS. Playback asks in order, so it reads on.
class OverlayFetcher {
public:
    struct Result {
        double t;
        std::vector<std::pair<uint64_t, BitmapPtr>> pictures;  // by overlay id
    };
    explicit OverlayFetcher(HWND hwnd) : hwnd_(hwnd), worker_([this] { Work(); }) {}
    ~OverlayFetcher() {
        {
            std::lock_guard l(mu_);
            quit_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }
    void Request(double t, std::vector<Overlay> overlays) {
        {
            std::lock_guard l(mu_);
            want_ = t;
            overlays_ = std::move(overlays);
        }
        cv_.notify_one();
    }

private:
    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        OverlayFrames frames;
        for (;;) {
            double t;
            std::vector<Overlay> overlays;
            {
                std::unique_lock l(mu_);
                cv_.wait(l, [&] { return quit_ || want_ >= 0; });
                if (quit_) break;
                t = want_;
                overlays = overlays_;
                want_ = -1;
            }
            auto* res = new Result{t, {}};
            for (const auto& [id, f] : frames.At(t, overlays))
                if (BitmapPtr b = f.Bgra()) res->pictures.push_back({id, b});
            if (!PostMessageW(hwnd_, WM_OVERLAYS, 0, (LPARAM)res)) delete res;
        }
        CoUninitialize();
    }

    HWND hwnd_;
    std::mutex mu_;
    std::condition_variable cv_;
    double want_ = -1;
    std::vector<Overlay> overlays_;
    bool quit_ = false;
    std::thread worker_;
};

// The zoomed-in filmstrip's pictures: decodes the frames asked for (in timeline order, the newest request replacing the
// rest) on a worker thread, scales each to `thumb` and posts it back as WM_STRIP. Reads on from one frame to the next,
// so a view's worth of frames a few apart costs one seek.
class StripFetcher {
public:
    struct Result {
        uint64_t gen;
        size_t frame;
        BitmapPtr thumb;
    };
    StripFetcher(Sequence seq, TimelineFrames tf, SIZE thumb, HWND hwnd, uint64_t gen)
        : seq_(std::move(seq)), tf_(std::move(tf)), thumb_(thumb), hwnd_(hwnd), gen_(gen), worker_([this] { Work(); }) {}
    ~StripFetcher() {
        {
            std::lock_guard l(mu_);
            quit_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }
    void Want(std::vector<size_t> frames) {  // these, in this order, instead of whatever was asked before
        {
            std::lock_guard l(mu_);
            want_ = std::move(frames);
        }
        cv_.notify_one();
    }

private:
    void Work() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SequenceReader r;
        const bool ok = r.Open(seq_);
        double lastT = -1e300;
        for (;;) {
            size_t n;
            {
                std::unique_lock l(mu_);
                cv_.wait(l, [&] { return quit_ || !want_.empty(); });
                if (quit_) break;
                n = want_.front();
                want_.erase(want_.begin());
            }
            if (!ok || n >= tf_.size()) continue;
            const double t = tf_.frames[n].t;
            if (!(t > lastT && t - lastT < 2.0)) r.Seek(t);  // reading on beats seeking for frames close ahead
            VideoFrame f, kept;
            double ft = 0;
            while (!quit_ && r.ReadFrame(&f, &ft, t - 5e-4)) {  // frames before it come without pictures
                kept = f;
                lastT = ft;
                if (ft >= t - 5e-4) break;
            }
            const BitmapPtr full = kept ? kept.Bgra() : nullptr;
            if (!full) {
                lastT = -1e300;
                continue;
            }
            auto* res = new Result{gen_, n, Resample(*full, thumb_.cx, thumb_.cy)};
            if (!res->thumb || !PostMessageW(hwnd_, WM_STRIP, 0, (LPARAM)res)) delete res;
        }
        CoUninitialize();
    }

    Sequence seq_;
    TimelineFrames tf_;
    SIZE thumb_;
    HWND hwnd_;
    uint64_t gen_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<size_t> want_;
    std::atomic<bool> quit_{false};
    std::thread worker_;
};

// Results posted back from worker threads (one definition, shared by sender and receiver).
struct TranscribeResult {
    bool ok = false;
    std::vector<Caption> caps;
    std::wstring err;
};
struct SaveResult {
    bool ok, gif;
    std::wstring out, tmp, err;
    bool review = false;          // a review video, with its notes list (and sheet) next to it
    std::wstring md, mdText, sheet;
    double underMB = 0;           // "Save smaller": the size it had to fit
    bool copy = false;            // "Copy clip": the file goes on the clipboard once it's in place
    double seconds = 0;           // its length
};
struct FrameTimesResult {
    std::wstring key;
    std::shared_ptr<const std::vector<double>> times;
};

// ---------- the window ----------

struct Hot {
    RECT r{};
    std::function<void()> click;
    std::wstring tip;
};

class VideoEditor;
std::vector<VideoEditor*> g_editors;

class VideoEditor {
public:
    explicit VideoEditor(std::wstring p) : path(std::move(p)) {}

    HWND hwnd = nullptr;
    std::wstring path;
    float s = 1;
    bool snapshotMode = false;

    std::unique_ptr<SequencePlayer> player;
    std::unique_ptr<FrameFetcher> fetcher;
    // Every frame of the timeline (for stepping and the frame readout), from each file's frame times, read in the
    // background (by lowercase path; a grid at the clip's rate until they come).
    std::map<std::wstring, std::shared_ptr<const std::vector<double>>> frameTimes;
    std::set<std::wstring> framesPending;
    TimelineFrames tframes;
    SIZE videoSize{16, 9};  // the sequence frame: the first video's size
    double duration = 0;    // of all clips
    std::vector<Clip> builtClips;  // what the player, fetcher and thumbnails were made for
    std::optional<uint64_t> selClip;
    uint64_t thumbGen = 0;
    std::shared_ptr<std::atomic<uint64_t>> thumbLatest = std::make_shared<std::atomic<uint64_t>>(0);  // shared with the jobs
    std::vector<Clip> transcribedClips;  // what the running auto-captions job is transcribing
    std::wstring app, window;

    VideoEdit edit;
    std::vector<VideoEdit> undoStack;
    bool dirty = false;
    std::optional<uint64_t> selected;
    bool cropping = false;
    int aspect = 0;
    bool playing = false;
    double paused = 0;  // the playhead while paused
    std::optional<double> lastReplay;  // tests: where the last replay started

    BitmapPtr raw, shown;
    double rawT = 0;
    std::vector<BitmapPtr> thumbs;
    bool busy = false;
    bool saving = false;  // an export is running (quitting waits for it)
    std::wstring lastSaveError;  // tests

    HWND field1 = nullptr, field2 = nullptr;
    WNDPROC editProc = nullptr;
    bool settingText = false;
    uint64_t fieldsFor = 0;
    int fieldsKind = -1;

    HFONT fUi = nullptr, fSmall = nullptr, fIcon = nullptr, fIconSmall = nullptr, fMono = nullptr, fEmoji = nullptr, fMonoBig = nullptr;
    bool goingTo = false;  // Ctrl+G: the field takes a frame number or a time
    std::vector<Hot> hots;
    RECT hoverRect{};

    // The marked range (Shift+drag on the timeline, Shift+I / Shift+O): the part Ctrl+C copies as a clip. A selection,
    // not an edit: on frame boundaries, inside the trim, never in the undo stack or Save. Esc, a plain click outside it
    // or a change to the clips clears it.
    struct Span {
        double start = 0, end = 0;
    };
    std::optional<Span> marked;

    enum class DragKind { None, Crop, Move, Handle, Caption, TrimStart, TrimEnd, Playhead, Item, ClipMove, ClipIn, ClipOut, Pan, Range, BinMove, OverlayItem, OverlayBox };
    struct Drag {
        DragKind kind = DragKind::None;
        VPoint from;
        Mark mark;
        int handle = 0;
        uint64_t id = 0;
        VRect rect;
        bool isCaption = false;
        int edge = 0;
        double grab = 0, s0 = 0, e0 = 0;
        size_t clip = 0;      // clip drags: which clip (bin drags: which bin piece)
        double value = 0;     // the new in or out point
        int target = -1;      // where a moved clip goes (-1 = it hasn't moved, or isn't over the clip lane)
        bool toBin = false;   // a clip dragged up over the bin: it goes there
        bool toOverlay = false;  // a bin piece over the overlay lane: it goes on top, at `value`
        Overlay overlay;      // overlay drags: the overlay as it was
        bool moved = false;   // a clip or bin piece dragged far enough to be a move, not a click
        int downX = 0, downY = 0;
        POINT at{};           // the mouse, for the piece carried along
        std::optional<Span> before;  // range drags: the marked range before it (Esc puts it back)
    } drag;

    int S(int v) const { return Px(s, v); }

    // ---- state ----

    double Now() const { return playing && player ? player->Now() : paused; }
    std::optional<size_t> SelCaption() const {
        if (!selected) return std::nullopt;
        for (size_t i = 0; i < edit.captions.size(); ++i)
            if (edit.captions[i].id == *selected) return i;
        return std::nullopt;
    }
    std::optional<size_t> SelMark() const {
        if (!selected) return std::nullopt;
        for (size_t i = 0; i < edit.marks.size(); ++i)
            if (edit.marks[i].id == *selected) return i;
        return std::nullopt;
    }
    std::optional<size_t> SelNote() const {
        if (!selected) return std::nullopt;
        for (size_t i = 0; i < edit.notes.size(); ++i)
            if (edit.notes[i].id == *selected) return i;
        return std::nullopt;
    }
    std::optional<size_t> SelOverlay() const {
        if (!selected) return std::nullopt;
        for (size_t i = 0; i < edit.overlays.size(); ++i)
            if (edit.overlays[i].id == *selected) return i;
        return std::nullopt;
    }
    VRect ViewRect() const {
        const VRect f{0, 0, (double)videoSize.cx, (double)videoSize.cy};
        if (!edit.crop) return f;
        const double x0 = std::max(f.x, edit.crop->x), y0 = std::max(f.y, edit.crop->y);
        const double x1 = std::min(f.MaxX(), edit.crop->MaxX()), y1 = std::min(f.MaxY(), edit.crop->MaxY());
        if (x1 - x0 < 16 || y1 - y0 < 16) return f;
        return {x0, y0, x1 - x0, y1 - y0};
    }

    void PushUndo() {
        undoStack.push_back(edit);
        if (undoStack.size() > 200) undoStack.erase(undoStack.begin());
        dirty = true;
    }

    void Undo() {
        if (undoStack.empty()) return;
        edit = undoStack.back();
        undoStack.pop_back();
        if (selected && !SelCaption() && !SelMark() && !SelNote() && !SelOverlay()) selected.reset();
        if (selClip && !SelClipIndex() && !SelBinIndex()) selClip.reset();
        Changed();
    }

    // Anything in the edit changed: the preview, the timeline and the fields follow.
    void Changed() {
        if (!SameFootage(edit.clips, builtClips)) RebuildSequence();
        else builtClips = edit.clips;  // only a flip: the players and thumbnails stay (the renderer and strip flip)
        if (marked) SetMarked(marked->start, marked->end);  // still inside the trim
        RequestOverlays(playing ? rawT : paused);
        LoadNotes();
        PlaceNotes();
        NotesChanged();
        if (player) player->SetMuted(edit.muted);
        Rerender();
        SyncFields();
        Invalidate();
    }

    void Select(std::optional<uint64_t> id) {
        if (id) selClip.reset();
        if (id == selected) return;
        selected = id;
        Rerender();
        SyncFields();
        Invalidate();
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Rerender() {
        if (!raw) return;
        FrameRenderer r(edit, videoSize, true);
        r.zoomInPreview = playing;
        if (!playing && selected) r.settled = *selected;  // a new item shows at the playhead, not faded out at its first frame
        r.overlayFrame = OverlayPictures();
        shown = r.Render(*raw, playing ? rawT : paused);
        if (mono && shown) Grayscale(*shown);
    }

    // ---- composition guides and the monochrome view ----
    // Lines over the preview to check framing (golden section, thirds, phi grid, diagonals), on the part the saved video
    // shows (the crop), and a black-and-white view to check values. The preview's only: never saved. They stay as set for
    // the next video opened.
    static inline std::array<bool, kGuideKinds> lastGuides{};
    static inline bool lastGuidesOn = false, lastMono = false;
    std::array<bool, kGuideKinds> guides = lastGuides;
    bool guidesOn = lastGuidesOn;  // Hide guides (G) keeps which ones were on
    bool mono = lastMono;
    bool GuideShown(GuideKind k) const { return guidesOn && guides[(size_t)k]; }
    bool AnyGuide() const {
        for (int k = 0; k < kGuideKinds; ++k)
            if (GuideShown((GuideKind)k)) return true;
        return false;
    }
    void GuidesChanged() {
        lastGuides = guides;
        lastGuidesOn = guidesOn;
        lastMono = mono;
        Invalidate();
    }
    void ToggleGuide(GuideKind k) {
        bool& on = guides[(size_t)k];
        if (!guidesOn) guidesOn = on = true;  // from hidden: just this one comes back on
        else on = !on;
        GuidesChanged();
    }
    void ShowAllGuides() {
        guides.fill(true);
        guidesOn = true;
        GuidesChanged();
    }
    void HideGuides() {
        guidesOn = false;
        GuidesChanged();
    }
    // G: hides the guides, or shows them again (the rule of thirds when none was picked yet).
    void ToggleGuides() {
        if (AnyGuide()) return HideGuides();
        guidesOn = true;
        if (!AnyGuide()) guides[(size_t)GuideKind::Thirds] = true;
        GuidesChanged();
    }
    void ToggleMono() {
        mono = !mono;
        Rerender();
        GuidesChanged();
    }
    static gp::Color GuideColor(GuideKind k) {
        switch (k) {
            case GuideKind::Golden: return gp::Color(235, 235, 40, 40);    // red
            case GuideKind::Thirds: return gp::Color(235, 50, 90, 255);    // blue
            case GuideKind::Phi: return gp::Color(235, 235, 215, 40);      // yellow
            default: return gp::Color(220, 255, 236, 120);                // pale yellow
        }
    }
    static const wchar_t* GuideColorName(GuideKind k) {
        switch (k) {
            case GuideKind::Golden: return L"red";
            case GuideKind::Thirds: return L"blue";
            case GuideKind::Phi: return L"yellow";
            default: return L"pale yellow";
        }
    }
    std::vector<MenuItem> GuidesMenu() {
        std::vector<MenuItem> v;
        MenuItem all;
        all.label = L"Show all guides";
        all.run = [this] { ShowAllGuides(); };
        v.push_back(all);
        MenuItem none;
        none.label = L"Hide guides\tG";
        none.enabled = AnyGuide();
        none.run = [this] { HideGuides(); };
        v.push_back(none);
        v.push_back(MenuItem::Sep());
        for (int k = 0; k < kGuideKinds; ++k) {
            const GuideKind kind = (GuideKind)k;
            MenuItem it;
            it.label = std::wstring(GuideLabel(kind)) + L" (" + GuideColorName(kind) + L")";
            it.checked = GuideShown(kind);
            it.run = [this, kind] { ToggleGuide(kind); };
            v.push_back(it);
        }
        v.push_back(MenuItem::Sep());
        MenuItem bw;
        bw.label = L"Monochrome view";
        bw.checked = mono;
        bw.run = [this] { ToggleMono(); };
        v.push_back(bw);
        return v;
    }
    static void Grayscale(Bitmap& b) {
        uint32_t* p = b.Bits();
        for (size_t i = 0, n = (size_t)b.Width() * b.Height(); i < n; ++i) {
            const uint32_t c = p[i];
            const uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 150 + (c & 255) * 29) >> 8;  // Rec. 601 luma
            p[i] = (c & 0xFF000000u) | y << 16 | y << 8 | y;
        }
    }
    void PaintGuides(gp::Graphics& g) {
        if (!AnyGuide()) return;
        const gp::RectF a = ToView(ViewRect());  // what the saved video shows
        gp::Pen shade(gp::Color(110, 0, 0, 0), 3.25f * s);  // a dark edge under each line, so it reads on any picture
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < kGuideKinds; ++k) {
                const GuideKind kind = (GuideKind)k;
                if (!GuideShown(kind)) continue;
                gp::Pen pen(GuideColor(kind), 1.25f * s);
                for (const auto& [p, q] : GuideLines(kind, {a.X, a.Y, a.Width, a.Height}))
                    g.DrawLine(pass ? &pen : &shade, (float)p.x, (float)p.y, (float)q.x, (float)q.y);
            }
    }
    POINT rDown{};  // where a right click started (a click without a drag opens the guides menu)

    // ---- overlay videos in the preview ----
    std::unique_ptr<OverlayFetcher> overlayFetcher;
    std::map<uint64_t, BitmapPtr> overlayPics;  // each overlay's picture at the frame on screen (as last decoded)
    double overlayPicsT = -1;                   // the time they're for
    // For a FrameRenderer drawing the frame on screen: each overlay's picture as last decoded.
    std::function<BitmapPtr(const Overlay&, double)> OverlayPictures() const {
        return [this](const Overlay& o, double) -> BitmapPtr {
            const auto it = overlayPics.find(o.id);
            return it != overlayPics.end() ? it->second : nullptr;
        };
    }

    // Asks for the overlays' pictures at timeline time `t` (they come as WM_OVERLAYS).
    void RequestOverlays(double t) {
        if (edit.overlays.empty() || !hwnd) {
            overlayPics.clear();
            return;
        }
        if (!overlayFetcher) overlayFetcher = std::make_unique<OverlayFetcher>(hwnd);
        overlayFetcher->Request(t, edit.overlays);
    }

    // ---- playback ----

    void Seek(double t) {
        t = std::clamp(t, 0.0, std::max(0.0, duration));
        paused = t;
        KeepOnView(t);
        if (player) player->Seek(t);
        if (!playing) Fetch(t);
        Invalidate();
    }

    // The paused preview decodes the frame nearest `t`, exactly that one (by its own time).
    void Fetch(double t) {
        if (!fetcher) return;
        if (tframes.empty()) return fetcher->Request(t);
        fetcher->Request(tframes.frames[tframes.Nearest(t)].t, 5e-4);
    }

    // ---- frames ----

    static std::wstring FileKey(const std::wstring& path) { return Lower(path); }
    bool FramesKnown() const {
        for (const auto& c : edit.clips)
            if (!frameTimes.count(FileKey(c.path))) return false;
        return true;
    }
    // Reads the frame times of files that are new to the editor, each on a thread of its own.
    void LoadFrameTimes() {
        for (const auto& c : edit.clips) {
            const std::wstring key = FileKey(c.path);
            if (frameTimes.count(key) || framesPending.count(key) || !hwnd) continue;
            framesPending.insert(key);
            std::thread([self = hwnd, path = c.path, key] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                auto* r = new FrameTimesResult{key, std::make_shared<const std::vector<double>>(FrameTimes(path))};
                CoUninitialize();
                if (!PostMessageW(self, WM_FRAMES, 0, (LPARAM)r)) delete r;
            }).detach();
        }
    }
    // ---- the zoomed-in filmstrip ----
    // Zoomed in, the strip is tiles at the thumbnails' own shape, each showing a frame from under it (the first frame
    // starting in it), decoded in the background as the view changes; an empty tile until its picture comes.
    std::unique_ptr<StripFetcher> stripFetcher;
    uint64_t stripGen = 0;
    std::map<size_t, BitmapPtr> stripThumbs;  // by timeline frame
    std::vector<size_t> stripWanted;          // last asked for
    struct StripTile {
        double t0, t1;  // the timeline time it covers
        size_t frame;   // the frame it shows
    };
    SIZE StripThumbSize() const {
        const int h = S(52);
        return {std::max(8, (int)std::lround(h * (double)videoSize.cx / std::max(1L, videoSize.cy))), h};
    }
    // The tiles on view at this zoom, on a grid from the timeline's start (so they stay put while panning).
    std::vector<StripTile> StripTiles() const {
        std::vector<StripTile> v;
        if (tframes.empty() || tlZoom <= 1.001) return v;
        const RECT tr = TimelineRect();
        const double dt = StripThumbSize().cx * TlSpan() / std::max(1, RectW(tr));
        const double end = std::min(duration, tlStart + TlSpan());
        for (long long j = (long long)std::floor(tlStart / dt); j * dt < end; ++j) {
            const double t0 = j * dt, t1 = std::min(duration, (j + 1) * dt);
            const auto it = std::lower_bound(tframes.frames.begin(), tframes.frames.end(), t0 - 1e-9, [](const TimelineFrames::Frame& f, double x) { return f.t < x; });
            const size_t n = it != tframes.frames.end() && it->t < t1 ? (size_t)(it - tframes.frames.begin()) : tframes.At(t0);
            v.push_back({t0, t1, n});
        }
        return v;
    }
    void ResetStrip() {
        stripFetcher.reset();
        stripThumbs.clear();
        stripWanted.clear();
        ++stripGen;
    }
    // Asks for the pictures of the tiles on view that aren't there yet.
    void RequestStrip(const std::vector<StripTile>& tiles) {
        std::vector<size_t> want;
        for (const auto& t : tiles)
            if (!stripThumbs.count(t.frame) && std::find(want.begin(), want.end(), t.frame) == want.end()) want.push_back(t.frame);
        if (want == stripWanted) return;
        stripWanted = want;
        if (want.empty() || !hwnd) return;
        if (!stripFetcher) stripFetcher = std::make_unique<StripFetcher>(Seq(), tframes, StripThumbSize(), hwnd, stripGen);
        stripFetcher->Want(want);
    }
    bool StripComplete() const {  // tests: every tile on view has its picture
        for (const auto& t : StripTiles())
            if (!stripThumbs.count(t.frame)) return false;
        return true;
    }

    void RebuildFrames() {
        ResetStrip();
        tframes = TimelineFrames::Of(edit.clips, [this](const Clip& c) -> const std::vector<double>* {
            const auto it = frameTimes.find(FileKey(c.path));
            return it != frameTimes.end() && it->second && !it->second->empty() ? it->second.get() : nullptr;
        });
        PlaceNotes();
    }
    size_t FrameNow() const { return tframes.Nearest(playing ? rawT : paused); }  // the frame on screen
    void GoToFrame(size_t n) {
        if (n < tframes.size()) Seek(tframes.frames[n].t);
    }

    // ---- loop ----
    // On, playback goes back to the start of the loop at its end instead of stopping: the selected clip (as much of it as
    // is in the trim), else the trim. Review playback (J/L) wraps the same way. A preview setting, not part of the edit;
    // it stays on for the next video opened.
    static inline bool lastLoop = false;
    bool loop = lastLoop;
    int loops = 0;  // tests: times playback went round
    std::pair<double, double> LoopRange() const {
        double a = edit.trimStart, b = edit.trimEnd;
        if (const auto i = SelClipIndex()) {
            const double c0 = ClipStart(edit.clips, *i), c1 = c0 + edit.clips[*i].Duration();
            if (std::max(a, c0) < std::min(b, c1) - 0.05) a = std::max(a, c0), b = std::min(b, c1);
        }
        return {a, b};
    }
    void ToggleLoop() {
        lastLoop = loop = !loop;
        Invalidate();
    }

    void Play() {
        if (!player) return;
        const auto [from, to] = loop ? LoopRange() : std::pair{edit.trimStart, edit.trimEnd};
        if (Now() < from || Now() >= to - 0.05) Seek(from);
        player->Seek(paused);
        player->SetRate(edit.speed);
        player->SetMuted(edit.muted);
        player->Play();
        playing = true;
        frameCopyPending = false;  // a Ctrl+C still waiting for its frame doesn't fire later
        Rerender();
        Invalidate();
    }

    void Pause() {
        StopReview();
        if (!playing) return;
        paused = player ? player->Now() : paused;
        if (player) player->Pause();
        playing = false;
        Fetch(paused);  // the exact frame (and zoom off) for editing
        Rerender();
        Invalidate();
    }

    void TogglePlay() { playing || reviewDir ? Pause() : Play(); }

    // ---- review playback (J/K/L) ----
    // L plays forward, J backward, K pauses; pressing L (or J) again while it plays that way cycles the preview speed
    // 0.25× → 0.5× → 1×. Forward at 1× is the usual playback (with sound). Slower, or backward, the paused preview
    // shows every frame in turn, each decoded exactly, at that pace (or slower when decoding can't keep up: it never
    // skips one). The speed is the preview's only: the edit, and so what Save writes, stays as it is.
    static constexpr double kPreviewRates[] = {0.25, 0.5, 1};
    int reviewDir = 0;          // 1 forward, -1 backward: the exact-frame playback is on
    double previewRate = 1;     // kept for the next J or L
    double reviewDue = 0;       // when the next frame is due (seconds on the steady clock)
    static double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    double NextPreviewRate() const {
        for (size_t i = 0; i < std::size(kPreviewRates); ++i)
            if (std::fabs(kPreviewRates[i] - previewRate) < 1e-9) return kPreviewRates[(i + 1) % std::size(kPreviewRates)];
        return 1;
    }
    bool Reviewing(int dir) const { return dir > 0 ? (reviewDir > 0 || playing) : reviewDir < 0; }
    void Review(int dir) {
        if (tframes.empty()) return;
        if (Reviewing(dir)) previewRate = NextPreviewRate();
        if (dir > 0 && previewRate == 1) {  // the usual playback, with sound
            StopReview();
            if (!playing) Play();
            Invalidate();
            return;
        }
        if (playing) Pause();
        if (reviewDir != dir) {
            const size_t n = tframes.Nearest(paused);
            GoToFrame(n);  // starts from the frame on screen
            reviewDir = dir;
            reviewDue = Seconds() + FrameStep(n, dir);
        }
        Invalidate();
    }
    void StopReview() {
        if (!reviewDir) return;
        reviewDir = 0;
        Invalidate();
    }
    // How long frame n shows going `dir` way at the preview speed.
    double FrameStep(size_t n, int dir) const {
        const size_t m = dir > 0 ? std::min(n + 1, tframes.size() - 1) : n > 0 ? n - 1 : 0;
        double d = std::fabs(tframes.frames[m].t - tframes.frames[n].t);
        if (d <= 0) d = 1 / tframes.Fps(n);
        return d / (previewRate * edit.speed);
    }
    // Each tick: once the frame shown has come and its time is up, on to the next one.
    void ReviewTick() {
        if (!reviewDir || tframes.empty()) return;
        if (!(raw && std::fabs(rawT - paused) < 1e-3)) return;  // still decoding the one asked for
        const double now = Seconds();
        if (now < reviewDue) return;
        const size_t n = tframes.Nearest(paused);
        const auto [from, to] = loop ? LoopRange() : std::pair{edit.trimStart, edit.trimEnd};
        const size_t first = tframes.At(from + 1e-3), last = tframes.At(to - 1e-3);
        size_t next = reviewDir > 0 ? n + 1 : n - 1;
        if ((reviewDir > 0 && n >= last) || (reviewDir < 0 && n <= first)) {  // at the end: round again, or stop
            if (!loop) return StopReview();
            next = reviewDir > 0 ? first : last;
            ++loops;
        }
        GoToFrame(next);
        reviewDue = std::max(reviewDue + FrameStep(n, reviewDir), now);  // no catching up by skipping
        ++reviewFrames;
    }
    int reviewFrames = 0;  // tests: frames shown by review playback
    std::function<void()> onFetched;  // tests: each paused frame as it's shown
    std::function<void()> onOverlays;  // tests: each time the overlays' pictures come


    // ←/→: `frames` frames at the rate of the clip they're in, landing on each frame's own time.
    void Step(int frames) {
        Pause();
        if (tframes.empty()) return Seek(Now() + frames / 30.0);
        const long long n = (long long)tframes.Nearest(paused) + frames;
        GoToFrame((size_t)std::clamp<long long>(n, 0, (long long)tframes.size() - 1));
    }
    // Shift+←/→: the frame nearest a second away.
    void StepSecond(int dir) {
        Pause();
        if (tframes.empty()) return Seek(Now() + dir);
        GoToFrame(tframes.Nearest(tframes.frames[tframes.Nearest(paused)].t + dir));
    }
    // Home/End: the first and last frames of the trim (the saved video's first and last).
    void GoTrimEnd(bool end) {
        Pause();
        if (tframes.empty()) return Seek(end ? edit.trimEnd : edit.trimStart);
        GoToFrame(tframes.At(end ? edit.trimEnd - 1e-3 : edit.trimStart + 1e-3));
    }

    void Replay(const Mark& m) {
        Pause();
        const double from = std::max(edit.trimStart, m.start - 0.4);
        lastReplay = from;
        Seek(from);
        Play();
    }

    void Tick() {
        ReviewTick();
        if (!player) return;
        double t = 0;
        if (playing) {
            if (auto f = player->NewFrame(&t)) {
                raw = f;  // already fitted into the sequence frame
                rawT = t;
                RequestOverlays(t);
                KeepOnView(t);
                Rerender();
                Invalidate();  // only when there's a new frame (the playhead moves with it)
            }
            const auto [from, to] = loop ? LoopRange() : std::pair{edit.trimStart, edit.trimEnd};
            const bool atEnd = player->Now() >= to - 0.01;
            if (loop && atEnd) {  // round again, still playing
                player->Seek(from);
                if (!player->Playing()) player->Play();
                KeepOnView(from);
                ++loops;
            } else if (atEnd || !player->Playing()) {
                Pause();
                Seek(edit.trimStart);
            }
        } else {
            player->NewFrame(&t);  // drains the engine; paused frames come from the fetcher
        }
    }

    // ---- clips ----

    Sequence Seq() const { return Sequence{edit.clips, videoSize}; }
    std::optional<size_t> SelClipIndex() const {
        for (size_t i = 0; selClip && i < edit.clips.size(); ++i)
            if (edit.clips[i].id == *selClip) return i;
        return std::nullopt;
    }
    // A selected bin piece (selClip names it: a clip keeps its id going to the bin and back).
    std::optional<size_t> SelBinIndex() const {
        for (size_t i = 0; selClip && i < edit.bin.size(); ++i)
            if (edit.bin[i].id == *selClip) return i;
        return std::nullopt;
    }

    void SelectClip(std::optional<uint64_t> id) {
        if (id) selected.reset();
        selClip = id;
        SyncFields();
        Rerender();
        Invalidate();
    }

    // The player, the paused-frame decoder and the thumbnails follow the clips (after edits and undo).
    void RebuildSequence() {
        builtClips = edit.clips;
        marked.reset();  // its times would now show other footage
        if (playing && player) paused = player->Now();  // the playhead stays where playback was
        playing = false;
        duration = ClipsDuration(edit.clips);
        paused = std::clamp(paused, 0.0, std::max(0.0, duration - 0.01));
        if (player) {
            player->SetSequence(Seq());
            player->Seek(paused);
        }
        RebuildFrames();
        if (hwnd) {
            fetcher = std::make_unique<FrameFetcher>(Seq(), hwnd);
            Fetch(paused);
            thumbs.clear();  // the old ones would be stretched over the wrong clips
            LoadThumbs();
            LoadFrameTimes();
        }
        if (selClip && !SelClipIndex() && !SelBinIndex()) selClip.reset();
        ClampTimeline();
    }

    void LoadThumbs() {
        HWND self = hwnd;
        const Sequence sq = Seq();
        const WPARAM gen = (WPARAM)++thumbGen;
        thumbLatest->store(gen);
        std::thread([sq, self, gen, latest = thumbLatest] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // A newer job (or closing the editor) makes this one stop between frames.
            auto* t = new std::vector<BitmapPtr>(VideoThumbnails(sq, 16, 240, [&] { return latest->load() != gen; }));
            CoUninitialize();
            if (!PostMessageW(self, WM_THUMBS, gen, (LPARAM)t)) delete t;
        }).detach();
    }

    // One undo step: the new clip list, with markup and captions moved along with their footage.
    void SetClips(std::vector<Clip> clips) {
        if (clips.empty() || clips == edit.clips) return;
        Pause();
        PushUndo();
        ApplyClips(edit, std::move(clips));
        Changed();
    }

    // Joins videos after the selected clip (or at the end). Any shape works: each is fitted into the frame.
    void AddClips(const std::vector<std::wstring>& paths) {
        std::vector<Clip> add;
        std::wstring bad;
        for (const auto& p : paths) {
            if (auto c = IsVideoFile(p) ? ClipOf(p) : std::nullopt) add.push_back(*c);
            else bad += (bad.empty() ? L"" : L", ") + FileNameOf(p);
        }
        if (!bad.empty()) ShowToast(L"Can't add that as a video", bad, nullptr, nullptr, 4000);
        if (add.empty()) return;
        std::vector<Clip> clips = edit.clips;
        const size_t at = SelClipIndex() ? *SelClipIndex() + 1 : clips.size();
        clips.insert(clips.begin() + at, add.begin(), add.end());
        SetClips(clips);
        SelectClip(add.front().id);
        Seek(ClipStart(edit.clips, at));
    }

    // Files dropped at `at`: on the bottom lane they go on top from that time, on a box on the video they go on top from
    // the playhead (each in a box of its own); anywhere else they join the sequence.
    void DropFiles(const std::vector<std::wstring>& files, POINT at) {
        const RECT ol = OverlayLaneRect(), st = StageRect();
        const bool onLane = at.y >= ol.top - S(4) && at.y <= ol.bottom + S(4) && at.x >= ol.left - S(10);
        const bool onBox = PtInRect(&st, at) && OverlayAt(at).has_value();
        if (!onLane && !onBox) return AddClips(files);
        double t = onLane ? SnapToFrame(TT(at.x)) : InsertTime();
        for (const auto& p : files) {
            if (auto c = IsVideoFile(p) ? ClipOf(p) : std::nullopt) {
                AddOverlay(*c, t, OverlayLayout::Pip);
                t += c->Duration();  // several: one after another
            } else {
                ShowToast(L"Can't add that as a video", FileNameOf(p), nullptr, nullptr, 4000);
            }
        }
    }

    void AddClipDialog() {
        const auto files = PickFiles(hwnd, MediaFilter(false, true), true, L"Add videos after this one");
        if (!files.empty()) AddClips(files);
    }

    // Cuts the clip under the playhead in two (then a middle part can be removed, or the halves reordered).
    void SplitAtPlayhead() {
        const double t = Now();
        const auto spot = LocateClip(edit.clips, t);
        if (!spot) return;
        const Clip& c = edit.clips[spot->first];
        if (spot->second - c.in < 0.1 || c.out - spot->second < 0.1) {
            ShowToast(L"Move the playhead into a clip to split it", L"", nullptr, nullptr, 2000);
            return;
        }
        std::vector<Clip> clips = edit.clips;
        Clip second = c;
        second.id = NewItemId();
        second.in = spot->second;
        clips[spot->first].out = spot->second;
        clips.insert(clips.begin() + spot->first + 1, second);
        SetClips(clips);
        SelectClip(second.id);
        Seek(t);
    }

    void MoveClip(size_t from, size_t to) {
        if (from >= edit.clips.size() || to >= edit.clips.size() || from == to) return;
        std::vector<Clip> clips = edit.clips;
        const Clip c = clips[from];
        clips.erase(clips.begin() + from);
        clips.insert(clips.begin() + to, c);
        SetClips(clips);
        Seek(ClipStart(edit.clips, to));
    }

    // One undo step for the clips and the bin together (a piece going from one to the other).
    void SetClipsAndBin(std::vector<Clip> clips, std::vector<Clip> bin) {
        if (clips.empty() || (clips == edit.clips && bin == edit.bin)) return;
        Pause();
        PushUndo();
        edit.bin = std::move(bin);
        ApplyClips(edit, std::move(clips));
        Changed();
    }

    // Takes a clip off the timeline into the bin, to put back later or delete there. The last clip stays.
    // Markup and captions on it go, as when a clip is cut shorter.
    void MoveToBin(size_t i) {
        if (i >= edit.clips.size()) return;
        if (edit.clips.size() < 2) {
            ShowToast(L"The last clip stays on the timeline", L"", nullptr, nullptr, 2000);
            return;
        }
        std::vector<Clip> clips = edit.clips, bin = edit.bin;
        bin.push_back(clips[i]);
        clips.erase(clips.begin() + i);
        SetClipsAndBin(clips, bin);
        SelectClip(std::nullopt);
        Seek(ClipStart(edit.clips, std::min(i, edit.clips.size() - 1)));
    }

    // Puts bin piece `i` back on the timeline as clip `at`.
    void PlaceFromBin(size_t i, size_t at) {
        if (i >= edit.bin.size()) return;
        std::vector<Clip> clips = edit.clips, bin = edit.bin;
        const Clip c = bin[i];
        bin.erase(bin.begin() + i);
        at = std::min(at, clips.size());
        clips.insert(clips.begin() + at, c);
        SetClipsAndBin(clips, bin);
        SelectClip(c.id);
        Seek(ClipStart(edit.clips, at));
    }

    // ---- overlay videos ----

    // Puts `clip` on top from timeline time `at` (picture in picture, or one half of a split screen), selected.
    void AddOverlay(const Clip& clip, double at, OverlayLayout layout) {
        Overlay o;
        o.clip = clip;
        o.start = std::clamp(at, 0.0, std::max(0.0, duration - 0.1));
        o.layout = layout;
        Pause();
        PushUndo();
        cropping = false;  // so the next drag on the video moves the box
        edit.overlays.push_back(o);
        Select(o.id);
        Changed();
    }
    void AddOverlayDialog(OverlayLayout layout) {
        const auto files = PickFiles(hwnd, MediaFilter(false, true), false,
                                     layout == OverlayLayout::Pip ? L"Video to show in a box on top" : L"Video to show side by side");
        if (files.empty()) return;
        if (auto c = IsVideoFile(files[0]) ? ClipOf(files[0]) : std::nullopt) AddOverlay(*c, InsertTime(), layout);
        else ShowToast(L"Can't add that as a video", FileNameOf(files[0]), nullptr, nullptr, 4000);
    }
    // Bin piece `i` goes on top from `at` (one undo step with taking it from the bin).
    void BinToOverlay(size_t i, double at) {
        if (i >= edit.bin.size()) return;
        Overlay o;
        o.clip = edit.bin[i];
        o.start = std::clamp(at, 0.0, std::max(0.0, duration - 0.1));
        Pause();
        PushUndo();
        cropping = false;
        edit.bin.erase(edit.bin.begin() + i);
        edit.overlays.push_back(o);
        Select(o.id);
        Changed();
    }
    // One undo step changing the selected overlay.
    void UpdateOverlay(const std::function<void(Overlay&)>& fn) {
        const auto i = SelOverlay();
        if (!i) return;
        PushUndo();
        fn(edit.overlays[*i]);
        Changed();
    }
    // The wheel over a box: `factor` times as wide, about its middle (a run of wheel turns is one undo step).
    uint64_t lastWheelScale = 0;  // GetTickCount64 of the last one
    void ScaleOverlay(size_t i, double factor) {
        if (i >= edit.overlays.size()) return;
        const uint64_t now = GetTickCount64();
        if (selected != edit.overlays[i].id || now - lastWheelScale > 800) PushUndo();
        lastWheelScale = now;
        Select(edit.overlays[i].id);
        Overlay& o = edit.overlays[i];
        const VRect v = ViewRect();
        const SIZE out{(LONG)v.w, (LONG)v.h};
        const VRect r0 = OverlayRect(o, out);
        o.width = std::clamp(o.width * factor, 0.08, 1.0);
        const VRect r1 = OverlayRect(o, out);
        o.pos = {(r0.MidX() - r1.w / 2) / v.w, (r0.MidY() - r1.h / 2) / v.h};
        const VRect r = OverlayRect(o, out);  // kept on the frame
        o.pos = {r.x / v.w, r.y / v.h};
        Changed();
    }

    // Where an overlay shows on the video (video coordinates), in the preview's frame.
    VRect OverlayOnVideo(const Overlay& o) const {
        const VRect v = ViewRect();
        return OverlayRect(o, {(LONG)v.w, (LONG)v.h}).Offset(v.x, v.y);
    }

    // Deletes bin piece `i` for good (undo brings it back).
    void DeleteFromBin(size_t i) {
        if (i >= edit.bin.size()) return;
        PushUndo();
        edit.bin.erase(edit.bin.begin() + i);
        SelectClip(std::nullopt);
        Changed();
    }

    void TrimClip(size_t i, std::optional<double> in, std::optional<double> out) {
        if (i >= edit.clips.size()) return;
        std::vector<Clip> clips = edit.clips;
        Clip& c = clips[i];
        const double shortest = std::min(0.2, c.Duration());
        if (in) c.in = std::clamp(*in, 0.0, c.out - shortest);
        if (out) c.out = std::clamp(*out, c.in + shortest, std::max(c.length, c.in + shortest));
        SetClips(clips);
    }

    // ---- editing ----

    void SetTrim(std::optional<double> start, std::optional<double> end) {
        if (start) edit.trimStart = std::min(std::max(0.0, *start), edit.trimEnd - 0.1);
        if (end) edit.trimEnd = std::max(std::min(duration, *end), edit.trimStart + 0.1);
        Changed();
    }
    void TrimAtPlayhead(bool start) {  // I and O
        PushUndo();
        start ? SetTrim(Now(), std::nullopt) : SetTrim(std::nullopt, Now());
    }

    double InsertTime() const { return std::min(std::max(Now(), edit.trimStart), std::max(edit.trimStart, edit.trimEnd - 0.5)); }

    void AddCaption() {
        PushUndo();
        Caption c;
        c.start = InsertTime();
        c.end = std::min(edit.trimEnd, c.start + 3);
        edit.captions.push_back(c);
        std::stable_sort(edit.captions.begin(), edit.captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
        selected = c.id;
        Changed();
        FocusField();
    }

    void AddMark(MarkKind k) {
        PushUndo();
        const VRect v = ViewRect();
        const double w = v.w, h = v.h;
        auto box = [&](double fw, double fh) { return std::make_pair(VPoint{v.MidX() - w * fw / 2, v.MidY() - h * fh / 2}, VPoint{v.MidX() + w * fw / 2, v.MidY() + h * fh / 2}); };
        auto [a, b] = box(0.3, 0.22);
        int color = 0, level = 2;
        std::wstring text;
        switch (k) {
            case MarkKind::Arrow:
                a = {v.x + w * 0.36, v.y + h * 0.66};
                b = {v.x + w * 0.5, v.y + h * 0.47};
                break;
            case MarkKind::Step: {
                const double q = h * 0.08;
                a = {v.MidX() - q / 2, v.MidY() - q / 2};
                b = {v.MidX() + q / 2, v.MidY() + q / 2};
                break;
            }
            case MarkKind::Text:
                a = {v.x + w * 0.08, v.y + h * 0.1};
                b = {v.x + w * 0.6, v.y + h * 0.2};
                color = 6;
                break;
            case MarkKind::Bubble:
                a = {v.MidX() - w * 0.16, v.y + h * 0.18};
                b = {v.MidX() + w * 0.16, v.y + h * 0.3};
                color = 6;
                break;
            case MarkKind::Emoji: {
                const double q = h * 0.14;
                a = {v.MidX() - q / 2, v.MidY() - q / 2};
                b = {v.MidX() + q / 2, v.MidY() + q / 2};
                text = L"✅";
                break;
            }
            case MarkKind::Zoom: std::tie(a, b) = box(0.4, 0.4); break;
            case MarkKind::Title:
                a = {v.x, v.y};
                b = {v.MaxX(), v.MaxY()};
                color = 7;
                break;
            default: break;
        }
        Mark m;
        m.kind = k;
        m.start = InsertTime();
        m.end = std::min(edit.trimEnd, m.start + KindDefaultSeconds(k));
        m.a = a;
        m.b = b;
        m.text = text;
        m.color = color;
        m.level = level;
        if (k == MarkKind::Step) {
            int mx = 0;
            for (const auto& o : edit.marks)
                if (o.kind == MarkKind::Step) mx = std::max(mx, o.step);
            m.step = mx + 1;
        }
        edit.marks.push_back(m);
        selected = m.id;
        Changed();
        if (k == MarkKind::Text || k == MarkKind::Bubble || k == MarkKind::Title) FocusField();
    }

    void UpdateMark(const std::function<void(Mark&)>& f) {
        if (auto i = SelMark()) {
            PushUndo();
            f(edit.marks[*i]);
            Changed();
        }
    }

    void UpdateCaption(const std::function<void(Caption&)>& f) {
        if (auto i = SelCaption()) {
            PushUndo();
            f(edit.captions[*i]);
            Changed();
        }
    }

    void SetCaptions(const std::function<void(VideoEdit&)>& f) {
        PushUndo();
        f(edit);
        Changed();
    }

    // Animation picks update the menu (title and checkmark) and replay the item so the choice shows.
    void PickAnimation(uint64_t id, const std::function<void(Mark&)>& f) {
        for (auto& m : edit.marks)
            if (m.id == id) {
                PushUndo();
                f(m);
                const Mark copy = m;
                Changed();
                Replay(copy);
                return;
            }
    }

    void DeleteSelected() {
        if (!selected) return;
        PushUndo();
        const uint64_t id = *selected;
        for (const auto& o : edit.overlays)  // an overlay video's footage goes to the bin, like a clip's
            if (o.id == id) {
                Clip c = o.clip;
                c.id = NewItemId();
                edit.bin.push_back(c);
            }
        std::erase_if(edit.overlays, [&](const Overlay& o) { return o.id == id; });
        std::erase_if(edit.captions, [&](const Caption& c) { return c.id == id; });
        std::erase_if(edit.marks, [&](const Mark& m) { return m.id == id; });
        std::erase_if(edit.notes, [&](const Note& n) { return n.id == id; });
        selected.reset();
        SetFocus(hwnd);
        Changed();
    }

    // ---- notes ----

    // The notes whose frames are on the timeline, in timeline order: (frame, index in edit.notes). Kept up to date by
    // PlaceNotes (after any change to the edit or the frames).
    std::vector<std::pair<size_t, size_t>> placed;
    bool notesOpenOnly = false;  // the list shows only notes not resolved
    int notesScroll = 0;         // the list's scroll, in pixels

    void PlaceNotes() {
        placed.clear();
        for (size_t i = 0; i < edit.notes.size(); ++i)
            if (const auto f = FrameOfSource(tframes, edit.clips, edit.notes[i].path, edit.notes[i].src)) placed.push_back({*f, i});
        std::stable_sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    // The notes the list shows (all, or the open ones).
    std::vector<std::pair<size_t, size_t>> ListedNotes() const {
        std::vector<std::pair<size_t, size_t>> v;
        for (const auto& p : placed)
            if (!notesOpenOnly || !edit.notes[p.second].resolved) v.push_back(p);
        return v;
    }
    std::optional<size_t> NoteFrame(const Note& n) const { return FrameOfSource(tframes, edit.clips, n.path, n.src); }
    std::optional<size_t> NoteEndFrame(const Note& n) const {
        return n.srcEnd ? FrameOfSource(tframes, edit.clips, n.path, *n.srcEnd) : std::nullopt;
    }
    // ---- notes kept next to their videos ("<video>.notes.json") ----

    std::map<std::wstring, std::vector<Note>> savedNotes;  // by file: as last read or written
    std::map<std::wstring, std::wstring> notePaths;        // by file: its path as the clips name it
    std::set<std::wstring> damagedNotes;                   // files whose sidecar couldn't be read: kept aside on the first write
    std::set<std::wstring> madeSidecars;                   // sidecars this editor made (gone again once their last note is)
    std::set<std::wstring> unsaved;                        // files whose sidecar couldn't be written (said once)

    // Reads the notes kept with the clips' files that the editor hasn't seen yet. They join the edit as if they had
    // always been there: in every undo step too, so undoing never takes away notes that were saved.
    void LoadNotes() {
        for (const auto& c : edit.clips) {
            const std::wstring key = FileKey(c.path);
            if (savedNotes.count(key)) continue;
            notePaths[key] = c.path;
            std::vector<Note> loaded;
            std::string text;
            const std::wstring side = NotesPath(c.path);
            const bool exists = GetFileAttributesW(side.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (exists && (!ReadFileUtf8(side, &text) || !ParseNotes(text, c.path, FileTimesOf(key), c.fps, &loaded))) {
                damagedNotes.insert(key);
                loaded.clear();
                if (!snapshotMode)
                    ShowToast(L"Couldn't read the notes kept with this video", FileNameOf(side) + L" stays as it is. New notes are saved next to it.", nullptr,
                              nullptr, 6000);
            }
            if (!exists) madeSidecars.insert(key);
            SortNotes(loaded);
            savedNotes[key] = loaded;
            edit.notes.insert(edit.notes.end(), loaded.begin(), loaded.end());
            for (auto& u : undoStack) u.notes.insert(u.notes.end(), loaded.begin(), loaded.end());
        }
    }
    const std::vector<double>& FileTimesOf(const std::wstring& key) const {
        static const std::vector<double> none;
        const auto it = frameTimes.find(key);
        return it != frameTimes.end() && it->second ? *it->second : none;
    }
    // Writes the sidecar of every file whose notes changed (as they change: typing, undo, everything).
    void NotesChanged() {
        std::map<std::wstring, std::vector<Note>> now;
        for (const auto& n : edit.notes) now[FileKey(n.path)].push_back(n);
        for (auto& [key, v] : now) SortNotes(v);
        std::set<std::wstring> keys;
        for (const auto& [k, v] : now) keys.insert(k);
        for (const auto& [k, v] : savedNotes) keys.insert(k);
        for (const auto& key : keys) {
            const std::vector<Note>& cur = now[key];
            if (savedNotes.count(key) && savedNotes[key] == cur) continue;
            if (WriteNotes(key, cur)) savedNotes[key] = cur;
        }
    }
    bool WriteNotes(const std::wstring& key, const std::vector<Note>& notes) {
        const auto pit = notePaths.find(key);
        if (pit == notePaths.end()) return false;
        const std::wstring video = pit->second, side = NotesPath(video);
        if (damagedNotes.count(key)) {  // kept aside, as it was, before the first write over it
            std::wstring aside = video + L".notes.damaged.json";
            for (int i = 2; GetFileAttributesW(aside.c_str()) != INVALID_FILE_ATTRIBUTES; ++i) aside = video + L".notes.damaged-" + std::to_wstring(i) + L".json";
            MoveFileExW(side.c_str(), aside.c_str(), 0);
            damagedNotes.erase(key);
        }
        if (notes.empty() && madeSidecars.count(key)) {  // a sidecar this editor made, now without notes: gone again
            DeleteFileW(side.c_str());
            return true;
        }
        double fps = 30;
        for (const auto& c : edit.clips)
            if (FileKey(c.path) == key && c.fps > 1) fps = c.fps;
        const std::string text = NotesJson(video, notes, FileTimesOf(key), fps);
        const std::wstring tmp = side + L".tmp";
        bool ok = false;
        if (HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
            CloseHandle(f);
            ok = ok && MoveFileExW(tmp.c_str(), side.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!ok) DeleteFileW(tmp.c_str());
        }
        if (!ok && !unsaved.count(key)) {
            unsaved.insert(key);
            ShowToast(L"Can't save notes next to this video", FileNameOf(side) + L": the folder may be read-only. They stay while the editor is open.",
                      nullptr, nullptr, 6000);
        }
        return ok;
    }
    // The edit as Save sees it (notes are never part of it), to tell whether closing loses anything.
    static VideoEdit WithoutNotes(VideoEdit e) {
        e.notes.clear();
        return e;
    }
    VideoEdit savedEdit;  // the picture edit as opened or last saved

    // M: a note on the frame on screen, its text field ready for typing.
    void AddNote() {
        if (tframes.empty()) return;
        Pause();
        EndGoTo();
        const size_t n = FrameNow();
        const TimelineFrames::Frame& f = tframes.frames[n];
        if (f.clip >= edit.clips.size()) return;
        PushUndo();
        Note note;
        note.path = edit.clips[f.clip].path;
        note.src = f.src;
        note.author = NoteAuthor();
        note.created = (int64_t)time(nullptr);
        edit.notes.push_back(note);
        selected = note.id;
        selClip.reset();
        GoToFrame(n);
        Changed();
        RevealNote(note.id);
        FocusField();
    }
    // "Range to here": the selected note runs on to the frame on screen (a later frame of the same video).
    void SetNoteRangeToPlayhead() {
        const auto i = SelNote();
        if (!i || tframes.empty()) return;
        const Note& n = edit.notes[*i];
        const auto f = NoteFrame(n);
        const size_t now = FrameNow();
        const TimelineFrames::Frame& fr = tframes.frames[now];
        if (!f || now <= *f || fr.clip >= edit.clips.size() || _wcsicmp(edit.clips[fr.clip].path.c_str(), n.path.c_str()) != 0 || fr.src <= n.src) {
            ShowToast(L"Move the playhead to a later frame first", L"The range runs from the note's frame to the frame on screen.", nullptr, nullptr, 3000);
            return;
        }
        const double end = fr.src;
        UpdateNote([end](Note& x) { x.srcEnd = end; });
    }
    void UpdateNote(const std::function<void(Note&)>& f) {
        if (auto i = SelNote()) {
            PushUndo();
            f(edit.notes[*i]);
            Changed();
        }
    }
    // Selects a note and goes to its frame (from the list, a tick, [ or ]).
    void SelectNote(uint64_t id) {
        for (const auto& [frame, i] : placed)
            if (edit.notes[i].id == id) {
                EndGoTo();
                Pause();
                Select(id);
                GoToFrame(frame);
                RevealNote(id);
                return;
            }
    }
    // [ and ]: the note before or after the frame on screen (of those the list shows).
    void JumpNote(int dir) {
        const auto v = ListedNotes();
        if (v.empty()) return;
        const size_t now = FrameNow();
        if (dir > 0) {
            for (const auto& p : v)
                if (p.first > now) return SelectNote(edit.notes[p.second].id);
        } else {
            for (auto it = v.rbegin(); it != v.rend(); ++it)
                if (it->first < now) return SelectNote(edit.notes[it->second].id);
        }
    }
    // Scrolls the list so that note's row shows.
    void RevealNote(uint64_t id) {
        const auto v = ListedNotes();
        const RECT list = NotesListRect();
        for (size_t k = 0; k < v.size(); ++k)
            if (edit.notes[v[k].second].id == id) {
                const int top = (int)k * NoteRowH(), bottom = top + NoteRowH();
                if (top < notesScroll) notesScroll = top;
                if (bottom > notesScroll + RectH(list)) notesScroll = bottom - RectH(list);
            }
        notesScroll = std::max(0, notesScroll);
    }
    std::vector<MenuItem> NoteKindMenu(uint64_t id) {
        std::vector<MenuItem> v;
        NoteKind cur = NoteKind::Note;
        for (const auto& n : edit.notes)
            if (n.id == id) cur = n.kind;
        for (int k = 0; k < kNoteKinds; ++k) {
            MenuItem it;
            it.label = NoteKindLabel((NoteKind)k);
            it.checked = cur == (NoteKind)k;
            it.swatch = annot::Color(NoteKindColor((NoteKind)k));
            it.run = [this, k] { UpdateNote([k](Note& x) { x.kind = (NoteKind)k; }); };
            v.push_back(it);
        }
        return v;
    }
    // Where clip `c`'s picture sits in the sequence frame (as FitInto puts it there).
    VRect ClipFit(const Clip& c) const {
        const double W = videoSize.cx, H = videoSize.cy, cw = c.w > 0 ? c.w : W, ch = c.h > 0 ? c.h : H;
        const double k = std::min(W / cw, H / ch);
        const double fw = std::clamp((double)std::lround(cw * k), 1.0, W), fh = std::clamp((double)std::lround(ch * k), 1.0, H);
        return {(double)(int)((W - fw) / 2), (double)(int)((H - fh) / 2), fw, fh};
    }
    // A note's pin in the sequence frame's pixels.
    std::optional<VPoint> PinOnVideo(const Note& n) const {
        const auto f = NoteFrame(n);
        if (!n.pin || !f || tframes.frames[*f].clip >= edit.clips.size()) return std::nullopt;
        const Clip& c = edit.clips[tframes.frames[*f].clip];
        const VRect r = ClipFit(c);
        const VPoint at = FlipPin(*n.pin, c);  // the pin is on the footage, so on a flipped clip it's mirrored too
        return VPoint{r.x + at.x * r.w, r.y + at.y * r.h};
    }
    // A pin (fractions of the clip's picture) as it shows on the clip's flipped picture, or the other way round.
    static VPoint FlipPin(VPoint p, const Clip& c) { return {c.flipH ? 1 - p.x : p.x, c.flipV ? 1 - p.y : p.y}; }

    void SetCrop(std::optional<VRect> r) {
        const VRect full{0, 0, (double)videoSize.cx, (double)videoSize.cy};
        edit.crop.reset();
        if (!r) return;
        const double x0 = std::max(0.0, r->x), y0 = std::max(0.0, r->y), x1 = std::min(full.w, r->MaxX()), y1 = std::min(full.h, r->MaxY());
        const VRect c = VRect{x0, y0, x1 - x0, y1 - y0}.Integral();
        if (c.w >= 16 && c.h >= 16 && !(c == full)) edit.crop = c;
    }

    void ApplyAspect(int i) {
        aspect = i;
        if (!i) return Invalidate();
        PushUndo();
        const double k = kAspectValues[i], vw = videoSize.cx, vh = videoSize.cy;
        const double w = std::min(vw, vh * k), h = w / k;
        SetCrop(VRect{(vw - w) / 2, (vh - h) / 2, w, h});
        Changed();
    }

    // ---- menus ----

    std::wstring AnimationTitle(const Mark& m) const {
        std::wstring t = L"Animation: ";
        t += m.style == AnimStyle::Auto ? std::wstring(L"Auto (") + StyleLabel(KindDefaultStyle(m.kind)) + L")" : StyleLabel(m.style);
        if (m.emphasis != Emphasis::None) t += std::wstring(L" · ") + EmphasisLabel(m.emphasis);
        if (m.exit) t += std::wstring(L" → ") + (*m.exit == AnimStyle::Auto ? StyleLabel(KindDefaultStyle(m.kind)) : StyleLabel(*m.exit));
        return t;
    }

    // One menu: the style (in and out), an optional effect while on screen, and the advanced options.
    std::vector<MenuItem> AnimationMenu(const Mark& m) {
        std::vector<MenuItem> items;
        const uint64_t id = m.id;
        std::vector<AnimStyle> styles = {AnimStyle::Auto};
        for (auto st : KindStyles(m.kind)) styles.push_back(st);
        for (auto st : styles) {
            MenuItem it;
            it.label = st == AnimStyle::Auto ? std::wstring(L"Auto (") + StyleLabel(KindDefaultStyle(m.kind)) + L")" : StyleLabel(st);
            it.checked = m.style == st;
            it.run = [this, id, st] { PickAnimation(id, [st](Mark& x) { x.style = st; }); };
            items.push_back(it);
        }
        if (m.kind != MarkKind::Blur && m.kind != MarkKind::Pixelate) {
            items.push_back(MenuItem::Sep());
            items.push_back(MenuItem::Header(L"While on screen"));
            for (Emphasis e : {Emphasis::None, Emphasis::Pulse, Emphasis::Bounce, Emphasis::Shake, Emphasis::Ping}) {
                MenuItem it;
                it.label = std::wstring(L"    ") + EmphasisLabel(e);
                it.checked = m.emphasis == e;
                it.run = [this, id, e] { PickAnimation(id, [e](Mark& x) { x.emphasis = e; }); };
                items.push_back(it);
            }
        }
        items.push_back(MenuItem::Sep());
        MenuItem diff;
        diff.label = L"Different exit animation";
        diff.checked = m.exit.has_value();
        diff.run = [this, id] {
            PickAnimation(id, [](Mark& x) {
                if (x.exit) x.exit.reset();
                else x.exit = AnimStyle::Fade;
            });
        };
        items.push_back(diff);
        if (m.exit) {
            MenuItem exitMenu;
            exitMenu.label = L"    Exit";
            for (auto st : KindStyles(m.kind)) {
                if (st == AnimStyle::DrawOn || st == AnimStyle::Typewriter) continue;
                MenuItem it;
                it.label = StyleLabel(st);
                it.checked = m.exit == st;
                it.run = [this, id, st] { PickAnimation(id, [st](Mark& x) { x.exit = st; }); };
                exitMenu.sub.push_back(it);
            }
            items.push_back(exitMenu);
        }
        MenuItem all;
        all.label = L"Apply to all " + KindPlural(m.kind);
        all.run = [this, m] {
            PushUndo();
            int n = 0;
            for (auto& x : edit.marks)
                if (x.kind == m.kind) {
                    x.style = m.style;
                    x.exit = m.exit;
                    x.emphasis = m.emphasis;
                    ++n;
                }
            Changed();
            ShowToast(L"Applied to " + std::to_wstring(n) + L" " + (n == 1 ? Lower(KindLabel(m.kind)) : KindPlural(m.kind)),
                      m.style == AnimStyle::Auto ? L"Auto" : StyleLabel(m.style), nullptr, nullptr, 2200);
        };
        items.push_back(all);
        return items;
    }

    std::vector<MenuItem> ColorItems(int current, const std::function<void(int)>& set) {
        std::vector<MenuItem> v;
        for (int i = 0; i < annot::ColorCount(); ++i) {
            MenuItem it;
            it.label = annot::ColorName(i);
            it.checked = current == i;
            it.swatch = annot::Color(i);
            it.run = [set, i] { set(i); };
            v.push_back(it);
        }
        return v;
    }

    // Toolbar "Captions" menu: look, animation, word highlight, size and colors, for the whole video.
    std::vector<MenuItem> CaptionsMenu() {
        std::vector<MenuItem> v;
        v.push_back(MenuItem::Header(L"Look"));
        for (CaptionLook l : {CaptionLook::Pill, CaptionLook::Outline, CaptionLook::Bar}) {
            MenuItem it;
            it.label = std::wstring(L"    ") + LookLabel(l);
            it.checked = edit.captionLook == l;
            it.run = [this, l] { SetCaptions([l](VideoEdit& e) { e.captionLook = l; }); };
            v.push_back(it);
        }
        v.push_back(MenuItem::Sep());
        v.push_back(MenuItem::Header(L"Animation"));
        for (AnimStyle st : CaptionStyles()) {
            MenuItem it;
            it.label = std::wstring(L"    ") + (st == AnimStyle::Auto ? L"Auto (Fade)" : StyleLabel(st));
            it.checked = edit.captionStyle == st;
            it.run = [this, st] { SetCaptions([st](VideoEdit& e) { e.captionStyle = st; }); };
            v.push_back(it);
        }
        v.push_back(MenuItem::Sep());
        MenuItem hl;
        hl.label = L"Highlight the spoken word";
        hl.checked = edit.highlightWords;
        hl.run = [this] { SetCaptions([](VideoEdit& e) { e.highlightWords = !e.highlightWords; }); };
        v.push_back(hl);
        v.push_back(MenuItem::Sep());
        v.push_back(MenuItem::Header(L"Size"));
        for (int i = 0; i < 5; ++i) {
            MenuItem it;
            it.label = std::wstring(L"    ") + VideoEdit::kCaptionSizes[i];
            it.checked = edit.captionSize == i;
            it.run = [this, i] { SetCaptions([i](VideoEdit& e) { e.captionSize = i; }); };
            v.push_back(it);
        }
        v.push_back(MenuItem::Sep());
        MenuItem tc;
        tc.label = L"Text color";
        tc.sub = ColorItems(edit.captionColor, [this](int i) { SetCaptions([i](VideoEdit& e) { e.captionColor = i; }); });
        v.push_back(tc);
        MenuItem ec;
        ec.label = edit.captionLook == CaptionLook::Outline ? L"Outline color" : L"Box color";
        ec.sub = ColorItems(edit.captionEdge, [this](int i) { SetCaptions([i](VideoEdit& e) { e.captionEdge = i; }); });
        v.push_back(ec);
        return v;
    }

    // ---- flip ----

    // What Flip flips: the selected video on top, else the selected clip, else the clip at the playhead.
    struct FlipTarget {
        int overlay = -1, clip = -1;
    };
    FlipTarget FlipWhat() const {
        if (const auto oi = SelOverlay()) return {(int)*oi, -1};
        if (const auto ci = SelClipIndex()) return {-1, (int)*ci};
        if (const auto spot = LocateClip(edit.clips, Now())) return {-1, (int)spot->first};
        return {};
    }
    const Clip* FlipClip() const {
        const FlipTarget w = FlipWhat();
        return w.overlay >= 0 ? &edit.overlays[(size_t)w.overlay].clip : w.clip >= 0 ? &edit.clips[(size_t)w.clip] : nullptr;
    }
    std::wstring FlipLabel() const {
        const Clip* c = FlipClip();
        return !c ? L"Flip" : c->flipH && c->flipV ? L"Flipped ↔ ↕" : c->flipH ? L"Flipped ↔" : c->flipV ? L"Flipped ↕" : L"Flip";
    }
    std::wstring FlipTip() const {
        const FlipTarget w = FlipWhat();
        if (w.overlay >= 0) return L"Mirror the selected video on top";
        if (SelClipIndex()) return L"Mirror the selected clip (left–right or upside down)";
        return edit.clips.size() > 1 ? L"Mirror the clip at the playhead (select a clip to pick another)" : L"Mirror the video (left–right or upside down)";
    }
    std::vector<MenuItem> FlipMenu() {
        const Clip* c = FlipClip();
        MenuItem h;
        h.label = L"Flip horizontally (mirror left–right)";
        h.checked = c && c->flipH;
        h.enabled = c != nullptr;
        h.run = [this] { ToggleFlip(true); };
        MenuItem v;
        v.label = L"Flip vertically (upside down)";
        v.checked = c && c->flipV;
        v.enabled = c != nullptr;
        v.run = [this] { ToggleFlip(false); };
        return {h, v};
    }
    // One undo step: one clip's (or video on top's) picture mirrored one way. On a clip, the markup that sits wholly
    // on it is mirrored with it, so it stays on the same footage; the crop (all clips share it) and captions stay put;
    // notes keep their spot on the footage (PinOnVideo mirrors it).
    void ToggleFlip(bool horizontal) {
        const FlipTarget w = FlipWhat();
        if (w.overlay < 0 && w.clip < 0) return;
        PushUndo();
        if (w.overlay >= 0) {
            Clip& c = edit.overlays[(size_t)w.overlay].clip;
            (horizontal ? c.flipH : c.flipV) ^= true;
            return Changed();
        }
        const size_t i = (size_t)w.clip;
        const double t0 = ClipStart(edit.clips, i), t1 = t0 + edit.clips[i].Duration();
        const double W = videoSize.cx, H = videoSize.cy;
        auto mirror = [&](VPoint& p) { (horizontal ? p.x : p.y) = (horizontal ? W - p.x : H - p.y); };
        for (auto& m : edit.marks)
            if (m.start >= t0 - 1e-6 && m.end <= t1 + 1e-6) {
                mirror(m.a);
                mirror(m.b);
            }
        Clip& c = edit.clips[i];
        (horizontal ? c.flipH : c.flipV) ^= true;
        Changed();
    }

    std::vector<MenuItem> AddMenu() {
        std::vector<MenuItem> v;
        MenuItem clip;
        clip.label = L"Video clip…\tCtrl+O";
        clip.run = [this] { AddClipDialog(); };
        v.push_back(clip);
        MenuItem pip;
        pip.label = L"Video on top (picture in picture)…";
        pip.run = [this] { AddOverlayDialog(OverlayLayout::Pip); };
        v.push_back(pip);
        MenuItem split;
        split.label = L"Video side by side (split screen)…";
        split.run = [this] { AddOverlayDialog(OverlayLayout::SplitRight); };
        v.push_back(split);
        v.push_back(MenuItem::Sep());
        MenuItem cap;
        cap.label = L"Caption\tT";
        cap.run = [this] { AddCaption(); };
        v.push_back(cap);
        MenuItem note;
        note.label = L"Review note\tM";
        note.run = [this] { AddNote(); };
        v.push_back(note);
        v.push_back(MenuItem::Sep());
        for (int k = 0; k < kMarkKinds; ++k) {
            const MarkKind kind = (MarkKind)k;
            MenuItem it;
            it.label = KindLabel(kind);
            if (KindKey(kind)) it.label += std::wstring(L"\t") + KindKey(kind);
            it.run = [this, kind] { AddMark(kind); };
            v.push_back(it);
            if (kind == MarkKind::Step || kind == MarkKind::Pixelate) v.push_back(MenuItem::Sep());
        }
        return v;
    }

    std::vector<MenuItem> ListMenu(const std::vector<std::wstring>& labels, int current, const std::function<void(int)>& pick) {
        std::vector<MenuItem> v;
        for (int i = 0; i < (int)labels.size(); ++i) {
            MenuItem it;
            it.label = labels[i];
            it.checked = i == current;
            it.run = [pick, i] { pick(i); };
            v.push_back(it);
        }
        return v;
    }

    void Popup(const RECT& under, const std::vector<MenuItem>& items) {
        POINT p{under.left, under.bottom + S(2)};
        ClientToScreen(hwnd, &p);
        RunMenu(hwnd, items, p);
    }

    // ---- fields ----

    void FocusField() {
        SyncFields();
        if (hwnd) {  // the field is shown by painting: paint now, so it can take the typing
            InvalidateRect(hwnd, nullptr, FALSE);
            UpdateWindow(hwnd);
        }
        if (field1 && (GetWindowLongW(field1, GWL_STYLE) & WS_VISIBLE)) {
            SetFocus(field1);
            SendMessageW(field1, EM_SETSEL, 0, -1);
        }
    }

    // Puts the selected item's text in the fields when the selection changes.
    void SyncFields() {
        if (!field1) return;
        std::wstring t1, t2;
        int kind = -1;
        uint64_t id = 0;
        if (goingTo) {
            kind = 200;
            id = ~0ull;
        } else if (auto ni = SelNote()) {
            t1 = edit.notes[*ni].text;
            t2 = edit.notes[*ni].author;
            kind = 300;
            id = edit.notes[*ni].id;
        } else if (auto ci = SelCaption()) {
            t1 = edit.captions[*ci].text;
            kind = 100;
            id = edit.captions[*ci].id;
        } else if (auto mi = SelMark()) {
            const Mark& m = edit.marks[*mi];
            if (KindHasText(m.kind)) {
                t1 = m.text;
                t2 = m.subtitle;
                kind = (int)m.kind;
                id = m.id;
            }
        }
        if (id != fieldsFor || kind != fieldsKind) {
            fieldsFor = id;
            fieldsKind = kind;
            settingText = true;
            SetWindowTextW(field1, t1.c_str());
            SetWindowTextW(field2, t2.c_str());
            settingText = false;
            const wchar_t* cue = kind == 200 ? L"757, 0:12:37 or 12.6" : kind == 300 ? L"What's on this frame?"
                                 : kind == 100 ? L"Caption text" : kind == (int)MarkKind::Bubble ? L"Bubble text" : kind == (int)MarkKind::Title ? L"Title"
                                 : kind == (int)MarkKind::Emoji ? L"Emoji (Win+. for more)" : L"Text";
            SendMessageW(field1, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
            SendMessageW(field2, EM_SETCUEBANNER, TRUE, (LPARAM)(kind == 300 ? L"Your name" : L"Subtitle (optional)"));
        }
        if (id == 0) {
            if (GetFocus() == field1 || GetFocus() == field2) SetFocus(hwnd);
            ShowWindow(field1, SW_HIDE);
            ShowWindow(field2, SW_HIDE);
        }
    }

    void FieldChanged(HWND f) {
        if (settingText || goingTo) return;
        const int n = GetWindowTextLengthW(f);
        std::wstring t(n, L'\0');
        GetWindowTextW(f, t.data(), n + 1);
        if (auto ni = SelNote()) {
            if (f == field2) {
                edit.notes[*ni].author = t;
                RememberAuthor(t);
            } else {
                edit.notes[*ni].text = t;
            }
            NotesChanged();
            Invalidate();
            return;
        }
        if (auto ci = SelCaption()) edit.captions[*ci].text = t;
        else if (auto mi = SelMark()) (f == field2 ? edit.marks[*mi].subtitle : edit.marks[*mi].text) = t;
        Rerender();
        Invalidate();
    }

    // ---- layout ----

    RECT Client() const {
        RECT c;
        GetClientRect(hwnd, &c);
        return c;
    }
    int RowCount() const {
        int rows = 0;
        for (const auto& [m, r] : Rows()) rows = std::max(rows, r + 1);
        return std::clamp(rows, 3, 8);
    }
    // Markup bars, one row per overlapping item.
    std::vector<std::pair<Mark, int>> Rows() const {
        std::vector<Mark> sorted = edit.marks;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Mark& a, const Mark& b) { return a.start < b.start; });
        std::vector<double> ends;
        std::vector<std::pair<Mark, int>> out;
        for (const auto& m : sorted) {
            int row = -1;
            for (int i = 0; i < (int)ends.size(); ++i)
                if (ends[i] <= m.start) {
                    row = i;
                    break;
                }
            if (row < 0) {
                ends.push_back(m.end);
                row = (int)ends.size() - 1;
            } else {
                ends[row] = m.end;
            }
            out.push_back({m, row});
        }
        return out;
    }
    // The clip lane, and the bin lane above the timeline, show once there's more than one clip or anything in the bin.
    bool ClipLanesShown() const { return edit.clips.size() > 1 || !edit.bin.empty(); }
    int LaneH() const { return ClipLanesShown() ? S(24) : 0; }
    int BinH() const { return ClipLanesShown() ? S(30) : 0; }
    RECT TimelineRect() const {
        const RECT c = Client();
        const int h = S(90) + RowCount() * S(18) + LaneH() + OverlayLaneH();
        return {S(100), c.bottom - S(14) - h, c.right - S(14), c.bottom - S(14)};
    }
    // The overlay videos' lane, at the bottom of the timeline under the markup's rows.
    int OverlayLaneH() const { return S(26); }
    RECT OverlayLaneRect() const {
        const RECT t = TimelineRect();
        return {t.left, t.bottom - S(22), t.right, t.bottom};
    }
    // Clear of the playhead knob, which pokes above the timeline.
    RECT BinRect() const {
        const RECT t = TimelineRect();
        return {t.left, t.top - S(12) - BinH(), t.right, t.top - S(12)};
    }
    RECT InspectorRect() const {
        const int top = BinH() ? BinRect().top : TimelineRect().top;
        const RECT c = Client();
        return {S(14), top - S(8) - S(30), c.right - S(14), top - S(8)};
    }
    // Each bin piece's chip, left to right: as long as on the timeline, shorter when they don't all fit.
    std::vector<RECT> BinChips() const {
        std::vector<RECT> out;
        const RECT r = BinRect();
        if (edit.bin.empty() || RectW(r) <= 0) return out;
        const int n = (int)edit.bin.size(), gap = S(4), minW = S(64);
        const int avail = std::max(n, RectW(r) - gap * (n + 1));
        double total = 0;
        for (const auto& c : edit.bin) total += c.Duration();
        const double k = std::min(RectW(r) / std::max(0.001, duration), avail / std::max(0.001, total));
        std::vector<int> w;
        int sum = 0;
        for (const auto& c : edit.bin) {
            w.push_back(std::max(minW, (int)(c.Duration() * k)));
            sum += w.back();
        }
        if (sum > avail)
            for (auto& x : w) x = avail / n;
        int x = r.left + gap;
        for (int wi : w) {
            out.push_back({x, r.top + S(3), x + wi, r.bottom - S(3)});
            x += wi + gap;
        }
        return out;
    }
    // Where a clip dropped at `x` lands on the timeline: how many clips (other than `skip`) end up before it.
    int InsertIndex(int x, size_t skip) const {
        int target = 0;
        for (size_t i = 0; i < edit.clips.size(); ++i)
            if (i != skip && TX(ClipStart(edit.clips, i) + edit.clips[i].Duration() / 2) < x) ++target;
        return target;
    }
    // The frame readout under the video.
    RECT ReadoutRect() const {
        const RECT c = Client();
        const int bottom = InspectorRect().top - S(4);
        return {S(14), bottom - S(22), c.right - S(14), bottom};
    }
    // The video and, when there are notes, the notes list at its right.
    RECT StageArea() const {
        const RECT c = Client();
        return {S(14), S(50), c.right - S(14), ReadoutRect().top - S(4)};
    }
    bool NotesShown() const { return !placed.empty(); }
    RECT StageRect() const {
        RECT r = StageArea();
        if (NotesShown()) r.right -= S(kNotesW) + S(8);
        return r;
    }
    static constexpr int kNotesW = 290;
    RECT NotesRect() const {
        const RECT a = StageArea();
        return {a.right - S(kNotesW), a.top, a.right, a.bottom};
    }
    RECT NotesListRect() const {
        const RECT r = NotesRect();
        return {r.left, r.top + S(44), r.right, r.bottom - S(4)};
    }
    int NoteRowH() const { return S(52); }
    // Where the video is drawn.
    gp::RectF VideoRect() const {
        const RECT st = StageRect();
        const double k = std::min(RectW(st) / (double)videoSize.cx, RectH(st) / (double)videoSize.cy);
        const double w = videoSize.cx * k, h = videoSize.cy * k;
        return gp::RectF((float)(st.left + (RectW(st) - w) / 2), (float)(st.top + (RectH(st) - h) / 2), (float)w, (float)h);
    }
    // Screen pixels per video pixel, with the magnifier's zoom. The video's top-left on view is (magX, magY).
    double ViewScale() const { return VideoRect().Width / std::max(1L, videoSize.cx) * magZoom; }
    VPoint ToVideo(POINT p) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return {magX + (p.x - v.X) / k, magY + (p.y - v.Y) / k};
    }
    gp::PointF ToView(VPoint p) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return gp::PointF((float)(v.X + (p.x - magX) * k), (float)(v.Y + (p.y - magY) * k));
    }
    gp::RectF ToView(VRect r) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return gp::RectF((float)(v.X + (r.x - magX) * k), (float)(v.Y + (r.y - magY) * k), (float)(r.w * k), (float)(r.h * k));
    }

    // ---- pixel magnifier ----
    // The wheel over the video zooms 1× to 8× around the cursor, a right-drag pans, F or a double-click fits it again.
    // From 2× the pixels are drawn sharp. For looking only: the crop and the export never change.
    double magZoom = 1;
    double magX = 0, magY = 0;  // the video pixel at the view's top-left
    std::optional<POINT> magDrag;  // right-drag: where it was last
    void ClampMagnifier() {
        magZoom = std::clamp(magZoom, 1.0, 8.0);
        magX = std::clamp(magX, 0.0, videoSize.cx - videoSize.cx / magZoom);
        magY = std::clamp(magY, 0.0, videoSize.cy - videoSize.cy / magZoom);
    }
    // Zooms by `factor`, keeping the video pixel under screen point `at` there.
    void Magnify(double factor, POINT at) {
        const VPoint under = ToVideo(at);
        magZoom = std::clamp(magZoom * factor, 1.0, 8.0);
        if (magZoom < 1.0001) magZoom = 1;
        const auto v = VideoRect();
        const double k = ViewScale();
        magX = under.x - (at.x - v.X) / k;
        magY = under.y - (at.y - v.Y) / k;
        ClampMagnifier();
        Invalidate();
    }
    void FitMagnifier() {
        magZoom = 1;
        magX = magY = 0;
        Invalidate();
    }
    void PanMagnifier(int dx, int dy) {
        const double k = ViewScale();
        magX -= dx / k;
        magY -= dy / k;
        ClampMagnifier();
        Invalidate();
    }

    double TX(double t) const {  // timeline x for a time
        const RECT r = TimelineRect();
        return r.left + RectW(r) * ((t - tlStart) / TlSpan());
    }
    double TT(int x) const {
        const RECT r = TimelineRect();
        return std::clamp(tlStart + (double)(x - r.left) / std::max(1, RectW(r)) * TlSpan(), 0.0, std::max(0.0, duration));
    }

    // ---- timeline zoom ----
    // Ctrl+wheel or Ctrl+= / Ctrl+− zoom in around the playhead, to about 14 px a frame; Ctrl+0 fits the trim. The wheel,
    // or dragging an empty lane, pans. Everything on the timeline goes through TX and TT, so it stays on its frames.
    double tlZoom = 1;   // 1: the whole timeline fits
    double tlStart = 0;  // the time at its left edge
    double TlSpan() const { return std::max(1e-6, std::max(0.001, duration) / tlZoom); }
    double PxPerFrame() const { return RectW(TimelineRect()) / TlSpan() / Seq().Fps(); }
    double MaxTlZoom() const { return std::max(1.0, std::max(0.001, duration) * Seq().Fps() * 14.0 / std::max(1, RectW(TimelineRect()))); }
    void ClampTimeline() {
        tlZoom = std::clamp(tlZoom, 1.0, MaxTlZoom());
        tlStart = std::clamp(tlStart, 0.0, std::max(0.0, duration - TlSpan()));
    }
    // Zooms by `factor`, keeping time `at` (the playhead) under the same spot (or centering it, when it was off view).
    void ZoomTimeline(double factor, std::optional<double> at = std::nullopt) {
        const double t = at ? *at : (playing ? rawT : paused);
        const RECT r = TimelineRect();
        double frac = (TX(t) - r.left) / std::max(1, RectW(r));
        if (frac < 0 || frac > 1) frac = 0.5;
        tlZoom *= factor;
        tlZoom = std::clamp(tlZoom, 1.0, MaxTlZoom());
        tlStart = t - frac * TlSpan();
        ClampTimeline();
        Invalidate();
    }
    void FitTrim() {
        const double span = std::max(0.01, edit.trimEnd - edit.trimStart);
        tlZoom = std::max(1.0, std::max(0.001, duration) / span);
        tlStart = edit.trimStart;
        ClampTimeline();
        Invalidate();
    }
    // Pans so time `t` is on view (stepping or playing past the edge turns the page).
    void KeepOnView(double t) {
        if (tlZoom <= 1) return;
        const double span = TlSpan();
        if (t < tlStart || t > tlStart + span) tlStart = t - span * 0.15;
        else if (t > tlStart + span * 0.97) tlStart = t - span * 0.15;
        ClampTimeline();
    }
    // A click on the timeline lands on the nearest frame once frames are a few pixels apart.
    double SnapToFrame(double t) const {
        if (tframes.empty() || PxPerFrame() < 3) return t;
        return tframes.frames[tframes.Nearest(t)].t;
    }

    // ---- the marked range ----

    // The frame boundary nearest `t`: where a frame starts, or the end of the last one.
    double SnapEdge(double t) const {
        if (tframes.empty()) return t;
        const double f = tframes.frames[tframes.Nearest(t)].t;
        return std::fabs(duration - t) < std::fabs(f - t) ? duration : f;
    }
    // From `a` to `b` (either way round), inside the trim; none when that leaves less than a frame.
    void SetMarked(double a, double b) {
        const double from = std::max(std::min(a, b), edit.trimStart), to = std::min(std::max(a, b), edit.trimEnd);
        if (to - from < 0.5 / Seq().Fps()) marked.reset();
        else marked = Span{from, to};
    }
    // Shift+I / Shift+O: the range starts or ends at the playhead, as I and O set the trim. The other end stays, or is
    // the trim's when there's no range yet or it would come out empty.
    void MarkAtPlayhead(bool start) {
        const double t = SnapEdge(Now());
        double from = marked ? marked->start : edit.trimStart, to = marked ? marked->end : edit.trimEnd;
        if (start) {
            from = t;
            if (to <= from) to = edit.trimEnd;
        } else {
            to = t;
            if (to <= from) from = edit.trimStart;
        }
        SetMarked(from, to);
        Invalidate();
    }
    // A range drag: from `anchor` (an edge being dragged keeps the other one), or a new range from the press.
    void BeginMark(POINT p, std::optional<double> anchor) {
        Pause();
        const std::optional<Span> before = marked;
        drag = {};
        drag.kind = DragKind::Range;
        drag.before = before;
        drag.grab = anchor ? *anchor : SnapEdge(std::clamp(TT(p.x), edit.trimStart, edit.trimEnd));
        if (!anchor) marked.reset();
        Invalidate();
    }
    // "6.2 s · Ctrl+C copies": how long the copied clip runs (at the edit's speed).
    std::wstring MarkedLabel() const {
        if (!marked) return L"";
        return ClipSeconds((marked->end - marked->start) / edit.speed) + L" · Ctrl+C copies";
    }

    // ---- stage geometry ----

    std::vector<VPoint> Handles(const Mark& m) const {
        if (m.kind == MarkKind::Title) return {};
        if (KindIsLine(m.kind)) return {m.a, m.b};
        const VRect r = m.Rect();
        return {{r.x, r.y}, {r.MaxX(), r.y}, {r.x, r.MaxY()}, {r.MaxX(), r.MaxY()}};
    }

    bool Hit(const Mark& m, VPoint p, double slop) const {
        if (m.kind == MarkKind::Title) return true;
        if (KindIsLine(m.kind)) {
            const double dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, l2 = dx * dx + dy * dy;
            const double t = l2 == 0 ? 0 : std::clamp(((p.x - m.a.x) * dx + (p.y - m.a.y) * dy) / l2, 0.0, 1.0);
            return std::hypot(p.x - (m.a.x + t * dx), p.y - (m.a.y + t * dy)) <= slop * 1.5;
        }
        VRect r = m.Rect();
        if (m.kind == MarkKind::Bubble) r.h *= 1.32;
        return r.Inset(-slop, -slop).Contains(p);
    }

    // Where a caption sits on the video, in video coordinates.
    VRect CaptionRect(const Caption& c) const {
        const VRect v = ViewRect();
        FrameRenderer r(edit, videoSize, true);
        auto pl = r.CaptionImage(c, {(LONG)v.w, (LONG)v.h});
        if (!pl) return {};
        return pl->rect.Offset(v.x, v.y);
    }

    // ---- painting ----

    void Hotspot(const RECT& r, std::function<void()> click, std::wstring tip = L"") { hots.push_back({r, std::move(click), std::move(tip)}); }

    // A toolbar or inspector button: optional icon glyph, label, optional ▾; returns its rect.
    RECT Button(HDC dc, gp::Graphics& g, int x, int y, int h, wchar_t glyph, const std::wstring& label, bool menu, bool on, std::function<void()> click,
                const std::wstring& tip = L"", bool accent = false, int minW = 0) {
        const int pad = S(9);
        int w = pad * 2;
        const int gw = glyph ? S(16) + (label.empty() ? 0 : S(6)) : 0;
        w += gw;
        if (!label.empty()) w += Measure(dc, fUi, label).cx;
        if (menu) w += S(14);
        w = std::max(w, minW);
        RECT r{x, y, x + w, y + h};
        const bool hover = EqualRect(&r, &hoverRect);
        if (accent) FillRR(g, r, (float)S(7), A(theme::kAccent));
        else if (on) FillRR(g, r, (float)S(6), A(theme::kSelected));
        else if (hover) FillRR(g, r, (float)S(6), A(theme::kBgRaised));
        const COLORREF fg = accent ? theme::kOnAccent : on ? theme::kAccent : theme::kText;
        int cx = x + pad;
        if (glyph) {
            Text(dc, fIcon, std::wstring(1, glyph), {cx, y, cx + S(16), y + h}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            cx += gw;
        }
        if (!label.empty()) {
            const int lw = Measure(dc, fUi, label).cx;
            Text(dc, fUi, label, {cx, y, cx + lw + S(2), y + h}, fg);
            cx += lw;
        }
        if (menu) Text(dc, fSmall, L"▾", {r.right - pad - S(10), y, r.right - pad + S(2), y + h}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Hotspot(r, std::move(click), tip);
        return r;
    }

    void Separator(HDC dc, int& x, int y, int h) {
        FillSolid(dc, {x + S(4), y + S(7), x + S(5), y + h - S(7)}, theme::kBorder);
        x += S(10);
    }

    void PaintToolbar(HDC dc, gp::Graphics& g) {
        const RECT c = Client();
        const int y = S(8), h = S(32);
        int x = S(14);
        RECT r = Button(dc, g, x, y, h, 0xE7A8, L"Crop", false, cropping, [this] { cropping = !cropping; Invalidate(); }, L"Crop (C): drag on the video");
        x = r.right + S(4);
        if (cropping) {
            r = Button(dc, g, x, y, h, 0, kAspects[aspect], true, false, [] {}, L"Crop shape");
            const RECT ar = r;
            hots.back().click = [this, ar] {
                std::vector<std::wstring> labels(std::begin(kAspects), std::end(kAspects));
                Popup(ar, ListMenu(labels, aspect, [this](int i) { ApplyAspect(i); }));
            };
            x = r.right + S(4);
        }
        const Clip* flipping = FlipClip();
        r = Button(dc, g, x, y, h, 0, FlipLabel(), true, flipping && flipping->Flipped(), [] {}, FlipTip());
        {
            const RECT fr = r;
            hots.back().click = [this, fr] { Popup(fr, FlipMenu()); };
        }
        x = r.right + S(4);
        r = Button(dc, g, x, y, h, 0, L"Guides", true, AnyGuide() || mono, [] {},
                   L"Composition guides over the preview (G), or a black-and-white view: never saved. Right-click the video for them too");
        {
            const RECT gr = r;
            hots.back().click = [this, gr] { Popup(gr, GuidesMenu()); };
        }
        x = r.right + S(4);
        Separator(dc, x, y, h);
        int si = 1;
        for (int i = 0; i < 5; ++i)
            if (VideoEdit::kSpeeds[i] == edit.speed) si = i;
        r = Button(dc, g, x, y, h, 0, L"Speed " + SpeedLabel(edit.speed), true, edit.speed != 1, [] {}, L"Playback speed of the saved video");
        {
            const RECT sr = r;
            hots.back().click = [this, sr, si] {
                std::vector<std::wstring> labels;
                for (double v : VideoEdit::kSpeeds) labels.push_back(SpeedLabel(v));
                Popup(sr, ListMenu(labels, si, [this](int i) {
                    PushUndo();
                    edit.speed = VideoEdit::kSpeeds[i];
                    if (playing && player) player->SetRate(edit.speed);
                    Changed();
                }));
            };
        }
        x = r.right + S(4);
        r = Button(dc, g, x, y, h, edit.muted ? 0xE74F : 0xE767, L"Mute", false, edit.muted, [this] {
            PushUndo();
            edit.muted = !edit.muted;
            Changed();
        }, L"Remove the sound");
        x = r.right + S(4);
        Separator(dc, x, y, h);
        r = Button(dc, g, x, y, h, 0xE710, L"Add", true, false, [] {}, L"Add another video, text, emoji, callouts, blur, zoom or a title card");
        {
            const RECT ar = r;
            hots.back().click = [this, ar] { Popup(ar, AddMenu()); };
        }
        x = r.right + S(4);
        r = Button(dc, g, x, y, h, 0xE720, busy ? L"Working…" : L"Auto captions", false, false, [this] { AutoCaptions(); },
                   L"Transcribe speech into captions, on this PC");
        x = r.right + S(4);
        r = Button(dc, g, x, y, h, 0xE7F0, L"Captions", true, false, [] {}, L"Caption look, animation and size, for the whole video");
        {
            const RECT cr = r;
            hots.back().click = [this, cr] { Popup(cr, CaptionsMenu()); };
        }
        // Right side: Save GIF, Save and its menu (▾: the review video).
        const int saveW = S(68), moreW = S(28);
        const RECT more{c.right - S(14) - moreW, y, c.right - S(14), y + h};
        RECT save{more.left - S(2) - saveW, y, more.left - S(2), y + h};
        const int gifW = S(9) * 2 + S(22) + Measure(dc, fUi, L"Save GIF").cx;
        Button(dc, g, save.left - S(6) - gifW, y, h, 0xE8B9, L"Save GIF", false, false, [this] { Save(true); }, L"Save as a GIF (Ctrl+Shift+S)");
        Button(dc, g, save.left, y, h, 0, L"Save", false, false, [this] { Save(false); }, L"Save as a new MP4 in your captures (Ctrl+S)", true, saveW);
        FillRR(g, more, (float)S(7), A(theme::kAccent));
        Text(dc, fSmall, L"▾", more, theme::kOnAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Hotspot(more, [this, more] { Popup(more, SaveMenu()); }, L"More ways to save: smaller (under 8, 25, 50 MB…), GIF, review video with your notes; copy a clip or the frame");
    }
    std::vector<MenuItem> SaveMenu() {
        std::vector<MenuItem> v;
        MenuItem mp4, gif, review;
        mp4.label = L"Save video\tCtrl+S";
        mp4.run = [this] { Save(false); };
        gif.label = L"Save GIF\tCtrl+Shift+S";
        gif.run = [this] { Save(true); };
        review.label = L"Save review video\tCtrl+Alt+S";
        review.run = [this] { SaveReview(); };
        review.enabled = !tframes.empty();
        // Save smaller ▸ Under 30 MB, 8 MB (Discord), 25 MB (email), 50 MB, Other size…: the edit, sized to fit.
        MenuItem smaller;
        smaller.label = L"Save smaller";
        for (double mb : CompressSizesFor({path})) {
            MenuItem m;
            m.label = L"Under " + MegabytesLabel(mb) + (mb == 8 ? L"  (Discord)" : mb == 25 ? L"  (email)" : L"");
            m.run = [this, mb] { SaveSmaller(mb); };
            smaller.sub.push_back(m);
        }
        smaller.sub.push_back(MenuItem::Sep());
        MenuItem other;
        other.label = L"Other size…";
        other.run = [this] { AskSmallerSize(); };
        smaller.sub.push_back(other);
        // Copy clip ▸ the marked range (or the selected clip) as an MP4 file on the clipboard; Copy frame ▸ the picture.
        MenuItem clip, frame;
        clip.label = L"Copy clip\tCtrl+C";
        clip.run = [this] { Copy(); };
        clip.enabled = CanCopyClip();
        frame.label = L"Copy frame\tCtrl+Shift+C";
        frame.run = [this] { CopyFrame(); };
        frame.enabled = raw != nullptr;
        v.push_back(mp4);
        v.push_back(smaller);
        v.push_back(gif);
        v.push_back(MenuItem::Sep());
        v.push_back(clip);
        v.push_back(frame);
        v.push_back(MenuItem::Sep());
        v.push_back(review);
        return v;
    }

    void PaintStage(HDC dc, gp::Graphics& g) {
        const RECT st = StageRect();
        FillRR(g, st, (float)S(8), gp::Color(255, 0, 0, 0));
        const auto v = VideoRect();
        if (shown && magZoom <= 1) {
            MemDC src(shown->Handle());
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, (int)std::lround(v.X), (int)std::lround(v.Y), (int)std::lround(v.Width), (int)std::lround(v.Height), src, 0, 0, shown->Width(),
                       shown->Height(), SRCCOPY);
        } else if (shown) {  // magnified: the whole pixels on view, each drawn as a block (sharp from 2×)
            const int W = shown->Width(), H = shown->Height();
            const int x0 = std::clamp((int)std::floor(magX), 0, W - 1), y0 = std::clamp((int)std::floor(magY), 0, H - 1);
            const int x1 = std::clamp((int)std::ceil(magX + W / magZoom), x0 + 1, W), y1 = std::clamp((int)std::ceil(magY + H / magZoom), y0 + 1, H);
            const gp::PointF a = ToView(VPoint{(double)x0, (double)y0}), b = ToView(VPoint{(double)x1, (double)y1});
            HRGN clip = CreateRectRgn((int)std::lround(v.X), (int)std::lround(v.Y), (int)std::lround(v.X + v.Width), (int)std::lround(v.Y + v.Height));
            SelectClipRgn(dc, clip);
            MemDC src(shown->Handle());
            SetStretchBltMode(dc, magZoom >= 2 ? COLORONCOLOR : HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            const int dx0 = (int)std::lround(a.X), dy0 = (int)std::lround(a.Y);
            StretchBlt(dc, dx0, dy0, (int)std::lround(b.X) - dx0, (int)std::lround(b.Y) - dy0, src, x0, y0, x1 - x0, y1 - y0, SRCCOPY);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        }
        // Guides and handles are drawn above the video, so they show while it plays too.
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        if (magZoom > 1) g.SetClip(v);
        PaintGuides(g);
        const COLORREF accent = theme::kAccent;
        if (edit.crop) {
            const gp::RectF r = ToView(*edit.crop);
            gp::Region outside(v);
            outside.Exclude(r);
            gp::SolidBrush shade(gp::Color(cropping ? 140 : 178, 0, 0, 0));
            g.FillRegion(&shade, &outside);
            gp::Pen pen(A(accent), 1.5f * s);
            if (!cropping) {
                const gp::REAL dash[] = {3.3f, 2.7f};
                pen.SetDashPattern(dash, 2);
            }
            g.DrawRectangle(&pen, r);
        } else if (cropping) {
            const std::wstring t = L"Drag to crop";
            const SIZE sz = Measure(dc, fUi, t);
            const RECT pill{(LONG)(v.X + v.Width / 2 - sz.cx / 2 - S(12)), (LONG)(v.Y + v.Height / 2 - sz.cy / 2 - S(6)),
                            (LONG)(v.X + v.Width / 2 + sz.cx / 2 + S(12)), (LONG)(v.Y + v.Height / 2 + sz.cy / 2 + S(6))};
            FillRR(g, pill, (float)S(8), gp::Color(140, 0, 0, 0));
            Text(dc, fUi, t, pill, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        const double now = Now();
        if (auto mi = SelMark()) {
            const Mark& m = edit.marks[*mi];
            const bool on = m.Active(now);
            gp::Pen pen(A(accent, on ? 255 : 102), 1.5f * s);
            const gp::REAL dash[] = {3.3f, 2.7f};
            pen.SetDashPattern(dash, 2);
            if (m.kind == MarkKind::Zoom) {
                g.DrawRectangle(&pen, ToView(FrameRenderer::ZoomTarget(m.Rect(), ViewRect())));
                const gp::REAL dots[] = {1.3f, 2};
                pen.SetDashPattern(dots, 2);
                g.DrawRectangle(&pen, ToView(m.Rect()));
            } else if (KindIsLine(m.kind)) {
                g.DrawLine(&pen, ToView(m.a), ToView(m.b));
            } else if (m.kind != MarkKind::Title) {
                VRect r = m.Rect();
                if (m.kind == MarkKind::Bubble) r.h *= 1.32;
                gp::RectF vr = ToView(r);
                vr.Inflate(3 * s, 3 * s);
                g.DrawRectangle(&pen, vr);
            }
            if (on)
                for (const VPoint& hpt : Handles(m)) {
                    const gp::PointF c = ToView(hpt);
                    gp::SolidBrush white(gp::Color(255, 255, 255, 255));
                    gp::Pen ring(A(accent), 1.5f * s);
                    g.FillEllipse(&white, c.X - 5 * s, c.Y - 5 * s, 10 * s, 10 * s);
                    g.DrawEllipse(&ring, c.X - 5 * s, c.Y - 5 * s, 10 * s, 10 * s);
                }
        }
        if (auto ci = SelCaption(); ci && edit.captions[*ci].Active(now)) {
            gp::RectF r = ToView(CaptionRect(edit.captions[*ci]));
            r.Inflate(3 * s, 3 * s);
            gp::GraphicsPath p;
            RoundPath(p, r.X, r.Y, r.Width, r.Height, 8 * s);
            gp::Pen pen(A(accent), 1.5f * s);
            g.DrawPath(&pen, &p);
        }
        if (auto oi = SelOverlay(); oi && edit.overlays[*oi].Active(now)) {  // the overlay: its box (and corner to size it) or its half
            const Overlay& o = edit.overlays[*oi];
            const gp::RectF r = ToView(OverlayOnVideo(o));
            gp::Pen pen(A(accent), 2.f * s);
            g.DrawRectangle(&pen, r);
            if (!o.IsSplit()) {  // a handle on each corner, to size it from there
                gp::SolidBrush white(gp::Color(255, 255, 255, 255));
                gp::Pen ring(A(accent), 1.5f * s);
                for (const float cx : {r.GetLeft(), r.GetRight()})
                    for (const float cy : {r.GetTop(), r.GetBottom()}) {
                        g.FillEllipse(&white, cx - 6 * s, cy - 6 * s, 12 * s, 12 * s);
                        g.DrawEllipse(&ring, cx - 6 * s, cy - 6 * s, 12 * s, 12 * s);
                    }
            }
        }
        // Pins of the notes on the frame on screen.
        if (!playing && !tframes.empty()) {
            const size_t frame = FrameNow();
            for (const auto& [f, i] : placed) {
                const Note& n = edit.notes[i];
                const auto pin = f == frame ? PinOnVideo(n) : std::nullopt;
                if (!pin) continue;
                const gp::PointF c = ToView(*pin);
                const bool sel = selected == n.id;
                const float rr = (sel ? 9.f : 7.f) * s;
                gp::SolidBrush fill(A(annot::Color(NoteKindColor(n.kind)), n.resolved ? 150 : 255));
                gp::Pen ring(gp::Color(255, 255, 255, 255), (sel ? 3.f : 2.f) * s);
                gp::Pen shadow(gp::Color(120, 0, 0, 0), 5.f * s);
                g.DrawEllipse(&shadow, c.X - rr, c.Y - rr, 2 * rr, 2 * rr);
                g.FillEllipse(&fill, c.X - rr, c.Y - rr, 2 * rr, 2 * rr);
                g.DrawEllipse(&ring, c.X - rr, c.Y - rr, 2 * rr, 2 * rr);
            }
        }
        g.ResetClip();
        if (magZoom > 1) {  // how far the magnifier is in
            wchar_t z[16];
            swprintf_s(z, L"%g×", std::round(magZoom * 10) / 10);
            const SIZE sz = Measure(dc, fSmall, z);
            const RECT pill{(LONG)v.X + S(8), (LONG)v.Y + S(8), (LONG)v.X + S(8) + sz.cx + S(14), (LONG)v.Y + S(8) + sz.cy + S(8)};
            FillRR(g, pill, (float)S(5), gp::Color(190, 0, 0, 0));
            Text(dc, fSmall, z, pill, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            Hotspot(pill, [this] { FitMagnifier(); }, L"Fit the video again (F or double-click)");
        }
    }


    // The notes list, right of the video: kind, timecode, author and first line of each; All or Open only; Copy.
    void PaintNotes(HDC dc, gp::Graphics& g) {
        if (!NotesShown()) return;
        const RECT r = NotesRect();
        FillRR(g, r, (float)S(8), A(theme::kSurface));
        int open = 0;
        for (const auto& p : placed) open += !edit.notes[p.second].resolved;
        const int hy = r.top + S(8), hh = S(28);
        const std::wstring title = L"Notes";
        const int tw = Measure(dc, fUi, title).cx;
        Text(dc, fUi, title, {r.left + S(12), hy, r.left + S(12) + tw + S(2), hy + hh}, theme::kText);
        const std::wstring count = std::to_wstring(open) + L" open";
        Text(dc, fSmall, count, {r.left + S(18) + tw, hy, r.left + S(18) + tw + Measure(dc, fSmall, count).cx + S(4), hy + hh}, theme::kMuted);
        // Right to left: Copy, Open, All.
        const int copyW = S(9) * 2 + S(22) + Measure(dc, fUi, L"Copy").cx;
        RECT b = Button(dc, g, r.right - S(6) - copyW, hy, hh, 0xE8C8, L"Copy", false, false, [this] { CopyNotes(); },
                        L"Copy notes: one line each, as in the review notes list");
        const int openW = S(9) * 2 + Measure(dc, fUi, L"Open").cx, allW = S(9) * 2 + Measure(dc, fUi, L"All").cx;
        b = Button(dc, g, b.left - S(4) - openW, hy, hh, 0, L"Open", false, notesOpenOnly, [this] { notesOpenOnly = true; notesScroll = 0; Invalidate(); },
                   L"Only notes not resolved");
        Button(dc, g, b.left - S(2) - allW, hy, hh, 0, L"All", false, !notesOpenOnly, [this] { notesOpenOnly = false; notesScroll = 0; Invalidate(); },
               L"Every note, resolved ones too");
        const RECT list = NotesListRect();
        const auto v = ListedNotes();
        const int rowH = NoteRowH();
        notesScroll = std::clamp(notesScroll, 0, std::max(0, (int)v.size() * rowH - RectH(list)));
        if (v.empty()) {
            Text(dc, fSmall, L"No open notes", {list.left + S(12), list.top, list.right, list.top + S(30)}, theme::kMuted);
            return;
        }
        HRGN clip = CreateRectRgn(list.left, list.top, list.right, list.bottom);
        SelectClipRgn(dc, clip);
        g.SetClip(gp::Rect(list.left, list.top, RectW(list), RectH(list)));
        for (size_t k = 0; k < v.size(); ++k) {
            const int y = list.top + (int)k * rowH - notesScroll;
            if (y + rowH < list.top || y > list.bottom) continue;
            const auto [frame, i] = v[k];
            const Note& n = edit.notes[i];
            const RECT row{list.left + S(6), y + S(2), list.right - S(6), y + rowH - S(2)};
            const bool sel = selected == n.id;
            if (sel) FillRR(g, row, (float)S(6), A(theme::kSelected));
            else if (EqualRect(&row, &hoverRect)) FillRR(g, row, (float)S(6), A(theme::kBgRaised));
            const COLORREF kc = annot::Color(NoteKindColor(n.kind));
            FillRR(g, {row.left + S(6), row.top + S(8), row.left + S(10), row.bottom - S(8)}, (float)S(2), A(kc, n.resolved ? 110 : 255));
            const int x = row.left + S(18);
            const std::wstring tc = tframes.Timecode(frame);
            const int tcw = Measure(dc, fMono, tc).cx;
            Text(dc, fMono, tc, {x, row.top + S(5), x + tcw + S(2), row.top + S(23)}, theme::kText);
            const std::wstring kind = n.resolved ? std::wstring(L"✓ Resolved") : std::wstring(NoteKindLabel(n.kind));
            const int kw = Measure(dc, fSmall, kind).cx;
            Text(dc, fSmall, kind, {row.right - S(10) - kw, row.top + S(5), row.right - S(8), row.top + S(23)}, n.resolved ? theme::kMuted : kc);
            Text(dc, fSmall, n.author.empty() ? std::wstring(L"·") : n.author, {x + tcw + S(10), row.top + S(5), row.right - S(16) - kw, row.top + S(23)},
                 theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            std::wstring first = n.text.substr(0, n.text.find_first_of(L"\r\n"));
            if (first.empty()) first = L"(no text yet)";
            Text(dc, fUi, first, {x, row.top + S(25), row.right - S(8), row.bottom - S(4)}, n.resolved || n.text.empty() ? theme::kMuted : theme::kText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            const uint64_t id = n.id;
            RECT hot = row;
            hot.top = std::max(hot.top, list.top);
            hot.bottom = std::min(hot.bottom, list.bottom);
            if (hot.bottom > hot.top) Hotspot(hot, [this, id] { SelectNote(id); });
        }
        g.ResetClip();
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    }

    // The notes in the review video (those in the trim), in timeline order, as the review video shows them.
    ReviewPlan MakeReviewPlan() const {
        ReviewPlan p;
        p.frames = tframes;
        if (tframes.empty()) return p;
        const size_t first = tframes.At(edit.trimStart + 1e-3), last = tframes.At(edit.trimEnd - 1e-3);
        for (const auto& [f, i] : placed) {
            if (f < first || f > last) continue;
            const Note& n = edit.notes[i];
            ReviewNote r;
            r.t = tframes.frames[f].t;
            r.frame = f;
            r.timecode = tframes.Timecode(f);
            r.kind = n.kind;
            r.resolved = n.resolved;
            r.author = n.author.empty() ? std::wstring(L"Reviewer") : n.author;
            r.text = n.text;
            r.pin = PinOnVideo(n);
            if (std::find(p.reviewers.begin(), p.reviewers.end(), r.author) == p.reviewers.end()) p.reviewers.push_back(r.author);
            p.notes.push_back(r);
        }
        p.title = FileNameOf(path);
        wchar_t date[128] = L"";
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, nullptr, nullptr, date, (int)std::size(date), nullptr);
        p.date = date;
        return p;
    }
    // The notes list as text, one line each (the review notes list and Copy): "m:ss:ff (frame n), Author: [Kind] text"
    // for the notes in the review video, a blank line between them so Markdown shows each on its own line.
    static std::wstring NoteLine(const ReviewNote& n) {
        std::wstring text = n.text;
        for (size_t at = 0; (at = text.find_first_of(L"\r\n", at)) != std::wstring::npos;) {
            const size_t end = text.find_first_not_of(L"\r\n", at);
            text.replace(at, (end == std::wstring::npos ? text.size() : end) - at, L" / ");
            at += 3;
        }
        return n.timecode + L" (frame " + std::to_wstring(n.frame) + L"), " + n.author + L": [" + NoteKindLine(n.kind, n.resolved) + L"] " + text;
    }
    static std::wstring NotesListText(const ReviewPlan& p) {
        std::wstring s;
        for (const auto& n : p.notes) s += (s.empty() ? L"" : L"\r\n\r\n") + NoteLine(n);
        return s.empty() ? s : s + L"\r\n";
    }
    std::wstring NotesText() const { return NotesListText(MakeReviewPlan()); }
    std::function<void(const std::wstring&)> copyHook;  // tests: instead of the clipboard
    void CopyNotes() {
        const std::wstring t = NotesText();
        if (t.empty()) return;
        if (copyHook) return copyHook(t);
        CopyTextToClipboard(hwnd, t);

        ShowToast(L"Notes copied", std::to_wstring(std::count(t.begin(), t.end(), L'\n')) + L" lines", nullptr, nullptr, 2000);
    }

    // The per-item settings row, or a hint when nothing is selected.
    void PaintInspector(HDC dc, gp::Graphics& g) {
        const RECT row = InspectorRect();
        const int h = S(28), y = row.top + (RectH(row) - h) / 2;
        std::vector<std::function<int(HDC, gp::Graphics&, int)>> parts;  // each draws at x and returns its right edge
        bool show1 = false, show2 = false;
        int w1 = 0, w2 = 0;
        RECT f1{}, f2{};
        auto field = [&](int which, int width) {
            parts.push_back([&, which, width](HDC, gp::Graphics& gg, int x) {
                RECT r{x, y + S(2), x + S(width), y + h - S(2)};
                FillRR(gg, r, (float)S(5), A(theme::kBgRaised));
                (which == 1 ? f1 : f2) = r;
                return (int)r.right;
            });
            (which == 1 ? show1 : show2) = true;
            (which == 1 ? w1 : w2) = width;
        };
        auto button = [&](wchar_t glyph, std::wstring label, bool menu, std::function<void(RECT)> click, std::wstring tip = L"", HFONT font = nullptr) {
            parts.push_back([&, glyph, label, menu, click, tip, font](HDC d, gp::Graphics& gg, int x) {
                HFONT keep = fUi;
                if (font) fUi = font;
                RECT r = Button(d, gg, x, y, h, glyph, label, menu, false, [] {}, tip);
                fUi = keep;
                hots.back().click = [click, r] { click(r); };
                return (int)r.right;
            });
        };
        auto label = [&](std::wstring t) {
            parts.push_back([&, t](HDC d, gp::Graphics&, int x) {
                const int w = Measure(d, fSmall, t).cx;
                Text(d, fSmall, t, {x + S(4), y, x + S(4) + w, y + h}, theme::kMuted);
                return x + w + S(8);
            });
        };
        auto colorPopup = [&](int current, std::wstring prefix, std::function<void(int)> set) {
            button(0, prefix + L": " + annot::ColorName(current), true, [this, current, set](RECT r) { Popup(r, ColorItems(current, set)); });
        };
        auto levelPopup = [&](int current) {
            std::vector<std::wstring> labels;
            for (int i = 1; i <= annot::Levels(); ++i) labels.push_back(L"Size " + std::to_wstring(i));
            button(0, labels[std::clamp(current, 0, annot::Levels() - 1)], true, [this, labels, current](RECT r) {
                Popup(r, ListMenu(labels, current, [this](int i) { UpdateMark([i](Mark& m) { m.level = i; }); }));
            });
        };
        const auto ci = goingTo ? std::nullopt : SelCaption();
        const auto mi = goingTo ? std::nullopt : SelMark();
        const auto ni = goingTo ? std::nullopt : SelNote();
        if (goingTo) {
            label(L"Go to");
            field(1, 200);
            label(L"a frame number, a timecode (m:ss:ff) or a time in seconds · Enter goes, Esc cancels");
        } else if (ni) {
            const Note n = edit.notes[*ni];
            const uint64_t id = n.id;
            button(0, NoteKindLabel(n.kind), true, [this, id](RECT r) { Popup(r, NoteKindMenu(id)); }, L"Note, Issue, Question or Looks good");
            field(1, 250);
            label(L"by");
            field(2, 110);
            button(n.resolved ? 0xE73E : 0, n.resolved ? L"Resolved" : L"Resolve", false, [this](RECT) { UpdateNote([](Note& x) { x.resolved = !x.resolved; }); },
                   n.resolved ? L"Open it again" : L"Mark as resolved: it stays in the list and the review video, marked Resolved");
            if (n.pin) button(0, L"Clear pin", false, [this](RECT) { UpdateNote([](Note& x) { x.pin.reset(); }); }, L"Remove the spot it points at");
            else label(L"Click the video to pin a spot");
            if (n.srcEnd) button(0, L"No range", false, [this](RECT) { UpdateNote([](Note& x) { x.srcEnd.reset(); }); }, L"Back to a single frame");
            else button(0, L"Range to here", false, [this](RECT) { SetNoteRangeToPlayhead(); }, L"Make the note run to the frame at the playhead");
        } else if (ci) {
            const Caption& c = edit.captions[*ci];
            field(1, 300);
            std::vector<std::wstring> pos = {L"Bottom", L"Middle", L"Top"};
            if (c.center) pos.push_back(L"Custom");
            const int cur = c.center ? 3 : (int)c.position;
            button(0, pos[cur], true, [this, pos, cur](RECT r) {
                Popup(r, ListMenu(pos, cur, [this](int i) {
                    if (i < 3) UpdateCaption([i](Caption& x) {
                        x.position = (CaptionPosition)i;
                        x.center.reset();
                    });
                }));
            }, L"Or drag the caption on the video");
            // Style is shared by every caption, so they stay consistent.
            label(L"All captions:");
            colorPopup(edit.captionColor, L"Text", [this](int i) { SetCaptions([i](VideoEdit& e) { e.captionColor = i; }); });
            colorPopup(edit.captionEdge, edit.captionLook == CaptionLook::Outline ? L"Outline" : L"Box",
                       [this](int i) { SetCaptions([i](VideoEdit& e) { e.captionEdge = i; }); });
            std::vector<std::wstring> sizes(std::begin(VideoEdit::kCaptionSizes), std::end(VideoEdit::kCaptionSizes));
            const int cs = edit.captionSize;
            button(0, sizes[cs], true, [this, sizes, cs](RECT r) { Popup(r, ListMenu(sizes, cs, [this](int i) { SetCaptions([i](VideoEdit& e) { e.captionSize = i; }); })); });
        } else if (mi) {
            const Mark m = edit.marks[*mi];
            switch (m.kind) {
                case MarkKind::Title:
                    field(1, 240);
                    field(2, 220);
                    colorPopup(m.color, L"Background", [this](int i) { UpdateMark([i](Mark& x) { x.color = i; }); });
                    break;
                case MarkKind::Emoji:
                    field(1, 110);
                    for (const wchar_t* e : kQuickEmoji) {
                        const std::wstring em = e;
                        button(0, em, false, [this, em](RECT) {
                            UpdateMark([em](Mark& x) { x.text = em; });
                            fieldsFor = 0;
                            SyncFields();
                        }, L"", fEmoji);
                    }
                    break;
                case MarkKind::Text:
                case MarkKind::Bubble:
                    field(1, 300);
                    colorPopup(m.color, m.kind == MarkKind::Bubble ? L"Bubble" : L"Color", [this](int i) { UpdateMark([i](Mark& x) { x.color = i; }); });
                    levelPopup(m.level);
                    break;
                case MarkKind::Arrow:
                case MarkKind::Box:
                case MarkKind::Ellipse:
                case MarkKind::Step:
                    colorPopup(m.color, L"Color", [this](int i) { UpdateMark([i](Mark& x) { x.color = i; }); });
                    levelPopup(m.level);
                    break;
                case MarkKind::Blur:
                case MarkKind::Pixelate: {
                    const int cur = m.kind == MarkKind::Blur ? 0 : 1;
                    button(0, cur ? L"Pixelate" : L"Blur", true, [this, cur](RECT r) {
                        Popup(r, ListMenu({L"Blur", L"Pixelate"}, cur, [this](int i) { UpdateMark([i](Mark& x) { x.kind = i == 0 ? MarkKind::Blur : MarkKind::Pixelate; }); }));
                    });
                    std::vector<std::wstring> lv;
                    for (int i = 1; i <= 5; ++i) lv.push_back(L"Strength " + std::to_wstring(i));
                    const int l = std::clamp(m.level, 0, 4);
                    button(0, lv[l], true, [this, lv, l](RECT r) { Popup(r, ListMenu(lv, l, [this](int i) { UpdateMark([i](Mark& x) { x.level = i; }); })); });
                    break;
                }
                case MarkKind::Zoom: {
                    const int cur = m.snappy ? 1 : 0;
                    button(0, cur ? L"Snappy zoom" : L"Smooth zoom", true, [this, cur](RECT r) {
                        Popup(r, ListMenu({L"Smooth zoom", L"Snappy zoom"}, cur, [this](int i) { UpdateMark([i](Mark& x) { x.snappy = i == 1; }); }));
                    });
                    label(L"The box sets how far it zooms · plays back zoomed");
                    break;
                }
            }
            if (!KindStyles(m.kind).empty()) {
                const uint64_t id = m.id;
                button(0, AnimationTitle(m), true, [this, id](RECT r) {
                    if (auto i = SelMark(); i && edit.marks[*i].id == id) Popup(r, AnimationMenu(edit.marks[*i]));
                });
                button(0xE768, L"", false, [this, m](RECT) { Replay(m); }, L"Play this item from just before it appears");
            }
        }
        if (ci || mi || ni) button(0xE74D, L"Delete", false, [this](RECT) { DeleteSelected(); }, L"Delete (Del)");
        if (const auto oi = goingTo ? std::nullopt : SelOverlay()) {
            const Overlay o = edit.overlays[*oi];
            label(L"Video on top:  " + FileNameOf(o.clip.path) + L"  ·  " + Clock(o.clip.Duration()) + L"  ·  no sound");
            std::vector<std::wstring> layouts;
            for (int k = 0; k <= (int)OverlayLayout::SplitBottom; ++k) layouts.push_back(OverlayLayoutLabel((OverlayLayout)k));
            button(0, OverlayLayoutLabel(o.layout), true, [this, layouts, o](RECT r) {
                Popup(r, ListMenu(layouts, (int)o.layout, [this](int k) { UpdateOverlay([k](Overlay& x) { x.layout = (OverlayLayout)k; }); }));
            }, L"In a box on top, or side by side with the video");
            if (o.IsSplit()) {
                button(0, o.fill ? L"Fill halves" : L"Fit halves", true, [this, o](RECT r) {
                    Popup(r, ListMenu({L"Fit: all of each picture, with bars", L"Fill: each half filled, cut to fit"}, o.fill ? 1 : 0,
                                      [this](int k) { UpdateOverlay([k](Overlay& x) { x.fill = k == 1; }); }));
                });
            } else {
                static const double sizes[] = {0.22, 0.34, 0.5};
                const std::vector<std::wstring> names = {L"Small", L"Medium", L"Large"};
                int cur = 0;
                for (int k = 1; k < 3; ++k)
                    if (std::fabs(o.width - sizes[k]) < std::fabs(o.width - sizes[cur])) cur = k;
                button(0, names[cur], true, [this, names, cur](RECT r) {
                    Popup(r, ListMenu(names, cur, [this](int k) { UpdateOverlay([k](Overlay& x) { x.width = sizes[k]; }); }));
                }, L"Its size (or drag its corner on the video)");
            }
            button(0xE70E, L"To bin", false, [this](RECT) { DeleteSelected(); }, L"Take it off into the bin (Del)");
        }
        if (const auto k = SelClipIndex(); k && !ci && !mi && !ni && !goingTo) {
            const size_t i = *k;
            const Clip& c = edit.clips[i];
            label(L"Clip " + std::to_wstring(i + 1) + L" of " + std::to_wstring(edit.clips.size()) + L":  " + FileNameOf(c.path) + L"  ·  " + Clock(c.Duration()));
            button(0xE8C6, L"Split at playhead", false, [this](RECT) { SplitAtPlayhead(); }, L"Cut this clip in two at the playhead (S)");
            if (i > 0) button(0xE760, L"Earlier", false, [this, i](RECT) { MoveClip(i, i - 1); }, L"Play this clip before the one on its left");
            if (i + 1 < edit.clips.size()) button(0xE761, L"Later", false, [this, i](RECT) { MoveClip(i, i + 1); }, L"Play this clip after the one on its right");
            if (edit.clips.size() > 1) button(0xE70E, L"To bin", false, [this, i](RECT) { MoveToBin(i); }, L"Take this clip out into the bin, to put back later (Del, or drag it up)");
        }
        if (const auto k = SelBinIndex(); k && !ci && !mi && !ni && !goingTo) {
            const size_t i = *k;
            const Clip& c = edit.bin[i];
            label(L"In the bin:  " + FileNameOf(c.path) + L"  ·  " + Clock(c.in) + L"–" + Clock(c.out) + L"  ·  " + Clock(c.Duration()));
            button(0xE710, L"Add at end", false, [this, i](RECT) { PlaceFromBin(i, edit.clips.size()); }, L"Put it back at the end of the video (or drag it onto the timeline)");
            button(0, L"On top", false, [this, i](RECT) { BinToOverlay(i, InsertTime()); },
                   L"Show it on top from the playhead, in a box or side by side (or drag it to the bottom lane)");
            button(0xE74D, L"Delete", false, [this, i](RECT) { DeleteFromBin(i); }, L"Delete this piece for good (Del)");
        }

        if (parts.empty()) {
            Text(dc, fSmall, L"Space plays · I and O trim · S split · Del to bin · Shift+drag mark · Ctrl+C copy · G guides · M note · [ ] notes · T caption · A arrow · R box · E emoji · X blur · Z zoom · C crop · drop videos to join",
                 row, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            // Measure by drawing off-screen first, then center the row.
            const size_t hotsBefore = hots.size();
            auto scratch = Bitmap::Create(1, 1);
            int width = 0;
            {
                MemDC mdc(scratch->Handle());
                gp::Graphics mg(mdc);
                int x = 0;
                for (auto& p : parts) x = p(mdc, mg, x) + S(6);
                width = x - S(6);
            }
            hots.resize(hotsBefore);
            const int x0 = std::max((int)row.left, (int)(row.left + (RectW(row) - width) / 2));
            int x = x0;
            for (auto& p : parts) x = p(dc, g, x) + S(6);
        }
        // The edit boxes are child windows placed over their slots.
        auto place = [&](HWND f, bool show, const RECT& r) {
            if (!f) return;
            if (show && fieldsFor != 0) {
                SetWindowPos(f, nullptr, r.left + S(6), r.top + (RectH(r) - S(18)) / 2, RectW(r) - S(12), S(18), SWP_NOZORDER | SWP_NOACTIVATE);
                if (!(GetWindowLongW(f, GWL_STYLE) & WS_VISIBLE)) ShowWindow(f, SW_SHOWNA);
            } else if (GetWindowLongW(f, GWL_STYLE) & WS_VISIBLE) {
                if (GetFocus() == f) SetFocus(hwnd);
                ShowWindow(f, SW_HIDE);
            }
        };
        place(field1, show1, f1);
        place(field2, show2, f2);
        (void)w1;
        (void)w2;
    }

    // A timeline picture stretched into place, mirrored as its clip `c` is flipped (the frames are decoded as they are).
    void PutThumb(HDC dc, int x, int y, int w, int h, HDC src, int sw, int sh, const Clip* c) const {
        const bool fh = c && c->flipH, fv = c && c->flipV;
        StretchBlt(dc, fh ? x + w - 1 : x, fv ? y + h - 1 : y, fh ? -w : w, fv ? -h : h, src, 0, 0, sw, sh, SRCCOPY);
    }
    const Clip* ClipAt(double t) const {
        const auto spot = LocateClip(edit.clips, t);
        return spot ? &edit.clips[spot->first] : nullptr;
    }
    const Clip* ClipOfFrame(size_t n) const {
        return n < tframes.size() && tframes.frames[n].clip < edit.clips.size() ? &edit.clips[tframes.frames[n].clip] : nullptr;
    }

    void PaintTimeline(HDC dc, gp::Graphics& g) {
        const RECT tr = TimelineRect();
        const int top = tr.top + LaneH();
        const int stripH = S(52), capY = top + S(62), capH = S(22), markY = top + S(90), rowH = S(18);
        const RECT strip{tr.left, top, tr.right, top + stripH};
        FillRR(g, strip, (float)S(6), A(theme::kSurface));
        // Zoomed in, things off the edges are cut off there.
        const RECT view{tr.left - S(6), tr.top - S(8), tr.right + S(6), tr.bottom};
        HRGN viewRgn = CreateRectRgnIndirect(&view);
        SelectClipRgn(dc, viewRgn);
        g.SetClip(gp::Rect(view.left, view.top, RectW(view), RectH(view)));
        if (const auto tiles = StripTiles(); !tiles.empty()) {  // zoomed in: tiles showing their own frames
            HRGN clip = CreateRectRgn(strip.left, strip.top, strip.right, strip.bottom);
            SelectClipRgn(dc, clip);
            SetStretchBltMode(dc, HALFTONE);
            for (const auto& tile : tiles) {
                const int x0 = (int)std::lround(TX(tile.t0)), x1 = (int)std::lround(TX(tile.t1));
                const auto it = stripThumbs.find(tile.frame);
                if (it == stripThumbs.end() || !it->second) continue;  // still coming: the strip's own color meanwhile
                MemDC src(it->second->Handle());
                PutThumb(dc, x0, strip.top, std::max(1, x1 - x0), RectH(strip), src, it->second->Width(), it->second->Height(), ClipOfFrame(tile.frame));
            }
            for (const auto& tile : tiles) {  // a hairline between tiles
                const int x = (int)std::lround(TX(tile.t0));
                FillSolid(dc, {x, strip.top, x + 1, strip.bottom}, RGB(0, 0, 0));
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
            RequestStrip(tiles);
        } else if (!thumbs.empty()) {
            const double D = std::max(0.001, duration);
            HRGN clip = CreateRectRgn(strip.left, strip.top, strip.right, strip.bottom);
            SelectClipRgn(dc, clip);
            SetStretchBltMode(dc, HALFTONE);
            for (size_t i = 0; i < thumbs.size(); ++i) {
                const auto& img = thumbs[i];
                const RECT cell{(LONG)TX(D * i / thumbs.size()), strip.top, (LONG)std::ceil(TX(D * (i + 1) / thumbs.size())), strip.bottom};
                if (cell.right < strip.left || cell.left > strip.right) continue;
                const double k = std::max(RectW(cell) / (double)img->Width(), RectH(cell) / (double)img->Height());
                const int dw = (int)std::ceil(img->Width() * k), dh = (int)std::ceil(img->Height() * k);
                HRGN cr = CreateRectRgnIndirect(&cell);
                ExtSelectClipRgn(dc, cr, RGN_AND);
                MemDC src(img->Handle());  // fitted: each of the 16 shows the frame at its middle
                PutThumb(dc, (cell.left + cell.right) / 2 - dw / 2, (cell.top + cell.bottom) / 2 - dh / 2, dw, dh, src, img->Width(), img->Height(), ClipAt(D * (i + 0.5) / thumbs.size()));
                SelectClipRgn(dc, clip);
                DeleteObject(cr);
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        }
        if (LaneH()) PaintClips(dc, g, tr, strip);
        PaintRuler(dc, g, strip);
        const int xs = (int)TX(edit.trimStart), xe = (int)TX(edit.trimEnd);
        gp::SolidBrush dim(gp::Color(166, 0, 0, 0));
        g.FillRectangle(&dim, (float)strip.left, (float)strip.top, (float)(xs - strip.left), (float)stripH);
        g.FillRectangle(&dim, (float)xe, (float)strip.top, (float)(strip.right - xe), (float)stripH);
        {
            gp::GraphicsPath p;
            RoundPath(p, xs + 1.f, strip.top + 1.f, (float)(xe - xs - 2), stripH - 2.f, (float)S(5));
            gp::Pen pen(A(theme::kAccent), 2.5f * s);
            g.DrawPath(&pen, &p);
            for (int hx : {xs, xe}) FillRR(g, {hx - S(4), strip.top + S(8), hx + S(4), strip.bottom - S(8)}, (float)S(3), A(theme::kAccent));
        }
        auto lane = [&](RECT r, const std::wstring& empty, bool isEmpty) {
            FillRR(g, r, (float)S(5), gp::Color(10, 255, 255, 255));
            if (isEmpty) Text(dc, fSmall, empty, {r.left + S(8), r.top, r.right, r.top + std::min((int)RectH(r), S(22))}, theme::kMuted);
        };
        auto bar = [&](RECT r, const std::wstring& label, wchar_t glyph, bool sel) {
            FillRR(g, r, (float)S(4), sel ? A(theme::kAccent) : gp::Color(46, 255, 255, 255));
            const COLORREF fg = sel ? theme::kOnAccent : theme::kText;
            HRGN clip = CreateRectRgn(r.left + S(3), r.top, r.right - S(3), r.bottom);
            SelectClipRgn(dc, clip);
            int x = r.left + S(5);
            if (glyph) {
                Text(dc, fIconSmall, std::wstring(1, glyph), {x, r.top, x + S(12), r.bottom}, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                x += S(15);
            }
            Text(dc, fSmall, label, {x, r.top, r.right, r.bottom}, fg);
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        };
        auto barRect = [&](double s0, double e0, int y, int h) {
            const int x0 = (int)TX(s0);
            return RECT{x0, y, x0 + std::max(S(6), (int)TX(e0) - x0), y + h};
        };
        lane({tr.left, capY, tr.right, capY + capH}, L"Captions", edit.captions.empty());
        for (const auto& c : edit.captions)
            bar(barRect(c.start, c.end, capY + S(2), capH - S(4)), c.text.empty() ? L"…" : c.text, 0, selected == c.id);
        const int rows = RowCount();
        lane({tr.left, markY, tr.right, markY + rowH * rows}, L"Text, emoji, callouts, blur, zoom and titles: Add ▾", edit.marks.empty());
        for (const auto& [m, row] : Rows()) {
            if (row >= rows) continue;
            const std::wstring label = KindHasText(m.kind) && !m.text.empty() ? m.text : m.kind == MarkKind::Step ? L"Step " + std::to_wstring(m.step) : KindLabel(m.kind);
            bar(barRect(m.start, m.end, markY + row * rowH + S(1), rowH - S(2)), label, KindGlyph(m.kind), selected == m.id);
        }
        {  // overlay videos: a bar each; it lights up while a bin piece is carried over it
            const RECT ol = OverlayLaneRect();
            const bool taking = drag.kind == DragKind::BinMove && drag.toOverlay;
            lane(ol, L"Videos on top: Add ▾ › Video on top or side by side, or drag a piece here from the bin", edit.overlays.empty() && !taking);
            if (taking) {
                gp::GraphicsPath p;
                RoundPath(p, ol.left + 0.5f, ol.top + 0.5f, RectW(ol) - 1.f, RectH(ol) - 1.f, (float)S(5));
                gp::Pen pen(A(theme::kAccent), 1.5f * s);
                g.DrawPath(&pen, &p);
                gp::SolidBrush mark(A(theme::kAccent));
                g.FillRectangle(&mark, (float)TX(drag.value) - 1.5f * s, (float)ol.top, 3 * s, (float)RectH(ol));
            }
            for (const auto& o : edit.overlays)
                bar(barRect(o.start, o.End(), ol.top + S(2), RectH(ol) - S(4)), (o.IsSplit() ? L"Split · " : L"On top · ") + FileNameOf(o.clip.path), 0,
                    selected == o.id);
        }
        // Notes: a flag at the top of the strip and a line down it, in the kind's color (faint once resolved); a range
        // gets a band along the bottom.
        for (const auto& [f, i] : placed) {
            const Note& n = edit.notes[i];
            const float x = (float)TX(tframes.frames[f].t);
            const BYTE alpha = n.resolved ? 120 : 255;
            const gp::Color kc = A(annot::Color(NoteKindColor(n.kind)), alpha);
            if (const auto e = NoteEndFrame(n); e && *e > f) {
                gp::SolidBrush band(A(annot::Color(NoteKindColor(n.kind)), 150));
                g.FillRectangle(&band, x, (float)(strip.bottom - S(6)), (float)TX(tframes.frames[*e].t) - x, (float)S(5));
            }
            gp::SolidBrush line(kc), edge(gp::Color(n.resolved ? 90 : 170, 0, 0, 0));
            g.FillRectangle(&edge, x - 2 * s, (float)strip.top, 4 * s, (float)stripH);  // a dark edge, to show on any picture
            g.FillRectangle(&line, x - 1 * s, (float)strip.top, 2 * s, (float)stripH);
            gp::PointF flag[] = {{x - 7 * s, (float)strip.top}, {x + 7 * s, (float)strip.top}, {x, (float)strip.top + 11 * s}};
            g.FillPolygon(&line, flag, 3);
            gp::Pen outline(selected == n.id ? gp::Color(255, 255, 255, 255) : gp::Color(170, 0, 0, 0), (selected == n.id ? 1.5f : 1.f) * s);
            g.DrawPolygon(&outline, flag, 3);
        }
        if (marked) PaintMarked(dc, g, tr, strip);
        const float px = (float)TX(Now());

        gp::SolidBrush white(gp::Color(255, 255, 255, 255));
        g.FillRectangle(&white, px - s, (float)tr.top - S(2), 2 * s, (float)RectH(tr) + S(2));
        g.FillEllipse(&white, px - 5 * s, (float)tr.top - S(4), 10 * s, 10 * s);
        g.ResetClip();
        SelectClipRgn(dc, nullptr);
        DeleteObject(viewRgn);

        // Play button and time, left of the timeline.
        const RECT play{S(14), tr.top + S(6), S(14) + S(34), tr.top + S(40)};
        if (EqualRect(&play, &hoverRect)) FillRR(g, play, (float)S(6), A(theme::kBgRaised));
        Text(dc, fIcon, playing || reviewDir ? L"" : L"", play, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Hotspot(play, [this] { TogglePlay(); }, L"Play / pause (Space)");
        const RECT loopBtn{play.right + S(4), play.top, play.right + S(4) + S(34), play.bottom};  // round and round
        if (loop) FillRR(g, loopBtn, (float)S(6), A(theme::kSelected));
        else if (EqualRect(&loopBtn, &hoverRect)) FillRR(g, loopBtn, (float)S(6), A(theme::kBgRaised));
        Text(dc, fIcon, L"\xE8EE", loopBtn, loop ? theme::kAccent : theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Hotspot(loopBtn, [this] { ToggleLoop(); },
                loop ? L"Loop is on (Ctrl+L): plays the selected clip, or the trim, over and over" : L"Loop (Ctrl+L): play the selected clip, or the trim, over and over");
        Text(dc, fMono, tframes.empty() ? Clock(Now()) : tframes.Timecode(FrameNow()), {S(12), play.bottom + S(8), tr.left - S(4), play.bottom + S(24)},
             theme::kTextDim);
        Text(dc, fMono, Clock(edit.OutputDuration()) + L" out", {S(12), play.bottom + S(24), tr.left - S(4), play.bottom + S(40)}, theme::kMuted);
    }

    // Zoomed in: a ruler along the bottom of the strip, a tick per frame once they're 4 px apart, and frame numbers
    // every so many frames, far enough apart to read.
    void PaintRuler(HDC dc, gp::Graphics& g, const RECT& strip) {
        if (tlZoom <= 1.001 || tframes.empty()) return;
        const int bandH = S(17);
        const RECT band{strip.left, strip.bottom - bandH, strip.right, strip.bottom};
        gp::SolidBrush bg(gp::Color(170, 0, 0, 0));
        g.FillRectangle(&bg, (float)band.left, (float)band.top, (float)RectW(band), (float)bandH);
        const double pf = PxPerFrame();
        size_t step = 1;
        for (size_t k : {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200, 3000, 6000, 18000, 36000})
            if ((step = k) * pf >= S(46)) break;
        const size_t first = tframes.At(tlStart), last = std::min(tframes.size() - 1, tframes.At(tlStart + TlSpan()) + 1);
        gp::SolidBrush tick(gp::Color(200, 255, 255, 255)), major(gp::Color(255, 255, 255, 255));
        const size_t every = pf >= 4 ? 1 : step;
        for (size_t n = first - first % every; n <= last; n += every) {
            const float x = (float)TX(tframes.frames[n].t);
            if (x < strip.left - 1 || x > strip.right + 1) continue;
            const bool labeled = n % step == 0;
            g.FillRectangle(labeled ? &major : &tick, x - 0.5f * s, (float)(band.bottom - (labeled ? S(7) : S(4))), 1 * s, (float)(labeled ? S(7) : S(4)));
            if (labeled) {
                const std::wstring num = std::to_wstring(n);
                const int w = Measure(dc, fSmall, num).cx;
                if (x + S(3) + w <= strip.right)  // whole, or not at all
                    Text(dc, fSmall, num, {(LONG)x + S(3), band.top, (LONG)x + S(3) + w + S(2), band.bottom - S(4)}, RGB(255, 255, 255));
            }
        }
    }

    // The marked range: a band down the timeline from the thumbnails, grips on its edges, and a label with its length
    // and the key that copies it.
    void PaintMarked(HDC dc, gp::Graphics& g, const RECT& tr, const RECT& strip) {
        const float x0 = (float)TX(marked->start), x1 = (float)TX(marked->end), h = (float)(tr.bottom - strip.top);
        if (x1 < tr.left || x0 > tr.right) return;  // zoomed in elsewhere
        gp::SolidBrush band(A(theme::kAccent, 56)), edge(A(theme::kAccent));
        g.FillRectangle(&band, x0, (float)strip.top, x1 - x0, h);
        for (float x : {x0, x1}) {
            g.FillRectangle(&edge, x - s, (float)strip.top, 2 * s, h);
            const RECT grip{(LONG)std::lround(x) - S(4), strip.top + S(12), (LONG)std::lround(x) + S(4), strip.bottom - S(12)};
            FillRR(g, grip, (float)S(3), gp::Color(255, 255, 255, 255));
            gp::GraphicsPath p;
            RoundPath(p, (float)grip.left, (float)grip.top, (float)RectW(grip), (float)RectH(grip), (float)S(3));
            gp::Pen ring(A(theme::kOnAccent), 1.f * s);
            g.DrawPath(&ring, &p);
        }
        // The label sits on the band, or beside it when the band is too narrow, and stays on view.
        const std::wstring label = MarkedLabel();
        const int w = Measure(dc, fSmall, label).cx + S(16), lh = S(18);
        int lx = (int)std::lround((x0 + x1) / 2) - w / 2;
        if (x1 - x0 < w + S(16)) lx = x1 + S(8) + w <= tr.right ? (int)x1 + S(8) : (int)x0 - S(8) - w;
        lx = std::clamp(lx, (int)tr.left, std::max((int)tr.left, (int)tr.right - w));
        const RECT pill{lx, strip.top + (RectH(strip) - lh) / 2, lx + w, strip.top + (RectH(strip) + lh) / 2};
        FillRR(g, pill, (float)S(5), A(theme::kAccent));
        Text(dc, fSmall, label, pill, theme::kOnAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Clip lane: one bar per clip (name and length), cut lines across the thumbnails, and what a drag would do.
    void PaintClips(HDC dc, gp::Graphics& g, const RECT& tr, const RECT& strip) {
        const int y = tr.top, h = LaneH() - S(4);
        for (size_t i = 0; i < edit.clips.size(); ++i) {
            const Clip& c = edit.clips[i];
            const double t0 = ClipStart(edit.clips, i), t1 = t0 + c.Duration();
            RECT r{(LONG)TX(t0) + S(1), y, (LONG)TX(t1) - S(1), y + h};
            if (drag.clip == i && (drag.kind == DragKind::ClipIn || drag.kind == DragKind::ClipOut)) {  // the proposed cut
                if (drag.kind == DragKind::ClipIn) r.left = (LONG)TX(t0 + drag.value - c.in);
                else r.right = (LONG)TX(t0 + drag.value - c.in);
            }
            const bool sel = selClip == c.id;
            const bool lifted = drag.kind == DragKind::ClipMove && drag.moved && drag.clip == i;
            PaintClipChip(dc, g, r, c, sel, lifted ? 120 : 255);
            if (sel)  // trim grips
                for (LONG hx : {r.left, r.right}) FillRR(g, {hx - S(2), r.top + S(4), hx + S(2), r.bottom - S(4)}, (float)S(2), A(theme::kOnAccent));
            if (i > 0) {
                gp::SolidBrush cut(gp::Color(230, 0, 0, 0));
                g.FillRectangle(&cut, (float)TX(t0) - s, (float)strip.top, 2 * s, (float)RectH(strip));
            }
        }
        if (drag.kind == DragKind::ClipMove && drag.target >= 0 && drag.clip < edit.clips.size()) {  // where the clip would go
            std::vector<Clip> order = edit.clips;
            order.erase(order.begin() + drag.clip);
            const float x = (float)TX(ClipStart(order, (size_t)drag.target));
            gp::SolidBrush mark(A(theme::kAccent));
            g.FillRectangle(&mark, x - 1.5f * s, (float)y - S(2), 3 * s, (float)(strip.bottom - y + S(2)));
        }
        if (drag.kind == DragKind::BinMove && drag.target >= 0) {  // where the bin piece would go
            const float x = (float)TX(ClipStart(edit.clips, (size_t)drag.target));
            gp::SolidBrush mark(A(theme::kAccent));
            g.FillRectangle(&mark, x - 1.5f * s, (float)y - S(2), 3 * s, (float)(strip.bottom - y + S(2)));
        }
    }

    // A clip's bar: its name and length, clipped to the bar.
    void PaintClipChip(HDC dc, gp::Graphics& g, const RECT& r, const Clip& c, bool sel, BYTE alpha) {
        FillRR(g, r, (float)S(4), sel ? A(theme::kAccent, alpha) : gp::Color((BYTE)(46 * alpha / 255), 255, 255, 255));
        HRGN clip = CreateRectRgn(r.left + S(3), r.top, r.right - S(3), r.bottom);
        SelectClipRgn(dc, clip);
        const std::wstring flips = std::wstring(c.flipH ? L"  ↔" : L"") + (c.flipV ? L"  ↕" : L"");  // flipped, at a glance
        Text(dc, fSmall, FileNameOf(c.path) + L"  " + Clock(c.Duration()) + flips, {r.left + S(7), r.top, r.right, r.bottom}, sel ? theme::kOnAccent : theme::kText);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    }

    // The bin lane: a chip per piece taken off the timeline. It lights up while a clip is dragged up to it, and a bin
    // piece being dragged follows the mouse.
    void PaintBin(HDC dc, gp::Graphics& g) {
        if (!BinH()) return;
        const RECT r = BinRect();
        const bool taking = drag.kind == DragKind::ClipMove && drag.toBin;
        FillRR(g, r, (float)S(5), taking ? A(theme::kAccent, 50) : gp::Color(10, 255, 255, 255));
        if (taking) {
            gp::GraphicsPath p;
            RoundPath(p, r.left + 0.5f, r.top + 0.5f, RectW(r) - 1.f, RectH(r) - 1.f, (float)S(5));
            gp::Pen pen(A(theme::kAccent), 1.5f * s);
            g.DrawPath(&pen, &p);
        }
        Text(dc, fSmall, L"Bin", {S(14), r.top, r.left - S(8), r.bottom}, theme::kTextDim);
        if (edit.bin.empty()) {
            Text(dc, fSmall, L"Clips taken out go here: Del, or drag a clip up · drag them back down to use them again", {r.left + S(8), r.top, r.right - S(8), r.bottom},
                 theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return;
        }
        const auto chips = BinChips();
        const bool carrying = drag.kind == DragKind::BinMove && drag.moved && drag.clip < edit.bin.size();
        for (size_t i = 0; i < chips.size(); ++i)
            PaintClipChip(dc, g, chips[i], edit.bin[i], selClip == edit.bin[i].id, carrying && drag.clip == i ? 90 : 255);
        if (carrying) {  // the piece beside the mouse, short, so the insertion mark under the mouse shows
            const RECT& from = chips[drag.clip];
            const int w = std::min((int)RectW(from), S(180)), h = RectH(from);
            const RECT at{drag.at.x + S(10), drag.at.y - h / 2, drag.at.x + S(10) + w, drag.at.y - h / 2 + h};
            PaintClipChip(dc, g, at, edit.bin[drag.clip], true, 200);
        }
    }

    void PaintTooltip(HDC dc, gp::Graphics& g) {
        for (const auto& h : hots)
            if (EqualRect(&h.r, &hoverRect) && !h.tip.empty()) {
                const SIZE sz = Measure(dc, fSmall, h.tip);
                const RECT c = Client();
                int x = std::min((int)h.r.left, (int)(c.right - sz.cx - S(20)));
                RECT r{x, h.r.bottom + S(6), x + sz.cx + S(16), h.r.bottom + S(6) + sz.cy + S(10)};
                if (r.bottom > c.bottom) OffsetRect(&r, 0, -(RectH(h.r) + RectH(r) + S(12)));
                FillRR(g, r, (float)S(5), gp::Color(245, 40, 41, 38));
                Text(dc, fSmall, h.tip, r, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return;
            }
    }

    void Paint(HDC target) {
        const RECT c = Client();
        const int w = std::max(1L, c.right), h = std::max(1L, c.bottom);
        if (!backBuffer || backBuffer->Width() != w || backBuffer->Height() != h) backBuffer = Bitmap::Create(w, h);  // reused while the size holds
        BitmapPtr back = backBuffer;
        if (!back) return;
        {
            MemDC dc(back->Handle());
            hots.clear();
            FillSolid(dc, {0, 0, w, h}, theme::kBg);
            gp::Graphics g(dc);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            PaintToolbar(dc, g);
            PaintStage(dc, g);
            PaintNotes(dc, g);
            PaintReadout(dc, g);
            PaintInspector(dc, g);
            PaintTimeline(dc, g);
            PaintBin(dc, g);
            if (tipShown) PaintTooltip(dc, g);
        }
        MemDC src(back->Handle());
        BitBlt(target, 0, 0, w, h, src, 0, 0, SRCCOPY);
    }
    bool tipShown = false;
    BitmapPtr backBuffer;

    // m:ss:ff · frame n · fps of the frame on screen.
    std::wstring ReadoutText() const { return tframes.empty() ? Clock(Now()) : tframes.Readout(FrameNow()); }

    void PaintReadout(HDC dc, gp::Graphics& g) {
        const RECT r = ReadoutRect();
        const std::wstring text = ReadoutText();
        const int w = Measure(dc, fMonoBig, text).cx;
        Text(dc, fMonoBig, text, {r.left + S(2), r.top, r.left + S(2) + w + S(4), r.bottom}, theme::kText);
        Hotspot({r.left, r.top, r.left + w + S(8), r.bottom}, [this] { BeginGoTo(); }, L"Go to a frame or time (Ctrl+G)");
        int x = r.left + w + S(16);
        if (reviewDir || (playing && previewRate != 1)) {  // the preview's own speed, while it plays that way
            const std::wstring state = (reviewDir < 0 ? L"◀ Reverse " : L"▶ Preview ") + SpeedLabel(previewRate);
            const SIZE sz = Measure(dc, fSmall, state);
            const RECT pill{x, r.top + S(2), x + sz.cx + S(16), r.bottom - S(2)};
            FillRR(g, pill, (float)S(5), A(theme::kSelected));
            Text(dc, fSmall, state, pill, theme::kAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x = pill.right + S(8);
        }
        Text(dc, fSmall, L"←/→ frame · Shift+←/→ second · J/K/L review · Home/End trim ends · Ctrl+G go to", {x, r.top, r.right, r.bottom},
             theme::kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ---- go to ----

    void BeginGoTo() {
        Pause();
        if (selected) Select(std::nullopt);
        goingTo = true;
        fieldsFor = 0;  // the field is emptied for it
        FocusField();
        Invalidate();
    }
    void EndGoTo() {
        if (!goingTo) return;
        goingTo = false;
        if (field1 && GetFocus() == field1) SetFocus(hwnd);
        SyncFields();
        Invalidate();
    }
    // Enter in the go-to field: lands on that frame, or says it isn't one.
    bool CommitGoTo() {
        const int n = field1 ? GetWindowTextLengthW(field1) : 0;
        std::wstring t(n, L'\0');
        if (n) GetWindowTextW(field1, t.data(), n + 1);
        const auto frame = tframes.Find(t);
        if (!frame) {
            ShowToast(L"Not a frame or time in this video", L"Type a frame number (757), a timecode (0:12:37) or seconds (12.6).", nullptr, nullptr, 3000);
            return false;
        }
        EndGoTo();
        GoToFrame(*frame);
        return true;
    }

    // ---- mouse ----

    bool OnHot(POINT p) {
        for (auto it = hots.rbegin(); it != hots.rend(); ++it)
            if (PtInRect(&it->r, p)) {
                auto fn = it->click;  // the click may repaint and rebuild `hots`
                if (fn) fn();
                Invalidate();
                return true;
            }
        return false;
    }

    void OnMouseDown(POINT p, bool dbl, bool shift = false) {
        SetFocus(hwnd);
        if (OnHot(p)) return;
        const RECT tr = TimelineRect();
        const RECT st = StageRect();
        const RECT br = BinRect();
        if (BinH() && PtInRect(&br, p)) return BinDown(p, dbl);
        if (PtInRect(&tr, p) || (p.y >= tr.top - S(6) && p.x >= tr.left && p.x <= tr.right)) return TimelineDown(p, dbl, shift);
        if (PtInRect(&st, p)) return StageDown(p, dbl);
    }

    // An overlay under `pt` on the video, topmost first: a corner of the selected box (1 top-left, 2 top-right,
    // 3 bottom-left, 4 bottom-right: to size it), or a box or a split's half (0: to move or select it).
    std::optional<std::pair<size_t, int>> OverlayAt(POINT pt) const {
        const VPoint vp = ToVideo(pt);
        const double slop = 12 * s / std::max(0.01, ViewScale()), t = Now();
        if (const auto oi = SelOverlay(); oi && edit.overlays[*oi].Active(t) && !edit.overlays[*oi].IsSplit()) {
            const VRect r = OverlayOnVideo(edit.overlays[*oi]);
            const VPoint corners[] = {{r.x, r.y}, {r.MaxX(), r.y}, {r.x, r.MaxY()}, {r.MaxX(), r.MaxY()}};
            for (int k = 0; k < 4; ++k)
                if (std::hypot(corners[k].x - vp.x, corners[k].y - vp.y) <= slop) return std::pair{*oi, k + 1};
        }
        for (size_t i = edit.overlays.size(); i-- > 0;)
            if (edit.overlays[i].Active(t) && OverlayOnVideo(edit.overlays[i]).Contains(vp)) return std::pair{i, 0};
        return std::nullopt;
    }

    void StageDown(POINT pt, bool dbl) {
        const VPoint vp = ToVideo(pt);
        const double slop = 8 * s / std::max(0.01, ViewScale());
        SetCapture(hwnd);
        // Overlay videos are drawn over the markup, so they're picked first; cropping still goes on around them (and
        // over one that isn't selected).
        if (const auto hit = OverlayAt(pt); hit && (!cropping || SelOverlay() == hit->first)) {
            const Overlay& o = edit.overlays[hit->first];
            cropping = false;
            Select(o.id);
            if (o.IsSplit()) {
                ReleaseCapture();
                return;
            }
            PushUndo();
            drag = {};
            drag.kind = DragKind::OverlayBox;
            drag.id = o.id;
            drag.overlay = o;
            drag.handle = hit->second;
            drag.from = vp;
            return;
        }
        if (cropping) {
            PushUndo();
            drag = {};
            drag.kind = DragKind::Crop;
            drag.from = vp;
            return;
        }
        if (auto ni = SelNote()) {  // a note selected: the click pins the spot it points at
            const Note& n = edit.notes[*ni];
            if (const auto f = NoteFrame(n); f && tframes.frames[*f].clip < edit.clips.size()) {
                ReleaseCapture();
                const Clip& c = edit.clips[tframes.frames[*f].clip];
                const VRect r = ClipFit(c);
                const VPoint pin = FlipPin({std::clamp((vp.x - r.x) / std::max(1.0, r.w), 0.0, 1.0), std::clamp((vp.y - r.y) / std::max(1.0, r.h), 0.0, 1.0)}, c);
                UpdateNote([pin](Note& x) { x.pin = pin; });
                if (FrameNow() != *f) GoToFrame(*f);
                return;
            }
        }
        const double t = Now();
        if (auto mi = SelMark(); mi && edit.marks[*mi].Active(t)) {  // handles of the selected mark first
            const Mark& m = edit.marks[*mi];
            const auto hs = Handles(m);
            for (int i = 0; i < (int)hs.size(); ++i)
                if (std::hypot(hs[i].x - vp.x, hs[i].y - vp.y) <= slop) {
                    PushUndo();
                    drag = {};
                    drag.kind = DragKind::Handle;
                    drag.id = m.id;
                    drag.handle = i;
                    drag.mark = m;
                    return;
                }
        }
        const Mark* hit = nullptr;
        for (auto it = edit.marks.rbegin(); it != edit.marks.rend() && !hit; ++it)
            if (it->Active(t) && it->kind != MarkKind::Title && Hit(*it, vp, slop)) hit = &*it;
        for (auto it = edit.marks.rbegin(); it != edit.marks.rend() && !hit; ++it)
            if (it->Active(t) && it->kind == MarkKind::Title) hit = &*it;
        if (hit) {
            const Mark m = *hit;
            Select(m.id);
            if (dbl && KindHasText(m.kind)) return FocusField();
            PushUndo();
            drag = {};
            drag.kind = DragKind::Move;
            drag.id = m.id;
            drag.from = vp;
            drag.mark = m;
            return;
        }
        for (const auto& c : edit.captions)
            if (c.Active(t) && CaptionRect(c).Contains(vp)) {
                Select(c.id);
                if (dbl) return FocusField();
                PushUndo();
                drag = {};
                drag.kind = DragKind::Caption;
                drag.id = c.id;
                drag.from = vp;
                drag.rect = CaptionRect(c);
                return;
            }
        ReleaseCapture();
        if (magZoom > 1) {  // magnified: a click doesn't play; a double-click fits the video again
            if (dbl) FitMagnifier();
            else if (selected) Select(std::nullopt);
            return;
        }
        if (selected) Select(std::nullopt);
        else TogglePlay();
    }

    void TimelineDown(POINT p, bool dbl, bool shift = false) {
        const RECT tr = TimelineRect();
        const int top = tr.top + LaneH();
        const int capY = top + S(62), markY = top + S(90), rowH = S(18);
        SetCapture(hwnd);
        if (shift) return BeginMark(p, std::nullopt);  // Shift+drag anywhere on the timeline marks a range
        if (marked) {
            const double d0 = std::abs(p.x - TX(marked->start)), d1 = std::abs(p.x - TX(marked->end));
            if (p.y >= top && p.y < capY && std::min(d0, d1) < S(6)) return BeginMark(p, d0 <= d1 ? marked->end : marked->start);  // its grips
            if (const double t = TT(p.x); t < marked->start || t >= marked->end) {  // a plain click outside it clears it
                marked.reset();
                Invalidate();
            }
        }
        // The playhead knob sits over the clip lane: it scrubs, whatever is under it.
        const bool onKnob = std::abs(p.x - TX(Now())) <= S(7) && p.y < tr.top + S(8);
        if (LaneH() && p.y < top && !onKnob) {  // the clip lane: grab the selected clip's edge to trim, or a clip to move
            auto begin = [&](size_t i, DragKind kind) {
                drag = {};
                drag.kind = kind;
                drag.clip = i;
                drag.downX = p.x;
                drag.downY = p.y;
                drag.grab = TT(p.x);
                drag.value = kind == DragKind::ClipIn ? edit.clips[i].in : edit.clips[i].out;
            };
            if (const auto k = SelClipIndex()) {  // its edges win over the neighbour's body
                const double t0 = ClipStart(edit.clips, *k), t1 = t0 + edit.clips[*k].Duration();
                const double d0 = std::abs(p.x - TX(t0)), d1 = std::abs(p.x - TX(t1));
                if (std::min(d0, d1) < S(6)) {
                    begin(*k, d0 <= d1 ? DragKind::ClipIn : DragKind::ClipOut);
                    Pause();
                    return;
                }
            }
            if (const auto spot = LocateClip(edit.clips, TT(p.x))) {
                begin(spot->first, DragKind::ClipMove);
                SelectClip(edit.clips[spot->first].id);
                return;
            }
            ReleaseCapture();
            SelectClip(std::nullopt);
            return;
        }
        if (p.y >= top && p.y <= top + S(14))  // a note's flag at the top of the strip
            for (auto it = placed.rbegin(); it != placed.rend(); ++it)
                if (std::abs(p.x - TX(tframes.frames[it->first].t)) <= S(6)) {
                    ReleaseCapture();
                    SelectNote(edit.notes[it->second].id);
                    return;
                }
        auto grab = [&](uint64_t id, bool isCaption, double s0, double e0) {
            drag = {};
            drag.kind = DragKind::Item;
            drag.id = id;
            drag.isCaption = isCaption;
            drag.edge = std::abs(p.x - TX(s0)) < S(6) ? -1 : std::abs(p.x - TX(e0)) < S(6) ? 1 : 0;
            drag.grab = TT(p.x);
            drag.s0 = s0;
            drag.e0 = e0;
            PushUndo();
            if (dbl) {
                Seek(s0);
                FocusField();
            }
        };
        if (const RECT ol = OverlayLaneRect(); p.y >= ol.top - S(2)) {  // the overlay lane: move a bar, or drag its ends to trim it
            for (auto it = edit.overlays.rbegin(); it != edit.overlays.rend(); ++it) {
                const double s0 = it->start, e0 = it->End();
                if (p.x < TX(s0) - S(4) || p.x > std::max(TX(s0) + S(6), TX(e0)) + S(4)) continue;
                Select(it->id);
                drag = {};
                drag.kind = DragKind::OverlayItem;
                drag.id = it->id;
                drag.overlay = *it;
                drag.edge = std::abs(p.x - TX(s0)) < S(6) ? -1 : std::abs(p.x - TX(e0)) < S(6) ? 1 : 0;
                drag.grab = TT(p.x);
                PushUndo();
                if (dbl) Seek(s0);
                return;
            }
            Select(std::nullopt);
            BeginPan(p);
            return;
        }
        if (p.y >= markY) {
            const auto rows = Rows();
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                const int x0 = (int)TX(it->first.start), x1 = x0 + std::max(S(6), (int)TX(it->first.end) - x0);
                const int y0 = markY + it->second * rowH;
                if (p.x >= x0 - S(4) && p.x <= x1 + S(4) && p.y >= y0 && p.y < y0 + rowH) {
                    Select(it->first.id);
                    grab(it->first.id, false, it->first.start, it->first.end);
                    return;
                }
            }
            Select(std::nullopt);
            BeginPan(p);
            return;
        }
        if (p.y >= capY) {
            for (auto it = edit.captions.rbegin(); it != edit.captions.rend(); ++it)
                if (TX(it->start) - S(4) <= p.x && p.x <= TX(it->end) + S(4)) {
                    Select(it->id);
                    grab(it->id, true, it->start, it->end);
                    return;
                }
            Select(std::nullopt);
            BeginPan(p);
            return;
        }
        drag = {};
        if (std::abs(p.x - TX(edit.trimStart)) < S(8)) {
            PushUndo();
            drag.kind = DragKind::TrimStart;
            return;
        }
        if (std::abs(p.x - TX(edit.trimEnd)) < S(8)) {
            PushUndo();
            drag.kind = DragKind::TrimEnd;
            return;
        }
        drag.kind = DragKind::Playhead;
        Pause();
        Seek(SnapToFrame(TT(p.x)));
    }
    // The bin lane: a click selects a piece, a drag carries it onto the timeline, a double-click puts it back at the end.
    void BinDown(POINT p, bool dbl) {
        const auto chips = BinChips();
        for (size_t i = 0; i < chips.size(); ++i)
            if (PtInRect(&chips[i], p)) {
                if (dbl) return PlaceFromBin(i, edit.clips.size());
                SetCapture(hwnd);
                drag = {};
                drag.kind = DragKind::BinMove;
                drag.clip = i;
                drag.downX = p.x;
                drag.downY = p.y;
                drag.at = p;
                SelectClip(edit.bin[i].id);
                return;
            }
        SelectClip(std::nullopt);
    }

    // An empty lane: a drag pans the timeline (zoomed in), a click goes to that time.
    void BeginPan(POINT p) {
        drag = {};
        drag.kind = DragKind::Pan;
        drag.downX = p.x;
        drag.grab = tlStart;
        drag.value = SnapToFrame(TT(p.x));
    }

    void OnMouseMove(POINT p, WPARAM keys) {
        if (magDrag && (keys & MK_RBUTTON)) {
            PanMagnifier(p.x - magDrag->x, p.y - magDrag->y);
            magDrag = p;
            return;
        }
        if (drag.kind == DragKind::None || !(keys & MK_LBUTTON)) {
            RECT hover{};
            for (auto it = hots.rbegin(); it != hots.rend(); ++it)
                if (PtInRect(&it->r, p)) {
                    hover = it->r;
                    break;
                }
            if (!EqualRect(&hover, &hoverRect)) {
                hoverRect = hover;
                tipShown = false;
                KillTimer(hwnd, 2);
                if (!IsRectEmpty(&hover)) SetTimer(hwnd, 2, 600, nullptr);
                Invalidate();
            }
            return;
        }
        const double now = TT(p.x);
        const VPoint vp = ToVideo(p);
        switch (drag.kind) {
            case DragKind::TrimStart:
                SetTrim(now, std::nullopt);
                Seek(edit.trimStart);
                break;
            case DragKind::TrimEnd:
                SetTrim(std::nullopt, now);
                Seek(edit.trimEnd);
                break;
            case DragKind::Playhead: Seek(SnapToFrame(now)); break;
            case DragKind::Range: {
                const double t = SnapEdge(std::clamp(now, edit.trimStart, edit.trimEnd));
                SetMarked(drag.grab, t);
                // Shows the frame at the edge that moves: the range's first frame, or its last.
                if (marked && !tframes.empty()) GoToFrame(t < drag.grab ? tframes.At(marked->start + 1e-4) : tframes.At(marked->end - 1e-4));
                Invalidate();
                break;
            }
            case DragKind::Pan:
                if (drag.target < 0 && std::abs(p.x - drag.downX) < S(4)) break;
                drag.target = 1;  // moved: a pan, not a click
                tlStart = drag.grab - (p.x - drag.downX) * TlSpan() / std::max(1, RectW(TimelineRect()));
                ClampTimeline();
                Invalidate();
                break;
            case DragKind::ClipIn:
            case DragKind::ClipOut: {
                if (drag.clip >= edit.clips.size()) break;
                const Clip& c = edit.clips[drag.clip];
                const double v = (drag.kind == DragKind::ClipIn ? c.in : c.out) + (now - drag.grab);
                const double shortest = std::min(0.2, c.Duration());
                drag.value = drag.kind == DragKind::ClipIn ? std::clamp(v, 0.0, c.out - shortest)
                                                           : std::clamp(v, c.in + shortest, std::max(c.length, c.in + shortest));
                const double t0 = ClipStart(edit.clips, drag.clip);
                if (drag.value >= c.in && drag.value <= c.out) Seek(t0 + drag.value - c.in);  // shows the frame at the cut
                Invalidate();
                break;
            }
            case DragKind::ClipMove: {
                if (drag.clip >= edit.clips.size() || (!drag.moved && std::abs(p.x - drag.downX) < S(5) && std::abs(p.y - drag.downY) < S(5))) break;
                drag.moved = true;
                drag.at = p;
                drag.toBin = p.y < BinRect().bottom + S(4);  // up over the bin
                drag.target = drag.toBin ? -1 : InsertIndex(p.x, drag.clip);
                Invalidate();
                break;
            }
            case DragKind::BinMove: {
                if (drag.clip >= edit.bin.size() || (!drag.moved && std::abs(p.x - drag.downX) < S(5) && std::abs(p.y - drag.downY) < S(5))) break;
                drag.moved = true;
                drag.at = p;
                const RECT tr = TimelineRect(), ol = OverlayLaneRect();
                const bool overClips = p.y >= BinRect().bottom + S(4) && p.y <= tr.top + LaneH() + S(52);  // the clip lane or the strip under it
                drag.target = overClips ? InsertIndex(p.x, SIZE_MAX) : -1;
                drag.toOverlay = p.y >= ol.top - S(6) && p.y <= ol.bottom + S(10);
                drag.value = SnapToFrame(TT(p.x));
                Invalidate();
                break;
            }
            case DragKind::OverlayItem: {
                const Overlay& o0 = drag.overlay;
                const double len = o0.clip.Duration(), most = std::max(o0.clip.length, o0.clip.out) - o0.clip.in;  // footage after its in point
                const double shortest = std::min(0.2, len);  // a piece already shorter than that keeps its length
                for (auto& o : edit.overlays) {
                    if (o.id != drag.id) continue;
                    if (drag.edge == -1) {  // its start and in point move together
                        const double d = std::clamp(now - o0.start, std::max(-o0.clip.in, -o0.start), len - shortest);
                        o.start = o0.start + d;
                        o.clip.in = o0.clip.in + d;
                        Seek(o.start);
                    } else if (drag.edge == 1) {
                        const double end = std::clamp(now, o0.start + shortest, o0.start + most);
                        o.clip.out = o0.clip.in + (end - o0.start);
                        Seek(end - 0.01);
                    } else {
                        o.start = std::clamp(o0.start + now - drag.grab, 0.0, std::max(0.0, duration - 0.1));
                        Seek(o.start);
                    }
                }
                Changed();
                break;
            }
            case DragKind::OverlayBox: {  // picture in picture: the box moves, or (its corner) grows and shrinks
                const VRect v = ViewRect();
                const SIZE out{(LONG)v.w, (LONG)v.h};
                const double dx = vp.x - drag.from.x, dy = vp.y - drag.from.y;
                for (auto& o : edit.overlays) {
                    if (o.id != drag.id) continue;
                    if (drag.handle == 0) {
                        o.pos = {drag.overlay.pos.x + dx / v.w, drag.overlay.pos.y + dy / v.h};
                    } else {  // a corner: the opposite one stays put, the shape stays the video's
                        const VRect r0 = OverlayRect(drag.overlay, out);
                        const bool left = drag.handle == 1 || drag.handle == 3, top = drag.handle <= 2;
                        const double ax = left ? r0.MaxX() : r0.x, ay = top ? r0.MaxY() : r0.y;  // the anchor, in output pixels
                        const double shape = r0.h / std::max(1.0, r0.w);
                        const double px = vp.x - v.x, py = vp.y - v.y;
                        double w = std::max(left ? ax - px : px - ax, (top ? ay - py : py - ay) / shape);
                        w = std::max(0.08 * v.w, std::min(w, std::min(left ? ax : v.w - ax, (top ? ay : v.h - ay) / shape)));  // stays on the frame
                        o.width = w / v.w;
                        o.pos = {(left ? ax - w : ax) / v.w, (top ? ay - w * shape : ay) / v.h};
                    }
                    const VRect r = OverlayRect(o, out);  // kept on the frame
                    o.pos = {r.x / v.w, r.y / v.h};
                }
                Changed();
                break;
            }
            case DragKind::Item: {
                double st = drag.s0, en = drag.e0;
                if (drag.edge == -1) st = std::min(now, drag.e0 - 0.2);
                else if (drag.edge == 1) en = std::max(now, drag.s0 + 0.2);
                else {
                    const double len = drag.e0 - drag.s0;
                    st = std::min(std::max(0.0, drag.s0 + now - drag.grab), duration - len);
                    en = st + len;
                }
                for (auto& c : edit.captions)
                    if (drag.isCaption && c.id == drag.id) c.start = st, c.end = en;
                for (auto& m : edit.marks)
                    if (!drag.isCaption && m.id == drag.id) m.start = st, m.end = en;
                Seek(drag.edge == 1 ? en - 0.01 : st);
                Changed();
                break;
            }
            case DragKind::Crop: {
                VPoint b = vp;
                if (aspect) {  // keep the chosen shape
                    const double k = kAspectValues[aspect];
                    const double w = std::fabs(b.x - drag.from.x), h = std::fabs(b.y - drag.from.y);
                    const double W = std::max(w, h * k), H = W / k;
                    b = {drag.from.x + (b.x < drag.from.x ? -W : W), drag.from.y + (b.y < drag.from.y ? -H : H)};
                }
                SetCrop(NormRect(drag.from, b));
                Changed();
                break;
            }
            case DragKind::Caption: {
                const VRect v = ViewRect();
                const VPoint mid{drag.rect.MidX() + vp.x - drag.from.x, drag.rect.MidY() + vp.y - drag.from.y};
                for (auto& c : edit.captions)
                    if (c.id == drag.id) c.center = VPoint{std::clamp((mid.x - v.x) / v.w, 0.0, 1.0), std::clamp((mid.y - v.y) / v.h, 0.0, 1.0)};
                Changed();
                break;
            }
            case DragKind::Move: {
                if (drag.mark.kind == MarkKind::Title) break;
                const double dx = vp.x - drag.from.x, dy = vp.y - drag.from.y;
                for (auto& m : edit.marks)
                    if (m.id == drag.id) {
                        m.a = {drag.mark.a.x + dx, drag.mark.a.y + dy};
                        m.b = {drag.mark.b.x + dx, drag.mark.b.y + dy};
                    }
                Changed();
                break;
            }
            case DragKind::Handle: {
                const Mark& o = drag.mark;
                for (auto& m : edit.marks) {
                    if (m.id != drag.id) continue;
                    if (KindIsLine(o.kind)) {
                        (drag.handle == 0 ? m.a : m.b) = vp;
                    } else {
                        const VRect r = o.Rect();
                        const VPoint anchor{drag.handle % 2 == 0 ? r.MaxX() : r.x, drag.handle < 2 ? r.MaxY() : r.y};
                        VPoint q = vp;
                        if (o.kind == MarkKind::Emoji || o.kind == MarkKind::Step || (keys & MK_SHIFT)) {  // keep the shape
                            const double k = r.w / std::max(1.0, r.h);
                            const double w = std::max(std::fabs(q.x - anchor.x), std::fabs(q.y - anchor.y) * k);
                            q = {anchor.x + (q.x < anchor.x ? -w : w), anchor.y + (q.y < anchor.y ? -w / k : w / k)};
                        }
                        m.a = anchor;
                        m.b = q;
                    }
                }
                Changed();
                break;
            }
            default: break;
        }
    }

    // The wheel: scrolls the notes list; over the video it magnifies; over the timeline, Ctrl+wheel zooms it and the
    // wheel pans it.
    void OnWheel(POINT p, int delta, WPARAM keys) {
        const RECT st = StageRect();
        if (PtInRect(&st, p)) {
            if (const auto hit = OverlayAt(p); hit && !edit.overlays[hit->first].IsSplit())  // over a box: it grows or shrinks
                return ScaleOverlay(hit->first, std::pow(1.1, delta / (double)WHEEL_DELTA));
            Magnify(std::pow(1.25, delta / (double)WHEEL_DELTA), p);
            return;
        }
        const RECT nl = NotesListRect();
        if (NotesShown() && PtInRect(&nl, p)) {
            notesScroll = std::max(0, notesScroll - delta * NoteRowH() / WHEEL_DELTA);
            Invalidate();
            return;
        }
        const RECT tr = TimelineRect();
        if (p.y >= tr.top - S(8) && p.y <= tr.bottom && p.x >= S(14)) {
            if (keys & MK_CONTROL) ZoomTimeline(std::pow(1.25, delta / (double)WHEEL_DELTA));
            else if (tlZoom > 1) {
                tlStart -= delta / (double)WHEEL_DELTA * TlSpan() * 0.15;
                ClampTimeline();
                Invalidate();
            }
            return;
        }
    }

    void OnMouseUp() {

        const Drag d = drag;
        if (d.kind == DragKind::Pan) {
            drag = {};
            ReleaseCapture();
            if (d.target < 0) Seek(d.value);  // a click: go there
            Invalidate();
            return;
        }
        if (d.kind == DragKind::ClipIn || d.kind == DragKind::ClipOut || d.kind == DragKind::ClipMove || d.kind == DragKind::BinMove) {
            drag = {};
            ReleaseCapture();
            if (d.kind == DragKind::ClipIn) TrimClip(d.clip, d.value, std::nullopt);
            else if (d.kind == DragKind::ClipOut) TrimClip(d.clip, std::nullopt, d.value);
            else if (d.kind == DragKind::BinMove) {
                if (d.toOverlay) BinToOverlay(d.clip, d.value);
                else if (d.target >= 0) PlaceFromBin(d.clip, (size_t)d.target);
            } else if (d.toBin) MoveToBin(d.clip);
            else if (d.target >= 0) MoveClip(d.clip, (size_t)d.target);
            Invalidate();
            return;
        }
        if (drag.kind == DragKind::Item)
            std::stable_sort(edit.captions.begin(), edit.captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
        drag = {};
        ReleaseCapture();
        Invalidate();
    }

    // ---- keys ----

    // Ends a drag without applying it (Esc, or the mouse capture taken away). Live item drags keep what they did so
    // far (it's one undo step); clip drags only apply on release, so nothing changes.
    void CancelDrag() {
        if (drag.kind == DragKind::None) return;
        if (drag.kind == DragKind::Range) marked = drag.before;
        drag = {};
        if (GetCapture() == hwnd) ReleaseCapture();
        Invalidate();
    }

    struct Mods {
        bool ctrl = false, shift = false, alt = false;
    };
    bool OnKey(WPARAM vk) { return Key(vk, {GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_MENU) < 0}); }

    bool Key(WPARAM vk, Mods mods) {
        if (drag.kind != DragKind::None) {
            if (vk == VK_ESCAPE) CancelDrag();
            return true;
        }
        if (mods.ctrl && mods.alt && !mods.shift && vk == 'S') {
            SaveReview();
            return true;
        }
        const bool ctrl = mods.ctrl && !mods.alt, shift = mods.shift;
        if (ctrl) {
            switch (vk) {
                case 'S': Save(shift); return true;
                case 'C': shift ? CopyFrame() : Copy(); return true;
                case 'O': AddClipDialog(); return true;
                case 'Z': Undo(); return true;
                case 'G': BeginGoTo(); return true;
                case 'L': ToggleLoop(); return true;
                case VK_OEM_PLUS:
                case VK_ADD: ZoomTimeline(1.5); return true;
                case VK_OEM_MINUS:
                case VK_SUBTRACT: ZoomTimeline(1 / 1.5); return true;
                case '0':
                case VK_NUMPAD0: FitTrim(); return true;

                case 'W': PostMessageW(hwnd, WM_CLOSE, 0, 0); return true;
                default: return false;
            }
        }
        switch (vk) {
            case VK_SPACE: TogglePlay(); break;
            case 'J': Review(-1); break;
            case 'K': Pause(); break;
            case 'L': Review(1); break;

            case VK_LEFT: shift ? StepSecond(-1) : Step(-1); break;
            case VK_RIGHT: shift ? StepSecond(1) : Step(1); break;
            case VK_HOME: GoTrimEnd(false); break;
            case VK_END: GoTrimEnd(true); break;
            case 'S': SplitAtPlayhead(); break;
            case 'I': shift ? MarkAtPlayhead(true) : TrimAtPlayhead(true); break;  // Shift: the marked range, not the trim
            case 'O': shift ? MarkAtPlayhead(false) : TrimAtPlayhead(false); break;
            case 'T': AddCaption(); break;
            case 'A': AddMark(MarkKind::Arrow); break;
            case 'R': AddMark(MarkKind::Box); break;
            case 'E': AddMark(MarkKind::Emoji); break;
            case 'N': AddMark(MarkKind::Step); break;
            case 'X': AddMark(MarkKind::Blur); break;
            case 'Z': AddMark(MarkKind::Zoom); break;
            case 'C': cropping = !cropping; Invalidate(); break;
            case 'M': AddNote(); break;
            case 'F': FitMagnifier(); break;
            case 'G': ToggleGuides(); break;
            case VK_OEM_4: JumpNote(-1); break;  // [
            case VK_OEM_6: JumpNote(1); break;   // ]
            case VK_DELETE:
            case VK_BACK:
                if (SelClipIndex() && !selected) MoveToBin(*SelClipIndex());
                else if (SelBinIndex() && !selected) DeleteFromBin(*SelBinIndex());
                else DeleteSelected();
                break;
            case VK_ESCAPE:
                if (marked) marked.reset();
                else if (cropping) cropping = false;
                else if (selected) Select(std::nullopt);
                else if (selClip) SelectClip(std::nullopt);
                else PostMessageW(hwnd, WM_CLOSE, 0, 0);
                Invalidate();
                break;
            default: return false;
        }
        return true;
    }

    // ---- long jobs ----

    void AutoCaptions() {
        if (busy) return;
        busy = true;
        Invalidate();
        ShowToast(L"Transcribing…", L"Turning speech into captions on this PC", nullptr, nullptr, 120000);
        const Sequence sq = Seq();
        transcribedClips = edit.clips;
        const double a = edit.trimStart, b = edit.trimEnd;
        HWND h = hwnd;
        std::thread([sq, a, b, h] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* r = new TranscribeResult{};
            r->ok = Transcribe(sq, a, b, &r->caps, &r->err);
            CoUninitialize();
            if (!PostMessageW(h, WM_TRANSCRIBED, 0, (LPARAM)r)) delete r;
        }).detach();
    }

    void Transcribed(bool ok, std::vector<Caption> caps, const std::wstring& err) {
        busy = false;
        HideToast();
        if (!ok) {
            ShowToast(L"Auto captions failed", err, nullptr, nullptr, 8000);
        } else if (caps.empty()) {
            ShowToast(L"No speech found", L"Add captions by hand with T.", nullptr, nullptr, 3000);
        } else {
            if (edit.clips != transcribedClips) {  // the clips changed while it ran: move the captions with their footage
                VideoEdit then;
                then.clips = transcribedClips;
                then.trimEnd = ClipsDuration(transcribedClips);
                then.captions = std::move(caps);
                ApplyClips(then, edit.clips);
                caps = std::move(then.captions);
            }
            PushUndo();
            std::erase_if(edit.captions, [](const Caption& c) { return c.text.empty(); });
            edit.captions.insert(edit.captions.end(), caps.begin(), caps.end());
            std::stable_sort(edit.captions.begin(), edit.captions.end(), [](const Caption& x, const Caption& y) { return x.start < y.start; });
            ShowToast(L"Added " + std::to_wstring(caps.size()) + L" caption" + (caps.size() == 1 ? L"" : L"s"), L"Click one on the timeline to edit or move it.",
                      nullptr, nullptr, 3000);
        }
        Changed();
    }

    void Save(bool gif) {
        if (busy) return;
        SetFocus(hwnd);  // commits a field being edited
        busy = saving = true;
        Pause();
        const std::wstring out = MakeCapturePath(g_folder, gif ? L"gif" : L"mp4", {window, app});
        SHCreateDirectoryExW(nullptr, out.substr(0, out.find_last_of(L'\\')).c_str(), nullptr);
        // Written in the temp folder (the encoder picks the container from the extension), then moved in.
        wchar_t tdir[MAX_PATH];
        GetTempPathW(MAX_PATH, tdir);
        const std::wstring tmp = std::wstring(tdir) + L"ather-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
                                 (gif ? L".gif" : L".mp4");
        const uint64_t toast = ShowToast(gif ? L"Saving GIF…" : L"Saving video…", Clock(edit.OutputDuration()) + L" long", nullptr, nullptr, 600000);
        const VideoEdit e = edit;
        const std::wstring src = path, srcApp = app, srcWindow = window;
        HWND h = hwnd;
        std::thread([=] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring err;
            int lastPct = -1;
            auto progress = [&](double p) {
                const int pct = (int)(p * 100);
                if (pct / 5 != lastPct / 5) {
                    lastPct = pct;
                    const std::wstring body = std::to_wstring(pct) + L"%";
                    RunOnUi([toast, body] { UpdateToastBody(toast, body); });
                }
                return true;
            };
            const bool ok = gif ? ExportGif(src, e, tmp, &err, 12, progress) : ExportMp4(src, e, tmp, &err, progress);
            CoUninitialize();
            auto* r = new SaveResult{ok, gif, out, tmp, err};
            if (!PostMessageW(h, WM_SAVED, 0, (LPARAM)r)) {  // the editor was closed meanwhile
                delete r;
                if (!ok) {
                    DeleteFileW(tmp.c_str());
                } else {
                    RunOnUi([src, out, tmp, srcApp, srcWindow] {
                        Library::Shared().NoteEdit(out, src, srcApp, srcWindow, true);
                        MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
                    });
                }
            }
        }).detach();
    }

    // Save ▾ › Save smaller: the edit as a new MP4 in the captures folder that fits in `mb` (compress.h: H.264 at the rate
    // that fits its length, scaled down when that's low, made again smaller when it comes out over).
    double lastSmallerMB = 0;  // tests
    void SaveSmaller(double mb) {
        if (busy || mb <= 0) return;
        SetFocus(hwnd);  // commits a field being edited
        busy = saving = true;
        Pause();
        RememberCompressSize(false, mb);
        lastSmallerMB = mb;
        const std::wstring out = MakeCapturePath(g_folder, L"mp4", {window, app});
        SHCreateDirectoryExW(nullptr, out.substr(0, out.find_last_of(L'\\')).c_str(), nullptr);
        const std::wstring label = MegabytesLabel(mb);
        const uint64_t toast = ShowToast(L"Saving under " + label + L"…", Clock(edit.OutputDuration()) + L" long", nullptr, nullptr, 600000);
        const VideoEdit e = edit;
        const std::wstring src = path, srcApp = app, srcWindow = window;
        HWND h = hwnd;
        std::thread([=] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            int lastPct = -1;
            CompressResult c = CompressEdit(src, e, MegabytesToBytes(mb), [&](double p, int attempt) {
                const int pct = (int)(p * 100);
                if (pct / 5 == lastPct / 5) return;
                lastPct = pct;
                const std::wstring body = std::to_wstring(pct) + L"%" + (attempt ? L", smaller try " + std::to_wstring(attempt + 1) : L"");
                RunOnUi([toast, body] { UpdateToastBody(toast, body); });
            });
            CoUninitialize();
            auto* r = new SaveResult{c.ok, false, out, c.tmp, c.error};
            r->underMB = mb;
            if (!PostMessageW(h, WM_SAVED, 0, (LPARAM)r)) {  // the editor was closed meanwhile
                delete r;
                if (!c.ok) {
                    DeleteFileW(c.tmp.c_str());
                } else {
                    const std::wstring tmp = c.tmp;
                    RunOnUi([src, out, tmp, srcApp, srcWindow] {
                        Library::Shared().NoteEdit(out, src, srcApp, srcWindow, true);
                        MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
                    });
                }
            }
        }).detach();
    }
    void AskSmallerSize() {
        if (busy) return;
        wchar_t last[32];
        swprintf_s(last, L"%g", CompressSizesFor({path}).front());
        HWND h = hwnd;
        ShowTextPrompt(L"Save smaller: under how many MB?  ·  Enter to save, Esc to cancel", last, [this, h](std::wstring text) {
            if (!IsWindow(h)) return;  // the editor was closed meanwhile
            const double mb = ParseMegabytes(text);
            if (mb <= 0) return (void)ShowToast(L"Type a size in MB", L"For example 10 or 2.5", nullptr, nullptr, 3000);
            SaveSmaller(mb);
        });
    }

    // Save review video (Ctrl+Alt+S): "<video> review.mp4" in the captures folder (the summary card, the edit with the
    // timecode and frame number burned in, each note's frame held under its card), with "<video> review notes.md" and
    // "<video> review sheet.png" next to it.
    std::wstring lastReviewOut;  // tests
    void SaveReview() {
        if (busy || tframes.empty()) return;
        SetFocus(hwnd);  // commits a field being edited
        EndGoTo();
        busy = saving = true;
        Pause();
        const ReviewPlan plan = MakeReviewPlan();
        const std::wstring capture = MakeCapturePath(g_folder, L"mp4", {window, app});
        const std::wstring folder = capture.substr(0, capture.find_last_of(L'\\'));
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
        std::wstring stem = FileNameOf(path);
        if (const size_t dot = stem.find_last_of(L'.'); dot != std::wstring::npos) stem.resize(dot);
        std::wstring name;
        for (int k = 1;; ++k) {  // never over an earlier one
            name = folder + L"\\" + stem + L" review" + (k > 1 ? L" " + std::to_wstring(k) : L"");
            bool taken = false;
            for (const wchar_t* ext : {L".mp4", L" notes.md", L" sheet.png"}) taken = taken || GetFileAttributesW((name + ext).c_str()) != INVALID_FILE_ATTRIBUTES;
            if (!taken) break;
        }
        const std::wstring out = name + L".mp4", md = name + L" notes.md", sheet = name + L" sheet.png";
        wchar_t tdir[MAX_PATH];
        GetTempPathW(MAX_PATH, tdir);
        const std::wstring tmp = std::wstring(tdir) + L"ather-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".mp4";
        const uint64_t toast = ShowToast(L"Saving review video…", std::to_wstring(plan.notes.size()) + (plan.notes.size() == 1 ? L" note" : L" notes"), nullptr,
                                         nullptr, 600000);
        const VideoEdit e = edit;
        const std::wstring src = path, mdText = NotesListText(plan);
        HWND h = hwnd;
        std::thread([=] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring err;
            int lastPct = -1;
            auto progress = [&](double p) {
                const int pct = (int)(p * 100);
                if (pct / 5 != lastPct / 5) {
                    lastPct = pct;
                    const std::wstring body = std::to_wstring(pct) + L"%";
                    RunOnUi([toast, body] { UpdateToastBody(toast, body); });
                }
                return true;
            };
            const bool ok = ExportReviewMp4(src, e, plan, tmp, &err, progress);
            const bool sheetOk = ok && WriteContactSheet(src, e, plan, sheet);
            CoUninitialize();
            auto* r = new SaveResult{ok, false, out, tmp, err, true, md, mdText, sheetOk ? sheet : L""};
            if (!PostMessageW(h, WM_SAVED, 0, (LPARAM)r)) {  // the editor was closed meanwhile
                delete r;
                if (!ok) DeleteFileW(tmp.c_str());
                else RunOnUi([out, tmp, md, mdText] {
                    MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
                    WriteFileUtf8(md, ToUtf8(mdText));
                });
            }
        }).detach();
    }
    void ReviewSaved(const SaveResult& r) {
        busy = saving = false;
        lastSaveError = r.ok ? L"" : r.err;
        Invalidate();
        if (!r.ok || !MoveFileExW(r.tmp.c_str(), r.out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            DeleteFileW(r.tmp.c_str());
            ShowToast(L"Saving the review video failed", r.ok ? r.out : r.err, nullptr, nullptr, 8000);
            return;
        }
        lastReviewOut = r.out;
        Library::Shared().NoteEdit(r.out, path, app, window, true);  // stacks with the original in the gallery
        WriteFileUtf8(r.md, ToUtf8(r.mdText));
        std::vector<std::wstring> made = {r.out, r.md};
        if (!r.sheet.empty()) made.push_back(r.sheet);
        ShowToast(L"Review video saved", FileNameOf(r.out) + L", its notes list and contact sheet  ·  click to show in Explorer", nullptr,
                  [made] { RevealInExplorer(made); }, 6000);
    }

    // `underMB` > 0: a "Save smaller" one.
    void Saved(bool ok, bool gif, const std::wstring& out, const std::wstring& tmp, const std::wstring& err, double underMB = 0) {
        busy = saving = false;
        lastSaveError = ok ? L"" : err;
        Invalidate();
        if (!ok) {
            DeleteFileW(tmp.c_str());
            ShowToast(underMB > 0 ? L"Couldn't save under " + MegabytesLabel(underMB) : L"Save failed", err, nullptr, nullptr, 8000);
            return;
        }
        // Joins the gallery stacked with the original, with its tags and collections, tagged "edited".
        Library::Shared().NoteEdit(out, path, app, window, true);
        if (!MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            DeleteFileW(tmp.c_str());
            ShowToast(L"Save failed", out, nullptr, nullptr, 8000);
            return;
        }
        dirty = false;
        savedEdit = WithoutNotes(edit);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa);
        const double mb = ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) / 1048576.0;
        wchar_t size[32];
        swprintf_s(size, mb >= 1 ? L"%.1f MB" : L"%.0f KB", mb >= 1 ? mb : mb * 1024);
        ShowToast(underMB > 0 ? L"Saved under " + MegabytesLabel(underMB) : gif ? L"GIF saved" : L"Video saved",
                  FileNameOf(out) + L"  ·  " + size + L"  ·  click to show in Explorer", nullptr,
                  [out] { RevealInExplorer({out}); }, 5000);
    }

    // ---- copy (Ctrl+C) ----

    // Ctrl+C: the marked range as a clip; with no range, the clip selected in the clip lane (as edited, inside the trim);
    // else the frame on screen.
    bool CanCopyClip() const { return marked || (SelClipIndex() && !selected); }
    void Copy() {
        if (marked) return CopyClip(marked->start, marked->end);
        if (const auto k = SelClipIndex(); k && !selected) {
            const double t0 = ClipStart(edit.clips, *k);
            const double a = std::max(t0, edit.trimStart), b = std::min(t0 + edit.clips[*k].Duration(), edit.trimEnd);
            if (b - a < 0.5 / Seq().Fps()) {
                ShowToast(L"This clip is outside the trim", L"Nothing of it would be saved. Mark a range with Shift+drag instead.", nullptr, nullptr, 3000);
                return;
            }
            return CopyClip(a, b);
        }
        CopyFrame();
    }

    // Copy clip: the edit over [a, b) saved as Save would write that part (crop, speed, mute, captions and markup; H.264
    // + AAC) into the captures folder, stacked with the original in the gallery, then the file put on the clipboard, so
    // Ctrl+V in Slack, Teams or Explorer attaches it.
    std::wstring lastCopiedClip;  // tests
    bool lastClipOnClipboard = false;
    void CopyClip(double a, double b) {
        if (busy) return;
        SetFocus(hwnd);  // commits a field being edited
        busy = saving = true;
        Pause();
        VideoEdit e = edit;
        e.trimStart = a;
        e.trimEnd = b - 1e-4;  // the frame starting right at `b` stays out, whatever the rounding of frame times
        const double seconds = (b - a) / e.speed;
        const std::wstring out = MakeCapturePath(g_folder, L"mp4", {window, app});
        SHCreateDirectoryExW(nullptr, out.substr(0, out.find_last_of(L'\\')).c_str(), nullptr);
        wchar_t tdir[MAX_PATH];
        GetTempPathW(MAX_PATH, tdir);
        const std::wstring tmp = std::wstring(tdir) + L"ather-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".mp4";
        const uint64_t toast = ShowToast(L"Copying clip…", ClipSeconds(seconds) + L" long", nullptr, nullptr, 600000);
        const std::wstring src = path, srcApp = app, srcWindow = window;
        HWND h = hwnd;
        std::thread([=] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            std::wstring err;
            int lastPct = -1;
            const bool ok = ExportMp4(src, e, tmp, &err, [&](double p) {
                const int pct = (int)(p * 100);
                if (pct / 5 != lastPct / 5) {
                    lastPct = pct;
                    const std::wstring body = std::to_wstring(pct) + L"%";
                    RunOnUi([toast, body] { UpdateToastBody(toast, body); });
                }
                return true;
            });
            CoUninitialize();
            auto* r = new SaveResult{ok, false, out, tmp, err};
            r->copy = true;
            r->seconds = seconds;
            if (!PostMessageW(h, WM_SAVED, 0, (LPARAM)r)) {  // the editor was closed meanwhile: saved and copied all the same
                const SaveResult res = *r;
                delete r;
                RunOnUi([res, src, srcApp, srcWindow] { PlaceCopiedClip(res, nullptr, src, srcApp, srcWindow); });
            }
        }).detach();
    }
    static std::wstring ClipSeconds(double secs) {
        wchar_t b[32];
        swprintf_s(b, L"%.1f s", secs);
        return secs < 60 ? std::wstring(b) : Clock(secs);
    }
    // Moves a copied clip into the captures folder, puts it on the clipboard and says so. False when that failed.
    static bool PlaceCopiedClip(const SaveResult& r, HWND owner, const std::wstring& src, const std::wstring& srcApp, const std::wstring& srcWindow,
                                bool* onClipboard = nullptr) {
        if (!r.ok) {
            DeleteFileW(r.tmp.c_str());
            ShowToast(L"Copy clip failed", r.err, nullptr, nullptr, 8000);
            return false;
        }
        Library::Shared().NoteEdit(r.out, src, srcApp, srcWindow, true);  // stacks with the original in the gallery
        if (!MoveFileExW(r.tmp.c_str(), r.out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            DeleteFileW(r.tmp.c_str());
            ShowToast(L"Copy clip failed", r.out, nullptr, nullptr, 8000);
            return false;
        }
        const bool copied = CopyFileToClipboard(owner, r.out);
        if (onClipboard) *onClipboard = copied;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(r.out.c_str(), GetFileExInfoStandard, &fa);
        const double mb = ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) / 1048576.0;
        wchar_t size[32];
        swprintf_s(size, mb >= 1 ? L"%.1f MB" : L"%.0f KB", mb >= 1 ? mb : mb * 1024);
        const std::wstring out = r.out;
        if (copied)
            ShowToast(L"Clip copied · " + ClipSeconds(r.seconds) + L" · " + size, L"Paste it in Slack or anywhere  ·  click to show in Explorer", nullptr,
                      [out] { RevealInExplorer({out}); }, 6000);
        else
            ShowToast(L"Clip saved, but the clipboard was busy", FileNameOf(out) + L"  ·  click to show it in Explorer and drag it in instead", nullptr,
                      [out] { RevealInExplorer({out}); }, 8000);
        return true;
    }
    void ClipCopied(const SaveResult& r) {
        busy = saving = false;
        lastSaveError = r.ok ? L"" : r.err;
        Invalidate();
        if (PlaceCopiedClip(r, hwnd, path, app, window, &lastClipOnClipboard)) lastCopiedClip = r.out;
    }

    // Copy frame (Ctrl+Shift+C, or Ctrl+C with nothing picked): the frame on screen as the export draws it (crop,
    // captions, markup and videos on top at that time), at the export's full size, as a picture on the clipboard (CF_DIB
    // and PNG). Not saved to a file. Right after a step it waits for that frame (and its overlays' pictures) to be
    // decoded, so it's the one the readout names.
    bool frameCopyPending = false;
    BitmapPtr lastCopiedFrame;  // tests
    void CopyFrame() {
        if (!raw) return;
        if (!playing && !tframes.empty() && tframes.Nearest(rawT) != tframes.Nearest(paused)) {
            frameCopyPending = true;  // WM_FETCHED comes back here
            return;
        }
        const double t = playing ? rawT : paused;
        const bool overlayShows = std::any_of(edit.overlays.begin(), edit.overlays.end(), [t](const Overlay& o) { return o.Active(t); });
        if (!playing && overlayShows && overlayPicsT != rawT) {
            frameCopyPending = true;  // the overlays' pictures for this frame: WM_OVERLAYS comes back here
            return;
        }
        frameCopyPending = false;
        FrameRenderer r(edit, videoSize, false);
        r.overlayFrame = OverlayPictures();
        const BitmapPtr f = r.Render(*raw, t);
        if (!f) return;
        if (!CopyImageToClipboard(hwnd, *f, true)) {
            ShowToast(L"Couldn't copy the frame", L"The clipboard is busy. Try again.", nullptr, nullptr, 3000);
            return;
        }
        lastCopiedFrame = f;
        const std::wstring at = tframes.empty() ? Clock(Now()) : BurnInText(tframes, FrameNow());
        ShowToast(L"Frame copied", at + L"  ·  " + std::to_wstring(f->Width()) + L" × " + std::to_wstring(f->Height()), f, nullptr, 2500);
    }

    // ---- window ----

    void Fonts() {
        for (HFONT f : {fUi, fSmall, fIcon, fIconSmall, fMono, fEmoji, fMonoBig})
            if (f) DeleteObject(f);
        fUi = MakeFont(S(13));
        fSmall = MakeFont(S(11));
        fIcon = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kIconFace);
        fIconSmall = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kIconFace);
        fMono = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
        fMonoBig = CreateFontW(-S(13), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
        fEmoji = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Emoji");
        for (HWND f : {field1, field2})
            if (f) SendMessageW(f, WM_SETFONT, (WPARAM)fUi, TRUE);
    }

    bool Create();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
};

VideoEditor* FromHwnd(HWND h) {
    for (auto* e : g_editors)
        if (e->hwnd == h) return e;
    return nullptr;
}

LRESULT CALLBACK FieldProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    VideoEditor* e = FromHwnd(GetParent(h));
    if (!e) return DefWindowProcW(h, m, w, l);
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
        if (e->goingTo && w == VK_RETURN) e->CommitGoTo();
        else if (e->goingTo) e->EndGoTo();
        else SetFocus(e->hwnd);
        return 0;
    }
    if (m == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && (w == 'S' || w == 'W')) return e->OnKey(w), 0;
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
    if (m == WM_SETFOCUS && !e->goingTo) e->PushUndo();  // one undo step per editing session
    if (m == WM_KILLFOCUS && e->goingTo) {  // clicked away: going to nowhere
        const LRESULT r = CallWindowProcW(e->editProc, h, m, w, l);
        e->EndGoTo();
        return r;
    }
    return CallWindowProcW(e->editProc, h, m, w, l);

}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* e = reinterpret_cast<VideoEditor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        e->hwnd = h;
    }
    if (VideoEditor* e = FromHwnd(h)) return e->Proc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT VideoEditor::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: Invalidate(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = {S(980), S(600)};
            return 0;
        }
        case WM_DPICHANGED: {
            s = HIWORD(w) / 96.f;
            Fonts();
            const RECT* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (w == kTimerFrame) Tick();
            if (w == 2) {
                KillTimer(hwnd, 2);
                tipShown = true;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: OnMouseDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, m == WM_LBUTTONDBLCLK, (w & MK_SHIFT) != 0); return 0;
        case WM_MOUSEMOVE: OnMouseMove({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, w); return 0;
        case WM_LBUTTONUP: OnMouseUp(); return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)l != hwnd) CancelDrag();  // Alt+Tab, a menu, another window took the mouse mid-drag
            return 0;
        case WM_MOUSELEAVE: hoverRect = {}; tipShown = false; Invalidate(); return 0;
        case WM_RBUTTONDOWN: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            const RECT st = StageRect();
            rDown = p;
            if (PtInRect(&st, p) && magZoom > 1) {
                magDrag = p;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (magDrag) {
                magDrag.reset();
                ReleaseCapture();
            }
            // A right click on the video (not a magnifier pan): the guides menu there.
            const RECT st = StageRect();
            if (PtInRect(&st, p) && std::abs(p.x - rDown.x) + std::abs(p.y - rDown.y) <= S(4)) {
                POINT sp = p;
                ClientToScreen(hwnd, &sp);
                RunMenu(hwnd, GuidesMenu(), sp);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {

            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &p);
            OnWheel(p, GET_WHEEL_DELTA_WPARAM(w), GET_KEYSTATE_WPARAM(w));
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (OnKey(w)) return 0;
            break;
        case WM_COMMAND:
            if (HIWORD(w) == EN_CHANGE && (LOWORD(w) == kField1 || LOWORD(w) == kField2)) FieldChanged((HWND)l);
            return 0;
        case WM_CTLCOLOREDIT: {
            static HBRUSH bg = CreateSolidBrush(theme::kBgRaised);
            SetTextColor((HDC)w, theme::kText);
            SetBkColor((HDC)w, theme::kBgRaised);
            return (LRESULT)bg;
        }
        case WM_ENGINE:
            if (w == MF_MEDIA_ENGINE_EVENT_ERROR) {
                ShowToast(L"Can't play this video", L"Saving still works.", nullptr, nullptr, 4000);
            }
            return 0;
        case WM_FETCHED: {
            std::unique_ptr<FrameFetcher::Result> r(reinterpret_cast<FrameFetcher::Result*>(l));
            if (!playing) {
                raw = r->frame;
                rawT = r->t;
                RequestOverlays(r->t);
                Rerender();
                Invalidate();
                if (frameCopyPending) CopyFrame();
                if (onFetched) onFetched();
            }
            return 0;
        }
        case WM_OVERLAYS: {
            std::unique_ptr<OverlayFetcher::Result> r(reinterpret_cast<OverlayFetcher::Result*>(l));
            overlayPics.clear();
            for (auto& [id, b] : r->pictures) overlayPics[id] = b;
            overlayPicsT = r->t;
            Rerender();
            Invalidate();
            if (frameCopyPending) CopyFrame();
            if (onOverlays) onOverlays();
            return 0;
        }
        case WM_STRIP: {
            std::unique_ptr<StripFetcher::Result> r(reinterpret_cast<StripFetcher::Result*>(l));
            if (r->gen != stripGen) return 0;  // for frames since rebuilt
            stripThumbs[r->frame] = r->thumb;
            if (stripThumbs.size() > 800) {  // keep the ones on view, drop the rest
                std::map<size_t, BitmapPtr> keep;
                for (const auto& t : StripTiles())
                    if (auto it = stripThumbs.find(t.frame); it != stripThumbs.end()) keep.insert(*it);
                stripThumbs = std::move(keep);
            }
            Invalidate();
            return 0;
        }
        case WM_FRAMES: {
            std::unique_ptr<FrameTimesResult> r(reinterpret_cast<FrameTimesResult*>(l));
            framesPending.erase(r->key);
            frameTimes[r->key] = r->times;
            RebuildFrames();
            if (!playing) Fetch(paused);
            Invalidate();
            return 0;
        }
        case WM_THUMBS: {
            std::unique_ptr<std::vector<BitmapPtr>> t(reinterpret_cast<std::vector<BitmapPtr>*>(l));
            if (w != (WPARAM)thumbGen) return 0;  // for clips that have changed since
            thumbs = std::move(*t);
            Invalidate();
            return 0;
        }
        case WM_DROPFILES: {  // videos dropped on the editor join the sequence; on the bottom lane or a box, they go on top
            HDROP drop = (HDROP)w;
            const std::vector<std::wstring> files = DroppedFiles(drop);
            POINT at{};
            DragQueryPoint(drop, &at);
            DragFinish(drop);
            ForceForeground(hwnd);
            DropFiles(files, at);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && drag.kind == DragKind::None) {  // over a box on the video: move, or size from a corner
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                const RECT st = StageRect();
                if (const auto hit = PtInRect(&st, p) && !cropping ? OverlayAt(p) : std::nullopt; hit && !edit.overlays[hit->first].IsSplit()) {
                    const int k = hit->second;
                    SetCursor(LoadCursorW(nullptr, k == 0 ? IDC_SIZEALL : k == 1 || k == 4 ? IDC_SIZENWSE : IDC_SIZENESW));
                    return TRUE;
                }
            }
            break;
        case WM_TRANSCRIBED: {
            std::unique_ptr<TranscribeResult> r(reinterpret_cast<TranscribeResult*>(l));
            Transcribed(r->ok, std::move(r->caps), r->err);
            return 0;
        }
        case WM_SAVED: {
            std::unique_ptr<SaveResult> r(reinterpret_cast<SaveResult*>(l));
            if (r->review) ReviewSaved(*r);
            else if (r->copy) ClipCopied(*r);
            else Saved(r->ok, r->gif, r->out, r->tmp, r->err, r->underMB);
            return 0;
        }
        case WM_CLOSE:
            if (dirty && !snapshotMode && !(WithoutNotes(edit) == savedEdit) &&
                MessageBoxW(hwnd, L"Close the video editor and discard your changes?", L"Edit video", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerFrame);
            thumbLatest->store(~0ull);  // stops a thumbnail job still running
            player.reset();
            fetcher.reset();
            stripFetcher.reset();
            return 0;

        case WM_NCDESTROY: {  // after the children: they still need FieldProc to find this editor
            std::erase(g_editors, this);
            if (g_editors.empty()) ClearRenderCache();  // rendered titles and captions can be large
            for (HFONT f : {fUi, fSmall, fIcon, fIconSmall, fMono, fEmoji, fMonoBig})
                if (f) DeleteObject(f);
            delete this;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

bool VideoEditor::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = VideoProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = g_icon;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    VideoInfo vi;
    BitmapPtr first;
    const auto clip = ClipOf(path);
    if (!clip || !ProbeVideo(path, &vi, 0, 0, &first) || vi.w <= 0) return false;
    videoSize = {vi.w, vi.h};
    raw = first;
    edit.clips = {*clip};
    edit.frameW = vi.w;
    edit.frameH = vi.h;
    builtClips = edit.clips;
    duration = ClipsDuration(edit.clips);
    edit.trimEnd = duration;
    LoadNotes();
    savedEdit = WithoutNotes(edit);

    const ItemMeta& meta = Library::Shared().Meta(path);
    app = meta.app;
    window = meta.window;

    POINT pt;
    GetCursorPos(&pt);
    s = DpiScaleAt(pt);
    Fonts();
    const RECT work = MonitorRectAt(pt, true);
    const int w = std::min(S(1240), (int)(RectW(work) * 0.92)), h = std::min(S(880), (int)(RectH(work) * 0.92));
    const std::wstring title = L"Edit video — " + FileNameOf(path);
    if (!CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - h) / 2, w, h,
                         nullptr, nullptr, GetModuleHandleW(nullptr), this))
        return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    for (int id : {kField1, kField2}) {
        HWND f = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(f, WM_SETFONT, (WPARAM)fUi, TRUE);
        SendMessageW(f, EM_SETLIMITTEXT, 500, 0);
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(f, GWLP_WNDPROC, (LONG_PTR)FieldProc);
        if (!editProc) editProc = old;
        (id == kField1 ? field1 : field2) = f;
    }
    std::wstring err;
    player = SequencePlayer::Open(Seq(), hwnd, WM_ENGINE, &err);
    fetcher = std::make_unique<FrameFetcher>(Seq(), hwnd);
    RebuildFrames();
    Rerender();
    SetTimer(hwnd, kTimerFrame, 15, nullptr);
    LoadThumbs();
    LoadFrameTimes();

    DragAcceptFiles(hwnd, TRUE);
    if (snapshotMode) return true;
    ShowWindow(hwnd, SW_SHOW);
    ForceForeground(hwnd);
    SetFocus(hwnd);
    return true;
}

}  // namespace

// ---------- API ----------

void SetVideoEditorOptions(const std::wstring& capturesFolder, HICON icon) {
    g_folder = capturesFolder;
    g_icon = icon;
}

bool VideoEditorHotkey(UINT mods, UINT vk) {
    if ((mods & (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) != (MOD_CONTROL | MOD_ALT) || vk != 'S') return false;
    const HWND fg = GetForegroundWindow();
    for (auto* e : g_editors)
        if (e->hwnd == fg) {
            e->SaveReview();
            return true;
        }
    return false;
}

void SetVideoEditorAuthor(const std::wstring& author, std::function<void(const std::wstring&)> remember) {

    g_noteAuthor = author;
    g_rememberAuthor = std::move(remember);
}


bool IsVideoFile(const std::wstring& path) { return MediaTypeOf(path) == MediaType::Video; }  // one list, in library.cpp

int VideoEditorCount() { return (int)g_editors.size(); }

bool VideoEditorsBusy() {
    return std::any_of(g_editors.begin(), g_editors.end(), [](const VideoEditor* e) { return e->saving; });
}

bool OpenVideoEditor(const std::wstring& path) {
    for (auto* e : g_editors)
        if (_wcsicmp(e->path.c_str(), path.c_str()) == 0) {
            if (IsIconic(e->hwnd)) ShowWindow(e->hwnd, SW_RESTORE);
            ForceForeground(e->hwnd);
            return true;
        }
    auto* e = new VideoEditor(path);
    g_editors.push_back(e);
    if (!e->Create()) {
        if (e->hwnd) {
            DestroyWindow(e->hwnd);  // WM_DESTROY deletes it
        } else {
            std::erase(g_editors, e);
            delete e;
        }
        ShowToast(L"Can't open this video", FileNameOf(path), nullptr, nullptr, 4000);
        return false;
    }
    return true;
}

// ---------- snapshots and tests ----------

namespace {

// Saves go to a test folder for the scope; restored even when a test returns early.
struct CapturesFolderForTest {
    std::wstring old = g_folder;
    explicit CapturesFolderForTest(const std::wstring& f) { g_folder = f; }
    ~CapturesFolderForTest() { g_folder = old; }
};

void Pump(int ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

VideoEditor* OpenHidden(const std::wstring& clip, int w, int h) {
    auto* e = new VideoEditor(clip);
    e->snapshotMode = true;
    g_editors.push_back(e);
    if (!e->Create()) {
        if (e->hwnd) {
            DestroyWindow(e->hwnd);
        } else {
            std::erase(g_editors, e);
            delete e;
        }
        return nullptr;
    }
    RECT wr{0, 0, w, h};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(e->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    return e;
}

// A clip whose every frame has a color of its own: frame i is (16 × (i % 16), 16 × (i / 16), `blue`), which survives
// compression well enough for NumberOf to read it back.
// For the snapshots: a recording-like clip whose every frame shows its number in big type (and its timecode), over a
// fine grid, with a bar that moves 8 px a frame, and a tone. Frame i at i / fps.
bool WriteFrameNumberClip(const std::wstring& path, int w, int h, int fps, int frames) {
    Mp4Writer mw;
    if (FAILED(mw.Begin(path, w, h, fps, 48000, 2))) return false;
    auto f = Bitmap::Create(w, h);
    if (!f) return false;
    textdraw::Style big, sub;
    big.family = L"Segoe UI";
    big.size = h * 0.3f;
    big.weight = 800;
    sub.family = L"Consolas";
    sub.size = h * 0.05f;
    sub.weight = 700;
    sub.color = RGB(255, 214, 10);
    int64_t audio = 0;
    std::vector<int16_t> pcm;
    for (int i = 0; i < frames; ++i) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool grid = x % 40 == 0 || y % 40 == 0;
                const uint32_t bg = 0xFF000000u | (uint32_t)(20 + 40 * x / w) << 16 | (uint32_t)(30 + 30 * y / h) << 8 | (uint32_t)(70 + 60 * x / w);
                f->Bits()[(size_t)y * w + x] = grid ? 0xFF5A6A8Au : bg;
            }
        const int bx = (i * 8) % w;
        for (int y = 0; y < h; ++y)
            for (int x = bx; x < std::min(w, bx + 24); ++x) f->Bits()[(size_t)y * w + x] = 0xFFE5484Du;
        textdraw::Draw(*f, std::to_wstring(i), big, 0, h * 0.28f, (float)w);
        textdraw::Draw(*f, L"frame " + std::to_wstring(i) + L" · " + Timecode((double)i / fps, fps), sub, 0, h * 0.68f, (float)w);
        if (FAILED(mw.WriteFrame(f->Bits(), std::llround(i * 1e7 / fps), std::llround(1e7 / fps)))) return false;
        const int64_t until = std::llround((i + 1) * 48000.0 / fps);
        pcm.clear();
        for (; audio < until; ++audio) {
            const int16_t v = (int16_t)std::lround(std::sin(2 * 3.14159265358979 * 330 * audio / 48000) * 6000);
            pcm.push_back(v), pcm.push_back(v);
        }
        mw.WriteAudio(pcm.data(), (uint32_t)(pcm.size() / 2), std::llround((audio - (int64_t)pcm.size() / 2) * 1e7 / 48000));
    }
    return SUCCEEDED(mw.Finalize());
}

// `tone`: with a 440 Hz tone, so an export of it has sound.
bool WriteNumberedClip(const std::wstring& path, int w, int h, int fps, int frames, int blue, bool tone = false) {
    Mp4Writer mw;
    if (FAILED(mw.Begin(path, w, h, fps, tone ? 48000 : 0, 2))) return false;
    std::vector<uint32_t> px((size_t)w * h);
    int64_t audio = 0;
    std::vector<int16_t> pcm;
    for (int i = 0; i < frames; ++i) {
        std::fill(px.begin(), px.end(), 0xFF000000u | (uint32_t)(16 * (i % 16)) << 16 | (uint32_t)(16 * (i / 16)) << 8 | (uint32_t)blue);
        if (FAILED(mw.WriteFrame(px.data(), std::llround(i * 1e7 / fps), std::llround(1e7 / fps)))) return false;
        if (!tone) continue;
        pcm.clear();
        const int64_t from = audio, until = std::llround((i + 1) * 48000.0 / fps);
        for (; audio < until; ++audio) {
            const int16_t v = (int16_t)std::lround(std::sin(2 * 3.14159265358979 * 440 * audio / 48000) * 6000);
            pcm.push_back(v), pcm.push_back(v);
        }
        mw.WriteAudio(pcm.data(), (uint32_t)(audio - from), std::llround(from * 1e7 / 48000));
    }
    return SUCCEEDED(mw.Finalize());
}
int NumberOf(uint32_t c) { return (int)std::lround(((c >> 8) & 255) / 16.0) * 16 + (int)std::lround(((c >> 16) & 255) / 16.0); }
bool BlueOf(uint32_t c) { return (c & 255) > 128; }

// The frame the paused editor shows once the frame at the playhead has been decoded: its number and whether it has
// blue (which clip), from the middle pixel. {-1, false} when it doesn't arrive.
std::pair<int, bool> ShownFrame(VideoEditor* e) {
    for (const ULONGLONG end = GetTickCount64() + 2000; GetTickCount64() < end;) {
        if (e->raw && std::fabs(e->rawT - e->paused) < 1e-3) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
    if (!e->raw || std::fabs(e->rawT - e->paused) >= 1e-3) return {-1, false};
    const uint32_t c = e->raw->Bits()[(size_t)(e->raw->Height() / 2) * e->raw->Width() + e->raw->Width() / 2];
    return {NumberOf(c), BlueOf(c)};
}

// A menu as Windows draws it: opened for real (as a click on its button opens it) and printed once it's up.
BitmapPtr g_menuShot;
void CALLBACK PrintMenu(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    RECT r{};
    if (HWND w = FindWindowW(L"#32768", nullptr); w && GetWindowRect(w, &r) && RectW(r) > 0 && RectH(r) > 0 && (g_menuShot = Bitmap::Create(RectW(r), RectH(r)))) {
        MemDC dc(g_menuShot->Handle());
        if (!PrintWindow(w, dc, PW_RENDERFULLCONTENT)) {
            HDC screen = GetDC(nullptr);
            BitBlt(dc, 0, 0, RectW(r), RectH(r), screen, r.left, r.top, SRCCOPY);
            ReleaseDC(nullptr, screen);
        }
    }
    EndMenu();
}
BitmapPtr MenuShot(HWND owner, const std::vector<MenuItem>& items) {
    std::vector<const MenuItem*> flat;
    HMENU m = BuildMenu(items, flat);
    g_menuShot.reset();
    const UINT_PTR timer = SetTimer(nullptr, 0, 500, PrintMenu);
    TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, 200, 200, 0, owner, nullptr);
    KillTimer(nullptr, timer);
    DestroyMenu(m);
    return std::exchange(g_menuShot, nullptr);
}

BitmapPtr Snapshot(VideoEditor* e) {
    RECT rc;
    GetClientRect(e->hwnd, &rc);
    auto out = Bitmap::Create(RectW(rc), RectH(rc));
    {
        MemDC dc(out->Handle());
        e->Paint(dc);
        e->Paint(dc);  // the first paint places the fields
        for (HWND f : {e->field1, e->field2}) {
            if (!(GetWindowLongW(f, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT er;
            GetWindowRect(f, &er);
            MapWindowPoints(nullptr, e->hwnd, (POINT*)&er, 2);
            POINT old;
            SetViewportOrgEx(dc, er.left, er.top, &old);
            SendMessageW(f, WM_PRINT, (WPARAM)(HDC)dc, PRF_CLIENT | PRF_ERASEBKGND);
            SetViewportOrgEx(dc, old.x, old.y, nullptr);
        }
    }
    for (size_t i = 0, n = (size_t)out->Width() * out->Height(); i < n; ++i) out->Bits()[i] |= 0xFF000000u;
    return out;
}

}  // namespace

int VideoEditorSnapshots(const std::wstring& outDir) {
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);  // as the other snapshot tools do (CI hands it a new folder)
    const std::wstring clip = outDir + L"\\snapshot-clip.mp4";
    if (!WriteTestClip(clip, 1280, 720, 30, 3, false)) return 1;  // silent: snapshots never make a sound
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    if (!e) return 2;
    Pump(1500);  // thumbnails
    VideoEdit& ed = e->edit;
    ed.trimStart = 0.4;
    ed.trimEnd = 2.6;
    ed.crop = VRect{120, 60, 960, 600};
    std::vector<CaptionWord> words;
    const wchar_t* ws[] = {L"Open", L"Settings,", L"then", L"click", L"Deploy"};
    for (int i = 0; i < 5; ++i) words.push_back({0.5 + i * 0.22, 0.72 + i * 0.22, ws[i]});
    Caption c1;
    c1.start = 0.5;
    c1.end = 1.6;
    c1.text = L"Open Settings, then click Deploy";
    c1.words = words;
    Caption c2;
    c2.start = 1.8;
    c2.end = 2.4;
    c2.text = L"Done";
    ed.captions = {c1, c2};
    ed.captionLook = CaptionLook::Outline;
    auto mk = [](MarkKind k, double s0, double e0, VPoint a, VPoint b, std::wstring text, int color, int level, int step = 1) {
        Mark m;
        m.kind = k;
        m.start = s0;
        m.end = e0;
        m.a = a;
        m.b = b;
        m.text = std::move(text);
        m.color = color;
        m.level = level;
        m.step = step;
        return m;
    };
    ed.marks = {mk(MarkKind::Arrow, 0.6, 2.0, {280, 500}, {500, 320}, L"", 6, 2),
                mk(MarkKind::Emoji, 0.6, 2.0, {840, 140}, {980, 280}, L"\U0001F389", 0, 2),
                mk(MarkKind::Bubble, 0.8, 2.2, {520, 120}, {820, 220}, L"Click here", 6, 1),
                mk(MarkKind::Step, 0.5, 2.5, {200, 160}, {260, 220}, L"", 0, 2, 1),
                mk(MarkKind::Blur, 0.5, 2.5, {600, 400}, {840, 500}, L"", 0, 2),
                mk(MarkKind::Zoom, 1.8, 2.6, {400, 200}, {720, 400}, L"", 0, 2)};
    e->Seek(1.0);
    e->Changed();
    Pump(800);
    SavePng(*Snapshot(e), outDir + L"\\video-editor.png");
    {  // the magnifier at 4×, over the speech bubble's text
        const gp::PointF at = e->ToView(VPoint{640, 165});

        e->Magnify(4, {(LONG)at.X, (LONG)at.Y});
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-magnifier.png");
        e->FitMagnifier();
    }
    ed.captions[0].center = VPoint{0.5, 0.62};
    ed.captionLook = CaptionLook::Pill;
    ed.captionColor = 2;
    ed.captionEdge = 4;
    e->Select(ed.captions[0].id);
    e->Changed();
    Pump(300);
    SavePng(*Snapshot(e), outDir + L"\\video-editor-caption.png");
    e->Select(ed.marks[2].id);
    e->Changed();
    Pump(300);
    SavePng(*Snapshot(e), outDir + L"\\video-editor-bubble.png");
    e->cropping = true;
    e->Select(std::nullopt);
    Pump(100);
    SavePng(*Snapshot(e), outDir + L"\\video-editor-crop.png");
    e->cropping = false;
    // Joined clips: a square second video (fitted with black bars), then the first one split in two.
    const std::wstring square = outDir + L"\\snapshot-square.mp4";
    if (WriteTestClip(square, 720, 720, 30, 2, false)) {
        e->AddClips({square});
        e->Seek(2.0);
        e->SplitAtPlayhead();
        e->SelectClip(e->edit.clips[1].id);
        e->Seek(3.6);
        for (int i = 0; i < 50 && e->thumbs.empty(); ++i) Pump(100);
        Pump(300);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-clips.png");
        // The bin: the square video taken out and selected, then carried back down over the clip lane.
        e->MoveToBin(e->edit.clips.size() - 1);
        e->SelectClip(e->edit.bin[0].id);
        Pump(300);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-bin.png");
        const RECT chip = e->BinChips()[0], tr = e->TimelineRect();
        e->OnMouseDown({(chip.left + chip.right) / 2, (chip.top + chip.bottom) / 2}, false);
        e->OnMouseMove({(chip.left + chip.right) / 2 + 40, chip.bottom + 20}, MK_LBUTTON);
        e->OnMouseMove({(int)e->TX(0.5), tr.top + e->LaneH() / 2}, MK_LBUTTON);  // before the first clip
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-bin-drag.png");
        e->CancelDrag();
        // The square video on top: in a box, then side by side.
        if (const auto sq = ClipOf(square)) {
            e->AddOverlay(*sq, 0.5, OverlayLayout::Pip);
            e->Seek(1.4);
            for (int i = 0; i < 60 && e->overlayPics.empty(); ++i) Pump(50);
            Pump(300);
            SavePng(*Snapshot(e), outDir + L"\\video-editor-overlay.png");
            e->UpdateOverlay([](Overlay& o) { o.layout = OverlayLayout::SplitRight; });
            Pump(300);
            SavePng(*Snapshot(e), outDir + L"\\video-editor-split.png");
        }
    }
    e->dirty = false;
    DestroyWindow(e->hwnd);
    // A 60 fps recording on frame 757, as the readout's example.
    const std::wstring sixty = outDir + L"\\snapshot-60fps.mp4";
    if (WriteFrameNumberClip(sixty, 1280, 720, 60, 780) && (e = OpenHidden(sixty, 1180, 760))) {

        for (int i = 0; i < 300 && (!e->FramesKnown() || e->thumbs.empty()); ++i) Pump(10);
        e->GoToFrame(757);
        Pump(800);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-readout.png");
        e->ToggleFlip(true);  // the same frame mirrored, filmstrip too
        Pump(300);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-flip.png");
        e->Undo();
        e->ToggleLoop();  // the Loop button, on
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-loop.png");
        e->ToggleLoop();
        e->ShowAllGuides();  // every composition guide, then the black-and-white view with the thirds
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-guides.png");
        e->guides.fill(false);
        e->ToggleGuide(GuideKind::Thirds);
        e->ToggleMono();
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-mono.png");
        e->ToggleMono();
        e->HideGuides();
        Pump(300);
        // Notes of every kind, one resolved, the selected one pinned.
        struct Spec {
            size_t frame;
            NoteKind kind;
            const wchar_t* text;
            const wchar_t* author;
            bool resolved;
        };
        const Spec specs[] = {{95, NoteKind::Note, L"Intro starts a beat late", L"Tin Nguyen", false},
                              {260, NoteKind::Issue, L"Health bar flickers for one frame when the shield breaks", L"Tin Nguyen", false},
                              {410, NoteKind::Question, L"Is this hit-stop intended?", L"Mai", false},
                              {540, NoteKind::Good, L"Dash trail reads well now", L"Mai", true},
                              {757, NoteKind::Issue, L"Muzzle flash missing on this frame", L"Tin Nguyen", false}};
        for (const Spec& sp : specs) {
            const TimelineFrames::Frame& f = e->tframes.frames[sp.frame];
            Note n;
            n.path = e->edit.clips[f.clip].path;
            n.src = f.src;
            n.kind = sp.kind;
            n.text = sp.text;
            n.author = sp.author;
            n.resolved = sp.resolved;
            e->edit.notes.push_back(n);
        }
        e->edit.notes.back().pin = VPoint{0.62, 0.38};
        e->edit.notes[1].srcEnd = e->edit.notes[1].src + 0.5;
        e->Changed();
        e->SelectNote(e->edit.notes.back().id);
        Pump(800);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-notes.png");
        // Zoomed all the way in around frame 757, with a second note close by: a tick per frame, numbered.
        {
            const TimelineFrames::Frame& f = e->tframes.frames[742];
            Note n;
            n.path = e->edit.clips[f.clip].path;
            n.src = f.src;
            n.kind = NoteKind::Question;
            n.text = L"Recoil starts here?";
            n.author = L"Mai";
            e->edit.notes.push_back(n);
            e->Changed();
        }
        e->ZoomTimeline(1000);
        e->tlStart = e->tframes.frames[757].t - e->TlSpan() * 0.6;
        e->ClampTimeline();
        e->RevealNote(*e->selected);
        Snapshot(e);  // asks for the strip's pictures
        for (int i = 0; i < 300 && !e->StripComplete(); ++i) Pump(10);
        Pump(100);
        SavePng(*Snapshot(e), outDir + L"\\video-editor-timeline-zoom.png");
        {  // the magnifier at 4× over the frame number's edge, the grid and the pin
            const gp::PointF at = e->ToView(VPoint{760, 300});
            e->Magnify(4, {(LONG)at.X, (LONG)at.Y});
            Pump(100);
            SavePng(*Snapshot(e), outDir + L"\\video-editor-magnifier-60fps.png");
            e->FitMagnifier();
        }

        // The review video: its summary card, a note card frame (the pinned Issue at frame 757) and the contact sheet.
        {
            const ReviewPlan plan = e->MakeReviewPlan();
            size_t k = 0;
            while (k + 1 < plan.notes.size() && plan.notes[k].frame != 757) ++k;
            const int intro = (int)std::lround(plan.intro * 60), hold = (int)std::lround(plan.hold * 60);
            const int cardAt = intro + (int)plan.notes[k].frame + (int)k * hold + hold / 2;  // trim from 0, 60 fps: output frame = frame number
            std::mutex mu;
            g_exportTap = [&](int i, const Bitmap& f) {
                if (i != 0 && i != cardAt) return;
                std::lock_guard l(mu);
                SavePng(f, outDir + (i == 0 ? L"\\review-summary-card.png" : L"\\review-note-card.png"));
            };
            std::wstring err;
            ExportReviewMp4(e->path, e->edit, plan, outDir + L"\\snapshot-review.mp4", &err);
            g_exportTap = nullptr;
            WriteContactSheet(e->path, e->edit, plan, outDir + L"\\review-sheet.png");
        }
        e->edit.notes.clear();  // its sidecar goes again
        e->Changed();

        // A marked range of 6.2 s (frames 300 to 671), the playhead on its first frame; then the Save menu with Copy clip
        // and Copy frame, as Windows draws it, pasted under ▾.
        e->FitTrim();
        e->Select(std::nullopt);
        e->GoToFrame(300);
        e->SetMarked(e->tframes.frames[300].t, e->tframes.frames[672].t);
        Pump(800);
        BitmapPtr shot = Snapshot(e);
        SavePng(*shot, outDir + L"\\video-editor-range.png");
        if (const BitmapPtr menu = MenuShot(e->hwnd, e->SaveMenu())) {
            SavePng(*menu, outDir + L"\\video-editor-save-menu-alone.png");
            const int x = shot->Width() - e->S(14) - menu->Width(), y = e->S(8) + e->S(32) + e->S(2);
            {
                MemDC dst(shot->Handle()), src(menu->Handle());
                BitBlt(dst, x, y, menu->Width(), menu->Height(), src, 0, 0, SRCCOPY);
            }
            for (size_t i = 0, n = (size_t)shot->Width() * shot->Height(); i < n; ++i) shot->Bits()[i] |= 0xFF000000u;
            SavePng(*shot, outDir + L"\\video-editor-save-menu.png");
        }

        e->dirty = false;
        DestroyWindow(e->hwnd);
    }
    return 0;
}

// Picking an animation updates the menu right away (it used to keep the old label on macOS) and replays the item.
ATHER_TEST(video_animation_menu_confirms_choice) {
    const std::wstring clip = test::TempDir() + L"\\clip.mp4";
    CHECK(WriteTestClip(clip, 640, 360, 30, 3, false));
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->Seek(1);
    e->AddMark(MarkKind::Box);
    const Mark m = e->edit.marks.back();
    CHECK(e->AnimationTitle(m) == L"Animation: Auto (Draw on)");
    auto items = e->AnimationMenu(m);
    auto find = [](std::vector<MenuItem>& v, const std::wstring& label) -> MenuItem* {
        for (auto& it : v)
            if (it.label == label) return &it;
        return nullptr;
    };
    MenuItem* pop = find(items, L"Pop");
    CHECK(pop != nullptr);
    if (pop) pop->run();
    const Mark& after = e->edit.marks.back();
    CHECK(after.style == AnimStyle::Pop);
    CHECK(e->AnimationTitle(after) == L"Animation: Pop");
    auto again = e->AnimationMenu(after);
    MenuItem* pop2 = find(again, L"Pop");
    CHECK(pop2 && pop2->checked);
    CHECK(e->lastReplay.has_value() && std::fabs(*e->lastReplay - (after.start - 0.4)) < 1e-6);  // replays from just before the item
    // Undo brings the old choice back.
    e->Undo();
    CHECK(e->edit.marks.back().style == AnimStyle::Auto);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Playback goes through the media engine (frames run through the renderer); paused seeks decode the exact frame.
ATHER_TEST(video_editor_plays_and_seeks) {
    const std::wstring clip = test::TempDir() + L"\\clip.mp4";
    CHECK(WriteTestClip(clip, 640, 360, 30, 3, false));
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    auto color = [&] { return e->raw ? e->raw->Bits()[(size_t)180 * e->raw->Width() + 320] & 0xFFFFFF : 0u; };
    CHECK(e->raw != nullptr);  // the first frame shows right away
    CHECK(color() == 0xFF0000 || ((color() >> 16) & 255) > 200);
    e->Play();
    Pump(1600);
    test::Note("rawT after playing " + std::to_string(e->rawT) + ", now " + std::to_string(e->Now()));
    CHECK(e->playing);
    CHECK(e->rawT > 0.8);  // frames arrive from the engine
    CHECK(((color() >> 8) & 255) > 200);  // second 1–2 is green
    e->Pause();
    e->Seek(2.5);
    Pump(1000);
    test::Note("rawT after seek " + std::to_string(e->rawT));
    CHECK_NEAR(e->rawT, 2.5, 0.05);
    CHECK((color() & 255) > 200);  // blue
    // Plays to the trim end, then stops and goes back to the trim start.
    e->edit.trimEnd = 1.0;
    e->Seek(0.5);
    e->Play();
    Pump(1200);
    CHECK(!e->playing);
    CHECK_NEAR(e->Now(), 0, 0.05);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Save writes a new MP4 (or GIF) into the captures folder; the original stays as it was.
ATHER_TEST(video_editor_saves_new_files) {
    const std::wstring dir = test::TempDir();
    const std::wstring clip = dir + L"\\clip.mp4", caps = dir + L"\\caps";
    CHECK(WriteTestClip(clip, 640, 360, 30, 3, true));
    WIN32_FILE_ATTRIBUTE_DATA before{};
    GetFileAttributesExW(clip.c_str(), GetFileExInfoStandard, &before);
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->Seek(1);
    e->OnKey('I');
    e->AddCaption();
    e->edit.captions.back().text = L"Saved from a test";
    for (bool gif : {false, true}) {
        e->Save(gif);
        for (int i = 0; i < 100 && e->busy; ++i) Pump(100);
        CHECK(!e->busy);
        test::Note("save error: " + ToUtf8(e->lastSaveError));
        CHECK(e->lastSaveError.empty());
    }
    CHECK(!e->dirty);
    const auto files = RecentCaptures(caps, 10, true);
    CHECK_EQ(files.size(), 2u);
    bool mp4 = false, gif = false;
    for (const auto& f : files) {
        if (IsVideoFile(f)) {
            mp4 = true;
            VideoInfo vi;
            CHECK(ProbeVideo(f, &vi) && std::fabs(vi.duration - 2) < 0.15);
        }
        if (f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".gif") == 0) gif = true;
    }
    CHECK(mp4 && gif);
    WIN32_FILE_ATTRIBUTE_DATA after{};
    GetFileAttributesExW(clip.c_str(), GetFileExInfoStandard, &after);
    CHECK(CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) == 0);  // the original is untouched
    DestroyWindow(e->hwnd);
}

// Save ▾ › Save smaller: the edit (here trimmed and cropped) as a new MP4 under the size, in the captures folder.
ATHER_TEST(video_editor_saves_smaller) {
    const std::wstring dir = test::TempDir();
    const std::wstring clip = dir + L"\\clip.mp4", caps = dir + L"\\caps";
    CHECK(WriteTestClip(clip, 1280, 720, 30, 3, true));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    // The menu: Save smaller ▸ the sizes, then Other size…
    const auto menu = e->SaveMenu();
    const auto smaller = std::find_if(menu.begin(), menu.end(), [](const MenuItem& m) { return m.label == L"Save smaller"; });
    CHECK(smaller != menu.end() && smaller->sub.size() >= 4 && smaller->sub.back().label == L"Other size…");
    e->Seek(1);
    e->OnKey('I');
    e->edit.crop = VRect{0, 0, 640, 360};
    const double mb = 0.15;  // 2 s in 154 KB: ~470 kbps with the sound, so a 640-wide crop stays as it is
    e->SaveSmaller(mb);
    for (int i = 0; i < 150 && e->busy; ++i) Pump(100);
    CHECK(!e->busy);
    test::Note("save error: " + ToUtf8(e->lastSaveError));
    CHECK(e->lastSaveError.empty());
    auto files = RecentCaptures(caps, 10, true);
    CHECK_EQ(files.size(), 1u);
    if (files.size() == 1) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(files[0].c_str(), GetFileExInfoStandard, &fa);
        CHECK(fa.nFileSizeHigh == 0 && fa.nFileSizeLow <= MegabytesToBytes(mb));
        VideoInfo vi;
        CHECK(ProbeVideo(files[0], &vi) && std::fabs(vi.duration - 2) < 0.15 && vi.w == 640 && vi.h == 360 && vi.hasAudio);
    }
    // A size too small for the length: an error, and nothing saved.
    e->SaveSmaller(0.01);
    for (int i = 0; i < 50 && e->busy; ++i) Pump(100);
    CHECK(!e->busy && !e->lastSaveError.empty());
    CHECK_EQ(RecentCaptures(caps, 10, true).size(), 1u);
    DestroyWindow(e->hwnd);
}

ATHER_TEST(video_editor_trim_keys_and_add_defaults) {
    const std::wstring clip = test::TempDir() + L"\\clip.mp4";
    CHECK(WriteTestClip(clip, 640, 360, 30, 3, false));
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_NEAR(e->edit.trimEnd, 3, 0.05);
    e->Seek(0.5);
    e->OnKey('I');
    CHECK_NEAR(e->edit.trimStart, 0.5, 1e-6);
    e->Seek(2.5);
    e->OnKey('O');
    CHECK_NEAR(e->edit.trimEnd, 2.5, 1e-6);
    e->Seek(1);
    e->OnKey('N');
    e->OnKey('N');
    CHECK_EQ(e->edit.marks.size(), 2u);
    if (e->edit.marks.size() == 2) {
        CHECK_EQ(e->edit.marks[1].step, 2);  // step numbers count up
        CHECK_NEAR(e->edit.marks[1].end - e->edit.marks[1].start, 1.5, 1e-6);  // 3 s, cut at the trim end
    }
    e->OnKey('T');
    CHECK_EQ(e->edit.captions.size(), 1u);
    CHECK(e->selected.has_value());
    e->OnKey(VK_DELETE);
    CHECK(e->edit.captions.empty());
    e->Undo();
    CHECK_EQ(e->edit.captions.size(), 1u);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// ←/→ step exactly one frame at the clip's own rate and show that very frame: through a 60 fps clip every frame once,
// in order, forward and back; then across a joined 30 fps clip. Shift steps a second; Home and End go to the trim's ends.
ATHER_TEST(video_editor_steps_every_frame_at_60fps) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4", b = dir + L"\\b30.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));    // 2 s at 60 fps
    CHECK(WriteNumberedClip(b, 320, 180, 30, 60, 255));   // 2 s at 30 fps, with blue
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(e->FramesKnown());
    // Walks `steps` presses of `vk` from where the playhead is, and says what each frame shown was ("a12", "b3").
    auto walk = [&](WPARAM vk, int steps, bool shift = false) {
        std::vector<std::string> seen;
        for (int i = 0; i < steps; ++i) {
            e->Key(vk, {false, shift, false});
            const auto [n, blue] = ShownFrame(e);
            seen.push_back((blue ? "b" : "a") + std::to_string(n));
            if (n < 0) break;  // the frame at the playhead never came: no use waiting for the rest
        }
        return seen;
    };
    auto expect = [](const char* clip, int from, int to) {  // from…to inclusive, either way
        std::vector<std::string> v;
        for (int i = from;; i += from <= to ? 1 : -1) {
            v.push_back(clip + std::to_string(i));
            if (i == to) break;
        }
        return v;
    };
    auto join = [](std::vector<std::string> x, const std::vector<std::string>& y) {
        x.insert(x.end(), y.begin(), y.end());
        return x;
    };
    auto show = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 24; ++i) s += v[i] + " ";
        return s;
    };
    e->Key(VK_HOME, {});
    const auto first = ShownFrame(e);
    CHECK(first.first == 0 && !first.second);
    auto fwd = walk(VK_RIGHT, 119);
    test::Note("60 fps forward: " + show(fwd));
    CHECK(fwd == expect("a", 1, 119));
    auto back = walk(VK_LEFT, 119);
    test::Note("60 fps back: " + show(back));
    CHECK(back == expect("a", 118, 0));
    // Shift: a second, which is 60 frames here.
    auto sec = walk(VK_RIGHT, 1, true);
    CHECK(sec == std::vector<std::string>{"a60"});
    walk(VK_LEFT, 1, true);
    // Joined with a 30 fps clip: 120 + 60 frames, each once, in order, both ways.
    e->AddClips({b});
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    e->Key(VK_HOME, {});
    CHECK(ShownFrame(e).first == 0);
    fwd = walk(VK_RIGHT, 179);
    test::Note("joined forward: " + show(std::vector<std::string>(fwd.begin() + std::min<size_t>(110, fwd.size()), fwd.end())));
    CHECK(fwd == join(expect("a", 1, 119), expect("b", 0, 59)));
    back = walk(VK_LEFT, 179);
    test::Note("joined back: " + show(back));
    CHECK(back == join(expect("b", 58, 0), expect("a", 119, 0)));
    // A second in the 30 fps clip is 30 frames.
    for (int i = 0; i < 120; ++i) e->Key(VK_RIGHT, {});
    CHECK(walk(VK_RIGHT, 1, true) == std::vector<std::string>{"b30"});
    // Home and End: the first and last frames of the trim (0.5 s into a, 0.5 s into b).
    e->edit.trimStart = 0.5;
    e->edit.trimEnd = 2.5;
    e->Key(VK_HOME, {});
    const auto home = ShownFrame(e);
    CHECK(home.first == 30 && !home.second);
    e->Key(VK_END, {});
    const auto end = ShownFrame(e);
    test::Note("end: " + std::to_string(end.first) + (end.second ? " b" : " a"));
    CHECK(end.first == 14 && end.second);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// The readout shows m:ss:ff · frame n · fps for the frame on screen; Ctrl+G takes a frame number, a timecode or a time
// and lands on that frame (nothing to undo); a joined 30 fps clip reads at its own rate.
ATHER_TEST(video_editor_readout_and_go_to) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4", b = dir + L"\\b30.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    CHECK(WriteNumberedClip(b, 320, 180, 30, 60, 255));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(e->ReadoutText() == L"0:00:00 · frame 0 · 60 fps");
    auto goTo = [&](const wchar_t* text) {
        e->Key('G', {true, false, false});
        CHECK(e->goingTo);
        SetWindowTextW(e->field1, text);
        return e->CommitGoTo();
    };
    CHECK(goTo(L"0:01:30"));
    CHECK(!e->goingTo);
    CHECK(ShownFrame(e) == std::make_pair(90, false));
    CHECK(e->ReadoutText() == L"0:01:30 · frame 90 · 60 fps");
    CHECK(goTo(L"45"));
    CHECK(ShownFrame(e) == std::make_pair(45, false));
    CHECK(e->ReadoutText() == L"0:00:45 · frame 45 · 60 fps");
    CHECK(goTo(L"1.26"));  // the frame on screen at 1.26 s: 75 (1.25 s)
    CHECK(ShownFrame(e) == std::make_pair(75, false));
    e->Key(VK_RIGHT, {});
    CHECK(e->ReadoutText() == L"0:01:16 · frame 76 · 60 fps");
    CHECK(!goTo(L"not a time"));  // says so and stays open
    CHECK(e->goingTo);
    e->EndGoTo();
    CHECK(!e->goingTo);
    CHECK(e->undoStack.empty() && !e->dirty);
    // Joined with a 30 fps clip: frame numbers run on; that clip's frames read at 30 fps.
    e->AddClips({b});
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(goTo(L"130"));
    CHECK(ShownFrame(e) == std::make_pair(10, true));
    CHECK(e->ReadoutText() == L"0:02:10 · frame 130 · 30 fps");
    CHECK(goTo(L"0:03:00"));
    CHECK(ShownFrame(e) == std::make_pair(30, true));
    CHECK(e->ReadoutText() == L"0:03:00 · frame 150 · 30 fps");
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// J/K/L: L plays, L again cycles the preview speed 0.25× → 0.5× → 1×, J plays backward, K pauses. The preview speed
// leaves the edit (and so the export) as it was, and slow playback shows every frame in turn, none skipped.
ATHER_TEST(video_editor_review_playback_jkl) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    const VideoEdit before = e->edit;
    // Every frame the editor shows during `ms`, by number, as it changes.
    auto watch = [&](int ms) {
        std::vector<int> seen;
        e->onFetched = [&] {
            const Bitmap& f = *e->raw;
            const int n = NumberOf(f.Bits()[(size_t)(f.Height() / 2) * f.Width() + f.Width() / 2]);
            if (seen.empty() || seen.back() != n) seen.push_back(n);
        };
        Pump(ms);
        e->onFetched = nullptr;
        return seen;
    };
    auto steps = [](const std::vector<int>& v, int by) {  // every frame after the first is the one `by` from the one before
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] - v[i - 1] != by) return false;
        return v.size() > 1;
    };
    auto show = [](const std::vector<int>& v) {
        std::string s;
        for (int n : v) s += std::to_string(n) + " ";
        return s;
    };
    e->GoToFrame(10);
    ShownFrame(e);
    e->Key('L', {});
    CHECK(e->playing && e->previewRate == 1 && e->reviewDir == 0);  // the usual playback first
    e->Key('L', {});
    CHECK(!e->playing && e->reviewDir == 1 && e->previewRate == 0.25);
    const int from = ShownFrame(e).first;
    const auto slow = watch(1500);
    test::Note("0.25x from " + std::to_string(from) + ": " + show(slow));
    CHECK(steps(slow, 1));
    CHECK(slow.size() >= 15 && slow.size() <= 26);  // 15 frames a second at 0.25× of 60 fps
    CHECK(e->edit == before);
    CHECK(e->edit.speed == 1);
    e->Key('L', {});
    CHECK(e->reviewDir == 1 && e->previewRate == 0.5);
    const auto half = watch(600);
    test::Note("0.5x: " + show(half));
    CHECK(steps(half, 1) && half.size() >= 10);
    e->Key('K', {});
    CHECK(e->reviewDir == 0 && !e->playing);
    const auto still = watch(300);
    CHECK(still.size() <= 1);
    // Backward at the speed kept (0.5×).
    e->Key('J', {});
    CHECK(e->reviewDir == -1 && e->previewRate == 0.5);
    const auto back = watch(800);
    test::Note("reverse: " + show(back));
    CHECK(steps(back, -1) && back.size() >= 15);
    e->Key('J', {});
    CHECK(e->reviewDir == -1 && e->previewRate == 1);
    // Runs into the start of the trim and stops there.
    e->edit.trimStart = (double)(ShownFrame(e).first - 5) / 60;
    watch(500);
    CHECK(e->reviewDir == 0);
    CHECK(ShownFrame(e).first == (int)std::lround(e->edit.trimStart * 60));
    e->edit.trimStart = 0;
    CHECK(e->edit == before);
    CHECK(e->undoStack.empty() && !e->dirty);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Review notes: M adds one on the frame on screen with its field ready; typing edits it; a click on the video pins a
// spot; kinds and Resolved; ticks and list rows go to their frames; [ and ] jump; Delete, and undo covers all of it.
ATHER_TEST(video_editor_notes_add_edit_pin_jump_undo) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(!e->NotesShown());
    // M on frame 30.
    e->GoToFrame(30);
    e->Key('M', {});
    CHECK_EQ(e->edit.notes.size(), 1u);
    if (e->edit.notes.size() != 1) return DestroyWindow(e->hwnd), void();
    const uint64_t first = e->edit.notes[0].id;
    CHECK(e->selected == first);
    CHECK_NEAR(e->edit.notes[0].src, 0.5, 1e-3);
    CHECK(!e->edit.notes[0].author.empty() && e->edit.notes[0].author == NoteAuthor());
    CHECK(e->fieldsFor == first && e->fieldsKind == 300);  // its text field
    CHECK(e->NotesShown());
    CHECK_EQ(e->undoStack.size(), 1u);
    SetWindowTextW(e->field1, L"Button flickers here");
    CHECK(e->edit.notes[0].text == L"Button flickers here");
    // A click in the middle of the video pins the middle of the frame (one undo step).
    Snapshot(e);  // lays out the video
    const gp::PointF mid = e->ToView(VPoint{160, 90});
    e->OnMouseDown({(LONG)std::lround(mid.X), (LONG)std::lround(mid.Y)}, false);
    e->OnMouseUp();
    CHECK(e->edit.notes[0].pin && std::fabs(e->edit.notes[0].pin->x - 0.5) < 0.02 && std::fabs(e->edit.notes[0].pin->y - 0.5) < 0.02);
    CHECK_EQ(e->undoStack.size(), 2u);
    // Kind and Resolved, each undoable.
    auto kinds = e->NoteKindMenu(first);
    CHECK_EQ(kinds.size(), 4u);
    if (kinds.size() == 4) kinds[1].run();
    CHECK(e->edit.notes[0].kind == NoteKind::Issue);
    e->Undo();
    CHECK(e->edit.notes[0].kind == NoteKind::Note && e->edit.notes[0].pin);
    if (kinds.size() == 4) kinds[1].run();
    // Two more: frame 90 (a question), then frame 60 (looks good, resolved).
    e->Select(std::nullopt);
    e->GoToFrame(90);
    e->Key('M', {});
    e->NoteKindMenu(e->edit.notes.back().id)[2].run();
    e->GoToFrame(60);
    e->Key('M', {});
    e->NoteKindMenu(e->edit.notes.back().id)[3].run();
    e->UpdateNote([](Note& n) { n.resolved = true; });
    CHECK_EQ(e->placed.size(), 3u);
    std::vector<size_t> frames;
    for (const auto& p : e->placed) frames.push_back(p.first);
    CHECK(frames == std::vector<size_t>({30, 60, 90}));
    // ] and [ from the start: 30, 60, 90, (stays), back to 60.
    e->Select(std::nullopt);
    e->GoToFrame(0);
    std::vector<size_t> seen;
    for (WPARAM k : {VK_OEM_6, VK_OEM_6, VK_OEM_6, VK_OEM_6, VK_OEM_4}) {
        e->Key(k, {});
        seen.push_back(e->FrameNow());
        CHECK(e->SelNote().has_value());
    }
    CHECK(seen == std::vector<size_t>({30, 60, 90, 90, 60}));
    CHECK(ShownFrame(e).first == 60);
    // Only open notes: the resolved one at 60 is skipped by the list and by ] and [.
    e->notesOpenOnly = true;
    CHECK_EQ(e->ListedNotes().size(), 2u);
    e->GoToFrame(0);
    e->Key(VK_OEM_6, {});
    e->Key(VK_OEM_6, {});
    CHECK_EQ(e->FrameNow(), 90u);
    e->notesOpenOnly = false;
    // Clicking a list row, then a tick on the timeline, goes to that note's frame.
    auto snap = Snapshot(e);  // lays out the list's rows
    const RECT list = e->NotesListRect();
    e->OnMouseDown({list.left + e->S(60), list.top + e->NoteRowH() / 2}, false);  // first row: frame 30
    e->OnMouseUp();
    CHECK(e->selected == first && e->FrameNow() == 30);
    const RECT tr = e->TimelineRect();
    e->OnMouseDown({(LONG)e->TX(e->tframes.frames[90].t), tr.top + e->LaneH() + e->S(4)}, false);
    e->OnMouseUp();
    CHECK(e->FrameNow() == 90 && e->SelNote() && e->edit.notes[*e->SelNote()].kind == NoteKind::Question);
    // Ticks wear their kind's color: the Issue's flag at frame 30 is red.
    snap = Snapshot(e);
    const uint32_t flag = snap->Bits()[(size_t)(tr.top + e->LaneH() + e->S(2)) * snap->Width() + (LONG)e->TX(e->tframes.frames[30].t)];
    test::Note("flag pixel " + std::to_string(flag & 0xFFFFFF));
    const COLORREF red = annot::Color(0);
    CHECK(std::abs((int)((flag >> 16) & 255) - GetRValue(red)) < 30 && std::abs((int)((flag >> 8) & 255) - GetGValue(red)) < 30 &&
          std::abs((int)(flag & 255) - GetBValue(red)) < 30);
    // Delete, then undo it.
    e->SelectNote(first);
    e->Key(VK_DELETE, {});
    CHECK_EQ(e->edit.notes.size(), 2u);
    CHECK_EQ(e->placed.size(), 2u);
    e->Undo();
    CHECK_EQ(e->edit.notes.size(), 3u);
    CHECK(e->edit.notes[0].text == L"Button flickers here" && e->edit.notes[0].kind == NoteKind::Issue);
    // None of this touched the picture's edit.
    VideoEdit plain = e->edit;
    plain.notes.clear();
    CHECK(plain.marks.empty() && plain.captions.empty());
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Notes are kept next to their video as they change, and come back at the same frames: after reopening, in a joined
// clip (each in its own file's sidecar), with a clip trimmed. A damaged sidecar never stops the video opening, and is
// kept aside rather than written over.
ATHER_TEST(video_editor_notes_kept_next_to_the_video) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4", b = dir + L"\\b30.mp4", c = dir + L"\\c60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    CHECK(WriteNumberedClip(b, 320, 180, 30, 60, 255));
    CHECK(WriteNumberedClip(c, 320, 180, 60, 60, 0));
    auto open = [&](const std::wstring& path) {
        VideoEditor* e = OpenHidden(path, 1180, 760);
        for (int i = 0; e && i < 300 && !e->FramesKnown(); ++i) Pump(10);
        return e;
    };
    auto sidecar = [](const std::wstring& video) {  // the sidecar's notes, read as another tool would
        std::string text;
        bool ok = false;
        const Json j = ReadFileUtf8(NotesPath(video), &text) ? Json::Parse(text, &ok) : Json();
        return ok ? j["notes"] : Json();
    };
    auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto noteAt = [](VideoEditor* e, size_t frame, NoteKind kind, const wchar_t* text) {
        e->Select(std::nullopt);
        e->GoToFrame(frame);
        e->Key('M', {});
        e->UpdateNote([&](Note& n) {
            n.kind = kind;
            n.text = text;
        });
        e->Select(std::nullopt);
    };
    // Round trip: two notes on a, then reopen.
    VideoEditor* e = open(a);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(!exists(NotesPath(a)));  // opening alone writes nothing
    noteAt(e, 30, NoteKind::Issue, L"Flicker");
    noteAt(e, 90, NoteKind::Question, L"Intended?");
    e->SelectNote(e->edit.notes[0].id);
    e->UpdateNote([](Note& n) {
        n.pin = VPoint{0.25, 0.75};
        n.resolved = true;
    });
    e->SetNoteRangeToPlayhead();  // the playhead is on the note: not a range
    e->GoToFrame(40);
    e->SetNoteRangeToPlayhead();
    const std::vector<Note> before = [&] {
        auto v = e->edit.notes;
        SortNotes(v);
        return v;
    }();
    Json kept = sidecar(a);
    CHECK_EQ(kept.size(), 2u);
    CHECK(kept[0]["frame"].Int() == 30 && kept[0]["kind"].Str() == "issue" && kept[0]["resolved"].Bool() && kept[0]["endFrame"].Int() == 40);
    CHECK(kept[1]["frame"].Int() == 90 && kept[1]["text"].Str() == "Intended?" && kept[0]["pin"]["x"].Num() == 0.25);
    // Undo takes a change back out of the sidecar too.
    noteAt(e, 100, NoteKind::Note, L"Gone again");
    CHECK_EQ(sidecar(a).size(), 3u);
    e->Undo();  // the text and kind
    e->Undo();  // the note
    CHECK_EQ(sidecar(a).size(), 2u);
    e->dirty = false;
    DestroyWindow(e->hwnd);
    e = open(a);
    CHECK(e != nullptr);
    if (!e) return;
    auto after = e->edit.notes;
    SortNotes(after);
    CHECK_EQ(after.size(), 2u);
    for (size_t i = 0; i < after.size() && i < before.size(); ++i) {
        CHECK(after[i].text == before[i].text && after[i].kind == before[i].kind && after[i].resolved == before[i].resolved && after[i].pin == before[i].pin &&
              after[i].author == before[i].author && after[i].srcEnd == before[i].srcEnd && after[i].src == before[i].src && after[i].id == before[i].id);
    }
    std::vector<size_t> at;
    for (const auto& p : e->placed) at.push_back(p.first);
    CHECK(at == std::vector<size_t>({30, 90}));
    CHECK(!e->dirty && e->undoStack.empty());
    // Joined: b's own note (made on b alone) shows at b's frame 10, frame 130 of a + b; a new note on b's footage
    // goes into b's sidecar, not a's.
    e->dirty = false;
    DestroyWindow(e->hwnd);
    e = open(b);
    CHECK(e != nullptr);
    if (!e) return;
    noteAt(e, 10, NoteKind::Good, L"b ten");
    e->dirty = false;
    DestroyWindow(e->hwnd);
    e = open(a);
    CHECK(e != nullptr);
    if (!e) return;
    e->AddClips({b});
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    at.clear();
    for (const auto& p : e->placed) at.push_back(p.first);
    CHECK(at == std::vector<size_t>({30, 90, 130}));
    e->SelectNote(e->edit.notes[e->placed[2].second].id);
    CHECK(ShownFrame(e) == std::make_pair(10, true));
    noteAt(e, 140, NoteKind::Note, L"b twenty");
    CHECK_EQ(sidecar(a).size(), 2u);
    CHECK_EQ(sidecar(b).size(), 2u);
    CHECK(sidecar(b)[1]["frame"].Int() == 20 && sidecar(b)[1]["text"].Str() == "b twenty");
    // b trimmed to start at its frame 15: the note at b's 10 isn't on the timeline (still kept), the one at 20 moves
    // to 120 + 5; undo brings the trim back.
    e->TrimClip(1, 0.5, std::nullopt);
    at.clear();
    for (const auto& p : e->placed) at.push_back(p.first);
    CHECK(at == std::vector<size_t>({30, 90, 125}));
    CHECK_EQ(sidecar(b).size(), 2u);
    e->SelectNote(e->edit.notes[e->placed[2].second].id);
    CHECK(ShownFrame(e) == std::make_pair(20, true));
    e->Undo();
    CHECK_EQ(e->placed.size(), 4u);
    // Undoing the note on b takes it out of b's sidecar; undoing the joining of b leaves b's own note there.
    e->Undo();
    e->Undo();
    CHECK_EQ(sidecar(b).size(), 1u);
    e->Undo();
    CHECK_EQ(e->edit.clips.size(), 1u);
    CHECK(sidecar(b).size() == 1 && sidecar(b)[0]["text"].Str() == "b ten");
    e->dirty = false;
    DestroyWindow(e->hwnd);
    // Damaged: the video still opens, without notes; the first new note keeps the damaged file aside, as it was.
    {
        HANDLE f = CreateFileW(NotesPath(c).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        const char damaged[] = "{\"notes\": [ {\"frame\": 3,";
        WriteFile(f, damaged, (DWORD)strlen(damaged), &w, nullptr);

        CloseHandle(f);
    }
    e = open(c);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->edit.notes.empty());
    CHECK(e->raw != nullptr);
    noteAt(e, 5, NoteKind::Note, L"After the damage");
    std::string aside;
    CHECK(ReadFileUtf8(c + L".notes.damaged.json", &aside) && aside == "{\"notes\": [ {\"frame\": 3,");
    CHECK(sidecar(c).size() == 1 && sidecar(c)[0]["frame"].Int() == 5);
    // A sidecar this editor made goes again with its last note.
    e->Undo();
    e->Undo();
    CHECK(e->edit.notes.empty());
    CHECK(exists(NotesPath(c)));  // it wasn't made here: written empty, not deleted
    CHECK_EQ(sidecar(c).size(), 0u);
    e->dirty = false;
    DestroyWindow(e->hwnd);
    const std::wstring d = dir + L"\\d60.mp4";
    CHECK(WriteNumberedClip(d, 320, 180, 60, 30, 0));
    e = open(d);
    CHECK(e != nullptr);
    if (!e) return;
    noteAt(e, 3, NoteKind::Note, L"Short-lived");
    CHECK(exists(NotesPath(d)));
    e->Select(e->edit.notes[0].id);
    e->Key(VK_DELETE, {});
    CHECK(!exists(NotesPath(d)));
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Timeline zoom: Ctrl+= zooms around the playhead (its frame stays put) down to about 14 px a frame; pixels and frames
// map both ways at every zoom and pan; at full zoom a click on a frame's tick lands on that frame; Ctrl+0 fits the
// trim; notes and caption bars stay on their frames.
ATHER_TEST(video_editor_timeline_zoom) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    const RECT tr = e->TimelineRect();
    CHECK_NEAR(e->TX(0), tr.left, 1e-9);
    CHECK_NEAR(e->TX(e->duration), tr.right, 1e-6);
    e->GoToFrame(50);
    const double t50 = e->tframes.frames[50].t;
    // Every frame's x leads back to that frame, and every pixel to the frame nearest it.
    auto roundTrip = [&] {
        int wrong = 0;
        for (size_t n = 0; n < e->tframes.size(); ++n) {
            const double x = e->TX(e->tframes.frames[n].t);
            if (x < tr.left || x > tr.right) continue;
            wrong += e->tframes.Nearest(e->TT((int)std::lround(x))) != n;
        }
        for (int x = tr.left; x <= tr.right; x += 7) {
            const size_t n = e->tframes.Nearest(e->TT(x));
            const double half = (double)RectW(tr) / e->TlSpan() / 120.0;  // half a frame in pixels
            const bool pastLast = n + 1 == e->tframes.size() && x >= e->TX(e->tframes.frames[n].t);  // the last frame shows to the end
            wrong += !pastLast && std::fabs(e->TX(e->tframes.frames[n].t) - x) > half + 1;
        }
        return wrong;
    };
    CHECK_EQ(roundTrip(), 0);
    double x50 = e->TX(t50);
    int presses = 0;
    while (e->tlZoom < e->MaxTlZoom() - 1e-9 && presses < 40) {
        e->Key(VK_OEM_PLUS, {true, false, false});
        ++presses;
        test::Note("zoom " + std::to_string(e->tlZoom) + ", frame 50 at " + std::to_string(e->TX(t50)) + " (was " + std::to_string(x50) + ")");
        CHECK(std::fabs(e->TX(t50) - x50) <= 1);  // the playhead's frame stays under the same spot
        CHECK_EQ(roundTrip(), 0);
    }
    const double pf = e->PxPerFrame();
    test::Note("full zoom: " + std::to_string(pf) + " px a frame after " + std::to_string(presses) + " presses");
    CHECK(pf >= 13.5 && pf <= 14.5);
    // At full zoom, a click on a frame's tick lands on that frame (and shows it).
    const int stripY = tr.top + e->LaneH() + e->S(30);
    for (size_t n : {48, 53, 57}) {
        e->OnMouseDown({(LONG)std::lround(e->TX(e->tframes.frames[n].t)), stripY}, false);
        e->OnMouseUp();
        CHECK_EQ(e->FrameNow(), n);
        CHECK(ShownFrame(e).first == (int)n);
    }
    // Panning with the wheel moves everything together; the mapping still holds.
    const double before = e->tlStart;
    e->OnWheel({tr.left + 100, stripY}, -WHEEL_DELTA, 0);
    CHECK(e->tlStart > before);
    CHECK_EQ(roundTrip(), 0);
    // Dragging an empty lane pans; a click there (no drag) goes to that time.
    const int laneY = tr.top + e->LaneH() + e->S(70);
    const double s0 = e->tlStart;
    e->OnMouseDown({tr.left + 300, laneY}, false);
    e->OnMouseMove({tr.left + 200, laneY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(e->tlStart > s0);
    const size_t clickAt = e->tframes.Nearest(e->TT(tr.left + 400));
    e->OnMouseDown({tr.left + 400, laneY}, false);
    e->OnMouseUp();
    CHECK_EQ(e->FrameNow(), clickAt);
    // A note and a caption bar sit on their frames, zoomed in: the note's flag shows its color at its frame's x.
    e->GoToFrame(60);
    e->Key('M', {});
    e->UpdateNote([](Note& n) { n.kind = NoteKind::Question; });
    e->Select(std::nullopt);
    e->Seek(e->tframes.frames[62].t);
    e->AddCaption();
    e->edit.captions.back().start = e->tframes.frames[62].t;
    e->edit.captions.back().text = L"Here";
    e->Select(std::nullopt);
    e->Changed();
    auto snap = Snapshot(e);
    auto px = [&](double x, int y) { return snap->Bits()[(size_t)y * snap->Width() + (int)std::lround(x)] & 0xFFFFFF; };
    const COLORREF blue = annot::Color(4);
    const uint32_t flag = px(e->TX(e->tframes.frames[60].t), tr.top + e->LaneH() + e->S(2));
    test::Note("flag " + std::to_string(flag));
    CHECK(std::abs((int)(flag & 255) - GetBValue(blue)) < 40 && std::abs((int)((flag >> 16) & 255) - GetRValue(blue)) < 40);
    const int capMid = tr.top + e->LaneH() + e->S(62) + e->S(11);
    const uint32_t onBar = px(e->TX(e->tframes.frames[62].t) + e->S(3), capMid), before62 = px(e->TX(e->tframes.frames[62].t) - e->S(3), capMid);
    test::Note("caption bar " + std::to_string(onBar) + " before it " + std::to_string(before62));
    CHECK(onBar != before62);
    // Ctrl+0 fits the trim.
    e->edit.trimStart = 0.2;  // 1.6 s: within the 1.58x a 2 s clip zooms to
    e->edit.trimEnd = 1.8;
    e->Key('0', {true, false, false});
    test::Note("fit: zoom " + std::to_string(e->tlZoom) + " start " + std::to_string(e->tlStart) + " x " + std::to_string(e->TX(0.2)) + ".." +
               std::to_string(e->TX(1.8)) + " of " + std::to_string(tr.left) + ".." + std::to_string(tr.right));
    CHECK(std::fabs(e->TX(0.2) - tr.left) <= 1 && std::fabs(e->TX(1.8) - tr.right) <= 1);
    e->edit.trimStart = 0;
    e->edit.trimEnd = e->duration;
    e->Key('0', {true, false, false});
    CHECK_NEAR(e->tlZoom, 1, 1e-9);
    // Zooming never touches the edit.
    CHECK(e->edit.speed == 1 && !e->edit.crop);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Zoomed in, every filmstrip tile shows a frame from the frames it covers (the first one starting in it), read from the
// painted strip itself, at several zooms and pans; the pictures come in the background, painting never waits for them.
ATHER_TEST(video_editor_filmstrip_shows_frames_under_each_tile) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 240, 0));  // 4 s
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    const RECT tr = e->TimelineRect();
    const int stripTop = tr.top + e->LaneH(), y = stripTop + e->S(22);  // under the flags, above the ruler
    int tilesChecked = 0, wrong = 0, maxPaint = 0;
    std::string first;
    for (double zoom : {2.0, 5.0, 1000.0}) {
        e->FitTrim();
        e->GoToFrame(100);
        e->ZoomTimeline(zoom);
        for (double at : {0.0, 0.5, 1.0}) {  // panned to the start, the middle and the end
            e->tlStart = at * std::max(0.0, e->duration - e->TlSpan());
            e->ClampTimeline();
            const ULONGLONG t0 = GetTickCount64();
            Snapshot(e);  // asks for the tiles' pictures
            maxPaint = std::max(maxPaint, (int)(GetTickCount64() - t0));
            for (int i = 0; i < 300 && !e->StripComplete(); ++i) Pump(10);
            CHECK(e->StripComplete());
            const auto snap = Snapshot(e);
            const double px = e->TX(e->Now());
            for (const auto& tile : e->StripTiles()) {
                const double xa = std::max(e->TX(tile.t0), (double)tr.left) + 3, xb = std::min(e->TX(tile.t1), (double)tr.right) - 3;
                if (xb - xa < 4) continue;  // hardly on view
                double x = (xa + xb) / 2;
                if (std::fabs(x - px) < 4) x = x < px ? px - 5 : px + 5;  // not on the playhead's line
                if (x < xa || x > xb) continue;
                const int n = NumberOf(snap->Bits()[(size_t)y * snap->Width() + (int)x]);
                const bool inside = n >= 0 && n < (int)e->tframes.size() && e->tframes.frames[(size_t)n].t >= tile.t0 - 1e-6 &&
                                    (e->tframes.frames[(size_t)n].t < tile.t1 || tile.t1 - tile.t0 < 1 / 60.0);
                if (!inside || n != (int)tile.frame) {
                    if (!wrong)
                        first = "zoom " + std::to_string(zoom) + " at " + std::to_string(at) + ": tile " + std::to_string(tile.t0) + "–" + std::to_string(tile.t1) +
                                " shows frame " + std::to_string(n) + " (wanted " + std::to_string(tile.frame) + ")";
                    ++wrong;
                }
                ++tilesChecked;
            }
        }
    }
    test::Note(std::to_string(tilesChecked) + " tiles, " + std::to_string(wrong) + " wrong; " + first + "; slowest paint " + std::to_string(maxPaint) + " ms");
    CHECK(tilesChecked >= 60);
    CHECK_EQ(wrong, 0);
    CHECK(maxPaint < 250);  // painting never waits for pictures
    // At full zoom each tile covers whole frames of its own: about 6 at 14 px a frame.
    e->ZoomTimeline(1000);
    const auto tiles = e->StripTiles();
    CHECK(!tiles.empty() && (tiles[0].t1 - tiles[0].t0) * 60 > 3 && (tiles[0].t1 - tiles[0].t0) * 60 < 12);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// The pixel magnifier: the wheel over the video zooms 1×–8× keeping the pixel under the cursor there; from 2× a 1-pixel
// checkerboard shows as hard-edged blocks (only black and white); a right-drag pans; F fits again; the edit (crop and
// all) never changes.
ATHER_TEST(video_editor_pixel_magnifier) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 60, 0));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    Pump(300);
    e->edit.crop = VRect{40, 20, 200, 120};  // the magnifier works on top of a crop and leaves it be
    const VideoEdit before = e->edit;
    // The frame shown: a 1-pixel checkerboard (exact pixels, as no video codec keeps them).
    auto checker = Bitmap::Create(320, 180);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) checker->Bits()[(size_t)y * 320 + x] = (x + y) % 2 ? 0xFFFFFFFFu : 0xFF000000u;
    auto showChecker = [&] {
        e->raw = checker;
        e->shown = checker;
    };
    const auto v = e->VideoRect();
    const POINT at{(LONG)(v.X + v.Width * 0.3), (LONG)(v.Y + v.Height * 0.6)};
    const VPoint under = e->ToVideo(at);
    for (int i = 0; i < 6; ++i) {
        e->OnWheel(at, WHEEL_DELTA, 0);
        const VPoint now = e->ToVideo(at);
        CHECK(std::fabs(now.x - under.x) < 0.01 && std::fabs(now.y - under.y) < 0.01);  // stays under the cursor
    }
    test::Note("zoom after 6 notches: " + std::to_string(e->magZoom));
    CHECK_NEAR(e->magZoom, std::pow(1.25, 6), 1e-9);
    for (int i = 0; i < 20; ++i) e->OnWheel(at, WHEEL_DELTA, 0);
    CHECK_NEAR(e->magZoom, 8, 1e-9);
    // At 4×: sharp blocks only.
    e->FitMagnifier();
    e->Magnify(4, at);
    CHECK_NEAR(e->magZoom, 4, 1e-9);
    showChecker();
    auto snap = Snapshot(e);
    auto stageColors = [&](const Bitmap& b, int* grays) {
        int bw = 0;
        *grays = 0;
        for (int y = (int)v.Y + 40; y < (int)(v.Y + v.Height) - 40; y += 3)  // inside the video, away from the label and the edges
            for (int x = (int)v.X + 60; x < (int)(v.X + v.Width) - 10; x += 3) {
                const uint32_t c = b.Bits()[(size_t)y * b.Width() + x] & 0xFFFFFF;
                if (c == 0 || c == 0xFFFFFF) ++bw;
                else ++*grays;
            }
        return bw;
    };
    int grays = 0;
    const int bw = stageColors(*snap, &grays);
    test::Note("4x: " + std::to_string(bw) + " black or white, " + std::to_string(grays) + " other");
    CHECK(bw > 1000);
    CHECK_EQ(grays, 0);
    // Each video pixel is a block about 4 × the fitted scale wide: count a row's runs.
    {
        const int y = (int)(v.Y + v.Height / 2);
        int runs = 0, longest = 0, run = 0;
        uint32_t last = 1;
        for (int x = (int)v.X + 60; x < (int)(v.X + v.Width) - 10; ++x) {
            const uint32_t c = snap->Bits()[(size_t)y * snap->Width() + x] & 0xFFFFFF;
            if (c == last) ++run;
            else {
                ++runs;
                run = 1;
                last = c;
            }
            longest = std::max(longest, run);
        }
        const double k = e->ViewScale();
        test::Note("block " + std::to_string(longest) + " px, scale " + std::to_string(k));
        CHECK(longest >= (int)std::floor(k) && longest <= (int)std::ceil(k));
    }
    // Fitted (1×), the same frame is drawn smoothed: other shades appear, as they should.
    e->FitMagnifier();
    showChecker();
    snap = Snapshot(e);
    stageColors(*snap, &grays);
    CHECK(grays > 0);
    // A right-drag pans by the video pixels dragged; F fits again.
    e->Magnify(4, at);
    const VPoint p0 = e->ToVideo(at);
    e->Proc(WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(at.x, at.y));
    e->OnMouseMove({at.x - 40, at.y - 20}, MK_RBUTTON);
    e->Proc(WM_RBUTTONUP, 0, MAKELPARAM(at.x - 40, at.y - 20));
    const VPoint p1 = e->ToVideo(at);
    CHECK_NEAR(p1.x - p0.x, 40 / e->ViewScale(), 0.01);
    CHECK_NEAR(p1.y - p0.y, 20 / e->ViewScale(), 0.01);
    e->Key('F', {});
    CHECK(e->magZoom == 1 && e->magX == 0 && e->magY == 0);
    // Wheel out never goes below 1×.
    e->OnWheel(at, -WHEEL_DELTA * 3, 0);
    CHECK(e->magZoom == 1);
    CHECK(e->edit == before);
    CHECK(e->undoStack.empty());
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Save review video (Ctrl+Alt+S, or Save ▾): "<video> review.mp4" with "… review notes.md" and "… review sheet.png"
// next to it in the captures; the notes list has one line per note in the trim, "m:ss:ff (frame n), Author: [Kind] text",
// and Copy puts the same text on the clipboard. The edit and the original are untouched; a second save never
// overwrites the first.
ATHER_TEST(video_editor_saves_review_video_notes_list_and_sheet) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\shot 60.mp4", caps = dir + L"\\caps";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    auto noteAt = [&](size_t frame, NoteKind kind, const wchar_t* text, const wchar_t* author, bool resolved) {
        e->Select(std::nullopt);
        e->GoToFrame(frame);
        e->Key('M', {});
        e->UpdateNote([&](Note& n) {
            n.kind = kind;
            n.text = text;
            n.author = author;
            n.resolved = resolved;
        });
    };
    noteAt(90, NoteKind::Question, L"Intended?", L"Mai", false);
    noteAt(30, NoteKind::Issue, L"Flicker\nsecond line", L"Tin", false);
    noteAt(5, NoteKind::Good, L"Cut before the trim", L"Tin", true);  // outside the trim below: not in the review
    e->edit.trimStart = 0.25;
    e->Changed();
    const std::wstring want = L"0:00:30 (frame 30), Tin: [Issue] Flicker / second line\r\n\r\n0:01:30 (frame 90), Mai: [Question] Intended?\r\n";
    test::Note("notes text: " + ToUtf8(e->NotesText()));
    CHECK(e->NotesText() == want);
    std::wstring copied;
    e->copyHook = [&](const std::wstring& t) { copied = t; };
    e->CopyNotes();
    CHECK(copied == want);
    const VideoEdit before = e->edit;
    e->Key('S', {true, false, true});  // Ctrl+Alt+S
    CHECK(e->busy);
    for (int i = 0; i < 300 && e->busy; ++i) Pump(100);
    test::Note("review save: " + ToUtf8(e->lastSaveError) + " → " + ToUtf8(e->lastReviewOut));
    CHECK(!e->busy && e->lastSaveError.empty());
    const std::wstring out = e->lastReviewOut;
    CHECK(FileNameOf(out) == L"shot 60 review.mp4");
    const std::wstring base = out.substr(0, out.size() - 4);
    std::string md;
    CHECK(ReadFileUtf8(base + L" notes.md", &md));
    CHECK(FromUtf8(md) == want);
    int sw = 0, sh = 0;
    CHECK(ImageSize(base + L" sheet.png", &sw, &sh) && sw == 1600 && sh > 300);
    VideoInfo vi;
    CHECK(ProbeVideo(out, &vi) && std::fabs(vi.duration - (1.75 + 3 + 2 * 3)) < 0.1);  // 1.75 s edit, card, two holds
    CHECK(e->edit == before);
    // Again: new names, the first files kept.
    e->SaveReview();
    for (int i = 0; i < 300 && e->busy; ++i) Pump(100);
    CHECK(FileNameOf(e->lastReviewOut) == L"shot 60 review 2.mp4");
    CHECK(GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Composition guides and the monochrome view: picked from the menu one by one or all at once, G hides them and brings
// them back, they're drawn over the preview (on the crop) and never change the edit; monochrome turns the preview grey.
ATHER_TEST(video_editor_guides) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 2, false));  // red, then green
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    const auto keepGuides = VideoEditor::lastGuides;
    const bool keepOn = VideoEditor::lastGuidesOn, keepMono = VideoEditor::lastMono;
    e->guides.fill(false);
    e->guidesOn = e->mono = false;
    CHECK(!e->AnyGuide());
    e->Key('G', {});  // nothing picked yet: the rule of thirds
    CHECK(e->GuideShown(GuideKind::Thirds) && !e->GuideShown(GuideKind::Golden));
    e->ToggleGuide(GuideKind::Golden);
    CHECK(e->GuideShown(GuideKind::Golden));
    e->Key('G', {});  // hide…
    CHECK(!e->AnyGuide());
    e->Key('G', {});  // …and back, the same two
    CHECK(e->GuideShown(GuideKind::Thirds) && e->GuideShown(GuideKind::Golden) && !e->GuideShown(GuideKind::Phi));
    // The menu: show all; its items are checked for the ones on.
    e->GuidesMenu()[0].run();
    CHECK(e->AnyGuide());
    for (int k = 0; k < kGuideKinds; ++k) CHECK(e->GuideShown((GuideKind)k));
    const auto menu = e->GuidesMenu();
    CHECK(menu.size() == 9 && menu[3].checked && menu[6].checked);
    CHECK(VideoEditor::lastGuidesOn);  // kept for the next video
    // Drawn over the preview: a thirds line across the middle of the video's left third shows in the window.
    e->Seek(0.5);
    for (int i = 0; i < 40 && !e->raw; ++i) Pump(50);
    const VideoEdit before = e->edit;
    auto snap = Snapshot(e);
    const gp::RectF view = e->ToView(e->ViewRect());
    const int gx = (int)std::lround(view.X + view.Width / 3), gy = (int)std::lround(view.Y + view.Height * 0.15);
    int blue = 0;
    for (int dx = -2; dx <= 2; ++dx) {
        const uint32_t px = snap->Bits()[(size_t)gy * snap->Width() + gx + dx];
        blue += (px & 255) > 150 && ((px >> 16) & 255) < 150;
    }
    CHECK(blue > 0);
    CHECK(e->edit == before);  // the edit is untouched
    // Monochrome: the preview turns grey (red footage reads as grey).
    e->ToggleMono();
    CHECK(e->mono && e->shown);
    if (e->shown) {
        const uint32_t p = e->shown->Pixel(320, 180);
        CHECK(((p >> 16) & 255) == (p & 255) && ((p >> 8) & 255) == (p & 255));
    }
    e->ToggleMono();
    if (e->shown) CHECK(((e->shown->Pixel(320, 180) >> 16) & 255) > 150);  // red again
    e->HideGuides();
    CHECK(!e->AnyGuide());
    VideoEditor::lastGuides = keepGuides;
    VideoEditor::lastGuidesOn = keepOn;
    VideoEditor::lastMono = keepMono;
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Loop: playback goes round the trim (or the selected clip) instead of stopping at its end, still playing; review
// playback (L at a slower speed) wraps too; off, playback stops at the end as before. Ctrl+L toggles it.
ATHER_TEST(video_editor_loop) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 2, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    const bool before = VideoEditor::lastLoop;
    if (e->loop) e->ToggleLoop();
    e->Key('L', {true, false, false});  // Ctrl+L
    CHECK(e->loop);
    // Waits for `done` (up to 10 s: a runner without a GPU starts playing slowly, more so after the clips change),
    // then a moment more so a round in progress settles.
    auto waitFor = [&](const std::function<bool()>& done) {
        for (int i = 0; i < 200 && !done(); ++i) Pump(50);
        Pump(150);
        test::Note("loops " + std::to_string(e->loops) + ", now " + std::to_string(e->Now()) + ", playing " + std::to_string(e->playing));
    };
    // Round the trim 0.3–0.9.
    e->edit.trimStart = 0.3;
    e->edit.trimEnd = 0.9;
    e->Changed();
    e->Seek(0.3);
    e->Play();
    waitFor([&] { return e->loops >= 1; });
    CHECK(e->loops >= 1);
    CHECK(e->playing);
    CHECK(e->Now() >= 0.25 && e->Now() <= 0.95);
    e->Pause();
    // Round the selected clip: the second half of a split.
    e->edit.trimStart = 0;
    e->edit.trimEnd = e->duration;
    e->Seek(1.0);
    e->SplitAtPlayhead();  // [0, 1) and [1, 2), the second selected
    CHECK(e->SelClipIndex() == 1u);
    CHECK_NEAR(e->LoopRange().first, 1.0, 0.02);
    e->loops = 0;
    e->Seek(1.5);
    e->Play();
    waitFor([&] { return e->loops >= 1; });
    CHECK(e->loops >= 1);
    CHECK(e->playing);
    CHECK(e->Now() >= 0.95);  // still in that clip
    e->Pause();
    // Review playback at a slower speed wraps too.
    e->SelectClip(std::nullopt);
    e->edit.trimStart = 0;
    e->edit.trimEnd = 0.3;
    e->Changed();
    e->loops = 0;
    e->Seek(0.2);
    e->previewRate = 0.5;
    e->Review(1);
    waitFor([&] { return e->loops >= 1; });
    CHECK(e->loops >= 1 && e->reviewDir == 1);
    e->Pause();
    // Off: it stops at the end.
    e->Key('L', {true, false, false});
    CHECK(!e->loop);
    e->edit.trimEnd = e->duration;
    e->Changed();
    e->loops = 0;
    e->Seek(1.6);
    e->Play();
    waitFor([&] { return !e->playing; });
    CHECK(!e->playing);
    CHECK_EQ(e->loops, 0);
    if (VideoEditor::lastLoop != before) VideoEditor::lastLoop = before;
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Flip flips one clip: the selected one, else the one at the playhead; one undo step each way. Markup that sits wholly
// on that clip is mirrored with it, the crop (every clip shares it) isn't; a note's pin stays on its spot of the
// footage; a selected video on top flips by itself. Saved, that clip's frames are mirror images, the others as they were.
ATHER_TEST(video_editor_flip) {
    const std::wstring dir = test::TempDir();
    const std::wstring clip = dir + L"/numbers.mp4";
    CHECK(WriteFrameNumberClip(clip, 640, 360, 30, 45));  // lopsided pictures: a number, a grid and a moving bar
    VideoEditor* e = OpenHidden(clip, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->Seek(0.75);
    e->SplitAtPlayhead();  // [0, 0.75) and [0.75, 1.5), the second selected
    CHECK(e->edit.clips.size() == 2);
    auto box = [&](double s0, double e0) {
        e->Seek(s0);
        e->AddMark(MarkKind::Box);
        Mark& m = e->edit.marks.back();
        m.start = s0, m.end = e0, m.a = {20, 30}, m.b = {120, 90};
    };
    box(0.9, 1.3);  // on the second clip
    box(0.1, 0.5);  // on the first
    e->edit.crop = VRect{0, 0, 400, 300};
    e->SelectClip(e->edit.clips[1].id);
    CHECK(e->FlipLabel() == L"Flip");
    e->ToggleFlip(true);
    CHECK(!e->edit.clips[0].flipH && e->edit.clips[1].flipH);
    CHECK(e->FlipLabel() == L"Flipped ↔");
    CHECK_NEAR(e->edit.marks[0].a.x, 620, 1e-9);
    CHECK_NEAR(e->edit.marks[1].a.x, 20, 1e-9);
    CHECK_NEAR(e->edit.crop->x, 0, 1e-9);
    e->ToggleFlip(false);
    CHECK(e->edit.clips[1].flipH && e->edit.clips[1].flipV);
    CHECK_NEAR(e->edit.marks[0].a.y, 330, 1e-9);
    e->Undo();
    e->Undo();
    CHECK(!e->edit.clips[1].Flipped() && e->edit.marks[0].a == (VPoint{20, 30}));
    // Nothing selected: the clip at the playhead.
    e->SelectClip(std::nullopt);
    e->Seek(0.3);
    e->ToggleFlip(true);
    CHECK(e->edit.clips[0].flipH && !e->edit.clips[1].flipH);
    e->Undo();
    // A pin a quarter in from the left of the footage shows a quarter in from the right once its clip is flipped.
    e->Seek(1.0);
    e->AddNote();
    e->edit.notes.back().pin = VPoint{0.25, 0.5};
    e->SelectClip(e->edit.clips[1].id);
    e->ToggleFlip(true);
    if (const auto p = e->PinOnVideo(e->edit.notes.back())) CHECK_NEAR(p->x, 480, 1);
    else CHECK(false);
    // A selected video on top flips by itself.
    if (const auto oc = ClipOf(clip)) {
        e->AddOverlay(*oc, 0, OverlayLayout::Pip);
        e->ToggleFlip(true);
        CHECK(e->edit.overlays.size() == 1 && e->edit.overlays[0].clip.flipH && e->edit.clips[1].flipH && !e->edit.clips[0].flipH);
        e->Undo();
        e->Undo();
    }
    CHECK(e->edit.overlays.empty());
    // Saved: the second clip's frames mirror the unflipped save's, the first clip's match them.
    e->edit.marks.clear();
    e->edit.crop.reset();
    e->edit.notes.clear();
    auto frames = [&](const VideoEdit& ed, const wchar_t* name) {
        std::pair<BitmapPtr, BitmapPtr> f;
        const std::wstring out = dir + name;
        if (ExportMp4(clip, ed, out, nullptr)) {
            ProbeVideo(out, nullptr, 0.4, 0, &f.first);
            ProbeVideo(out, nullptr, 1.1, 0, &f.second);
        }
        return f;
    };
    VideoEdit plainEdit = e->edit;
    for (auto& c : plainEdit.clips) c.flipH = c.flipV = false;
    const auto plain = frames(plainEdit, L"/plain.mp4"), flipped = frames(e->edit, L"/flipped.mp4");
    // Of a grid of points on `a`: how many match `b` at the same spot, and at the mirrored one.
    auto compare = [](const BitmapPtr& a, const BitmapPtr& b, int* same, int* mirrored) {
        *same = *mirrored = 0;
        auto close = [](uint32_t p, uint32_t q) {
            for (int sh : {0, 8, 16})
                if (std::abs((int)((p >> sh) & 255) - (int)((q >> sh) & 255)) > 40) return false;
            return true;
        };
        int n = 0;
        for (int y = 8; y < 360; y += 16)
            for (int x = 4; x < 640; x += 12, ++n) {
                *same += close(a->Pixel(x, y), b->Pixel(x, y));
                *mirrored += close(a->Pixel(x, y), b->Pixel(639 - x, y));
            }
        return n;
    };
    CHECK(plain.first && plain.second && flipped.first && flipped.second);
    if (plain.first && plain.second && flipped.first && flipped.second && plain.first->Width() == 640 && flipped.first->Width() == 640) {
        int same = 0, mirrored = 0;
        const int n = compare(flipped.first, plain.first, &same, &mirrored);
        test::Note("first clip: same " + std::to_string(same) + ", mirrored " + std::to_string(mirrored) + " of " + std::to_string(n));
        CHECK(same > n * 9 / 10);
        compare(flipped.second, plain.second, &same, &mirrored);
        test::Note("second clip: same " + std::to_string(same) + ", mirrored " + std::to_string(mirrored) + " of " + std::to_string(n));
        CHECK(mirrored > n * 9 / 10);
        CHECK(same < mirrored);
    }
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Several videos as one: add (any shape), split, reorder, remove, undo; playback crosses the cuts; save joins them.
ATHER_TEST(video_editor_joins_splits_and_reorders_clips) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4", caps = dir + L"/caps";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, true));
    CHECK(WriteTestClip(b, 360, 360, 30, 2, false));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->edit.clips.size(), 1u);
    CHECK_EQ(e->LaneH(), 0);  // one video: no clip lane
    e->AddClips({b, dir + L"/missing.mp4"});
    CHECK_EQ(e->edit.clips.size(), 2u);
    CHECK_NEAR(e->duration, 5, 0.1);
    CHECK_NEAR(e->edit.trimEnd, e->duration, 1e-9);  // an untrimmed end grows with the sequence
    CHECK(e->LaneH() > 0);
    // A caption in b's footage, then split a: everything stays put.
    e->Seek(4.0);
    e->AddCaption();
    const double capAt = e->edit.captions.back().start;
    e->Seek(1.5);
    e->SplitAtPlayhead();
    CHECK_EQ(e->edit.clips.size(), 3u);
    CHECK_NEAR(e->edit.captions.back().start, capAt, 1e-6);
    // b to the front: its caption comes along.
    const double aLen = e->edit.clips[0].Duration() + e->edit.clips[1].Duration();
    e->MoveClip(2, 0);
    CHECK(e->edit.clips[0].path == b);
    CHECK_NEAR(e->edit.captions.back().start, capAt - aLen, 1e-6);
    // Take the middle clip out into the bin, undo it.
    e->MoveToBin(1);
    CHECK_EQ(e->edit.clips.size(), 2u);
    CHECK_EQ(e->edit.bin.size(), 1u);
    CHECK_NEAR(e->duration, 3.5, 0.1);
    e->Undo();
    CHECK_EQ(e->edit.clips.size(), 3u);
    CHECK(e->edit.bin.empty());
    CHECK_NEAR(e->duration, 5, 0.1);
    // Playback runs on over a cut instead of stopping at it.
    e->Seek(1.4);
    e->Play();
    Pump(1500);
    test::Note("now " + std::to_string(e->Now()) + ", rawT " + std::to_string(e->rawT));
    CHECK(e->Now() > 2.3);
    CHECK(e->rawT > 2.1);
    e->Pause();
    // Paused frames come from the right clip: 0.5 s into b is red, in a square frame with black bars.
    e->Seek(0.5);
    Pump(800);
    CHECK(e->raw && e->raw->Width() == 640);
    if (e->raw) {
        const uint32_t mid = e->raw->Bits()[(size_t)180 * 640 + 320] & 0xFFFFFF, side = e->raw->Bits()[(size_t)180 * 640 + 20] & 0xFFFFFF;
        CHECK(((mid >> 16) & 255) > 200 && ((mid >> 8) & 255) < 60);
        CHECK((side & 0xF0F0F0) == 0);
    }
    e->Save(false);
    for (int i = 0; i < 150 && e->busy; ++i) Pump(100);
    test::Note("save error: " + ToUtf8(e->lastSaveError));
    CHECK(!e->busy && e->lastSaveError.empty());
    const auto files = RecentCaptures(caps, 10, true);
    CHECK_EQ(files.size(), 1u);
    VideoInfo vi;
    CHECK(!files.empty() && ProbeVideo(files[0], &vi) && std::fabs(vi.duration - 5) < 0.2 && vi.w == 640 && vi.hasAudio);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// The clip lane by mouse: click selects, dragging a clip reorders, dragging a selected clip's edge trims it.
ATHER_TEST(video_editor_clip_lane_mouse) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, false));
    CHECK(WriteTestClip(b, 640, 360, 30, 2, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->AddClips({b});
    e->SelectClip(std::nullopt);
    CHECK_EQ(e->edit.clips.size(), 2u);
    const RECT tr = e->TimelineRect();
    const int laneY = tr.top + e->LaneH() / 2;
    auto mid = [&](size_t i) { return (int)e->TX(ClipStart(e->edit.clips, i) + e->edit.clips[i].Duration() / 2); };
    // Drag b onto the left half of a: it goes first.
    const uint64_t bId = e->edit.clips[1].id;
    e->OnMouseDown({mid(1), laneY}, false);
    CHECK(e->selClip == bId);
    e->OnMouseMove({mid(1) - 40, laneY}, MK_LBUTTON);
    e->OnMouseMove({(int)e->TX(0.2), laneY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(e->edit.clips[0].id == bId);
    // Trim b's end by dragging its right edge left by half a second.
    const double before = e->edit.clips[0].out;
    const int edge = (int)e->TX(e->edit.clips[0].Duration());
    e->OnMouseDown({edge, laneY}, false);
    e->OnMouseMove({(int)e->TX(e->edit.clips[0].Duration() - 0.5), laneY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK_NEAR(e->edit.clips[0].out, before - 0.5, 0.05);
    CHECK_NEAR(e->duration, ClipsDuration(e->edit.clips), 1e-9);
    e->Undo();
    CHECK_NEAR(e->edit.clips[0].out, before, 1e-9);
    // Delete takes the selected clip out into the bin; the last one can't go. Delete in the bin deletes for good.
    e->OnKey(VK_DELETE);
    CHECK_EQ(e->edit.clips.size(), 1u);
    CHECK_EQ(e->edit.bin.size(), 1u);
    e->SelectClip(e->edit.clips[0].id);
    e->OnKey(VK_DELETE);
    CHECK_EQ(e->edit.clips.size(), 1u);
    CHECK(e->LaneH() > 0);  // the bin holds a piece: both lanes stay
    e->SelectClip(e->edit.bin[0].id);
    e->OnKey(VK_DELETE);
    CHECK(e->edit.bin.empty());
    CHECK_EQ(e->LaneH(), 0);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Overlay videos in the editor: one added shows in the preview once decoded; its bar moves on the bottom lane, its box
// on the video; a split takes half the frame; Del takes it into the bin (undo puts it back), and a bin piece dragged to
// the lane goes on top there.
ATHER_TEST(video_editor_overlay_videos) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, false));  // red, green, blue
    CHECK(WriteTestClip(b, 320, 180, 30, 3, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    const auto bc = ClipOf(b);
    CHECK(bc.has_value());
    if (!bc) return;
    e->AddOverlay(*bc, 0.5, OverlayLayout::Pip);
    CHECK(e->edit.overlays.size() == 1 && e->SelOverlay() == 0u);
    // The preview: at 1.3 the video is green and the overlay (0.8 into b) red.
    e->Seek(1.3);
    for (int i = 0; i < 60 && (e->overlayPics.empty() || std::fabs(e->overlayPicsT - e->rawT) > 1e-3 || std::fabs(e->rawT - 1.3) > 0.05); ++i) Pump(50);
    CHECK(!e->overlayPics.empty());
    const VRect box = e->OverlayOnVideo(e->edit.overlays[0]);
    if (e->shown) {
        const uint32_t in = e->shown->Pixel((int)box.MidX(), (int)box.MidY()), out = e->shown->Pixel(40, 40);
        CHECK(((in >> 16) & 255) > 150 && ((in >> 8) & 255) < 100);
        CHECK(((out >> 8) & 255) > 150 && ((out >> 16) & 255) < 100);
    }
    // Copy frame draws it too, waiting for its picture at that frame: at 2.2 the video is blue and the overlay green.
    e->Seek(2.2);
    e->lastCopiedFrame.reset();
    e->CopyFrame();  // right away: neither the frame nor the overlay's picture is decoded yet
    for (int i = 0; i < 300 && !e->lastCopiedFrame; ++i) Pump(10);
    CHECK(e->lastCopiedFrame != nullptr);
    if (e->lastCopiedFrame) {
        const uint32_t in = e->lastCopiedFrame->Pixel((int)box.MidX(), (int)box.MidY()), out = e->lastCopiedFrame->Pixel(40, 40);
        CHECK(((in >> 8) & 255) > 150 && ((in >> 16) & 255) < 100 && (in & 255) < 100);
        CHECK((out & 255) > 150 && ((out >> 8) & 255) < 100);
    }
    // Its bar on the bottom lane moves 0.3 s later.
    const RECT ol = e->OverlayLaneRect();
    const int laneY = (ol.top + ol.bottom) / 2;
    e->OnMouseDown({(int)e->TX(1.5), laneY}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::OverlayItem);
    e->OnMouseMove({(int)e->TX(1.8), laneY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK_NEAR(e->edit.overlays[0].start, 0.8, 0.06);
    // Its box moves left when dragged on the video.
    e->Seek(1.3);
    const VPoint mid{e->OverlayOnVideo(e->edit.overlays[0]).MidX(), e->OverlayOnVideo(e->edit.overlays[0]).MidY()};
    const gp::PointF at = e->ToView(mid);
    const double x0 = e->edit.overlays[0].pos.x;
    e->OnMouseDown({(LONG)at.X, (LONG)at.Y}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::OverlayBox);
    e->OnMouseMove({(LONG)at.X - 60, (LONG)at.Y}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(e->edit.overlays[0].pos.x < x0 - 0.05);
    // Its top-left corner sizes it, the bottom-right corner staying put; even with Crop on.
    const VRect before = e->OverlayOnVideo(e->edit.overlays[0]);
    const gp::PointF tl = e->ToView(VPoint{before.x, before.y});
    e->cropping = true;
    e->OnMouseDown({(LONG)tl.X, (LONG)tl.Y}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::OverlayBox && e->drag.handle == 1 && !e->cropping);
    e->OnMouseMove({(LONG)tl.X - 40, (LONG)tl.Y - 40}, MK_LBUTTON);
    e->OnMouseUp();
    const VRect bigger = e->OverlayOnVideo(e->edit.overlays[0]);
    CHECK(bigger.w > before.w + 20);
    CHECK_NEAR(bigger.MaxX(), before.MaxX(), 2);
    CHECK_NEAR(bigger.MaxY(), before.MaxY(), 2);
    CHECK_NEAR(bigger.h / bigger.w, 180.0 / 320, 0.02);  // the video's shape
    CHECK(!e->edit.crop.has_value());
    // The wheel over it: 10% wider a notch, about its middle.
    const gp::PointF c = e->ToView(VPoint{bigger.MidX(), bigger.MidY()});
    e->OnWheel({(LONG)c.X, (LONG)c.Y}, WHEEL_DELTA, 0);
    const VRect wheeled = e->OverlayOnVideo(e->edit.overlays[0]);
    CHECK_NEAR(wheeled.w, bigger.w * 1.1, 2);
    CHECK_NEAR(wheeled.MidX(), bigger.MidX(), 2);
    // A file dropped on the bottom lane goes on top from there.
    e->DropFiles({b}, {(int)e->TX(2.0), laneY});
    CHECK_EQ(e->edit.overlays.size(), 2u);
    if (e->edit.overlays.size() == 2) CHECK_NEAR(e->edit.overlays[1].start, 2.0, 0.05);
    e->Undo();
    CHECK_EQ(e->edit.overlays.size(), 1u);
    e->Select(e->edit.overlays[0].id);
    // Side by side: it takes the left half.
    e->UpdateOverlay([](Overlay& o) { o.layout = OverlayLayout::SplitLeft; });
    CHECK_NEAR(e->OverlayOnVideo(e->edit.overlays[0]).w, 320, 1);
    // Del: into the bin; undo puts it back.
    CHECK(e->SelOverlay() == 0u);
    e->OnKey(VK_DELETE);
    CHECK(e->edit.overlays.empty() && e->edit.bin.size() == 1);
    e->Undo();
    CHECK(e->edit.overlays.size() == 1 && e->edit.bin.empty());
    // From the bin onto the bottom lane: on top from where it's dropped.
    e->Select(e->edit.overlays[0].id);
    e->OnKey(VK_DELETE);
    const RECT chip = e->BinChips()[0];
    e->OnMouseDown({(chip.left + chip.right) / 2, (chip.top + chip.bottom) / 2}, false);
    e->OnMouseMove({(chip.left + chip.right) / 2 + 30, chip.bottom + 30}, MK_LBUTTON);
    e->OnMouseMove({(int)e->TX(1.0), laneY}, MK_LBUTTON);
    CHECK(e->drag.toOverlay);
    e->OnMouseUp();
    CHECK(e->edit.overlays.size() == 1 && e->edit.bin.empty());
    if (!e->edit.overlays.empty()) CHECK_NEAR(e->edit.overlays[0].start, 1.0, 0.05);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// An overlay shorter than the 0.2 s a trim leaves (a 0.15 s split piece) keeps its footage when its ends are dragged:
// its in point never goes before the file's start, nor its out point past the file's end.
ATHER_TEST(video_editor_short_overlay_trims_stay_in_its_footage) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, false));
    CHECK(WriteTestClip(b, 320, 180, 30, 3, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    const auto bc = ClipOf(b);
    CHECK(bc.has_value());
    if (!bc) return;
    const RECT ol = e->OverlayLaneRect();
    const int laneY = (ol.top + ol.bottom) / 2;
    auto trim = [&](double in, double out, bool left, double to) {
        Clip c = *bc;
        c.in = in;
        c.out = out;
        e->edit.overlays.clear();
        e->AddOverlay(c, 1.0, OverlayLayout::Pip);
        const Overlay& o = e->edit.overlays[0];
        const int x = left ? (int)e->TX(o.start) + 1 : (int)e->TX(o.End()) - 1;
        e->OnMouseDown({x, laneY}, false);
        CHECK(e->drag.kind == VideoEditor::DragKind::OverlayItem && e->drag.edge == (left ? -1 : 1));
        e->OnMouseMove({(int)e->TX(to), laneY}, MK_LBUTTON);
        e->OnMouseUp();
        return e->edit.overlays[0];
    };
    const Overlay first = trim(0, 0.15, true, 1.5);  // the file's first 0.15 s: its left edge dragged right
    CHECK(first.clip.in >= 0 && first.start >= 1.0 - 1e-9);
    CHECK_NEAR(first.clip.Duration(), 0.15, 1e-6);
    const Overlay last = trim(2.85, 3.0, false, 1.05);  // the file's last 0.15 s: its right edge dragged left
    CHECK(last.clip.out <= last.clip.length + 1e-9);
    CHECK_NEAR(last.clip.Duration(), 0.15, 1e-6);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// The bin by mouse and keys: Del and dragging a clip up take it out; dragging a piece down puts it back where it's
// dropped; a double-click puts it back at the end; Del in the bin deletes it for good; undo brings each step back.
ATHER_TEST(video_editor_clip_bin) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, false));
    CHECK(WriteTestClip(b, 640, 360, 30, 2, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->BinH(), 0);
    e->AddClips({b});
    e->Seek(1.5);
    e->SplitAtPlayhead();  // [a1, a2, b], a2 selected
    CHECK_EQ(e->edit.clips.size(), 3u);
    CHECK(e->BinH() > 0);
    const uint64_t a2 = e->edit.clips[1].id;
    CHECK(e->selClip == a2);
    // Del: a2 goes to the bin; the video is shorter. Undo puts it back, Del again takes it out.
    e->OnKey(VK_DELETE);
    CHECK_EQ(e->edit.clips.size(), 2u);
    CHECK(e->edit.bin.size() == 1 && e->edit.bin[0].id == a2);
    CHECK_NEAR(e->duration, 3.5, 0.1);
    e->Undo();
    CHECK(e->edit.clips.size() == 3 && e->edit.bin.empty());
    e->SelectClip(a2);
    e->OnKey(VK_DELETE);
    CHECK_EQ(e->edit.bin.size(), 1u);
    // Drag it from the bin to the start of the timeline.
    auto chipMid = [&](size_t i) {
        const RECT r = e->BinChips()[i];
        return POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    };
    const RECT tr = e->TimelineRect();
    const int laneY = tr.top + e->LaneH() / 2;
    POINT c = chipMid(0);
    e->OnMouseDown(c, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::BinMove && e->SelBinIndex() == 0u);
    e->OnMouseMove({c.x + 30, c.y + 20}, MK_LBUTTON);
    e->OnMouseMove({(int)e->TX(0.1), laneY}, MK_LBUTTON);
    CHECK_EQ(e->drag.target, 0);
    e->OnMouseUp();
    CHECK(e->edit.clips.size() == 3 && e->edit.clips[0].id == a2 && e->edit.bin.empty());
    CHECK(e->selClip == a2 && e->SelClipIndex() == 0u);
    CHECK_NEAR(e->duration, 5, 0.1);
    e->Undo();
    CHECK(e->edit.clips.size() == 2 && e->edit.bin.size() == 1);
    // Drag b (the last clip) up onto the bin.
    const uint64_t bId = e->edit.clips[1].id;
    const int bMid = (int)e->TX(ClipStart(e->edit.clips, 1) + e->edit.clips[1].Duration() / 2);
    const RECT br = e->BinRect();
    e->OnMouseDown({bMid, laneY}, false);
    e->OnMouseMove({bMid, laneY - 30}, MK_LBUTTON);
    e->OnMouseMove({bMid, (br.top + br.bottom) / 2}, MK_LBUTTON);
    CHECK(e->drag.toBin);
    e->OnMouseUp();
    CHECK(e->edit.clips.size() == 1 && e->edit.bin.size() == 2 && e->edit.bin[1].id == bId);
    // A click on a piece selects it without moving anything; a double-click puts it back at the end.
    c = chipMid(1);
    e->OnMouseDown(c, false);
    e->OnMouseUp();
    CHECK(e->SelBinIndex() == 1u && e->edit.bin.size() == 2);
    e->OnMouseDown(c, true);
    e->OnMouseUp();
    CHECK(e->edit.clips.size() == 2 && e->edit.clips[1].id == bId && e->edit.bin.size() == 1);
    // Del in the bin deletes for good; undo brings it back.
    e->SelectClip(e->edit.bin[0].id);
    e->OnKey(VK_DELETE);
    CHECK(e->edit.bin.empty() && e->edit.clips.size() == 2);
    e->Undo();
    CHECK(e->edit.bin.size() == 1 && e->edit.bin[0].id == a2);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// The lane's fiddly parts: a selected clip's left edge trims (not its neighbour), the playhead knob scrubs over the
// lane, keys can't change the clips mid-drag, and a click on a very short clip's edge leaves it alone.
ATHER_TEST(video_editor_clip_lane_edges_knob_and_keys) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, false));
    CHECK(WriteTestClip(b, 640, 360, 30, 2, false));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->AddClips({b});  // [a, b], b selected, playhead at the cut
    CHECK(e->edit.clips.size() == 2 && e->selClip == e->edit.clips[1].id);
    const RECT tr = e->TimelineRect();
    const int laneY = tr.top + e->LaneH() / 2;
    const uint64_t bId = e->edit.clips[1].id;
    // b's left edge sits right at the end of a: grabbing it trims b.
    const int edge = (int)e->TX(ClipStart(e->edit.clips, 1));
    e->OnMouseDown({edge, laneY}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::ClipIn);
    e->OnMouseMove({(int)e->TX(ClipStart(e->edit.clips, 1) + 0.5), laneY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(e->edit.clips[0].path == a && e->edit.clips[1].id == bId);  // not reordered
    CHECK_NEAR(e->edit.clips[1].in, 0.5, 0.06);
    // The knob scrubs, even over the lane.
    e->Seek(1.0);
    e->OnMouseDown({(int)e->TX(1.0), tr.top}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::Playhead);
    e->OnMouseMove({(int)e->TX(2.0), tr.top}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK_NEAR(e->Now(), 2.0, 0.05);
    CHECK(e->edit.clips[1].id == bId);
    // Mid-drag, Delete and Ctrl+Z do nothing (Esc cancels the drag).
    e->OnMouseDown({(int)e->TX(0.5), laneY}, false);
    e->OnMouseMove({(int)e->TX(0.5) + 40, laneY}, MK_LBUTTON);
    e->OnKey(VK_DELETE);
    CHECK_EQ(e->edit.clips.size(), 2u);
    e->OnKey(VK_ESCAPE);
    CHECK(e->drag.kind == VideoEditor::DragKind::None);
    e->OnMouseUp();
    CHECK_EQ(e->edit.clips.size(), 2u);
    // A 0.15 s piece at the end of b: clicking its right edge without moving changes nothing.
    const double end = e->duration;
    e->Seek(end - 0.15);
    e->SplitAtPlayhead();
    CHECK_EQ(e->edit.clips.size(), 3u);
    const Clip tail = e->edit.clips[2];
    const size_t undoBefore = e->undoStack.size();
    const int right = (int)e->TX(e->duration) - 1;
    e->OnMouseDown({right, laneY}, false);
    e->OnMouseUp();
    CHECK(e->edit.clips[2] == tail);
    CHECK(e->edit.clips[2].out <= e->edit.clips[2].length + 1e-9);
    CHECK_EQ(e->undoStack.size(), undoBefore);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// ---- copy (Ctrl+C): the marked range, the selected clip, the frame on screen ----

namespace {

// What the clipboard holds now (these tests use the real one).
std::vector<std::wstring> ClipboardFiles() {
    std::vector<std::wstring> v;
    if (!OpenClipboard(nullptr)) return v;
    if (HANDLE h = GetClipboardData(CF_HDROP)) v = DroppedFiles(h);
    CloseClipboard();
    return v;
}
std::wstring ClipboardText() {
    std::wstring t;
    if (!OpenClipboard(nullptr)) return t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) t = p;
        GlobalUnlock(h);
    }
    CloseClipboard();
    return t;
}
// The CF_DIB as top-down pixels (32-bit, as CopyImageToClipboard puts it there).
BitmapPtr ClipboardDib() {
    BitmapPtr out;
    if (!OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_DIB)) {
        const auto* bih = static_cast<const BITMAPINFOHEADER*>(GlobalLock(h));
        if (bih && bih->biBitCount == 32 && bih->biCompression == BI_RGB && (out = Bitmap::Create(bih->biWidth, std::abs(bih->biHeight)))) {
            const int w = out->Width(), rows = out->Height();
            const auto* px = reinterpret_cast<const uint32_t*>(reinterpret_cast<const BYTE*>(bih) + bih->biSize);
            for (int y = 0; y < rows; ++y) memcpy(out->Bits() + (size_t)y * w, px + (size_t)(bih->biHeight > 0 ? rows - 1 - y : y) * w, (size_t)w * 4);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out;
}
// The "PNG", decoded.
BitmapPtr ClipboardPng() {
    std::string bytes;
    if (!OpenClipboard(nullptr)) return nullptr;
    if (HANDLE h = GetClipboardData(RegisterClipboardFormatW(L"PNG"))) {
        if (const auto* p = static_cast<const char*>(GlobalLock(h))) bytes.assign(p, GlobalSize(h));
        GlobalUnlock(h);
    }
    CloseClipboard();
    const std::wstring file = test::TempDir() + L"\\clipboard.png";
    return !bytes.empty() && WriteFileUtf8(file, bytes) ? LoadImageFile(file) : nullptr;
}
void EmptyTheClipboard() {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    CloseClipboard();
}
// Pixels that differ between two pictures (color only), or -1 when their sizes do.
int PixelsDiffer(const Bitmap& a, const Bitmap& b) {
    if (a.Width() != b.Width() || a.Height() != b.Height()) return -1;
    int n = 0;
    for (size_t i = 0, k = (size_t)a.Width() * a.Height(); i < k; ++i) n += (a.Bits()[i] & 0xFFFFFF) != (b.Bits()[i] & 0xFFFFFF);
    return n;
}
// ShownFrame, allowing for a slow first decode (a runner without a GPU can take over two seconds for it).
std::pair<int, bool> FrameOnScreen(VideoEditor* e) {
    auto r = ShownFrame(e);
    for (int i = 0; i < 3 && r.first < 0; ++i) r = ShownFrame(e);
    return r;
}
int MiddleNumber(const Bitmap& f) { return NumberOf(f.Bits()[(size_t)(f.Height() / 2) * f.Width() + f.Width() / 2]); }
// Every frame of an MP4, in order, by the number painted on it (WriteNumberedClip), and whether it has blue.
std::vector<std::pair<int, bool>> FrameNumbers(const std::wstring& path) {
    std::vector<std::pair<int, bool>> v;
    VideoReader r;
    if (!r.Open(path)) return v;
    BitmapPtr f;
    double t = 0;
    while (r.Read(&f, &t)) {
        const uint32_t c = f->Bits()[(size_t)(f->Height() / 2) * f->Width() + f->Width() / 2];
        v.push_back({NumberOf(c), BlueOf(c)});
    }
    return v;
}
// An MP4 with H.264 video and AAC sound has their sample entries ('avc1', 'mp4a').
bool H264AndAac(const std::wstring& path) {
    std::string bytes;
    return ReadFileUtf8(path, &bytes) && bytes.find("avc1") != std::string::npos && bytes.find("mp4a") != std::string::npos;
}

}  // namespace

// The marked range: Shift+drag on the timeline marks one on frame boundaries (a press a third of a frame off still lands
// on that frame's edge) at two zooms and either way round; it stays inside the trim; its grips drag (showing the frame at
// that edge); Shift+I / Shift+O set its ends at the playhead; Esc (before its other meanings), a plain click outside it
// and a change to the clips clear it, and Esc mid-drag puts it back. The edit and the undo stack never change. It's drawn
// as a lime band with white grips and its label.
ATHER_TEST(video_editor_marks_a_range_to_copy) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4", b = dir + L"\\b60.mp4";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 240, 0));  // 4 s
    CHECK(WriteNumberedClip(b, 320, 180, 60, 60, 255));
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(e->FramesKnown() && e->tframes.size() >= 230);
    if (e->tframes.size() < 230) return DestroyWindow(e->hwnd), void();
    auto T = [&](size_t n) { return e->tframes.frames[n].t; };
    auto is = [&](size_t n0, size_t n1) { return e->marked && std::fabs(e->marked->start - T(n0)) < 1e-9 && std::fabs(e->marked->end - T(n1)) < 1e-9; };
    auto X = [&](double t) { return (int)std::lround(e->TX(t)); };
    e->edit.trimStart = T(20);
    e->edit.trimEnd = T(220);
    e->Changed();
    const VideoEdit before = e->edit;
    const size_t undoBefore = e->undoStack.size();
    const RECT tr = e->TimelineRect();
    const int top = tr.top + e->LaneH(), stripY = top + e->S(30), laneY = top + e->S(73);  // the thumbnails; the caption lane
    auto shiftDrag = [&](int x0, int x1) {
        e->OnMouseDown({x0, stripY}, false, true);
        e->OnMouseMove({(x0 + x1) / 2, stripY}, MK_LBUTTON | MK_SHIFT);
        e->OnMouseMove({x1, stripY}, MK_LBUTTON | MK_SHIFT);
        e->OnMouseUp();
    };
    for (double zoom : {1.0, 8.0}) {
        e->tlZoom = 1;
        e->tlStart = 0;
        e->GoToFrame(120);
        if (zoom > 1) e->ZoomTimeline(zoom);
        const double pf = e->PxPerFrame();
        test::Note("zoom " + std::to_string(e->tlZoom) + ": " + std::to_string(pf) + " px a frame");
        // Pixel to time and back: every frame on view, from its x, snaps to its own edge.
        int wrong = 0;
        for (size_t n = 1; n + 1 < e->tframes.size(); ++n)
            if (const int x = X(T(n)); x > tr.left && x < tr.right) wrong += std::fabs(e->SnapEdge(e->TT(x)) - T(n)) > 1e-9;
        CHECK_EQ(wrong, 0);
        const int off = std::max(1, (int)std::floor(pf / 3));
        shiftDrag(X(T(110)) + off, X(T(130)) - off);
        CHECK(is(110, 130));
        shiftDrag(X(T(130)) + off, X(T(110)) - off);  // right to left
        CHECK(is(110, 130));
        if (zoom > 1) {  // more than half a frame on: the next frame's edge
            shiftDrag(X(T(110)) + (int)std::ceil(pf * 0.7), X(T(130)) - off);
            CHECK(is(111, 130));
        }
        CHECK(e->drag.kind == VideoEditor::DragKind::None);
    }
    // Inside the trim: a drag over the whole timeline stops at the trim's ends.
    e->tlZoom = 1;
    e->tlStart = 0;
    shiftDrag(tr.left + 1, tr.right - 1);
    CHECK(is(20, 220));
    // The grips drag without Shift: the end to frame 150 (the editor shows the range's last frame meanwhile), then the
    // start to frame 60.
    shiftDrag(X(T(110)), X(T(130)));
    e->OnMouseDown({X(e->marked ? e->marked->end : 0) + 1, stripY}, false);
    CHECK(e->drag.kind == VideoEditor::DragKind::Range);
    e->OnMouseMove({X(T(150)), stripY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(is(110, 150));
    CHECK_EQ(e->FrameNow(), 149u);
    e->OnMouseDown({X(e->marked ? e->marked->start : 0) - 1, stripY}, false);
    e->OnMouseMove({X(T(60)), stripY}, MK_LBUTTON);
    e->OnMouseUp();
    CHECK(is(60, 150));
    CHECK_EQ(e->FrameNow(), 60u);
    // Shift+I / Shift+O at the playhead; the trim stays.
    e->GoToFrame(40);
    e->Key('I', {false, true, false});
    CHECK(is(40, 150));
    e->GoToFrame(90);
    e->Key('O', {false, true, false});
    CHECK(is(40, 90));
    e->GoToFrame(25);
    e->Key('O', {false, true, false});  // before its start: from the trim's start
    CHECK(is(20, 25));
    CHECK(e->edit.trimStart == before.trimStart && e->edit.trimEnd == before.trimEnd);
    // Esc clears it first; then Esc goes on to its other meanings (here: crop mode off).
    e->cropping = true;
    e->Key(VK_ESCAPE, {});
    CHECK(!e->marked && e->cropping);
    e->Key(VK_ESCAPE, {});
    CHECK(!e->cropping);
    // No range yet: Shift+I runs it to the trim's end.
    e->GoToFrame(50);
    e->Key('I', {false, true, false});
    CHECK(is(50, 220));
    // A plain click inside it moves the playhead and keeps it; outside it, it goes (on any lane).
    shiftDrag(X(T(60)), X(T(120)));
    e->OnMouseDown({X(T(90)) + 1, stripY}, false);
    e->OnMouseUp();
    CHECK(is(60, 120));
    e->OnMouseDown({X(T(150)), laneY}, false);
    e->OnMouseUp();
    CHECK(!e->marked);
    // Esc mid-drag puts it back as it was.
    shiftDrag(X(T(60)), X(T(120)));
    e->OnMouseDown({X(T(120)) + 1, stripY}, false);
    e->OnMouseMove({X(T(170)), stripY}, MK_LBUTTON);
    CHECK(is(60, 170));
    e->Key(VK_ESCAPE, {});
    e->OnMouseUp();
    CHECK(is(60, 120));
    CHECK(e->MarkedLabel() == L"1.0 s · Ctrl+C copies");
    // A selection, not an edit.
    CHECK(e->edit == before);
    CHECK_EQ(e->undoStack.size(), undoBefore);
    // Drawn: the caption lane turns lime inside the band only; a white grip on its end.
    e->GoToFrame(180);  // the playhead's line out of the way
    const auto snap = Snapshot(e);
    auto px = [&](int x, int y) { return snap->Bits()[(size_t)y * snap->Width() + x] & 0xFFFFFF; };
    const int capMid = top + e->S(62) + e->S(11);
    const uint32_t in = px(X(T(75)), capMid), out = px(X(T(200)), capMid), grip = px(X(T(120)), top + e->S(20));
    auto G = [](uint32_t c) { return (int)((c >> 8) & 255); };
    auto B = [](uint32_t c) { return (int)(c & 255); };
    test::Note("band " + std::to_string(in) + ", outside " + std::to_string(out) + ", grip " + std::to_string(grip));
    CHECK(G(in) - B(in) > 25 && std::abs(G(out) - B(out)) < 10);
    CHECK(G(grip) > 200 && B(grip) > 200 && (int)((grip >> 16) & 255) > 200);
    // A change to the clips clears it.
    e->AddClips({b});
    CHECK(!e->marked);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Copy clip: Ctrl+C with a marked range saves that part of the edit as an MP4 in the captures folder (H.264 + AAC, the
// range's length, the range's frames in order: compared with the frames the editor shows at its ends) and puts that file,
// and only it, on the clipboard (CF_HDROP). A second Ctrl+C while it saves is ignored; the edit and the range stay. With
// the clipboard held by another program the file is still saved, just not on the clipboard.
ATHER_TEST(video_editor_copies_the_marked_range_as_a_clip) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\shot.mp4", caps = dir + L"\\caps";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 240, 0, true));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(e->tframes.size() >= 200);
    if (e->tframes.size() < 200) return DestroyWindow(e->hwnd), void();
    const VideoEdit before = e->edit;
    e->GoToFrame(159);
    const auto last = FrameOnScreen(e);
    e->GoToFrame(40);
    const auto first = FrameOnScreen(e);
    e->Key('I', {false, true, false});
    e->GoToFrame(160);
    e->Key('O', {false, true, false});
    CHECK(e->marked && std::fabs(e->marked->end - e->marked->start - (e->tframes.frames[160].t - e->tframes.frames[40].t)) < 1e-9);
    const double length = e->marked ? e->marked->end - e->marked->start : 0;
    EmptyTheClipboard();
    e->Key('C', {true, false, false});
    CHECK(e->busy);
    e->Key('C', {true, false, false});  // ignored while it saves
    for (int i = 0; i < 300 && e->busy; ++i) Pump(100);
    test::Note("copy error: " + ToUtf8(e->lastSaveError) + " → " + ToUtf8(e->lastCopiedClip));
    CHECK(!e->busy && e->lastSaveError.empty());
    const std::wstring out = e->lastCopiedClip;
    const auto files = RecentCaptures(caps, 10, true);
    CHECK(files.size() == 1 && _wcsicmp(files[0].c_str(), out.c_str()) == 0);
    CHECK(e->lastClipOnClipboard);
    const auto copied = ClipboardFiles();
    CHECK(copied.size() == 1 && _wcsicmp(copied[0].c_str(), out.c_str()) == 0);
    VideoInfo vi;
    CHECK(ProbeVideo(out, &vi) && vi.hasAudio && vi.w == 320 && vi.h == 180);
    CHECK(H264AndAac(out));
    // As long as the range: its video from the first frame's start to the last one's end (the file as a whole may run
    // on by the sound's last AAC packet).
    const std::vector<double> times = FrameTimes(out);
    const double video = times.empty() ? 0 : times.back() - times.front() + 1 / 60.0;
    test::Note("video " + std::to_string(video) + " s, file " + std::to_string(vi.duration) + " s, for a range of " + std::to_string(length) + " s");
    CHECK(std::fabs(video - length) <= 1 / 60.0 + 1e-6);
    CHECK(std::fabs(vi.duration - length) < 0.05);
    const auto frames = FrameNumbers(out);
    test::Note(std::to_string(frames.size()) + " frames, " + (frames.empty() ? std::string("none") : std::to_string(frames.front().first) + "…" + std::to_string(frames.back().first)) +
               "; the editor shows " + std::to_string(first.first) + "…" + std::to_string(last.first));
    CHECK_EQ(frames.size(), 120u);
    CHECK(!frames.empty() && frames.front() == first && frames.back() == last);
    CHECK(e->edit == before);
    CHECK(e->marked.has_value());
    // The clipboard held by another program: saved all the same, not on the clipboard, and nothing else goes wrong.
    std::atomic<int> held{0};
    std::thread holder([&] {
        if (!OpenClipboard(nullptr)) return held.store(-1);
        held = 1;
        while (held == 1) Sleep(5);
        CloseClipboard();
    });
    while (held == 0) Sleep(1);
    CHECK_EQ(held.load(), 1);
    e->Key('C', {true, false, false});
    for (int i = 0; i < 300 && e->busy; ++i) Pump(100);
    if (held == 1) held = 2;
    holder.join();
    CHECK(!e->busy && e->lastSaveError.empty() && !e->lastClipOnClipboard);
    CHECK_EQ(RecentCaptures(caps, 10, true).size(), 2u);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Copy clip with no range: the clip selected in the clip lane, as edited (from its in point, to the trim's end inside
// it): its frames, as the editor shows them, and nothing of the clip before it.
ATHER_TEST(video_editor_copies_the_selected_clip) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a.mp4", b = dir + L"\\b.mp4", caps = dir + L"\\caps";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0, true));
    CHECK(WriteNumberedClip(b, 320, 180, 60, 120, 255, true));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    e->AddClips({b});
    e->TrimClip(1, 0.5, std::nullopt);  // b from half a second in
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    e->edit.trimEnd = e->duration - 0.5;  // and the trim ends half a second before its end
    e->Changed();
    CHECK(e->edit.clips.size() == 2 && e->FramesKnown());
    if (e->edit.clips.size() != 2) return DestroyWindow(e->hwnd), void();
    e->SelectClip(e->edit.clips[1].id);
    CHECK(!e->marked && e->CanCopyClip());
    const size_t f0 = e->tframes.clipFirst[1], f1 = e->tframes.At(e->edit.trimEnd - 1e-3);
    e->GoToFrame(f1);
    const auto last = FrameOnScreen(e);
    e->GoToFrame(f0);
    const auto first = FrameOnScreen(e);
    CHECK(first.second && last.second);  // both from b
    e->Key('C', {true, false, false});
    for (int i = 0; i < 300 && e->busy; ++i) Pump(100);
    test::Note("copy error: " + ToUtf8(e->lastSaveError));
    CHECK(!e->busy && e->lastSaveError.empty());
    const auto frames = FrameNumbers(e->lastCopiedClip);
    test::Note(std::to_string(frames.size()) + " frames for timeline frames " + std::to_string(f0) + "…" + std::to_string(f1));
    CHECK_EQ(frames.size(), f1 - f0 + 1);
    CHECK(!frames.empty() && frames.front() == first && frames.back() == last);
    CHECK(std::all_of(frames.begin(), frames.end(), [](const auto& f) { return f.second; }));  // none of a
    const auto copied = ClipboardFiles();
    CHECK(copied.size() == 1 && _wcsicmp(copied[0].c_str(), e->lastCopiedClip.c_str()) == 0);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

// Copy frame: with nothing picked Ctrl+C, and Ctrl+Shift+C at any time (also with a range marked), put the frame on
// screen on the clipboard as CF_DIB and PNG, pixel for pixel FrameRenderer's frame for the export (here cropped, with a
// caption and a box on it) at the export's size, and the very frame an export starting there renders. Nothing is saved.
// Save ▾ lists Copy clip and Copy frame. A text field with focus keeps Ctrl+C for its own text.
ATHER_TEST(video_editor_copies_the_frame_on_screen) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"\\a60.mp4", caps = dir + L"\\caps";
    CHECK(WriteNumberedClip(a, 320, 180, 60, 120, 0));
    const CapturesFolderForTest folder(caps);
    VideoEditor* e = OpenHidden(a, 1180, 760);
    CHECK(e != nullptr);
    if (!e) return;
    for (int i = 0; i < 300 && !e->FramesKnown(); ++i) Pump(10);
    CHECK(e->tframes.size() >= 100);
    if (e->tframes.size() < 100) return DestroyWindow(e->hwnd), void();
    e->GoToFrame(60);
    FrameOnScreen(e);
    e->AddCaption();
    e->edit.captions.back().text = L"Copied frame";
    const uint64_t caption = e->edit.captions.back().id;
    e->AddMark(MarkKind::Box);
    e->edit.crop = VRect{40, 20, 240, 140};
    e->Select(std::nullopt);
    e->Changed();
    SetFocus(e->hwnd);
    auto copyFrame = [&](size_t n, bool shift) {
        e->GoToFrame(n);  // pressed right away: it waits for that frame
        e->lastCopiedFrame.reset();
        EmptyTheClipboard();
        e->Key('C', {true, shift, false});
        for (int i = 0; i < 300 && !e->lastCopiedFrame; ++i) Pump(10);
        return e->lastCopiedFrame;
    };
    auto check = [&](size_t n) {
        CHECK_EQ(e->FrameNow(), n);
        CHECK(e->raw && e->tframes.Nearest(e->rawT) == n);
        if (!e->raw) return;
        const BitmapPtr want = FrameRenderer(e->edit, e->videoSize, false).Render(*e->raw, e->paused);
        CHECK(want->Width() == 240 && want->Height() == 140);  // the export's size: the crop
        const BitmapPtr dib = ClipboardDib(), png = ClipboardPng();
        CHECK(dib && PixelsDiffer(*dib, *want) == 0);
        CHECK(png && PixelsDiffer(*png, *want) == 0);
        if (!dib) return;
        CHECK_EQ(MiddleNumber(*dib), FrameOnScreen(e).first);  // the frame on screen
        VideoEdit plain = e->edit;
        plain.captions.clear();
        plain.marks.clear();
        const int drawn = PixelsDiffer(*dib, *FrameRenderer(plain, e->videoSize, false).Render(*e->raw, e->paused));
        test::Note("caption and box: " + std::to_string(drawn) + " pixels");
        CHECK(drawn > 300);
    };
    CHECK(copyFrame(75, false));
    check(75);
    // The frame an export starting at that frame renders (before encoding) is that same picture.
    {
        VideoEdit one = e->edit;
        one.trimStart = e->tframes.frames[75].t;
        one.trimEnd = one.trimStart + 0.05;
        std::mutex mu;
        BitmapPtr exported;
        g_exportTap = [&](int i, const Bitmap& f) {
            std::lock_guard l(mu);
            if (i != 0 || !(exported = Bitmap::Create(f.Width(), f.Height()))) return;
            memcpy(exported->Bits(), f.Bits(), (size_t)f.Width() * f.Height() * 4);
        };
        std::wstring err;
        CHECK(ExportMp4(a, one, dir + L"\\one.mp4", &err));
        g_exportTap = nullptr;
        const int d = exported && e->lastCopiedFrame ? PixelsDiffer(*exported, *e->lastCopiedFrame) : -2;
        test::Note("export's frame vs the copy: " + std::to_string(d) + " pixels differ");
        CHECK_EQ(d, 0);
    }
    // With a range marked, Ctrl+Shift+C still copies the frame (no clip is made).
    e->GoToFrame(30);
    e->Key('I', {false, true, false});
    e->GoToFrame(90);
    e->Key('O', {false, true, false});
    CHECK(e->marked.has_value());
    CHECK(copyFrame(80, true));
    check(80);
    CHECK(!e->busy && RecentCaptures(caps, 10, true).empty());
    // Save ▾: Copy clip (with a range or a selected clip) and Copy frame.
    auto item = [&](const std::wstring& label) {
        const auto menu = e->SaveMenu();
        const auto it = std::find_if(menu.begin(), menu.end(), [&](const MenuItem& m) { return m.label == label; });
        return it == menu.end() ? -1 : (int)it->enabled;
    };
    CHECK_EQ(item(L"Copy clip\tCtrl+C"), 1);
    CHECK_EQ(item(L"Copy frame\tCtrl+Shift+C"), 1);
    e->Key(VK_ESCAPE, {});
    CHECK_EQ(item(L"Copy clip\tCtrl+C"), 0);
    // The caption's field has focus: Ctrl+C there copies its text, not a frame.
    e->Select(caption);
    e->FocusField();
    SetWindowTextW(e->field1, L"Copied frame");
    SendMessageW(e->field1, EM_SETSEL, 0, -1);
    EmptyTheClipboard();
    e->lastCopiedFrame.reset();
    BYTE keys[256];
    GetKeyboardState(keys);
    const BYTE ctrl = keys[VK_CONTROL];
    keys[VK_CONTROL] |= 0x80;
    SetKeyboardState(keys);
    SendMessageW(e->field1, WM_KEYDOWN, 'C', 0);
    SendMessageW(e->field1, WM_CHAR, 3, 0);  // Ctrl+C as TranslateMessage hands it to the field
    keys[VK_CONTROL] = ctrl;
    SetKeyboardState(keys);
    Pump(100);
    CHECK(ClipboardText() == L"Copied frame");
    CHECK(!e->lastCopiedFrame && !IsClipboardFormatAvailable(CF_DIB));
    CHECK(!e->busy && RecentCaptures(caps, 10, true).empty());
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

}  // namespace ather
