#include "videoedit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <emmintrin.h>
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

double ClipsDuration(const std::vector<Clip>& clips) {
    double d = 0;
    for (const auto& c : clips) d += c.Duration();
    return d;
}

double ClipStart(const std::vector<Clip>& clips, size_t i) {
    double d = 0;
    for (size_t k = 0; k < i && k < clips.size(); ++k) d += clips[k].Duration();
    return d;
}

std::optional<std::pair<size_t, double>> LocateClip(const std::vector<Clip>& clips, double t) {
    if (clips.empty()) return std::nullopt;
    double at = 0;
    for (size_t i = 0; i < clips.size(); ++i) {
        const double d = clips[i].Duration();
        if (t < at + d || i + 1 == clips.size()) return std::make_pair(i, clips[i].in + std::clamp(t - at, 0.0, d));
        at += d;
    }
    return std::nullopt;
}

void ApplyClips(VideoEdit& e, std::vector<Clip> clips) {
    const std::vector<Clip> before = e.clips;
    const double oldTotal = ClipsDuration(before), newTotal = ClipsDuration(clips);
    // Where an old timeline time lands now: the same moment of the same footage, if that clip and moment are kept.
    // `side` says which way it fell off when that moment was cut: -1 before the kept part, 1 after it.
    struct Landing {
        std::optional<double> t;
        int side = 0;
        uint64_t clip = 0;
    };
    auto land = [&](double t) {
        Landing l;
        const auto spot = LocateClip(before, t);
        if (!spot) return l;
        const Clip& oc = before[spot->first];
        const double src = spot->second;
        // The footage may now be in another piece of the same video (a split): any piece that shows this moment
        // will do, the same clip first. Not another copy of the file added separately. Otherwise the same clip,
        // where the moment was cut off.
        std::optional<size_t> found;
        for (size_t i = 0; i < clips.size(); ++i) {
            const Clip& nc = clips[i];
            if (nc.source != oc.source || _wcsicmp(nc.path.c_str(), oc.path.c_str()) != 0 || src < nc.in - 1e-6 || src > nc.out + 1e-6) continue;
            if (!found || nc.id == oc.id) found = i;
        }
        for (size_t i = 0; i < clips.size() && !found; ++i)
            if (clips[i].id == oc.id) found = i;
        if (!found) return l;
        const Clip& nc = clips[*found];
        l.clip = nc.id;
        l.side = src < nc.in - 1e-6 ? -1 : src > nc.out + 1e-6 ? 1 : 0;
        l.t = ClipStart(clips, *found) + std::clamp(src - nc.in, 0.0, nc.Duration());
        return l;
    };
    // An item keeps its length; it goes when its clip is gone or both its ends were cut off on the same side.
    auto move = [&](double& start, double& end) {
        const Landing a = land(start), b = land(std::max(start, end - 1e-6));
        if (!a.t) return false;
        if (a.side != 0 && b.clip == a.clip && b.side == a.side) return false;
        const double len = end - start;
        start = std::min(*a.t, std::max(0.0, newTotal - 0.1));
        end = std::min(newTotal, start + len);
        return end > start;
    };
    for (auto it = e.marks.begin(); it != e.marks.end();) it = move(it->start, it->end) ? it + 1 : e.marks.erase(it);
    for (auto it = e.captions.begin(); it != e.captions.end();) {
        const double s0 = it->start;
        if (!move(it->start, it->end)) {
            it = e.captions.erase(it);
            continue;
        }
        for (auto& w : it->words) w.start += it->start - s0, w.end += it->start - s0;
        ++it;
    }
    std::stable_sort(e.captions.begin(), e.captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    // The trim: an untouched end stays at the end; otherwise both ends follow their footage.
    const bool wholeStart = e.trimStart <= 1e-6, wholeEnd = e.trimEnd >= oldTotal - 1e-6;
    const auto ts = land(e.trimStart), te = land(std::max(0.0, e.trimEnd - 1e-6));
    e.trimStart = wholeStart || !ts.t ? 0 : *ts.t;
    e.trimEnd = wholeEnd || !te.t ? newTotal : std::min(newTotal, *te.t + (te.side ? 0 : 1e-6));  // looked up just before itself
    if (e.trimEnd - e.trimStart < 0.1) e.trimStart = 0, e.trimEnd = newTotal;
    e.clips = std::move(clips);
}

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

// Scale and Over for four pixels at once, with the same integer results: x / 255 for x ≤ 255 × 255 + 127 is
// (x + 1 + (x >> 8)) >> 8, and the final add is on whole pixels, as Over's is.
inline __m128i Div255(__m128i v) { return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(v, _mm_set1_epi16(1)), _mm_srli_epi16(v, 8)), 8); }

inline __m128i ScaleFour(__m128i p, __m128i klo, __m128i khi) {  // channels × k / 255; k per channel, 16-bit
    const __m128i zero = _mm_setzero_si128(), r = _mm_set1_epi16(127);
    const __m128i lo = Div255(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(p, zero), klo), r));
    const __m128i hi = Div255(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(p, zero), khi), r));
    return _mm_packus_epi16(lo, hi);
}

inline void OverFour(uint32_t* d, const uint32_t* s, uint32_t a) {  // Over(d[i], Scale(s[i], a)) for i < 4
    __m128i p = _mm_loadu_si128((const __m128i*)s);
    if (a < 255) p = ScaleFour(p, _mm_set1_epi16((short)a), _mm_set1_epi16((short)a));
    const __m128i dd = _mm_loadu_si128((const __m128i*)d);
    const __m128i sa = _mm_srli_epi32(p, 24);
    const __m128i k16 = _mm_packs_epi32(_mm_sub_epi32(_mm_set1_epi32(255), sa), _mm_setzero_si128());  // 255 − sa, per pixel
    const __m128i kk = _mm_unpacklo_epi16(k16, k16);  // k0 k0 k1 k1 k2 k2 k3 k3
    const __m128i res = _mm_add_epi32(p, ScaleFour(dd, _mm_unpacklo_epi32(kk, kk), _mm_unpackhi_epi32(kk, kk)));
    const __m128i none = _mm_cmpeq_epi32(sa, _mm_setzero_si128());  // fully transparent: left as it was
    _mm_storeu_si128((__m128i*)d, _mm_or_si128(_mm_and_si128(none, dd), _mm_andnot_si128(none, res)));
}

inline uint32_t Mix(uint32_t a, uint32_t b, double t) {  // a·(1−t) + b·t
    const uint32_t k = (uint32_t)std::clamp(std::lround(t * 255), 0L, 255L);
    return Scale(a, 255 - k) + Scale(b, k);
}

BitmapPtr Copy(const Bitmap& src) {
    auto b = Bitmap::CreateRecycled(src.Width(), src.Height());
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
// Each pass is a box along the rows, then one down the columns. The four channels go side by side (an SSE lane
// each, each lane doing exactly the float arithmetic of that channel alone). Rows stream through the six passes,
// each column pass keeping just the rows its window spans, so the work stays in the cache.
std::vector<uint32_t> BlurArea(const Bitmap& img, const RECT& inner, double sigma) {
    const int r = std::max(1, (int)std::lround(sigma));
    const int W = img.Width(), H = img.Height();
    const RECT P{std::max(0L, inner.left - 3 * r), std::max(0L, inner.top - 3 * r), std::min((LONG)W, inner.right + 3 * r),
                 std::min((LONG)H, inner.bottom + 3 * r)};
    const int pw = RectW(P), ph = RectH(P), iw = RectW(inner), ih = RectH(inner);
    std::vector<uint32_t> out((size_t)iw * ih, 0);
    const __m128i zero = _mm_setzero_si128();
    const bool pass = pw > 1 && ph > 1;
    const int rh = std::min(r, pw - 1), rv = std::min(r, ph - 1), R = 2 * rv + 2;  // R: rows a column window spans
    // Kept per thread between calls (fresh memory costs a page fault per 4 KB), unless it grew big.
    thread_local std::vector<__m128> mem;
    mem.resize((size_t)pw * (3 * (R + 2) + 1));
    __m128* scratch = mem.data();
    struct Stage {
        __m128 *ring, *sum, *out;
        int fetched = -1;
    } stages[3];
    for (int s = 0; s < 3; ++s) {
        __m128* base = mem.data() + (size_t)pw * (1 + s * (R + 2));
        stages[s] = {base, base + (size_t)pw * R, base + (size_t)pw * (R + 1)};
    }
    auto load = [&](int y, __m128* d) {  // row y of the area, as floats
        const uint32_t* row = img.Bits() + (size_t)(P.top + y) * W + P.left;
        for (int x = 0; x < pw; ++x) d[x] = _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)row[x]), zero), zero));
    };
    const __m128 invH = _mm_set1_ps(1.f / (2 * rh + 1)), firstH = _mm_set1_ps((float)(rh + 1));
    const __m128 invV = _mm_set1_ps(1.f / (2 * rv + 1)), firstV = _mm_set1_ps((float)(rv + 1));
    auto along = [&](const __m128* d, __m128* o) {  // a box along one row (the clamps only near its ends)
        __m128 sum = _mm_mul_ps(d[0], firstH);
        for (int i = 1; i <= rh; ++i) sum = _mm_add_ps(sum, d[i]);
        const int mid0 = std::min(rh, pw), mid1 = std::max(mid0, pw - rh - 1);  // i − rh ≥ 0 and i + rh + 1 ≤ pw − 1 in between
        int i = 0;
        for (; i < mid0; ++i) {
            o[i] = _mm_mul_ps(sum, invH);
            sum = _mm_add_ps(sum, _mm_sub_ps(d[std::min(i + rh + 1, pw - 1)], d[std::max(i - rh, 0)]));
        }
        for (; i < mid1; ++i) {
            o[i] = _mm_mul_ps(sum, invH);
            sum = _mm_add_ps(sum, _mm_sub_ps(d[i + rh + 1], d[i - rh]));
        }
        for (; i < pw; ++i) {
            o[i] = _mm_mul_ps(sum, invH);
            sum = _mm_add_ps(sum, _mm_sub_ps(d[std::min(i + rh + 1, pw - 1)], d[std::max(i - rh, 0)]));
        }
    };
    // Row i (asked for in order from 0) after pass s: the box down the columns over rows that had the box along
    // them, fed by pass s − 1 (or the image).
    auto passRow = [&](auto& self, int s, int i) -> const __m128* {
        Stage& st = stages[s];
        auto in = [&](int j) { return st.ring + (size_t)(j % R) * pw; };
        auto fetch = [&](int j) {
            while (st.fetched < j) {
                const int y = ++st.fetched;
                if (s == 0) {
                    load(y, scratch);
                    along(scratch, in(y));
                } else {
                    along(self(self, s - 1, y), in(y));
                }
            }
        };
        if (i == 0) {
            fetch(rv);
            const __m128* r0 = in(0);
            for (int x = 0; x < pw; ++x) st.sum[x] = _mm_mul_ps(r0[x], firstV);
            for (int k = 1; k <= rv; ++k) {
                const __m128* rk = in(k);
                for (int x = 0; x < pw; ++x) st.sum[x] = _mm_add_ps(st.sum[x], rk[x]);
            }
        }
        for (int x = 0; x < pw; ++x) st.out[x] = _mm_mul_ps(st.sum[x], invV);
        const int a = std::min(i + rv + 1, ph - 1), b = std::max(i - rv, 0);
        fetch(a);
        const __m128 *add = in(a), *sub = in(b);
        for (int x = 0; x < pw; ++x) st.sum[x] = _mm_add_ps(st.sum[x], _mm_sub_ps(add[x], sub[x]));
        return st.out;
    };
    // Rounded like lround (halves up; anything below zero is 0 after the clamp), clamped to 0…255.
    const __m128 half = _mm_set1_ps(0.5f), one = _mm_set1_ps(1.f), fzero = _mm_setzero_ps();
    const int top = inner.top - P.top, left = inner.left - P.left;
    for (int y = 0; y < top + ih; ++y) {
        const __m128* row;
        if (pass) {
            row = passRow(passRow, 2, y);
        } else {
            load(y, scratch);
            row = scratch;
        }
        if (y < top) continue;
        const __m128* src = row + left;
        uint32_t* dst = out.data() + (size_t)(y - top) * iw;
        for (int x = 0; x < iw; ++x) {
            const __m128 v = _mm_max_ps(src[x], fzero);
            const __m128 tr = _mm_cvtepi32_ps(_mm_cvttps_epi32(v));
            const __m128 rounded = _mm_add_ps(tr, _mm_and_ps(_mm_cmpge_ps(_mm_sub_ps(v, tr), half), one));
            const __m128i q = _mm_cvttps_epi32(rounded);
            dst[x] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(q, zero), zero));
        }
    }
    if (mem.capacity() > (1u << 19)) std::vector<__m128>().swap(mem);  // 8 MB
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
    auto out = Bitmap::CreateRecycled(w, h);
    if (!out) return nullptr;
    const int W = src.Width(), H = src.Height();
    if (std::fabs(r.w - w) <= 1.01 && std::fabs(r.h - h) <= 1.01 && r.x == std::floor(r.x) && r.y == std::floor(r.y)) {  // a plain crop
        const int x0 = (int)r.x;
        for (int y = 0; y < h; ++y) {
            const int sy = std::clamp((int)r.y + y, 0, H - 1);
            uint32_t* d = out->Bits() + (size_t)y * w;
            const uint32_t* s = src.Bits() + (size_t)sy * W;
            if (x0 >= 0 && x0 + w <= W) memcpy(d, s + x0, (size_t)w * 4);
            else
                for (int x = 0; x < w; ++x) d[x] = s[std::clamp(x0 + x, 0, W - 1)];
        }
        return out;
    }
    // Bilinear, pixel for pixel as Sample does it (the same double arithmetic, so the same result), with the
    // source positions worked out once per column and row and two channels at a time.
    const double kx = r.w / w, ky = r.h / h;
    std::vector<int> xs((size_t)w);
    std::vector<double> fxs((size_t)w), gxs((size_t)w);
    for (int x = 0; x < w; ++x) {
        const double sx = std::min(std::clamp(r.x + (x + 0.5) * kx - 0.5, 0.0, W - 1.0), W - 1.001);
        xs[x] = (int)std::floor(sx);
        fxs[x] = sx - xs[x];
        gxs[x] = 1 - fxs[x];
    }
    const __m128i zero = _mm_setzero_si128();
    const __m128d half = _mm_set1_pd(0.5), one = _mm_set1_pd(1);
    auto lanes = [&](uint32_t q, __m128d* bg, __m128d* ra) {
        const __m128i c = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)q), zero), zero);
        *bg = _mm_cvtepi32_pd(c);
        *ra = _mm_cvtepi32_pd(_mm_srli_si128(c, 8));
    };
    auto round = [&](__m128d v) {  // lround for v ≥ 0
        const __m128d tr = _mm_cvtepi32_pd(_mm_cvttpd_epi32(v));
        return _mm_cvttpd_epi32(_mm_add_pd(tr, _mm_and_pd(_mm_cmpge_pd(_mm_sub_pd(v, tr), half), one)));
    };
    for (int y = 0; y < h; ++y) {
        const double sy = std::min(std::clamp(r.y + (y + 0.5) * ky - 0.5, 0.0, H - 1.0), H - 1.001);
        const int y0 = (int)std::floor(sy);
        const double fy = sy - y0, gy = 1 - fy;
        const uint32_t* r0 = src.Bits() + (size_t)y0 * W;
        const uint32_t* r1 = r0 + W;
        uint32_t* d = out->Bits() + (size_t)y * w;
        __m128d bg0{}, ra0{}, bg1{}, ra1{}, bg2{}, ra2{}, bg3{}, ra3{};
        int have = -1;  // the source column whose pixels are in those (zoomed in, neighbors share them)
        for (int x = 0; x < w; ++x) {
            const int x0 = xs[x];
            const double fx = fxs[x], gx = gxs[x];
            const __m128d w0 = _mm_set1_pd(gx * gy), w1 = _mm_set1_pd(fx * gy), w2 = _mm_set1_pd(gx * fy), w3 = _mm_set1_pd(fx * fy);
            if (x0 != have) {
                have = x0;
                lanes(r0[x0], &bg0, &ra0);
                lanes(r0[x0 + 1], &bg1, &ra1);
                lanes(r1[x0], &bg2, &ra2);
                lanes(r1[x0 + 1], &bg3, &ra3);
            }
            __m128d bg = _mm_mul_pd(bg0, w0), ra = _mm_mul_pd(ra0, w0);
            bg = _mm_add_pd(bg, _mm_mul_pd(bg1, w1)), ra = _mm_add_pd(ra, _mm_mul_pd(ra1, w1));
            bg = _mm_add_pd(bg, _mm_mul_pd(bg2, w2)), ra = _mm_add_pd(ra, _mm_mul_pd(ra2, w2));
            bg = _mm_add_pd(bg, _mm_mul_pd(bg3, w3)), ra = _mm_add_pd(ra, _mm_mul_pd(ra3, w3));
            const __m128i q = _mm_unpacklo_epi64(round(bg), round(ra));  // B, G, R, A
            d[x] = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(_mm_packs_epi32(q, zero), zero));
        }
    }
    return out;
}

// Rendered pieces (marks, captions, titles) keyed by everything that changes their pixels.
// Bounded by memory as well as count: a title card is a full output frame (33 MB at 4K), and editing makes a
// new one for every keystroke.
std::mutex g_cacheMu;
std::unordered_map<std::wstring, BitmapPtr> g_cache;
size_t g_cacheBytes = 0;
constexpr size_t kCacheMaxBytes = 256u << 20, kCacheMaxEntries = 400;

template <class F>
BitmapPtr Cached(const std::wstring& key, F make) {
    {
        std::lock_guard lock(g_cacheMu);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
    }
    BitmapPtr img = make();
    if (!img) return nullptr;
    const size_t bytes = (size_t)img->Width() * img->Height() * 4;
    std::lock_guard lock(g_cacheMu);
    if (g_cache.size() >= kCacheMaxEntries || g_cacheBytes + bytes > kCacheMaxBytes) {
        g_cache.clear();
        g_cacheBytes = 0;
    }
    if (g_cache.emplace(key, img).second) g_cacheBytes += bytes;
    return img;
}

std::wstring Typed(const std::wstring& s, double reveal) {
    if (reveal >= 1) return s;
    size_t n = std::min(s.size(), (size_t)std::ceil(s.size() * reveal));
    if (n > 0 && n < s.size() && IS_HIGH_SURROGATE(s[n - 1])) ++n;  // never split an emoji in half
    return s.substr(0, n);
}

std::wstring Trimmed(const std::wstring& s) {
    const size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

double Ease(double x) { return x * x * (3 - 2 * x); }

}  // namespace

void ClearRenderCache() {
    std::lock_guard lock(g_cacheMu);
    g_cache.clear();
    g_cacheBytes = 0;
}

// Where PlaceImage puts `img` before clipping it to the destination: `r` widened for a blur-in, scaled, moved.
static std::optional<VRect> PlacedRect(const Bitmap& img, VRect r, const Motion& mo) {
    if (r.w <= 0 || r.h <= 0 || mo.alpha <= 0.001) return std::nullopt;
    if (mo.blur > 0.3) {
        const double k = img.Width() / r.w;
        const int pad = (int)std::ceil(mo.blur * 3 * k);
        r = r.Inset(-pad / k, -pad / k);
    }
    const VRect c{r.MidX() - r.w * mo.scale / 2 + mo.dx, r.MidY() - r.h * mo.scale / 2 + mo.dy, r.w * mo.scale, r.h * mo.scale};
    if (c.w < 0.5 || c.h < 0.5) return std::nullopt;
    return c;
}

void PlaceImage(Bitmap& dst, const Bitmap& img, VRect r, const Motion& mo, POINT at) {
    if (r.w <= 0 || r.h <= 0 || mo.alpha <= 0.001) return;
    const Bitmap* src = &img;
    BitmapPtr blurred;
    double clipX = img.Width() * (mo.wipe && mo.reveal < 1 ? std::max(0.0, mo.reveal) : 1.0);  // wipe: source columns kept
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
            clipX = 1e9;
            r = r.Inset(-pad / k, -pad / k);
        }
    }
    // Scale about the center, then move.
    VRect c{r.MidX() - r.w * mo.scale / 2 + mo.dx, r.MidY() - r.h * mo.scale / 2 + mo.dy, r.w * mo.scale, r.h * mo.scale};
    if (c.w < 0.5 || c.h < 0.5) return;
    const uint32_t a = (uint32_t)std::clamp(std::lround(mo.alpha * 255), 0L, 255L);
    // `dst` covers [at.x, at.x + W) × [at.y, at.y + H) of the frame; everything is worked out in frame pixels.
    const int W = dst.Width(), H = dst.Height();
    const int x0 = std::max((int)at.x, (int)std::floor(c.x)), y0 = std::max((int)at.y, (int)std::floor(c.y));
    const int x1 = std::min((int)at.x + W, (int)std::ceil(c.MaxX())), y1 = std::min((int)at.y + H, (int)std::ceil(c.MaxY()));
    const int sw = src->Width(), sh = src->Height();
    const bool exact = c.x == std::floor(c.x) && c.y == std::floor(c.y) && std::fabs(c.w - sw) < 1e-6 && std::fabs(c.h - sh) < 1e-6;
    for (int y = y0; y < y1; ++y) {
        uint32_t* const line = dst.Bits() + (size_t)(y - at.y) * W;
        auto row = [&](int x) -> uint32_t& { return line[x - at.x]; };
        int xs = x0;
        if (exact && y - (int)c.y < sh) {  // four at a time while the source row lasts, then one by one
            const int cx = (int)c.x;
            const int end = (int)std::min({(double)x1, (double)cx + sw, cx + std::ceil(clipX)});
            const uint32_t* s = src->Bits() + (size_t)(y - (int)c.y) * sw;
            for (; xs + 4 <= end; xs += 4) OverFour(&row(xs), s + (xs - cx), a);
        }
        for (int x = xs; x < x1; ++x) {
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
            Over(row(x), p);
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

BitmapPtr FrameRenderer::Render(const Bitmap& src, double t) const { return Draw(src, nullptr, t, false); }

BitmapPtr FrameRenderer::Render(const BitmapPtr& src, double t, bool owned) const { return src ? Draw(*src, &src, t, owned) : nullptr; }

BitmapPtr FrameRenderer::Draw(const Bitmap& src, const BitmapPtr* shared, double t, bool owned) const {
    // Steps 1–2 change the video itself, on a copy of it (or on it, when it's `owned`). An export frame without
    // them reads the source as is.
    bool onVideo = false;
    for (const auto& m : edit_.marks) onVideo = onVideo || (m.kind != MarkKind::Title && m.kind != MarkKind::Zoom && m.Active(t));
    const BitmapPtr img = preview_ || onVideo ? (owned && shared && !preview_ ? *shared : Copy(src)) : nullptr;
    if ((preview_ || onVideo) && !img) return nullptr;
    const VRect extent{0, 0, (double)src.Width(), (double)src.Height()};
    // 1. Blur and pixelate, in the order they were added.
    for (const auto& m : edit_.marks) {
        if (!img || (m.kind != MarkKind::Blur && m.kind != MarkKind::Pixelate) || !m.Active(t)) continue;
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
        if (!img || KindIsRegion(m.kind) || m.kind == MarkKind::Title || !m.Active(t)) continue;
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
    const Bitmap& video = img ? *img : src;
    bool overlays = false;  // anything for step 4 to draw
    for (const auto& c : edit_.captions) overlays = overlays || (c.Active(t) && (preview_ || !Trimmed(c.text).empty()));
    for (const auto& m : edit_.marks) overlays = overlays || (m.kind == MarkKind::Title && m.Active(t));
    BitmapPtr framed;
    if (preview_ && r == view_) framed = nullptr;  // drawn in place
    else if (!preview_ && r == extent && tsize.cx == src.Width() && tsize.cy == src.Height())  // the whole frame, unscaled
        framed = img ? img : shared && (!overlays || owned) ? *shared : Copy(src);
    else framed = SampleRect(video, r, tsize.cx, tsize.cy);
    if (!framed && !(preview_ && r == view_)) return nullptr;
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

bool FrameRenderer::Untouched(double t) const {
    if (preview_ || out_.cx != full_.cx || out_.cy != full_.cy) return false;
    for (const auto& m : edit_.marks)
        if (m.Active(t) && m.kind != MarkKind::Zoom) return false;
    for (const auto& c : edit_.captions)
        if (c.Active(t) && !Trimmed(c.text).empty()) return false;
    return ViewRect(t) == VRect{0, 0, (double)full_.cx, (double)full_.cy};
}

std::optional<RECT> FrameRenderer::CaptionArea(double t) const {
    if (preview_ || out_.cx != full_.cx || out_.cy != full_.cy || (full_.cx & 1) || (full_.cy & 1)) return std::nullopt;
    for (const auto& m : edit_.marks)
        if (m.Active(t) && m.kind != MarkKind::Zoom) return std::nullopt;
    if (!(ViewRect(t) == VRect{0, 0, (double)full_.cx, (double)full_.cy})) return std::nullopt;
    RECT a{full_.cx, full_.cy, 0, 0};
    for (const auto& c : edit_.captions) {
        if (!c.Active(t) || Trimmed(c.text).empty()) continue;
        const Motion mo = CaptionMotion(c, t);
        if (mo.alpha <= 0.001) continue;
        const auto pl = CaptionImage(c, out_, t, mo.reveal);
        const auto r = pl ? PlacedRect(*pl->image, pl->rect, mo) : std::nullopt;
        if (!r) continue;
        a.left = std::min(a.left, (LONG)std::max(0.0, std::floor(r->x)));
        a.top = std::min(a.top, (LONG)std::max(0.0, std::floor(r->y)));
        a.right = std::max(a.right, (LONG)std::min((double)full_.cx, std::ceil(r->MaxX())));
        a.bottom = std::max(a.bottom, (LONG)std::min((double)full_.cy, std::ceil(r->MaxY())));
    }
    if (a.left >= a.right || a.top >= a.bottom) return RECT{};
    // Whole 2 × 2 blocks, as the chroma has them.
    return RECT{a.left & ~1L, a.top & ~1L, std::min((LONG)full_.cx, (a.right + 1) & ~1L), std::min((LONG)full_.cy, (a.bottom + 1) & ~1L)};
}

void FrameRenderer::DrawCaptions(Bitmap& area, POINT at, double t) const {
    for (const auto& c : edit_.captions) {
        if (!c.Active(t) || Trimmed(c.text).empty()) continue;
        const Motion mo = CaptionMotion(c, t);
        if (mo.alpha <= 0.001) continue;
        if (auto pl = CaptionImage(c, out_, t, mo.reveal)) PlaceImage(area, *pl->image, pl->rect, mo, at);
    }
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
    if (settled && m.id == settled) return {};
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
    if (settled && c.id == settled) return {};
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
    // Paused preview: the selected item shows fully, even at its first frame.
    Mark fresh = MakeMark(MarkKind::Text, {0, 0}, {10, 10}, 0, L"New", AnimStyle::Fade);
    CHECK_NEAR(r.MotionOf(fresh, 1).alpha, 0, 0.01);
    FrameRenderer paused(EditWith({}), kSize, true);
    paused.settled = fresh.id;
    CHECK_EQ(paused.MotionOf(fresh, 1).alpha, 1.0);
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

// The typewriter never shows half of an emoji (a UTF-16 surrogate pair).
ATHER_TEST(video_typewriter_keeps_emoji_whole) {
    const std::wstring t = L"Go \U0001F680!";  // 'G' 'o' ' ' high low '!'
    for (int i = 0; i <= 60; ++i) {
        const std::wstring part = Typed(t, i / 60.0);
        CHECK(part.empty() || !IS_HIGH_SURROGATE(part.back()));
    }
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

static Clip TestClip(const wchar_t* path, double in, double out) {
    Clip c;
    c.path = path;
    c.in = in;
    c.out = out;
    c.length = 10;
    c.w = 640;
    c.h = 360;
    return c;
}

ATHER_TEST(video_clips_locate_and_total) {
    const std::vector<Clip> v = {TestClip(L"a", 1, 3), TestClip(L"b", 0, 4)};
    CHECK_NEAR(ClipsDuration(v), 6, 1e-9);
    CHECK_NEAR(ClipStart(v, 1), 2, 1e-9);
    auto s = LocateClip(v, 2.5);
    CHECK(s && s->first == 1 && std::fabs(s->second - 0.5) < 1e-9);
    s = LocateClip(v, 0.5);
    CHECK(s && s->first == 0 && std::fabs(s->second - 1.5) < 1e-9);
    s = LocateClip(v, 99);  // past the end: the end of the last clip
    CHECK(s && s->first == 1 && std::fabs(s->second - 4) < 1e-9);
}

// Reordering, removing and trimming clips moves markup and captions with their footage.
ATHER_TEST(video_clip_changes_move_items_with_their_footage) {
    VideoEdit e;
    const Clip a = TestClip(L"a", 0, 4), b = TestClip(L"b", 0, 2);
    e.clips = {a, b};
    e.trimEnd = 6;
    Mark inA, inB;
    inA.start = 1, inA.end = 2;
    inB.start = 4.5, inB.end = 5.5;
    e.marks = {inA, inB};
    Caption cap;
    cap.start = 4.2, cap.end = 4.8;
    cap.words = {{4.2, 4.5, L"hi"}};
    e.captions = {cap};

    VideoEdit swapped = e;
    ApplyClips(swapped, {b, a});  // b first now
    CHECK_NEAR(swapped.marks[0].start, 3, 1e-9);    // a's mark moved 2 s later
    CHECK_NEAR(swapped.marks[1].start, 0.5, 1e-9);  // b's mark moved to the front
    CHECK_NEAR(swapped.captions[0].start, 0.2, 1e-9);
    CHECK_NEAR(swapped.captions[0].words[0].start, 0.2, 1e-9);  // word times follow
    CHECK_NEAR(swapped.trimEnd, 6, 1e-9);

    VideoEdit removed = e;
    ApplyClips(removed, {a});
    CHECK_EQ(removed.marks.size(), 1u);  // b's mark went with b
    CHECK(removed.captions.empty());
    CHECK_NEAR(removed.trimEnd, 4, 1e-9);

    VideoEdit trimmed = e;
    Clip a2 = a;
    a2.in = 2.5;  // cut a's first 2.5 s: the mark at 1–2 s is gone, b moves 2.5 s earlier
    ApplyClips(trimmed, {a2, b});
    CHECK_EQ(trimmed.marks.size(), 1u);
    CHECK_NEAR(trimmed.marks[0].start, 2, 1e-9);
    CHECK_NEAR(trimmed.trimEnd, 3.5, 1e-9);

    VideoEdit split = e;  // splitting a clip keeps everything where it was; dropping one half drops its items
    Clip left = a, right = a;
    left.out = 1.5;
    right.id = NewItemId();
    right.in = 1.5;
    ApplyClips(split, {left, right, b});
    CHECK_EQ(split.marks.size(), 2u);
    CHECK_NEAR(split.marks[0].start, 1, 1e-9);
    CHECK_NEAR(split.marks[1].start, 4.5, 1e-9);
    ApplyClips(split, {right, b});
    CHECK_EQ(split.marks.size(), 1u);
    CHECK_NEAR(split.marks[0].start, 3, 1e-9);

    VideoEdit twice;  // the same file added twice: removing one copy drops its items, they don't jump to the other
    Clip x1 = TestClip(L"x", 0, 10), x2 = TestClip(L"x", 0, 10);
    x1.source = 1;
    x2.source = 2;
    twice.clips = {x1, x2};
    twice.trimEnd = 18;
    Caption late;
    late.start = 15, late.end = 17;
    twice.captions = {late};
    ApplyClips(twice, {x1});
    CHECK(twice.captions.empty());
    CHECK_NEAR(twice.trimEnd, 10, 1e-9);

    VideoEdit kept = e;  // the trim follows its footage
    kept.trimStart = 1;  // 1 s into a
    kept.trimEnd = 5;    // 1 s into b
    VideoEdit cut = kept;
    Clip b2 = b;
    b2.out = 1.5;
    ApplyClips(cut, {a, b2});
    CHECK_NEAR(cut.trimStart, 1, 1e-9);
    CHECK_NEAR(cut.trimEnd, 5, 1e-9);
    ApplyClips(kept, {b, a});  // the ends would cross: back to the whole sequence
    CHECK_NEAR(kept.trimStart, 0, 1e-9);
    CHECK_NEAR(kept.trimEnd, 6, 1e-9);
}

}  // namespace ather
