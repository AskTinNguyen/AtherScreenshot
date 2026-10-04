#include "gallery.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <commctrl.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "gallery_model.h"
#include "media.h"
#include "videoio.h"
#include "ocr.h"
#include "output.h"
#include "palette.h"
#include "pin.h"
#include "smart.h"
#include "toast.h"

#pragma comment(lib, "comctl32")

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotGallery";
constexpr wchar_t kIconFace[] = L"Segoe Fluent Icons";
constexpr UINT WM_THUMB = WM_APP + 30, WM_LIBCHANGED = WM_APP + 31;
enum : UINT_PTR { kTimerLib = 1, kTimerSearch, kTimerTip, kTimerSlide, kTimerGif, kTimerSpin, kTimerVideo };
enum : int { kSearchId = 100, kTagEditId, kCommentId };

constexpr int kSidebarW = 216, kInspectorW = 290, kSpacing = 8, kPadX = 16, kBottomPad = 76, kListRowH = 34, kListHeadH = 30;

// ---------- small helpers ----------

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

void FillRR(HDC dc, const RECT& r, float rad, gp::Color c) {
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    gp::GraphicsPath p;
    RoundPath(p, (float)r.left, (float)r.top, (float)RectW(r), (float)RectH(r), rad);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void StrokeRR(HDC dc, const RECT& r, float rad, gp::Color c, float width, bool dashed = false) {
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    gp::GraphicsPath p;
    const float inset = width / 2;
    RoundPath(p, r.left + inset, r.top + inset, RectW(r) - width, RectH(r) - width, rad);
    gp::Pen pen(c, width);
    if (dashed) {
        const gp::REAL pattern[] = {3, 2};
        pen.SetDashPattern(pattern, 2);
    }
    g.DrawPath(&pen, &p);
}

// Translucent floating surface: soft shadow, near-opaque fill, hairline.
void Glass(HDC dc, const RECT& r, float rad, float s) {
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        for (int k = 6; k >= 1; --k) {
            const float grow = 2.5f * s * k;
            gp::GraphicsPath p;
            RoundPath(p, r.left - grow, r.top - grow + 6 * s, RectW(r) + 2 * grow, RectH(r) + 2 * grow, rad + grow);
            gp::SolidBrush b(gp::Color((BYTE)(6 + (6 - k) * 4), 0, 0, 0));
            g.FillPath(&b, &p);
        }
    }
    FillRR(dc, r, rad, gp::Color(240, 30, 31, 29));
    StrokeRR(dc, r, rad, gp::Color(32, 255, 255, 255), std::max(1.f, 0.75f * s));
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

std::wstring Bytes(int64_t b) {
    wchar_t s[32];
    if (b >= 1000LL * 1000 * 1000) swprintf_s(s, L"%.1f GB", b / 1e9);
    else if (b >= 1000 * 1000) swprintf_s(s, L"%.1f MB", b / 1e6);
    else if (b >= 1000) swprintf_s(s, L"%lld KB", (long long)(b / 1000));
    else swprintf_s(s, L"%lld bytes", (long long)b);
    return s;
}

std::wstring Duration(double secs) {
    const int t = (int)std::lround(secs);
    wchar_t s[16];
    swprintf_s(s, L"%d:%02d", t / 60, t % 60);
    return s;
}

std::wstring Stem(const std::wstring& path) {
    std::wstring n = FileNameOf(path);
    const size_t dot = n.find_last_of(L'.');
    return dot == std::wstring::npos ? n : n.substr(0, dot);
}

std::wstring Stars(int n) { return std::wstring(n, L'★'); }

std::vector<std::wstring> SplitTags(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t c = s.find(L',', start);
        out.push_back(s.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start));
        if (c == std::wstring::npos) break;
        start = c + 1;
    }
    return NormalizeTags(out);
}

std::wstring Join(const std::vector<std::wstring>& v, const std::wstring& sep) {
    std::wstring s;
    for (const auto& x : v) s += (s.empty() ? L"" : sep) + x;
    return s;
}

// A child window's own visibility (IsWindowVisible is false whenever the parent is hidden).
bool Shown(HWND h) { return h && (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0; }

int CountWords(const std::wstring& t) {
    int n = 0;
    bool in = false;
    for (wchar_t c : t) {
        const bool w = !iswspace(c);
        if (w && !in) ++n;
        in = w;
    }
    return n;
}

const wchar_t* const kPresetColors[] = {L"#FF3B30", L"#FF9500", L"#FFCC00", L"#34C759", L"#00C7BE", L"#0A84FF", L"#5E5CE6",
                                        L"#BF5AF2", L"#FF2D55", L"#A2845E", L"#FFFFFF", L"#8E8E93", L"#1C1C1E", L"#D4FF00"};

COLORREF HexColor(const std::wstring& hex) {
    uint8_t r = 0, g = 0, b = 0;
    Filter::Rgb(hex, &r, &g, &b);
    return RGB(r, g, b);
}

// ---------- thumbnails: decoded on worker threads, cached by size bucket ----------

struct ThumbJob {
    std::wstring path;
    int bucket;
    std::wstring key;
};
struct ThumbDone {
    std::wstring key;
    BitmapPtr bmp;
};

struct ThumbShared {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<ThumbJob> jobs;
    bool alive = true;
    HWND notify = nullptr;
};

class Thumbs {
public:
    void Start(HWND notify) {
        sh_ = std::make_shared<ThumbShared>();
        sh_->notify = notify;
        for (int i = 0; i < 2; ++i) std::thread(Work, sh_).detach();
    }
    void Stop() {
        if (!sh_) return;
        {
            std::lock_guard lock(sh_->mu);
            sh_->alive = false;
            sh_->jobs.clear();
        }
        sh_->cv.notify_all();
        sh_.reset();
    }
    static int Bucket(int px) { return px <= 200 ? 256 : px <= 400 ? 512 : 1024; }
    static std::wstring Key(const std::wstring& path, int bucket, double mtime) {
        return path + L"#" + std::to_wstring(bucket) + L"#" + std::to_wstring((int64_t)(mtime * 1000));
    }
    // The decoded image for this bucket, or the closest one already cached (queued for decoding if missing).
    BitmapPtr Get(const std::wstring& path, int bucket, double mtime, bool* exact) {
        const std::wstring key = Key(path, bucket, mtime);
        if (auto it = cache_.find(key); it != cache_.end()) {
            it->second.use = ++clock_;
            *exact = true;
            return it->second.bmp;
        }
        *exact = false;
        if (!pending_.count(key) && sh_) {
            pending_.insert(key);
            std::lock_guard lock(sh_->mu);
            sh_->jobs.push_front({path, bucket, key});  // most recently painted first
            if (sh_->jobs.size() > 400) {  // the oldest request is dropped; it's asked for again when painted again
                pending_.erase(sh_->jobs.back().key);
                sh_->jobs.pop_back();
            }
            sh_->cv.notify_one();
        }
        for (int b : {256, 512, 1024})
            if (b != bucket)
                if (auto it = cache_.find(Key(path, b, mtime)); it != cache_.end() && it->second.bmp) return it->second.bmp;
        return nullptr;
    }
    void Store(ThumbDone* d) {
        pending_.erase(d->key);
        Entry& e = cache_[d->key];
        bytes_ -= e.bytes;
        e.bmp = d->bmp;
        e.bytes = d->bmp ? (size_t)d->bmp->Width() * d->bmp->Height() * 4 : 0;
        e.use = ++clock_;
        bytes_ += e.bytes;
        Trim();
    }
    void Clear() {
        cache_.clear();
        pending_.clear();
        bytes_ = 0;
    }

private:
    struct Entry {
        BitmapPtr bmp;
        size_t bytes = 0;
        uint64_t use = 0;
    };
    void Trim() {
        constexpr size_t kBudget = 320u << 20;
        if (bytes_ <= kBudget) return;
        std::vector<std::pair<uint64_t, std::wstring>> order;
        for (const auto& [k, e] : cache_) order.push_back({e.use, k});
        std::sort(order.begin(), order.end());
        for (const auto& [use, k] : order) {
            if (bytes_ <= kBudget * 3 / 4) break;
            bytes_ -= cache_[k].bytes;
            cache_.erase(k);
        }
    }
    static void Work(std::shared_ptr<ThumbShared> sh) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            ThumbJob job;
            {
                std::unique_lock lock(sh->mu);
                sh->cv.wait(lock, [&] { return !sh->jobs.empty() || !sh->alive; });
                if (!sh->alive) break;
                job = std::move(sh->jobs.front());
                sh->jobs.pop_front();
            }
            BitmapPtr bmp;
            if (MediaTypeOf(job.path) == MediaType::Video) {
                VideoInfo vi;
                if (ProbeVideo(job.path, &vi)) ProbeVideo(job.path, nullptr, std::min(1.0, vi.duration / 2), job.bucket, &bmp);
            } else {
                bmp = LoadImageScaled(job.path, job.bucket);
            }
            auto* done = new ThumbDone{job.key, bmp};
            bool posted = false;
            {
                std::lock_guard lock(sh->mu);
                posted = sh->alive && PostMessageW(sh->notify, WM_THUMB, 0, (LPARAM)done);
            }
            if (!posted) delete done;
        }
        CoUninitialize();
    }
    std::shared_ptr<ThumbShared> sh_;
    std::unordered_map<std::wstring, Entry> cache_;
    std::unordered_set<std::wstring> pending_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
};

// Scaled, rounded tiles ready to blit (the gallery background is solid, so corners are baked in).
class TileCache {
public:
    BitmapPtr Find(const std::wstring& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return nullptr;
        it->second.use = ++clock_;
        return it->second.bmp;
    }
    void Put(const std::wstring& key, BitmapPtr b) {
        Entry& e = map_[key];
        bytes_ -= e.bytes;
        e.bmp = b;
        e.bytes = b ? (size_t)b->Width() * b->Height() * 4 : 0;
        e.use = ++clock_;
        bytes_ += e.bytes;
        if (bytes_ > (160u << 20)) {
            std::vector<std::pair<uint64_t, std::wstring>> order;
            for (const auto& [k, v] : map_) order.push_back({v.use, k});
            std::sort(order.begin(), order.end());
            for (const auto& [u, k] : order) {
                if (bytes_ <= (110u << 20)) break;
                bytes_ -= map_[k].bytes;
                map_.erase(k);
            }
        }
    }
    void Clear() {
        map_.clear();
        bytes_ = 0;
    }

private:
    struct Entry {
        BitmapPtr bmp;
        size_t bytes = 0;
        uint64_t use = 0;
    };
    std::unordered_map<std::wstring, Entry> map_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
};

// Draws `src` into a w×h tile: `fill` crops to cover, otherwise fits on a raised background. Rounded corners
// (anti-aliased) over `bg`.
BitmapPtr RenderTile(const Bitmap& src, int w, int h, bool fill, float radius, COLORREF bg) {
    auto out = Bitmap::Create(w, h);
    if (!out) return nullptr;
    const uint32_t bgPx = 0xFF000000u | GetRValue(bg) << 16 | GetGValue(bg) << 8 | GetBValue(bg);
    std::fill_n(out->Bits(), (size_t)w * h, bgPx);
    auto content = Bitmap::Create(w, h);
    if (!content) return out;
    {
        const COLORREF raised = theme::kBgRaised;
        std::fill_n(content->Bits(), (size_t)w * h, 0xFF000000u | (raised & 0xFF) << 16 | (raised >> 8 & 0xFF) << 8 | (raised >> 16 & 0xFF));
        gp::Bitmap s(src.Width(), src.Height(), src.Width() * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(src.Bits()));
        gp::Bitmap d(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(content->Bits()));
        gp::Graphics g(&d);
        g.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        const double sa = (double)src.Width() / src.Height(), da = (double)w / h;
        gp::ImageAttributes ia;
        ia.SetWrapMode(gp::WrapModeTileFlipXY);  // no dark fringe at the edges
        if (fill) {
            double sw = src.Width(), shh = src.Height(), sx = 0, sy = 0;
            if (sa > da) {
                sw = shh * da;
                sx = (src.Width() - sw) / 2;
            } else {
                shh = sw / da;
                sy = (src.Height() - shh) / 2;
            }
            g.DrawImage(&s, gp::RectF(0, 0, (float)w, (float)h), (float)sx, (float)sy, (float)sw, (float)shh, gp::UnitPixel, &ia);
        } else {
            double k = std::min((double)w / src.Width(), (double)h / src.Height());
            const float iw = (float)(src.Width() * k), ih = (float)(src.Height() * k);
            g.DrawImage(&s, gp::RectF((w - iw) / 2, (h - ih) / 2, iw, ih), 0, 0, (float)src.Width(), (float)src.Height(), gp::UnitPixel, &ia);
        }
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) content->Bits()[i] |= 0xFF000000u;
    {
        gp::Bitmap d(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(out->Bits()));
        gp::Bitmap c(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(content->Bits()));
        gp::Graphics g(&d);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        gp::TextureBrush tb(&c);
        gp::GraphicsPath p;
        RoundPath(p, 0, 0, (float)w, (float)h, radius);
        g.FillPath(&tb, &p);
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) out->Bits()[i] |= 0xFF000000u;
    return out;
}

// ---------- GIF frames for the preview ----------

// An animated GIF in the preview, decoded one frame at a time as it plays (a long recording can have
// thousands of full-size frames; decoding them all up front froze the window and took gigabytes).
struct GifAnim {
    struct State {
        IWICImagingFactory* f = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        UINT count = 0, next = 0;
        int W = 0, H = 0;
        BitmapPtr canvas, restore;  // composited picture; what to restore after a "previous" disposal
        int disposal = 0;
        RECT last{};  // area of the frame shown last, for its disposal
        ~State() {
            if (dec) dec->Release();
            if (f) f->Release();
        }
    };
    std::wstring path;
    std::shared_ptr<State> st;
    bool Valid() const { return st && st->count > 0; }
    bool Animated() const { return st && st->count > 1; }

    // The next frame of the animation (looping), and how long to show it.
    BitmapPtr Next(int* delayMs) {
        *delayMs = 100;
        if (!Valid()) return nullptr;
        State& a = *st;
        if (a.next >= a.count) {  // loop: start from a clean canvas
            a.next = 0;
            a.canvas.reset();
            a.restore.reset();
            a.disposal = 0;
        }
        if (a.canvas) {  // dispose of the previous frame
            if (a.disposal == 2)
                for (int y = std::max(0L, a.last.top); y < std::min((LONG)a.H, a.last.bottom); ++y)
                    std::fill_n(a.canvas->Bits() + (size_t)y * a.W + std::max(0L, a.last.left),
                                std::max(0L, std::min((LONG)a.W, a.last.right) - std::max(0L, a.last.left)), 0xFF000000u);
            if (a.disposal == 3 && a.restore) a.canvas = a.restore;
        }
        IWICBitmapFrameDecode* fr = nullptr;
        if (FAILED(a.dec->GetFrame(a.next++, &fr))) return a.canvas ? a.canvas->Crop({0, 0, a.W, a.H}) : nullptr;
        UINT fw = 0, fh = 0;
        fr->GetSize(&fw, &fh);
        int left = 0, top = 0, delay = 10, disposal = 0;
        IWICMetadataQueryReader* q = nullptr;
        if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/imgdesc/Left", &v)) && v.vt == VT_UI2) left = v.uiVal;
            PropVariantClear(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/imgdesc/Top", &v)) && v.vt == VT_UI2) top = v.uiVal;
            PropVariantClear(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/grctlext/Delay", &v)) && v.vt == VT_UI2 && v.uiVal > 0) delay = v.uiVal;
            PropVariantClear(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/grctlext/Disposal", &v)) && v.vt == VT_UI1) disposal = v.bVal;
            PropVariantClear(&v);
            q->Release();
        }
        if (!a.W || !a.H) a.W = (int)fw, a.H = (int)fh;
        if (!a.canvas) {
            a.canvas = Bitmap::Create(a.W, a.H);
            if (!a.canvas) {
                fr->Release();
                return nullptr;
            }
            std::fill_n(a.canvas->Bits(), (size_t)a.W * a.H, 0xFF000000u);
        }
        a.restore = disposal == 3 ? a.canvas->Crop({0, 0, a.W, a.H}) : nullptr;
        IWICFormatConverter* conv = nullptr;
        std::vector<uint32_t> px((size_t)fw * fh);
        if (SUCCEEDED(a.f->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            conv->CopyPixels(nullptr, fw * 4, (UINT)px.size() * 4, reinterpret_cast<BYTE*>(px.data()));
        if (conv) conv->Release();
        fr->Release();
        for (UINT y = 0; y < fh; ++y)
            for (UINT x = 0; x < fw; ++x) {
                const uint32_t c = px[(size_t)y * fw + x];
                const int cx = left + (int)x, cy = top + (int)y;
                if ((c >> 24) >= 128 && cx < a.W && cy < a.H) a.canvas->Bits()[(size_t)cy * a.W + cx] = c | 0xFF000000u;
            }
        a.disposal = disposal;
        a.last = {left, top, left + (LONG)fw, top + (LONG)fh};
        *delayMs = std::max(20, delay * 10);
        return a.canvas->Crop({0, 0, a.W, a.H});  // the canvas keeps changing: show a copy
    }
};

GifAnim LoadGif(const std::wstring& path) {
    GifAnim g;
    g.path = path;
    auto st = std::make_shared<GifAnim::State>();
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&st->f))) ||
        FAILED(st->f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &st->dec)) ||
        FAILED(st->dec->GetFrameCount(&st->count)))
        return g;
    IWICMetadataQueryReader* gq = nullptr;
    if (SUCCEEDED(st->dec->GetMetadataQueryReader(&gq))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(gq->GetMetadataByName(L"/logscrdesc/Width", &v)) && v.vt == VT_UI2) st->W = v.uiVal;
        PropVariantClear(&v);
        if (SUCCEEDED(gq->GetMetadataByName(L"/logscrdesc/Height", &v)) && v.vt == VT_UI2) st->H = v.uiVal;
        PropVariantClear(&v);
        gq->Release();
    }
    g.st = std::move(st);
    return g;
}

// ---------- the window ----------

struct Hot {
    RECT r{};
    std::function<void()> click, dbl, right;
    std::wstring tip;
    int tile = -1;  // a browser tile (drag source, hover, selection)
    LPCWSTR cursor = IDC_HAND;
};

struct TileRect {
    int index;   // into model.visible
    RECT r;      // content coordinates (scroll applied when drawn)
    bool fill;   // crop to cover (rows) or fit (grid)
    bool caption;
    int group = -1;  // duplicates: group index
};

class Gallery {
public:
    explicit Gallery(const GalleryHost& h) : host(h), model(Library::Shared()) {}
    GalleryHost host;
    GalleryModel model;
    HWND hwnd = nullptr, search = nullptr, tagEdit = nullptr, commentEdit = nullptr;
    WNDPROC editProc = nullptr;
    Thumbs thumbs;
    TileCache tiles;
    float s = 1;
    HFONT fUi{}, fSmall{}, fTiny{}, fBold{}, fTitle{}, fIcon{}, fIconSmall{}, fBadge{}, fHead{}, fBig{}, fStar{};
    HBRUSH editBrush = CreateSolidBrush(RGB(44, 45, 42));
    int libListener = 0;

    // layout
    RECT client{};
    std::vector<TileRect> tileRects;
    int contentH = 0, scrollY = 0, sideScroll = 0, sideH = 0, inspScroll = 0, inspH = 0;
    int hover = -1;  // tile index under the mouse
    std::vector<Hot> hots;
    int pressed = -1;
    POINT pressPt{};
    int pressTile = -1;
    bool draggingScroll = false;
    int dragScrollStart = 0, dragMouseStart = 0;
    bool tagsCollapsed = true, showAllTags = false, filterOpen = false, showShortcuts = false, showText = false;
    bool addingTag = false, editingComment = false;
    std::wstring commentFor;
    RECT filterBtn{}, scopeBtn{}, viewBtn{};
    // tooltip
    std::wstring tipText;
    POINT tipAt{};
    int tipHot = -1;
    bool tipShown = false;
    // preview
    GifAnim gif;
    BitmapPtr previewImg;
    std::unique_ptr<VideoPlayer> previewVideo;  // a video in the preview plays
    std::wstring previewLoaded;
    double pvZoom = 0;  // 0 = fit
    double pvX = 0, pvY = 0;
    bool pvDragging = false;
    POINT pvLast{}, pvDown{};
    // compare
    BitmapPtr cmpA, cmpB;
    std::wstring cmpKey;
    double split = 0.5;
    bool sideBySide = false, cmpDragging = false;
    RECT cmpBox{};
    float spin = 0;
    bool snapshotMode = false;
    std::vector<std::pair<RECT, std::wstring>> collectionDrops;  // sidebar rows that accept dropped files

    int S(int v) const { return Px(s, v); }
    float Sf(float v) const { return v * s; }

    bool Create();
    void Fonts();
    void Destroy();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);

    // model glue
    void Changed(bool relayout = true) {
        if (relayout) Layout();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    void Recompute() {
        model.Recompute();
        hots.clear();  // they point into the old list: a click before the repaint must not act on the wrong capture
        UpdateSearchCue();
        Changed();
    }
    void SetFilter(const Filter& f) {
        model.SetFilter(f);
        if (GetWindowTextLengthW(search) != (int)f.text.size()) SetWindowTextW(search, f.text.c_str());
        scrollY = 0;
        UpdateSearchCue();
        Changed();
    }
    void SetScope(const Scope& sc) {
        model.SetScope(sc);
        if (sc.kind == ScopeKind::Smart) SetWindowTextW(search, model.filter().text.c_str());
        scrollY = 0;
        UpdateSearchCue();
        Changed();
    }
    bool Typing() const { HWND f = GetFocus(); return f && f == search; }
    void EndTyping() {
        if (GetFocus() != hwnd) SetFocus(hwnd);
    }
    void UpdateSearchCue() {
        const size_t n = model.visible.size();
        std::wstring cue = n == 1 ? L"Search 1 capture" : L"Search " + std::to_wstring(n) + L" captures";
        SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)cue.c_str());
    }

    // geometry
    RECT SidebarRect() const { return model.showSidebar ? RECT{0, 0, S(kSidebarW), client.bottom} : RECT{}; }
    RECT InspectorRect() const {
        return model.showInspector ? RECT{client.right - S(kInspectorW), 0, client.right, client.bottom} : RECT{};
    }
    RECT CanvasRect() const {
        RECT c = client;
        if (model.showSidebar) c.left = S(kSidebarW) + 1;
        if (model.showInspector) c.right = client.right - S(kInspectorW) - 1;
        return c;
    }
    bool HasChipsRow() const {
        Filter f = model.filter();
        f.text.clear();
        return !f.IsEmpty() || (model.relatedCount > 0 && !model.filter().text.empty());
    }
    int ContentTop() const { return S(HasChipsRow() ? 92 : 56); }
    void Layout();
    void ClampScroll();
    void EnsureVisible(const std::wstring& path);

    // painting
    void Paint(HDC hdc);
    void PaintBrowser(HDC dc, const RECT& canvas);
    void PaintTile(HDC dc, const TileRect& t, int dy);
    void PaintList(HDC dc, const RECT& canvas);
    void PaintTopBar(HDC dc, const RECT& canvas);
    void PaintChips(HDC dc, const RECT& canvas, int y);
    void PaintActionBar(HDC dc, const RECT& canvas);
    void PaintSidebar(HDC dc, const RECT& r);
    void PaintInspector(HDC dc, const RECT& r);
    void PaintFilterPanel(HDC dc);
    void PaintShortcuts(HDC dc);
    void PaintPreview(HDC dc);
    void PaintCompare(HDC dc);
    void PaintTooltip(HDC dc);
    void PaintEmpty(HDC dc, const RECT& canvas);
    RECT GlassButton(HDC dc, int x, int y, const std::wstring& glyph, bool on, std::function<void()> click, const std::wstring& tip,
                     int minW = 30, const std::wstring& label = L"");
    RECT Chip(HDC dc, int x, int y, const std::wstring& label, bool on, bool dashed, std::function<void()> click,
              std::function<void()> right = nullptr, const std::wstring& tip = L"", std::function<void()> dbl = nullptr);
    int Flow(HDC dc, int left, int right, int y, int rowH, const std::vector<std::function<RECT(int x, int y, bool measure)>>& items);
    void AddHot(const RECT& r, std::function<void()> click, const std::wstring& tip = L"", std::function<void()> right = nullptr,
                std::function<void()> dbl = nullptr, LPCWSTR cursor = IDC_HAND) {
        Hot h;
        h.r = r;
        h.click = std::move(click);
        h.right = std::move(right);
        h.dbl = std::move(dbl);
        h.tip = tip;
        h.cursor = cursor;
        hots.push_back(std::move(h));
    }
    BitmapPtr TileBitmap(const std::wstring& path, int w, int h, bool fill);

    // input
    bool OverlayUp() const { return showShortcuts || model.compare.has_value() || !model.preview.empty(); }
    int HotAt(POINT p) const {
        for (int i = (int)hots.size() - 1; i >= 0; --i)
            if (PtInRect(&hots[i].r, p)) return i;
        return -1;
    }
    void OnKey(WPARAM vk);
    bool OnCommandKey(WPARAM vk, bool shift);
    void OnMouseDown(POINT p, bool dbl);
    void OnMouseUp(POINT p);
    void OnMouseMove(POINT p, WPARAM keys);
    void OnRightClick(POINT p);
    void OnWheel(POINT p, int delta, bool ctrl);
    void OnDropFiles(HDROP drop);
    void StartDrag();
    void Escape();

    // actions (Gallery.swift GalleryModel actions)
    void Open(const std::wstring& u = L"");
    void Copy();
    void Pin();
    void Reveal();
    void Collage();
    void AddToEditor();
    void Upload();
    void CopyText();
    void Rate(int r);
    void Rename();
    void BatchRename(const std::vector<std::wstring>& ts);
    void Trash(const std::vector<std::wstring>* list = nullptr);
    void FindSimilar(const std::wstring& u = L"");
    void TagPicker();
    void CollectionPicker();
    void SaveSmartFolder();
    void ActionsPalette();
    void ImportPanel();
    void ImportFiles(const std::vector<std::wstring>& files, const std::wstring& collection = L"");
    void Compare();
    void SetPreview(const std::wstring& u);
    void ContextMenu(const std::wstring& u, POINT screen);
    void ScopeMenu();
    void ViewMenu();
    void NewCollection();
    void ToggleInspector() {
        model.SetShowInspector(!model.showInspector);
        Changed();
    }
    void ToggleSidebar() {
        model.showSidebar = !model.showSidebar;
        model.SaveViewOptions();
        Changed();
    }
    void Reveal(const std::wstring& u) {
        if (!model.RevealInGallery(u)) return (void)ShowToast(L"The original is no longer in the gallery", L"", nullptr, nullptr, 2500);
        SetWindowTextW(search, model.filter().text.c_str());
        Changed();
        EnsureVisible(u);
    }
    int Menu(HMENU m, POINT screen) {
        SetForegroundWindow(hwnd);
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        return id;
    }
    POINT ScreenPt(int x, int y) const {
        POINT p{x, y};
        ClientToScreen(hwnd, &p);
        return p;
    }
    void ShowFilterMenu(int which, POINT at);
};

Gallery* g_gallery = nullptr;

void Gallery::Fonts() {
    for (HFONT* f : {&fUi, &fSmall, &fTiny, &fBold, &fTitle, &fIcon, &fIconSmall, &fBadge, &fHead, &fBig, &fStar})
        if (*f) DeleteObject(*f);
    fUi = MakeFont(S(13));
    fSmall = MakeFont(S(12));
    fTiny = MakeFont(S(11));
    fBold = MakeFont(S(13), FW_SEMIBOLD);
    fTitle = MakeFont(S(15), FW_SEMIBOLD);
    fIcon = MakeFont(S(14), FW_NORMAL, kIconFace);
    fIconSmall = MakeFont(S(11), FW_NORMAL, kIconFace);
    fBadge = MakeFont(S(10), FW_BOLD);
    fHead = MakeFont(S(11), FW_SEMIBOLD);
    fBig = MakeFont(S(36), FW_NORMAL, kIconFace);
    fStar = MakeFont(S(9), FW_NORMAL, kIconFace);
    for (HWND e : {search, tagEdit, commentEdit})
        if (e) SendMessageW(e, WM_SETFONT, (WPARAM)fUi, TRUE);
}

// ---------- layout ----------

void Gallery::Layout() {
    GetClientRect(hwnd, &client);
    tileRects.clear();
    const RECT c = CanvasRect();
    const int width = RectW(c) - 2 * S(kPadX);
    int y = ContentTop() + S(8);
    const std::vector<std::wstring>& vis = model.visible;
    model.rows.clear();
    if (vis.empty()) {
        contentH = 0;
        ClampScroll();
        return;
    }
    const ScopeKind kind = model.scope().kind;
    if (kind == ScopeKind::Duplicates) {
        y += S(28);  // summary line
        int idx = 0;
        for (int gi = 0; gi < (int)model.groups.size(); ++gi) {
            const auto& g = model.groups[gi];
            y += S(12) + S(28);  // panel padding + header
            int x = S(kPadX) + S(12);
            const int th = S(140);
            for (size_t k = 0; k < g.size(); ++k, ++idx) {
                const int tw = std::min(S(260), (int)(std::clamp(model.lib.Meta(g[k]).Aspect(), 0.4, 3.0) * th));
                if (x + tw > S(kPadX) + width - S(12) && x > S(kPadX) + S(12)) {
                    x = S(kPadX) + S(12);
                    y += th + S(26);
                }
                tileRects.push_back({idx, {x, y, x + tw, y + th}, true, true, gi});
                x += tw + S(kSpacing);
            }
            y += th + S(22) + S(12) + S(18);
        }
        contentH = y + S(kBottomPad);
        ClampScroll();
        return;
    }
    if (kind == ScopeKind::Similar) y += S(34);
    if (model.layout == GalleryLayout::List) {
        y += S(kListHeadH);
        for (int i = 0; i < (int)vis.size(); ++i) {
            tileRects.push_back({i, {S(kPadX), y, S(kPadX) + width, y + S(kListRowH)}, false, false});
            y += S(kListRowH);
        }
        model.columns = 1;
        contentH = y + S(kBottomPad);
        ClampScroll();
        return;
    }
    if (model.layout == GalleryLayout::Grid) {
        const int side = S((int)model.thumbSize), sp = S(kSpacing);
        const int cols = std::max(1, (width + sp) / (side + sp));
        const int cw = (width - (cols - 1) * sp) / cols, chh = (int)(cw * 0.72);
        const int cap = S(20);
        model.columns = cols;
        for (int i = 0; i < (int)vis.size(); ++i) {
            const int col = i % cols, row = i / cols;
            const int x = S(kPadX) + col * (cw + sp), ty = y + row * (chh + cap + sp + S(6));
            tileRects.push_back({i, {x, ty, x + cw, ty + chh}, false, true});
        }
        const int rows = ((int)vis.size() + cols - 1) / cols;
        contentH = y + rows * (chh + cap + sp + S(6)) + S(kBottomPad);
        ClampScroll();
        return;
    }
    // Justified rows.
    std::vector<double> aspects;
    aspects.reserve(vis.size());
    for (const auto& p : vis) aspects.push_back(model.lib.Meta(p).Aspect());
    const double sp = S(kSpacing);
    model.rows = JustifiedRows(aspects, width, s * model.thumbSize, sp);
    model.columns = std::max(1, (int)((width + sp) / (s * model.thumbSize + sp)));
    const int nameH = model.showNames ? S(18) : 0;
    for (const auto& row : model.rows) {
        double x = S(kPadX);
        const int h = (int)std::lround(row.height);
        for (int k : row.items) {
            const double w = std::max(20.0, ClampedAspect(aspects[k]) * row.height);
            tileRects.push_back({k, {(int)std::lround(x), y, (int)std::lround(x + w), y + h}, true, model.showNames});
            x += w + sp;
        }
        y += h + S(kSpacing) + nameH;
    }
    contentH = y + S(kBottomPad);
    ClampScroll();
}

void Gallery::ClampScroll() {
    const int view = RectH(client);
    scrollY = std::clamp(scrollY, 0, std::max(0, contentH - view));
}

void Gallery::EnsureVisible(const std::wstring& path) {
    for (const auto& t : tileRects) {
        if (model.visible[t.index] != path) continue;
        const int top = t.r.top - scrollY, bottom = t.r.bottom - scrollY + (t.caption ? S(20) : 0);
        const int viewTop = ContentTop(), viewBottom = client.bottom - (model.selection.empty() ? S(16) : S(64));
        if (top < viewTop) scrollY -= viewTop - top + S(8);
        else if (bottom > viewBottom) scrollY += bottom - viewBottom + S(8);
        ClampScroll();
        return;
    }
}

// ---------- tiles ----------

BitmapPtr Gallery::TileBitmap(const std::wstring& path, int w, int h, bool fill) {
    if (w <= 0 || h <= 0) return nullptr;
    const ItemMeta& m = model.lib.Meta(path);
    const std::wstring key = path + L"|" + std::to_wstring(w) + L"x" + std::to_wstring(h) + (fill ? L"f" : L"t") +
                             std::to_wstring((int64_t)(m.mtime * 1000));
    if (auto b = tiles.Find(key)) return b;
    bool exact = false;
    BitmapPtr src = thumbs.Get(path, Thumbs::Bucket(std::max(w, h)), m.mtime, &exact);
    if (!src) return nullptr;
    auto t = RenderTile(*src, w, h, fill, Sf(8), theme::kBg);
    if (exact) tiles.Put(key, t);  // a stand-in from another bucket is redrawn once the right one arrives
    return t;
}

void Gallery::PaintTile(HDC dc, const TileRect& t, int dy) {
    const std::wstring& u = model.visible[t.index];
    const ItemMeta& m = model.lib.Meta(u);
    RECT r = t.r;
    OffsetRect(&r, 0, dy);
    const bool sel = model.selection.count(u) != 0;
    const bool list = model.layout == GalleryLayout::List && model.scope().kind != ScopeKind::Duplicates;
    if (list) {
        if (sel) FillRR(dc, r, Sf(6), gp::Color(28, 255, 255, 255));
        else if (t.index == hover) FillRR(dc, r, Sf(6), gp::Color(12, 255, 255, 255));
        if (model.focus == u && !sel) StrokeRR(dc, r, Sf(6), gp::Color(70, 255, 255, 255), 1);
        const int cy = (r.top + r.bottom) / 2;
        RECT th{r.left + S(4), cy - S(15), r.left + S(48), cy + S(15)};
        if (auto b = TileBitmap(u, RectW(th), RectH(th), true)) {
            MemDC md(b->Handle(), dc);
            BitBlt(dc, th.left, th.top, RectW(th), RectH(th), md, 0, 0, SRCCOPY);
        }
        // Name | Type | Dimensions | Size | Date | Rating | Tags
        const int x0 = r.left + S(60), w = RectW(r) - S(60);
        const int nameW = std::max(S(140), w - S(80 + 100 + 76 + 120 + 70) - std::max(S(120), w / 5));
        int x = x0;
        auto cell = [&](const std::wstring& text, int cw, COLORREF c, HFONT f = nullptr) {
            Text(dc, f ? f : fSmall, text, {x, r.top, x + cw - S(8), r.bottom}, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            x += cw;
        };
        cell(FileNameOf(u), nameW, sel ? theme::kText : theme::kTextDim, fUi);
        std::wstring type = MediaTypeLabel(MediaTypeOf(u));
        type.pop_back();
        cell(type, S(80), theme::kMuted);
        cell(m.w > 0 ? std::to_wstring(m.w) + L" × " + std::to_wstring(m.h) : L"—", S(100), theme::kMuted);
        cell(Bytes(m.size), S(76), theme::kMuted);
        cell(FormatLibraryDate(m.mtime), S(120), theme::kMuted);
        cell(std::wstring(m.rating, (wchar_t)0xE735), S(70), theme::kAccent, fStar);
        cell(Join(m.tags, L", "), std::max<int>(S(60), (int)(r.right - x)), theme::kMuted);
        AddHot(r, nullptr, FileNameOf(u));
        hots.back().tile = t.index;
        return;
    }
    if (auto b = TileBitmap(u, RectW(r), RectH(r), t.fill)) {
        MemDC md(b->Handle(), dc);
        BitBlt(dc, r.left, r.top, RectW(r), RectH(r), md, 0, 0, SRCCOPY);
    } else {
        FillRR(dc, r, Sf(8), A(theme::kBgRaised));
    }
    // Badges, bottom left: type or duration, stars, similarity, and the name on hover.
    std::vector<std::pair<std::wstring, HFONT>> badges;
    const MediaType mt = MediaTypeOf(u);
    if (mt == MediaType::Gif) badges.push_back({L"GIF", fBadge});
    else if (mt == MediaType::Video) badges.push_back({m.duration ? Duration(*m.duration) : L"MP4", fBadge});
    if (m.rating > 0) badges.push_back({std::wstring(m.rating, (wchar_t)0xE735), fStar});  // the icon font's star measures right
    if (auto d = model.distances.find(u); d != model.distances.end())
        badges.push_back({std::to_wstring(Library::SimilarityPercent(d->second)) + L"%", fBadge});
    const bool showName = t.index == hover && !t.caption;
    int bx = r.left + S(6);
    const int bh = S(17), by = r.bottom - S(6) - bh;
    auto badge = [&](const std::wstring& text, HFONT f, int maxW) {
        SIZE sz = Measure(dc, f, text);
        const int bw = std::min(maxW, (int)sz.cx + S(12));
        if (bw < S(20)) return;
        RECT br{bx, by, bx + bw, by + bh};
        FillRR(dc, br, RectH(br) / 2.f, gp::Color(150, 18, 18, 18));
        const bool fits = bw >= sz.cx + S(12);
        Text(dc, f, text, {br.left + S(6), br.top, fits ? br.left + S(6) + sz.cx + S(4) : br.right - S(6), br.bottom}, RGB(255, 255, 255),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | (fits ? 0u : (UINT)DT_END_ELLIPSIS));
        bx = br.right + S(4);
    };
    for (const auto& [text, font] : badges) badge(text, font, RectW(r) - S(12));
    if (showName) badge(Stem(u), fTiny, r.right - bx - S(6));
    // Selection is the only use of the accent colour; focus gets a faint ring.
    RECT ring = r;
    InflateRect(&ring, S(2), S(2));
    if (sel) StrokeRR(dc, ring, Sf(10), A(theme::kAccent), Sf(2.5f));
    else if (model.focus == u) StrokeRR(dc, ring, Sf(10), gp::Color(90, 255, 255, 255), 1);
    AddHot(r, nullptr, FileNameOf(u) + (m.tags.empty() ? L"" : L"\n" + Join(m.tags, L", ")));
    hots.back().tile = t.index;
    // Version stack badge, top right: click expands or collapses the stack.
    if (auto st = model.stacks.find(u); st != model.stacks.end()) {
        const bool open = model.IsExpanded(u);
        const std::wstring label = std::to_wstring(st->second);
        SIZE sz = Measure(dc, fBadge, label);
        RECT sb{r.right - S(6) - sz.cx - S(28), r.top + S(6), r.right - S(6), r.top + S(6) + S(19)};
        FillRR(dc, sb, RectH(sb) / 2.f, gp::Color(160, 18, 18, 18));
        Text(dc, fIconSmall, open ? L"\xE73F" : L"\xE81E", {sb.left + S(7), sb.top, sb.left + S(20), sb.bottom}, RGB(255, 255, 255),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, fBadge, label, {sb.left + S(21), sb.top, sb.right - S(6), sb.bottom}, RGB(255, 255, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const std::wstring path = u;
        AddHot(sb, [this, path] {
            model.ToggleStack(path);
            Changed();
        }, open ? L"Collapse versions" : std::to_wstring(st->second) + L" versions: the original and its edits. Click to show all.");
    }
    if (t.caption) {
        RECT cr{r.left, r.bottom + S(4), r.right, r.bottom + S(20)};
        Text(dc, fTiny, FileNameOf(u), cr, sel ? theme::kText : theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
}

void Gallery::PaintEmpty(HDC dc, const RECT& c) {
    const int cy = (c.top + c.bottom) / 2;
    const bool none = model.lib.Paths().empty();
    Text(dc, fBig, none ? L"\xE722" : L"\xE71C", {c.left, cy - S(70), c.right, cy - S(20)}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    std::wstring title = none ? L"No captures yet" : model.scope().kind == ScopeKind::Duplicates ? L"No duplicates found" : L"Nothing matches";
    Text(dc, fTitle, title, {c.left, cy - S(12), c.right, cy + S(12)}, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (none) {
        Text(dc, fSmall, L"Press " + host.regionHotkey + L" to capture a region, or drop images here to import them.",
             {c.left, cy + S(14), c.right, cy + S(36)}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (!model.filter().IsEmpty()) {
        const std::wstring label = L"Clear filters";
        SIZE sz = Measure(dc, fSmall, label);
        RECT b{(c.left + c.right) / 2 - sz.cx / 2 - S(12), cy + S(18), (c.left + c.right) / 2 + sz.cx / 2 + S(12), cy + S(44)};
        FillRR(dc, b, RectH(b) / 2.f, gp::Color(26, 255, 255, 255));
        Text(dc, fSmall, label, b, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(b, [this] { SetFilter(Filter()); });
    }
}

void Gallery::PaintBrowser(HDC dc, const RECT& c) {
    if (model.visible.empty()) return PaintEmpty(dc, c);
    const int dy = c.top - scrollY;
    HRGN clip = CreateRectRgn(c.left, c.top, c.right, c.bottom);
    SelectClipRgn(dc, clip);
    const ScopeKind kind = model.scope().kind;
    if (kind == ScopeKind::Duplicates) {
        const int width = RectW(c) - 2 * S(kPadX);
        int y = ContentTop() + S(8) + dy;
        Text(dc, fSmall,
             std::to_wstring(model.groups.size()) + (model.groups.size() == 1 ? L" group" : L" groups") +
                 L" of identical or near-identical captures. The newest copy is first.",
             {c.left + S(kPadX), y, c.right - S(kPadX), y + S(22)}, theme::kTextDim);
        // Group panels behind the tiles.
        for (int gi = 0; gi < (int)model.groups.size(); ++gi) {
            int top = INT_MAX, bottom = 0;
            for (const auto& t : tileRects)
                if (t.group == gi) top = std::min(top, (int)t.r.top), bottom = std::max(bottom, (int)t.r.bottom);
            RECT panel{c.left + S(kPadX), top + dy - S(40), c.left + S(kPadX) + width, bottom + dy + S(34)};
            if (panel.bottom < c.top || panel.top > c.bottom) continue;
            FillRR(dc, panel, Sf(10), A(theme::kSurface));
            const auto& g = model.groups[gi];
            int64_t reclaim = 0;
            for (size_t k = 1; k < g.size(); ++k) reclaim += model.lib.Meta(g[k]).size;
            RECT hr{panel.left + S(12), panel.top + S(10), panel.right - S(12), panel.top + S(34)};
            Text(dc, fBold, std::to_wstring(g.size()) + L" copies", hr, theme::kText);
            SIZE cs = Measure(dc, fBold, std::to_wstring(g.size()) + L" copies");
            Text(dc, fTiny, Bytes(reclaim) + L" reclaimable", {hr.left + cs.cx + S(10), hr.top, hr.right, hr.bottom}, theme::kMuted);
            const std::wstring label = L"Keep newest, trash " + std::to_wstring(g.size() - 1);
            SIZE ls = Measure(dc, fSmall, label);
            RECT b{hr.right - ls.cx - S(20), hr.top, hr.right, hr.bottom};
            FillRR(dc, b, RectH(b) / 2.f, gp::Color(26, 255, 255, 255));
            Text(dc, fSmall, label, b, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            std::vector<std::wstring> rest(g.begin() + 1, g.end());
            AddHot(b, [this, rest] { Trash(&rest); });
        }
    }
    if (kind == ScopeKind::Similar) {
        const int y = ContentTop() + S(8) + dy;
        Text(dc, fIcon, L"\xE71E", {c.left + S(kPadX), y, c.left + S(kPadX) + S(20), y + S(26)}, theme::kAccent);
        Text(dc, fSmall, L"Most similar first. Percentages are visual similarity.", {c.left + S(kPadX) + S(26), y, c.right - S(140), y + S(26)},
             theme::kTextDim);
        const std::wstring label = L"Back to all";
        SIZE ls = Measure(dc, fSmall, label);
        RECT b{c.right - S(kPadX) - ls.cx - S(20), y + S(1), c.right - S(kPadX), y + S(25)};
        FillRR(dc, b, RectH(b) / 2.f, gp::Color(26, 255, 255, 255));
        Text(dc, fSmall, label, b, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(b, [this] { SetScope(Scope()); });
    }
    if (model.layout == GalleryLayout::List && kind != ScopeKind::Duplicates) {
        // Column headings, under the toolbar.
        const int y = ContentTop() + S(8) + (kind == ScopeKind::Similar ? S(34) : 0) + dy;
        const int x0 = c.left + S(kPadX) + S(60), w = RectW(c) - 2 * S(kPadX) - S(60);
        const int nameW = std::max(S(140), w - S(80 + 100 + 76 + 120 + 70) - std::max(S(120), w / 5));
        int x = x0;
        for (auto [label, cw] : std::vector<std::pair<const wchar_t*, int>>{{L"Name", nameW}, {L"Type", S(80)}, {L"Dimensions", S(100)},
                                                                               {L"Size", S(76)}, {L"Date", S(120)}, {L"Rating", S(70)},
                                                                               {L"Tags", S(200)}}) {
            Text(dc, fHead, label, {x, y, x + cw, y + S(kListHeadH)}, theme::kMuted);
            x += cw;
        }
        FillSolid(dc, {c.left + S(kPadX), y + S(kListHeadH) - 1, c.right - S(kPadX), y + S(kListHeadH)}, theme::kBorder);
    }
    for (const auto& t : tileRects) {
        if (t.r.bottom + dy + S(22) < c.top || t.r.top + dy > c.bottom) continue;
        TileRect shifted = t;
        OffsetRect(&shifted.r, c.left, 0);
        PaintTile(dc, shifted, dy);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    // Overlay scrollbar.
    const int view = RectH(c);
    if (contentH > view) {
        const int th = std::max(S(30), view * view / contentH);
        const int ty = c.top + (view - th) * scrollY / std::max(1, contentH - view);
        RECT bar{c.right - S(7), ty + S(2), c.right - S(3), ty + th - S(2)};
        FillRR(dc, bar, Sf(2), gp::Color(draggingScroll ? 120 : 60, 255, 255, 255));
        RECT grab{c.right - S(12), c.top, c.right, c.bottom};
        Hot h;
        h.r = grab;
        h.cursor = IDC_ARROW;
        h.click = nullptr;
        h.tile = -2;  // the scrollbar
        hots.push_back(h);
    }
}

// ---------- chrome ----------

RECT Gallery::GlassButton(HDC dc, int x, int y, const std::wstring& glyph, bool on, std::function<void()> click, const std::wstring& tip,
                          int minW, const std::wstring& label) {
    int w = S(minW);
    SIZE ls{};
    if (!label.empty()) {
        ls = Measure(dc, fUi, label);
        w = std::max(w, (int)ls.cx + S(glyph.empty() ? 20 : 40));
    }
    RECT r{x, y, x + w, y + S(28)};
    POINT m;
    GetCursorPos(&m);
    ScreenToClient(hwnd, &m);
    const bool hov = PtInRect(&r, m);
    if (on || hov) FillRR(dc, r, RectH(r) / 2.f, gp::Color(on ? 31 : 20, 255, 255, 255));
    if (label.empty()) {
        Text(dc, fIcon, glyph, r, on ? theme::kText : theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        int tx = r.left + S(10);
        if (!glyph.empty()) {
            Text(dc, fIcon, glyph, {tx, r.top, tx + S(18), r.bottom}, on ? theme::kText : theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            tx += S(22);
        }
        Text(dc, fUi, label, {tx, r.top, r.right - S(8), r.bottom}, on ? theme::kText : theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    AddHot(r, std::move(click), tip);
    return r;
}

RECT Gallery::Chip(HDC dc, int x, int y, const std::wstring& label, bool on, bool dashed, std::function<void()> click,
                   std::function<void()> right, const std::wstring& tip, std::function<void()> dbl) {
    SIZE sz = Measure(dc, fTiny, label);
    RECT r{x, y, x + sz.cx + S(16), y + S(22)};
    if (dashed) StrokeRR(dc, r, RectH(r) / 2.f, gp::Color(70, 255, 255, 255), std::max(1.f, Sf(0.8f)), true);
    else FillRR(dc, r, RectH(r) / 2.f, gp::Color(on ? 50 : 20, 255, 255, 255));
    Text(dc, fTiny, label, r, on || !dashed ? theme::kText : theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    AddHot(r, std::move(click), tip, std::move(right), std::move(dbl));
    return r;
}

void Gallery::PaintTopBar(HDC dc, const RECT& c) {
    const bool wide = Typing() || !model.filter().text.empty();
    const int searchW = S(wide ? 340 : 240);
    // Measure: [sidebar][scope?][search][filter][more][progress?]
    const std::wstring title = model.Title();
    SIZE ts = Measure(dc, fBold, title);
    const int scopeW = model.showSidebar ? 0 : std::min(S(200), (int)ts.cx) + S(32);
    const auto prog = model.lib.Progress();
    const int total = S(30) + scopeW + S(2) + searchW + S(4) + S(30) + S(2) + S(30) + (prog.second > 0 ? S(34) : 0);
    const int barW = total + S(8);
    const int x0 = std::max<int>(c.left + S(12), (c.left + c.right) / 2 - barW / 2);
    RECT bar{x0, S(8), x0 + barW, S(8) + S(36)};
    Glass(dc, bar, Sf(18), s);
    AddHot(bar, nullptr, L"", nullptr, nullptr, IDC_ARROW);  // its empty parts aren't the tiles underneath
    int x = bar.left + S(4);
    const int y = bar.top + S(4);
    x = GlassButton(dc, x, y, L"\xE90C", model.showSidebar, [this] { ToggleSidebar(); },
                    model.showSidebar ? L"Hide sidebar (Ctrl+B)" : L"Show sidebar (Ctrl+B)").right + S(2);
    if (!model.showSidebar) {
        scopeBtn = {x, y, x + scopeW, y + S(28)};
        POINT m;
        GetCursorPos(&m);
        ScreenToClient(hwnd, &m);
        if (PtInRect(&scopeBtn, m)) FillRR(dc, scopeBtn, Sf(14), gp::Color(20, 255, 255, 255));
        Text(dc, fBold, title, {x + S(10), y, scopeBtn.right - S(20), y + S(28)}, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, fIconSmall, L"\xE70D", {scopeBtn.right - S(20), y, scopeBtn.right - S(6), y + S(28)}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(scopeBtn, [this] { ScopeMenu(); }, std::to_wstring(model.lib.Paths().size()) + L" captures in your library");
        x = scopeBtn.right + S(2);
    }
    // Search pill (the EDIT control sits inside it).
    RECT pill{x, y, x + searchW, y + S(28)};
    FillRR(dc, pill, RectH(pill) / 2.f, A(RGB(44, 45, 42)));
    Text(dc, fIcon, L"\xE721", {pill.left + S(10), pill.top, pill.left + S(28), pill.bottom}, theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int editL = pill.left + S(32), editR = pill.right - (model.filter().text.empty() ? S(12) : S(30));
    const int eh = S(18);
    SetWindowPos(search, nullptr, editL, (pill.top + pill.bottom) / 2 - eh / 2, editR - editL, eh, SWP_NOZORDER | SWP_NOACTIVATE | (OverlayUp() ? 0 : SWP_SHOWWINDOW));
    if (!model.filter().text.empty()) {
        RECT xr{pill.right - S(28), pill.top, pill.right - S(6), pill.bottom};
        Text(dc, fIconSmall, L"\xE711", xr, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(xr, [this] {
            Filter f = model.filter();
            f.text.clear();
            SetFilter(f);
        }, L"Clear search");
    }
    AddHot(pill, [this] { SetFocus(search); }, L"", nullptr, nullptr, IDC_IBEAM);
    x = pill.right + S(4);
    Filter rest = model.filter();
    rest.text.clear();
    filterBtn = GlassButton(dc, x, y, L"\xE71C", filterOpen, [this] {
        filterOpen = !filterOpen;
        Changed(false);
    }, L"Filters");
    if (!rest.IsEmpty()) {  // a small dot while filters are on
        RECT dot{filterBtn.right - S(10), filterBtn.top + S(5), filterBtn.right - S(4), filterBtn.top + S(11)};
        FillRR(dc, dot, Sf(3), A(theme::kAccent));
    }
    x = filterBtn.right + S(2);
    viewBtn = GlassButton(dc, x, y, L"\xE712", false, [this] { ViewMenu(); }, L"View options");
    x = viewBtn.right;
    if (prog.second > 0) {
        RECT pr{x + S(6), y + S(5), x + S(24), y + S(23)};
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::Pen track(gp::Color(40, 255, 255, 255), Sf(2.2f));
        gp::Pen arc(A(theme::kTextDim), Sf(2.2f));
        g.DrawEllipse(&track, (float)pr.left, (float)pr.top, (float)RectW(pr), (float)RectH(pr));
        const float sweep = 360.f * prog.first / std::max(1, prog.second);
        g.DrawArc(&arc, (float)pr.left, (float)pr.top, (float)RectW(pr), (float)RectH(pr), -90, std::max(12.f, sweep));
        AddHot(pr, nullptr, L"Indexing " + std::to_wstring(prog.first) + L" of " + std::to_wstring(prog.second) + L"…", nullptr, nullptr, IDC_ARROW);
    }
    if (HasChipsRow()) PaintChips(dc, c, bar.bottom + S(8));
}

// Chips for the filters in effect, under the toolbar; or the note about related matches.
void Gallery::PaintChips(HDC dc, const RECT& c, int y) {
    const Filter& f = model.filter();
    Filter rest = f;
    rest.text.clear();
    if (rest.IsEmpty()) {
        const std::wstring note = model.relatedCount == (int)model.visible.size()
                                      ? L"No exact matches; showing " + std::to_wstring(model.relatedCount) + L" related by meaning"
                                      : L"Plus " + std::to_wstring(model.relatedCount) + L" related by meaning, after the exact matches";
        SIZE sz = Measure(dc, fTiny, note);
        RECT r{(c.left + c.right) / 2 - sz.cx / 2 - S(10), y, (c.left + c.right) / 2 + sz.cx / 2 + S(10), y + S(26)};
        Glass(dc, r, Sf(12), s);
        Text(dc, fTiny, note, r, theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    std::vector<std::pair<std::wstring, std::function<void(Filter&)>>> items;
    if (!f.types.empty()) {
        std::vector<std::wstring> names;
        for (auto t : f.types) names.push_back(MediaTypeLabel(t));
        items.push_back({Join(names, L", "), [](Filter& x) { x.types.clear(); }});
    }
    if (f.untagged) items.push_back({L"Untagged", [](Filter& x) { x.untagged = false; }});
    for (const auto& t : f.tags)
        items.push_back({L"#" + t, [t](Filter& x) { x.tags.erase(std::remove(x.tags.begin(), x.tags.end(), t), x.tags.end()); }});
    if (f.minRating > 0) items.push_back({Stars(f.minRating) + L"+", [](Filter& x) { x.minRating = 0; }});
    if (!f.color.empty()) items.push_back({L"Color " + f.color, [](Filter& x) { x.color.clear(); }});
    if (f.shape != ShapeFilter::Any) items.push_back({ShapeLabel(f.shape), [](Filter& x) { x.shape = ShapeFilter::Any; }});
    if (f.minWidth > 0 || f.minHeight > 0)
        items.push_back({L"≥ " + std::to_wstring(f.minWidth) + L" × " + std::to_wstring(f.minHeight), [](Filter& x) { x.minWidth = x.minHeight = 0; }});
    if (f.date != DateFilter::Any) items.push_back({DateLabel(f.date), [](Filter& x) { x.date = DateFilter::Any; }});
    if (f.size != SizeFilter::Any) items.push_back({SizeLabel(f.size), [](Filter& x) { x.size = SizeFilter::Any; }});
    for (const auto& a : f.apps) items.push_back({a, [a](Filter& x) { x.apps.erase(std::remove(x.apps.begin(), x.apps.end(), a), x.apps.end()); }});
    const bool smart = model.scope().kind == ScopeKind::Smart;
    const std::wstring saveLabel = smart ? L"Update smart folder" : L"Save as smart folder";
    int total = 0;
    for (const auto& it : items) total += Measure(dc, fTiny, it.first).cx + S(36) + S(6);
    total += Measure(dc, fTiny, saveLabel).cx + S(8);
    int x = std::max<int>(c.left + S(12), (c.left + c.right) / 2 - total / 2);
    for (const auto& it : items) {
        SIZE sz = Measure(dc, fTiny, it.first);
        RECT r{x, y, x + sz.cx + S(36), y + S(26)};
        Glass(dc, r, Sf(12), s);
        Text(dc, fTiny, it.first, {r.left + S(10), r.top, r.right - S(22), r.bottom}, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, fIconSmall, L"\xE711", {r.right - S(22), r.top, r.right - S(8), r.bottom}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        auto remove = it.second;
        AddHot(r, [this, remove] {
            Filter nf = model.filter();
            remove(nf);
            SetFilter(nf);
        }, L"Remove filter");
        x = r.right + S(6);
    }
    SIZE ss = Measure(dc, fTiny, saveLabel);
    RECT sr{x + S(4), y, x + S(4) + ss.cx, y + S(26)};
    Text(dc, fTiny, saveLabel, sr, theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    AddHot(sr, [this] { SaveSmartFolder(); });
}

void Gallery::PaintActionBar(HDC dc, const RECT& c) {
    if (model.selection.empty()) return;
    const int n = (int)model.selection.size();
    const std::wstring count = n == 1 ? L"1 selected" : std::to_wstring(n) + L" selected";
    struct Act {
        const wchar_t* glyph;
        std::wstring tip;
        std::function<void()> run;
    };
    std::vector<Act> acts = {{L"\xE8C8", L"Copy (Ctrl+C)", [this] { Copy(); }},
                             {L"\xE8EC", L"Add tags (T)", [this] { TagPicker(); }},
                             {L"\xE8F4", L"Add to collection (F)", [this] { CollectionPicker(); }},
                             {L"\xE734", L"Rate (0–5)", [this] {
                                  HMENU m = CreatePopupMenu();
                                  for (int r = 0; r <= 5; ++r) AppendMenuW(m, MF_STRING, r + 1, r == 0 ? L"No rating" : Stars(r).c_str());
                                  POINT p;
                                  GetCursorPos(&p);
                                  if (int id = Menu(m, p)) Rate(id - 1);
                              }}};
    if (n == 2) acts.push_back({L"\xE8AB", L"Compare (C)", [this] { Compare(); }});
    if (n > 1) acts.push_back({L"\xF0E2", L"Make collage (Ctrl+G)", [this] { Collage(); }});
    acts.push_back({L"\xE718", L"Pin to screen (Ctrl+P)", [this] { Pin(); }});
    acts.push_back({L"\xE898", L"Upload and copy link (Ctrl+U)", [this] { Upload(); }});
    acts.push_back({L"\xE74D", L"Move to the Recycle Bin (Del)", [this] { Trash(); }});
    const SIZE cs = Measure(dc, fSmall, count);
    const bool hint = !model.inspectorDiscovered;
    const int detailsW = hint ? Measure(dc, fUi, L"Details").cx + Measure(dc, fTiny, L"Ctrl+I").cx + S(60) : S(30);
    const int w = S(4) + S(30) + S(4) + cs.cx + S(10) + S(9) + (int)acts.size() * S(32) + S(9) + detailsW + S(4);
    const int x0 = (c.left + c.right) / 2 - w / 2, y0 = c.bottom - S(14) - S(36);
    RECT bar{x0, y0, x0 + w, y0 + S(36)};
    Glass(dc, bar, Sf(18), s);
    AddHot(bar, nullptr, L"", nullptr, nullptr, IDC_ARROW);  // its empty parts aren't the tiles underneath
    int x = bar.left + S(4);
    const int y = bar.top + S(4);
    x = GlassButton(dc, x, y, L"\xE711", false, [this] {
        model.selection.clear();
        Changed(false);
    }, L"Deselect (Esc)").right + S(4);
    Text(dc, fSmall, count, {x, y, x + cs.cx, y + S(28)}, theme::kTextDim);
    x += cs.cx + S(10);
    FillSolid(dc, {x + S(4), y + S(5), x + S(5), y + S(23)}, RGB(60, 61, 58));
    x += S(9);
    for (auto& a : acts) x = GlassButton(dc, x, y, a.glyph, false, a.run, a.tip).right + S(2);
    FillSolid(dc, {x + S(4), y + S(5), x + S(5), y + S(23)}, RGB(60, 61, 58));
    x += S(9);
    // First-run hint: "Details Ctrl+I" until the inspector has been opened once, then just an icon.
    RECT d = GlassButton(dc, x, y, L"\xE946", model.showInspector, [this] { ToggleInspector(); },
                         model.showInspector ? L"Hide details (Ctrl+I)" : L"Show details (Ctrl+I)", hint ? (int)(detailsW / s) : 30, hint ? L"Details" : L"");
    if (hint) {
        SIZE ks = Measure(dc, fTiny, L"Ctrl+I");
        RECT kr{d.right - ks.cx - S(16), d.top + S(6), d.right - S(6), d.bottom - S(6)};
        StrokeRR(dc, kr, Sf(4), gp::Color(50, 255, 255, 255), 1);
        Text(dc, fTiny, L"Ctrl+I", kr, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// ---------- sidebar ----------

void Gallery::PaintSidebar(HDC dc, const RECT& r) {
    FillSolid(dc, r, RGB(22, 23, 21));
    FillSolid(dc, {r.right, r.top, r.right + 1, r.bottom}, RGB(36, 37, 34));
    HRGN clip = CreateRectRgn(r.left, r.top, r.right, r.bottom - S(44));
    SelectClipRgn(dc, clip);
    int y = S(56) - sideScroll;
    auto header = [&](const std::wstring& t, std::function<void()> add, bool* collapsed) {
        y += S(14);
        RECT hr{r.left + S(16), y, r.right - S(12), y + S(18)};
        Text(dc, fHead, t, hr, theme::kMuted);
        if (collapsed) {
            SIZE sz = Measure(dc, fHead, t);
            Text(dc, fIconSmall, *collapsed ? L"\xE76C" : L"\xE70D", {hr.left + sz.cx + S(4), hr.top, hr.left + sz.cx + S(18), hr.bottom}, theme::kMuted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            AddHot(hr, [this, collapsed] {
                *collapsed = !*collapsed;
                WritePrivateProfileStringW(L"View", L"TagsCollapsed", tagsCollapsed ? L"1" : L"0", (SupportFolder() + L"\\gallery.ini").c_str());
                Changed(false);
            });
        }
        if (add) {
            RECT ar{hr.right - S(18), hr.top, hr.right, hr.bottom};
            Text(dc, fIconSmall, L"\xE710", ar, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            AddHot(ar, add, L"New");
        }
        y += S(22);
    };
    auto row = [&](const Scope& sc, const std::wstring& title, const wchar_t* glyph, std::function<void()> right) {
        RECT rr{r.left + S(6), y, r.right - S(6), y + S(28)};
        const bool on = model.scope() == sc;
        POINT m;
        GetCursorPos(&m);
        ScreenToClient(hwnd, &m);
        if (on || PtInRect(&rr, m)) FillRR(dc, rr, Sf(6), gp::Color(on ? 26 : 12, 255, 255, 255));
        Text(dc, fIcon, glyph, {rr.left + S(10), rr.top, rr.left + S(28), rr.bottom}, on ? theme::kText : theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, fUi, title, {rr.left + S(34), rr.top, rr.right - S(8), rr.bottom}, on ? theme::kText : theme::kTextDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        AddHot(rr, [this, sc] { SetScope(sc); }, L"", right);
        y += S(30);
        return rr;
    };
    header(L"Library", nullptr, nullptr);
    row(Scope(), L"All captures", L"\xE8A9", nullptr);
    row(Scope::Of(ScopeKind::Recent), L"Last 7 days", L"\xE823", nullptr);
    row(Scope::Of(ScopeKind::Rated), L"Rated", L"\xE734", nullptr);
    row(Scope::Of(ScopeKind::Uncategorized), L"Uncategorized", L"\xE7B8", nullptr);
    row(Scope::Of(ScopeKind::Duplicates), L"Duplicates", L"\xE8C8", nullptr);
    if (!model.lib.Collections().empty()) header(L"Collections", [this] { NewCollection(); }, nullptr);
    for (const auto& col : model.lib.Collections()) {
        const std::wstring id = col.id, name = col.name, autoTags = Join(col.autoTags, L", ");
        RECT dropRow = row(Scope::OfCollection(id), name, col.autoTags.empty() ? L"\xE8B7" : L"\xE8F4", [this, id, name, autoTags] {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Rename…");
            AppendMenuW(m, MF_STRING, 2, L"Auto tags…");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, L"Delete collection (keeps the files)");
            POINT p;
            GetCursorPos(&p);
            switch (Menu(m, p)) {
                case 1:
                    ShowTextPrompt(L"Rename collection", name, [this, id](std::wstring n) {
                        if (!n.empty()) model.lib.RenameCollection(id, n);
                    });
                    break;
                case 2:
                    ShowTextPrompt(L"Tags added to everything dropped into “" + name + L"” (comma-separated)", autoTags,
                                   [this, id](std::wstring t) { model.lib.SetAutoTags(id, SplitTags(t)); });
                    break;
                case 3:
                    if (model.scope() == Scope::OfCollection(id)) SetScope(Scope());
                    model.lib.DeleteCollection(id);
                    break;
            }
        });
        collectionDrops.push_back({dropRow, id});
    }
    if (!model.lib.SmartFolders().empty()) header(L"Smart folders", [this] { SaveSmartFolder(); }, nullptr);
    for (const auto& sf : model.lib.SmartFolders()) {
        const std::wstring id = sf.id, name = sf.name;
        row(Scope::OfSmart(id), name, L"\xE838", [this, id, name] {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Rename…");
            AppendMenuW(m, MF_STRING, 2, L"Update with current filters");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 3, L"Delete smart folder");
            POINT p;
            GetCursorPos(&p);
            switch (Menu(m, p)) {
                case 1:
                    ShowTextPrompt(L"Rename smart folder", name, [this, id](std::wstring n) {
                        if (!n.empty()) model.lib.UpdateSmartFolder(id, nullptr, &n);
                    });
                    break;
                case 2: {
                    Filter f = model.filter();
                    model.lib.UpdateSmartFolder(id, &f, nullptr);
                    break;
                }
                case 3:
                    if (model.scope() == Scope::OfSmart(id)) SetScope(Scope());
                    model.lib.DeleteSmartFolder(id);
                    break;
            }
        });
    }
    const auto tags = model.lib.AllTags();
    if (!tags.empty()) {
        header(L"Tags", nullptr, &tagsCollapsed);
        if (!tagsCollapsed) {
            int x = r.left + S(12);
            const size_t shown = showAllTags ? tags.size() : std::min<size_t>(tags.size(), 24);
            for (size_t i = 0; i < shown; ++i) {
                const std::wstring t = tags[i].first;
                const std::wstring label = t + L"  " + std::to_wstring(tags[i].second);
                const bool on = std::any_of(model.filter().tags.begin(), model.filter().tags.end(),
                                            [&](const std::wstring& x2) { return LowerText(x2) == LowerText(t); });
                SIZE sz = Measure(dc, fTiny, label);
                if (x + sz.cx + S(16) > r.right - S(10) && x > r.left + S(12)) {
                    x = r.left + S(12);
                    y += S(26);
                }
                RECT cr = Chip(dc, x, y, label, on, false, [this, t] {
                    Filter f = model.filter();
                    auto it = std::find_if(f.tags.begin(), f.tags.end(), [&](const std::wstring& x2) { return LowerText(x2) == LowerText(t); });
                    if (it != f.tags.end()) f.tags.erase(it);
                    else f.tags.push_back(t);
                    SetFilter(f);
                }, [this, t] {
                    HMENU m = CreatePopupMenu();
                    AppendMenuW(m, MF_STRING, 1, L"Rename tag…");
                    AppendMenuW(m, MF_STRING, 2, L"Delete tag from all captures");
                    POINT p;
                    GetCursorPos(&p);
                    const int id = Menu(m, p);
                    if (id == 1)
                        ShowTextPrompt(L"Rename tag “" + t + L"” everywhere", t, [this, t](std::wstring n) {
                            if (!n.empty()) model.lib.RenameTag(t, n);
                        });
                    else if (id == 2)
                        model.lib.DeleteTag(t);
                });
                x = cr.right + S(5);
            }
            y += S(30);
            if (tags.size() > 24) {
                const std::wstring more = showAllTags ? L"Fewer" : L"All " + std::to_wstring(tags.size()) + L" tags";
                RECT mr{r.left + S(14), y, r.left + S(14) + Measure(dc, fTiny, more).cx, y + S(18)};
                Text(dc, fTiny, more, mr, theme::kAccent);
                AddHot(mr, [this] {
                    showAllTags = !showAllTags;
                    Changed(false);
                });
                y += S(22);
            }
        }
    }
    sideH = y + sideScroll + S(20);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    // A single "+" at the bottom creates a collection or a smart folder.
    RECT plus{r.left + S(8), r.bottom - S(40), r.left + S(38), r.bottom - S(12)};
    FillSolid(dc, {r.left, r.bottom - S(44), r.right, r.bottom}, RGB(22, 23, 21));
    GlassButton(dc, plus.left, plus.top, L"\xE710", false, [this] {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"New collection…");
        AppendMenuW(m, MF_STRING | (model.filter().IsEmpty() ? MF_GRAYED : 0), 2,
                    model.filter().IsEmpty() ? L"New smart folder (set filters first)" : L"New smart folder from filters");
        POINT p;
        GetCursorPos(&p);
        const int id = Menu(m, p);
        if (id == 1) NewCollection();
        if (id == 2) SaveSmartFolder();
    }, L"New collection or smart folder");
}

void Gallery::NewCollection() {
    ShowTextPrompt(L"New collection", L"", [this](std::wstring name) {
        if (!name.empty()) SetScope(Scope::OfCollection(model.lib.CreateCollection(name).id));
    });
}

}  // namespace
}  // namespace ather

namespace ather {
namespace {

// ---------- inspector ----------

int WrapText(HDC dc, HFONT f, const std::wstring& t, RECT r, COLORREF c, bool draw) {
    HGDIOBJ o = SelectObject(dc, f);
    RECT calc = r;
    DrawTextW(dc, t.c_str(), (int)t.size(), &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    if (draw) {
        SetTextColor(dc, c);
        SetBkMode(dc, TRANSPARENT);
        RECT d{r.left, r.top, r.right, r.top + RectH(calc)};
        DrawTextW(dc, t.c_str(), (int)t.size(), &d, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    }
    SelectObject(dc, o);
    return RectH(calc);
}

void Gallery::PaintInspector(HDC dc, const RECT& r) {
    FillSolid(dc, r, theme::kSurface);
    FillSolid(dc, {r.left - 1, r.top, r.left, r.bottom}, RGB(36, 37, 34));
    GlassButton(dc, r.right - S(38), S(12), L"\xE711", false, [this] { ToggleInspector(); }, L"Hide details (Ctrl+I)");
    const RECT body{r.left, S(52), r.right, r.bottom};
    HRGN clip = CreateRectRgn(body.left, body.top, body.right, body.bottom);
    SelectClipRgn(dc, clip);
    const size_t firstBodyHot = hots.size();  // clipped to the body below: scrolled-out controls can't take clicks
    const int L = r.left + S(14), R = r.right - S(14);
    int y = body.top - inspScroll;
    bool tagEditShown = false, commentShown = false;
    auto section = [&](const std::wstring& t) {
        Text(dc, fHead, t, {L, y, R, y + S(18)}, theme::kMuted);
        y += S(22);
    };
    auto info = [&](const std::wstring& k, const std::wstring& v) {
        Text(dc, fTiny, k, {L, y, L + S(82), y + S(18)}, theme::kMuted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        const int h = std::min(S(48), WrapText(dc, fTiny, v, {L + S(82), y, R, y + S(400)}, theme::kTextDim, false));
        WrapText(dc, fTiny, v, {L + S(82), y, R, y + h}, theme::kTextDim, true);
        y += std::max(S(18), h) + S(4);
    };
    auto link = [&](const std::wstring& k, const std::wstring& path) {
        if (!k.empty()) Text(dc, fTiny, k, {L, y, L + S(82), y + S(18)}, theme::kMuted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        const std::wstring name = Stem(path);
        RECT lr{L + S(82), y, R, y + S(18)};
        Text(dc, fTiny, name, lr, theme::kAccent, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        lr.right = std::min(lr.right, lr.left + (int)Measure(dc, fTiny, name).cx);
        AddHot(lr, [this, path] { Reveal(path); }, path);
        y += S(20);
    };
    auto stars = [&](int rating, std::function<void(int)> set) {
        for (int i = 1; i <= 5; ++i) {
            RECT sr{L + (i - 1) * S(18), y, L + i * S(18), y + S(18)};
            Text(dc, fIcon, i <= rating ? L"\xE735" : L"\xE734", sr, i <= rating ? theme::kAccent : RGB(80, 80, 76), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            AddHot(sr, [set, i, rating] { set(i == rating ? 0 : i); }, i == rating ? L"Clear rating" : Stars(i));
        }
        y += S(26);
    };
    auto ghost = [&](int x, const std::wstring& glyph, const std::wstring& label, std::function<void()> run) {
        return GlassButton(dc, x, y, glyph, false, std::move(run), L"", 30, label).right + S(4);
    };
    // A chip with an × that removes it; double-click shows everything with it.
    auto removable = [&](int& x, const std::wstring& label, const wchar_t* glyph, std::function<void()> open, std::function<void()> remove) {
        SIZE sz = Measure(dc, fTiny, label);
        const int iconW = glyph ? S(16) : 0;
        RECT cr{x, y, x + S(10) + iconW + sz.cx + S(22), y + S(22)};
        if (cr.right > R && x > L) {
            x = L;
            y += S(26);
            cr = {x, y, x + S(10) + iconW + sz.cx + S(22), y + S(22)};
        }
        FillRR(dc, cr, RectH(cr) / 2.f, gp::Color(22, 255, 255, 255));
        if (glyph) Text(dc, fIconSmall, glyph, {cr.left + S(8), cr.top, cr.left + S(8) + iconW, cr.bottom}, theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, fTiny, label, {cr.left + S(8) + iconW, cr.top, cr.right - S(18), cr.bottom}, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT xr{cr.right - S(18), cr.top, cr.right - S(4), cr.bottom};
        AddHot(cr, nullptr, L"Double-click to show all", nullptr, std::move(open));
        Text(dc, fIconSmall, L"\xE711", xr, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(xr, std::move(remove), L"Remove");
        x = cr.right + S(5);
    };
    auto tagField = [&](const std::vector<std::wstring>& targets) {
        RECT fr{L, y, R, y + S(28)};
        FillRR(dc, fr, Sf(6), A(RGB(44, 45, 42)));
        SetWindowPos(tagEdit, nullptr, fr.left + S(8), fr.top + S(5), RectW(fr) - S(16), S(18), SWP_NOZORDER | SWP_NOACTIVATE | (OverlayUp() ? 0 : SWP_SHOWWINDOW));
        tagEditShown = true;
        y = fr.bottom + S(6);
        // Matching existing tags under the field.
        wchar_t buf[256] = {};
        GetWindowTextW(tagEdit, buf, 256);
        std::wstring typed = buf;
        const size_t comma = typed.find_last_of(L',');
        std::wstring q = LowerText(comma == std::wstring::npos ? typed : typed.substr(comma + 1));
        while (!q.empty() && q[0] == L' ') q.erase(0, 1);
        if (!q.empty()) {
            std::unordered_set<std::wstring> have;
            for (const auto& t : targets)
                for (const auto& tg : model.lib.Meta(t).tags) have.insert(LowerText(tg));
            int x = L, shown = 0;
            for (const auto& [t, n] : model.lib.AllTags()) {
                const std::wstring lt = LowerText(t);
                if (lt.compare(0, q.size(), q) != 0 || have.count(lt) || shown >= 6) continue;
                ++shown;
                x = Chip(dc, x, y, t, false, true, [this, t, targets] {
                    model.lib.AddTags({t}, targets);
                    SetWindowTextW(tagEdit, L"");
                }).right + S(4);
            }
            if (shown) y += S(28);
        }
    };

    const std::wstring single = !model.Single().empty() ? model.Single() : model.selection.empty() ? model.focus : L"";
    if (!single.empty()) {
        const std::wstring u = single;
        const ItemMeta& m = model.lib.Meta(u);
        const int nh = std::min(S(36), WrapText(dc, fBold, Stem(u), {L, y, R, y + S(200)}, theme::kText, false));
        RECT nr{L, y, R, y + nh};
        WrapText(dc, fBold, Stem(u), nr, theme::kText, true);
        AddHot(nr, nullptr, L"Double-click to rename", nullptr, [this] { Rename(); }, IDC_ARROW);
        y += nh + S(2);
        std::vector<std::wstring> parts;
        if (m.w > 0) parts.push_back(std::to_wstring(m.w) + L" \u00d7 " + std::to_wstring(m.h));
        if (m.duration && MediaTypeOf(u) != MediaType::Image) parts.push_back(Duration(*m.duration));
        parts.push_back(Bytes(m.size));
        parts.push_back(FormatLibraryDate(m.mtime));
        Text(dc, fTiny, Join(parts, L"  \u00b7  "), {L, y, R, y + S(18)}, theme::kMuted);
        y += S(26);
        stars(m.rating, [this, u](int r) { model.lib.SetRating(r, {u}); });
        // Tags and collections the capture is in.
        std::vector<const LibCollection*> members;
        for (const auto& c : model.lib.Collections())
            if (std::find(m.collections.begin(), m.collections.end(), c.id) != m.collections.end()) members.push_back(&c);
        if (!m.tags.empty() || !members.empty()) {
            int x = L;
            for (const auto& t : m.tags)
                removable(x, t, nullptr, [this, t] {
                    Filter f;
                    f.tags = {t};
                    SetScope(Scope());
                    SetFilter(f);
                }, [this, t, u] { model.lib.RemoveTag(t, {u}); });
            for (const LibCollection* c : members) {
                const std::wstring id = c->id;
                removable(x, c->name, L"\xE8B7", [this, id] { SetScope(Scope::OfCollection(id)); },
                          [this, id, u] { model.lib.RemoveFromCollection({u}, id); });
            }
            y += S(30);
        }
        // Suggested tags: dashed; click adds, right-click "Don't suggest".
        const auto sugg = autotag::Pending(u, m);
        if (!sugg.empty()) {
            int x = L;
            for (const auto& t : sugg) {
                SIZE sz = Measure(dc, fTiny, L"+ " + t);
                if (x + sz.cx + S(16) > R && x > L) {
                    x = L;
                    y += S(26);
                }
                x = Chip(dc, x, y, L"+ " + t, false, true, [this, t, u] { model.lib.AddTags({t}, {u}); },
                         [this, t, u] {
                             HMENU mm = CreatePopupMenu();
                             AppendMenuW(mm, MF_STRING, 1, (L"Don't suggest “" + t + L"” for this capture").c_str());
                             POINT p;
                             GetCursorPos(&p);
                             if (Menu(mm, p) == 1) model.lib.DismissSuggestion(t, {u});
                         },
                         L"Suggested tag: click to add. Right-click to stop suggesting it.")
                        .right + S(5);
            }
            if (sugg.size() > 1) {
                const std::wstring all = L"Add all";
                SIZE sz = Measure(dc, fTiny, all);
                if (x + sz.cx > R) {
                    x = L;
                    y += S(26);
                }
                RECT ar{x + S(2), y, x + S(2) + sz.cx, y + S(22)};
                Text(dc, fTiny, all, ar, theme::kMuted);
                AddHot(ar, [this, sugg, u] { model.lib.AddTags(sugg, {u}); });
            }
            y += S(30);
        }
        if (addingTag) tagField({u});
        if (editingComment || !m.comment.empty()) {
            if (commentFor != u) {
                commentFor = u;
                SetWindowTextW(commentEdit, m.comment.c_str());
            }
            const int lines = std::clamp(WrapText(dc, fUi, m.comment.empty() ? L"x" : m.comment, {L, y, R - S(16), y + 2000}, 0, false), S(40), S(120));
            RECT cr{L, y, R, y + lines + S(12)};
            FillRR(dc, cr, Sf(6), A(RGB(44, 45, 42)));
            SetWindowPos(commentEdit, nullptr, cr.left + S(8), cr.top + S(6), RectW(cr) - S(16), RectH(cr) - S(12),
                         SWP_NOZORDER | SWP_NOACTIVATE | (OverlayUp() ? 0 : SWP_SHOWWINDOW));
            commentShown = true;
            y = cr.bottom + S(8);
        }
        {
            int x = L;
            if (!addingTag) x = ghost(x, L"\xE710", L"Tag", [this] {
                addingTag = true;
                SetWindowTextW(tagEdit, L"");
                Changed(false);
                SetFocus(tagEdit);
            });
            x = ghost(x, L"\xE710", L"Collection", [this] { CollectionPicker(); });
            if (!editingComment && m.comment.empty()) ghost(x, L"\xE710", L"Comment", [this, u] {
                editingComment = true;
                commentFor.clear();
                Changed(false);
                SetFocus(commentEdit);
            });
            y += S(38);
        }
        // Palette as a thin bar: click filters by that colour, right-click copies the hex.
        if (!m.colors.empty()) {
            const size_t n = std::min<size_t>(6, m.colors.size());
            double total = 0;
            for (size_t i = 0; i < n; ++i) total += m.colors[i].ratio;
            RECT bar{L, y, R, y + S(12)};
            {
                gp::Graphics g(dc);
                g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                gp::GraphicsPath clipPath;
                RoundPath(clipPath, (float)bar.left, (float)bar.top, (float)RectW(bar), (float)RectH(bar), RectH(bar) / 2.f);
                g.SetClip(&clipPath);
                float x = (float)bar.left;
                for (size_t i = 0; i < n; ++i) {
                    const float w = std::max(Sf(3), (float)(RectW(bar) * m.colors[i].ratio / std::max(0.0001, total)));
                    gp::SolidBrush b(gp::Color(255, m.colors[i].r, m.colors[i].g, m.colors[i].b));
                    g.FillRectangle(&b, x, (float)bar.top, w + 1, (float)RectH(bar));
                    x += w;
                }
            }
            float x = (float)bar.left;
            for (size_t i = 0; i < n; ++i) {
                const float w = std::max(Sf(3), (float)(RectW(bar) * m.colors[i].ratio / std::max(0.0001, total)));
                const std::wstring hex = m.colors[i].Hex();
                const int pct = (int)std::lround(m.colors[i].ratio * 100);
                AddHot({(int)x, bar.top - S(3), (int)(x + w), bar.bottom + S(3)},
                       [this, hex] {
                           Filter f = model.filter();
                           f.color = hex;
                           if (model.scope().kind == ScopeKind::Similar) SetScope(Scope());
                           SetFilter(f);
                       },
                       hex + L"  \u00b7  " + std::to_wstring(pct) + L"%  \u00b7  click to find this color, right-click to copy",
                       [this, hex] {
                           CopyTextToClipboard(hwnd, hex);
                           ShowToast(hex + L" copied", L"", nullptr, nullptr, 1500);
                       });
                x += w;
            }
            y += S(24);
        }
        if (!m.app.empty()) info(L"App", m.app);
        if (!m.window.empty()) info(L"Window", m.window);
        if (m.editedFrom) link(L"Edited from", *m.editedFrom);
        if (!m.includes.empty()) {
            for (size_t i = 0; i < m.includes.size() && i < 6; ++i) link(i == 0 ? L"Includes" : L"", m.includes[i]);
            if (m.includes.size() > 6) {
                Text(dc, fTiny, L"and " + std::to_wstring(m.includes.size() - 6) + L" more", {L + S(82), y, R, y + S(18)}, theme::kMuted);
                y += S(20);
            }
        }
        if (!m.app.empty() || !m.window.empty() || m.editedFrom || !m.includes.empty()) y += S(6);
        // Versions: newest first, with this one, the original and edits marked.
        const auto vs = model.Versions(u);
        if (vs.size() > 1) {
            Text(dc, fTiny, L"Versions", {L, y, R, y + S(18)}, theme::kTextDim);
            RECT cr{R - S(70), y, R, y + S(18)};
            Text(dc, fIconSmall, L"\xE8AB", {cr.left, cr.top, cr.left + S(16), cr.bottom}, theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, fTiny, L"Compare", {cr.left + S(16), cr.top, cr.right, cr.bottom}, theme::kMuted);
            AddHot(cr, [this] { Compare(); }, L"Compare with the previous version (C)");
            y += S(22);
            for (const auto& v : vs) {
                const bool me = v == u;
                const wchar_t* mark = me ? L"\xE915" : model.lib.Meta(v).editedFrom ? L"\xE70F" : L"\xEA3A";
                RECT vr{L, y, R, y + S(20)};
                Text(dc, fIconSmall, mark, {vr.left, vr.top, vr.left + S(16), vr.bottom}, me ? theme::kAccent : theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                const std::wstring date = FormatLibraryDate(model.lib.Meta(v).mtime);
                SIZE ds = Measure(dc, fTiny, date);
                Text(dc, fTiny, Stem(v), {vr.left + S(18), vr.top, vr.right - ds.cx - S(8), vr.bottom}, me ? theme::kText : theme::kTextDim,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                Text(dc, fTiny, date, {vr.right - ds.cx, vr.top, vr.right, vr.bottom}, theme::kMuted);
                if (!me) AddHot(vr, [this, v] { Reveal(v); }, me ? L"" : L"Show this version");
                y += S(22);
            }
            y += S(8);
        }
        // Text in image: folded to one line with a word count and a copy button.
        if (m.text && !m.text->empty()) {
            const std::wstring t = *m.text;
            RECT hr{L, y, R - S(24), y + S(20)};
            Text(dc, fIconSmall, showText ? L"\xE70D" : L"\xE76C", {hr.left, hr.top, hr.left + S(14), hr.bottom}, theme::kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            Text(dc, fTiny, L"Text in image", {hr.left + S(16), hr.top, hr.right, hr.bottom}, theme::kTextDim);
            SIZE ts = Measure(dc, fTiny, L"Text in image");
            Text(dc, fTiny, std::to_wstring(CountWords(t)) + L" words", {hr.left + S(22) + ts.cx, hr.top, hr.right, hr.bottom}, theme::kMuted);
            AddHot(hr, [this] {
                showText = !showText;
                Changed(false);
            });
            RECT cp{R - S(20), y, R, y + S(20)};
            Text(dc, fIconSmall, L"\xE8C8", cp, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            AddHot(cp, [this, t] {
                CopyTextToClipboard(hwnd, t);
                ShowToast(L"Text copied", L"", nullptr, nullptr, 1500);
            }, L"Copy text (Ctrl+T)");
            y += S(24);
            if (showText) y += WrapText(dc, fTiny, t, {L, y, R, y + S(4000)}, theme::kTextDim, true) + S(8);
        }
        {
            const MediaType mt = MediaTypeOf(u);
            int x = L;
            x = ghost(x, mt == MediaType::Image ? L"\xE70F" : mt == MediaType::Video ? L"\xE714" : L"\xE768",
                      mt == MediaType::Image ? L"Annotate" : mt == MediaType::Video ? L"Edit video" : L"Open", [this, u] { Open(u); });
            ghost(x, L"\xE71E", L"Find similar", [this, u] { FindSimilar(u); });
            y += S(40);
        }
    } else if (model.Selected().size() > 1) {
        const auto us = model.Selected();
        Text(dc, fBold, std::to_wstring(us.size()) + L" captures selected", {L, y, R, y + S(20)}, theme::kText);
        y += S(22);
        int64_t bytes = 0;
        std::set<int> ratings;
        for (const auto& u : us) {
            bytes += model.lib.Meta(u).size;
            ratings.insert(model.lib.Meta(u).rating);
        }
        Text(dc, fTiny, Bytes(bytes), {L, y, R, y + S(18)}, theme::kMuted);
        y += S(24);
        stars(ratings.size() == 1 ? *ratings.begin() : 0, [this, us](int r) { model.lib.SetRating(r, us); });
        section(L"Tags on all of them");
        std::vector<std::wstring> common;
        for (const auto& t : model.lib.Meta(us[0]).tags)
            if (std::all_of(us.begin(), us.end(), [&](const std::wstring& u) {
                    const auto& ts = model.lib.Meta(u).tags;
                    return std::any_of(ts.begin(), ts.end(), [&](const std::wstring& x) { return LowerText(x) == LowerText(t); });
                }))
                common.push_back(t);
        if (!common.empty()) {
            int x = L;
            for (const auto& t : common)
                removable(x, t, nullptr, [this, t] {
                    Filter f;
                    f.tags = {t};
                    SetFilter(f);
                }, [this, t, us] { model.lib.RemoveTag(t, us); });
            y += S(30);
        }
        tagField(us);
        section(L"Collections");
        int x = L;
        for (const auto& c : model.lib.Collections()) {
            if (!std::all_of(us.begin(), us.end(), [&](const std::wstring& u) {
                    const auto& cs = model.lib.Meta(u).collections;
                    return std::find(cs.begin(), cs.end(), c.id) != cs.end();
                }))
                continue;
            const std::wstring id = c.id;
            removable(x, c.name, L"\xE8B7", [this, id] { SetScope(Scope::OfCollection(id)); },
                      [this, id, us] { model.lib.RemoveFromCollection(us, id); });
        }
        RECT ar{x, y, x + S(60), y + S(22)};
        Text(dc, fIconSmall, L"\xE710", {ar.left, ar.top, ar.left + S(14), ar.bottom}, theme::kAccent, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, fTiny, L"Add", {ar.left + S(16), ar.top, ar.right, ar.bottom}, theme::kAccent);
        AddHot(ar, [this] { CollectionPicker(); });
        y += S(34);
        ghost(L, L"\xE8AC", L"Batch rename", [this, us] { BatchRename(us); });
        y += S(40);
    } else {
        section(L"Library");
        int64_t bytes = 0;
        int counts[3] = {};
        for (const auto& p : model.lib.Paths()) {
            bytes += model.lib.Meta(p).size;
            ++counts[(int)MediaTypeOf(p)];
        }
        info(L"Captures", std::to_wstring(model.lib.Paths().size()));
        for (MediaType t : {MediaType::Image, MediaType::Gif, MediaType::Video}) info(MediaTypeLabel(t), std::to_wstring(counts[(int)t]));
        info(L"On disk", Bytes(bytes));
        info(L"Tags", std::to_wstring(model.lib.AllTags().size()));
        info(L"Collections", std::to_wstring(model.lib.Collections().size()));
        y += S(6);
        FillSolid(dc, {L, y, R, y + 1}, theme::kBorder);
        y += S(10);
        y += WrapText(dc, fTiny, L"Select a capture to tag, rate and comment on it. Space previews, T adds tags, F files it into a collection, 1–5 rates.",
                      {L, y, R, y + S(200)}, theme::kMuted, true);
    }
    inspH = y + inspScroll - body.top + S(20);
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);
    // Controls scrolled out of the body are hidden by the clip; drop their hot rects too (they sat over the
    // close button and the header), and trim the partly visible ones.
    for (size_t i = firstBodyHot; i < hots.size();) {
        RECT vis;
        if (!IntersectRect(&vis, &hots[i].r, &body)) {
            hots.erase(hots.begin() + (std::ptrdiff_t)i);
            continue;
        }
        hots[i].r = vis;
        ++i;
    }
    if (!tagEditShown && Shown(tagEdit)) {
        if (GetFocus() == tagEdit) SetFocus(hwnd);
        ShowWindow(tagEdit, SW_HIDE);
    }
    if (!commentShown && Shown(commentEdit)) {
        if (GetFocus() == commentEdit) SetFocus(hwnd);
        ShowWindow(commentEdit, SW_HIDE);
    }
}

}  // namespace
}  // namespace ather
namespace ather {
namespace {

// ---------- filter popover ----------

static RECT g_filterPanel{};

void Gallery::ShowFilterMenu(int which, POINT at) {
    Filter f = model.filter();
    HMENU m = CreatePopupMenu();
    auto add = [&](int id, const std::wstring& label, bool checked, bool radio = false) {
        AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, label.c_str());
        if (radio && checked) CheckMenuRadioItem(m, id, id, id, MF_BYCOMMAND);
    };
    const auto tags = model.lib.AllTags();
    const auto apps = model.lib.AllApps();
    switch (which) {
        case 0:
            for (MediaType t : {MediaType::Image, MediaType::Gif, MediaType::Video})
                add(1 + (int)t, MediaTypeLabel(t), std::find(f.types.begin(), f.types.end(), t) != f.types.end());
            break;
        case 1:
            add(1, L"Untagged only", f.untagged);
            add(2, L"Match any tag (instead of all)", f.anyTag);
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < tags.size() && i < 40; ++i)
                add(10 + (int)i, tags[i].first + L"  (" + std::to_wstring(tags[i].second) + L")",
                    std::any_of(f.tags.begin(), f.tags.end(), [&](const std::wstring& x) { return LowerText(x) == LowerText(tags[i].first); }));
            break;
        case 2:
            add(1, L"Any", f.minRating == 0, true);
            for (int r = 1; r <= 5; ++r) add(1 + r, Stars(r) + (r < 5 ? L" or more" : L""), f.minRating == r, true);
            break;
        case 3:
            add(1, L"Any", f.shape == ShapeFilter::Any, true);
            for (int i = 1; i <= 5; ++i) add(1 + i, ShapeLabel((ShapeFilter)i), (int)f.shape == i, true);
            break;
        case 4: {
            const std::pair<int, int> dims[] = {{0, 0}, {800, 600}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
            const wchar_t* labels[] = {L"Any", L"At least 800 × 600", L"At least 1280 × 720 (HD)", L"At least 1920 × 1080 (Full HD)",
                                       L"At least 2560 × 1440", L"At least 3840 × 2160 (4K)"};
            for (int i = 0; i < 6; ++i) add(1 + i, labels[i], f.minWidth == dims[i].first && f.minHeight == dims[i].second, true);
            break;
        }
        case 5:
            add(1, L"Any time", f.date == DateFilter::Any, true);
            for (int i = 1; i <= 4; ++i) add(1 + i, DateLabel((DateFilter)i), (int)f.date == i, true);
            break;
        case 6:
            add(1, L"Any size", f.size == SizeFilter::Any, true);
            for (int i = 1; i <= 4; ++i) add(1 + i, SizeLabel((SizeFilter)i), (int)f.size == i, true);
            break;
        case 7:
            add(1, L"Any app", f.apps.empty());
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < apps.size() && i < 60; ++i)
                add(10 + (int)i, apps[i].first + L"  (" + std::to_wstring(apps[i].second) + L")",
                    std::find(f.apps.begin(), f.apps.end(), apps[i].first) != f.apps.end());
            break;
    }
    const int id = Menu(m, at);
    if (!id) return;
    switch (which) {
        case 0: {
            const MediaType t = (MediaType)(id - 1);
            auto it = std::find(f.types.begin(), f.types.end(), t);
            if (it != f.types.end()) f.types.erase(it);
            else f.types.push_back(t);
            break;
        }
        case 1:
            if (id == 1) f.untagged = !f.untagged;
            else if (id == 2) f.anyTag = !f.anyTag;
            else if (id >= 10) {
                const std::wstring t = tags[id - 10].first;
                auto it = std::find_if(f.tags.begin(), f.tags.end(), [&](const std::wstring& x) { return LowerText(x) == LowerText(t); });
                if (it != f.tags.end()) f.tags.erase(it);
                else f.tags.push_back(t);
            }
            break;
        case 2: f.minRating = id - 1; break;
        case 3: f.shape = (ShapeFilter)(id - 1); break;
        case 4: {
            const std::pair<int, int> dims[] = {{0, 0}, {800, 600}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
            f.minWidth = dims[id - 1].first;
            f.minHeight = dims[id - 1].second;
            break;
        }
        case 5: f.date = (DateFilter)(id - 1); break;
        case 6: f.size = (SizeFilter)(id - 1); break;
        case 7:
            if (id == 1) f.apps.clear();
            else if (id >= 10) {
                const std::wstring a = apps[id - 10].first;
                auto it = std::find(f.apps.begin(), f.apps.end(), a);
                if (it != f.apps.end()) f.apps.erase(it);
                else f.apps.push_back(a);
            }
            break;
    }
    SetFilter(f);
}

void Gallery::PaintFilterPanel(HDC dc) {
    if (!filterOpen) {
        g_filterPanel = {};
        return;
    }
    const Filter& f = model.filter();
    const int w = S(340);
    const int x0 = std::clamp<int>((filterBtn.left + filterBtn.right) / 2 - w / 2, S(8), client.right - w - S(8));
    const int y0 = filterBtn.bottom + S(12);
    // Chip labels: "Name" or "Name: value".
    std::vector<std::wstring> labels;
    auto label = [&](const std::wstring& name, const std::wstring& value) { labels.push_back(value.empty() ? name : name + L": " + value); };
    {
        std::vector<std::wstring> names;
        for (auto t : f.types) names.push_back(MediaTypeLabel(t));
        label(L"Type", Join(names, L", "));
    }
    label(L"Tags", f.untagged ? L"Untagged" : Join(f.tags, f.anyTag ? L" or " : L" + "));
    label(L"Rating", f.minRating > 0 ? Stars(f.minRating) + L"+" : L"");
    label(L"Shape", f.shape != ShapeFilter::Any ? ShapeLabel(f.shape) : L"");
    label(L"Dimensions", f.minWidth > 0 || f.minHeight > 0 ? L"\u2265 " + std::to_wstring(f.minWidth) + L" \u00d7 " + std::to_wstring(f.minHeight) : L"");
    label(L"Date", f.date != DateFilter::Any ? DateLabel(f.date) : L"");
    label(L"File size", f.size != SizeFilter::Any ? SizeLabel(f.size) : L"");
    const bool hasApps = !model.lib.AllApps().empty();
    if (hasApps) label(L"App", Join(f.apps, L", "));
    // Lay out chips to know the height.
    std::vector<RECT> chipRects;
    int x = x0 + S(14), y = y0 + S(14);
    for (const auto& l : labels) {
        const int cw = Measure(dc, fSmall, l).cx + S(36);
        if (x + cw > x0 + w - S(14) && x > x0 + S(14)) {
            x = x0 + S(14);
            y += S(32);
        }
        chipRects.push_back({x, y, x + std::min(cw, w - S(28)), y + S(26)});
        x += std::min(cw, w - S(28)) + S(6);
    }
    y += S(26) + S(14);
    const int colorsTop = y;
    const int swatch = S(22), gap = S(9);
    const int rows = (int)((std::size(kPresetColors) + 1 + 7) / 8);
    y += S(20) + rows * (swatch + gap);
    Filter rest = f;
    rest.text.clear();
    const bool constraints = !rest.IsEmpty();
    if (constraints) y += S(14) + S(26);
    y += S(10);
    RECT panel{x0, y0, x0 + w, y};
    g_filterPanel = panel;
    Glass(dc, panel, Sf(12), s);
    AddHot(panel, nullptr, L"", nullptr, nullptr, IDC_ARROW);
    // A little arrow towards the button.
    for (size_t i = 0; i < labels.size(); ++i) {
        const RECT& r = chipRects[i];
        const bool on = labels[i].find(L':') != std::wstring::npos;
        FillRR(dc, r, RectH(r) / 2.f, gp::Color(on ? 46 : 18, 255, 255, 255));
        Text(dc, fSmall, labels[i], {r.left + S(10), r.top, r.right - S(20), r.bottom}, on ? theme::kText : theme::kTextDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        Text(dc, fIconSmall, L"\xE70D", {r.right - S(20), r.top, r.right - S(6), r.bottom}, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const int which = (int)i;
        AddHot(r, [this, which, r] { ShowFilterMenu(which, ScreenPt(r.left, r.bottom + S(2))); });
    }
    Text(dc, fTiny, L"Contains color", {x0 + S(14), colorsTop, x0 + w, colorsTop + S(18)}, theme::kMuted);
    int cx = x0 + S(14), cy = colorsTop + S(22), n = 0;
    for (const wchar_t* hex : kPresetColors) {
        RECT sr{cx, cy, cx + swatch, cy + swatch};
        const bool on = _wcsicmp(f.color.c_str(), hex) == 0;
        {
            gp::Graphics g(dc);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            gp::SolidBrush b(A(HexColor(hex)));
            g.FillEllipse(&b, (float)sr.left, (float)sr.top, (float)swatch, (float)swatch);
            gp::Pen ring(on ? gp::Color(255, 255, 255, 255) : gp::Color(46, 255, 255, 255), on ? Sf(2) : Sf(0.6f));
            g.DrawEllipse(&ring, (float)sr.left, (float)sr.top, (float)swatch, (float)swatch);
        }
        const std::wstring h = hex;
        AddHot(sr, [this, h, on] {
            Filter nf = model.filter();
            nf.color = on ? L"" : h;
            SetFilter(nf);
        }, h);
        cx += swatch + gap;
        if (++n % 8 == 0) {
            cx = x0 + S(14);
            cy += swatch + gap;
        }
    }
    {  // custom colour
        RECT sr{cx, cy, cx + swatch, cy + swatch};
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        const COLORREF wheel[] = {RGB(255, 59, 48), RGB(255, 204, 0), RGB(52, 199, 89), RGB(10, 132, 255)};
        for (int i = 0; i < 4; ++i) {
            gp::SolidBrush b(A(wheel[i]));
            g.FillPie(&b, (float)sr.left, (float)sr.top, (float)swatch, (float)swatch, -90.f + i * 90, 90.f);
        }
        AddHot(sr, [this] {
            static COLORREF custom[16] = {};
            CHOOSECOLORW cc{sizeof(cc)};
            cc.hwndOwner = hwnd;
            cc.lpCustColors = custom;
            cc.rgbResult = model.filter().color.empty() ? RGB(255, 59, 48) : HexColor(model.filter().color);
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!ChooseColorW(&cc)) return;
            wchar_t hex[8];
            swprintf_s(hex, L"#%02X%02X%02X", GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
            Filter nf = model.filter();
            nf.color = hex;
            SetFilter(nf);
        }, L"Custom color");
    }
    if (constraints) {
        const int by = panel.bottom - S(10) - S(26);
        FillSolid(dc, {x0 + S(14), by - S(8), x0 + w - S(14), by - S(7)}, theme::kBorder);
        RECT cl{x0 + S(14), by, x0 + S(14) + Measure(dc, fSmall, L"Clear filters").cx, by + S(26)};
        Text(dc, fSmall, L"Clear filters", cl, theme::kTextDim);
        AddHot(cl, [this] {
            Filter nf;
            nf.text = model.filter().text;
            SetFilter(nf);
        });
        const int sw = Measure(dc, fSmall, L"Save as smart folder").cx;
        RECT sv{x0 + w - S(14) - sw, by, x0 + w - S(14), by + S(26)};
        Text(dc, fSmall, L"Save as smart folder", sv, theme::kTextDim);
        AddHot(sv, [this] {
            filterOpen = false;
            SaveSmartFolder();
        });
    }
}

// ---------- shortcuts sheet ----------

void Gallery::PaintShortcuts(HDC dc) {
    if (!showShortcuts) return;
    {
        gp::Graphics g(dc);
        gp::SolidBrush dim(gp::Color(90, 0, 0, 0));
        g.FillRectangle(&dim, 0, 0, client.right, client.bottom);
    }
    AddHot(client, [this] {
        showShortcuts = false;
        Changed(false);
    }, L"", nullptr, nullptr, IDC_ARROW);
    static const std::pair<const wchar_t*, const wchar_t*> rows[] = {
        {L"Space", L"Preview"},           {L"Enter", L"Open or annotate"},   {L"← → ↑ ↓", L"Move selection"},
        {L"T", L"Add tags"},              {L"F", L"Add to collection"},      {L"1–5, 0", L"Rate, clear rating"},
        {L"C", L"Compare versions"},      {L"/  or  Ctrl+F", L"Search"},     {L"Ctrl+C", L"Copy"},
        {L"Ctrl+T", L"Copy text (OCR)"},  {L"Ctrl+P", L"Pin to screen"},     {L"Ctrl+R", L"Rename"},
        {L"Ctrl+U", L"Upload and copy link"}, {L"Ctrl+O", L"Show in Explorer"}, {L"Del", L"Move to the Recycle Bin"},
        {L"Ctrl+I", L"Details"},          {L"Ctrl+B", L"Sidebar"},           {L"Ctrl+=  Ctrl+−", L"Thumbnail size"},
        {L"Ctrl+Shift+S", L"Save as smart folder"}, {L"Ctrl+G", L"Make collage"}, {L"Ctrl+K", L"All actions"},
    };
    const int n = (int)std::size(rows), half = (n + 1) / 2;
    const int w = S(600), h = S(20) + S(40) + half * S(26) + S(16);
    RECT card{client.right / 2 - w / 2, client.bottom / 2 - h / 2, client.right / 2 + w / 2, client.bottom / 2 + h / 2};
    Glass(dc, card, Sf(18), s);
    AddHot(card, nullptr, L"", nullptr, nullptr, IDC_ARROW);
    Text(dc, fTitle, L"Keyboard shortcuts", {card.left + S(20), card.top + S(18), card.right - S(60), card.top + S(42)}, theme::kText);
    GlassButton(dc, card.right - S(50), card.top + S(16), L"\xE711", false, [this] {
        showShortcuts = false;
        Changed(false);
    }, L"Close (Esc)");
    for (int i = 0; i < n; ++i) {
        const int col = i / half, row = i % half;
        const int x = card.left + S(20) + col * S(290), y = card.top + S(60) + row * S(26);
        Text(dc, fBold, rows[i].first, {x, y, x + S(110), y + S(22)}, theme::kText);
        Text(dc, fSmall, rows[i].second, {x + S(116), y, x + S(280), y + S(22)}, theme::kTextDim);
    }
}

// ---------- preview ----------

void Gallery::SetPreview(const std::wstring& u) {
    model.preview = u;
    if (u.empty()) {
        model.slideshow = false;
        KillTimer(hwnd, kTimerSlide);
        KillTimer(hwnd, kTimerGif);
        KillTimer(hwnd, kTimerVideo);
        previewVideo.reset();
        gif = {};
        previewImg.reset();
        previewLoaded.clear();
    }
    Changed(false);
}

void Gallery::PaintPreview(HDC dc) {
    const std::wstring u = model.preview;
    if (u.empty()) return;
    if (previewLoaded != u) {  // load on first paint of a new capture
        previewLoaded = u;
        pvZoom = 0;
        pvX = pvY = 0;
        KillTimer(hwnd, kTimerGif);
        KillTimer(hwnd, kTimerVideo);
        previewVideo.reset();
        gif = {};
        previewImg.reset();
        const MediaType mt = MediaTypeOf(u);
        if (mt == MediaType::Gif) {
            gif = LoadGif(u);
            if (gif.Valid()) {
                int delay = 100;
                previewImg = gif.Next(&delay);
                if (gif.Animated()) SetTimer(hwnd, kTimerGif, delay, nullptr);
            }
        } else if (mt == MediaType::Video) {
            VideoInfo vi;
            if (ProbeVideo(u, &vi)) ProbeVideo(u, nullptr, 0, 0, &previewImg);  // shown until playback starts
            previewVideo = VideoPlayer::Open(u, hwnd, WM_NULL, nullptr);
            if (previewVideo) {
                previewVideo->Play();
                SetTimer(hwnd, kTimerVideo, 15, nullptr);
            }
        } else {
            previewImg = LoadImageFile(u);
        }
    }
    {
        gp::Graphics g(dc);
        gp::SolidBrush bg(gp::Color(240, 0, 0, 0));
        g.FillRectangle(&bg, 0, 0, client.right, client.bottom);
    }
    AddHot(client, nullptr, L"", nullptr, nullptr, IDC_ARROW);
    const RECT area{S(16), S(48) + S(12), client.right - S(16), client.bottom - S(16)};
    if (previewImg) {
        const double fit = std::min({1.0, (double)RectW(area) / previewImg->Width(), (double)RectH(area) / previewImg->Height()});
        const double z = pvZoom > 0 ? pvZoom : fit;
        const int w = std::max(1, (int)(previewImg->Width() * z)), h = std::max(1, (int)(previewImg->Height() * z));
        const int x = (area.left + area.right) / 2 - w / 2 + (int)pvX, y = (area.top + area.bottom) / 2 - h / 2 + (int)pvY;
        HRGN clip = CreateRectRgn(area.left, area.top, area.right, area.bottom);
        SelectClipRgn(dc, clip);
        MemDC md(previewImg->Handle(), dc);
        SetStretchBltMode(dc, z < 1 ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x, y, w, h, md, 0, 0, previewImg->Width(), previewImg->Height(), SRCCOPY);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        const bool video = MediaTypeOf(u) == MediaType::Video;
        Hot h2;
        h2.r = area;
        h2.cursor = pvZoom > 0 ? IDC_SIZEALL : IDC_ARROW;
        h2.tile = -3;  // the preview image: drag pans, wheel zooms
        if (video && previewVideo && previewVideo->Playing())
            h2.click = [this] {
                previewVideo->Pause();
                Changed(false);
            };
        h2.dbl = [this] {
            pvZoom = pvZoom > 0 ? 0 : 1.0;
            pvX = pvY = 0;
            Changed(false);
        };
        hots.push_back(h2);
        // After the image's own hot rect, so it's on top and takes the click.
        if (video && !(previewVideo && previewVideo->Playing())) {  // paused or ended: a play button; Enter opens the video editor
            const int cx = (area.left + area.right) / 2, cy = (area.top + area.bottom) / 2;
            RECT pb{cx - S(36), cy - S(36), cx + S(36), cy + S(36)};
            FillRR(dc, pb, Sf(36), gp::Color(170, 0, 0, 0));
            Text(dc, fBig, L"\xE768", pb, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            AddHot(pb, [this, u] {
                if (previewVideo) {
                    previewVideo->Play();
                    SetTimer(hwnd, kTimerVideo, 15, nullptr);
                    Changed(false);
                } else {
                    Open(u);
                }
            }, previewVideo ? L"Play" : L"Edit video (Enter)");
        }
    } else {
        Text(dc, fUi, L"Can't show this file", area, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // Top bar.
    RECT top{0, 0, client.right, S(48)};
    {
        gp::Graphics g(dc);
        gp::SolidBrush b(gp::Color(128, 0, 0, 0));
        g.FillRectangle(&b, 0, 0, client.right, S(48));
    }
    const int idx = (int)(std::find(model.visible.begin(), model.visible.end(), u) - model.visible.begin()) + 1;
    const ItemMeta& m = model.lib.Meta(u);
    int x = S(18);
    const std::wstring name = FileNameOf(u);
    SIZE ns = Measure(dc, fBold, name);
    const int nameW = std::min<int>((int)ns.cx, client.right / 2);
    Text(dc, fBold, name, {x, 0, x + nameW, S(48)}, RGB(255, 255, 255), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    x += nameW + S(14);
    const std::wstring count = std::to_wstring(idx) + L" / " + std::to_wstring(model.visible.size());
    Text(dc, fSmall, count, {x, 0, x + S(80), S(48)}, RGB(150, 150, 150));
    x += Measure(dc, fSmall, count).cx + S(14);
    if (m.w > 0) Text(dc, fSmall, std::to_wstring(m.w) + L" \u00d7 " + std::to_wstring(m.h), {x, 0, x + S(120), S(48)}, RGB(150, 150, 150));
    int bx = client.right - S(18) - 5 * S(34);
    auto btn = [&](const wchar_t* glyph, const std::wstring& tip, std::function<void()> run) {
        RECT r{bx, S(10), bx + S(30), S(38)};
        Text(dc, fIcon, glyph, r, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(r, std::move(run), tip);
        bx += S(34);
    };
    btn(L"\xE76B", L"Previous (←)", [this] {
        model.Step(-1);
        Changed(false);
    });
    btn(model.slideshow ? L"\xE769" : L"\xE768", L"Slideshow", [this] {
        model.slideshow = !model.slideshow;
        if (model.slideshow) SetTimer(hwnd, kTimerSlide, 3000, nullptr);
        else KillTimer(hwnd, kTimerSlide);
        Changed(false);
    });
    btn(L"\xE76C", L"Next (→)", [this] {
        model.Step(1);
        Changed(false);
    });
    btn(GalleryModel::IsImage(u) ? L"\xE70F" : L"\xE8A7", L"Annotate / open (Enter)", [this, u] { Open(u); });
    btn(L"\xE711", L"Close (Space / Esc)", [this] { SetPreview(L""); });
    (void)top;
}

// ---------- compare ----------

void Gallery::PaintCompare(HDC dc) {
    if (!model.compare) return;
    const auto [before, after] = *model.compare;
    if (cmpKey != before + L"|" + after) {
        cmpKey = before + L"|" + after;
        cmpA = LoadImageFile(before);
        cmpB = LoadImageFile(after);
        split = 0.5;
    }
    {
        gp::Graphics g(dc);
        gp::SolidBrush bg(gp::Color(240, 0, 0, 0));
        g.FillRectangle(&bg, 0, 0, client.right, client.bottom);
    }
    AddHot(client, nullptr, L"", nullptr, nullptr, IDC_ARROW);
    // Header: Before → After, mode, swap, close.
    int x = S(20);
    const int hy = S(16), hh = S(30);
    auto label = [&](const std::wstring& tag, const std::wstring& path) {
        Text(dc, fBadge, tag, {x, hy, x + S(60), hy + hh}, theme::kMuted);
        x += Measure(dc, fBadge, tag).cx + S(6);
        const std::wstring n = Stem(path);
        const int w = std::min<int>((int)Measure(dc, fBold, n).cx, client.right / 4);
        Text(dc, fBold, n, {x, hy, x + w, hy + hh}, theme::kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        x += w + S(10);
    };
    label(L"BEFORE", before);
    Text(dc, fIconSmall, L"\xE72A", {x, hy, x + S(16), hy + hh}, theme::kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    x += S(22);
    label(L"AFTER", after);
    int rx = client.right - S(20);
    rx -= S(30);
    GlassButton(dc, rx, hy + S(1), L"\xE711", false, [this] {
        model.compare.reset();
        cmpKey.clear();
        Changed(false);
    }, L"Close (Esc)");
    rx -= S(34);
    GlassButton(dc, rx, hy + S(1), L"\xE8AB", false, [this] {
        if (model.compare) model.compare = std::make_pair(model.compare->second, model.compare->first);
        Changed(false);
    }, L"Swap");
    // Segmented Slider | Side by side.
    const int segW = S(200);
    rx -= segW + S(10);
    RECT seg{rx, hy + S(1), rx + segW, hy + S(29)};
    FillRR(dc, seg, Sf(8), gp::Color(26, 255, 255, 255));
    for (int i = 0; i < 2; ++i) {
        RECT half{seg.left + i * segW / 2, seg.top, seg.left + (i + 1) * segW / 2, seg.bottom};
        const bool on = (i == 1) == sideBySide;
        if (on) {
            RECT in = half;
            InflateRect(&in, -S(2), -S(2));
            FillRR(dc, in, Sf(6), gp::Color(46, 255, 255, 255));
        }
        Text(dc, fSmall, i == 0 ? L"Slider" : L"Side by side", half, on ? theme::kText : theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        AddHot(half, [this, i] {
            sideBySide = i == 1;
            Changed(false);
        });
    }
    const RECT area{S(20), S(64), client.right - S(20), client.bottom - S(20)};
    auto draw = [&](const BitmapPtr& b, RECT box, int clipRight = INT_MAX) {
        if (!b) return;
        const double k = std::min((double)RectW(box) / b->Width(), (double)RectH(box) / b->Height());
        const int w = (int)(b->Width() * k), h = (int)(b->Height() * k);
        const int x0 = (box.left + box.right) / 2 - w / 2, y0 = (box.top + box.bottom) / 2 - h / 2;
        HRGN clip = CreateRectRgn(box.left, box.top, std::min<int>(box.right, clipRight), box.bottom);
        SelectClipRgn(dc, clip);
        MemDC md(b->Handle(), dc);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x0, y0, w, h, md, 0, 0, b->Width(), b->Height(), SRCCOPY);
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
    };
    if (sideBySide) {
        const int half = (RectW(area) - S(12)) / 2;
        draw(cmpA, {area.left, area.top, area.left + half, area.bottom});
        draw(cmpB, {area.right - half, area.top, area.right, area.bottom});
        return;
    }
    // One frame for both images, sized by the larger of the two (at most 2×).
    const double bw = std::max(cmpA ? cmpA->Width() : 16, cmpB ? cmpB->Width() : 16);
    const double bh = std::max(cmpA ? cmpA->Height() : 10, cmpB ? cmpB->Height() : 10);
    const double k = std::min({(double)RectW(area) / bw, (double)RectH(area) / bh, 2.0});
    const int w = (int)(bw * k), h = (int)(bh * k);
    cmpBox = {(area.left + area.right) / 2 - w / 2, (area.top + area.bottom) / 2 - h / 2, 0, 0};
    cmpBox.right = cmpBox.left + w;
    cmpBox.bottom = cmpBox.top + h;
    draw(cmpB, cmpBox);
    const int divX = cmpBox.left + (int)(w * split);
    draw(cmpA, cmpBox, divX);
    FillSolid(dc, {divX - 1, cmpBox.top, divX + 1, cmpBox.bottom}, RGB(255, 255, 255));
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::SolidBrush wb(gp::Color(255, 255, 255, 255));
        const float r = Sf(13);
        const float cy = (cmpBox.top + cmpBox.bottom) / 2.f;
        g.FillEllipse(&wb, divX - r, cy - r, 2 * r, 2 * r);
    }
    Text(dc, fIconSmall, L"\xE8AB", {divX - S(13), (cmpBox.top + cmpBox.bottom) / 2 - S(13), divX + S(13), (cmpBox.top + cmpBox.bottom) / 2 + S(13)},
         RGB(0, 0, 0), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Hot h2;
    h2.r = cmpBox;
    h2.cursor = IDC_SIZEWE;
    h2.tile = -4;  // the compare slider: drag moves the divider
    hots.push_back(h2);
}

// ---------- tooltip ----------

void Gallery::PaintTooltip(HDC dc) {
    if (!tipShown || tipText.empty()) return;
    RECT calc{0, 0, S(320), 0};
    HGDIOBJ o = SelectObject(dc, fTiny);
    DrawTextW(dc, tipText.c_str(), (int)tipText.size(), &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o);
    const int w = RectW(calc) + S(16), h = RectH(calc) + S(10);
    int x = std::clamp<int>(tipAt.x + S(12), S(4), client.right - w - S(4));
    int y = tipAt.y + S(20);
    if (y + h > client.bottom - S(4)) y = tipAt.y - h - S(8);
    RECT r{x, y, x + w, y + h};
    FillRR(dc, r, Sf(6), gp::Color(245, 52, 53, 50));
    StrokeRR(dc, r, Sf(6), gp::Color(30, 255, 255, 255), 1);
    RECT t{r.left + S(8), r.top + S(5), r.right - S(8), r.bottom - S(5)};
    Text(dc, fTiny, tipText, t, theme::kText, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

}  // namespace
}  // namespace ather
namespace ather {
namespace {

// ---------- painting ----------

static HDC g_backDC = nullptr;
static HBITMAP g_back = nullptr;
static HGDIOBJ g_backOld = nullptr;
static int g_backW = 0, g_backH = 0;


void Gallery::Paint(HDC hdc) {
    GetClientRect(hwnd, &client);
    const int w = std::max(1L, client.right), h = std::max(1L, client.bottom);
    if (!g_backDC || w > g_backW || h > g_backH) {
        if (g_backDC) {
            SelectObject(g_backDC, g_backOld);
            DeleteObject(g_back);
            DeleteDC(g_backDC);
        }
        g_backW = std::max(g_backW, w);
        g_backH = std::max(g_backH, h);
        g_backDC = CreateCompatibleDC(hdc);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), g_backW, -g_backH, 1, 32, BI_RGB};
        void* bits = nullptr;
        g_back = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        g_backOld = SelectObject(g_backDC, g_back);
    }
    HDC dc = g_backDC;
    hots.clear();
    collectionDrops.clear();
    FillSolid(dc, {0, 0, w, h}, theme::kBg);
    const RECT c = CanvasRect();
    PaintBrowser(dc, c);
    PaintTopBar(dc, c);
    PaintActionBar(dc, c);
    if (model.showSidebar) PaintSidebar(dc, SidebarRect());
    if (model.showInspector) {
        PaintInspector(dc, InspectorRect());
    } else {
        for (HWND e : {tagEdit, commentEdit})
            if (Shown(e)) {
                if (GetFocus() == e) SetFocus(hwnd);
                ShowWindow(e, SW_HIDE);
            }
    }
    PaintFilterPanel(dc);
    PaintShortcuts(dc);
    PaintCompare(dc);
    PaintPreview(dc);
    // Child edit boxes would draw over full-window overlays: hide them while one is up.
    const bool overlay = showShortcuts || model.compare.has_value() || !model.preview.empty();
    for (HWND e : {search, tagEdit, commentEdit})
        if (overlay && Shown(e)) {
            if (GetFocus() == e) SetFocus(hwnd);
            ShowWindow(e, SW_HIDE);
        }
    PaintTooltip(dc);
    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
}

// ---------- input ----------

static Hot g_pressed;
static bool g_havePressed = false, g_dragStarted = false, g_panning = false, g_splitting = false;

void Gallery::OnMouseDown(POINT p, bool dbl) {
    tipShown = false;
    KillTimer(hwnd, kTimerTip);
    if (filterOpen && !PtInRect(&g_filterPanel, p) && !PtInRect(&filterBtn, p)) {
        filterOpen = false;  // like a popover: the outside click only closes it
        Changed(false);
        return;
    }
    const int hi = HotAt(p);
    if (hi < 0) {
        const RECT c = CanvasRect();
        if (PtInRect(&c, p) && model.preview.empty() && !model.compare) {
            EndTyping();
            if (GetKeyState(VK_CONTROL) >= 0 && !model.selection.empty()) {
                model.selection.clear();
                Changed(false);
            }
        }
        return;
    }
    const Hot h = hots[hi];
    if (h.tile >= (int)model.visible.size()) return;  // painted for a list that has changed since
    if (h.tile >= 0) {
        EndTyping();
        const std::wstring u = model.visible[h.tile];
        if (dbl) return Open(u);
        const bool mods = GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_CONTROL) < 0;
        // A tile that's already selected waits for mouse up, so a multi-selection can be dragged together.
        if (mods || !model.selection.count(u)) {
            model.Click(u, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0);
            Changed(false);
        }
        pressTile = h.tile;
        pressPt = p;
        g_dragStarted = false;
        SetCapture(hwnd);
        return;
    }
    if (h.tile == -2) {  // scrollbar
        draggingScroll = true;
        dragScrollStart = scrollY;
        dragMouseStart = p.y;
        SetCapture(hwnd);
        return;
    }
    if (h.tile == -3) {  // preview image: drag pans; a click without dragging runs its click (pause a video)
        if (dbl && h.dbl) return h.dbl();
        g_panning = true;
        pvLast = p;
        pvDown = p;
        g_pressed = h;
        g_havePressed = (bool)h.click;
        SetCapture(hwnd);
        return;
    }
    if (h.tile == -4) {  // compare divider
        g_splitting = true;
        split = std::clamp((double)(p.x - cmpBox.left) / std::max(1, RectW(cmpBox)), 0.0, 1.0);
        SetCapture(hwnd);
        Changed(false);
        return;
    }
    if (dbl && h.dbl) return h.dbl();
    g_pressed = h;
    g_havePressed = true;
    SetCapture(hwnd);  // so a release outside the window is seen, and the press can't fire later
}

void Gallery::OnMouseUp(POINT p) {
    const bool havePressed = g_havePressed, wasPanning = g_panning;
    const Hot pressedHot = g_pressed;
    g_havePressed = false;
    g_pressed = Hot{};
    if (GetCapture() == hwnd) ReleaseCapture();
    draggingScroll = g_panning = g_splitting = false;
    if (pressTile >= 0 && pressTile < (int)model.visible.size() && !g_dragStarted) {
        const std::wstring u = model.visible[pressTile];
        const bool mods = GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_CONTROL) < 0;
        if (!mods && model.selection.count(u) && (model.selection.size() > 1 || model.focus != u)) {
            model.Click(u, false, false);  // a plain click inside a multi-selection selects just this one
            Changed(false);
        }
    }
    pressTile = -1;
    if (havePressed && PtInRect(&pressedHot.r, p) && pressedHot.click) {
        if (wasPanning && std::abs(p.x - pvDown.x) + std::abs(p.y - pvDown.y) > S(4)) return;  // that was a pan
        pressedHot.click();
    }
}

void Gallery::OnMouseMove(POINT p, WPARAM keys) {
    if (draggingScroll) {
        const RECT c = CanvasRect();
        const int view = RectH(c);
        const int th = std::max(S(30), view * view / std::max(1, contentH));
        scrollY = dragScrollStart + (p.y - dragMouseStart) * (contentH - view) / std::max(1, view - th);
        ClampScroll();
        Changed(false);
        return;
    }
    if (g_panning) {
        pvX += p.x - pvLast.x;
        pvY += p.y - pvLast.y;
        pvLast = p;
        Changed(false);
        return;
    }
    if (g_splitting) {
        split = std::clamp((double)(p.x - cmpBox.left) / std::max(1, RectW(cmpBox)), 0.0, 1.0);
        Changed(false);
        return;
    }
    if (pressTile >= 0 && (keys & MK_LBUTTON) && !g_dragStarted &&
        (std::abs(p.x - pressPt.x) > S(6) || std::abs(p.y - pressPt.y) > S(6))) {
        g_dragStarted = true;
        pressTile = -1;
        StartDrag();
        return;
    }
    const int hi = HotAt(p);
    const int tile = hi >= 0 ? hots[hi].tile : -1;
    if (tile != hover) hover = tile >= 0 ? tile : -1;
    // Tooltip after a short hover.
    const std::wstring tip = hi >= 0 ? hots[hi].tip : L"";
    if (tip != tipText || std::abs(p.x - tipAt.x) > S(3) || std::abs(p.y - tipAt.y) > S(3)) {
        tipShown = false;
        tipText = tip;
        tipAt = p;
        KillTimer(hwnd, kTimerTip);
        if (!tip.empty()) SetTimer(hwnd, kTimerTip, 650, nullptr);
    }
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&tme);
    InvalidateRect(hwnd, nullptr, FALSE);  // hover states
}

void Gallery::OnRightClick(POINT p) {
    const int hi = HotAt(p);
    if (hi < 0) return;
    const Hot h = hots[hi];
    if (h.tile >= (int)model.visible.size()) return;  // painted for a list that has changed since
    if (h.tile >= 0) {
        const std::wstring u = model.visible[h.tile];
        if (!model.selection.count(u)) {
            model.selection = {u};
            model.focus = model.anchor = u;
            Changed(false);
        }
        ContextMenu(u, ScreenPt(p.x, p.y));
        return;
    }
    if (h.right) h.right();
}

void Gallery::OnWheel(POINT p, int delta, bool ctrl) {
    if (!model.preview.empty()) {
        if (!previewImg) return;
        const RECT area{S(16), S(60), client.right - S(16), client.bottom - S(16)};
        const double fit = std::min({1.0, (double)RectW(area) / previewImg->Width(), (double)RectH(area) / previewImg->Height()});
        const double z0 = pvZoom > 0 ? pvZoom : fit;
        const double z1 = std::clamp(z0 * (delta > 0 ? 1.2 : 1 / 1.2), 0.05, 16.0);
        // Keep the point under the cursor in place.
        const double cx = (area.left + area.right) / 2.0 + pvX, cy = (area.top + area.bottom) / 2.0 + pvY;
        pvX += (p.x - cx) * (1 - z1 / z0);
        pvY += (p.y - cy) * (1 - z1 / z0);
        pvZoom = z1;
        Changed(false);
        return;
    }
    if (model.compare || showShortcuts) return;
    const RECT side = SidebarRect(), insp = InspectorRect();
    if (PtInRect(&side, p)) {
        sideScroll = std::clamp(sideScroll - delta * S(60) / WHEEL_DELTA, 0, std::max(0, sideH - RectH(side) + S(50)));
    } else if (PtInRect(&insp, p)) {
        inspScroll = std::clamp(inspScroll - delta * S(60) / WHEEL_DELTA, 0, std::max(0, inspH - RectH(insp) + S(52)));
    } else if (ctrl && model.layout != GalleryLayout::List) {
        model.Zoom(delta > 0 ? 1.15 : 1 / 1.15);
        tiles.Clear();
        Layout();
    } else {
        scrollY -= delta * S(110) / WHEEL_DELTA;
        ClampScroll();
    }
    Changed(false);
}

void Gallery::StartDrag() {
    if (GetCapture() == hwnd) ReleaseCapture();
    const auto files = model.Targets();
    if (files.empty()) return;
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& f : files)
        if (auto id = ILCreateFromPathW(f.c_str())) ids.push_back(id);
    if (ids.empty()) return;
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr))) {
        IDataObject* obj = nullptr;
        if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&obj)))) {
            DWORD effect = 0;
            SHDoDragDrop(hwnd, obj, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);  // never a move: the capture stays
            obj->Release();
        }
        arr->Release();
    }
    for (auto id : ids) ILFree(id);
}

// Files dropped on the gallery or a collection: our own captures are filed, outside files are imported.
void Gallery::OnDropFiles(HDROP drop) {
    POINT p{};
    DragQueryPoint(drop, &p);
    std::vector<std::wstring> files;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, path, (UINT)std::size(path))) files.push_back(path);
    }
    DragFinish(drop);
    std::wstring col;
    for (const auto& [r, id] : collectionDrops)
        if (PtInRect(&r, p)) col = id;
    std::vector<std::wstring> ours, outside;
    for (const auto& f : files) (model.lib.IsInLibrary(f) ? ours : outside).push_back(f);
    if (!col.empty() && !ours.empty()) {
        model.lib.AddToCollection(ours, col);
        const LibCollection* c = model.lib.FindCollection(col);
        ShowToast(L"Added to " + (c ? c->name : std::wstring(L"collection")),
                  std::to_wstring(ours.size()) + (ours.size() == 1 ? L" capture" : L" captures"), nullptr, nullptr, 2000);
    }
    if (!outside.empty()) ImportFiles(outside, col);
}

void Gallery::Escape() {
    if (filterOpen) {
        filterOpen = false;
    } else if (model.scope().kind == ScopeKind::Similar) {
        SetScope(Scope());
    } else if (!model.selection.empty()) {
        model.selection.clear();
    } else if (!model.filter().IsEmpty()) {
        SetFilter(Filter());
    } else {
        DestroyWindow(hwnd);
        return;
    }
    Changed(false);
}

bool Gallery::OnCommandKey(WPARAM vk, bool shift) {
    switch (vk) {
        case 'B': ToggleSidebar(); return true;
        case VK_OEM_2:  // Ctrl+/
            showShortcuts = !showShortcuts;
            Changed(false);
            return true;
        case VK_OEM_PLUS:
        case VK_ADD:
            model.Zoom(1.15);
            tiles.Clear();
            Changed();
            return true;
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            model.Zoom(1 / 1.15);
            tiles.Clear();
            Changed();
            return true;
        case 'A':
            model.SelectAll();
            Changed(false);
            return true;
        case 'C': Copy(); return true;
        case 'P': Pin(); return true;
        case 'R': Rename(); return true;
        case 'U': Upload(); return true;
        case 'T': CopyText(); return true;
        case 'O': Reveal(); return true;
        case 'K': ActionsPalette(); return true;
        case 'F': SetFocus(search); return true;
        case 'I': ToggleInspector(); return true;
        case 'G': Collage(); return true;
        case 'S':
            if (shift) SaveSmartFolder();
            return shift;
        case 'W': DestroyWindow(hwnd); return true;
    }
    return false;
}

void Gallery::OnKey(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    tipShown = false;
    if (showShortcuts) {
        if (vk == VK_ESCAPE || (ctrl && vk == VK_OEM_2) || (shift && vk == VK_OEM_2)) {
            showShortcuts = false;
            Changed(false);
        }
        return;
    }
    if (model.compare) {
        if (vk == VK_ESCAPE || vk == VK_SPACE || vk == 'C') {
            model.compare.reset();
            cmpKey.clear();
            Changed(false);
        }
        return;
    }
    if (!model.preview.empty()) {
        switch (vk) {
            case VK_LEFT:
            case VK_UP:
                model.Step(-1);
                Changed(false);
                return;
            case VK_RIGHT:
            case VK_DOWN:
                model.Step(1);
                Changed(false);
                return;
            case VK_ESCAPE:
            case VK_SPACE: SetPreview(L""); return;
            case VK_RETURN: Open(model.preview); return;
        }
        if (ctrl) OnCommandKey(vk, shift);
        return;
    }
    if (ctrl) {
        OnCommandKey(vk, shift);
        return;
    }
    switch (vk) {
        case VK_LEFT: model.Move(-1, shift); break;
        case VK_RIGHT: model.Move(1, shift); break;
        case VK_UP: model.MoveRow(-1, shift); break;
        case VK_DOWN: model.MoveRow(1, shift); break;
        case VK_HOME:
            if (!model.visible.empty()) model.Click(model.visible.front(), shift, false);
            break;
        case VK_END:
            if (!model.visible.empty()) model.Click(model.visible.back(), shift, false);
            break;
        case VK_PRIOR: model.Move(-model.columns * 4, shift); break;
        case VK_NEXT: model.Move(model.columns * 4, shift); break;
        case VK_SPACE: {
            const auto sel = model.Selected();
            const std::wstring u = !model.focus.empty() ? model.focus : sel.empty() ? L"" : sel.front();
            if (!u.empty()) SetPreview(u);
            return;
        }
        case VK_RETURN: Open(); return;
        case VK_ESCAPE: Escape(); return;
        case VK_DELETE: Trash(); return;
        case 'T': TagPicker(); return;
        case 'F': CollectionPicker(); return;
        case 'C': Compare(); return;
        case '0': case '1': case '2': case '3': case '4': case '5': Rate((int)(vk - '0')); return;
        case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3: case VK_NUMPAD4: case VK_NUMPAD5:
            Rate((int)(vk - VK_NUMPAD0));
            return;
        case VK_OEM_2:  // "/" searches, "?" shows the shortcuts
            if (shift) showShortcuts = true;
            else SetFocus(search);
            Changed(false);
            return;
        case VK_F5: model.lib.Refresh(); return;
        default: return;
    }
    if (!model.focus.empty()) EnsureVisible(model.focus);
    Changed(false);
}

// ---------- actions ----------

void Gallery::Open(const std::wstring& path) {
    std::wstring u = path;
    if (u.empty()) u = !model.focus.empty() ? model.focus : model.Selected().empty() ? L"" : model.Selected().front();
    if (u.empty()) return;
    const MediaType mt = MediaTypeOf(u);
    if (mt == MediaType::Image && host.openImage) host.openImage(u);
    else if (mt == MediaType::Video && host.openVideo) host.openVideo(u);
    else OpenPath(u);
}

void Gallery::Copy() {
    const auto ts = model.Targets();
    if (ts.empty()) return;
    bool ok = false;
    if (ts.size() == 1 && GalleryModel::IsImage(ts[0])) {
        if (auto img = LoadImageFile(ts[0])) ok = CopyImageToClipboard(hwnd, *img);
    } else {
        ok = CopyFilesToClipboard(hwnd, ts);
    }
    if (ok) ShowToast(L"Copied", ts.size() == 1 ? FileNameOf(ts[0]) : std::to_wstring(ts.size()) + L" files", nullptr, nullptr, 1800);
}

void Gallery::Pin() {
    for (const auto& u : model.Targets())
        if (GalleryModel::IsImage(u))
            if (auto img = LoadImageFile(u)) PinImage(img, nullptr);
}

void Gallery::Reveal() { RevealInExplorer(model.Targets()); }

void Gallery::Collage() {
    std::vector<std::wstring> imgs;
    for (const auto& u : model.Targets())
        if (GalleryModel::IsImage(u)) imgs.push_back(u);
    if (imgs.size() < 2) return (void)ShowToast(L"Select two or more screenshots for a collage", L"", nullptr, nullptr, 2500);
    if (host.collage) host.collage(imgs);
}

void Gallery::AddToEditor() {
    if (!host.editorOpen || !host.editorOpen())
        return (void)ShowToast(L"No editor is open", L"Open a screenshot to annotate first", nullptr, nullptr, 2500);
    std::vector<std::wstring> imgs;
    for (const auto& u : model.Targets())
        if (GalleryModel::IsImage(u)) imgs.push_back(u);
    if (host.addToEditor) host.addToEditor(imgs);
}

void Gallery::Upload() {
    for (const auto& u : model.Targets())
        if (host.upload) host.upload(u);
}

void Gallery::CopyText() {
    std::vector<std::wstring> ts;
    for (const auto& u : model.Targets())
        if (GalleryModel::IsImage(u)) ts.push_back(u);
    std::wstring text;
    for (const auto& u : ts)
        if (const auto& t = model.lib.Meta(u).text; t && !t->empty()) text += (text.empty() ? L"" : L"\n\n") + *t;
    if (!text.empty()) {
        CopyTextToClipboard(hwnd, text);
        ShowToast(L"Text copied", text.substr(0, 280), nullptr, nullptr, 2500);
        return;
    }
    if (ts.empty()) return;
    auto img = LoadImageFile(ts[0]);
    if (!img) return;
    HWND h = hwnd;
    RecognizeTextAsync(img, [h](std::wstring t, std::wstring err) {
        if (!err.empty() || t.empty()) return (void)ShowToast(L"No text found", err, nullptr, nullptr, 2500);
        CopyTextToClipboard(h, t);
        ShowToast(L"Text copied", t.substr(0, 280), nullptr, nullptr, 2500);
    });
}

void Gallery::Rate(int r) {
    const auto ts = model.Targets();
    if (ts.empty()) return;
    model.lib.SetRating(r, ts);
    ShowToast(r == 0 ? L"Rating cleared" : Stars(r), ts.size() == 1 ? FileNameOf(ts[0]) : std::to_wstring(ts.size()) + L" captures", nullptr,
              nullptr, 1500);
}

void Gallery::Rename() {
    const auto ts = model.Targets();
    if (ts.empty()) return;
    if (ts.size() > 1) return BatchRename(ts);
    const std::wstring first = ts[0];
    ShowTextPrompt(L"Rename  ·  Enter renames, Esc keeps the current name", Stem(first), [this, first](std::wstring name) {
        const std::wstring n = RenameCapture(first, name);
        if (n.empty()) return (void)ShowToast(L"Rename failed", FileNameOf(first), nullptr, nullptr, 3000);
        model.lib.Moved(first, n);
        model.selection = {n};
        model.focus = model.anchor = n;
        if (model.preview == first) model.preview = n;
        model.lib.Refresh();
        Recompute();
    });
}

// Template tokens: {n} running number, {name} current name, {date} capture date, {app} source app.
void Gallery::BatchRename(const std::vector<std::wstring>& ts) {
    ShowTextPrompt(L"Rename " + std::to_wstring(ts.size()) + L" files  ·  {n} number · {name} current name · {date} · {app}", L"{name}",
                   [this, ts](std::wstring tmpl) {
                       if (tmpl.empty()) return;
                       const int digits = (int)std::to_wstring(ts.size()).size();
                       std::vector<std::wstring> renamed;
                       for (size_t i = 0; i < ts.size(); ++i) {
                           const std::wstring& u = ts[i];
                           const ItemMeta m = model.lib.Meta(u);
                           wchar_t num[16];
                           swprintf_s(num, L"%0*d", digits, (int)i + 1);
                           std::wstring name = tmpl;
                           auto rep = [&](const std::wstring& a, const std::wstring& b) {
                               for (size_t at = 0; (at = name.find(a, at)) != std::wstring::npos; at += b.size()) name.replace(at, a.size(), b);
                           };
                           rep(L"{n}", num);
                           rep(L"{name}", Stem(u));
                           rep(L"{date}", FormatLibraryDate(m.mtime).substr(0, 10));
                           rep(L"{app}", m.app.empty() ? L"screen" : m.app);
                           const std::wstring n = RenameCapture(u, name);
                           if (n.empty()) continue;
                           model.lib.Moved(u, n);
                           if (model.preview == u) model.preview = n;
                           renamed.push_back(n);
                       }
                       model.selection = std::set<std::wstring>(renamed.begin(), renamed.end());
                       model.lib.Refresh();
                       Recompute();
                       ShowToast(L"Renamed " + std::to_wstring(renamed.size()) + L" of " + std::to_wstring(ts.size()), L"", nullptr, nullptr, 2000);
                   });
}

void Gallery::Trash(const std::vector<std::wstring>* list) {
    const auto ts = list ? *list : model.Targets();
    if (ts.empty()) return;
    // The capture after the last one removed gets the selection.
    std::wstring next;
    if (auto it = std::find(model.visible.begin(), model.visible.end(), ts.back()); it != model.visible.end()) {
        const size_t i = it - model.visible.begin();
        for (size_t k = i + 1; k < model.visible.size() && next.empty(); ++k)
            if (std::find(ts.begin(), ts.end(), model.visible[k]) == ts.end()) next = model.visible[k];
        for (size_t k = i; k-- > 0 && next.empty();)
            if (std::find(ts.begin(), ts.end(), model.visible[k]) == ts.end()) next = model.visible[k];
    }
    const auto gone = RecycleFiles(ts, hwnd);
    if (gone.empty()) return (void)ShowToast(L"Couldn't move to the Recycle Bin", FileNameOf(ts[0]), nullptr, nullptr, 3000);
    model.lib.Removed(gone);
    if (!next.empty()) {
        model.selection = {next};
        model.focus = model.anchor = next;
    }
    if (std::find(gone.begin(), gone.end(), model.preview) != gone.end()) model.preview = next;
    Recompute();
    ShowToast(L"Moved to the Recycle Bin", gone.size() == 1 ? FileNameOf(gone[0]) : std::to_wstring(gone.size()) + L" files", nullptr, nullptr, 2000);
}

void Gallery::FindSimilar(const std::wstring& path) {
    std::wstring u = path;
    if (u.empty()) u = !model.focus.empty() ? model.focus : model.Selected().empty() ? L"" : model.Selected().front();
    if (u.empty()) return;
    if (!model.lib.HasFeatures())
        return (void)ShowToast(L"Still indexing", L"Try again once the gallery finishes indexing.", nullptr, nullptr, 2500);
    model.SetFilter(Filter());
    SetWindowTextW(search, L"");
    SetScope(Scope::SimilarTo(u));
    model.selection = {u};
    model.focus = model.anchor = u;
    Changed(false);
}

// Eagle's "T": add tags to the selection, picking existing ones or typing new ones.
void Gallery::TagPicker() {
    const auto ts = model.Targets();
    if (ts.empty()) return;
    std::unordered_set<std::wstring> have;
    for (const auto& u : ts)
        for (const auto& t : model.lib.Meta(u).tags) have.insert(LowerText(t));
    const auto tags = model.lib.AllTags();
    std::vector<PaletteItem> items;
    for (size_t i = 0; i < tags.size(); ++i)
        items.push_back({(int)i, tags[i].first, std::to_wstring(tags[i].second), L"tag",
                         have.count(LowerText(tags[i].first)) ? (wchar_t)0xE73E : (wchar_t)0xE8EC});
    PaletteOptions opt;
    opt.placeholder = L"Add a tag to " + (ts.size() == 1 ? FileNameOf(ts[0]) : std::to_wstring(ts.size()) + L" captures") +
                      L" (comma-separated to add several)…";
    opt.createLabel = L"Add tag";
    opt.onCreate = [this, ts](std::wstring text) { model.lib.AddTags(SplitTags(text), ts); };
    ShowPalette(std::move(items), [this, ts, tags](int id) {
        if (id < 0 || id >= (int)tags.size()) return;
        const std::wstring t = tags[id].first;
        const bool all = std::all_of(ts.begin(), ts.end(), [&](const std::wstring& u) {
            const auto& mt = model.lib.Meta(u).tags;
            return std::any_of(mt.begin(), mt.end(), [&](const std::wstring& x) { return LowerText(x) == LowerText(t); });
        });
        if (all) model.lib.RemoveTag(t, ts);
        else model.lib.AddTags({t}, ts);
    }, opt);
}

// Eagle's "F": file the selection into a collection (or a new one).
void Gallery::CollectionPicker() {
    const auto ts = model.Targets();
    if (ts.empty()) return;
    std::vector<PaletteItem> items;
    const auto cols = model.lib.Collections();
    for (size_t i = 0; i < cols.size(); ++i)
        items.push_back({(int)i, cols[i].name, std::to_wstring(model.lib.CountIn(cols[i].id)), L"collection folder " + Join(cols[i].autoTags, L" "),
                         (wchar_t)0xE8B7});
    PaletteOptions opt;
    opt.placeholder = L"Add to collection…";
    opt.createLabel = L"New collection";
    opt.onCreate = [this, ts](std::wstring name) {
        if (name.empty()) return;
        auto c = model.lib.CreateCollection(name);
        model.lib.AddToCollection(ts, c.id);
        ShowToast(L"Added to " + c.name, std::to_wstring(ts.size()) + (ts.size() == 1 ? L" capture" : L" captures"), nullptr, nullptr, 2000);
    };
    ShowPalette(std::move(items), [this, ts, cols](int id) {
        if (id < 0 || id >= (int)cols.size()) return;
        model.lib.AddToCollection(ts, cols[id].id);
        ShowToast(L"Added to " + cols[id].name, ts.size() == 1 ? FileNameOf(ts[0]) : std::to_wstring(ts.size()) + L" captures", nullptr,
                  nullptr, 2000);
    }, opt);
}

void Gallery::SaveSmartFolder() {
    if (model.filter().IsEmpty())
        return (void)ShowToast(L"Set a filter first", L"A smart folder saves the current search and filters.", nullptr, nullptr, 2500);
    if (model.scope().kind == ScopeKind::Smart) {
        Filter f = model.filter();
        model.lib.UpdateSmartFolder(model.scope().id, &f, nullptr);
        return (void)ShowToast(L"Smart folder updated", L"", nullptr, nullptr, 1800);
    }
    ShowTextPrompt(L"Name this smart folder", model.filter().text.empty() ? L"Smart folder" : model.filter().text, [this](std::wstring name) {
        if (name.empty()) return;
        auto folder = model.lib.SaveSmartFolder(name, model.filter());
        SetScope(Scope::OfSmart(folder.id));
    });
}

void Gallery::Compare() {
    if (!model.CompareSelected())
        return (void)ShowToast(L"Select two screenshots to compare", L"Or one edited capture to compare with its original", nullptr, nullptr, 2500);
    Changed(false);
}

void Gallery::ImportPanel() {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Images and videos\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff;*.heic;*.mp4;*.mov\0All files\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<std::wstring> parts;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) parts.push_back(p);
    std::vector<std::wstring> files;
    if (parts.size() == 1) files = parts;
    else
        for (size_t i = 1; i < parts.size(); ++i) files.push_back(parts[0] + L"\\" + parts[i]);
    ImportFiles(files);
}

void Gallery::ImportFiles(const std::vector<std::wstring>& files, const std::wstring& collection) {
    const auto out = model.lib.ImportFiles(files, collection);
    ShowToast(out.empty() ? L"Nothing to import" : L"Imported " + std::to_wstring(out.size()) + (out.size() == 1 ? L" file" : L" files"), L"",
              nullptr, nullptr, 2000);
    model.selection = std::set<std::wstring>(out.begin(), out.end());
}

void Gallery::ActionsPalette() {
    const auto ts = model.Targets();
    const size_t n = ts.size();
    struct Act {
        std::wstring title, keywords, hint;
        wchar_t icon;
        std::function<void()> run;
    };
    std::vector<Act> acts = {
        {L"Find similar captures", L"reverse image search look alike", L"", 0xE71E, [this] { FindSimilar(); }},
        {L"Add tags…", L"tag label", L"T", 0xE8EC, [this] { TagPicker(); }},
        {L"Add to collection…", L"folder file category", L"F", 0xE8F4, [this] { CollectionPicker(); }},
        {L"Preview", L"quick look view full screen", L"Space", 0xE890, [this] {
             const auto sel = model.Selected();
             SetPreview(!model.focus.empty() ? model.focus : sel.empty() ? L"" : sel.front());
         }},
        {L"Annotate / open", L"edit", L"Enter", 0xE70F, [this] { Open(); }},
        {L"Copy", L"clipboard", L"Ctrl+C", 0xE8C8, [this] { Copy(); }},
        {L"Copy text (OCR)", L"ocr", L"Ctrl+T", 0xE8D2, [this] { CopyText(); }},
        {L"Pin to screen", L"float", L"Ctrl+P", 0xE718, [this] { Pin(); }},
        {n > 1 ? L"Batch rename…" : L"Rename…", L"name", L"Ctrl+R", 0xE8AC, [this] { Rename(); }},
        {L"Upload and copy link", L"share imgur s3", L"Ctrl+U", 0xE898, [this] { Upload(); }},
        {L"Show in Explorer", L"reveal folder", L"Ctrl+O", 0xE838, [this] { Reveal(); }},
        {L"Move to the Recycle Bin", L"delete remove trash", L"Del", 0xE74D, [this] { Trash(); }},
    };
    if (n >= 2) acts.push_back({L"Make collage", L"combine grid layout", L"Ctrl+G", 0xF0E2, [this] { Collage(); }});
    if (host.editorOpen && host.editorOpen()) acts.push_back({L"Add to open editor", L"insert layer image", L"", 0xE710, [this] { AddToEditor(); }});
    acts.push_back({L"Compare", L"before after versions", L"C", 0xE8AB, [this] { Compare(); }});
    for (int r = 0; r <= 5; ++r)
        acts.push_back({r == 0 ? L"Clear rating" : L"Rate " + Stars(r), L"stars rating", std::to_wstring(r), 0xE734, [this, r] { Rate(r); }});
    acts.push_back({L"Save filters as smart folder", L"saved search", L"Ctrl+Shift+S", 0xE838, [this] { SaveSmartFolder(); }});
    acts.push_back({L"Show duplicates", L"identical similar", L"", 0xE8C8, [this] { SetScope(Scope::Of(ScopeKind::Duplicates)); }});
    acts.push_back({L"Shuffle (random order)", L"random", L"", 0xE8B1, [this] {
                        model.SetSort(GallerySort::Random);
                        model.Shuffle();
                        Changed();
                    }});
    acts.push_back({L"Import files…", L"add", L"", 0xE8B5, [this] { ImportPanel(); }});
    std::vector<PaletteItem> items;
    for (size_t i = 0; i < acts.size(); ++i) items.push_back({(int)i, acts[i].title, acts[i].hint, acts[i].keywords, acts[i].icon});
    PaletteOptions opt;
    opt.placeholder = n > 0 ? L"Actions for " + (n == 1 ? FileNameOf(ts[0]) : std::to_wstring(n) + L" captures") + L"…" : L"Gallery actions…";
    auto shared = std::make_shared<std::vector<Act>>(std::move(acts));
    ShowPalette(std::move(items), [shared](int id) {
        if (id >= 0 && id < (int)shared->size()) (*shared)[id].run();
    }, opt);
}

void Gallery::ContextMenu(const std::wstring& u, POINT at) {
    HMENU m = CreatePopupMenu();
    std::vector<std::function<void()>> acts;
    auto add = [&](const std::wstring& title, std::function<void()> run) {
        acts.push_back(std::move(run));
        AppendMenuW(m, MF_STRING, acts.size(), title.c_str());
    };
    auto sep = [&] { AppendMenuW(m, MF_SEPARATOR, 0, nullptr); };
    const MediaType mt = MediaTypeOf(u);
    add(mt == MediaType::Image ? L"Annotate" : mt == MediaType::Video ? L"Edit video" : L"Open", [this, u] { Open(u); });
    add(L"Preview\tSpace", [this, u] { SetPreview(u); });
    add(L"Find similar", [this, u] { FindSimilar(u); });
    const bool multi = model.selection.size() > 1 && model.selection.count(u);
    if (multi) add(L"Make collage\tCtrl+G", [this] { Collage(); });
    if (host.editorOpen && host.editorOpen()) add(L"Add to open editor", [this] { AddToEditor(); });
    if (model.selection.size() == 2 && model.selection.count(u)) {
        add(L"Compare\tC", [this] { Compare(); });
    } else if (model.Versions(u).size() > 1) {
        add(L"Compare with previous version", [this, u] {
            model.selection = {u};
            model.focus = u;
            Compare();
        });
    }
    if (model.stacks.count(u)) add(model.IsExpanded(u) ? L"Collapse versions" : L"Show all versions", [this, u] {
        model.ToggleStack(u);
        Changed();
    });
    sep();
    add(L"Add tags…\tT", [this] { TagPicker(); });
    add(L"Add to collection…\tF", [this] { CollectionPicker(); });
    HMENU rating = CreatePopupMenu();
    for (int r = 0; r <= 5; ++r) {
        acts.push_back([this, r] { Rate(r); });
        AppendMenuW(rating, MF_STRING, acts.size(), r == 0 ? L"None" : Stars(r).c_str());
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)rating, L"Rating");
    if (model.scope().kind == ScopeKind::Collection) {
        const std::wstring id = model.scope().id;
        add(L"Remove from this collection", [this, id] { model.lib.RemoveFromCollection(model.Targets(), id); });
    }
    sep();
    add(L"Copy\tCtrl+C", [this] { Copy(); });
    add(L"Copy text (OCR)\tCtrl+T", [this] { CopyText(); });
    add(L"Pin to screen\tCtrl+P", [this] { Pin(); });
    add(std::wstring(multi ? L"Batch rename…" : L"Rename…") + L"\tCtrl+R", [this] { Rename(); });
    add(L"Upload and copy link\tCtrl+U", [this] { Upload(); });
    add(L"Show in Explorer\tCtrl+O", [this] { Reveal(); });
    sep();
    add(L"Move to the Recycle Bin\tDel", [this] { Trash(); });
    const int id = Menu(m, at);
    if (id > 0 && id <= (int)acts.size()) acts[id - 1]();
}

void Gallery::ScopeMenu() {
    HMENU m = CreatePopupMenu();
    std::vector<Scope> scopes;
    auto header = [&](const wchar_t* t) { AppendMenuW(m, MF_STRING | MF_DISABLED | MF_GRAYED, 0, t); };
    auto add = [&](const Scope& sc, const std::wstring& title) {
        scopes.push_back(sc);
        AppendMenuW(m, MF_STRING | (model.scope() == sc ? MF_CHECKED : 0), scopes.size(), (L"   " + title).c_str());
    };
    header(L"Library");
    add(Scope(), L"All captures");
    add(Scope::Of(ScopeKind::Recent), L"Last 7 days");
    add(Scope::Of(ScopeKind::Rated), L"Rated");
    add(Scope::Of(ScopeKind::Uncategorized), L"Uncategorized");
    add(Scope::Of(ScopeKind::Duplicates), L"Duplicates");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    header(L"Types");
    for (MediaType t : {MediaType::Image, MediaType::Gif, MediaType::Video}) add(Scope::OfType(t), MediaTypeLabel(t));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    header(L"Collections");
    for (const auto& c : model.lib.Collections()) add(Scope::OfCollection(c.id), c.name);
    AppendMenuW(m, MF_STRING, 900, L"   New collection…");
    if (!model.lib.SmartFolders().empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        header(L"Smart folders");
        for (const auto& folder : model.lib.SmartFolders()) add(Scope::OfSmart(folder.id), folder.name);
    }
    const int id = Menu(m, ScreenPt(scopeBtn.left, scopeBtn.bottom + S(4)));
    if (id == 900) return NewCollection();
    if (id > 0 && id <= (int)scopes.size()) SetScope(scopes[id - 1]);
}

void Gallery::ViewMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Rows");
    AppendMenuW(m, MF_STRING, 2, L"Grid");
    AppendMenuW(m, MF_STRING, 3, L"List");
    CheckMenuRadioItem(m, 1, 3, 1 + (int)model.layout, MF_BYCOMMAND);
    HMENU sortMenu = CreatePopupMenu();
    for (int i = 0; i <= (int)GallerySort::Random; ++i) AppendMenuW(sortMenu, MF_STRING, 20 + i, SortLabel((GallerySort)i));
    CheckMenuRadioItem(sortMenu, 20, 20 + (int)GallerySort::Random, 20 + (int)model.sort(), MF_BYCOMMAND);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sortMenu, (std::wstring(L"Sort: ") + SortLabel(model.sort())).c_str());
    if (model.sort() == GallerySort::Random) AppendMenuW(m, MF_STRING, 30, L"Shuffle again");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const UINT listGray = model.layout == GalleryLayout::List ? MF_GRAYED : 0;
    AppendMenuW(m, MF_STRING | listGray, 4, L"Larger thumbnails\tCtrl+=");
    AppendMenuW(m, MF_STRING | listGray, 5, L"Smaller thumbnails\tCtrl+-");
    AppendMenuW(m, MF_STRING | (model.showNames ? MF_CHECKED : 0), 6, L"Show names");
    AppendMenuW(m, MF_STRING | (model.stackEdits ? MF_CHECKED : 0), 7, L"Stack edited versions");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (model.showSidebar ? MF_CHECKED : 0), 8, L"Sidebar\tCtrl+B");
    AppendMenuW(m, MF_STRING | (model.showInspector ? MF_CHECKED : 0), 9, L"Inspector\tCtrl+I");
    AppendMenuW(m, MF_STRING, 10, L"Keyboard shortcuts\tCtrl+/");
    const int id = Menu(m, ScreenPt(viewBtn.left, viewBtn.bottom + S(4)));
    switch (id) {
        case 1:
        case 2:
        case 3:
            model.layout = (GalleryLayout)(id - 1);
            model.SaveViewOptions();
            scrollY = 0;
            break;
        case 4:
            model.Zoom(1.15);
            tiles.Clear();
            break;
        case 5:
            model.Zoom(1 / 1.15);
            tiles.Clear();
            break;
        case 6:
            model.showNames = !model.showNames;
            model.SaveViewOptions();
            break;
        case 7: model.SetStackEdits(!model.stackEdits); break;
        case 8: ToggleSidebar(); return;
        case 9: ToggleInspector(); return;
        case 10: showShortcuts = true; break;
        case 30: model.Shuffle(); break;
        default:
            if (id >= 20 && id <= 20 + (int)GallerySort::Random) model.SetSort((GallerySort)(id - 20));
            else return;
    }
    Changed();
}

// ---------- window ----------

LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    Gallery* G = g_gallery;
    if (!G) return DefWindowProcW(h, m, w, l);
    if (m == WM_KEYDOWN) {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (h == G->search) {
            if (w == VK_ESCAPE) {
                SetFocus(G->hwnd);
                G->Changed(false);
                return 0;
            }
            if (w == VK_RETURN || w == VK_DOWN) {
                SetFocus(G->hwnd);
                if (G->model.focus.empty()) G->model.Move(1, false);
                G->Changed(false);
                return 0;
            }
            if (ctrl && (w == 'I' || w == 'K' || w == 'W' || w == 'B' || w == VK_OEM_2 || w == 'G' || (w == 'S' && shift))) {
                G->OnCommandKey(w, shift);
                return 0;
            }
        } else if (h == G->tagEdit) {
            if (w == VK_RETURN) {
                wchar_t buf[512] = {};
                GetWindowTextW(h, buf, 512);
                std::vector<std::wstring> ts = G->model.Single().empty() ? G->model.Selected() : std::vector<std::wstring>{G->model.Single()};
                if (ts.empty() && !G->model.focus.empty()) ts = {G->model.focus};
                G->model.lib.AddTags(SplitTags(buf), ts);
                SetWindowTextW(h, L"");
                if (ts.size() == 1) {
                    G->addingTag = false;
                    SetFocus(G->hwnd);
                }
                G->Changed(false);
                return 0;
            }
            if (w == VK_ESCAPE) {
                SetWindowTextW(h, L"");
                G->addingTag = false;
                SetFocus(G->hwnd);
                G->Changed(false);
                return 0;
            }
        } else if (h == G->commentEdit && w == VK_ESCAPE) {
            G->editingComment = false;
            SetFocus(G->hwnd);
            G->Changed(false);
            return 0;
        }
    }
    if (m == WM_CHAR && (w == VK_ESCAPE || (w == VK_RETURN && h != G->commentEdit))) return 0;  // no beep
    return CallWindowProcW(G->editProc, h, m, w, l);
}

LRESULT Gallery::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_THUMB: {
            auto* d = reinterpret_cast<ThumbDone*>(l);
            thumbs.Store(d);
            delete d;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LIBCHANGED:
            SetTimer(hwnd, kTimerLib, 120, nullptr);  // coalesce bursts (indexing, saves)
            return 0;
        case WM_TIMER:
            switch (w) {
                case kTimerLib:
                    KillTimer(hwnd, kTimerLib);
                    Recompute();
                    break;
                case kTimerSearch: {
                    KillTimer(hwnd, kTimerSearch);
                    wchar_t buf[512] = {};
                    GetWindowTextW(search, buf, 512);
                    Filter f = model.filter();
                    if (f.text != buf) {
                        f.text = buf;
                        model.SetFilter(f);
                        scrollY = 0;
                        UpdateSearchCue();
                        Changed();
                    }
                    break;
                }
                case kTimerTip:
                    KillTimer(hwnd, kTimerTip);
                    tipShown = !tipText.empty() && GetCapture() != hwnd;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                case kTimerSlide:
                    if (model.slideshow && !model.preview.empty()) {
                        model.Step(1);
                        Changed(false);
                    } else {
                        KillTimer(hwnd, kTimerSlide);
                    }
                    break;
                case kTimerVideo: {
                    double t = 0;
                    if (!previewVideo || model.preview.empty()) {
                        KillTimer(hwnd, kTimerVideo);
                    } else if (auto f = previewVideo->NewFrame(&t)) {
                        previewImg = f;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else if (!previewVideo->Playing()) {
                        KillTimer(hwnd, kTimerVideo);  // ended: the play button comes back
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                }
                case kTimerGif:
                    KillTimer(hwnd, kTimerGif);
                    if (gif.Animated() && model.preview == gif.path) {
                        int delay = 100;
                        if (auto f = gif.Next(&delay)) previewImg = f;
                        SetTimer(hwnd, kTimerGif, delay, nullptr);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(w) == EN_CHANGE) {
                if ((HWND)l == search) {
                    SetTimer(hwnd, kTimerSearch, 120, nullptr);
                } else if ((HWND)l == tagEdit) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if ((HWND)l == commentEdit && !commentFor.empty()) {
                    const int len = GetWindowTextLengthW(commentEdit);
                    std::wstring text(len, L'\0');
                    GetWindowTextW(commentEdit, text.data(), len + 1);
                    if (text != model.lib.Meta(commentFor).comment) model.lib.SetComment(text, commentFor);
                }
            } else if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE);  // the search pill widens while typing
            }
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)w, theme::kText);
            SetBkColor((HDC)w, RGB(44, 45, 42));
            return (LRESULT)editBrush;
        case WM_KEYDOWN: OnKey(w); return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetFocus(hwnd);
            OnMouseDown({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, m == WM_LBUTTONDBLCLK);
            return 0;
        case WM_LBUTTONUP: OnMouseUp({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0;
        case WM_RBUTTONUP: OnRightClick({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0;
        case WM_MOUSEMOVE: OnMouseMove({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, w); return 0;
        case WM_MOUSELEAVE:
            hover = -1;
            tipShown = false;
            KillTimer(hwnd, kTimerTip);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &p);
            OnWheel(p, GET_WHEEL_DELTA_WPARAM(w), (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) != 0);
            return 0;
        }
        case WM_CAPTURECHANGED:
            draggingScroll = g_panning = g_splitting = false;
            if ((HWND)l != hwnd) {  // the press was lost (released elsewhere, another window took the mouse)
                g_havePressed = false;
                g_pressed = Hot{};
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && (HWND)w == hwnd) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                const int hi = HotAt(p);
                SetCursor(LoadCursorW(nullptr, hi >= 0 ? (hots[hi].tile >= 0 ? IDC_ARROW : hots[hi].cursor) : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DROPFILES: OnDropFiles((HDROP)w); return 0;
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) Changed();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            mmi->ptMinTrackSize = {S(640), S(420)};
            return 0;
        }
        case WM_DPICHANGED: {
            s = HIWORD(w) / 96.f;
            Fonts();
            tiles.Clear();
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            Changed();
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE && !Typing() && GetFocus() != tagEdit && GetFocus() != commentEdit) SetFocus(hwnd);
            return 0;
        case WM_DESTROY: {
            WINDOWPLACEMENT wp{sizeof(wp)};
            if (GetWindowPlacement(hwnd, &wp)) {
                const std::wstring ini = SupportFolder() + L"\\gallery.ini";
                // rcNormalPosition is in workspace coordinates; CreateWindowEx takes screen coordinates.
                RECT r = wp.rcNormalPosition;
                MONITORINFO mi{sizeof(mi)};
                if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
                    OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
                wchar_t v[96];
                swprintf_s(v, L"%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
                WritePrivateProfileStringW(L"Window", L"Placement", v, ini.c_str());
            }
            model.lib.Unsubscribe(libListener);
            thumbs.Stop();
            previewVideo.reset();
            g_havePressed = false;  // holds a click bound to this window
            g_pressed = Hot{};
            return 0;
        }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

LRESULT CALLBACK GalleryProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* g = static_cast<Gallery*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        g->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)g);
    }
    auto* g = reinterpret_cast<Gallery*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!g) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        for (HFONT f : {g->fUi, g->fSmall, g->fTiny, g->fBold, g->fTitle, g->fIcon, g->fIconSmall, g->fBadge, g->fHead, g->fBig, g->fStar}) DeleteObject(f);
        DeleteObject(g->editBrush);
        if (g_gallery == g) g_gallery = nullptr;
        delete g;
        return DefWindowProcW(h, m, w, l);
    }
    return g->Proc(m, w, l);
}

bool Gallery::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = GalleryProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = host.icon;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    POINT pt;
    GetCursorPos(&pt);
    s = DpiScaleAt(pt);
    Fonts();
    model.LoadViewOptions();
    const std::wstring ini = SupportFolder() + L"\\gallery.ini";
    tagsCollapsed = GetPrivateProfileIntW(L"View", L"TagsCollapsed", 1, ini.c_str()) != 0;
    RECT work = MonitorRectAt(pt, true);
    int w = std::min(S(1280), (int)(RectW(work) * 0.92)), h = std::min(S(820), (int)(RectH(work) * 0.92));
    int x = work.left + (RectW(work) - w) / 2, y = work.top + (RectH(work) - h) / 2;
    bool maximized = false;
    wchar_t place[96] = {};
    GetPrivateProfileStringW(L"Window", L"Placement", L"", place, 96, ini.c_str());
    RECT saved{};
    int mx = 0;
    if (swscanf_s(place, L"%ld,%ld,%ld,%ld,%d", &saved.left, &saved.top, &saved.right, &saved.bottom, &mx) == 5 && RectW(saved) > 200 &&
        RectH(saved) > 150 && MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        x = saved.left;
        y = saved.top;
        w = RectW(saved);
        h = RectH(saved);
        maximized = mx != 0;
    }
    if (!CreateWindowExW(WS_EX_ACCEPTFILES, kClass, L"Capture gallery", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr,
                         GetModuleHandleW(nullptr), this))
        return false;
    // The saved position can be on another monitor than the cursor: use that monitor's scale.
    if (const UINT dpi = GetDpiForWindow(hwnd); dpi && std::fabs(dpi / 96.f - s) > 0.01f) {
        s = dpi / 96.f;
        Fonts();
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    auto makeEdit = [&](int id, DWORD style) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | style, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)fUi, TRUE);
        WNDPROC old = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)EditSubclass);
        if (!editProc) editProc = old;
        return e;
    };
    search = makeEdit(kSearchId, ES_AUTOHSCROLL | WS_VISIBLE);
    tagEdit = makeEdit(kTagEditId, ES_AUTOHSCROLL);
    commentEdit = makeEdit(kCommentId, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN);
    SendMessageW(tagEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Add tags, comma-separated");
    SendMessageW(commentEdit, EM_SETLIMITTEXT, 4000, 0);
    thumbs.Start(hwnd);
    HWND self = hwnd;
    libListener = model.lib.Subscribe([self] { PostMessageW(self, WM_LIBCHANGED, 0, 0); });
    model.Recompute();
    UpdateSearchCue();
    Layout();
    model.lib.Refresh([self] {
        if (g_gallery && g_gallery->hwnd == self) {
            g_gallery->Recompute();
            if (g_gallery->model.focus.empty() && !g_gallery->model.visible.empty()) g_gallery->model.focus = g_gallery->model.visible.front();
        }
    });
    if (snapshotMode) return true;  // drawn into memory by GallerySnapshots, never shown
    ShowWindow(hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOW);
    ForceForeground(hwnd);
    SetFocus(hwnd);  // shortcuts work right away: the search box only takes focus when asked
    return true;
}

}  // namespace

void ShowGallery(const GalleryHost& host) {
    if (g_gallery) {
        g_gallery->host = host;
        g_gallery->model.lib.Refresh();
        if (IsIconic(g_gallery->hwnd)) ShowWindow(g_gallery->hwnd, SW_RESTORE);
        ForceForeground(g_gallery->hwnd);
        SetFocus(g_gallery->hwnd);
        return;
    }
    auto* g = new Gallery(host);
    g_gallery = g;
    if (!g->Create()) {
        g_gallery = nullptr;
        delete g;
    }
}

bool GalleryVisible() { return g_gallery && IsWindowVisible(g_gallery->hwnd); }

}  // namespace ather

namespace ather {
namespace {

// ---------- developer snapshots ----------

LRESULT CALLBACK SnapMsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP_RUN) {
        auto* fn = reinterpret_cast<std::function<void()>*>(l);
        (*fn)();
        delete fn;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

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

BitmapPtr Card(int w, int h, COLORREF a, COLORREF b, const std::wstring& title, const std::wstring& body) {
    auto bmp = Bitmap::Create(w, h);
    {
        gp::Bitmap gb(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(bmp->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        gp::LinearGradientBrush bg(gp::Point(0, 0), gp::Point(w, h), A(a), A(b));
        g.FillRectangle(&bg, 0, 0, w, h);
        gp::GraphicsPath p;
        RoundPath(p, w / 12.f, h / 6.f, w * 5 / 6.f, h * 2 / 3.f, 18);
        gp::SolidBrush card(gp::Color(235, 255, 255, 255));
        g.FillPath(&card, &p);
        gp::Font tf(L"Segoe UI", h / 12.f, gp::FontStyleBold, gp::UnitPixel);
        gp::Font bf(L"Segoe UI", h / 22.f, gp::FontStyleRegular, gp::UnitPixel);
        gp::SolidBrush ink(gp::Color(255, 0, 0, 0)), grey(gp::Color(255, 90, 90, 90));
        g.DrawString(title.c_str(), (INT)title.size(), &tf, gp::PointF(w / 8.f, h / 4.f), &ink);
        g.DrawString(body.c_str(), (INT)body.size(), &bf, gp::PointF(w / 8.f, h / 4.f + h / 7.f), &grey);
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) bmp->Bits()[i] |= 0xFF000000u;
    return bmp;
}

void SetMtime(const std::wstring& path, double hoursAgo) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t t = (uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime;
    t -= (uint64_t)(hoursAgo * 3600.0 * 1e7);
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

}  // namespace

int GallerySnapshots(const std::wstring& outDir) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring root = std::wstring(tmp) + L"AtherGallerySnap-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring support = root + L"\\support", caps = root + L"\\caps", month = caps + L"\\2026-10";
    SHCreateDirectoryExW(nullptr, support.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, month.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);
    SetEnvironmentVariableW(L"ATHER_SUPPORT_DIR", support.c_str());  // never the real library

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SnapMsgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AtherSnapMsg";
    RegisterClassExW(&wc);
    HWND msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    SetUiWindow(msgWnd);

    struct Spec {
        const wchar_t* name;
        int w, h;
        COLORREF a, b;
        const wchar_t *title, *body;
    };
    const Spec specs[] = {
        {L"Checkout error", 1600, 1000, RGB(255, 59, 48), RGB(255, 149, 0), L"Payment failed", L"Error 402 · card declined"},
        {L"Dashboard", 1800, 1000, RGB(48, 176, 199), RGB(10, 132, 255), L"Weekly revenue", L"Up 12% over last week"},
        {L"Login screen", 900, 1400, RGB(88, 86, 214), RGB(175, 82, 222), L"Sign in", L"Use your Ather account"},
        {L"Release notes", 1400, 900, RGB(52, 199, 89), RGB(48, 176, 199), L"Version 0.0.1", L"Gallery, tags and smart folders"},
        {L"Long page", 800, 2600, RGB(142, 142, 147), RGB(72, 72, 74), L"Docs", L"Scrolling capture of a long page"},
        {L"Bug report", 1500, 950, RGB(255, 45, 85), RGB(255, 59, 48), L"Crash on launch", L"Stack trace attached"},
        {L"Design review", 2000, 900, RGB(255, 204, 0), RGB(255, 149, 0), L"Hero section", L"New illustration and copy"},
        {L"Settings", 1200, 1200, RGB(10, 132, 255), RGB(88, 86, 214), L"Preferences", L"Shortcuts and uploads"},
        {L"Chat", 1000, 1500, RGB(0, 199, 190), RGB(52, 199, 89), L"Team chat", L"Ship it on Friday?"},
        {L"Analytics", 1700, 950, RGB(175, 82, 222), RGB(255, 45, 85), L"Funnel", L"Drop-off at step 3"},
        {L"Dashboard copy", 1800, 1000, RGB(48, 176, 199), RGB(10, 132, 255), L"Weekly revenue", L"Up 12% over last week"},
        {L"Terminal", 1500, 900, RGB(0, 0, 0), RGB(60, 60, 60), L"build succeeded", L"12 tests passed"},
    };
    auto url = [&](const std::wstring& n) { return month + L"\\" + n + L".png"; };
    for (size_t i = 0; i < std::size(specs); ++i) {
        const Spec& s = specs[i];
        SavePng(*Card(s.w, s.h, s.a, s.b, s.title, s.body), url(s.name));
        SetMtime(url(s.name), (double)i);
    }
    Library& lib = Library::Shared();
    lib.SetFolder(caps);
    lib.LoadIfNeeded();
    bool scanned = false;
    lib.Refresh([&] { scanned = true; });
    for (int i = 0; i < 400 && !scanned; ++i) Pump(25);
    for (int i = 0; i < 1200; ++i) {  // wait for indexing (OCR included)
        Pump(50);
        bool all = lib.Progress().second == 0;
        for (const auto& p : lib.Paths()) all = all && lib.Meta(p).indexed > 0;
        if (all) break;
    }
    lib.AddTags({L"bug", L"checkout"}, {url(L"Checkout error"), url(L"Bug report")});
    lib.AddTags({L"dashboard"}, {url(L"Dashboard"), url(L"Analytics"), url(L"Dashboard copy")});
    lib.AddTags({L"design"}, {url(L"Design review"), url(L"Login screen")});
    lib.SetRating(5, {url(L"Dashboard")});
    lib.SetRating(3, {url(L"Design review")});
    auto col = lib.CreateCollection(L"Sprint 42");
    lib.AddToCollection({url(L"Checkout error"), url(L"Bug report"), url(L"Release notes")}, col.id);
    Filter red;
    red.color = L"#FF3B30";
    lib.SaveSmartFolder(L"Red things", red);
    lib.TestSetApp(url(L"Dashboard"), L"chrome");
    lib.TestSetApp(url(L"Terminal"), L"WindowsTerminal");
    lib.SetComment(L"Numbers look off for Tuesday — check with data team.", url(L"Dashboard"));
    // An edited version: stacks with the original.
    const std::wstring edited = month + L"\\Dashboard annotated.png";
    lib.NoteEdit(edited, url(L"Dashboard"), L"", L"", true);
    SavePng(*Card(1800, 1000, RGB(255, 149, 0), RGB(175, 82, 222), L"Weekly revenue ✎", L"Annotated: Tuesday dip circled"), edited);
    SetMtime(edited, -0.02);
    for (int i = 0; i < 200 && (!lib.Has(edited) || lib.Meta(edited).indexed == 0); ++i) Pump(50);

    GalleryHost host;
    host.regionHotkey = L"PrintScreen";
    auto* g = new Gallery(host);
    g->snapshotMode = true;
    g_gallery = g;
    g->Create();
    RECT wr{0, 0, 1320, 820};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(g->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    GalleryModel& m = g->model;
    m.showSidebar = m.showInspector = m.inspectorDiscovered = false;
    g->Recompute();
    auto snap = [&](const wchar_t* name) {
        g->Layout();
        {  // a first paint requests the thumbnails it needs
            auto warm = Bitmap::Create(1, 1);
            MemDC wdc(warm->Handle());
            g->Paint(wdc);
        }
        Pump(1200);  // thumbnails arrive
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        auto out = Bitmap::Create(RectW(rc), RectH(rc));
        {
            MemDC dc(out->Handle());
            g->Paint(dc);
            for (HWND e : {g->search, g->tagEdit, g->commentEdit}) {  // the edit boxes are child windows
                if (!(GetWindowLongW(e, GWL_STYLE) & WS_VISIBLE)) continue;  // the parent is hidden
                RECT er;
                GetWindowRect(e, &er);
                MapWindowPoints(nullptr, g->hwnd, (POINT*)&er, 2);
                POINT old;
                SetViewportOrgEx(dc, er.left, er.top, &old);
                SendMessageW(e, WM_PRINT, (WPARAM)(HDC)dc, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
                SetViewportOrgEx(dc, old.x, old.y, nullptr);
            }
        }
        for (size_t i = 0, n = (size_t)out->Width() * out->Height(); i < n; ++i) out->Bits()[i] |= 0xFF000000u;
        SavePng(*out, outDir + L"\\" + name + L".png");
    };
    snap(L"gallery-canvas");
    m.selection = {edited};
    m.focus = edited;
    g->Changed(false);
    snap(L"gallery-selected");
    m.SetShowInspector(true);
    snap(L"gallery-inspector");
    m.showInspector = false;
    m.showSidebar = true;
    g->tagsCollapsed = false;
    snap(L"gallery-sidebar");
    m.showSidebar = false;
    g->showShortcuts = true;
    snap(L"gallery-shortcuts");
    g->showShortcuts = false;
    Filter f;
    f.text = L"revenue";
    g->SetFilter(f);
    snap(L"gallery-search");
    Filter tf;
    tf.tags = {L"bug"};
    tf.color = L"#FF3B30";
    g->SetFilter(tf);
    snap(L"gallery-filtered");
    g->filterOpen = true;
    snap(L"gallery-filter-panel");
    g->filterOpen = false;
    g->SetFilter(Filter());
    g->SetScope(Scope::Of(ScopeKind::Duplicates));
    snap(L"gallery-duplicates");
    g->FindSimilar(url(L"Checkout error"));
    snap(L"gallery-similar");
    g->SetScope(Scope());
    m.layout = GalleryLayout::Grid;
    m.selection = {url(L"Bug report"), url(L"Checkout error"), url(L"Chat")};
    g->Changed();
    snap(L"gallery-grid-multi");
    m.layout = GalleryLayout::List;
    g->Changed();
    snap(L"gallery-list");
    m.layout = GalleryLayout::Justified;
    m.selection.clear();
    g->SetPreview(url(L"Login screen"));
    snap(L"gallery-preview");
    g->SetPreview(L"");
    m.compare = std::make_pair(url(L"Dashboard"), edited);
    snap(L"gallery-compare");
    m.compare.reset();
    SetWindowPos(g->hwnd, nullptr, 0, 0, 720, 640, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    g->Changed();
    snap(L"gallery-narrow");
    m.SaveViewOptions();
    DestroyWindow(g->hwnd);
    lib.StopBackgroundWork();
    lib.Flush();
    DestroyWindow(msgWnd);
    std::wstring from = root + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
    return 0;
}

}  // namespace ather
