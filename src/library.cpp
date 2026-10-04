#include "library.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <unordered_set>

#include "media.h"
#include "ocr.h"
#include "smart.h"

namespace ather {

// ---- small helpers ----

std::wstring LowerText(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring out(s.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), (int)s.size(), out.data(), (int)out.size(),
                                nullptr, nullptr, 0);
    if (n <= 0) {
        out = s;
        for (auto& c : out) c = (wchar_t)towlower(c);
    } else {
        out.resize(n);
    }
    return out;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::vector<std::wstring> NormalizeTags(const std::vector<std::wstring>& tags) {
    std::vector<std::wstring> out, seen;
    for (const auto& raw : tags) {
        std::wstring t = Trim(raw);
        if (t.empty()) continue;
        std::wstring l = LowerText(t);
        if (std::find(seen.begin(), seen.end(), l) != seen.end()) continue;
        seen.push_back(l);
        out.push_back(t);
    }
    return out;
}

static bool SameTag(const std::wstring& a, const std::wstring& b) { return LowerText(a) == LowerText(b); }

std::wstring NewUuid() {
    GUID g;
    CoCreateGuid(&g);
    wchar_t s[40];
    swprintf_s(s, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
               g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;  // same spelling as Swift's UUID().uuidString
}

static double FileTimeToEpoch(const FILETIME& ft) {
    const uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (double)((int64_t)t - 116444736000000000ll) / 1e7;
}

std::wstring FormatLibraryDate(double epoch) {
    const uint64_t t = (uint64_t)(epoch * 1e7) + 116444736000000000ull;
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t s[32];
    swprintf_s(s, L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return s;
}

static std::wstring ExtOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    return LowerText(path.substr(dot + 1));
}

MediaType MediaTypeOf(const std::wstring& path) {
    const std::wstring e = ExtOf(path);
    if (e == L"gif") return MediaType::Gif;
    if (e == L"mp4" || e == L"mov" || e == L"m4v") return MediaType::Video;
    return MediaType::Image;
}

const wchar_t* MediaTypeName(MediaType t) { return t == MediaType::Image ? L"image" : t == MediaType::Gif ? L"gif" : L"video"; }
const wchar_t* MediaTypeLabel(MediaType t) { return t == MediaType::Image ? L"Screenshots" : t == MediaType::Gif ? L"GIFs" : L"Videos"; }
const wchar_t* MediaTypeWords(MediaType t) {
    return t == MediaType::Image ? L"image png screenshot" : t == MediaType::Gif ? L"gif animation" : L"video mp4 recording movie";
}

bool IsMediaFile(const std::wstring& path) {
    static const wchar_t* const kExts[] = {L"png", L"jpg", L"jpeg", L"gif", L"mp4", L"mov", L"m4v", L"heic", L"tif", L"tiff", L"webp", L"bmp"};
    const std::wstring e = ExtOf(path);
    for (const wchar_t* k : kExts)
        if (e == k) return true;
    return false;
}

std::wstring PaletteColor::Hex() const {
    wchar_t s[8];
    swprintf_s(s, L"#%02X%02X%02X", r, g, b);
    return s;
}

// ---- JSON mapping (same keys and spellings as the Mac app's Codable types) ----

static Json StrArray(const std::vector<std::wstring>& v) {
    Json a = Json::Array();
    for (const auto& s : v) a.Push(Json(s));
    return a;
}

static std::vector<std::wstring> ReadStrArray(const Json& j) {
    std::vector<std::wstring> out;
    for (const auto& e : j.Items())
        if (e.IsString()) out.push_back(e.WStr());
    return out;
}

Json ItemMeta::ToJson() const {
    Json o = Json::Object();
    o.Set("mtime", Json(mtime));
    o.Set("size", Json((int64_t)size));
    o.Set("w", Json(w));
    o.Set("h", Json(h));
    if (duration) o.Set("duration", Json(*duration));
    o.Set("tags", StrArray(tags));
    o.Set("rating", Json(rating));
    o.Set("comment", Json(comment));
    o.Set("collections", StrArray(collections));
    o.Set("app", Json(app));
    o.Set("window", Json(window));
    if (text) o.Set("text", Json(*text));
    Json cs = Json::Array();
    for (const auto& c : colors) {
        Json cj = Json::Object();
        cj.Set("r", Json((int)c.r));
        cj.Set("g", Json((int)c.g));
        cj.Set("b", Json((int)c.b));
        cj.Set("ratio", Json(c.ratio));
        cs.Push(std::move(cj));
    }
    o.Set("colors", std::move(cs));
    if (dhash) o.Set("dhash", Json(*dhash));
    o.Set("indexed", Json(indexed));
    if (editedFrom) o.Set("editedFrom", Json(*editedFrom));
    o.Set("includes", StrArray(includes));
    o.Set("dismissed", StrArray(dismissed));
    return o;
}

ItemMeta ItemMeta::FromJson(const Json& j) {
    ItemMeta m;
    m.mtime = j["mtime"].Num(0);
    m.size = j["size"].Int(0);
    m.w = (int)j["w"].Int(0);
    m.h = (int)j["h"].Int(0);
    if (j["duration"].IsNumber()) m.duration = j["duration"].Num();
    m.tags = ReadStrArray(j["tags"]);
    m.rating = (int)std::clamp<int64_t>(j["rating"].Int(0), 0, 5);
    m.comment = j["comment"].WStr();
    m.collections = ReadStrArray(j["collections"]);
    m.app = j["app"].WStr();
    m.window = j["window"].WStr();
    if (j["text"].IsString()) m.text = j["text"].WStr();
    for (const auto& c : j["colors"].Items()) {
        if (!c.IsObject()) continue;
        PaletteColor p;
        p.r = (uint8_t)std::clamp<int64_t>(c["r"].Int(0), 0, 255);
        p.g = (uint8_t)std::clamp<int64_t>(c["g"].Int(0), 0, 255);
        p.b = (uint8_t)std::clamp<int64_t>(c["b"].Int(0), 0, 255);
        p.ratio = c["ratio"].Num(0);
        m.colors.push_back(p);
    }
    if (j["dhash"].IsNumber()) m.dhash = j["dhash"].UInt();
    m.indexed = (int)j["indexed"].Int(0);
    if (j["editedFrom"].IsString()) m.editedFrom = j["editedFrom"].WStr();
    m.includes = ReadStrArray(j["includes"]);
    m.dismissed = ReadStrArray(j["dismissed"]);
    return m;
}

// ---- filters ----

static const wchar_t* const kShapeNames[] = {L"", L"landscape", L"portrait", L"square", L"wide", L"tall"};
static const wchar_t* const kDateNames[] = {L"", L"today", L"week", L"month", L"year"};
static const wchar_t* const kSizeNames[] = {L"", L"small", L"medium", L"large", L"huge"};

const wchar_t* ShapeLabel(ShapeFilter s) {
    static const wchar_t* const k[] = {L"Any shape", L"Landscape", L"Portrait", L"Square-ish", L"Panorama (> 2:1)", L"Long page (< 1:2)"};
    return k[(int)s];
}
const wchar_t* DateLabel(DateFilter d) {
    static const wchar_t* const k[] = {L"Any time", L"Today", L"Last 7 days", L"Last 30 days", L"Last 12 months"};
    return k[(int)d];
}
const wchar_t* SizeLabel(SizeFilter s) {
    static const wchar_t* const k[] = {L"Any size", L"Under 200 KB", L"200 KB – 2 MB", L"2 – 20 MB", L"Over 20 MB"};
    return k[(int)s];
}

bool ShapeMatches(ShapeFilter s, double a) {
    switch (s) {
        case ShapeFilter::Landscape: return a > 1.1;
        case ShapeFilter::Portrait: return a < 0.9;
        case ShapeFilter::Square: return a >= 0.9 && a <= 1.1;
        case ShapeFilter::Wide: return a > 2;
        case ShapeFilter::Tall: return a < 0.5;
        default: return true;
    }
}

bool SizeMatches(SizeFilter s, int64_t b) {
    switch (s) {
        case SizeFilter::Small: return b < 200'000;
        case SizeFilter::Medium: return b >= 200'000 && b < 2'000'000;
        case SizeFilter::Large: return b >= 2'000'000 && b < 20'000'000;
        case SizeFilter::Huge: return b >= 20'000'000;
        default: return true;
    }
}

double DateSince(DateFilter d, double now) {
    switch (d) {
        case DateFilter::Today: {
            SearchQuery q = SearchQuery::Parse(L"today", now);
            return q.since.value_or(now - 86400);
        }
        case DateFilter::Week: return now - 7 * 86400;
        case DateFilter::Month: return now - 30 * 86400;
        case DateFilter::Year: return now - 365 * 86400;
        default: return 0;
    }
}

int Filter::ActiveCount() const {
    const bool parts[] = {!text.empty(), !types.empty(), !tags.empty() || untagged, minRating > 0, !color.empty(),
                          shape != ShapeFilter::Any, date != DateFilter::Any, size != SizeFilter::Any, !apps.empty(),
                          minWidth > 0 || minHeight > 0};
    return (int)std::count(std::begin(parts), std::end(parts), true);
}

bool Filter::Rgb(const std::wstring& hex, uint8_t* r, uint8_t* g, uint8_t* b) {
    std::wstring s = hex;
    while (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() != 6) return false;
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(s.c_str(), &end, 16);
    if (!end || *end) return false;
    *r = (uint8_t)(v >> 16 & 255);
    *g = (uint8_t)(v >> 8 & 255);
    *b = (uint8_t)(v & 255);
    return true;
}

double Filter::Distance(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2) {
    const double rm = (r1 + (double)r2) / 2;
    const double dr = (double)r1 - r2, dg = (double)g1 - g2, db = (double)b1 - b2;
    return std::sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

MatchKind Filter::Match(const std::wstring& path, const ItemMeta& m, double now) const {
    if (!types.empty() && std::find(types.begin(), types.end(), MediaTypeOf(path)) == types.end()) return MatchKind::None;
    if (untagged && !m.tags.empty()) return MatchKind::None;
    if (!tags.empty()) {
        std::vector<std::wstring> mine;
        for (const auto& t : m.tags) mine.push_back(LowerText(t));
        auto has = [&](const std::wstring& t) { return std::find(mine.begin(), mine.end(), LowerText(t)) != mine.end(); };
        if (anyTag ? !std::any_of(tags.begin(), tags.end(), has) : !std::all_of(tags.begin(), tags.end(), has)) return MatchKind::None;
    }
    if (minRating > 0 && m.rating < minRating) return MatchKind::None;
    if (!ShapeMatches(shape, m.Aspect())) return MatchKind::None;
    if (now == 0) now = NowEpoch();
    if (date != DateFilter::Any && m.mtime < DateSince(date, now)) return MatchKind::None;
    if (!SizeMatches(size, m.size)) return MatchKind::None;
    if (!apps.empty() && std::find(apps.begin(), apps.end(), m.app) == apps.end()) return MatchKind::None;
    if (minWidth > 0 && m.w < minWidth) return MatchKind::None;
    if (minHeight > 0 && m.h < minHeight) return MatchKind::None;
    uint8_t r, g, b;
    if (!color.empty() && Rgb(color, &r, &g, &b)) {
        const bool any = std::any_of(m.colors.begin(), m.colors.end(), [&](const PaletteColor& c) {
            return c.ratio >= 0.03 && Distance(r, g, b, c.r, c.g, c.b) < 100;
        });
        if (!any) return MatchKind::None;
    }
    const SearchQuery q = SearchQuery::Parse(text, now);
    if (q.IsEmpty()) return MatchKind::Exact;
    std::wstring hay = FileNameOf(path) + L" " + FormatLibraryDate(m.mtime) + L" " + MediaTypeWords(MediaTypeOf(path)) + L" " +
                       m.text.value_or(L"") + L" " + m.comment;
    for (const auto& t : m.tags) hay += L" " + t;
    for (const auto& t : autotag::Suggest(path, m)) hay += L" " + t;
    hay += L" " + m.app + L" " + m.window;
    return q.Match(LowerText(hay), m.mtime);
}

Json Filter::ToJson() const {
    Json o = Json::Object();
    o.Set("text", Json(text));
    Json ts = Json::Array();
    for (auto t : types) ts.Push(Json(std::wstring(MediaTypeName(t))));
    o.Set("types", std::move(ts));
    o.Set("tags", StrArray(tags));
    o.Set("anyTag", Json(anyTag));
    o.Set("untagged", Json(untagged));
    o.Set("minRating", Json(minRating));
    if (!color.empty()) o.Set("color", Json(color));
    if (shape != ShapeFilter::Any) o.Set("shape", Json(std::wstring(kShapeNames[(int)shape])));
    if (date != DateFilter::Any) o.Set("date", Json(std::wstring(kDateNames[(int)date])));
    if (size != SizeFilter::Any) o.Set("size", Json(std::wstring(kSizeNames[(int)size])));
    o.Set("apps", StrArray(apps));
    o.Set("minWidth", Json(minWidth));
    o.Set("minHeight", Json(minHeight));
    return o;
}

template <size_t N>
static int EnumIndex(const wchar_t* const (&names)[N], const std::wstring& v) {
    for (size_t i = 1; i < N; ++i)
        if (v == names[i]) return (int)i;
    return 0;
}

Filter Filter::FromJson(const Json& j) {
    Filter f;
    f.text = j["text"].WStr();
    for (const auto& t : ReadStrArray(j["types"])) {
        if (t == L"image") f.types.push_back(MediaType::Image);
        else if (t == L"gif") f.types.push_back(MediaType::Gif);
        else if (t == L"video") f.types.push_back(MediaType::Video);
    }
    f.tags = ReadStrArray(j["tags"]);
    f.anyTag = j["anyTag"].Bool(false);
    f.untagged = j["untagged"].Bool(false);
    f.minRating = (int)j["minRating"].Int(0);
    f.color = j["color"].WStr();
    f.shape = (ShapeFilter)EnumIndex(kShapeNames, j["shape"].WStr());
    f.date = (DateFilter)EnumIndex(kDateNames, j["date"].WStr());
    f.size = (SizeFilter)EnumIndex(kSizeNames, j["size"].WStr());
    f.apps = ReadStrArray(j["apps"]);
    f.minWidth = (int)j["minWidth"].Int(0);
    f.minHeight = (int)j["minHeight"].Int(0);
    return f;
}

// ---- indexer ----

namespace indexer {

// 0xAARRGGBB -> r, g, b
static inline void Rgb(uint32_t p, int& r, int& g, int& b) {
    r = (p >> 16) & 255;
    g = (p >> 8) & 255;
    b = p & 255;
}

uint64_t DHash(const Bitmap& img) {
    auto px = Resample(img, 9, 8);
    if (!px) return 0;
    auto lum = [&](int x, int y) {
        int r, g, b;
        Rgb(px->Bits()[y * 9 + x], r, g, b);
        return r * 299 + g * 587 + b * 114;
    };
    uint64_t h = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) h = h << 1 | (lum(x, y) > lum(x + 1, y) ? 1 : 0);
    return h;
}

std::vector<PaletteColor> Palette(const Bitmap& img, int k) {
    constexpr int n = 48;
    auto thumb = Resample(img, n, n);
    if (!thumb) return {};
    struct P {
        double r, g, b;
    };
    std::vector<P> pts;
    pts.reserve(n * n);
    for (int i = 0; i < n * n; ++i) {
        int r, g, b;
        Rgb(thumb->Bits()[i], r, g, b);
        pts.push_back({(double)r, (double)g, (double)b});
    }
    // Deterministic seeds spread across the luminance range.
    std::vector<P> sorted = pts;
    std::stable_sort(sorted.begin(), sorted.end(), [](const P& a, const P& b) { return a.r + a.g + a.b < b.r + b.g + b.b; });
    std::vector<P> centers;
    for (int j = 0; j < k; ++j) centers.push_back(sorted[std::min<size_t>(sorted.size() - 1, (size_t)(j * 2 + 1) * sorted.size() / (2 * k))]);
    std::vector<int> assign(pts.size(), 0);
    for (int it = 0; it < 10; ++it) {
        std::vector<std::array<double, 4>> sums(k, {0, 0, 0, 0});
        for (size_t i = 0; i < pts.size(); ++i) {
            int best = 0;
            double bd = 1e300;
            for (int j = 0; j < k; ++j) {
                const double dr = pts[i].r - centers[j].r, dg = pts[i].g - centers[j].g, db = pts[i].b - centers[j].b;
                const double d = dr * dr + dg * dg + db * db;
                if (d < bd) bd = d, best = j;
            }
            assign[i] = best;
            sums[best][0] += pts[i].r;
            sums[best][1] += pts[i].g;
            sums[best][2] += pts[i].b;
            sums[best][3] += 1;
        }
        for (int j = 0; j < k; ++j)
            if (sums[j][3] > 0) centers[j] = {sums[j][0] / sums[j][3], sums[j][1] / sums[j][3], sums[j][2] / sums[j][3]};
    }
    std::vector<int> counts(k, 0);
    for (int a : assign) ++counts[a];
    std::vector<PaletteColor> out;
    for (int j = 0; j < k; ++j) {
        if (!counts[j]) continue;
        PaletteColor s{(uint8_t)std::lround(centers[j].r), (uint8_t)std::lround(centers[j].g), (uint8_t)std::lround(centers[j].b),
                       (double)counts[j] / pts.size()};
        auto close = std::find_if(out.begin(), out.end(), [&](const PaletteColor& o) {  // merge near-identical clusters
            return Filter::Distance(o.r, o.g, o.b, s.r, s.g, s.b) < 30;
        });
        if (close != out.end()) close->ratio += s.ratio;
        else out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const PaletteColor& a, const PaletteColor& b) { return a.ratio > b.ratio; });
    return out;
}

// Visual feature vector for "find similar" and duplicate confirmation. Windows has no built-in image
// embedding (the Mac app uses Vision's feature print), so this combines two cheap, size-independent views:
//   * a 4×4×4 RGB colour histogram (square-rooted, i.e. Hellinger) - what colours, how much of each
//   * the 255 AC coefficients of a 16×16 grayscale DCT (DC removed) - the layout
// Each half is unit-length; the halves are weighted kHistW²/kDctW² and the whole vector is unit-length, so
// cosine similarity is a dot product. FeatureDistance scales (1 - cos) by kScale.
// Calibration (`--feature-stats` on 29 real screenshots, 406 pairs): a half-size re-encoded copy scores
// ≤ 0.011; different captures have a median of 1.1 and a 5th percentile of 0.53; of the 4 pairs under 0.3,
// only the one that was a real near-duplicate also had a dHash within 10 bits. That fits the Mac thresholds:
// duplicates need d ≤ 0.3 (plus dHash ≤ 10), and similarity is 100 × (1 − d / 1.25).
constexpr float kHistW = 0.6f, kDctW = 0.8f, kScale = 1.6f;

std::vector<float> Feature(const Bitmap& img) {
    std::vector<float> v(64 + 255, 0.f);
    auto thumb = Resample(img, 64, 64);
    auto gray = Resample(img, 16, 16);
    if (!thumb || !gray) return {};
    for (int i = 0; i < 64 * 64; ++i) {
        int r, g, b;
        Rgb(thumb->Bits()[i], r, g, b);
        v[(r >> 6) * 16 + (g >> 6) * 4 + (b >> 6)] += 1.f;
    }
    double hn = 0;
    for (int i = 0; i < 64; ++i) {
        v[i] = std::sqrt(v[i] / (64.f * 64.f));
        hn += (double)v[i] * v[i];
    }
    hn = std::sqrt(std::max(hn, 1e-12));
    for (int i = 0; i < 64; ++i) v[i] = (float)(v[i] / hn * kHistW);

    double g[16][16];
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            int r, gg, b;
            Rgb(gray->Bits()[y * 16 + x], r, gg, b);
            g[y][x] = (r * 0.299 + gg * 0.587 + b * 0.114) / 255.0;
        }
    constexpr double kPi = 3.14159265358979323846;
    double dn = 0;
    int k = 64;
    for (int u = 0; u < 16; ++u)
        for (int w = 0; w < 16; ++w) {
            if (u == 0 && w == 0) continue;
            double s = 0;
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) s += g[y][x] * std::cos((2 * y + 1) * u * kPi / 32) * std::cos((2 * x + 1) * w * kPi / 32);
            v[k++] = (float)s;
            dn += s * s;
        }
    if (dn < 1e-9) {  // a flat image has no layout: leave that half at zero, scale the colours to unit length
        for (int i = 0; i < 64; ++i) v[i] /= kHistW;
        for (int i = 64; i < (int)v.size(); ++i) v[i] = 0;
        return v;
    }
    dn = std::sqrt(dn);
    for (int i = 64; i < (int)v.size(); ++i) v[i] = (float)(v[i] / dn * kDctW);
    return v;
}

float FeatureDistance(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 2.f;
    double dot = 0;
    for (size_t i = 0; i < a.size(); ++i) dot += (double)a[i] * b[i];
    return (float)std::max(0.0, (1 - dot) * kScale);
}

IndexResult Index(const std::wstring& path, bool ocr) {
    IndexResult r;
    BitmapPtr frame, full;
    if (MediaTypeOf(path) == MediaType::Video) {
        VideoInfo vi;
        if (!ProbeVideo(path, &vi)) return r;
        r.duration = vi.duration;
        r.w = vi.w;
        r.h = vi.h;
        ProbeVideo(path, nullptr, std::min(1.0, vi.duration / 2), 512, &frame);
    } else {
        if (MediaTypeOf(path) == MediaType::Gif) r.duration = GifDuration(path);
        if (ocr) {
            full = LoadImageScaled(path, 0, &r.w, &r.h);
            if (full) {
                const int m = std::max(full->Width(), full->Height());
                frame = m > 512 ? Resample(*full, std::max(1, full->Width() * 512 / m), std::max(1, full->Height() * 512 / m)) : full;
            }
        } else {
            frame = LoadImageScaled(path, 512, &r.w, &r.h);
        }
    }
    if (!frame) return r;
    r.colors = Palette(*frame);
    r.dhash = DHash(*frame);
    r.feature = Feature(*frame);
    if (ocr) {
        std::wstring t = RecognizeTextBlocking(full ? *full : *frame);
        for (auto& c : t)
            if (c == L'\n' || c == L'\r') c = L' ';
        r.text = t;
    }
    return r;
}

}  // namespace indexer

// ---- the store ----

struct Library::Workers {
    std::atomic<bool> alive{true};
    std::mutex ioMu;            // one writer at a time, newest snapshot wins
    uint64_t lastWrittenSeq = 0;
    HANDLE watchStop = nullptr;
    std::thread watcher;
    ~Workers() {
        if (watchStop) {
            SetEvent(watchStop);
            if (watcher.joinable()) watcher.join();
            CloseHandle(watchStop);
        }
    }
};

struct Library::Snapshot {
    uint64_t seq = 0;
    std::wstring file, featuresFile;
    std::unordered_map<std::wstring, ItemMeta> meta;
    std::vector<LibCollection> collections;
    std::vector<SmartFolder> smartFolders;
    bool withFeatures = false;
    std::unordered_map<std::wstring, std::vector<float>> features;
};

static std::map<UINT_PTR, Library*>& Timers() {
    static std::map<UINT_PTR, Library*> t;
    return t;
}

Library& Library::Shared() {
    static Library* lib = new Library(true);  // lives for the whole process
    return *lib;
}

Library::Library(bool persists) : persists_(persists), shared_(std::make_shared<Workers>()) {
    if (!persists) loaded_ = true;
}

Library::~Library() {
    StopBackgroundWork();
    for (UINT_PTR t : {saveTimer_, diskTimer_})
        if (t) {
            KillTimer(nullptr, t);
            Timers().erase(t);
        }
}

void Library::StopBackgroundWork() {
    shared_->alive = false;
    if (shared_->watchStop) SetEvent(shared_->watchStop);
}

std::wstring Library::FilePath() const { return SupportFolder() + L"\\library.json"; }

void Library::SetFolder(const std::wstring& f) {
    std::wstring folder = f;
    while (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    folder_ = folder;
}

int Library::Subscribe(std::function<void()> fn) {
    listeners_[nextListener_] = std::move(fn);
    return nextListener_++;
}

void Library::Unsubscribe(int id) { listeners_.erase(id); }

void Library::Changed() {
    ++version_;
    auto copy = listeners_;  // a listener may unsubscribe
    for (auto& [id, fn] : copy)
        if (fn) fn();
}

const ItemMeta& Library::Meta(const std::wstring& path) const {
    static const ItemMeta kEmpty;
    auto it = meta_.find(path);
    return it == meta_.end() ? kEmpty : it->second;
}

const LibCollection* Library::FindCollection(const std::wstring& id) const {
    for (const auto& c : collections_)
        if (c.id == id) return &c;
    return nullptr;
}

static std::string ReadAll(const std::wstring& path, bool* exists) {
    *exists = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    *exists = true;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data((size_t)size.QuadPart, '\0');
    DWORD read = 0;
    if (size.QuadPart > 0) ReadFile(f, data.data(), (DWORD)size.QuadPart, &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    return data;
}

static bool WriteAtomically(const std::wstring& path, const void* data, size_t bytes) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(f, data, (DWORD)bytes, &wr, nullptr) && wr == bytes && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void Library::LoadIfNeeded() {
    if (loaded_) return;
    loaded_ = true;
    LoadFile();
}

void Library::LoadFile() {
    bool exists = false;
    const std::string data = ReadAll(FilePath(), &exists);
    if (exists) {
        bool ok = false;
        Json j = Json::Parse(data, &ok);
        if (ok && j.IsObject()) {
            for (const auto& [k, v] : j["items"].Members())
                if (v.IsObject()) meta_[FromUtf8(k)] = ItemMeta::FromJson(v);
            for (const auto& c : j["collections"].Items()) {
                if (!c.IsObject()) continue;
                LibCollection col{c["id"].WStr(NewUuid()), c["name"].WStr(L"Collection"), ReadStrArray(c["autoTags"])};
                if (col.id.empty()) col.id = NewUuid();
                collections_.push_back(col);
            }
            for (const auto& s : j["smartFolders"].Items()) {
                if (!s.IsObject()) continue;
                SmartFolder sf{s["id"].WStr(NewUuid()), s["name"].WStr(L"Smart folder"), Filter::FromJson(s["filter"])};
                if (sf.id.empty()) sf.id = NewUuid();
                smartFolders_.push_back(sf);
            }
        } else {
            // Unreadable (corrupt, or from a newer version): keep it aside rather than overwrite it.
            const std::wstring bad = SupportFolder() + L"\\library.unreadable-" + std::to_wstring((long long)NowEpoch()) + L".json";
            MoveFileExW(FilePath().c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    } else {
        ImportLegacyOcr();
    }
    // features.bin: "ATHF", version, count, then per item: path (UTF-16, length-prefixed) and floats.
    const std::string fb = ReadAll(SupportFolder() + L"\\features.bin", &exists);
    size_t at = 12;
    auto u32 = [&](size_t o) {
        uint32_t v;
        memcpy(&v, fb.data() + o, 4);
        return v;
    };
    if (fb.size() >= 12 && fb.compare(0, 4, "ATHF") == 0 && u32(4) == 1) {
        const uint32_t count = u32(8);
        for (uint32_t i = 0; i < count && at + 4 <= fb.size(); ++i) {
            const uint32_t plen = u32(at);
            at += 4;
            if (at + plen * 2 + 4 > fb.size()) break;
            std::wstring p(plen, L'\0');
            memcpy(p.data(), fb.data() + at, plen * 2);
            at += plen * 2;
            const uint32_t dims = u32(at);
            at += 4;
            if (at + dims * 4 > fb.size()) break;
            std::vector<float> f(dims);
            memcpy(f.data(), fb.data() + at, dims * 4);
            at += dims * 4;
            features_[p] = std::move(f);
        }
    }
}

// The old History window kept OCR text in ocr-index.txt (UTF-16, "lower-case path<TAB>text" lines). Reuse it
// so nothing gets OCR'd twice; it's matched to files case-insensitively when the first scan finds them.
void Library::ImportLegacyOcr() {
    bool exists = false;
    const std::string data = ReadAll(SupportFolder() + L"\\ocr-index.txt", &exists);
    if (!exists || data.size() < 2) return;
    std::wstring text(data.size() / 2, L'\0');
    memcpy(text.data(), data.data(), text.size() * 2);
    for (size_t start = 0; start < text.size();) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos) legacyOcr_[LowerText(line.substr(0, tab))] = line.substr(tab + 1);
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
}

void Library::Save() {
    if (!persists_ || !loaded_) return;
    if (saveTimer_) {
        KillTimer(nullptr, saveTimer_);
        Timers().erase(saveTimer_);
    }
    saveTimer_ = SetTimer(nullptr, 0, 800, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        auto it = Timers().find(id);
        if (it == Timers().end()) return;
        Library* self = it->second;
        Timers().erase(it);
        self->saveTimer_ = 0;
        auto snap = std::make_shared<Snapshot>();
        snap->seq = ++self->saveSeq_;
        snap->file = self->FilePath();
        snap->featuresFile = SupportFolder() + L"\\features.bin";
        snap->meta = self->meta_;
        snap->collections = self->collections_;
        snap->smartFolders = self->smartFolders_;
        if (self->featuresDirty_) {  // the big file only when it changed
            snap->withFeatures = true;
            snap->features = self->features_;
            self->featuresDirty_ = false;
        }
        auto sh = self->shared_;
        std::thread([self, snap, sh] {
            if (sh->alive) self->WriteSnapshot(snap);
        }).detach();
    });
    if (saveTimer_) Timers()[saveTimer_] = this;
}

void Library::Flush() {
    if (!persists_ || !loaded_) return;
    if (saveTimer_) {
        KillTimer(nullptr, saveTimer_);
        Timers().erase(saveTimer_);
        saveTimer_ = 0;
        auto snap = std::make_shared<Snapshot>();
        snap->seq = ++saveSeq_;
        snap->file = FilePath();
        snap->featuresFile = SupportFolder() + L"\\features.bin";
        snap->meta = meta_;
        snap->collections = collections_;
        snap->smartFolders = smartFolders_;
        snap->withFeatures = featuresDirty_;
        if (featuresDirty_) snap->features = features_;
        featuresDirty_ = false;
        WriteSnapshot(snap);
    } else {
        std::lock_guard lock(shared_->ioMu);  // wait for a write that's already under way
    }
}

void Library::WriteSnapshot(const std::shared_ptr<Snapshot>& s) {
    std::lock_guard lock(shared_->ioMu);
    if (s->seq < shared_->lastWrittenSeq) return;  // a newer snapshot is already on disk
    shared_->lastWrittenSeq = s->seq;
    Json items = Json::Object();
    std::vector<const std::wstring*> keys;
    for (const auto& kv : s->meta) keys.push_back(&kv.first);
    std::sort(keys.begin(), keys.end(), [](const std::wstring* a, const std::wstring* b) { return *a < *b; });
    for (const std::wstring* k : keys) items.Set(ToUtf8(*k), s->meta.at(*k).ToJson());
    Json cols = Json::Array();
    for (const auto& c : s->collections) {
        Json o = Json::Object();
        o.Set("id", Json(c.id));
        o.Set("name", Json(c.name));
        o.Set("autoTags", StrArray(c.autoTags));
        cols.Push(std::move(o));
    }
    Json smart = Json::Array();
    for (const auto& f : s->smartFolders) {
        Json o = Json::Object();
        o.Set("id", Json(f.id));
        o.Set("name", Json(f.name));
        o.Set("filter", f.filter.ToJson());
        smart.Push(std::move(o));
    }
    Json root = Json::Object();
    root.Set("version", Json(1));
    root.Set("items", std::move(items));
    root.Set("collections", std::move(cols));
    root.Set("smartFolders", std::move(smart));
    const std::string text = root.Dump();
    WriteAtomically(s->file, text.data(), text.size());
    if (s->withFeatures) {
        std::string b = "ATHF";
        auto put = [&](uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
        put(1);
        put((uint32_t)s->features.size());
        for (const auto& [p, f] : s->features) {
            put((uint32_t)p.size());
            b.append(reinterpret_cast<const char*>(p.data()), p.size() * 2);
            put((uint32_t)f.size());
            b.append(reinterpret_cast<const char*>(f.data()), f.size() * 4);
        }
        WriteAtomically(s->featuresFile, b.data(), b.size());
    }
}

// ---- scanning ----

bool Library::IsInside(const std::wstring& path, const std::wstring& dir) {
    std::wstring d = dir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    if (d.empty() || path.size() <= d.size()) return false;
    const wchar_t sep = path[d.size()];
    return (sep == L'\\' || sep == L'/') && CompareStringOrdinal(path.c_str(), (int)d.size(), d.c_str(), (int)d.size(), TRUE) == CSTR_EQUAL;
}

std::vector<FileStat> Library::ListCaptures(const std::wstring& folder) {
    std::vector<FileStat> out;
    std::vector<std::wstring> dirs{folder};
    while (!dirs.empty()) {
        const std::wstring dir = dirs.back();
        dirs.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.') continue;  // ., .., and dot-files/folders
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) continue;
            const std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(p);
                continue;
            }
            if (!IsMediaFile(p)) continue;
            out.push_back({p, FileTimeToEpoch(fd.ftLastWriteTime), (int64_t)((uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow)});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const FileStat& a, const FileStat& b) {
        return a.mtime != b.mtime ? a.mtime > b.mtime : a.path < b.path;
    });
    return out;
}

void Library::Refresh(std::function<void()> done) {
    LoadIfNeeded();
    Watch();
    const int gen = generation_;
    const std::wstring folder = folder_;
    auto sh = shared_;
    std::thread([this, gen, folder, sh, done] {
        auto stats = ListCaptures(folder);
        RunOnUi([this, gen, sh, done, stats = std::move(stats)] {
            if (!sh->alive) return;
            // A rename or delete happened while listing: this listing is stale, take a new one.
            if (gen != generation_) return Refresh(done);
            Apply(stats);
            if (done) done();
            IndexInBackground();
        });
    }).detach();
}

void Library::Apply(const std::vector<FileStat>& stats) {
    LoadIfNeeded();
    std::unordered_set<std::wstring> live;
    for (const auto& s : stats) {
        live.insert(s.path);
        ItemMeta& e = meta_[s.path];
        if (e.mtime != s.mtime) {  // changed on disk: re-index everything derived from the pixels
            e.indexed = 0;
            if (e.mtime != 0) e.text.reset();
        }
        e.mtime = s.mtime;
        e.size = s.size;
        if (!e.text && !legacyOcr_.empty()) {
            auto it = legacyOcr_.find(LowerText(s.path));
            if (it != legacyOcr_.end()) e.text = it->second;
        }
        auto p = pending_.find(s.path);
        if (p != pending_.end()) {
            const ItemMeta& n = p->second;
            if (e.app.empty()) e.app = n.app;
            if (e.window.empty()) e.window = n.window;
            for (const auto& t : n.tags)
                if (std::none_of(e.tags.begin(), e.tags.end(), [&](const std::wstring& x) { return SameTag(x, t); })) e.tags.push_back(t);
            for (const auto& c : n.collections)
                if (std::find(e.collections.begin(), e.collections.end(), c) == e.collections.end()) e.collections.push_back(c);
            if (e.rating == 0) e.rating = n.rating;
            if (e.comment.empty()) e.comment = n.comment;
            if (!e.editedFrom) e.editedFrom = n.editedFrom;
            if (e.includes.empty()) e.includes = n.includes;
            pending_.erase(p);
        }
    }
    for (auto it = meta_.begin(); it != meta_.end();) {
        if (!live.count(it->first) && IsInside(it->first, folder_)) it = meta_.erase(it);
        else ++it;
    }
    bool deadFeatures = false;
    for (auto it = features_.begin(); it != features_.end();) {
        if (!meta_.count(it->first)) {
            it = features_.erase(it);
            deadFeatures = true;
        } else {
            ++it;
        }
    }
    if (deadFeatures) {
        featuresDirty_ = true;
        ++techVersion_;
    }
    paths_.clear();
    for (const auto& s : stats) paths_.push_back(s.path);
    Save();
    Changed();
}

void Library::NoteCapture(const std::wstring& path, const std::wstring& app, const std::wstring& window) {
    if (app.empty() && window.empty()) return;
    ItemMeta& p = pending_[path];
    p.app = app;
    p.window = window;
}

void Library::NoteEdit(const std::wstring& path, const std::wstring& source, const std::wstring& app, const std::wstring& window,
                       bool edited, const std::vector<std::wstring>& includes, bool collage) {
    LoadIfNeeded();
    ItemMeta& p = pending_[path];
    p.includes = includes;
    if (collage) {
        std::vector<const ItemMeta*> ms;
        for (const auto& i : includes)
            if (auto it = meta_.find(i); it != meta_.end()) ms.push_back(&it->second);
        if (!ms.empty()) {
            p.tags.clear();
            for (const auto& t : ms[0]->tags)
                if (std::all_of(ms.begin(), ms.end(), [&](const ItemMeta* m) {
                        return std::any_of(m->tags.begin(), m->tags.end(), [&](const std::wstring& x) { return SameTag(x, t); });
                    }))
                    p.tags.push_back(t);
            p.collections.clear();
            for (const auto& c : ms[0]->collections)
                if (std::all_of(ms.begin(), ms.end(), [&](const ItemMeta* m) {
                        return std::find(m->collections.begin(), m->collections.end(), c) != m->collections.end();
                    }))
                    p.collections.push_back(c);
        }
        if (std::none_of(p.tags.begin(), p.tags.end(), [](const std::wstring& t) { return SameTag(t, L"collage"); }))
            p.tags.push_back(L"collage");
        return;
    }
    if (!source.empty()) {
        if (auto it = meta_.find(source); it != meta_.end()) {
            const ItemMeta& o = it->second;
            p.tags = o.tags;
            p.collections = o.collections;
            p.rating = o.rating;
            p.comment = o.comment;
            p.app = o.app;
            p.window = o.window;
            p.editedFrom = source;
        }
    }
    if (p.app.empty()) p.app = app;
    if (p.window.empty()) p.window = window;
    if (edited && std::none_of(p.tags.begin(), p.tags.end(), [](const std::wstring& t) { return SameTag(t, L"edited"); }))
        p.tags.push_back(L"edited");
}

// ---- watching the captures folder ----

// Anything written into the captures folder (captures, edits, recordings, files from Explorer) shows up in an
// open gallery without reopening it.
void Library::Watch() {
    if (!persists_ || folder_.empty() || watchedFolder_ == folder_) return;
    if (shared_->watchStop) {  // a different folder: stop the old watcher
        SetEvent(shared_->watchStop);
        if (shared_->watcher.joinable()) shared_->watcher.join();
        ResetEvent(shared_->watchStop);
    } else {
        shared_->watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    watchedFolder_ = folder_;
    CreateDirectoryW(folder_.c_str(), nullptr);
    HANDLE dir = CreateFileW(folder_.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return;
    auto sh = shared_;
    HANDLE stop = shared_->watchStop;
    shared_->watcher = std::thread([this, dir, stop, sh] {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        alignas(DWORD) BYTE buf[16384];
        for (;;) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            if (!ReadDirectoryChangesW(dir, buf, sizeof(buf), TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE,
                                       &got, &ov, nullptr))
                break;
            HANDLE waits[2] = {stop, ov.hEvent};
            const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0 + 1) {
                CancelIoEx(dir, &ov);
                GetOverlappedResult(dir, &ov, &got, TRUE);
                break;
            }
            if (!sh->alive) break;
            RunOnUi([this, sh] {
                if (sh->alive) ChangedOnDisk();
            });
        }
        CloseHandle(ov.hEvent);
        CloseHandle(dir);
    });
}

void Library::ChangedOnDisk() {
    if (diskTimer_) {
        KillTimer(nullptr, diskTimer_);
        Timers().erase(diskTimer_);
    }
    diskTimer_ = SetTimer(nullptr, 0, 300, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        auto it = Timers().find(id);
        if (it == Timers().end()) return;
        Library* self = it->second;
        Timers().erase(it);
        self->diskTimer_ = 0;
        self->Refresh();
    });
    if (diskTimer_) Timers()[diskTimer_] = this;
}

// ---- indexing ----

void Library::IndexInBackground() {
    if (indexing_ || !persists_) return;
    struct Todo {
        std::wstring path;
        double mtime;
        bool ocr;
    };
    std::vector<Todo> todo;
    for (const auto& p : paths_) {
        auto it = meta_.find(p);
        if (it == meta_.end() || it->second.indexed >= kIndexVersion) continue;
        // Text survives only for legacy-imported OCR of an unchanged file.
        todo.push_back({p, it->second.mtime, !it->second.text.has_value()});
    }
    if (todo.empty()) return;
    indexing_ = true;
    progress_ = {0, (int)todo.size()};
    Changed();
    auto sh = shared_;
    std::thread([this, sh, todo = std::move(todo)] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // CPU and disk: stay out of the way
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const int n = (int)todo.size();
        for (int i = 0; i < n && sh->alive; ++i) {
            IndexResult r = indexer::Index(todo[i].path, todo[i].ocr);
            RunOnUi([this, sh, i, n, path = todo[i].path, mtime = todo[i].mtime, r = std::move(r)]() mutable {
                if (!sh->alive) return;
                ApplyIndexResult(path, mtime, std::move(r));
                progress_ = {i + 1, n};
                if ((i + 1) % 20 == 0 || i == n - 1) Save();
                Changed();
            });
        }
        if (SUCCEEDED(co)) CoUninitialize();
        RunOnUi([this, sh] {
            if (!sh->alive) return;
            indexing_ = false;
            progress_ = {0, 0};
            Changed();
            IndexInBackground();  // files that arrived meanwhile
        });
    }).detach();
}

void Library::ApplyIndexResult(const std::wstring& path, double mtime, IndexResult r) {
    auto it = meta_.find(path);
    // Drop results for files renamed, deleted or changed while they were being indexed.
    if (it == meta_.end() || it->second.mtime != mtime) return;
    ItemMeta& e = it->second;
    e.w = r.w;
    e.h = r.h;
    e.duration = r.duration;
    e.colors = r.colors;
    e.dhash = r.dhash;
    if (r.text) e.text = r.text;
    else if (!e.text) e.text = L"";
    e.indexed = kIndexVersion;
    if (!r.feature.empty()) features_[path] = std::move(r.feature);
    else features_.erase(path);
    featuresDirty_ = true;
    ++techVersion_;
    if (autoTag_) {
        std::vector<std::wstring> one{path};
        ApplySuggestions(&one);
    }
}

// "Tag captures automatically": turning it on tags the whole library once, then every new capture.
void Library::SetAutoTag(bool on) {
    if (on && !autoTag_ && loaded_) {
        autoTag_ = true;
        ApplySuggestions();
    }
    autoTag_ = on;
}

// ---- mutations ----

void Library::Edit(const std::vector<std::wstring>& paths, const std::function<void(ItemMeta&)>& body) {
    LoadIfNeeded();
    for (const auto& p : paths) body(meta_[p]);
    Save();
    Changed();
}

void Library::AddTags(const std::vector<std::wstring>& tags, const std::vector<std::wstring>& paths) {
    Edit(paths, [&](ItemMeta& e) {
        auto all = e.tags;
        all.insert(all.end(), tags.begin(), tags.end());
        e.tags = NormalizeTags(all);
    });
}

void Library::RemoveTag(const std::wstring& tag, const std::vector<std::wstring>& paths) {
    Edit(paths, [&](ItemMeta& e) {
        e.tags.erase(std::remove_if(e.tags.begin(), e.tags.end(), [&](const std::wstring& t) { return SameTag(t, tag); }), e.tags.end());
    });
}

void Library::DismissSuggestion(const std::wstring& tag, const std::vector<std::wstring>& paths) {
    Edit(paths, [&](ItemMeta& e) {
        auto all = e.dismissed;
        all.push_back(tag);
        e.dismissed = NormalizeTags(all);
    });
}

void Library::ApplySuggestions(const std::vector<std::wstring>* paths) {
    LoadIfNeeded();
    bool changed = false;
    for (const auto& p : paths ? *paths : paths_) {
        auto it = meta_.find(p);
        if (it == meta_.end()) continue;
        auto add = autotag::Pending(p, it->second);
        if (add.empty()) continue;
        auto all = it->second.tags;
        all.insert(all.end(), add.begin(), add.end());
        it->second.tags = NormalizeTags(all);
        changed = true;
    }
    if (!changed) return;
    Save();
    Changed();
}

void Library::SetRating(int r, const std::vector<std::wstring>& paths) {
    Edit(paths, [&](ItemMeta& e) { e.rating = std::clamp(r, 0, 5); });
}

void Library::SetComment(const std::wstring& c, const std::wstring& path) {
    Edit({path}, [&](ItemMeta& e) { e.comment = c; });
}

void Library::RenameTag(const std::wstring& from, const std::wstring& to) {
    std::vector<std::wstring> ps;
    for (const auto& p : paths_) {
        const auto& t = Meta(p).tags;
        if (std::any_of(t.begin(), t.end(), [&](const std::wstring& x) { return SameTag(x, from); })) ps.push_back(p);
    }
    for (auto& sf : smartFolders_)
        for (auto& t : sf.filter.tags)
            if (SameTag(t, from)) t = to;
    Edit(ps, [&](ItemMeta& e) {
        for (auto& t : e.tags)
            if (SameTag(t, from)) t = to;
        e.tags = NormalizeTags(e.tags);
    });
}

void Library::DeleteTag(const std::wstring& tag) { RemoveTag(tag, paths_); }

LibCollection Library::CreateCollection(const std::wstring& name) {
    LoadIfNeeded();
    LibCollection c{NewUuid(), Trim(name), {}};
    collections_.push_back(c);
    Save();
    Changed();
    return c;
}

void Library::RenameCollection(const std::wstring& id, const std::wstring& name) {
    for (auto& c : collections_)
        if (c.id == id) c.name = name;
    Save();
    Changed();
}

void Library::SetAutoTags(const std::wstring& id, const std::vector<std::wstring>& tags) {
    for (auto& c : collections_)
        if (c.id == id) c.autoTags = NormalizeTags(tags);
    Save();
    Changed();
}

void Library::DeleteCollection(const std::wstring& id) {
    collections_.erase(std::remove_if(collections_.begin(), collections_.end(), [&](const LibCollection& c) { return c.id == id; }),
                       collections_.end());
    Edit(paths_, [&](ItemMeta& e) { e.collections.erase(std::remove(e.collections.begin(), e.collections.end(), id), e.collections.end()); });
}

void Library::AddToCollection(const std::vector<std::wstring>& paths, const std::wstring& id) {
    const LibCollection* c = FindCollection(id);
    const std::vector<std::wstring> autoTags = c ? c->autoTags : std::vector<std::wstring>{};
    Edit(paths, [&](ItemMeta& e) {
        if (std::find(e.collections.begin(), e.collections.end(), id) == e.collections.end()) e.collections.push_back(id);
        auto all = e.tags;
        all.insert(all.end(), autoTags.begin(), autoTags.end());
        e.tags = NormalizeTags(all);
    });
}

void Library::RemoveFromCollection(const std::vector<std::wstring>& paths, const std::wstring& id) {
    Edit(paths, [&](ItemMeta& e) { e.collections.erase(std::remove(e.collections.begin(), e.collections.end(), id), e.collections.end()); });
}

SmartFolder Library::SaveSmartFolder(const std::wstring& name, const Filter& f) {
    LoadIfNeeded();
    SmartFolder s{NewUuid(), name, f};
    smartFolders_.push_back(s);
    Save();
    Changed();
    return s;
}

void Library::UpdateSmartFolder(const std::wstring& id, const Filter* f, const std::wstring* name) {
    for (auto& s : smartFolders_)
        if (s.id == id) {
            if (f) s.filter = *f;
            if (name) s.name = *name;
        }
    Save();
    Changed();
}

void Library::DeleteSmartFolder(const std::wstring& id) {
    smartFolders_.erase(std::remove_if(smartFolders_.begin(), smartFolders_.end(), [&](const SmartFolder& s) { return s.id == id; }),
                        smartFolders_.end());
    Save();
    Changed();
}

void Library::Moved(const std::wstring& from, const std::wstring& to) {
    if (from == to) return;
    LoadIfNeeded();
    ++generation_;
    if (auto it = meta_.find(from); it != meta_.end()) {
        ItemMeta e = std::move(it->second);
        meta_.erase(it);
        meta_[to] = std::move(e);
    }
    if (auto it = features_.find(from); it != features_.end()) {
        auto f = std::move(it->second);
        features_.erase(it);
        features_[to] = std::move(f);
        featuresDirty_ = true;
    }
    for (auto& p : paths_)
        if (p == from) p = to;
    ++techVersion_;
    if (onMoved) onMoved(from, to);
    Save();
    Changed();
}

void Library::Removed(const std::vector<std::wstring>& paths) {
    LoadIfNeeded();
    ++generation_;
    featuresDirty_ = true;
    ++techVersion_;
    std::unordered_set<std::wstring> gone(paths.begin(), paths.end());
    for (const auto& p : paths) {
        meta_.erase(p);
        features_.erase(p);
    }
    paths_.erase(std::remove_if(paths_.begin(), paths_.end(), [&](const std::wstring& p) { return gone.count(p) != 0; }), paths_.end());
    if (onRemoved) onRemoved(paths);
    Save();
    Changed();
}

std::vector<std::wstring> Library::ImportFiles(const std::vector<std::wstring>& files, const std::wstring& collection) {
    const std::wstring dir = folder_ + L"\\Imported";
    CreateDirectoryW(folder_.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<std::wstring> out;
    for (const auto& f : files) {
        if (!IsMediaFile(f)) continue;
        if (IsInLibrary(f)) {  // already in the library
            out.push_back(f);
            continue;
        }
        const std::wstring name = FileNameOf(f);
        const size_t dot = name.find_last_of(L'.');
        const std::wstring stem = name.substr(0, dot), ext = name.substr(dot);
        std::wstring dst = dir + L"\\" + name;
        for (int i = 2; GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000; ++i)
            dst = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
        if (CopyFileW(f.c_str(), dst.c_str(), TRUE)) out.push_back(dst);
    }
    Refresh([this, out, collection] {
        if (!collection.empty() && !out.empty()) AddToCollection(out, collection);
    });
    return out;
}

// ---- queries ----

std::vector<std::pair<std::wstring, int>> Library::AllTags() const {
    std::map<std::wstring, std::pair<std::wstring, int>> counts;
    for (const auto& [p, e] : meta_)
        for (const auto& t : e.tags) {
            auto& c = counts[LowerText(t)];
            if (c.first.empty()) c.first = t;
            ++c.second;
        }
    std::vector<std::pair<std::wstring, int>> out;
    for (auto& [k, v] : counts) out.push_back(v);
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : LowerText(a.first) < LowerText(b.first);
    });
    return out;
}

std::vector<std::pair<std::wstring, int>> Library::AllApps() const {
    std::map<std::wstring, int> c;
    for (const auto& p : paths_) {
        const std::wstring& a = Meta(p).app;
        if (!a.empty()) ++c[a];
    }
    std::vector<std::pair<std::wstring, int>> out(c.begin(), c.end());
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

int Library::CountIn(const std::wstring& id) const {
    int n = 0;
    for (const auto& p : paths_) {
        const auto& cs = Meta(p).collections;
        if (std::find(cs.begin(), cs.end(), id) != cs.end()) ++n;
    }
    return n;
}

std::vector<std::vector<std::wstring>> Library::DuplicateGroups(int maxDistance, float maxFeatureDistance) {
    if (dupCache_.version == techVersion_ && dupCache_.count == paths_.size()) return dupCache_.groups;
    struct It {
        const std::wstring* path;
        uint64_t hash;
    };
    std::vector<It> items;
    for (const auto& p : paths_)
        if (auto h = Meta(p).dhash) items.push_back({&p, *h});
    std::vector<size_t> parent(items.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    for (size_t i = 0; i < items.size(); ++i)
        for (size_t j = i + 1; j < items.size(); ++j) {
            const int bits = std::popcount(items[i].hash ^ items[j].hash);
            if (bits > maxDistance) continue;
            auto fi = features_.find(*items[i].path), fj = features_.find(*items[j].path);
            if (fi != features_.end() && fj != features_.end()) {
                if (indexer::FeatureDistance(fi->second, fj->second) > maxFeatureDistance) continue;
            } else if (bits > 3) {
                continue;  // hash alone: be strict
            }
            const size_t a = find(i), b = find(j);
            if (a != b) parent[b] = a;
        }
    std::map<size_t, std::vector<std::wstring>> groups;
    for (size_t i = 0; i < items.size(); ++i) groups[find(i)].push_back(*items[i].path);
    std::vector<std::vector<std::wstring>> out;
    for (auto& [k, g] : groups) {
        if (g.size() < 2) continue;
        std::stable_sort(g.begin(), g.end(), [&](const std::wstring& a, const std::wstring& b) { return Meta(a).mtime > Meta(b).mtime; });
        out.push_back(std::move(g));
    }
    std::stable_sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        return a.size() != b.size() ? a.size() > b.size() : Meta(a[0]).mtime > Meta(b[0]).mtime;
    });
    dupCache_ = {techVersion_, paths_.size(), out};
    return out;
}

std::vector<std::pair<std::wstring, float>> Library::Similar(const std::wstring& path, size_t limit) {
    if (simCache_.path == path && simCache_.version == techVersion_ && simCache_.count == paths_.size()) return simCache_.result;
    std::vector<std::pair<std::wstring, float>> out;
    auto me = features_.find(path);
    if (me != features_.end())
        for (const auto& p : paths_) {
            if (p == path) continue;
            auto f = features_.find(p);
            if (f != features_.end()) out.push_back({p, indexer::FeatureDistance(me->second, f->second)});
        }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (out.size() > limit) out.resize(limit);
    simCache_ = {path, techVersion_, paths_.size(), out};
    return out;
}

int Library::SimilarityPercent(float d) { return (int)std::lround(std::clamp(100.0 * (1.0 - d / 1.25), 0.0, 100.0)); }

// ---- test hooks ----

void Library::TestSetHash(const std::wstring& p, uint64_t h) {
    meta_[p].dhash = h;
    ++techVersion_;
}
void Library::TestSetFeature(const std::wstring& p, std::vector<float> f) {
    features_[p] = std::move(f);
    ++techVersion_;
}
void Library::TestSetText(const std::wstring& p, const std::wstring& t, int indexed) {
    meta_[p].text = t;
    meta_[p].indexed = indexed;
}
void Library::TestSetApp(const std::wstring& p, const std::wstring& a) { meta_[p].app = a; }

}  // namespace ather
