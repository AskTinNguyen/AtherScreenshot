#include "videoedit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

#include "annot.h"
#include "selftest.h"
#include "textdraw.h"

namespace ather {

// ---------- geometry and labels ----------

VRect VRect::Integral() const {
    const double x0 = std::floor(x), y0 = std::floor(y), x1 = std::ceil(x + w), y1 = std::ceil(y + h);
    return {x0, y0, x1 - x0, y1 - y0};
}

VRect NormRect(VPoint a, VPoint b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)}; }

static std::optional<VRect> Intersect(const VRect& a, const VRect& b) {
    const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.MaxX(), b.MaxX()), y1 = std::min(a.MaxY(), b.MaxY());
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return VRect{x0, y0, x1 - x0, y1 - y0};
}

const wchar_t* KindLabel(MarkKind k) {
    static const wchar_t* const n[] = {L"Text", L"Speech bubble", L"Emoji", L"Arrow", L"Box", L"Ellipse", L"Step number",
                                       L"Blur", L"Pixelate", L"Zoom", L"Title card"};
    return n[(int)k];
}

std::wstring KindPlural(MarkKind k) {
    if (k == MarkKind::Box) return L"boxes";
    if (k == MarkKind::Emoji) return L"emoji";
    std::wstring s = KindLabel(k);
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s + L"s";
}

wchar_t KindKey(MarkKind k) {
    switch (k) {
        case MarkKind::Arrow: return L'A';
        case MarkKind::Box: return L'R';
        case MarkKind::Emoji: return L'E';
        case MarkKind::Step: return L'N';
        case MarkKind::Blur: return L'X';
        case MarkKind::Zoom: return L'Z';
        default: return 0;
    }
}

wchar_t KindGlyph(MarkKind k) {
    switch (k) {
        case MarkKind::Text: return 0xE8D2;      // Font
        case MarkKind::Bubble: return 0xE8BD;    // Message
        case MarkKind::Emoji: return 0xE76E;     // Emoji2
        case MarkKind::Arrow: return 0xE8AD;     // Go (arrow)
        case MarkKind::Box: return 0xE739;       // Checkbox (square)
        case MarkKind::Ellipse: return 0xEA3A;   // circle
        case MarkKind::Step: return 0xF146;      // circled 1
        case MarkKind::Blur: return 0xEB42;      // drop
        case MarkKind::Pixelate: return 0xF0E2;  // grid
        case MarkKind::Zoom: return 0xE8A3;      // ZoomIn
        case MarkKind::Title: return 0xE8A1;     // card
    }
    return 0xE710;
}

bool KindIsLine(MarkKind k) { return k == MarkKind::Arrow; }
bool KindHasText(MarkKind k) { return k == MarkKind::Text || k == MarkKind::Bubble || k == MarkKind::Emoji || k == MarkKind::Title; }
bool KindIsRegion(MarkKind k) { return k == MarkKind::Blur || k == MarkKind::Pixelate || k == MarkKind::Zoom; }
bool KindIsStroke(MarkKind k) { return k == MarkKind::Arrow || k == MarkKind::Box || k == MarkKind::Ellipse; }
double KindDefaultSeconds(MarkKind k) { return k == MarkKind::Title ? 2.5 : 3; }

AnimStyle KindDefaultStyle(MarkKind k) {
    switch (k) {
        case MarkKind::Arrow:
        case MarkKind::Box:
        case MarkKind::Ellipse: return AnimStyle::DrawOn;
        case MarkKind::Step:
        case MarkKind::Emoji:
        case MarkKind::Bubble: return AnimStyle::Pop;
        case MarkKind::Text: return AnimStyle::Slide;
        case MarkKind::Title: return AnimStyle::Fade;
        default: return AnimStyle::None;
    }
}

std::vector<AnimStyle> KindStyles(MarkKind k) {
    using A = AnimStyle;
    switch (k) {
        case MarkKind::Arrow:
        case MarkKind::Box:
        case MarkKind::Ellipse: return {A::None, A::Fade, A::DrawOn, A::Pop, A::Scale};
        case MarkKind::Step:
        case MarkKind::Emoji: return {A::None, A::Fade, A::Pop, A::Scale, A::Slide};
        case MarkKind::Text:
        case MarkKind::Bubble: return {A::None, A::Fade, A::Pop, A::Slide, A::Wipe, A::BlurIn, A::Typewriter};
        case MarkKind::Title: return {A::None, A::Fade, A::Slide, A::Wipe, A::BlurIn};
        case MarkKind::Blur:
        case MarkKind::Pixelate: return {A::None, A::Fade, A::BlurIn};
        case MarkKind::Zoom: return {};
    }
    return {};
}

const wchar_t* StyleLabel(AnimStyle s) {
    static const wchar_t* const n[] = {L"Auto", L"None", L"Fade", L"Pop", L"Scale", L"Slide up", L"Wipe", L"Blur in", L"Draw on", L"Typewriter"};
    return n[(int)s];
}

const std::vector<AnimStyle>& CaptionStyles() {
    static const std::vector<AnimStyle> s = {AnimStyle::Auto, AnimStyle::None, AnimStyle::Fade, AnimStyle::Pop, AnimStyle::Slide, AnimStyle::Typewriter};
    return s;
}

const wchar_t* EmphasisLabel(Emphasis e) {
    static const wchar_t* const n[] = {L"None", L"Pulse", L"Bounce", L"Shake", L"Ping"};
    return n[(int)e];
}

const wchar_t* LookLabel(CaptionLook l) {
    static const wchar_t* const n[] = {L"Pill", L"Outline", L"Bar"};
    return n[(int)l];
}

const wchar_t* PositionLabel(CaptionPosition p) {
    static const wchar_t* const n[] = {L"Bottom", L"Middle", L"Top"};
    return n[(int)p];
}

const wchar_t* const VideoEdit::kCaptionSizes[5] = {L"Extra small", L"Small", L"Medium", L"Large", L"Extra large"};

uint64_t NewItemId() {
    static std::atomic<uint64_t> next{1};
    return next++;
}

AnimStyle Mark::InStyle() const { return style == AnimStyle::Auto ? KindDefaultStyle(kind) : style; }

AnimStyle Mark::OutStyle() const {
    AnimStyle s = exit ? (*exit == AnimStyle::Auto ? KindDefaultStyle(kind) : *exit) : InStyle();
    return s == AnimStyle::DrawOn || s == AnimStyle::Typewriter ? AnimStyle::Fade : s;  // these don't play backwards well
}

// ---------- motion ----------

void Motion::Apply(AnimStyle s, double p, double height) {
    p = std::clamp(p, 0.0, 1.0);
    const double out = 1 - std::pow(1 - p, 3);
    switch (s) {
        case AnimStyle::None:
        case AnimStyle::Auto: break;
        case AnimStyle::Fade: alpha = p; break;
        case AnimStyle::Pop: {
            const double back = 1 + 2.2 * std::pow(p - 1, 3) + 1.2 * std::pow(p - 1, 2);  // ease-out with a little overshoot
            alpha = std::min(1.0, p * 2.5);
            scale = 0.55 + 0.45 * back;
            break;
        }
        case AnimStyle::Scale:
            alpha = p;
            scale = 0.85 + 0.15 * out;
            break;
        case AnimStyle::Slide:
            alpha = p;
            dy = (1 - out) * height * 0.035;
            break;
        case AnimStyle::Wipe:
            wipe = true;
            reveal = out;
            break;
        case AnimStyle::BlurIn:
            alpha = std::min(1.0, p * 1.6);
            blur = (1 - out) * std::max(1.0, height / 720) * 12;
            break;
        case AnimStyle::DrawOn:
        case AnimStyle::Typewriter: reveal = p; break;
    }
}

Motion Motion::Between(AnimStyle inS, AnimStyle outS, double start, double end, double t, int chars, double height) {
    const double len = std::max(0.05, end - start);
    auto dur = [&](AnimStyle s, bool entering) -> double {
        switch (s) {
            case AnimStyle::None:
            case AnimStyle::Auto: return 0;
            case AnimStyle::Pop: return entering ? 0.24 : 0.16;
            case AnimStyle::DrawOn: return std::min(0.6, len / 2);
            case AnimStyle::Typewriter: return std::min(len * 0.6, std::max(0.3, chars * 0.035));
            case AnimStyle::Wipe: return 0.4;
            default: return entering ? 0.3 : 0.22;
        }
    };
    Motion m;
    const double di = std::min(dur(inS, true), len / 2), dO = std::min(dur(outS, false), len / 2);
    if (di > 0 && t - start < di) m.Apply(inS, (t - start) / di, height);
    if (dO > 0 && end - t < dO) {
        Motion o;
        o.Apply(outS, std::max(0.0, end - t) / dO, height);
        m.alpha *= o.alpha;
        m.scale *= o.scale;
        m.dx += o.dx;
        m.dy += o.dy;
        m.blur = std::max(m.blur, o.blur);
        if (o.wipe) {
            m.wipe = true;
            m.reveal = std::min(m.reveal, o.reveal);
        }
    }
    return m;
}

// ---------- captions from words ----------

std::vector<Caption> ChunkCaptions(const std::vector<CaptionWord>& words) {
    std::vector<Caption> out;
    std::optional<Caption> cur;
    for (const auto& w : words) {
        if (cur && w.start - cur->end < 0.7 && cur->text.size() + w.text.size() < 42 && w.end - cur->start < 3.5) {
            cur->text += L" " + w.text;
            cur->end = w.end;
            cur->words.push_back(w);
        } else {
            if (cur) out.push_back(*cur);
            Caption c;
            c.start = w.start;
            c.end = std::max(w.end, w.start + 0.4);
            c.text = w.text;
            c.words = {w};
            cur = c;
        }
    }
    if (cur) out.push_back(*cur);
    // Keep each caption on screen until the next one, up to a short hold.
    for (size_t i = 0; i < out.size(); ++i) {
        const double next = i + 1 < out.size() ? out[i + 1].start : out[i].end + 0.8;
        out[i].end = std::min(next, out[i].end + 0.8);
        if (!out[i].words.empty()) out[i].words.back().end = out[i].end;  // the last word stays lit
    }
    return out;
}

// ---------- pixels ----------

namespace {

inline uint32_t Scale(uint32_t p, uint32_t k) {  // premultiplied pixel × k/255
    if (k >= 255) return p;
    return (((p & 0xFF) * k + 127) / 255) | (((p >> 8 & 0xFF) * k + 127) / 255) << 8 | (((p >> 16 & 0xFF) * k + 127) / 255) << 16 |
           (((p >> 24) * k + 127) / 255) << 24;
}

inline void Over(uint32_t& d, uint32_t s) {  // premultiplied source over
    const uint32_t sa = s >> 24;
    if (!sa) return;
    if (sa == 255) {
        d = s;
        return;
    }
    d = s + Scale(d, 255 - sa);
}

inline uint32_t Mix(uint32_t a, uint32_t b, double t) {  // a·(1−t) + b·t
    const uint32_t k = (uint32_t)std::clamp(std::lround(t * 255), 0L, 255L);
    return Scale(a, 255 - k) + Scale(b, k);
}

BitmapPtr Copy(const Bitmap& src) {
    auto b = Bitmap::Create(src.Width(), src.Height());
    if (b) memcpy(b->Bits(), src.Bits(), (size_t)src.Width() * src.Height() * 4);
    return b;
}

BitmapPtr Blank(int w, int h) {
    auto b = Bitmap::Create(std::max(1, w), std::max(1, h));
    if (b) memset(b->Bits(), 0, (size_t)b->Width() * b->Height() * 4);
    return b;
}

// Gaussian-like blur (three box passes) of `inner`, sampling up to 3 radii around it so the edges blend with
// the surroundings (like Core Image's clamped blur). Returns the blurred pixels of `inner`.
std::vector<uint32_t> BlurArea(const Bitmap& img, const RECT& inner, double sigma) {
    const int r = std::max(1, (int)std::lround(sigma));
    const int W = img.Width(), H = img.Height();
    const RECT P{std::max(0L, inner.left - 3 * r), std::max(0L, inner.top - 3 * r), std::min((LONG)W, inner.right + 3 * r),
                 std::min((LONG)H, inner.bottom + 3 * r)};
    const int pw = RectW(P), ph = RectH(P), iw = RectW(inner), ih = RectH(inner);
    std::vector<uint32_t> out((size_t)iw * ih, 0);
    std::vector<float> plane((size_t)pw * ph), tmp((size_t)std::max(pw, ph));
    auto pass = [&](float* data, int n, int stride) {
        const int rr = std::min(r, n - 1);
        const float inv = 1.f / (2 * rr + 1);
        float sum = data[0] * (rr + 1);
        for (int i = 1; i <= rr; ++i) sum += data[(size_t)i * stride];
        for (int i = 0; i < n; ++i) {
            tmp[i] = sum * inv;
            sum += data[(size_t)std::min(i + rr + 1, n - 1) * stride] - data[(size_t)std::max(i - rr, 0) * stride];
        }
        for (int i = 0; i < n; ++i) data[(size_t)i * stride] = tmp[i];
    };
    for (int ch = 0; ch < 4; ++ch) {
        const int sh = ch * 8;
        for (int y = 0; y < ph; ++y) {
            const uint32_t* row = img.Bits() + (size_t)(P.top + y) * W + P.left;
            float* dst = plane.data() + (size_t)y * pw;
            for (int x = 0; x < pw; ++x) dst[x] = (float)((row[x] >> sh) & 255);
        }
        if (pw > 1 && ph > 1)
            for (int k = 0; k < 3; ++k) {
                for (int y = 0; y < ph; ++y) pass(plane.data() + (size_t)y * pw, pw, 1);
                for (int x = 0; x < pw; ++x) pass(plane.data() + x, ph, pw);
            }
        for (int y = 0; y < ih; ++y) {
            const float* src = plane.data() + (size_t)(inner.top - P.top + y) * pw + (inner.left - P.left);
            uint32_t* dst = out.data() + (size_t)y * iw;
            for (int x = 0; x < iw; ++x) dst[x] |= (uint32_t)std::clamp((int)std::lround(src[x]), 0, 255) << sh;
        }
    }
    return out;
}

// Bilinear sample of a premultiplied image; transparent outside it.
inline uint32_t Sample(const Bitmap& b, double sx, double sy) {
    const int W = b.Width(), H = b.Height();
    const int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
    const double fx = sx - x0, fy = sy - y0;
    auto at = [&](int x, int y) -> uint32_t { return x < 0 || y < 0 || x >= W || y >= H ? 0u : b.Bits()[(size_t)y * W + x]; };
    const uint32_t q[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
    const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    uint32_t out = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        double v = 0;
        for (int k = 0; k < 4; ++k) v += ((q[k] >> sh) & 255) * w[k];
        out |= (uint32_t)std::clamp((int)std::lround(v), 0, 255) << sh;
    }
    return out;
}

// The part `r` of `src` (fractional, source pixels) scaled to w × h. Opaque frames, so edges clamp.
BitmapPtr SampleRect(const Bitmap& src, VRect r, int w, int h) {
    auto out = Bitmap::Create(w, h);
    if (!out) return nullptr;
    const int W = src.Width(), H = src.Height();
    if (std::fabs(r.w - w) <= 1.01 && std::fabs(r.h - h) <= 1.01 && r.x == std::floor(r.x) && r.y == std::floor(r.y)) {  // a plain crop
        for (int y = 0; y < h; ++y) {
            const int sy = std::clamp((int)r.y + y, 0, H - 1);
            for (int x = 0; x < w; ++x) out->Bits()[(size_t)y * w + x] = src.Bits()[(size_t)sy * W + std::clamp((int)r.x + x, 0, W - 1)];
        }
        return out;
    }
    const double kx = r.w / w, ky = r.h / h;
    for (int y = 0; y < h; ++y) {
        const double sy = std::clamp(r.y + (y + 0.5) * ky - 0.5, 0.0, H - 1.0);
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp(r.x + (x + 0.5) * kx - 0.5, 0.0, W - 1.0);
            out->Bits()[(size_t)y * w + x] = Sample(src, std::min(sx, W - 1.001), std::min(sy, H - 1.001));
        }
    }
    return out;
}

// Rendered pieces (marks, captions, titles) keyed by everything that changes their pixels.
std::mutex g_cacheMu;
std::unordered_map<std::wstring, BitmapPtr> g_cache;

template <class F>
BitmapPtr Cached(const std::wstring& key, F make) {
    {
        std::lock_guard lock(g_cacheMu);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
    }
    BitmapPtr img = make();
    if (!img) return nullptr;
    std::lock_guard lock(g_cacheMu);
    if (g_cache.size() > 400) g_cache.clear();
    g_cache[key] = img;
    return img;
}

std::wstring Typed(const std::wstring& s, double reveal) {
    if (reveal >= 1) return s;
    return s.substr(0, std::min(s.size(), (size_t)std::ceil(s.size() * reveal)));
}

std::wstring Trimmed(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

double Ease(double x) { return x * x * (3 - 2 * x); }

}  // namespace

void PlaceImage(Bitmap& dst, const Bitmap& img, VRect r, const Motion& mo) {
    if (r.w <= 0 || r.h <= 0 || mo.alpha <= 0.001) return;
    const Bitmap* src = &img;
    BitmapPtr blurred;
    double clipX = img.Width() * (mo.wipe && mo.reveal < 1 ? std::max(0.0, mo.reveal) : 1.0);  // wipe: source columns kept
    double padSrc = 0;
    if (mo.blur > 0.3) {  // blur in: a blurred copy with room to spread
        const double k = img.Width() / r.w;
        const int pad = (int)std::ceil(mo.blur * 3 * k);
        blurred = Blank(img.Width() + 2 * pad, img.Height() + 2 * pad);
        if (blurred) {
            for (int y = 0; y < img.Height(); ++y)
                for (int x = 0; x < img.Width(); ++x)
                    blurred->Bits()[(size_t)(y + pad) * blurred->Width() + x + pad] = x < clipX ? img.Bits()[(size_t)y * img.Width() + x] : 0;
            auto px = BlurArea(*blurred, {0, 0, blurred->Width(), blurred->Height()}, mo.blur * k);
            memcpy(blurred->Bits(), px.data(), px.size() * 4);
            src = blurred.get();
            padSrc = pad;
            clipX = 1e9;
            r = r.Inset(-pad / k, -pad / k);
        }
    }
    // Scale about the center, then move.
    VRect c{r.MidX() - r.w * mo.scale / 2 + mo.dx, r.MidY() - r.h * mo.scale / 2 + mo.dy, r.w * mo.scale, r.h * mo.scale};
    if (c.w < 0.5 || c.h < 0.5) return;
    const uint32_t a = (uint32_t)std::clamp(std::lround(mo.alpha * 255), 0L, 255L);
    const int W = dst.Width(), H = dst.Height();
    const int x0 = std::max(0, (int)std::floor(c.x)), y0 = std::max(0, (int)std::floor(c.y));
    const int x1 = std::min(W, (int)std::ceil(c.MaxX())), y1 = std::min(H, (int)std::ceil(c.MaxY()));
    const int sw = src->Width(), sh = src->Height();
    const bool exact = c.x == std::floor(c.x) && c.y == std::floor(c.y) && std::fabs(c.w - sw) < 1e-6 && std::fabs(c.h - sh) < 1e-6;
    (void)padSrc;
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = dst.Bits() + (size_t)y * W;
        for (int x = x0; x < x1; ++x) {
            uint32_t p;
            if (exact) {
                const int sx = x - (int)c.x, sy = y - (int)c.y;
                if (sx >= clipX) continue;
                p = src->Bits()[(size_t)sy * sw + sx];
            } else {
                const double sx = (x + 0.5 - c.x) * sw / c.w - 0.5, sy = (y + 0.5 - c.y) * sh / c.h - 0.5;
                if (sx >= clipX) continue;
                p = Sample(*src, sx, sy);
            }
            if (a < 255) p = Scale(p, a);
            Over(row[x], p);
        }
    }
}

// ---------- the renderer ----------

FrameRenderer::FrameRenderer(const VideoEdit& edit, SIZE full, bool preview) : edit_(edit), full_(full), preview_(preview) {
    const VRect f{0, 0, (double)full.cx, (double)full.cy};
    VRect v = f;
    if (edit.crop)
        if (auto i = Intersect(*edit.crop, f)) v = i->Integral();
    if (v.w < 16 || v.h < 16) v = f;
    view_ = v;
    out_ = preview ? full : SIZE{(LONG)(std::floor(v.w / 2) * 2), (LONG)(std::floor(v.h / 2) * 2)};  // H.264 wants even sizes
    unit_ = std::max(1.0, full.cy / 720.0);
}

BitmapPtr FrameRenderer::Render(const Bitmap& src, double t) const {
    BitmapPtr img = Copy(src);
    if (!img) return nullptr;
    const VRect extent{0, 0, (double)img->Width(), (double)img->Height()};
    // 1. Blur and pixelate, in the order they were added.
    for (const auto& m : edit_.marks) {
        if ((m.kind != MarkKind::Blur && m.kind != MarkKind::Pixelate) || !m.Active(t)) continue;
        auto ri = Intersect(m.Rect(), extent);
        if (!ri) continue;
        const VRect q = ri->Integral();
        const RECT rc{(LONG)q.x, (LONG)q.y, (LONG)q.MaxX(), (LONG)q.MaxY()};
        if (RectW(rc) < 1 || RectH(rc) < 1) continue;
        const Motion mo = MotionOf(m, t);
        const double k = mo.blur > 0 ? std::max(0.15, 1 - mo.blur / (12 * unit_)) : 1;  // blur in: the effect strengthens
        const double side = std::min(ri->w, ri->h);
        const int lv = std::clamp(m.level, 0, 4);
        std::vector<uint32_t> fx;
        if (m.kind == MarkKind::Blur) {
            static const double pct[] = {0.03, 0.05, 0.08, 0.12, 0.18};
            fx = BlurArea(*img, rc, k * std::max(4.0, side * pct[lv]));
        } else {
            static const double pct[] = {0.04, 0.07, 0.1, 0.14, 0.2};
            const int block = (int)std::lround(std::max(6.0, k * side * pct[lv]));
            auto copy = img->Crop(rc);
            if (!copy) continue;
            copy->Pixelate({0, 0, copy->Width(), copy->Height()}, block);
            fx.assign(copy->Bits(), copy->Bits() + (size_t)copy->Width() * copy->Height());
        }
        const int w = RectW(rc);
        for (int y = rc.top; y < rc.bottom; ++y) {
            uint32_t* row = img->Bits() + (size_t)y * img->Width();
            const uint32_t* f = fx.data() + (size_t)(y - rc.top) * w;
            for (int x = rc.left; x < rc.right; ++x) row[x] = mo.alpha < 1 ? Mix(row[x], f[x - rc.left], std::max(0.0, mo.alpha)) : f[x - rc.left];
        }
    }
    // 2. Markup that sits on the video (moves with zoom).
    for (const auto& m : edit_.marks) {
        if (KindIsRegion(m.kind) || m.kind == MarkKind::Title || !m.Active(t)) continue;
        const Motion mo = MotionOf(m, t);
        if (mo.alpha <= 0.001) continue;
        auto pl = MarkImage(m, mo.wipe ? 1 : mo.reveal);
        if (!pl) continue;
        if (mo.ring)
            if (auto ring = RingImage(pl->rect)) {
                Motion rm;
                rm.alpha = (1 - *mo.ring) * 0.8;
                rm.scale = 1 + *mo.ring * 0.5;
                PlaceImage(*img, *ring->image, ring->rect, rm);
            }
        PlaceImage(*img, *pl->image, pl->rect, mo);
    }
    // 3. The visible area: the crop, or a zoom into it.
    const VRect r = ViewRect(t);
    const SIZE tsize = preview_ ? SIZE{(LONG)view_.w, (LONG)view_.h} : out_;
    BitmapPtr framed;
    if (preview_ && r == view_) framed = nullptr;  // drawn in place
    else framed = SampleRect(*img, r, tsize.cx, tsize.cy);
    // 4. Captions and title cards stay put on screen.
    Bitmap& target = framed ? *framed : *img;
    const double ox = framed ? 0 : view_.x, oy = framed ? 0 : view_.y;
    for (const auto& c : edit_.captions) {
        if (!c.Active(t) || (!preview_ && Trimmed(c.text).empty())) continue;
        const Motion mo = CaptionMotion(c, t);
        if (mo.alpha <= 0.001) continue;
        if (auto pl = CaptionImage(c, tsize, t, mo.reveal)) PlaceImage(target, *pl->image, pl->rect.Offset(ox, oy), mo);
    }
    for (const auto& m : edit_.marks) {
        if (m.kind != MarkKind::Title || !m.Active(t)) continue;
        const Motion mo = MotionOf(m, t);
        if (auto card = TitleImage(m, tsize)) PlaceImage(target, *card, {ox, oy, (double)tsize.cx, (double)tsize.cy}, mo);
    }
    if (!framed) return img;
    if (!preview_) return framed;
    for (int y = 0; y < tsize.cy; ++y)  // preview: the framed output sits in place inside the full frame
        memcpy(img->Bits() + (size_t)(y + (int)view_.y) * img->Width() + (int)view_.x, framed->Bits() + (size_t)y * tsize.cx, (size_t)tsize.cx * 4);
    return img;
}

VRect FrameRenderer::ViewRect(double t) const {
    if (preview_ && !zoomInPreview) return view_;
    const Mark* z = nullptr;
    for (const auto& m : edit_.marks)
        if (m.kind == MarkKind::Zoom && m.Active(t)) {
            z = &m;
            break;
        }
    if (!z) return view_;
    const VRect target = ZoomTarget(z->Rect(), view_);
    const double ramp = std::min(z->snappy ? 0.18 : 0.45, (z->end - z->start) / 3);
    const double p = Ease(std::min(1.0, std::min(t - z->start, z->end - t) / std::max(0.01, ramp)));
    const VRect& v = view_;
    return {v.x + (target.x - v.x) * p, v.y + (target.y - v.y) * p, v.w + (target.w - v.w) * p, v.h + (target.h - v.h) * p};
}

VRect FrameRenderer::ZoomTarget(VRect r, VRect v) {
    const double k = v.w / v.h;
    double w = std::max({r.w, r.h * k, v.w / 6}), h = w / k;
    if (h > v.h) {
        h = v.h;
        w = h * k;
    }
    double x = r.MidX() - w / 2, y = r.MidY() - h / 2;
    x = std::min(std::max(v.x, x), v.MaxX() - w);
    y = std::min(std::max(v.y, y), v.MaxY() - h);
    return {x, y, w, h};
}

Motion FrameRenderer::MotionOf(const Mark& m, double t) const {
    Motion mo = Motion::Between(m.InStyle(), m.OutStyle(), m.start, m.end, t, (int)m.text.size(), (double)full_.cy);
    const double a = t - m.start, u = full_.cy;
    constexpr double kPi = 3.14159265358979323846;
    switch (m.emphasis) {
        case Emphasis::None: break;
        case Emphasis::Pulse: mo.scale *= 1 + 0.045 * std::sin(a * 2 * kPi / 1.2); break;
        case Emphasis::Bounce: mo.dy -= std::fabs(std::sin(a * kPi / 0.55)) * u * 0.018; break;
        case Emphasis::Shake: {
            const double c = std::fmod(a, 2.0);  // a short shake every two seconds
            if (c < 0.45) mo.dx += std::sin(c * 2 * kPi * 9) * (1 - c / 0.45) * u * 0.008;
            break;
        }
        case Emphasis::Ping: mo.ring = std::fmod(a, 1.4) / 1.4; break;
    }
    return mo;
}

Motion FrameRenderer::CaptionMotion(const Caption& c, double t) const {
    const AnimStyle s = edit_.captionStyle == AnimStyle::Auto ? AnimStyle::Fade : edit_.captionStyle;
    return Motion::Between(s, s == AnimStyle::Typewriter ? AnimStyle::Fade : s, c.start, c.end, t, (int)c.text.size(), (double)full_.cy);
}

std::optional<Placed> FrameRenderer::MarkImage(const Mark& m, double reveal) const {
    const double u = unit_;
    const double rv = std::ceil(reveal * 30) / 30;  // a step per frame is plenty, and keeps the cache small
    if (KindIsStroke(m.kind) && rv < 1) return StrokeOn(m, rv);
    const VRect mr = m.Rect();
    std::optional<annot::Shape> shape;
    annot::Shape s;
    s.color = m.color;
    s.level = m.level;
    s.unit = (float)u;
    switch (m.kind) {
        case MarkKind::Arrow:
            s.kind = annot::Kind::Arrow;
            s.ax = (float)m.a.x, s.ay = (float)m.a.y, s.bx = (float)m.b.x, s.by = (float)m.b.y;
            shape = s;
            break;
        case MarkKind::Box:
        case MarkKind::Ellipse:
            s.kind = m.kind == MarkKind::Box ? annot::Kind::Rect : annot::Kind::Ellipse;
            s.ax = (float)mr.x, s.ay = (float)mr.y, s.bx = (float)mr.MaxX(), s.by = (float)mr.MaxY();
            shape = s;
            break;
        case MarkKind::Step:
            s.kind = annot::Kind::Step;
            s.ax = (float)mr.MidX(), s.ay = (float)mr.MidY();
            s.step = m.step;
            shape = s;
            break;
        case MarkKind::Text: {
            const std::wstring shown = m.text.empty() && preview_ ? L"Text" : Typed(m.text, rv);
            if (shown.empty()) return std::nullopt;
            s.kind = annot::Kind::Text;
            s.ax = (float)mr.x, s.ay = (float)mr.y;
            s.text = shown;
            s.wrap = (float)std::max(40.0, mr.w);
            shape = s;
            break;
        }
        default: break;
    }
    if (shape) {
        annot::Shape whole = *shape;
        if (m.kind == MarkKind::Text && !m.text.empty()) whole.text = m.text;  // typewriter: the box of the whole text, so it doesn't drift
        const annot::BoxF bb = annot::Bounds(whole);
        const VRect b = VRect{bb.x - 8 * u, bb.y - 8 * u, bb.w + 16 * u, bb.h + 16 * u}.Integral();
        const std::wstring key = std::format(L"a|{}|{},{},{},{}|{}|{}|{}|{}|{}|{},{},{},{}", (int)m.kind, shape->ax, shape->ay, shape->bx, shape->by,
                                             shape->color, shape->level, shape->text, shape->step, u, b.x, b.y, b.w, b.h);
        auto img = Cached(key, [&] {
            auto bmp = Blank((int)b.w, (int)b.h);
            if (bmp) annot::Draw(*bmp, *shape, (float)b.x, (float)b.y);
            return bmp;
        });
        if (!img) return std::nullopt;
        return Placed{img, b};
    }
    const VRect r = mr.Integral();
    if (r.w < 4 || r.h < 4) return std::nullopt;
    if (m.kind == MarkKind::Emoji) {
        const std::wstring shown = m.text.empty() ? (preview_ ? L"\U0001F642" : L"") : m.text;
        if (shown.empty()) return std::nullopt;
        auto img = Cached(std::format(L"e|{}|{}x{}", shown, r.w, r.h), [&] {
            auto bmp = Blank((int)r.w, (int)r.h);
            if (!bmp) return bmp;
            textdraw::Style st;
            st.family = L"Segoe UI Emoji";
            st.weight = 400;
            st.size = (float)(r.h * 0.82);
            auto ext = textdraw::Measure(shown, st, 100000);
            if (ext.w > r.w) {
                st.size *= (float)(r.w / ext.w);
                ext = textdraw::Measure(shown, st, 100000);
            }
            textdraw::Draw(*bmp, shown, st, 0, (float)(r.h - ext.h) / 2, (float)r.w);
            return bmp;
        });
        if (!img) return std::nullopt;
        return Placed{img, r};
    }
    if (m.kind == MarkKind::Bubble) {
        const std::wstring whole = m.text.empty() && preview_ ? L"Say something" : m.text;
        if (whole.empty()) return std::nullopt;
        const std::wstring shown = m.text.empty() ? whole : Typed(whole, rv);
        const double tail = r.h * 0.32;
        const VRect fullR{r.x, r.y, r.w, std::ceil(r.h + tail)};
        auto img = Cached(std::format(L"b|{}|{}|{}x{}|{}|{}|{}", shown, whole, r.w, r.h, m.color, m.level, u), [&] {
            auto bmp = Blank((int)fullR.w, (int)fullR.h);
            if (!bmp) return bmp;
            const VRect body{2, 2, r.w - 4, r.h - 4};
            const COLORREF fill = annot::Color(m.color);
            const double rad = std::min(body.h / 2.5, 28 * u);
            textdraw::FillBubble(*bmp, (float)body.x, (float)body.y, (float)body.w, (float)body.h, (float)rad, (float)tail, fill);
            textdraw::DropShadow(*bmp, (float)(6 * u), 0, (float)(2 * u), 0.35f);
            textdraw::Style st;
            st.weight = 600;
            st.size = (float)(annot::TextPx(m.level) * u * 0.8);
            st.color = annot::IsDark(fill) ? RGB(255, 255, 255) : RGB(0, 0, 0);
            const VRect inner = body.Inset(body.h * 0.28, body.h * 0.14);
            // Sized by the whole text, so typing it out doesn't change the layout.
            auto tb = textdraw::Measure(whole, st, (float)inner.w);
            while (tb.h > inner.h && st.size > 8) {
                st.size *= 0.9f;
                tb = textdraw::Measure(whole, st, (float)inner.w);
            }
            textdraw::Draw(*bmp, shown, st, (float)inner.x, (float)(inner.MidY() - tb.h / 2), (float)inner.w);
            return bmp;
        });
        if (!img) return std::nullopt;
        return Placed{img, fullR};
    }
    return std::nullopt;
}

// Arrow, box or ellipse drawn partway along its outline.
std::optional<Placed> FrameRenderer::StrokeOn(const Mark& m, double p) const {
    const double u = unit_;
    if (m.kind == MarkKind::Arrow) {  // the arrow grows from its tail, head first
        Mark g = m;
        const double k = std::max(0.08, p);
        g.b = {m.a.x + (m.b.x - m.a.x) * k, m.a.y + (m.b.y - m.a.y) * k};
        return MarkImage(g, 1);
    }
    const VRect mr = m.Rect();
    annot::Shape s;
    s.kind = m.kind == MarkKind::Box ? annot::Kind::Rect : annot::Kind::Ellipse;
    s.color = m.color;
    s.level = m.level;
    s.unit = (float)u;
    s.ax = (float)mr.x, s.ay = (float)mr.y, s.bx = (float)mr.MaxX(), s.by = (float)mr.MaxY();
    const annot::BoxF bb = annot::Bounds(s);
    const VRect b = VRect{bb.x - 8 * u, bb.y - 8 * u, bb.w + 16 * u, bb.h + 16 * u}.Integral();
    auto img = Cached(std::format(L"s|{}|{},{},{},{}|{}|{}|{}|{}", (int)m.kind, mr.x, mr.y, mr.w, mr.h, m.color, m.level, p, u), [&] {
        auto bmp = Blank((int)b.w, (int)b.h);
        if (bmp) annot::DrawPartialOutline(*bmp, s, (float)p, (float)b.x, (float)b.y);
        return bmp;
    });
    if (!img) return std::nullopt;
    return Placed{img, b};
}

// Ping: a ring around the item that spreads and fades.
std::optional<Placed> FrameRenderer::RingImage(VRect r) const {
    const double pad = std::max(6.0, std::min(r.w, r.h) * 0.12);
    const VRect box = r.Inset(-pad, -pad).Integral();
    const double u = unit_;
    auto img = Cached(std::format(L"r|{}x{}|{}", box.w, box.h, u), [&] {
        auto bmp = Blank((int)box.w, (int)box.h);
        if (bmp)
            textdraw::StrokeEllipse(*bmp, (float)(3 * u), (float)(3 * u), (float)(box.w - 6 * u), (float)(box.h - 6 * u), (float)std::max(2.0, 2.5 * u),
                                    RGB(255, 255, 255));
        return bmp;
    });
    if (!img) return std::nullopt;
    return Placed{img, box};
}

std::optional<Placed> FrameRenderer::CaptionImage(const Caption& c, SIZE size, std::optional<double> t, double reveal) const {
    const std::wstring whole = c.text.empty() ? L"Type a caption…" : c.text;
    const CaptionLook look = edit_.captionLook;
    const COLORREF textColor = annot::Color(edit_.captionColor), edgeColor = annot::Color(edit_.captionEdge);
    const COLORREF yellow = RGB(255, 214, 10);
    const COLORREF highlight = edit_.captionColor == 2 ? (annot::IsDark(edgeColor) ? RGB(255, 255, 255) : RGB(0, 0, 0)) : yellow;  // stands out from yellow text too
    const double W = size.cx, H = size.cy;
    const double fontSize = std::max(14.0, H * VideoEdit::kCaptionScale[std::clamp(edit_.captionSize, 0, 4)]) * (look == CaptionLook::Outline ? 1.25 : 1);
    const double maxW = W * (look == CaptionLook::Bar ? 0.92 : 0.86);
    // The spoken word lights up, while the caption still matches its transcription.
    std::optional<std::pair<size_t, size_t>> hot;
    if (edit_.highlightWords && t && !c.words.empty()) {
        std::wstring joined;
        for (size_t i = 0; i < c.words.size(); ++i) joined += (i ? L" " : L"") + c.words[i].text;
        if (joined == c.text) {
            size_t lo = 0;
            for (const auto& w : c.words) {
                if (*t >= w.start && *t < w.end) {
                    hot = std::make_pair(lo, w.text.size());
                    break;
                }
                lo += w.text.size() + 1;
            }
        }
    }
    const std::wstring shown = Typed(whole, std::ceil(reveal * 30) / 30);
    textdraw::Style st;
    st.weight = look == CaptionLook::Outline ? 900 : 600;
    st.size = (float)fontSize;
    st.color = textColor;
    st.alpha = c.text.empty() ? 0.5f : 1;
    if (look == CaptionLook::Outline) {  // text with an edge in the edge color, no box
        st.edge = edgeColor;
        st.edgeWidth = (float)std::max(1.0, fontSize * 0.03);
    }
    if (hot && hot->first + hot->second <= shown.size()) st.ranges.push_back({hot->first, hot->second, highlight});
    textdraw::Style measure = st;
    measure.ranges.clear();
    const auto tb = textdraw::Measure(whole, measure, (float)maxW);
    const double tbw = std::ceil(tb.w), tbh = std::ceil(tb.h);
    const double pad = fontSize * 0.45;
    double w = tbw + pad * 2, h = tbh + pad * 1.2;
    if (look == CaptionLook::Bar) w = W;
    const double margin = look == CaptionLook::Bar ? 0 : H * 0.06;
    double y = c.position == CaptionPosition::Top ? margin : c.position == CaptionPosition::Middle ? (H - h) / 2 : H - margin - h;
    double x = (W - w) / 2;
    if (c.center) {  // dragged: kept fully on screen
        x = look == CaptionLook::Bar ? 0 : std::min(std::max(0.0, c.center->x * W - w / 2), W - w);
        y = std::min(std::max(0.0, c.center->y * H - h / 2), H - h);
    }
    const VRect r{std::round(x), std::round(y), std::ceil(w), std::ceil(h)};
    const std::wstring key = std::format(L"c|{}|{}|{}x{}|{}|{}|{}|{}|{}|{}|{}", shown, whole, r.w, r.h, fontSize, (int)look,
                                         hot ? (int)hot->first : -1, hot ? (int)hot->second : 0, edit_.captionColor, edit_.captionEdge, c.text.empty());
    auto img = Cached(key, [&] {
        auto bmp = Blank((int)r.w, (int)r.h);
        if (!bmp) return bmp;
        if (look != CaptionLook::Outline)
            textdraw::FillRounded(*bmp, 0, 0, (float)r.w, (float)r.h, look == CaptionLook::Bar ? 0.f : (float)(fontSize * 0.35), edgeColor,
                                  look == CaptionLook::Bar ? 0.7f : 0.62f);
        textdraw::Draw(*bmp, shown, st, (float)((r.w - tbw) / 2), (float)(pad * 0.6), (float)tbw + 1);
        if (look == CaptionLook::Outline) textdraw::DropShadow(*bmp, (float)(fontSize * 0.1), 0, (float)(fontSize * 0.03), 0.45f);
        return bmp;
    });
    if (!img) return std::nullopt;
    return Placed{img, r};
}

BitmapPtr FrameRenderer::TitleImage(const Mark& m, SIZE size) const {
    const std::wstring title = m.text.empty() && preview_ ? L"Title" : m.text;
    return Cached(std::format(L"t|{}|{}|{}|{}x{}", title, m.subtitle, m.color, size.cx, size.cy), [&] {
        auto bmp = Bitmap::Create(size.cx, size.cy);
        if (!bmp) return bmp;
        const COLORREF bg = annot::Color(m.color);
        std::fill_n(bmp->Bits(), (size_t)size.cx * size.cy, 0xFF000000u | (uint32_t)GetRValue(bg) << 16 | (uint32_t)GetGValue(bg) << 8 | GetBValue(bg));
        const COLORREF fg = annot::IsDark(bg) ? RGB(255, 255, 255) : RGB(0, 0, 0);
        const float W = (float)size.cx, H = (float)size.cy, w = W * 0.8f;
        textdraw::Style ts;
        ts.weight = 700;
        ts.size = H * 0.085f;
        ts.color = fg;
        textdraw::Style ss;
        ss.weight = 500;
        ss.size = H * 0.04f;
        ss.color = fg;
        ss.alpha = 0.7f;
        const auto tb = textdraw::Measure(title, ts, w);
        const auto sb = m.subtitle.empty() ? textdraw::Extent{} : textdraw::Measure(m.subtitle, ss, w);
        const float gap = m.subtitle.empty() ? 0 : H * 0.03f;
        const float top = (H - tb.h - gap - sb.h) / 2;
        textdraw::Draw(*bmp, title, ts, (W - w) / 2, top, w);
        if (!m.subtitle.empty()) textdraw::Draw(*bmp, m.subtitle, ss, (W - w) / 2, top + tb.h + gap, w);
        return bmp;
    });
}

// ---------- tests (MarkupTests.swift, VideoTests.testCaptionChunking) ----------

namespace {

struct Rgbf {
    double r, g, b;
    double Brightness() const { return std::max({r, g, b}); }
};

Rgbf Px(const Bitmap& b, int x, int y) {
    const uint32_t p = b.Bits()[(size_t)y * b.Width() + x];
    return {((p >> 16) & 255) / 255.0, ((p >> 8) & 255) / 255.0, (p & 255) / 255.0};
}

// Left half red, right half blue, a white bar at x 300–340.
BitmapPtr TestFrame() {
    auto b = Bitmap::Create(640, 360);
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x)
            b->Bits()[(size_t)y * 640 + x] = x >= 300 && x < 340 ? 0xFFFFFFFFu : x < 320 ? 0xFFFF0000u : 0xFF0000FFu;
    return b;
}

constexpr SIZE kSize{640, 360};

VideoEdit EditWith(std::vector<Mark> marks) {
    VideoEdit e;
    e.trimEnd = 10;
    e.marks = std::move(marks);
    return e;
}

Mark MakeMark(MarkKind k, VPoint a, VPoint b, int color = 0, std::wstring text = L"", AnimStyle anim = AnimStyle::None) {
    Mark m;
    m.kind = k;
    m.start = 1;
    m.end = 3;
    m.a = a;
    m.b = b;
    m.text = std::move(text);
    m.color = color;
    m.style = anim;
    return m;
}

BitmapPtr RenderAt(const VideoEdit& e, double t, bool preview = false, bool zoom = false) {
    static BitmapPtr frame = TestFrame();
    FrameRenderer r(e, kSize, preview);
    r.zoomInPreview = zoom;
    return r.Render(*frame, t);
}

int Lit(const Bitmap& b) {
    int n = 0;
    for (size_t i = 0, c = (size_t)b.Width() * b.Height(); i < c; ++i) n += (b.Bits()[i] >> 24) > 128;
    return n;
}

}  // namespace

ATHER_TEST(video_blur_only_while_active) {
    const auto e = EditWith({MakeMark(MarkKind::Blur, {260, 0}, {380, 360})});
    CHECK(Px(*RenderAt(e, 0.5), 296, 180).g < 0.05);  // before: pure red
    CHECK(Px(*RenderAt(e, 2), 296, 180).g > 0.15);    // during: red mixed with the white bar
    CHECK(Px(*RenderAt(e, 3.5), 296, 180).g < 0.05);  // after
}

ATHER_TEST(video_arrow_emoji_and_title) {
    const auto arrow = MakeMark(MarkKind::Arrow, {60, 300}, {220, 140}, 6);  // white on red
    CHECK(Px(*RenderAt(EditWith({arrow}), 2), 140, 220).g > 0.8);

    const auto emoji = MakeMark(MarkKind::Emoji, {480, 40}, {600, 160}, 0, L"✅");  // green box on blue
    auto img = RenderAt(EditWith({emoji}), 2);
    int greens = 0;
    for (int x = 490; x < 590; x += 5)
        for (int y = 50; y < 150; y += 5) {
            const Rgbf c = Px(*img, x, y);
            greens += c.g > 0.5 && c.b < 0.6;
        }
    CHECK(greens > 20);

    const auto title = MakeMark(MarkKind::Title, {0, 0}, {640, 360}, 7, L"Release 1.2");
    auto t = RenderAt(EditWith({title}), 2);
    CHECK(Px(*t, 10, 10).Brightness() < 0.15);  // the card covers the frame
    CHECK(Px(*t, 630, 350).Brightness() < 0.15);
}

ATHER_TEST(video_zoom_in_export_and_only_while_playing_in_preview) {
    const auto z = MakeMark(MarkKind::Zoom, {500, 150}, {560, 210});
    const auto e = EditWith({z});
    CHECK(Px(*RenderAt(e, 0.5), 40, 180).r > 0.9);  // not zoomed yet: red on the left
    CHECK(Px(*RenderAt(e, 2), 40, 180).b > 0.9);    // zoomed into the blue area
    CHECK(Px(*RenderAt(e, 2, true), 40, 180).b < 0.1);  // paused preview: not zoomed
    CHECK(Px(*RenderAt(e, 2, true, true), 40, 180).b > 0.9);
    const VRect zt = FrameRenderer::ZoomTarget(z.Rect(), {0, 0, 640, 360});
    CHECK_NEAR(zt.w / zt.h, 640.0 / 360, 0.01);
}

ATHER_TEST(video_animation_styles) {
    FrameRenderer r(EditWith({}), kSize, false);
    auto mo = [&](AnimStyle st, double t, MarkKind kind = MarkKind::Box, std::optional<AnimStyle> exit = std::nullopt, Emphasis em = Emphasis::None) {
        Mark m = MakeMark(kind, {0, 0}, {10, 10}, 0, L"Hello world", st);
        m.exit = exit;
        m.emphasis = em;
        return r.MotionOf(m, t);
    };
    CHECK_NEAR(mo(AnimStyle::Fade, 1).alpha, 0, 0.01);
    CHECK_NEAR(mo(AnimStyle::Fade, 2).alpha, 1, 0.01);
    CHECK(mo(AnimStyle::Fade, 2.95).alpha < 0.5);  // the same style plays it out
    CHECK(mo(AnimStyle::Pop, 1.02).scale < 0.8);
    CHECK_NEAR(mo(AnimStyle::Pop, 1.5).scale, 1, 0.01);
    CHECK(mo(AnimStyle::Slide, 1.05).dy > 5);  // comes up from below
    CHECK(mo(AnimStyle::DrawOn, 1.1).reveal < 0.5);
    CHECK_EQ(mo(AnimStyle::DrawOn, 2.95).reveal, 1.0);  // draw on leaves with a fade instead
    CHECK(mo(AnimStyle::DrawOn, 2.95).alpha < 1);
    CHECK(mo(AnimStyle::Wipe, 1.1).wipe);
    CHECK(mo(AnimStyle::BlurIn, 1.05).blur > 1);
    CHECK_EQ(mo(AnimStyle::Auto, 1.05, MarkKind::Arrow).reveal, mo(AnimStyle::DrawOn, 1.05, MarkKind::Arrow).reveal);  // auto: the kind's default
    // Advanced: a different exit.
    CHECK_EQ(mo(AnimStyle::Pop, 2.95, MarkKind::Box, AnimStyle::None).alpha, 1.0);
    CHECK_NEAR(mo(AnimStyle::None, 2.95, MarkKind::Box, AnimStyle::Fade).alpha, mo(AnimStyle::Fade, 2.95).alpha, 0.01);
    // While on screen.
    CHECK(mo(AnimStyle::None, 2.3, MarkKind::Box, std::nullopt, Emphasis::Pulse).scale != 1);
    CHECK(mo(AnimStyle::None, 2, MarkKind::Box, std::nullopt, Emphasis::Ping).ring.has_value());
    CHECK(mo(AnimStyle::None, 2.1, MarkKind::Box, std::nullopt, Emphasis::Bounce).dy != 0);
}

ATHER_TEST(video_draw_on_and_typewriter_render_partway) {
    FrameRenderer r(EditWith({}), kSize, false);
    const auto box = MakeMark(MarkKind::Box, {100, 100}, {300, 250}, 6);
    auto full = r.MarkImage(box), half = r.MarkImage(box, 0.3);
    CHECK(full && half);
    if (!full || !half) return;
    CHECK(Lit(*half->image) < Lit(*full->image) / 2);
    const auto text = MakeMark(MarkKind::Text, {50, 50}, {400, 100}, 6, L"Typewriter text here");
    auto partial = r.MarkImage(text, 0.25), whole = r.MarkImage(text);
    CHECK(partial && whole);
    if (!partial || !whole) return;
    CHECK(Lit(*partial->image) < Lit(*whole->image));
    CHECK(partial->rect == whole->rect);  // same box while typing, so it doesn't drift
}

ATHER_TEST(video_caption_looks_and_word_highlight) {
    VideoEdit e = EditWith({});
    Caption c;
    c.start = 1;
    c.end = 3;
    c.text = L"Click Deploy";
    c.words = {{1, 1.5, L"Click"}, {1.5, 3, L"Deploy"}};
    e.captions = {c};
    auto pill = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize, 1.2);
    CHECK(pill && pill->rect.w < kSize.cx * 0.6);
    e.captionLook = CaptionLook::Bar;
    auto bar = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize, 1.2);
    CHECK(bar && bar->rect.w == kSize.cx);
    // The spoken word turns yellow: count yellow pixels in the first and second half of the caption.
    e.captionLook = CaptionLook::Pill;
    FrameRenderer rr(e, kSize, false);
    auto yellowSide = [&](double t) {
        auto img = rr.CaptionImage(e.captions[0], kSize, t)->image;
        int l = 0, rt = 0;
        for (int x = 0; x < img->Width(); x += 2)
            for (int y = 0; y < img->Height(); y += 2) {
                const Rgbf p = Px(*img, x, y);
                if (p.r > 0.8 && p.g > 0.6 && p.b < 0.3) (x < img->Width() / 2 ? l : rt)++;
            }
        return std::make_pair(l, rt);
    };
    const auto early = yellowSide(1.2), late = yellowSide(2);
    CHECK(early.first > early.second);  // "Click" lit
    CHECK(late.second > late.first);    // then "Deploy"
    // Edited text no longer matches the words: no highlight, still drawn.
    e.captions[0].text = L"Press Deploy";
    auto edited = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize, 1.2);
    CHECK(edited.has_value());
}

ATHER_TEST(video_crop_applies_to_markup_and_captions) {
    VideoEdit e = EditWith({MakeMark(MarkKind::Title, {0, 0}, {640, 360}, 6, L"Hi")});
    e.crop = VRect{320, 0, 320, 360};
    auto img = RenderAt(e, 2);
    CHECK_EQ(img->Width(), 320);
    CHECK(Px(*img, 5, 5).Brightness() > 0.9);  // white card fills the cropped frame
}

ATHER_TEST(video_caption_drag_position_and_colors) {
    VideoEdit e = EditWith({});
    Caption c;
    c.start = 1;
    c.end = 3;
    c.text = L"Moved";
    e.captions = {c};
    auto base = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize)->rect;
    CHECK(base.y > kSize.cy * 0.7);  // bottom by default
    e.captions[0].center = VPoint{0.25, 0.2};
    auto moved = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize)->rect;
    CHECK_NEAR(moved.MidX(), kSize.cx * 0.25, 2);
    CHECK_NEAR(moved.MidY(), kSize.cy * 0.2, 2);
    e.captions[0].center = VPoint{1, 1};  // kept on screen
    auto edge = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize)->rect;
    CHECK(edge.MaxX() <= kSize.cx + 1);
    CHECK(edge.MaxY() <= kSize.cy + 1);
    // Text and box colors: a red box behind blue text.
    e.captions[0].center.reset();
    e.captionEdge = 0;
    e.captionColor = 4;
    auto img = FrameRenderer(e, kSize, false).CaptionImage(e.captions[0], kSize)->image;
    int reds = 0, blues = 0;
    for (int x = 0; x < img->Width(); x += 2)
        for (int y = 0; y < img->Height(); y += 2) {
            const Rgbf p = Px(*img, x, y);
            reds += p.r > 0.5 && p.b < 0.3;
            blues += p.b > 0.6 && p.r < 0.3;
        }
    CHECK(reds > 20);
    CHECK(blues > 5);
}

ATHER_TEST(video_captions_appear_and_disappear_on_time) {
    VideoEdit e = EditWith({});
    Caption c;
    c.start = 1;
    c.end = 2;
    c.text = L"Hello";
    e.captions = {c};
    auto dark = [&](double t) {
        auto img = RenderAt(e, t);
        int n = 0;
        for (int y = 290; y < 345; y += 3)
            for (int x = 270; x < 370; x += 3) n += Px(*img, x, y).Brightness() < 0.6;
        return n;
    };
    CHECK_EQ(dark(0.5), 0);
    CHECK(dark(1.5) > 10);
    CHECK_EQ(dark(2.5), 0);
}

ATHER_TEST(video_caption_chunking) {
    const std::vector<CaptionWord> words = {{0, 0.3, L"Open"}, {0.35, 0.6, L"the"}, {0.65, 1.0, L"settings"},
                                            {2.5, 2.8, L"Then"}, {2.85, 3.2, L"click"}, {3.25, 3.6, L"save."}};
    const auto caps = ChunkCaptions(words);
    CHECK_EQ(caps.size(), 2u);
    if (caps.size() != 2) return;
    CHECK(caps[0].text == L"Open the settings");
    CHECK(caps[1].text == L"Then click save.");
    CHECK_EQ(caps[0].start, 0.0);
    CHECK(caps[0].end <= caps[1].start);
    CHECK_EQ(caps[0].words.size(), 3u);
    CHECK_EQ(caps[0].words.back().end, caps[0].end);  // the last word stays lit
}

}  // namespace ather
