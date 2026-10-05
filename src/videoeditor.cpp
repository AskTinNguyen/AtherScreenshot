#include "videoeditor.h"

#include <dwmapi.h>
#include <mfmediaengine.h>
#include <objidl.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "annot.h"
#include "json.h"
#include "library.h"
#include "media.h"
#include "output.h"
#include "selftest.h"
#include "toast.h"
#include "videoedit.h"
#include "videoio.h"

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotVideoEditor";
constexpr wchar_t kIconFace[] = L"Segoe Fluent Icons";
constexpr UINT WM_ENGINE = WM_APP + 40, WM_THUMBS = WM_APP + 41, WM_TRANSCRIBED = WM_APP + 42, WM_SAVED = WM_APP + 43, WM_FETCHED = WM_APP + 44;
enum : UINT_PTR { kTimerFrame = 1 };
enum : int { kField1 = 200, kField2 };

const wchar_t* const kQuickEmoji[] = {L"✅", L"❌", L"⚠️", L"\U0001F449", L"\U0001F440", L"\U0001F4A1",
                                      L"\U0001F389", L"\U0001F525", L"⭐", L"❤️", L"\U0001F44D", L"\U0001F914"};
const wchar_t* const kAspects[] = {L"Free", L"16:9", L"4:3", L"1:1", L"9:16"};
const double kAspectValues[] = {0, 16.0 / 9, 4.0 / 3, 1, 9.0 / 16};

std::wstring g_folder;
HICON g_icon = nullptr;

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

// Decodes the frame at a time on a worker thread (keeps one reader open, so stepping forward is cheap).
class FrameFetcher {
public:
    FrameFetcher(std::wstring path, HWND hwnd) : path_(std::move(path)), hwnd_(hwnd), worker_([this] { Work(); }) {}
    ~FrameFetcher() {
        {
            std::lock_guard l(mu_);
            quit_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }
    void Request(double t) {
        {
            std::lock_guard l(mu_);
            want_ = t;
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
        VideoReader r;
        const bool ok = r.Open(path_);
        BitmapPtr last;
        double lastT = -1;
        for (;;) {
            double t;
            {
                std::unique_lock l(mu_);
                cv_.wait(l, [&] { return quit_ || want_ >= 0; });
                if (quit_) break;
                t = want_;
                want_ = -1;
            }
            if (!ok) continue;
            if (!(last && t >= lastT && t - lastT < 1.0)) {  // stepping forward reads on; anything else seeks
                r.Seek(t);
                last = nullptr;
                lastT = -1;
            }
            BitmapPtr f;
            double ft = 0;
            const double frameDur = r.Fps() > 1 ? 1 / r.Fps() : 1 / 30.0;
            while (!(last && lastT >= t - frameDur * 0.5) && r.Read(&f, &ft)) {
                last = f;
                lastT = ft;
            }
            if (!last) continue;
            auto* res = new Result{last, lastT};
            if (!PostMessageW(hwnd_, WM_FETCHED, 0, (LPARAM)res)) delete res;
        }
        CoUninitialize();
    }

    std::wstring path_;
    HWND hwnd_;
    std::mutex mu_;
    std::condition_variable cv_;
    double want_ = -1;
    bool quit_ = false;
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

    std::unique_ptr<VideoPlayer> player;
    std::unique_ptr<FrameFetcher> fetcher;
    SIZE videoSize{16, 9};
    double duration = 0;
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

    HFONT fUi = nullptr, fSmall = nullptr, fIcon = nullptr, fIconSmall = nullptr, fMono = nullptr, fEmoji = nullptr;
    std::vector<Hot> hots;
    RECT hoverRect{};

    enum class DragKind { None, Crop, Move, Handle, Caption, TrimStart, TrimEnd, Playhead, Item };
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
        if (selected && !SelCaption() && !SelMark()) selected.reset();
        Changed();
    }

    // Anything in the edit changed: the preview, the timeline and the fields follow.
    void Changed() {
        if (player) player->SetMuted(edit.muted);
        Rerender();
        SyncFields();
        Invalidate();
    }

    void Select(std::optional<uint64_t> id) {
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
        shown = r.Render(*raw, playing ? rawT : paused);
    }

    // ---- playback ----

    void Seek(double t) {
        t = std::clamp(t, 0.0, std::max(0.0, duration));
        paused = t;
        if (player) player->Seek(t);
        if (!playing && fetcher) fetcher->Request(t);
        Invalidate();
    }

    void Play() {
        if (!player) return;
        if (Now() < edit.trimStart || Now() >= edit.trimEnd - 0.05) Seek(edit.trimStart);
        player->Seek(paused);
        player->SetRate(edit.speed);
        player->SetMuted(edit.muted);
        player->Play();
        playing = true;
        Rerender();
        Invalidate();
    }

    void Pause() {
        if (!playing) return;
        paused = player ? player->Now() : paused;
        if (player) player->Pause();
        playing = false;
        if (fetcher) fetcher->Request(paused);  // the exact frame (and zoom off) for editing
        Rerender();
        Invalidate();
    }

    void TogglePlay() { playing ? Pause() : Play(); }

    void Step(double frames) {
        Pause();
        Seek(Now() + frames / 30);
    }

    void Replay(const Mark& m) {
        Pause();
        const double from = std::max(edit.trimStart, m.start - 0.4);
        lastReplay = from;
        Seek(from);
        Play();
    }

    void Tick() {
        if (!player) return;
        double t = 0;
        if (playing) {
            if (auto f = player->NewFrame(&t)) {
                if (f->Width() != videoSize.cx || f->Height() != videoSize.cy) f = Resample(*f, videoSize.cx, videoSize.cy);
                if (f) raw = f;
                rawT = t;
                Rerender();
                Invalidate();  // only when there's a new frame (the playhead moves with it)
            }
            if (player->Now() >= edit.trimEnd - 0.01 || !player->Playing()) {
                Pause();
                Seek(edit.trimStart);
            }
        } else {
            player->NewFrame(&t);  // drains the engine; paused frames come from the fetcher
        }
    }

    // ---- editing ----

    void SetTrim(std::optional<double> start, std::optional<double> end) {
        if (start) edit.trimStart = std::min(std::max(0.0, *start), edit.trimEnd - 0.1);
        if (end) edit.trimEnd = std::max(std::min(duration, *end), edit.trimStart + 0.1);
        Changed();
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
        std::erase_if(edit.captions, [&](const Caption& c) { return c.id == id; });
        std::erase_if(edit.marks, [&](const Mark& m) { return m.id == id; });
        selected.reset();
        SetFocus(hwnd);
        Changed();
    }

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

    std::vector<MenuItem> AddMenu() {
        std::vector<MenuItem> v;
        MenuItem cap;
        cap.label = L"Caption\tT";
        cap.run = [this] { AddCaption(); };
        v.push_back(cap);
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
        if (auto ci = SelCaption()) {
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
            const wchar_t* cue = kind == 100 ? L"Caption text" : kind == (int)MarkKind::Bubble ? L"Bubble text" : kind == (int)MarkKind::Title ? L"Title"
                                 : kind == (int)MarkKind::Emoji ? L"Emoji (Win+. for more)" : L"Text";
            SendMessageW(field1, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
            SendMessageW(field2, EM_SETCUEBANNER, TRUE, (LPARAM)L"Subtitle (optional)");
        }
        if (id == 0) {
            if (GetFocus() == field1 || GetFocus() == field2) SetFocus(hwnd);
            ShowWindow(field1, SW_HIDE);
            ShowWindow(field2, SW_HIDE);
        }
    }

    void FieldChanged(HWND f) {
        if (settingText) return;
        const int n = GetWindowTextLengthW(f);
        std::wstring t(n, L'\0');
        GetWindowTextW(f, t.data(), n + 1);
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
    RECT TimelineRect() const {
        const RECT c = Client();
        const int h = S(90) + RowCount() * S(18);
        return {S(100), c.bottom - S(14) - h, c.right - S(14), c.bottom - S(14)};
    }
    RECT InspectorRect() const {
        const RECT t = TimelineRect();
        const RECT c = Client();
        return {S(14), t.top - S(8) - S(30), c.right - S(14), t.top - S(8)};
    }
    RECT StageRect() const {
        const RECT c = Client();
        return {S(14), S(50), c.right - S(14), InspectorRect().top - S(8)};
    }
    // Where the video is drawn.
    gp::RectF VideoRect() const {
        const RECT st = StageRect();
        const double k = std::min(RectW(st) / (double)videoSize.cx, RectH(st) / (double)videoSize.cy);
        const double w = videoSize.cx * k, h = videoSize.cy * k;
        return gp::RectF((float)(st.left + (RectW(st) - w) / 2), (float)(st.top + (RectH(st) - h) / 2), (float)w, (float)h);
    }
    double ViewScale() const { return VideoRect().Width / std::max(1L, videoSize.cx); }
    VPoint ToVideo(POINT p) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return {(p.x - v.X) / k, (p.y - v.Y) / k};
    }
    gp::PointF ToView(VPoint p) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return gp::PointF((float)(v.X + p.x * k), (float)(v.Y + p.y * k));
    }
    gp::RectF ToView(VRect r) const {
        const auto v = VideoRect();
        const double k = ViewScale();
        return gp::RectF((float)(v.X + r.x * k), (float)(v.Y + r.y * k), (float)(r.w * k), (float)(r.h * k));
    }

    double TX(double t) const {  // timeline x for a time
        const RECT r = TimelineRect();
        return r.left + RectW(r) * (t / std::max(0.001, duration));
    }
    double TT(int x) const {
        const RECT r = TimelineRect();
        return std::clamp((double)(x - r.left) / std::max(1, RectW(r)), 0.0, 1.0) * duration;
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
        r = Button(dc, g, x, y, h, 0xE710, L"Add", true, false, [] {}, L"Add text, emoji, callouts, blur, zoom or a title card at the playhead");
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
        // Right side: Save GIF, Save.
        const int saveW = S(68);
        RECT save{c.right - S(14) - saveW, y, c.right - S(14), y + h};
        const int gifW = S(9) * 2 + S(22) + Measure(dc, fUi, L"Save GIF").cx;
        Button(dc, g, save.left - S(6) - gifW, y, h, 0xE8B9, L"Save GIF", false, false, [this] { Save(true); }, L"Save as a GIF (Ctrl+Shift+S)");
        Button(dc, g, save.left, y, h, 0, L"Save", false, false, [this] { Save(false); }, L"Save as a new MP4 in your captures (Ctrl+S)", true, saveW);
    }

    void PaintStage(HDC dc, gp::Graphics& g) {
        const RECT st = StageRect();
        FillRR(g, st, (float)S(8), gp::Color(255, 0, 0, 0));
        const auto v = VideoRect();
        if (shown) {
            MemDC src(shown->Handle());
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, (int)std::lround(v.X), (int)std::lround(v.Y), (int)std::lround(v.Width), (int)std::lround(v.Height), src, 0, 0, shown->Width(),
                       shown->Height(), SRCCOPY);
        }
        // Guides and handles are drawn above the video, so they show while it plays too.
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
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
        const auto ci = SelCaption();
        const auto mi = SelMark();
        if (ci) {
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
        if (ci || mi) button(0xE74D, L"Delete", false, [this](RECT) { DeleteSelected(); }, L"Delete (Del)");

        if (parts.empty()) {
            Text(dc, fSmall, L"Space plays · I and O trim · T caption · A arrow · R box · E emoji · N step · X blur · Z zoom · C crop · Ctrl+Z undo",
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

    void PaintTimeline(HDC dc, gp::Graphics& g) {
        const RECT tr = TimelineRect();
        const int stripH = S(52), capY = tr.top + S(62), capH = S(22), markY = tr.top + S(90), rowH = S(18);
        const RECT strip{tr.left, tr.top, tr.right, tr.top + stripH};
        FillRR(g, strip, (float)S(6), A(theme::kSurface));
        if (!thumbs.empty()) {
            const double w = RectW(strip) / (double)thumbs.size();
            HRGN clip = CreateRectRgn(strip.left, strip.top, strip.right, strip.bottom);
            SelectClipRgn(dc, clip);
            SetStretchBltMode(dc, HALFTONE);
            for (size_t i = 0; i < thumbs.size(); ++i) {
                const auto& img = thumbs[i];
                const RECT cell{(LONG)(strip.left + i * w), strip.top, (LONG)std::ceil(strip.left + (i + 1) * w), strip.bottom};
                const double k = std::max(RectW(cell) / (double)img->Width(), RectH(cell) / (double)img->Height());
                const int dw = (int)std::ceil(img->Width() * k), dh = (int)std::ceil(img->Height() * k);
                HRGN cr = CreateRectRgnIndirect(&cell);
                ExtSelectClipRgn(dc, cr, RGN_AND);
                MemDC src(img->Handle());
                StretchBlt(dc, (cell.left + cell.right) / 2 - dw / 2, (cell.top + cell.bottom) / 2 - dh / 2, dw, dh, src, 0, 0, img->Width(), img->Height(), SRCCOPY);
                SelectClipRgn(dc, clip);
                DeleteObject(cr);
            }
            SelectClipRgn(dc, nullptr);
            DeleteObject(clip);
        }
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
        const float px = (float)TX(Now());
        gp::SolidBrush white(gp::Color(255, 255, 255, 255));
        g.FillRectangle(&white, px - s, (float)tr.top - S(2), 2 * s, (float)RectH(tr) + S(2));
        g.FillEllipse(&white, px - 5 * s, (float)tr.top - S(4), 10 * s, 10 * s);

        // Play button and time, left of the timeline.
        const RECT play{S(14), tr.top + S(6), S(14) + S(34), tr.top + S(40)};
        if (EqualRect(&play, &hoverRect)) FillRR(g, play, (float)S(6), A(theme::kBgRaised));
        Text(dc, fIcon, playing ? L"" : L"", play, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        Hotspot(play, [this] { TogglePlay(); }, L"Play / pause (Space)");
        Text(dc, fMono, Clock(Now()), {S(12), play.bottom + S(8), tr.left - S(4), play.bottom + S(24)}, theme::kTextDim);
        Text(dc, fMono, Clock(edit.OutputDuration()) + L" out", {S(12), play.bottom + S(24), tr.left - S(4), play.bottom + S(40)}, theme::kMuted);
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
            PaintInspector(dc, g);
            PaintTimeline(dc, g);
            if (tipShown) PaintTooltip(dc, g);
        }
        MemDC src(back->Handle());
        BitBlt(target, 0, 0, w, h, src, 0, 0, SRCCOPY);
    }
    bool tipShown = false;
    BitmapPtr backBuffer;

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

    void OnMouseDown(POINT p, bool dbl) {
        SetFocus(hwnd);
        if (OnHot(p)) return;
        const RECT tr = TimelineRect();
        const RECT st = StageRect();
        if (PtInRect(&tr, p) || (p.y >= tr.top - S(6) && p.x >= tr.left && p.x <= tr.right)) return TimelineDown(p, dbl);
        if (PtInRect(&st, p)) return StageDown(p, dbl);
    }

    void StageDown(POINT pt, bool dbl) {
        const VPoint vp = ToVideo(pt);
        const double slop = 8 * s / std::max(0.01, ViewScale());
        SetCapture(hwnd);
        if (cropping) {
            PushUndo();
            drag = {};
            drag.kind = DragKind::Crop;
            drag.from = vp;
            return;
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
        if (selected) Select(std::nullopt);
        else TogglePlay();
    }

    void TimelineDown(POINT p, bool dbl) {
        const RECT tr = TimelineRect();
        const int capY = tr.top + S(62), markY = tr.top + S(90), rowH = S(18);
        SetCapture(hwnd);
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
            Seek(TT(p.x));
            ReleaseCapture();
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
            Seek(TT(p.x));
            ReleaseCapture();
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
        Seek(TT(p.x));
    }

    void OnMouseMove(POINT p, WPARAM keys) {
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
            case DragKind::Playhead: Seek(now); break;
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

    void OnMouseUp() {
        if (drag.kind == DragKind::Item)
            std::stable_sort(edit.captions.begin(), edit.captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
        drag = {};
        ReleaseCapture();
        Invalidate();
    }

    // ---- keys ----

    bool OnKey(WPARAM vk) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl) {
            switch (vk) {
                case 'S': Save(shift); return true;
                case 'Z': Undo(); return true;
                case 'W': PostMessageW(hwnd, WM_CLOSE, 0, 0); return true;
                default: return false;
            }
        }
        switch (vk) {
            case VK_SPACE: TogglePlay(); break;
            case VK_LEFT: Step(shift ? -30 : -1); break;
            case VK_RIGHT: Step(shift ? 30 : 1); break;
            case 'I': PushUndo(); SetTrim(Now(), std::nullopt); break;
            case 'O': PushUndo(); SetTrim(std::nullopt, Now()); break;
            case 'T': AddCaption(); break;
            case 'A': AddMark(MarkKind::Arrow); break;
            case 'R': AddMark(MarkKind::Box); break;
            case 'E': AddMark(MarkKind::Emoji); break;
            case 'N': AddMark(MarkKind::Step); break;
            case 'X': AddMark(MarkKind::Blur); break;
            case 'Z': AddMark(MarkKind::Zoom); break;
            case 'C': cropping = !cropping; Invalidate(); break;
            case VK_DELETE:
            case VK_BACK: DeleteSelected(); break;
            case VK_ESCAPE:
                if (cropping) cropping = false;
                else if (selected) Select(std::nullopt);
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
        const std::wstring p = path;
        const double a = edit.trimStart, b = edit.trimEnd;
        HWND h = hwnd;
        std::thread([p, a, b, h] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto* r = new TranscribeResult{};
            r->ok = Transcribe(p, a, b, &r->caps, &r->err);
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

    void Saved(bool ok, bool gif, const std::wstring& out, const std::wstring& tmp, const std::wstring& err) {
        busy = saving = false;
        lastSaveError = ok ? L"" : err;
        Invalidate();
        if (!ok) {
            DeleteFileW(tmp.c_str());
            ShowToast(L"Save failed", err, nullptr, nullptr, 8000);
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
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa);
        const double mb = ((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) / 1048576.0;
        wchar_t size[32];
        swprintf_s(size, mb >= 1 ? L"%.1f MB" : L"%.0f KB", mb >= 1 ? mb : mb * 1024);
        ShowToast(gif ? L"GIF saved" : L"Video saved", FileNameOf(out) + L"  ·  " + size + L"  ·  click to show in Explorer", nullptr,
                  [out] { RevealInExplorer({out}); }, 5000);
    }

    // ---- window ----

    void Fonts() {
        for (HFONT f : {fUi, fSmall, fIcon, fIconSmall, fMono, fEmoji})
            if (f) DeleteObject(f);
        fUi = MakeFont(S(13));
        fSmall = MakeFont(S(11));
        fIcon = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kIconFace);
        fIconSmall = CreateFontW(-S(10), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, kIconFace);
        fMono = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Cascadia Mono");
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
        SetFocus(e->hwnd);
        return 0;
    }
    if (m == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && (w == 'S' || w == 'W')) return e->OnKey(w), 0;
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0;  // no beep
    if (m == WM_SETFOCUS) e->PushUndo();  // one undo step per editing session
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
        case WM_LBUTTONDBLCLK: OnMouseDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, m == WM_LBUTTONDBLCLK); return 0;
        case WM_MOUSEMOVE: OnMouseMove({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, w); return 0;
        case WM_LBUTTONUP: OnMouseUp(); return 0;
        case WM_MOUSELEAVE: hoverRect = {}; tipShown = false; Invalidate(); return 0;
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
                Rerender();
                Invalidate();
            }
            return 0;
        }
        case WM_THUMBS: {
            std::unique_ptr<std::vector<BitmapPtr>> t(reinterpret_cast<std::vector<BitmapPtr>*>(l));
            thumbs = std::move(*t);
            Invalidate();
            return 0;
        }
        case WM_TRANSCRIBED: {
            std::unique_ptr<TranscribeResult> r(reinterpret_cast<TranscribeResult*>(l));
            Transcribed(r->ok, std::move(r->caps), r->err);
            return 0;
        }
        case WM_SAVED: {
            std::unique_ptr<SaveResult> r(reinterpret_cast<SaveResult*>(l));
            Saved(r->ok, r->gif, r->out, r->tmp, r->err);
            return 0;
        }
        case WM_CLOSE:
            if (dirty && !snapshotMode &&
                MessageBoxW(hwnd, L"Close the video editor and discard your changes?", L"Edit video", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerFrame);
            player.reset();
            fetcher.reset();
            return 0;
        case WM_NCDESTROY: {  // after the children: they still need FieldProc to find this editor
            std::erase(g_editors, this);
            if (g_editors.empty()) ClearRenderCache();  // rendered titles and captions can be large
            for (HFONT f : {fUi, fSmall, fIcon, fIconSmall, fMono, fEmoji})
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
    if (!ProbeVideo(path, &vi, 0, 0, &first) || vi.w <= 0) return false;
    videoSize = {vi.w, vi.h};
    duration = vi.duration;
    raw = first;
    edit.trimEnd = duration;
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
    player = VideoPlayer::Open(path, hwnd, WM_ENGINE, &err);
    fetcher = std::make_unique<FrameFetcher>(path, hwnd);
    Rerender();
    SetTimer(hwnd, kTimerFrame, 15, nullptr);
    HWND self = hwnd;
    const std::wstring p = path;
    std::thread([p, self] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* t = new std::vector<BitmapPtr>(VideoThumbnails(p, 16, 240));
        CoUninitialize();
        if (!PostMessageW(self, WM_THUMBS, 0, (LPARAM)t)) delete t;
    }).detach();
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

bool IsVideoFile(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring ext = Lower(path.substr(dot + 1));
    return ext == L"mp4" || ext == L"mov" || ext == L"m4v" || ext == L"wmv" || ext == L"avi" || ext == L"mkv";
}

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
    e->dirty = false;
    DestroyWindow(e->hwnd);
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
    const std::wstring oldFolder = g_folder;
    g_folder = caps;
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
    g_folder = oldFolder;
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

}  // namespace ather
