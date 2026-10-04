#include "editor.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <set>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "annot.h"
#include "collage.h"
#include "library.h"
#include "media.h"
#include "ocr.h"
#include "output.h"
#include "palette.h"
#include "pin.h"
#include "selftest.h"
#include "toast.h"

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotEditor";
constexpr int kToolbarH = 52, kStatusH = 26, kMargin = 16, kBtn = 34, kBarH = 40;

enum class Tool { Select, Arrow, Line, Rect, Ellipse, Pen, Highlight, Text, Step, Blur, Pixelate, Spotlight, Magnify, Crop, Canvas, Image };

// Tools applied to the pixels underneath the vector annotations.
bool IsPixelTool(Tool t) { return t == Tool::Blur || t == Tool::Pixelate || t == Tool::Magnify; }
// Changes to these redraw the background raster (image layers sit under blur and pixelate).
bool IsRasterTool(Tool t) { return IsPixelTool(t) || t == Tool::Image; }
// Tools defined by a dragged rectangle.
bool IsRectTool(Tool t) {
    return t == Tool::Rect || t == Tool::Ellipse || t == Tool::Highlight || t == Tool::Blur || t == Tool::Pixelate ||
           t == Tool::Spotlight || t == Tool::Crop;
}

struct ToolInfo {
    Tool tool;
    const wchar_t* name;
    wchar_t key;
    const wchar_t* hint;
};

const ToolInfo kTools[] = {
    {Tool::Select, L"Select", 'V', L"Click to select, drag to move, Delete removes, Ctrl+D duplicates"},
    {Tool::Arrow, L"Arrow", 'A', L"Drag to draw  ·  Shift snaps to 45°"},
    {Tool::Line, L"Line", 'L', L"Drag to draw  ·  Shift snaps to 45°"},
    {Tool::Rect, L"Rectangle", 'R', L"Drag to draw  ·  Shift for a square"},
    {Tool::Ellipse, L"Ellipse", 'E', L"Drag to draw  ·  Shift for a circle"},
    {Tool::Pen, L"Pen", 'P', L"Draw freehand"},
    {Tool::Highlight, L"Highlighter", 'H', L"Drag over text to highlight it"},
    {Tool::Text, L"Text", 'T', L"Click to type  ·  Enter to finish  ·  Shift+Enter for a new line"},
    {Tool::Step, L"Step number", 'N', L"Click to drop numbered markers 1, 2, 3…"},
    {Tool::Blur, L"Blur", 'B', L"Drag over an area to blur it  ·  size sets the strength"},
    {Tool::Pixelate, L"Pixelate", 'X', L"Drag over sensitive information  ·  Ctrl+R auto-redacts"},
    {Tool::Spotlight, L"Spotlight", 'S', L"Drag to keep an area bright and dim everything else"},
    {Tool::Magnify, L"Magnifier", 'M', L"Drag from the detail to where the zoomed bubble should go"},
    {Tool::Crop, L"Crop", 'C', L"Drag the area to keep"},
    {Tool::Canvas, L"Canvas", 'K', L"Drag an edge to add space  ·  Alt drags both sides"},
    {Tool::Image, L"Insert image", 'I', L"Drop, paste or pick a screenshot  ·  drag a corner to resize, Shift for free resize"},
};

const COLORREF kColors[] = {RGB(255, 59, 48),  RGB(255, 149, 0),  RGB(255, 204, 0),   RGB(52, 199, 89),
                            RGB(10, 132, 255), theme::kAccent, RGB(255, 255, 255), RGB(24, 24, 24)};
const wchar_t* const kColorNames[] = {L"Red", L"Orange", L"Yellow", L"Green", L"Blue", L"Ather lime", L"White", L"Black"};
constexpr int kLevels = 5;
const float kStroke[kLevels] = {2, 3, 5, 8, 12};
const float kTextPx[kLevels] = {16, 22, 30, 42, 58};
const float kStepR[kLevels] = {11, 14, 18, 23, 30};
const float kBlurR[kLevels] = {3, 6, 10, 15, 22};
const float kMagR[kLevels] = {36, 48, 64, 84, 110};  // magnifier bubble radius (zoom is 2x)

enum Action {
    ActUndo, ActRedo, ActCopy, ActSave, ActSaveAs, ActPin, ActDone, ActClose,
    ActStyle, ActRedact, ActCopyAnnots, ActPasteAnnots, ActDuplicate,
    ActInsertImage, ActSpaceBelow, ActSpaceRight, ActEvenMargin, ActResetFrame, ActFront, ActBack
};

// A screenshot placed on the canvas. Always flattened on export.
struct ImageLayer {
    BitmapPtr image;
    std::wstring source;
    float radius = 0;
    bool shadow = false, border = false;
    float opacity = 1;
    int slot = -1;  // index into the collage's images when part of a collage layout
};

struct Annot {
    Tool type = Tool::Arrow;
    COLORREF color = 0;
    int level = 1;
    float unit = 1;  // display scale when created, so strokes look the same on high-DPI captures
    std::vector<gp::PointF> pts;
    std::wstring text;
    int step = 0;
    float wrap = 0;                            // text: wrap width (0 = none), so notes stay on the canvas
    std::shared_ptr<const ImageLayer> layer;   // Tool::Image
};

// Everything that undo restores. `crop` is the document frame in image coordinates: inside the image it crops,
// beyond the image it adds space, painted with `fill` (none = transparent).
struct DocState {
    std::vector<Annot> annots;
    int nextStep = 1;
    std::optional<gp::RectF> crop;
    std::optional<COLORREF> fill;
};

struct Button {
    enum Kind { KTool, KColor, KWidth, KAction } kind;
    int value;
    RECT r;
    std::wstring tip;
};

EditorOptions g_defaults;
std::vector<Annot> g_annotClipboard;  // shared between editor windows (Ctrl+Shift+C / Ctrl+Shift+V)

// ---- drawing helpers (image coordinates) ----

gp::Color GC(COLORREF c, BYTE a = 255) { return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

gp::RectF NormRect(gp::PointF a, gp::PointF b) {
    return gp::RectF(std::min(a.X, b.X), std::min(a.Y, b.Y), std::fabs(b.X - a.X), std::fabs(b.Y - a.Y));
}

float StrokeW(const Annot& a) { return kStroke[a.level] * a.unit; }

bool IsDark(COLORREF c) { return GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114 < 150000; }

std::unique_ptr<gp::Font> TextFont(const Annot& a) {
    return std::make_unique<gp::Font>(L"Segoe UI", kTextPx[a.level] * a.unit, gp::FontStyleBold, gp::UnitPixel);
}

const gp::StringFormat* TypoFormat() {
    static gp::StringFormat* f = [] {
        auto* sf = gp::StringFormat::GenericTypographic()->Clone();
        sf->SetFormatFlags(sf->GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
        return sf;
    }();
    return f;
}

gp::Graphics& MeasureGraphics() {
    static gp::Bitmap* bmp = new gp::Bitmap(1, 1, PixelFormat32bppARGB);
    static gp::Graphics* g = new gp::Graphics(bmp);
    return *g;
}

gp::RectF MeasureText(const Annot& a, const std::wstring& text) {
    auto font = TextFont(a);
    gp::RectF r;
    const std::wstring& t = text.empty() ? std::wstring(L"Ag") : text;
    if (a.wrap > 0)
        MeasureGraphics().MeasureString(t.c_str(), (INT)t.size(), font.get(), gp::RectF(a.pts[0].X, a.pts[0].Y, a.wrap, 1e6f), TypoFormat(), &r);
    else
        MeasureGraphics().MeasureString(t.c_str(), (INT)t.size(), font.get(), a.pts[0], TypoFormat(), &r);
    r.X = a.pts[0].X;
    r.Y = a.pts[0].Y;
    if (text.empty()) r.Width = 0;
    return r;
}

gp::RectF Bounds(const Annot& a) {
    gp::RectF r;
    switch (a.type) {
        case Tool::Text: r = MeasureText(a, a.text); break;
        case Tool::Image: return NormRect(a.pts[0], a.pts.back());
        case Tool::Step: {
            float rad = kStepR[a.level] * a.unit;
            r = gp::RectF(a.pts[0].X - rad, a.pts[0].Y - rad, 2 * rad, 2 * rad);
            break;
        }
        case Tool::Pen: {
            float x0 = a.pts[0].X, y0 = a.pts[0].Y, x1 = x0, y1 = y0;
            for (const auto& p : a.pts) {
                x0 = std::min(x0, p.X);
                y0 = std::min(y0, p.Y);
                x1 = std::max(x1, p.X);
                y1 = std::max(y1, p.Y);
            }
            r = gp::RectF(x0, y0, x1 - x0, y1 - y0);
            break;
        }
        case Tool::Magnify: {
            const float R = kMagR[a.level] * a.unit;
            gp::RectF src(a.pts[0].X - R / 2, a.pts[0].Y - R / 2, R, R);
            gp::RectF dst(a.pts.back().X - R, a.pts.back().Y - R, 2 * R, 2 * R);
            gp::RectF::Union(r, src, dst);
            break;
        }
        default: r = NormRect(a.pts[0], a.pts.back());
    }
    float pad = StrokeW(a) / 2 + 4 * a.unit;
    r.Inflate(pad, pad);
    return r;
}

RECT ClampRect(const gp::RectF& r, const Bitmap& b) {
    return {std::clamp((LONG)std::floor(r.X), 0L, (LONG)b.Width()), std::clamp((LONG)std::floor(r.Y), 0L, (LONG)b.Height()),
            std::clamp((LONG)std::ceil(r.GetRight()), 0L, (LONG)b.Width()),
            std::clamp((LONG)std::ceil(r.GetBottom()), 0L, (LONG)b.Height())};
}

// Three box-blur passes ≈ Gaussian, on all four (premultiplied) channels. Samples outside the area are clamped
// to its edge so nothing outside the selection bleeds in or gets modified.
void GaussBlur(Bitmap& b, const gp::RectF& area, int radius) {
    const RECT rc = ClampRect(area, b);
    const int w = RectW(rc), h = RectH(rc);
    if (w <= 1 || h <= 1 || radius < 1) return;
    std::vector<float> ch[4];
    for (auto& c : ch) c.resize((size_t)w * h);
    uint32_t* px = b.Bits();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t p = px[(size_t)(rc.top + y) * b.Width() + rc.left + x];
            for (int k = 0; k < 4; ++k) ch[k][(size_t)y * w + x] = (float)((p >> (k * 8)) & 255);
        }
    std::vector<float> tmp((size_t)std::max(w, h));
    auto pass1d = [&](float* data, int n, int stride) {
        const int r = std::min(radius, n - 1);
        const float inv = 1.f / (2 * r + 1);
        float sum = data[0] * (r + 1);
        for (int i = 1; i <= r; ++i) sum += data[(size_t)i * stride];
        for (int i = 0; i < n; ++i) {
            tmp[i] = sum * inv;
            const int add = std::min(i + r + 1, n - 1), sub = std::max(i - r, 0);
            sum += data[(size_t)add * stride] - data[(size_t)sub * stride];
        }
        for (int i = 0; i < n; ++i) data[(size_t)i * stride] = tmp[i];
    };
    for (auto& c : ch)
        for (int pass = 0; pass < 3; ++pass) {
            for (int y = 0; y < h; ++y) pass1d(c.data() + (size_t)y * w, w, 1);
            for (int x = 0; x < w; ++x) pass1d(c.data() + x, h, w);
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            uint32_t out = 0;
            for (int k = 0; k < 4; ++k) out |= (uint32_t)std::clamp((int)std::lround(ch[k][i]), 0, 255) << (k * 8);
            px[(size_t)(rc.top + y) * b.Width() + rc.left + x] = out;
        }
}

// Copies a 2x-zoomed circle around `src` into a circle of radius R around `dst` (bilinear).
void Magnify(Bitmap& b, gp::PointF src, gp::PointF dst, float R) {
    const int W = b.Width(), H = b.Height();
    std::vector<uint32_t> snap(b.Bits(), b.Bits() + (size_t)W * H);  // read from the pre-bubble image
    auto at = [&](int x, int y) { return snap[(size_t)std::clamp(y, 0, H - 1) * W + std::clamp(x, 0, W - 1)]; };
    const int x0 = std::max(0, (int)(dst.X - R)), x1 = std::min(W, (int)std::ceil(dst.X + R));
    const int y0 = std::max(0, (int)(dst.Y - R)), y1 = std::min(H, (int)std::ceil(dst.Y + R));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - dst.X, dy = y + 0.5f - dst.Y;
            if (dx * dx + dy * dy > R * R) continue;
            const float sx = src.X + dx / 2 - 0.5f, sy = src.Y + dy / 2 - 0.5f;
            const int ix = (int)std::floor(sx), iy = (int)std::floor(sy);
            const float fx = sx - ix, fy = sy - iy;
            uint32_t q[4] = {at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1)};
            uint32_t out = 0;
            for (int sh = 0; sh <= 24; sh += 8) {
                float v = ((q[0] >> sh) & 255) * (1 - fx) * (1 - fy) + ((q[1] >> sh) & 255) * fx * (1 - fy) +
                          ((q[2] >> sh) & 255) * (1 - fx) * fy + ((q[3] >> sh) & 255) * fx * fy;
                out |= (uint32_t)std::lround(v) << sh;
            }
            b.Bits()[(size_t)y * W + x] = out;
        }
}

void Pixelate(Bitmap& b, const gp::RectF& r, int block) {
    int x0 = std::clamp((int)std::floor(r.X), 0, b.Width()), y0 = std::clamp((int)std::floor(r.Y), 0, b.Height());
    int x1 = std::clamp((int)std::ceil(r.GetRight()), 0, b.Width()),
        y1 = std::clamp((int)std::ceil(r.GetBottom()), 0, b.Height());
    uint32_t* px = b.Bits();
    const int stride = b.Width();
    for (int by = y0; by < y1; by += block) {
        for (int bx = x0; bx < x1; bx += block) {
            int ex = std::min(bx + block, x1), ey = std::min(by + block, y1);
            uint64_t s[4] = {}, n = 0;
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) {
                    const uint32_t p = px[(size_t)y * stride + x];
                    for (int k = 0; k < 4; ++k) s[k] += (p >> (k * 8)) & 255;
                    ++n;
                }
            uint32_t avg = 0;
            for (int k = 0; k < 4; ++k) avg |= (uint32_t)(s[k] / n) << (k * 8);
            for (int y = by; y < ey; ++y) std::fill_n(px + (size_t)y * stride + bx, ex - bx, avg);
        }
    }
}

void RoundedPath(gp::GraphicsPath& p, const gp::RectF& r, float rad) {
    rad = std::max(0.f, std::min({rad, r.Width / 2, r.Height / 2}));
    if (rad < 0.5f) {
        p.AddRectangle(r);
        return;
    }
    p.AddArc(r.X, r.Y, 2 * rad, 2 * rad, 180, 90);
    p.AddArc(r.GetRight() - 2 * rad, r.Y, 2 * rad, 2 * rad, 270, 90);
    p.AddArc(r.GetRight() - 2 * rad, r.GetBottom() - 2 * rad, 2 * rad, 2 * rad, 0, 90);
    p.AddArc(r.X, r.GetBottom() - 2 * rad, 2 * rad, 2 * rad, 90, 90);
    p.CloseFigure();
}

void DrawArrow(gp::Graphics& g, const Annot& a) {
    gp::PointF p0 = a.pts[0], p1 = a.pts.back();
    float dx = p1.X - p0.X, dy = p1.Y - p0.Y, len = std::hypot(dx, dy);
    if (len < 1) return;
    float w = StrokeW(a), ux = dx / len, uy = dy / len;
    float head = std::min(std::max(w * 3.6f, 12 * a.unit), len * 0.7f);
    gp::PointF base(p1.X - ux * head, p1.Y - uy * head);
    float hw = head * 0.58f;
    gp::PointF tri[3] = {p1, {base.X - uy * hw, base.Y + ux * hw}, {base.X + uy * hw, base.Y - ux * hw}};
    gp::Pen pen(GC(a.color), w);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapFlat);
    g.DrawLine(&pen, p0, gp::PointF(base.X + ux * 1, base.Y + uy * 1));
    gp::SolidBrush br(GC(a.color));
    g.FillPolygon(&br, tri, 3);
}

// Blur / pixelate / spotlight are applied to pixels; here they only draw a live preview. For the magnifier
// this draws the frame (rings + connector) around the zoomed bubble.
void DrawAnnot(gp::Graphics& g, const Annot& a) {
    const gp::Color c = GC(a.color);
    gp::Pen pen(c, StrokeW(a));
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    switch (a.type) {
        case Tool::Arrow: DrawArrow(g, a); break;
        case Tool::Line: g.DrawLine(&pen, a.pts[0], a.pts.back()); break;
        case Tool::Rect: g.DrawRectangle(&pen, NormRect(a.pts[0], a.pts.back())); break;
        case Tool::Ellipse: g.DrawEllipse(&pen, NormRect(a.pts[0], a.pts.back())); break;
        case Tool::Pen:
            if (a.pts.size() == 1) {
                gp::SolidBrush br(c);
                float r = StrokeW(a) / 2;
                g.FillEllipse(&br, a.pts[0].X - r, a.pts[0].Y - r, 2 * r, 2 * r);
            } else {
                g.DrawLines(&pen, a.pts.data(), (INT)a.pts.size());
            }
            break;
        case Tool::Highlight: {
            gp::SolidBrush br(GC(a.color, 90));
            g.FillRectangle(&br, NormRect(a.pts[0], a.pts.back()));
            break;
        }
        case Tool::Text: {
            auto font = TextFont(a);
            float off = std::max(1.f, a.unit * 1.2f);
            gp::SolidBrush shadow(IsDark(a.color) ? gp::Color(150, 255, 255, 255) : gp::Color(150, 0, 0, 0));
            gp::SolidBrush br(c);
            if (a.wrap > 0) {  // wraps at the canvas edge
                g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), gp::RectF(a.pts[0].X + off, a.pts[0].Y + off, a.wrap, 1e6f), TypoFormat(), &shadow);
                g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), gp::RectF(a.pts[0].X, a.pts[0].Y, a.wrap, 1e6f), TypoFormat(), &br);
            } else {
                g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), gp::PointF(a.pts[0].X + off, a.pts[0].Y + off), TypoFormat(), &shadow);
                g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), a.pts[0], TypoFormat(), &br);
            }
            break;
        }
        case Tool::Step: {
            float r = kStepR[a.level] * a.unit;
            gp::RectF rc(a.pts[0].X - r, a.pts[0].Y - r, 2 * r, 2 * r);
            gp::SolidBrush br(c);
            g.FillEllipse(&br, rc);
            gp::Pen ring(gp::Color(230, 255, 255, 255), std::max(1.f, r * 0.12f));
            g.DrawEllipse(&ring, rc);
            gp::Font font(L"Segoe UI", r * 1.05f, gp::FontStyleBold, gp::UnitPixel);
            gp::StringFormat sf;
            sf.SetAlignment(gp::StringAlignmentCenter);
            sf.SetLineAlignment(gp::StringAlignmentCenter);
            gp::SolidBrush tb(IsDark(a.color) || GetGValue(a.color) < 200 ? gp::Color(255, 255, 255, 255) : gp::Color(255, 20, 20, 20));
            std::wstring n = std::to_wstring(a.step);
            rc.Y += r * 0.04f;
            g.DrawString(n.c_str(), (INT)n.size(), &font, rc, &sf, &tb);
            break;
        }
        case Tool::Blur:
        case Tool::Pixelate:
        case Tool::Spotlight: {  // live preview only
            gp::RectF rc = NormRect(a.pts[0], a.pts.back());
            if (a.type == Tool::Spotlight) {
                gp::SolidBrush lit(gp::Color(40, 255, 255, 255));
                g.FillRectangle(&lit, rc);
            } else {
                gp::HatchBrush hb(gp::HatchStyleLargeCheckerBoard, gp::Color(70, 255, 255, 255), gp::Color(70, 0, 0, 0));
                g.FillRectangle(&hb, rc);
            }
            gp::Pen dash(gp::Color(220, 255, 255, 255), a.unit);
            dash.SetDashStyle(gp::DashStyleDash);
            g.DrawRectangle(&dash, rc);
            break;
        }
        case Tool::Magnify: {
            const float R = kMagR[a.level] * a.unit;
            const gp::PointF s = a.pts[0], d = a.pts.back();
            gp::Pen ring(c, std::max(2.f, 3 * a.unit));
            gp::Pen thin(c, std::max(1.5f, 2 * a.unit));
            const float dx = d.X - s.X, dy = d.Y - s.Y, len = std::hypot(dx, dy);
            if (len > R * 1.5f) {  // connector from the source ring to the bubble edge
                const float ux = dx / len, uy = dy / len;
                g.DrawLine(&thin, s.X + ux * R / 2, s.Y + uy * R / 2, d.X - ux * R, d.Y - uy * R);
            }
            g.DrawEllipse(&thin, s.X - R / 2, s.Y - R / 2, R, R);
            g.DrawEllipse(&ring, d.X - R, d.Y - R, 2 * R, 2 * R);
            break;
        }
        default: break;
    }
}

// ---- the render pipeline: fill → base image → image layers → blur, pixelate, magnify → spotlight → vector ----

gp::RectF Extent(const Bitmap& base) { return gp::RectF(0, 0, (float)base.Width(), (float)base.Height()); }

bool HasLayers(const DocState& st) {
    return std::any_of(st.annots.begin(), st.annots.end(), [](const Annot& a) { return a.type == Tool::Image && a.layer; });
}

// The document frame (integral). A crop that misses the image entirely falls back to the whole image, unless
// image layers give the canvas something to show (a collage).
RECT FrameOf(const Bitmap& base, const DocState& st) {
    const RECT extent{0, 0, base.Width(), base.Height()};
    if (!st.crop) return extent;
    const gp::RectF c = *st.crop;
    RECT f{(LONG)std::floor(c.X + 0.001f), (LONG)std::floor(c.Y + 0.001f), (LONG)std::ceil(c.GetRight() - 0.001f), (LONG)std::ceil(c.GetBottom() - 0.001f)};
    if (RectW(f) < 1 || RectH(f) < 1) return extent;
    RECT inter;
    if (!IntersectRect(&inter, &f, &extent) && !HasLayers(st)) return extent;
    return f;
}

// A soft drop shadow: a blurred rounded rectangle, black at `alpha`, drawn under the layer.
void DrawShadow(gp::Graphics& g, const gp::RectF& r, float rad, float blur, float dy, BYTE alpha) {
    const int m = (int)std::ceil(blur * 2) + 2;
    const int w = (int)std::ceil(r.Width) + 2 * m, h = (int)std::ceil(r.Height) + 2 * m;
    if (w <= 0 || h <= 0 || (size_t)w * h > 60'000'000) return;
    auto mask = Bitmap::Create(w, h);
    if (!mask) return;
    std::fill_n(mask->Bits(), (size_t)w * h, 0u);
    {
        gp::Bitmap gb(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(mask->Bits()));
        gp::Graphics mg(&gb);
        mg.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        gp::GraphicsPath p;
        RoundedPath(p, gp::RectF((float)m, (float)m, r.Width, r.Height), rad);
        gp::SolidBrush b(gp::Color(alpha, 0, 0, 0));
        mg.FillPath(&b, &p);
    }
    GaussBlur(*mask, gp::RectF(0, 0, (float)w, (float)h), std::max(1, (int)std::lround(blur / 2)));
    gp::Bitmap src(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(mask->Bits()));
    g.DrawImage(&src, r.X - m, r.Y - m + dy, (float)w, (float)h);
}

void DrawLayer(gp::Graphics& g, const Annot& a) {
    if (!a.layer || !a.layer->image) return;
    const ImageLayer& l = *a.layer;
    const gp::RectF r = NormRect(a.pts[0], a.pts.back());
    if (r.Width < 1 || r.Height < 1) return;
    const float rad = std::min({l.radius, r.Width / 2, r.Height / 2});
    // Everything for this layer goes into its own transparent bitmap first, so opacity applies to the whole.
    const float d = std::max(r.Width, r.Height);
    const float blur = l.shadow ? std::min(30 * a.unit, d * 0.04f) : 0, dy = l.shadow ? std::min(10 * a.unit, d * 0.012f) : 0;
    const int m = (int)std::ceil(blur * 2 + dy + StrokeW(a)) + 4;
    const int w = (int)std::ceil(r.Width) + 2 * m, h = (int)std::ceil(r.Height) + 2 * m;
    if ((size_t)w * h > 120'000'000) return;
    auto tmp = Bitmap::Create(w, h);
    if (!tmp) return;
    std::fill_n(tmp->Bits(), (size_t)w * h, 0u);
    const float ox = r.X - m, oy = r.Y - m;  // tmp's origin in document coordinates
    const gp::RectF local(r.X - ox, r.Y - oy, r.Width, r.Height);
    {
        gp::Bitmap gb(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(tmp->Bits()));
        gp::Graphics lg(&gb);
        lg.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        lg.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        lg.SetInterpolationMode(gp::InterpolationModeHighQualityBicubic);
        if (l.shadow) DrawShadow(lg, local, rad, blur, dy, 115);
        // Shrink big images first (box filter), so the texture brush samples a clean image.
        BitmapPtr img = l.image;
        const int tw = std::max(1, (int)std::lround(r.Width)), th = std::max(1, (int)std::lround(r.Height));
        if (img->Width() > tw * 4 / 3 || img->Height() > th * 4 / 3) img = Resample(*img, tw, th);
        gp::Bitmap src(img->Width(), img->Height(), img->Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img->Bits()));
        gp::TextureBrush tb(&src, gp::WrapModeClamp);
        tb.TranslateTransform(local.X, local.Y);
        tb.ScaleTransform(local.Width / img->Width(), local.Height / img->Height());
        gp::GraphicsPath path;
        RoundedPath(path, local, rad);
        lg.FillPath(&tb, &path);
        if (l.border) {
            gp::Pen pen(GC(a.color), StrokeW(a));
            lg.DrawPath(&pen, &path);
        }
    }
    gp::Bitmap layerBmp(w, h, w * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(tmp->Bits()));
    if (l.opacity < 0.999f) {
        gp::ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, l.opacity, 0}, {0, 0, 0, 0, 1}}};
        gp::ImageAttributes ia;
        ia.SetColorMatrix(&cm);
        g.DrawImage(&layerBmp, gp::RectF(ox, oy, (float)w, (float)h), 0, 0, (float)w, (float)h, gp::UnitPixel, &ia);
    } else {
        g.DrawImage(&layerBmp, ox, oy, (float)w, (float)h);
    }
}

// Everything under the annotations, covering the document frame: fill, the screenshot, image layers.
BitmapPtr Raster(const Bitmap& base, const DocState& st, RECT* frameOut) {
    const RECT f = FrameOf(base, st);
    if (frameOut) *frameOut = f;
    const int W = RectW(f), H = RectH(f);
    auto out = Bitmap::Create(W, H);
    if (!out) return nullptr;
    const uint32_t fill = st.fill ? 0xFF000000u | GetRValue(*st.fill) << 16 | GetGValue(*st.fill) << 8 | GetBValue(*st.fill) : 0u;
    const RECT extent{0, 0, base.Width(), base.Height()};
    RECT inter{};
    const bool overlap = IntersectRect(&inter, &f, &extent) != 0;
    if (!(overlap && EqualRect(&inter, &f)) || (base.Width() == 1 && base.Height() == 1)) std::fill_n(out->Bits(), (size_t)W * H, fill);
    // A collage starts from a blank 1×1 base: nothing to draw under the layers.
    const bool blank = base.Width() == 1 && base.Height() == 1 && (base.Bits()[0] >> 24) == 0;
    if (overlap && !blank)
        for (int y = inter.top; y < inter.bottom; ++y)
            memcpy(out->Bits() + (size_t)(y - f.top) * W + (inter.left - f.left), base.Bits() + (size_t)y * base.Width() + inter.left,
                   (size_t)RectW(inter) * 4);
    if (HasLayers(st)) {
        gp::Bitmap gb(W, H, W * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(out->Bits()));
        gp::Graphics g(&gb);
        g.TranslateTransform((float)-f.left, (float)-f.top);
        for (const auto& a : st.annots)
            if (a.type == Tool::Image) DrawLayer(g, a);
    }
    return out;
}

gp::RectF Offset(const gp::RectF& r, const RECT& f) { return gp::RectF(r.X - f.left, r.Y - f.top, r.Width, r.Height); }
gp::PointF Offset(const gp::PointF& p, const RECT& f) { return gp::PointF(p.X - f.left, p.Y - f.top); }

// The raster with blur, pixelate and magnify applied in order.
BitmapPtr PixelBase(const Bitmap& base, const DocState& st, RECT* frameOut) {
    RECT f;
    auto out = Raster(base, st, &f);
    if (frameOut) *frameOut = f;
    if (!out) return nullptr;
    for (const auto& a : st.annots) {
        if (!IsPixelTool(a.type)) continue;
        const gp::RectF r = Offset(NormRect(a.pts[0], a.pts.back()), f);
        switch (a.type) {
            case Tool::Pixelate: Pixelate(*out, r, std::max(4, (int)std::lround(10 * a.unit))); break;
            case Tool::Blur: GaussBlur(*out, r, std::max(1, (int)std::lround(kBlurR[a.level] * a.unit))); break;
            case Tool::Magnify: Magnify(*out, Offset(a.pts[0], f), Offset(a.pts.back(), f), kMagR[a.level] * a.unit); break;
            default: break;
        }
    }
    return out;
}

// Dims everything outside the spotlights (rounded) to 45%.
void Spotlights(gp::Graphics& g, const std::vector<Annot>& annots, const gp::RectF& extent, const gp::RectF* extra = nullptr) {
    std::vector<gp::RectF> spots;
    for (const auto& a : annots)
        if (a.type == Tool::Spotlight) spots.push_back(NormRect(a.pts[0], a.pts.back()));
    if (extra) spots.push_back(*extra);
    if (spots.empty()) return;
    gp::GraphicsPath path(gp::FillModeAlternate);
    path.AddRectangle(extent);
    for (const auto& r : spots) RoundedPath(path, r, std::min({8.f, r.Width / 4, r.Height / 4}));
    gp::SolidBrush dim(gp::Color(140, 0, 0, 0));
    g.FillPath(&dim, &path);
}

// Full composition in document pixels: the frame (crop or extra space), image layers and annotations,
// flattened. `pixel` is a cached PixelBase for the same state, if the caller has one.
BitmapPtr Compose(const Bitmap& base, const DocState& st, BitmapPtr pixel = nullptr) {
    const RECT f = FrameOf(base, st);
    BitmapPtr out;
    if (pixel && pixel->Width() == RectW(f) && pixel->Height() == RectH(f)) out = pixel->Crop({0, 0, pixel->Width(), pixel->Height()});
    else out = PixelBase(base, st, nullptr);  // never the unannotated base
    if (!out) return nullptr;
    {
        gp::Bitmap gb(out->Width(), out->Height(), out->Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(out->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.TranslateTransform((float)-f.left, (float)-f.top);
        Spotlights(g, st.annots, gp::RectF((float)f.left, (float)f.top, (float)RectW(f), (float)RectH(f)));
        for (const auto& a : st.annots)
            if (!IsRasterTool(a.type) || a.type == Tool::Magnify) DrawAnnot(g, a);
    }
    return out;
}

// Average colour along the image border: a fill that makes added space look like part of the screenshot.
COLORREF EdgeColor(const Bitmap& img) {
    auto s = Resample(img, 32, 32);
    if (!s) return RGB(255, 255, 255);
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            if (x != 0 && y != 0 && x != 31 && y != 31) continue;
            const uint32_t p = s->Bits()[y * 32 + x];
            r += (p >> 16) & 255;
            g += (p >> 8) & 255;
            b += p & 255;
            ++n;
        }
    return RGB((BYTE)(r / n), (BYTE)(g / n), (BYTE)(b / n));
}

// ---- toolbar icons, drawn in a 16x16 design grid ----

struct IconPainter {
    gp::Graphics& g;
    RECT r;
    float k, ox, oy;
    IconPainter(gp::Graphics& gr, const RECT& rc, float scale) : g(gr), r(rc) {
        k = scale * 18 / 16.f;
        ox = (r.left + r.right) / 2.f - 8 * k;
        oy = (r.top + r.bottom) / 2.f - 8 * k;
    }
    gp::PointF P(float x, float y) const { return {ox + x * k, oy + y * k}; }
    gp::RectF R(float x, float y, float w, float h) const { return {ox + x * k, oy + y * k, w * k, h * k}; }
};

void DrawToolIcon(gp::Graphics& g, Tool t, const RECT& r, float s, gp::Color c) {
    IconPainter ip(g, r, s);
    gp::Pen pen(c, 1.6f * s);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    gp::SolidBrush br(c);
    switch (t) {
        case Tool::Select: {
            gp::PointF pts[] = {ip.P(3, 1), ip.P(3, 14), ip.P(6.5f, 10.5f), ip.P(9, 15.5f), ip.P(11, 14.5f), ip.P(8.5f, 9.5f), ip.P(13, 9.5f)};
            pen.SetWidth(1.4f * s);
            g.DrawPolygon(&pen, pts, 7);
            break;
        }
        case Tool::Arrow: {
            g.DrawLine(&pen, ip.P(2, 14), ip.P(12, 4));
            gp::PointF tri[] = {ip.P(14.5f, 1.5f), ip.P(7.5f, 3.5f), ip.P(12.5f, 8.5f)};
            g.FillPolygon(&br, tri, 3);
            break;
        }
        case Tool::Line: g.DrawLine(&pen, ip.P(2, 14), ip.P(14, 2)); break;
        case Tool::Rect: g.DrawRectangle(&pen, ip.R(1.5f, 3.5f, 13, 9)); break;
        case Tool::Ellipse: g.DrawEllipse(&pen, ip.R(1, 3, 14, 10)); break;
        case Tool::Pen: g.DrawBezier(&pen, ip.P(1.5f, 12), ip.P(5, -1), ip.P(9, 17), ip.P(14.5f, 4)); break;
        case Tool::Highlight: {
            gp::SolidBrush hb(gp::Color(150, 255, 204, 0));
            g.FillRectangle(&hb, ip.R(1, 5, 14, 6));
            g.DrawLine(&pen, ip.P(3, 8), ip.P(13, 8));
            break;
        }
        case Tool::Text: {
            gp::Font font(L"Segoe UI", 15 * s, gp::FontStyleBold, gp::UnitPixel);
            gp::StringFormat sf;
            sf.SetAlignment(gp::StringAlignmentCenter);
            sf.SetLineAlignment(gp::StringAlignmentCenter);
            g.DrawString(L"T", 1, &font, ip.R(0, 0, 16, 16), &sf, &br);
            break;
        }
        case Tool::Pixelate:
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    if ((x + y) % 2 == 0) g.FillRectangle(&br, ip.R(1.f + x * 3.5f, 1.f + y * 3.5f, 3.5f, 3.5f));
            break;
        case Tool::Blur:  // a soft dot: concentric rings fading out
            for (int i = 4; i >= 1; --i) {
                gp::SolidBrush soft(gp::Color((BYTE)(255 / (i + 0.6f)), c.GetR(), c.GetG(), c.GetB()));
                float rr = 1.8f * i;
                g.FillEllipse(&soft, ip.R(8 - rr, 8 - rr, 2 * rr, 2 * rr));
            }
            break;
        case Tool::Spotlight: {
            gp::SolidBrush dim(gp::Color(90, c.GetR(), c.GetG(), c.GetB()));
            g.FillRectangle(&dim, ip.R(1, 2, 14, 12));
            g.FillRectangle(&br, ip.R(5, 5, 6, 6));
            break;
        }
        case Tool::Magnify:
            g.DrawEllipse(&pen, ip.R(1.5f, 1.5f, 9.5f, 9.5f));
            pen.SetWidth(2.4f * s);
            g.DrawLine(&pen, ip.P(10, 10), ip.P(14.5f, 14.5f));
            break;
        case Tool::Step: {
            g.DrawEllipse(&pen, ip.R(1.5f, 1.5f, 13, 13));
            gp::Font font(L"Segoe UI", 10 * s, gp::FontStyleBold, gp::UnitPixel);
            gp::StringFormat sf;
            sf.SetAlignment(gp::StringAlignmentCenter);
            sf.SetLineAlignment(gp::StringAlignmentCenter);
            g.DrawString(L"1", 1, &font, ip.R(0, 0.5f, 16, 16), &sf, &br);
            break;
        }
        case Tool::Crop: {
            gp::PointF a[] = {ip.P(4, 1), ip.P(4, 12), ip.P(15, 12)};
            gp::PointF b[] = {ip.P(1, 4), ip.P(12, 4), ip.P(12, 15)};
            g.DrawLines(&pen, a, 3);
            g.DrawLines(&pen, b, 3);
            break;
        }
        case Tool::Canvas: {  // a small picture inside a larger frame, arrows outward
            gp::Pen thin(c, 1.2f * s);
            thin.SetDashStyle(gp::DashStyleDash);
            g.DrawRectangle(&thin, ip.R(1, 1, 14, 14));
            g.FillRectangle(&br, ip.R(4.5f, 4.5f, 7, 7));
            break;
        }
        case Tool::Image: {  // a photo with a plus
            g.DrawRectangle(&pen, ip.R(1.5f, 3.5f, 10, 9));
            gp::PointF hill[] = {ip.P(2.5f, 11.5f), ip.P(6, 7.5f), ip.P(10.5f, 11.5f)};
            g.FillPolygon(&br, hill, 3);
            g.DrawLine(&pen, ip.P(13.5f, 1), ip.P(13.5f, 6));
            g.DrawLine(&pen, ip.P(11, 3.5f), ip.P(16, 3.5f));
            break;
        }
    }
}

// ---- the editor window ----

struct EditorTest;

class Editor;
std::vector<Editor*> g_editors;  // most recently active last

class Editor {
public:
    Editor(BitmapPtr img, const EditorSource& src) : base_(std::move(img)), source_(src) {
        st_.fill = EdgeColor(*base_);
        version_ = 0;
    }
    bool Create();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    void RunCommand(int id);
    void InsertImages(const std::vector<std::wstring>& paths, const gp::PointF* at = nullptr);
    HWND hwnd = nullptr;

private:
    int S(int v) const { return Px(s_, v); }
    RECT Canvas() const { return {0, S(kToolbarH), client_.right, client_.bottom - S(kStatusH)}; }
    const ToolInfo& CurTool() const { return kTools[(int)tool_]; }
    RECT Frame() const { return FrameOf(*base_, st_); }

    // View transform: image coordinates <-> client pixels. While a canvas edge is dragged the view is frozen,
    // so the picture stays still under the mouse.
    float Zoom() const;
    POINT Origin() const;
    POINT ViewMin() const;
    gp::PointF ToImg(int x, int y) const {
        const float z = Zoom();
        const POINT o = Origin(), m = ViewMin();
        return {m.x + (x - o.x) / z, m.y + (y - o.y) / z};
    }
    RECT Shown() const;  // the frame on screen

    void Layout();
    void FitView();
    void RebuildViewCache();
    void Compose();
    void Changed(bool pixels) {
        if (pixels) pixel_ = PixelBase(*base_, st_, nullptr);
        Compose();
    }
    void Paint(HDC hdc);
    void DrawToolbar(HDC dc);
    void DrawStatus(HDC dc);
    void DrawCanvasBar(HDC dc);
    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }
    void RebuildFonts();
    void UpdateTitle();

    void PushUndo();
    void Undo();
    void Redo();
    void Restore(DocState st);

    void SetTool(Tool t);
    void SetColor(int i);
    void SetLevel(int l);
    gp::PointF Constrain(gp::PointF a, gp::PointF b) const;
    int HitTest(gp::PointF p) const;
    void CommitText();
    void CancelLive();
    BitmapPtr Export();
    BitmapPtr StyledFrame(const Bitmap& img) const;
    void AutoRedact();
    // Pixelate boxes from auto-redact. While a drag is under way they wait: the drag's rollback (Esc, a collage
    // swap) restores a snapshot from before they existed and would silently drop them.
    void ReceiveRedactions(std::vector<Annot> boxes);
    void AddRedactions(std::vector<Annot> boxes);
    void FlushRedactions();
    bool Gesture() const { return drawing_ || moving_ || resizing_.has_value() || framing_.has_value(); }
    std::vector<Annot> pendingRedactions_;
    void CopyAnnotations();
    void PasteAnnotations();
    void DuplicateSelected();
    void DeleteSelected();
    void Reorder(bool front);
    void Insert(BitmapPtr img, const std::wstring& source, const gp::PointF* at);
    void PickImage();
    bool PasteImage();
    void LayerMenu(int index, POINT screen);
    void EditLayer(int index, const std::function<void(ImageLayer&)>& f);
public:
    enum class Extend { Below, Right, Even };

private:
    void ExtendFrame(Extend how);
    void ResetFrame();
    void FillMenu(POINT screen);
    std::vector<std::wstring> Includes() const;
    bool Edited(const Bitmap& out) const {
        return !st_.annots.empty() || out.Width() != base_->Width() || out.Height() != base_->Height();
    }

    void Copy();
    void Save(bool quiet = false);
    void SaveAs();
    void Pin();
    void Done();
    void Close();
    void Action(int act);
    void OpenCommandPalette();

    void OnLButtonDown(int x, int y, bool dbl);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnRButtonUp(int x, int y);
    void OnKeyDown(WPARAM vk);
    void OnChar(wchar_t ch);
    void OnDropFiles(HDROP drop);

    // document
    bool styled_ = g_defaults.styledExport;
    bool redacting_ = false;
    BitmapPtr base_, committed_, pixel_;
    EditorSource source_;
    DocState st_;
    std::vector<DocState> undo_, redo_;
    bool dirty_ = false;
    uint64_t version_ = 0;  // bumped on every change; an async save only clears dirty_ for the version it wrote

    // tool state
    Tool tool_ = Tool::Arrow;
    int color_ = 0, level_ = 1;
    float unit_ = 1;

    // interaction
    bool drawing_ = false, moving_ = false, texting_ = false;
    Annot live_;
    int movingIndex_ = -1, selected_ = -1, hoverBtn_ = -1;
    gp::PointF lastPt_{}, moveStart_{};
    struct Resizing {
        int index;
        gp::PointF anchor;
        float aspect;
    };
    std::optional<Resizing> resizing_;
    struct Framing {
        int edges;  // 1 left, 2 right, 4 top, 8 bottom
        gp::RectF start;
        gp::PointF from;
    };
    std::optional<Framing> framing_;
    struct Frozen {
        float zoom;
        POINT origin, viewMin;
        HBITMAP view;  // the picture as it was, drawn until the drag ends
        RECT shown;
    };
    std::optional<Frozen> frozen_;

    // view
    float s_ = 1, zoom_ = 1;
    RECT client_{};
    int ox_ = 0, oy_ = 0, viewW_ = 1, viewH_ = 1;
    HBITMAP viewCache_ = nullptr;
    HDC backDC_ = nullptr;
    HBITMAP back_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0, backH_ = 0;
    HFONT fUi_ = nullptr, fIcon_ = nullptr, fStatus_ = nullptr, fBar_ = nullptr;
    std::vector<Button> buttons_;
    struct BarButton {
        RECT r;
        int act;  // Action, or -1 for the fill menu
        std::wstring label;
    };
    std::vector<BarButton> barButtons_;
    int hoverBar_ = -1;
    int minClientW_ = 600;

    bool hidden_ = false;  // drawn into memory by the snapshot tool, never shown
    // Collage: the screenshots, where each came from, and the layout options.
    std::optional<CollageSpec> collage_;
    std::vector<BitmapPtr> collageImages_;
    std::vector<std::wstring> collageSources_;
    void ApplyCollage(bool undoable);
    void SwapCollage(int a, int b);
    void CollageMenu(int which, POINT at);
    void DrawCollageBar(HDC dc);
    friend struct EditorTest;
    friend bool ather::OpenCollage(const std::vector<std::wstring>&);
    friend int ather::EditorSnapshots(const std::wstring&);
    friend LRESULT CALLBACK EditorProc(HWND, UINT, WPARAM, LPARAM);
    friend void ather::OpenEditor(BitmapPtr, const EditorSource&);
    friend bool ather::AddImagesToEditor(const std::vector<std::wstring>&);
};

Editor* FromHwnd(HWND h) { return IsWindow(h) ? reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr; }

LRESULT CALLBACK EditorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* e = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        e->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)e);
    }
    auto* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!e) return DefWindowProcW(h, m, w, l);
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), e), g_editors.end());
        delete e;
        return 0;
    }
    return e->Proc(m, w, l);
}

bool Editor::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = g_defaults.icon;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    POINT pt;
    GetCursorPos(&pt);
    s_ = DpiScaleAt(pt);
    unit_ = s_;
    RebuildFonts();
    Changed(true);

    RECT work = MonitorRectAt(pt, true);
    const RECT f = Frame();
    int cw = RectW(f) + 2 * S(kMargin), ch = RectH(f) + S(kToolbarH) + S(kStatusH) + 2 * S(kMargin);
    cw = std::clamp(cw, S(1400), std::max(S(1400), (int)(RectW(work) * 0.9)));  // full toolbar always fits
    ch = std::clamp(ch, S(440), (int)(RectH(work) * 0.9));
    RECT r{0, 0, cw, ch};
    UINT dpi = (UINT)(s_ * 96 + 0.5f);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = std::min(RectW(r), RectW(work)), h = std::min(RectH(r), RectH(work));
    if (!CreateWindowExW(WS_EX_ACCEPTFILES, kClass, L"Annotate", WS_OVERLAPPEDWINDOW, work.left + (RectW(work) - w) / 2,
                         work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this))
        return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    UpdateTitle();
    g_editors.push_back(this);
    if (!hidden_) ShowWithoutFlash(hwnd, true);
    return true;
}

void Editor::UpdateTitle() {
    if (!hwnd) return;
    const RECT f = Frame();
    const std::wstring size = std::to_wstring(RectW(f)) + L" × " + std::to_wstring(RectH(f));
    std::wstring t = collage_ ? L"Collage — " + std::to_wstring(collageImages_.size()) + L" screenshots, " + size : L"Annotate — " + size;
    SetWindowTextW(hwnd, t.c_str());
}

void Editor::RebuildFonts() {
    for (HFONT* f : {&fUi_, &fIcon_, &fStatus_, &fBar_})
        if (*f) DeleteObject(*f);
    fUi_ = MakeDisplayFont(S(18));
    fIcon_ = MakeFont(S(16), FW_NORMAL, L"Segoe Fluent Icons");
    fStatus_ = MakeFont(S(12));
    fBar_ = MakeFont(S(13));
}

void Editor::Layout() {
    GetClientRect(hwnd, &client_);
    buttons_.clear();
    const int btn = S(kBtn), cy = S(kToolbarH) / 2;
    int x = S(10);
    for (int i = 0; i < (int)std::size(kTools); ++i) {
        std::wstring tip = std::wstring(kTools[i].name) + L"  (" + kTools[i].key + L")  —  " + kTools[i].hint;
        buttons_.push_back({Button::KTool, i, {x, cy - btn / 2, x + btn, cy + btn / 2}, tip});
        x += btn + S(2);
    }
    x += S(12);
    const int sw = S(24);
    for (int i = 0; i < (int)std::size(kColors); ++i) {
        buttons_.push_back({Button::KColor, i, {x, cy - sw / 2, x + sw, cy + sw / 2}, std::wstring(kColorNames[i]) + L"  (" + std::to_wstring(i + 1) + L")"});
        x += sw + S(4);
    }
    x += S(12);
    const int ww = S(26);
    for (int i = 0; i < kLevels; ++i) {
        buttons_.push_back({Button::KWidth, i, {x, cy - ww / 2, x + ww, cy + ww / 2}, L"Size " + std::to_wstring(i + 1) + L"  ([ and ] or mouse wheel)"});
        x += ww + S(2);
    }
    const int leftEnd = x;
    struct ActDef {
        int act;
        int width;
        const wchar_t* tip;
    };
    const ActDef acts[] = {{ActDone, S(78), L"Done — copy, save and close  (Enter)"},
                           {ActPin, btn, L"Pin to screen  (Ctrl+P)"},
                           {ActSave, btn, L"Save to captures folder  (Ctrl+S, Ctrl+Shift+S = Save as)"},
                           {ActCopy, btn, L"Copy to clipboard  (Ctrl+C)"},
                           {ActRedo, btn, L"Redo  (Ctrl+Y)"},
                           {ActUndo, btn, L"Undo  (Ctrl+Z)"}};
    int rx = client_.right - S(10);
    for (const auto& a : acts) {
        buttons_.push_back({Button::KAction, a.act, {rx - a.width, cy - btn / 2, rx, cy + btn / 2}, a.tip});
        rx -= a.width + (a.act == ActCopy ? S(14) : S(2));
    }
    minClientW_ = leftEnd + (client_.right - rx) + S(16);
    FitView();
}

float Editor::Zoom() const { return frozen_ ? frozen_->zoom : zoom_; }
POINT Editor::Origin() const { return frozen_ ? frozen_->origin : POINT{ox_, oy_}; }
POINT Editor::ViewMin() const {
    if (frozen_) return frozen_->viewMin;
    const RECT f = Frame();
    return {f.left, f.top};
}

RECT Editor::Shown() const {
    const RECT f = Frame();
    const float z = Zoom();
    const POINT o = Origin(), m = ViewMin();
    return {o.x + (LONG)std::lround((f.left - m.x) * z), o.y + (LONG)std::lround((f.top - m.y) * z),
            o.x + (LONG)std::lround((f.right - m.x) * z), o.y + (LONG)std::lround((f.bottom - m.y) * z)};
}

void Editor::FitView() {
    RECT c = Canvas();
    const RECT f = Frame();
    const int reserve = tool_ == Tool::Canvas || collage_ ? S(kBarH + 20) : 0;  // room for the floating bar
    int aw = std::max(1, RectW(c) - 2 * S(kMargin)), ah = std::max(1, RectH(c) - 2 * S(kMargin) - reserve);
    zoom_ = std::min({1.f, (float)aw / RectW(f), (float)ah / RectH(f)});
    viewW_ = std::max(1, (int)std::lround(RectW(f) * zoom_));
    viewH_ = std::max(1, (int)std::lround(RectH(f) * zoom_));
    ox_ = c.left + (RectW(c) - viewW_) / 2;
    oy_ = c.top + (RectH(c) - reserve - viewH_) / 2;
    RebuildViewCache();
}

// The composed picture scaled for the screen (over a checkerboard where it's transparent).
void Editor::RebuildViewCache() {
    if (!hwnd || !committed_) return;
    if (viewCache_) DeleteObject(viewCache_);
    auto view = Bitmap::Create(viewW_, viewH_);
    if (!view) return;
    const bool alpha = !st_.fill;
    if (alpha)
        for (int y = 0; y < viewH_; ++y)
            for (int x = 0; x < viewW_; ++x)
                view->Bits()[(size_t)y * viewW_ + x] = ((x / 8 + y / 8) % 2) ? 0xFFB4B4B4u : 0xFFD9D9D9u;
    {
        gp::Bitmap dst(viewW_, viewH_, viewW_ * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(view->Bits()));
        gp::Graphics g(&dst);
        g.SetCompositingMode(alpha ? gp::CompositingModeSourceOver : gp::CompositingModeSourceCopy);
        const bool same = viewW_ == committed_->Width() && viewH_ == committed_->Height();
        g.SetInterpolationMode(same ? gp::InterpolationModeNearestNeighbor : gp::InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        gp::Bitmap src(committed_->Width(), committed_->Height(), committed_->Width() * 4, PixelFormat32bppPARGB,
                       reinterpret_cast<BYTE*>(committed_->Bits()));
        gp::ImageAttributes ia;
        ia.SetWrapMode(gp::WrapModeTileFlipXY);
        g.DrawImage(&src, gp::Rect(0, 0, viewW_, viewH_), 0, 0, committed_->Width(), committed_->Height(), gp::UnitPixel, &ia);
    }
    for (size_t i = 0, n = (size_t)viewW_ * viewH_; i < n; ++i) view->Bits()[i] |= 0xFF000000u;
    HDC screen = GetDC(hwnd);
    viewCache_ = CreateCompatibleBitmap(screen, viewW_, viewH_);
    {
        MemDC d(viewCache_, screen), sdc(view->Handle(), screen);
        BitBlt(d, 0, 0, viewW_, viewH_, sdc, 0, 0, SRCCOPY);
    }
    ReleaseDC(hwnd, screen);
    Invalidate();
}

// fill + base + layers + pixel tools (cached in pixel_) + spotlight + annotations -> committed_.
void Editor::Compose() {
    const RECT f = Frame();
    if (!pixel_ || pixel_->Width() != RectW(f) || pixel_->Height() != RectH(f)) pixel_ = PixelBase(*base_, st_, nullptr);
    committed_ = ather::Compose(*base_, st_, pixel_);
    UpdateTitle();
    if (!hwnd) return;
    RECT c;
    GetClientRect(hwnd, &c);
    if (RectW(c) > 0) FitView();
}

void Editor::PushUndo() {
    undo_.push_back(st_);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
    ++version_;
}

void Editor::Restore(DocState st) {
    st_ = std::move(st);
    selected_ = -1;
    Changed(true);
    dirty_ = true;
    ++version_;
}

void Editor::Undo() {
    CancelLive();
    CommitText();
    if (undo_.empty()) return;
    redo_.push_back(st_);
    DocState st = std::move(undo_.back());
    undo_.pop_back();
    Restore(std::move(st));
}

void Editor::Redo() {
    CancelLive();
    if (redo_.empty()) return;
    undo_.push_back(st_);
    DocState st = std::move(redo_.back());
    redo_.pop_back();
    Restore(std::move(st));
}

void Editor::SetTool(Tool t) {
    if (t == Tool::Image) return PickImage();  // an action, not a mode
    CommitText();
    const bool reflow = (t == Tool::Canvas) != (tool_ == Tool::Canvas);
    tool_ = t;
    if (t != Tool::Select) selected_ = -1;
    if (reflow) FitView();
    Invalidate();
}

void Editor::SetColor(int i) {
    color_ = i;
    if (texting_) live_.color = kColors[i];
    else if (selected_ >= 0 && selected_ < (int)st_.annots.size()) {
        PushUndo();
        st_.annots[selected_].color = kColors[i];
        Changed(st_.annots[selected_].type == Tool::Image);  // a layer's border uses the colour
    }
    Invalidate();
}

void Editor::SetLevel(int l) {
    level_ = std::clamp(l, 0, kLevels - 1);
    if (texting_) live_.level = level_;
    else if (selected_ >= 0 && selected_ < (int)st_.annots.size()) {
        PushUndo();
        st_.annots[selected_].level = level_;
        Changed(IsRasterTool(st_.annots[selected_].type));
    }
    Invalidate();
}

gp::PointF Editor::Constrain(gp::PointF a, gp::PointF b) const {
    if (GetKeyState(VK_SHIFT) >= 0) return b;
    float dx = b.X - a.X, dy = b.Y - a.Y;
    if (IsRectTool(tool_)) {
        float d = std::max(std::fabs(dx), std::fabs(dy));
        return {a.X + std::copysign(d, dx), a.Y + std::copysign(d, dy)};
    }
    float len = std::hypot(dx, dy), ang = std::atan2(dy, dx);
    float snap = std::round(ang / (3.14159265f / 4)) * (3.14159265f / 4);
    return {a.X + len * std::cos(snap), a.Y + len * std::sin(snap)};
}

int Editor::HitTest(gp::PointF p) const {
    for (int i = (int)st_.annots.size() - 1; i >= 0; --i)
        if (Bounds(st_.annots[i]).Contains(p)) return i;
    return -1;
}

void Editor::CommitText() {
    if (!texting_) return;
    texting_ = false;
    if (!live_.text.empty()) {
        PushUndo();
        st_.annots.push_back(live_);
        Changed(false);
    }
    Invalidate();
}

void Editor::CancelLive() {
    if (moving_) {  // put the annotation back where it was
        st_.annots.insert(st_.annots.begin() + movingIndex_, live_);
        st_.annots[movingIndex_].pts = live_.pts;
        for (auto& p : st_.annots[movingIndex_].pts) p = gp::PointF(p.X - (lastPt_.X - moveStart_.X), p.Y - (lastPt_.Y - moveStart_.Y));
        undo_.pop_back();
        Changed(IsRasterTool(live_.type));
    }
    if (resizing_) {
        resizing_.reset();
        if (!undo_.empty()) {
            st_ = undo_.back();
            undo_.pop_back();
            Changed(true);
        }
    }
    if (framing_) {
        framing_.reset();
        if (frozen_) {
            DeleteObject(frozen_->view);
            frozen_.reset();
        }
        if (!undo_.empty()) {
            st_ = undo_.back();
            undo_.pop_back();
            Changed(true);
        }
    }
    drawing_ = moving_ = texting_ = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    Invalidate();
}

BitmapPtr Editor::Export() {
    CommitText();
    if (styled_) return StyledFrame(*committed_);
    return committed_->Crop({0, 0, committed_->Width(), committed_->Height()});
}

// Presentation export: gradient backdrop, padding, soft shadow and rounded corners.
BitmapPtr Editor::StyledFrame(const Bitmap& img) const {
    const float u = unit_;
    const int pad = (int)std::lround(std::clamp(std::min(img.Width(), img.Height()) * 0.08f, 24 * u, 72 * u));
    const int W = img.Width() + 2 * pad, H = img.Height() + 2 * pad;
    auto out = Bitmap::Create(W, H);
    if (!out) return nullptr;
    {
        gp::Bitmap gb(W, H, W * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(out->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        gp::LinearGradientBrush bg(gp::Point(0, 0), gp::Point(W, H), gp::Color(255, 124, 108, 255), gp::Color(255, 64, 170, 255));
        g.FillRectangle(&bg, 0, 0, W, H);
        const float r = 12 * u;
        DrawShadow(g, gp::RectF((float)pad, (float)pad, (float)img.Width(), (float)img.Height()), r, 36 * u, 8 * u, 115);
        gp::GraphicsPath clip;
        RoundedPath(clip, gp::RectF((float)pad, (float)pad, (float)img.Width(), (float)img.Height()), r);
        gp::Bitmap src(img.Width(), img.Height(), img.Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img.Bits()));
        gp::TextureBrush tb(&src, gp::WrapModeClamp);
        tb.TranslateTransform((float)pad, (float)pad);
        g.FillPath(&tb, &clip);
    }
    for (size_t i = 0, n = (size_t)W * H; i < n; ++i) out->Bits()[i] |= 0xFF000000u;
    return out;
}

void Editor::AutoRedact() {
    if (redacting_) return;
    CommitText();
    redacting_ = true;
    ShowToast(L"Looking for sensitive text…", L"Emails, IPs, keys, tokens, card and phone numbers", nullptr, nullptr, 8000);
    HWND h = hwnd;
    const float unit = unit_;
    RECT f;
    BitmapPtr raster = Raster(*base_, st_, &f);  // includes image layers and added space
    if (raster) raster = Flatten(*raster);
    RecognizeWordsAsync(raster, [h, unit, f](std::vector<OcrWord> words, std::wstring err) {
        Editor* e = FromHwnd(h);
        if (!e) return;
        e->redacting_ = false;
        if (!err.empty()) return (void)ShowToast(L"Auto-redact failed", err, nullptr, nullptr, 4000);
        auto rects = FindSensitive(words);
        if (rects.empty())
            return (void)ShowToast(L"Nothing sensitive found", std::to_wstring(words.size()) + L" words checked", nullptr, nullptr, 2500);
        std::vector<Annot> boxes;
        for (const RECT& r : rects) {
            Annot a;
            a.type = Tool::Pixelate;
            a.unit = unit;
            a.level = 2;
            a.pts = {gp::PointF((float)(r.left + f.left), (float)(r.top + f.top)), gp::PointF((float)(r.right + f.left), (float)(r.bottom + f.top))};
            boxes.push_back(a);
        }
        e->ReceiveRedactions(std::move(boxes));
    });
}

void Editor::ReceiveRedactions(std::vector<Annot> boxes) {
    if (Gesture()) {  // added when the drag ends
        pendingRedactions_.insert(pendingRedactions_.end(), boxes.begin(), boxes.end());
        return;
    }
    AddRedactions(std::move(boxes));
}

void Editor::AddRedactions(std::vector<Annot> boxes) {
    if (boxes.empty()) return;
    PushUndo();
    const size_t n = boxes.size();
    for (auto& a : boxes) st_.annots.push_back(std::move(a));
    Changed(true);
    SetTool(Tool::Select);
    ShowToast(L"Redacted " + std::to_wstring(n) + (n == 1 ? L" item" : L" items"), L"Each one is a normal pixelate box: move, delete or undo it.",
              nullptr, nullptr, 3000);
}

void Editor::FlushRedactions() {
    if (pendingRedactions_.empty() || Gesture()) return;
    AddRedactions(std::move(pendingRedactions_));
    pendingRedactions_.clear();
}

void Editor::CopyAnnotations() {
    CommitText();
    if (selected_ >= 0 && selected_ < (int)st_.annots.size()) g_annotClipboard = {st_.annots[selected_]};
    else g_annotClipboard = st_.annots;
    ShowToast(L"Copied " + std::to_wstring(g_annotClipboard.size()) + L" annotation(s)", L"Paste into any editor with Ctrl+Shift+V", nullptr, nullptr, 1800);
}

void Editor::PasteAnnotations() {
    if (g_annotClipboard.empty()) return;
    CommitText();
    PushUndo();
    for (const auto& a : g_annotClipboard) st_.annots.push_back(a);
    selected_ = (int)st_.annots.size() - 1;
    tool_ = Tool::Select;
    Changed(true);
}

void Editor::DuplicateSelected() {
    if (selected_ < 0 || selected_ >= (int)st_.annots.size()) return;
    PushUndo();
    Annot a = st_.annots[selected_];
    for (auto& p : a.pts) {
        p.X += 16 * unit_;
        p.Y += 16 * unit_;
    }
    if (a.type == Tool::Step) a.step = st_.nextStep++;
    if (a.layer && a.layer->slot >= 0) {  // a copy is a free layer, not part of a collage layout
        auto l = std::make_shared<ImageLayer>(*a.layer);
        l->slot = -1;
        a.layer = l;
    }
    st_.annots.push_back(a);
    selected_ = (int)st_.annots.size() - 1;
    Changed(IsRasterTool(a.type));
}

void Editor::DeleteSelected() {
    if (selected_ < 0 || selected_ >= (int)st_.annots.size()) return;
    PushUndo();
    const bool px = IsRasterTool(st_.annots[selected_].type);
    st_.annots.erase(st_.annots.begin() + selected_);
    selected_ = -1;
    Changed(px);
}

void Editor::Reorder(bool front) {
    if (selected_ < 0 || selected_ >= (int)st_.annots.size()) return;
    PushUndo();
    Annot a = st_.annots[selected_];
    st_.annots.erase(st_.annots.begin() + selected_);
    if (front) {
        st_.annots.push_back(a);
        selected_ = (int)st_.annots.size() - 1;
    } else {
        st_.annots.insert(st_.annots.begin(), a);
        selected_ = 0;
    }
    Changed(IsRasterTool(a.type));
}

// Places a screenshot centred at `at` (or the middle of the canvas), at most 60% of the canvas.
void Editor::Insert(BitmapPtr img, const std::wstring& source, const gp::PointF* at) {
    if (!img) return;
    CommitText();
    const RECT v = Frame();
    const float w = (float)img->Width(), h = (float)img->Height();
    const float k = std::min({1.f, RectW(v) * 0.6f / w, RectH(v) * 0.6f / h});
    const gp::PointF c = at ? *at : gp::PointF((v.left + v.right) / 2.f, (v.top + v.bottom) / 2.f);
    const float x = std::round(c.X - w * k / 2), y = std::round(c.Y - h * k / 2);
    Annot a;
    a.type = Tool::Image;
    a.color = kColors[6];
    a.level = 0;
    a.unit = unit_;
    a.pts = {gp::PointF(x, y), gp::PointF(x + std::round(w * k), y + std::round(h * k))};
    auto layer = std::make_shared<ImageLayer>();
    layer->image = img;
    layer->source = source;
    layer->radius = 8 * unit_;
    layer->shadow = true;
    a.layer = layer;
    PushUndo();
    st_.annots.push_back(a);
    selected_ = (int)st_.annots.size() - 1;
    tool_ = Tool::Select;
    Changed(true);
}

void Editor::InsertImages(const std::vector<std::wstring>& paths, const gp::PointF* at) {
    std::optional<gp::PointF> p;
    if (at) p = *at;
    for (const auto& u : paths) {
        auto img = LoadImageFile(u);
        if (!img) {
            ShowToast(L"Can't open that image", FileNameOf(u), nullptr, nullptr, 2500);
            continue;
        }
        Insert(img, u, p ? &*p : nullptr);
        if (p) p = gp::PointF(p->X + 24 * unit_, p->Y + 24 * unit_);
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    ForceForeground(hwnd);
}

// I: a searchable list of recent screenshots, plus "Choose a file…".
void Editor::PickImage() {
    CommitText();
    std::vector<std::wstring> recent;
    const Library& lib = Library::Shared();
    for (const auto& p : lib.Paths())
        if (MediaTypeOf(p) == MediaType::Image && recent.size() < 300) recent.push_back(p);
    std::vector<PaletteItem> items = {{0, L"Choose a file…", L"", L"open explorer disk browse", 0xE838}};
    for (size_t i = 0; i < recent.size(); ++i) {
        const ItemMeta& m = lib.Meta(recent[i]);
        std::wstring name = FileNameOf(recent[i]);
        name = name.substr(0, name.find_last_of(L'.'));
        std::wstring kw = m.app + L" " + m.window;
        for (const auto& t : m.tags) kw += L" " + t;
        items.push_back({(int)i + 1, name, m.w > 0 ? std::to_wstring(m.w) + L" × " + std::to_wstring(m.h) : L"", kw, (wchar_t)0xEB9F});
    }
    HWND h = hwnd;
    PaletteOptions opt;
    opt.placeholder = L"Insert a screenshot…";
    ShowPalette(std::move(items), [h, recent](int id) {
        Editor* e = FromHwnd(h);
        if (!e) return;
        if (id > 0 && id <= (int)recent.size()) return e->InsertImages({recent[id - 1]});
        std::vector<wchar_t> buf(32768, L'\0');
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = h;
        ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff\0All files\0*.*\0";
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
        e->InsertImages(files);
    }, opt);
}

// Ctrl+V: image files from Explorer, or a copied image, become image layers.
bool Editor::PasteImage() {
    if (!OpenClipboard(hwnd)) return false;
    std::vector<std::wstring> files;
    BitmapPtr img;
    if (HANDLE hd = GetClipboardData(CF_HDROP)) {
        auto drop = (HDROP)hd;
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            wchar_t p[MAX_PATH * 2];
            if (DragQueryFileW(drop, i, p, (UINT)std::size(p)) && IsMediaFile(p) && MediaTypeOf(p) == MediaType::Image) files.push_back(p);
        }
    }
    if (files.empty())
        if (HBITMAP hb = (HBITMAP)GetClipboardData(CF_BITMAP)) {
            BITMAP bm{};
            GetObjectW(hb, sizeof(bm), &bm);
            img = Bitmap::Create(bm.bmWidth, bm.bmHeight);
            if (img) {
                BITMAPINFO bi{};
                bi.bmiHeader = {sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB};
                HDC dc = GetDC(nullptr);
                GetDIBits(dc, hb, 0, bm.bmHeight, img->Bits(), &bi, DIB_RGB_COLORS);
                ReleaseDC(nullptr, dc);
                for (size_t i = 0, n = (size_t)bm.bmWidth * bm.bmHeight; i < n; ++i) img->Bits()[i] |= 0xFF000000u;
            }
        }
    CloseClipboard();
    if (!files.empty()) {
        InsertImages(files);
        return true;
    }
    if (img) {
        Insert(img, L"", nullptr);
        return true;
    }
    return false;
}

void Editor::EditLayer(int i, const std::function<void(ImageLayer&)>& f) {
    if (i < 0 || i >= (int)st_.annots.size() || !st_.annots[i].layer) return;
    PushUndo();
    auto l = std::make_shared<ImageLayer>(*st_.annots[i].layer);
    f(*l);
    st_.annots[i].layer = l;
    Changed(true);
}

void Editor::LayerMenu(int i, POINT at) {
    const ImageLayer l = *st_.annots[i].layer;
    HMENU m = CreatePopupMenu();
    auto add = [&](int id, const wchar_t* t, bool on = false) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, t); };
    add(1, L"Bring to front\tCtrl+]");
    add(2, L"Send to back\tCtrl+[");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(3, L"Rounded corners", l.radius > 0);
    add(4, L"Shadow", l.shadow);
    add(5, L"Border (uses the current color and size)", l.border);
    const float ops[] = {1.f, 0.75f, 0.5f, 0.25f};
    for (int k = 0; k < 4; ++k) add(10 + k, (L"Opacity " + std::to_wstring((int)(ops[k] * 100)) + L"%").c_str(), std::fabs(l.opacity - ops[k]) < 0.01f);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(6, L"Actual size");
    add(7, L"Delete\tDel");
    SetForegroundWindow(hwnd);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    selected_ = i;
    switch (id) {
        case 1: Reorder(true); break;
        case 2: Reorder(false); break;
        case 3: EditLayer(i, [this](ImageLayer& x) { x.radius = x.radius > 0 ? 0 : 8 * unit_; }); break;
        case 4: EditLayer(i, [](ImageLayer& x) { x.shadow = !x.shadow; }); break;
        case 5: {
            EditLayer(i, [](ImageLayer& x) { x.border = !x.border; });
            st_.annots[i].color = kColors[color_];
            st_.annots[i].level = level_;
            Changed(true);
            break;
        }
        case 6: {
            PushUndo();
            const gp::RectF r = NormRect(st_.annots[i].pts[0], st_.annots[i].pts.back());
            st_.annots[i].pts = {gp::PointF(r.X, r.Y), gp::PointF(r.X + l.image->Width(), r.Y + l.image->Height())};
            Changed(true);
            break;
        }
        case 7: DeleteSelected(); break;
        default:
            if (id >= 10 && id < 14) EditLayer(i, [&](ImageLayer& x) { x.opacity = ops[id - 10]; });
    }
    Invalidate();
}

// Space below, space on the right, or an even margin; the fill paints it.
void Editor::ExtendFrame(Extend how) {
    CommitText();
    PushUndo();
    const RECT f = Frame();
    const float W = (float)RectW(f), H = (float)RectH(f);
    switch (how) {
        case Extend::Below: st_.crop = gp::RectF((float)f.left, (float)f.top, W, H + std::max(160 * unit_, H * 0.35f)); break;
        case Extend::Right: st_.crop = gp::RectF((float)f.left, (float)f.top, W + std::max(240 * unit_, W * 0.4f), H); break;
        case Extend::Even: {
            const float m = std::round(std::max(32 * unit_, std::min(W, H) * 0.06f));
            st_.crop = gp::RectF(f.left - m, f.top - m, W + 2 * m, H + 2 * m);
            break;
        }
    }
    Changed(true);
}

void Editor::ResetFrame() {
    if (!st_.crop) return;
    PushUndo();
    st_.crop.reset();
    Changed(true);
}

void Editor::FillMenu(POINT at) {
    HMENU m = CreatePopupMenu();
    const COLORREF edge = EdgeColor(*base_);
    const int cur = !st_.fill ? 4 : *st_.fill == edge ? 1 : *st_.fill == RGB(255, 255, 255) ? 2 : *st_.fill == RGB(0, 0, 0) ? 3 : 0;
    AppendMenuW(m, MF_STRING, 1, L"Edge color");
    AppendMenuW(m, MF_STRING, 2, L"White");
    AppendMenuW(m, MF_STRING, 3, L"Black");
    AppendMenuW(m, MF_STRING, 4, L"Transparent");
    if (cur) CheckMenuRadioItem(m, 1, 4, cur, MF_BYCOMMAND);
    SetForegroundWindow(hwnd);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, at.x, at.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (!id) return;
    PushUndo();
    if (id == 1) st_.fill = edge;
    else if (id == 2) st_.fill = RGB(255, 255, 255);
    else if (id == 3) st_.fill = RGB(0, 0, 0);
    else st_.fill.reset();
    Changed(true);
}

// Screenshots placed on the canvas, recorded on the saved copy.
std::vector<std::wstring> Editor::Includes() const {
    std::vector<std::wstring> out;
    for (const auto& a : st_.annots)
        if (a.layer && !a.layer->source.empty() && _wcsicmp(a.layer->source.c_str(), source_.path.c_str()) != 0 &&
            std::find(out.begin(), out.end(), a.layer->source) == out.end())
            out.push_back(a.layer->source);
    return out;
}

void Editor::Copy() {
    auto img = Export();
    if (CopyImageToClipboard(hwnd, *img))
        ShowToast(L"Copied to clipboard", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
}

void Editor::Save(bool quiet) {
    auto img = Export();
    CaptureNameInfo info{source_.window, source_.app, img->Width(), img->Height()};
    const std::wstring path = MakeCapturePath(g_defaults.capturesFolder, L"png", info);
    // Joins the gallery with the original's tags and collections, stacked as a version of it. A collage is
    // tagged "collage" and keeps what all of its screenshots share.
    Library::Shared().NoteEdit(path, source_.path, source_.app, source_.window, Edited(*img), Includes(), collage_.has_value());
    HWND h = hwnd;
    const uint64_t version = version_;
    SavePngAsync(img, path, [path, h, version, quiet, img](bool ok) {
        if (ok) {
            if (!quiet) ShowToast(L"Saved", FileNameOf(path), nullptr, [path] { RevealInExplorer({path}); }, 2200);
        } else {
            ShowToast(L"Save failed", path, nullptr, nullptr, 4000);
        }
        // Only a successful write counts as saved, and only if nothing changed while it was written.
        Editor* e = FromHwnd(h);
        if (e && ok && e->version_ == version) e->dirty_ = false;
    });
}

void Editor::SaveAs() {
    auto img = Export();
    wchar_t file[MAX_PATH] = L"annotated.png";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    Library::Shared().NoteEdit(file, source_.path, source_.app, source_.window, true, Includes(), collage_.has_value());
    if (SavePng(*img, file)) dirty_ = false;  // a failed save must still warn before closing
    else ShowToast(L"Save failed", file, nullptr, nullptr, 4000);
}

void Editor::Pin() { PinImage(Flatten(*Export()), nullptr); }

void Editor::Done() {
    auto img = Export();
    const bool copied = CopyImageToClipboard(hwnd, *img);
    if (g_defaults.saveToFile) {
        Save(true);
        ShowToast(copied ? L"Copied and saved" : L"Saved", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 2000);
    } else if (copied) {
        ShowToast(L"Copied to clipboard", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
    }
    dirty_ = false;
    DestroyWindow(hwnd);
}

void Editor::Close() {
    CommitText();
    if (dirty_ && MessageBoxW(hwnd, L"Close the editor and discard your changes?", L"Ather Screenshot", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    DestroyWindow(hwnd);
}

void Editor::Action(int act) {
    switch (act) {
        case ActUndo: Undo(); break;
        case ActRedo: Redo(); break;
        case ActCopy: Copy(); break;
        case ActSave: Save(); break;
        case ActSaveAs: SaveAs(); break;
        case ActPin: Pin(); break;
        case ActDone: Done(); break;
        case ActClose: Close(); break;
        case ActStyle:
            styled_ = !styled_;
            ShowToast(styled_ ? L"Styled export: On" : L"Styled export: Off",
                      L"Background, padding, shadow and rounded corners when you copy, save or pin.", nullptr, nullptr, 2200);
            Invalidate();
            break;
        case ActRedact: AutoRedact(); break;
        case ActCopyAnnots: CopyAnnotations(); break;
        case ActPasteAnnots: PasteAnnotations(); break;
        case ActDuplicate: DuplicateSelected(); break;
        case ActInsertImage: PickImage(); break;
        case ActSpaceBelow: ExtendFrame(Extend::Below); break;
        case ActSpaceRight: ExtendFrame(Extend::Right); break;
        case ActEvenMargin: ExtendFrame(Extend::Even); break;
        case ActResetFrame: ResetFrame(); break;
        case ActFront: Reorder(true); break;
        case ActBack: Reorder(false); break;
    }
}

// Palette ids: 100+tool, 200+color, 300+size, 400+action.
void Editor::RunCommand(int id) {
    if (id >= 400) Action(id - 400);
    else if (id >= 300) SetLevel(id - 300);
    else if (id >= 200) SetColor(id - 200);
    else if (id >= 100) SetTool((Tool)(id - 100));
}

void Editor::OpenCommandPalette() {
    CommitText();
    std::vector<PaletteItem> items;
    struct A {
        int act;
        const wchar_t* title;
        const wchar_t* hint;
        const wchar_t* kw;
        wchar_t icon;
    };
    const A acts[] = {{ActDone, L"Done: copy, save and close", L"Enter", L"finish", 0xE8FB},
                      {ActCopy, L"Copy to clipboard", L"Ctrl+C", L"", 0xE8C8},
                      {ActSave, L"Save to captures folder", L"Ctrl+S", L"png", 0xE74E},
                      {ActSaveAs, L"Save as…", L"Ctrl+Shift+S", L"export file", 0xE74E},
                      {ActPin, L"Pin to screen", L"Ctrl+P", L"float", 0xE840},
                      {ActUndo, L"Undo", L"Ctrl+Z", L"", 0xE7A7},
                      {ActRedo, L"Redo", L"Ctrl+Y", L"", 0xE7A6},
                      {ActRedact, L"Auto-redact sensitive text", L"Ctrl+R", L"ocr privacy hide email key token password pixelate", 0xE72E},
                      {ActStyle, L"Styled export (background, shadow, rounded corners)", styled_ ? L"On" : L"Off", L"beautify pretty frame padding share", 0xE771},
                      {ActCopyAnnots, L"Copy annotations", L"Ctrl+Shift+C", L"shapes markup clipboard", 0xE8C8},
                      {ActPasteAnnots, L"Paste annotations", L"Ctrl+Shift+V", L"shapes markup clipboard", 0xE77F},
                      {ActDuplicate, L"Duplicate selected", L"Ctrl+D", L"clone copy", 0xE8C8},
                      {ActInsertImage, L"Insert image…", L"I", L"screenshot overlay picture layer", 0xEB9F},
                      {ActSpaceBelow, L"Add space below for notes", L"", L"canvas margin extend border", 0xE740},
                      {ActSpaceRight, L"Add space on the right", L"", L"canvas margin extend border", 0xE740},
                      {ActEvenMargin, L"Add an even margin", L"", L"canvas margin extend border padding", 0xE740},
                      {ActResetFrame, L"Remove added space and crop", L"", L"canvas reset", 0xE7A7},
                      {ActFront, L"Bring to front", L"Ctrl+]", L"image layer order", 0xE8F4},
                      {ActBack, L"Send to back", L"Ctrl+[", L"image layer order", 0xE8F4},
                      {ActClose, L"Close editor", L"Esc", L"discard quit", 0xE711}};
    for (const auto& a : acts) items.push_back({400 + a.act, a.title, a.hint, a.kw, a.icon});
    for (int i = 0; i < (int)std::size(kTools); ++i)
        items.push_back({100 + i, std::wstring(L"Tool: ") + kTools[i].name, std::wstring(1, kTools[i].key), L"tool draw annotate", 0xE70F});
    for (int i = 0; i < (int)std::size(kColors); ++i)
        items.push_back({200 + i, std::wstring(L"Color: ") + kColorNames[i], std::to_wstring(i + 1), L"colour", 0xE790});
    for (int i = 0; i < kLevels; ++i)
        items.push_back({300 + i, L"Size " + std::to_wstring(i + 1), i == level_ ? L"current" : L"", L"thickness width stroke font", 0xE9E9});
    HWND h = hwnd;
    PaletteOptions opt;
    opt.placeholder = L"Editor commands…";
    ShowPalette(std::move(items), [h](int id) {
        if (Editor* e = FromHwnd(h)) e->RunCommand(id);
    }, opt);
}

// ---- input ----

// Handles of a selected image layer: the four corners.
std::vector<gp::PointF> Handles(const gp::RectF& r) {
    return {{r.X, r.Y}, {r.GetRight(), r.Y}, {r.X, r.GetBottom()}, {r.GetRight(), r.GetBottom()}};
}

void Editor::OnLButtonDown(int x, int y, bool dbl) {
    SetFocus(hwnd);
    // Any click commits the text being typed, except picking a color or size for it.
    bool styling = false;
    for (const auto& b : buttons_)
        if (PtInRect(&b.r, {x, y})) styling = b.kind == Button::KColor || b.kind == Button::KWidth;
    const bool wasTexting = texting_;
    if (texting_ && !styling) CommitText();
    for (const auto& b : barButtons_) {
        if (!PtInRect(&b.r, {x, y})) continue;
        if (b.act <= -10) {
            POINT p{b.r.left, b.r.top};
            ClientToScreen(hwnd, &p);
            CollageMenu(-10 - b.act, p);
        } else if (b.act < 0) {
            POINT p{b.r.left, b.r.top};
            ClientToScreen(hwnd, &p);
            FillMenu(p);
        } else {
            Action(b.act);
        }
        return;
    }
    for (const auto& b : buttons_) {
        if (!PtInRect(&b.r, {x, y})) continue;
        switch (b.kind) {
            case Button::KTool: SetTool((Tool)b.value); break;
            case Button::KColor: SetColor(b.value); break;
            case Button::KWidth: SetLevel(b.value); break;
            case Button::KAction: Action(b.value); break;
        }
        return;
    }
    if (y < S(kToolbarH) || y >= client_.bottom - S(kStatusH)) return;
    const gp::PointF p = ToImg(x, y);
    if (wasTexting && tool_ == Tool::Text) return;  // that click only finished the text
    const float z = Zoom();
    // Corner handles of a selected image layer resize it, whatever the tool.
    if (selected_ >= 0 && selected_ < (int)st_.annots.size() && st_.annots[selected_].type == Tool::Image) {
        const gp::RectF r = NormRect(st_.annots[selected_].pts[0], st_.annots[selected_].pts.back());
        for (const auto& h : Handles(r)) {
            if (std::hypot(h.X - p.X, h.Y - p.Y) > 8 / z) continue;
            PushUndo();
            resizing_ = Resizing{selected_, gp::PointF(h.X == r.X ? r.GetRight() : r.X, h.Y == r.Y ? r.GetBottom() : r.Y), r.Width / std::max(1.f, r.Height)};
            movingIndex_ = selected_;
            live_ = st_.annots[selected_];
            st_.annots.erase(st_.annots.begin() + selected_);
            Changed(true);
            SetCapture(hwnd);
            return;
        }
    }
    Annot a;
    a.type = tool_;
    a.color = kColors[color_];
    a.level = level_;
    a.unit = unit_;
    a.pts = {p, p};
    switch (tool_) {
        case Tool::Canvas: {
            const RECT f = Frame();
            const float t = 10 / z;
            int edges = 0;
            if (p.X >= f.left - t && p.X <= f.right + t && p.Y >= f.top - t && p.Y <= f.bottom + t) {
                if (std::fabs(p.X - f.left) <= t) edges |= 1;
                if (std::fabs(p.X - f.right) <= t) edges |= 2;
                if (std::fabs(p.Y - f.top) <= t) edges |= 4;
                if (std::fabs(p.Y - f.bottom) <= t) edges |= 8;
            }
            if (!edges) return;
            PushUndo();
            // Freeze the view (and keep the current picture) so it stays still under the mouse.
            HDC screen = GetDC(hwnd);
            HBITMAP copy = CreateCompatibleBitmap(screen, viewW_, viewH_);
            {
                MemDC d(copy, screen), sdc(viewCache_, screen);
                BitBlt(d, 0, 0, viewW_, viewH_, sdc, 0, 0, SRCCOPY);
            }
            ReleaseDC(hwnd, screen);
            frozen_ = Frozen{zoom_, {ox_, oy_}, {f.left, f.top}, copy, {ox_, oy_, ox_ + viewW_, oy_ + viewH_}};
            framing_ = Framing{edges, gp::RectF((float)f.left, (float)f.top, (float)RectW(f), (float)RectH(f)), p};
            SetCapture(hwnd);
            return;
        }
        case Tool::Select: {
            selected_ = HitTest(p);
            if (selected_ >= 0) {
                if (dbl && st_.annots[selected_].type == Tool::Text) {  // edit the text again
                    PushUndo();
                    live_ = st_.annots[selected_];
                    st_.annots.erase(st_.annots.begin() + selected_);
                    selected_ = -1;
                    texting_ = true;
                    Changed(false);
                    return;
                }
                PushUndo();
                movingIndex_ = selected_;
                live_ = st_.annots[selected_];
                st_.annots.erase(st_.annots.begin() + selected_);
                Changed(IsRasterTool(live_.type));
                moving_ = true;
                lastPt_ = moveStart_ = p;
                SetCapture(hwnd);
            }
            Invalidate();
            return;
        }
        case Tool::Step:
            a.pts = {p};
            a.step = st_.nextStep;
            PushUndo();
            st_.annots.push_back(a);
            ++st_.nextStep;
            Changed(false);
            return;
        case Tool::Text: {
            a.pts = {p};
            const RECT f = Frame();
            a.wrap = std::max(80 * unit_, f.right - p.X - 12 * unit_);  // wraps at the canvas edge
            live_ = a;
            texting_ = true;
            Invalidate();
            return;
        }
        case Tool::Pen: a.pts = {p}; break;
        default: break;
    }
    live_ = a;
    drawing_ = true;
    SetCapture(hwnd);
    Invalidate();
}

void Editor::OnMouseMove(int x, int y) {
    int hover = -1;
    for (int i = 0; i < (int)buttons_.size(); ++i)
        if (PtInRect(&buttons_[i].r, {x, y})) hover = i;
    int hb = -1;
    for (int i = 0; i < (int)barButtons_.size(); ++i)
        if (PtInRect(&barButtons_[i].r, {x, y})) hb = i;
    if (hover != hoverBtn_ || hb != hoverBar_) {
        hoverBtn_ = hover;
        hoverBar_ = hb;
        Invalidate();
    }
    const gp::PointF p = ToImg(x, y);
    if (framing_) {
        const Framing& f = *framing_;
        const float dx = p.X - f.from.X, dy = p.Y - f.from.Y, minSide = 16 * unit_;
        gp::RectF r = f.start;
        if (f.edges & 1) {
            r.X = std::min(f.start.GetRight() - minSide, f.start.X + dx);
            r.Width = f.start.GetRight() - r.X;
        }
        if (f.edges & 2) r.Width = std::max(minSide, f.start.Width + dx);
        if (f.edges & 4) {
            r.Y = std::min(f.start.GetBottom() - minSide, f.start.Y + dy);
            r.Height = f.start.GetBottom() - r.Y;
        }
        if (f.edges & 8) r.Height = std::max(minSide, f.start.Height + dy);
        if (GetKeyState(VK_MENU) < 0) {  // Alt: the opposite edge moves too
            const float gx = r.Width - f.start.Width, gy = r.Height - f.start.Height;
            if (f.edges & 3) r = gp::RectF(f.start.X - gx / 2, r.Y, f.start.Width + gx, r.Height);
            if (f.edges & 12) r = gp::RectF(r.X, f.start.Y - gy / 2, r.Width, f.start.Height + gy);
        }
        st_.crop = gp::RectF(std::round(r.X), std::round(r.Y), std::round(r.Width), std::round(r.Height));
        UpdateTitle();
        Invalidate();
        return;
    }
    if (resizing_) {
        const Resizing& rz = *resizing_;
        float w = std::fabs(p.X - rz.anchor.X), h = std::fabs(p.Y - rz.anchor.Y);
        if (GetKeyState(VK_SHIFT) >= 0) {  // keeps proportions unless Shift
            if (w / std::max(1.f, h) > rz.aspect) h = w / rz.aspect;
            else w = h * rz.aspect;
        }
        w = std::max(8.f, w);
        h = std::max(8.f, h);
        const float nx = p.X < rz.anchor.X ? rz.anchor.X - w : rz.anchor.X, ny = p.Y < rz.anchor.Y ? rz.anchor.Y - h : rz.anchor.Y;
        live_.pts = {gp::PointF(nx, ny), gp::PointF(nx + w, ny + h)};
        Invalidate();
        return;
    }
    if (drawing_) {
        if (live_.type == Tool::Pen) {
            const gp::PointF& last = live_.pts.back();
            if (std::hypot(p.X - last.X, p.Y - last.Y) * Zoom() >= 1.5f) live_.pts.push_back(p);
        } else {
            live_.pts[1] = Constrain(live_.pts[0], p);
        }
        Invalidate();
    } else if (moving_) {
        const float dx = p.X - lastPt_.X, dy = p.Y - lastPt_.Y;
        for (auto& q : live_.pts) {
            q.X += dx;
            q.Y += dy;
        }
        lastPt_ = p;
        Invalidate();
    }
}

void Editor::OnLButtonUp(int x, int y) {
    if (drawing_) OnMouseMove(x, y);  // include the release point
    const bool wasDrawing = drawing_, wasMoving = moving_;
    // Clear state *before* releasing capture: WM_CAPTURECHANGED would otherwise cancel the shape.
    drawing_ = moving_ = false;
    std::optional<Framing> framing = framing_;
    std::optional<Resizing> resizing = resizing_;
    framing_.reset();
    resizing_.reset();
    if (GetCapture() == hwnd) ReleaseCapture();
    if (framing) {
        if (frozen_) {
            DeleteObject(frozen_->view);
            frozen_.reset();
        }
        Changed(true);
        return;
    }
    if (resizing) {
        st_.annots.insert(st_.annots.begin() + std::min<size_t>(movingIndex_, st_.annots.size()), live_);
        selected_ = movingIndex_;
        Changed(true);
        return;
    }
    if (wasMoving) {
        // In a collage, dropping one screenshot on another swaps them.
        if (collage_ && live_.layer && live_.layer->slot >= 0 && std::hypot(lastPt_.X - moveStart_.X, lastPt_.Y - moveStart_.Y) * Zoom() > 12) {
            const gp::RectF r = NormRect(live_.pts[0], live_.pts.back());
            const gp::PointF c(r.X + r.Width / 2, r.Y + r.Height / 2);
            for (const auto& other : st_.annots) {
                if (!other.layer || other.layer->slot < 0 || !NormRect(other.pts[0], other.pts.back()).Contains(c)) continue;
                const int a = live_.layer->slot, b = other.layer->slot;
                st_ = undo_.back();  // the swap records its own undo step
                undo_.pop_back();
                SwapCollage(a, b);
                return;
            }
        }
        st_.annots.insert(st_.annots.begin() + std::min<size_t>(movingIndex_, st_.annots.size()), live_);
        selected_ = movingIndex_;
        Changed(IsRasterTool(live_.type));
        return;
    }
    if (!wasDrawing) return;
    gp::RectF r = NormRect(live_.pts[0], live_.pts.back());
    const float minSize = 3 / Zoom();
    if (live_.type == Tool::Crop) {
        // A drag that misses the canvas would crop to nothing, so it's ignored.
        const RECT f = Frame();
        RECT want{(LONG)std::floor(r.X), (LONG)std::floor(r.Y), (LONG)std::ceil(r.GetRight()), (LONG)std::ceil(r.GetBottom())}, c;
        if (r.Width >= minSize && r.Height >= minSize && IntersectRect(&c, &want, &f) && RectW(c) >= 1 && RectH(c) >= 1) {
            PushUndo();
            st_.crop = gp::RectF((float)c.left, (float)c.top, (float)RectW(c), (float)RectH(c));
            Changed(true);
        }
        Invalidate();
        return;
    }
    if (live_.type != Tool::Pen && r.Width < minSize && r.Height < minSize) {  // just a click: nothing to add
        Invalidate();
        return;
    }
    PushUndo();
    st_.annots.push_back(live_);
    Changed(IsPixelTool(live_.type));
}

void Editor::OnRButtonUp(int x, int y) {
    if (y < S(kToolbarH) || y >= client_.bottom - S(kStatusH) || drawing_ || moving_) return;
    const int i = HitTest(ToImg(x, y));
    if (i < 0 || st_.annots[i].type != Tool::Image || !st_.annots[i].layer) return;
    selected_ = i;
    Invalidate();
    POINT p{x, y};
    ClientToScreen(hwnd, &p);
    LayerMenu(i, p);
}

void Editor::OnDropFiles(HDROP drop) {
    POINT pt{};
    DragQueryPoint(drop, &pt);
    std::vector<std::wstring> files;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        wchar_t path[MAX_PATH * 2];
        if (DragQueryFileW(drop, i, path, (UINT)std::size(path)) && MediaTypeOf(path) == MediaType::Image && IsMediaFile(path)) files.push_back(path);
    }
    DragFinish(drop);
    if (files.empty()) return (void)ShowToast(L"Drop an image to add it", L"PNG, JPEG, BMP, WebP or TIFF", nullptr, nullptr, 2200);
    const gp::PointF at = ToImg(pt.x, pt.y);
    InsertImages(files, &at);
}

void Editor::OnKeyDown(WPARAM vk) {
    // AltGr arrives as Ctrl+Alt: typing "ż" or "@" must not trigger Ctrl shortcuts.
    const bool ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (Gesture()) {  // mid-drag only Esc does anything: other commands would act on a half-moved document
        if (vk == VK_ESCAPE) {
            CancelLive();
            FlushRedactions();
        }
        return;
    }
    if (ctrl) {
        switch (vk) {
            case 'Z': shift ? Redo() : Undo(); return;
            case 'Y': Redo(); return;
            case 'C': shift ? CopyAnnotations() : Copy(); return;
            case 'V':
                if (shift) PasteAnnotations();
                else if (texting_) {
                    if (OpenClipboard(hwnd)) {
                        if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                            if (auto* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                                for (; *t; ++t)
                                    if (*t >= 32 || *t == L'\n') live_.text += *t;
                                GlobalUnlock(h);
                            }
                        CloseClipboard();
                    }
                    Invalidate();
                } else if (!PasteImage()) {
                    MessageBeep(MB_OK);
                }
                return;
            case 'D': DuplicateSelected(); return;
            case 'E': Action(ActStyle); return;
            case 'R': AutoRedact(); return;
            case 'S': shift ? SaveAs() : Save(); return;
            case 'P': Pin(); return;
            case 'K': OpenCommandPalette(); return;
            case 'W': Close(); return;
            case VK_OEM_6: Reorder(true); return;   // Ctrl+]
            case VK_OEM_4: Reorder(false); return;  // Ctrl+[
        }
        return;
    }
    if (texting_) {
        if (vk == VK_ESCAPE) CommitText();
        return;  // characters arrive through WM_CHAR
    }
    if (drawing_ || moving_ || resizing_ || framing_) {
        if (vk == VK_ESCAPE) CancelLive();
        return;
    }
    switch (vk) {
        case VK_ESCAPE: Close(); return;
        case VK_RETURN: Done(); return;
        case VK_DELETE:
        case VK_BACK: DeleteSelected(); return;
        case VK_OEM_4: SetLevel(level_ - 1); return;
        case VK_OEM_6: SetLevel(level_ + 1); return;
    }
    if (vk >= '1' && vk < '1' + (int)std::size(kColors)) return SetColor((int)(vk - '1'));
    for (const auto& t : kTools)
        if (vk == (WPARAM)t.key) return SetTool(t.tool);
}

void Editor::OnChar(wchar_t ch) {
    if (!texting_) return;
    if (ch == L'\r') {
        if (GetKeyState(VK_SHIFT) < 0) live_.text += L'\n';
        else CommitText();
    } else if (ch == 8) {
        if (!live_.text.empty()) live_.text.pop_back();
    } else if (ch >= 32 && ch != 127) {
        live_.text += ch;
    }
    Invalidate();
}

// ---- painting ----

void Editor::DrawToolbar(HDC dc) {
    const int tb = S(kToolbarH);
    FillSolid(dc, {0, 0, client_.right, tb}, theme::kSurface);
    FillSolid(dc, {0, tb - 1, client_.right, tb}, theme::kBorder);
    gp::Graphics g(dc);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < (int)buttons_.size(); ++i) {
        const Button& b = buttons_[i];
        const bool hover = i == hoverBtn_;
        switch (b.kind) {
            case Button::KTool: {
                bool active = (int)tool_ == b.value;
                if (active || hover) FillRounded(dc, b.r, S(7), active ? theme::kSelected : theme::kBgRaised);
                gp::Color c = active ? GC(theme::kAccent) : GC(hover ? theme::kText : theme::kTextDim);
                DrawToolIcon(g, kTools[b.value].tool, b.r, s_, c);
                break;
            }
            case Button::KColor: {
                gp::RectF rc((float)b.r.left, (float)b.r.top, (float)RectW(b.r), (float)RectH(b.r));
                if (b.value == color_) {
                    gp::Pen ring(GC(theme::kText), 2 * s_);
                    g.DrawEllipse(&ring, rc);
                    rc.Inflate(-4 * s_, -4 * s_);
                } else {
                    rc.Inflate(-(hover ? 2 : 3) * s_, -(hover ? 2 : 3) * s_);
                }
                gp::SolidBrush br(GC(kColors[b.value]));
                g.FillEllipse(&br, rc);
                if (b.value == 7) {  // black needs an outline on a dark toolbar
                    gp::Pen o(GC(theme::kBorder), s_);
                    g.DrawEllipse(&o, rc);
                }
                break;
            }
            case Button::KWidth: {
                bool active = b.value == level_;
                if (active || hover) FillRounded(dc, b.r, S(6), active ? theme::kSelected : theme::kBgRaised);
                float d = (3 + b.value * 2.6f) * s_;
                float cx = (b.r.left + b.r.right) / 2.f, cy = (b.r.top + b.r.bottom) / 2.f;
                gp::SolidBrush br(GC(active ? theme::kAccent : theme::kTextDim));
                g.FillEllipse(&br, cx - d / 2, cy - d / 2, d, d);
                break;
            }
            case Button::KAction: {
                RECT r = b.r;
                if (b.value == ActDone) {
                    FillRounded(dc, r, S(7), hover ? theme::kAccentSoft : theme::kAccent);
                    HGDIOBJ of = SelectObject(dc, fUi_);
                    SetTextColor(dc, theme::kOnAccent);
                    DrawTextW(dc, L"DONE", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(dc, of);
                    break;
                }
                bool enabled = !((b.value == ActUndo && undo_.empty()) || (b.value == ActRedo && redo_.empty()));
                if (hover && enabled) FillRounded(dc, r, S(7), theme::kBgRaised);
                const wchar_t glyph = b.value == ActUndo ? 0xE7A7 : b.value == ActRedo ? 0xE7A6 : b.value == ActCopy ? 0xE8C8 : b.value == ActPin ? 0xE840 : 0xE74E;
                HGDIOBJ of = SelectObject(dc, fIcon_);
                SetTextColor(dc, enabled ? (hover ? theme::kText : theme::kTextDim) : theme::kBorder);
                DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, of);
                break;
            }
        }
    }
}

void Editor::DrawStatus(HDC dc) {
    const int top = client_.bottom - S(kStatusH);
    FillSolid(dc, {0, top, client_.right, client_.bottom}, theme::kSurface);
    FillSolid(dc, {0, top, client_.right, top + 1}, theme::kBorder);
    HGDIOBJ of = SelectObject(dc, fStatus_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::kMuted);
    RECT r{S(12), top, client_.right - S(12), client_.bottom};
    std::wstring left = hoverBtn_ >= 0 ? buttons_[hoverBtn_].tip : std::wstring(CurTool().name) + L"  —  " + CurTool().hint;
    const RECT f = Frame();
    wchar_t right[160];
    swprintf_s(right, L"%s%d × %d   ·   %d%%   ·   Ctrl+K commands", styled_ ? L"Styled export   ·   " : L"", RectW(f), RectH(f),
               (int)std::lround(Zoom() * 100));
    DrawTextW(dc, right, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE rs{};
    GetTextExtentPoint32W(dc, right, (int)wcslen(right), &rs);
    r.right -= rs.cx + S(24);
    DrawTextW(dc, left.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

// The canvas tool's floating bar: Space below, Space right, Even margin | Fill ▾, Reset.
void Editor::DrawCanvasBar(HDC dc) {
    barButtons_.clear();
    if (tool_ != Tool::Canvas) return;
    const COLORREF edge = EdgeColor(*base_);
    const std::wstring fill = !st_.fill ? L"Transparent" : *st_.fill == edge ? L"Edge color" : *st_.fill == RGB(255, 255, 255) ? L"White"
                                                       : *st_.fill == RGB(0, 0, 0)                     ? L"Black"
                                                                                                        : L"Custom";
    struct Item {
        int act;
        std::wstring label;
    };
    const Item items[] = {{ActSpaceBelow, L"Space below"}, {ActSpaceRight, L"Space right"}, {ActEvenMargin, L"Even margin"},
                          {-1, L"Fill: " + fill + L"  ▾"}, {ActResetFrame, L"Reset"}};
    HGDIOBJ of = SelectObject(dc, fBar_);
    int total = S(10);
    std::vector<int> widths;
    for (const auto& it : items) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, it.label.c_str(), (int)it.label.size(), &sz);
        widths.push_back(sz.cx + S(20));
        total += sz.cx + S(20) + S(4) + (it.act == -1 ? S(9) : 0);
    }
    const RECT c = Canvas();
    const int h = S(kBarH), x0 = (c.left + c.right) / 2 - total / 2, y0 = c.bottom - S(14) - h;
    RECT bar{x0, y0, x0 + total, y0 + h};
    FillRounded(dc, bar, S(12), RGB(30, 31, 29), RGB(58, 59, 56));
    int x = bar.left + S(6);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < std::size(items); ++i) {
        if (items[i].act == -1) {  // separator before Fill
            FillSolid(dc, {x + S(2), bar.top + S(11), x + S(3), bar.bottom - S(11)}, RGB(70, 71, 68));
            x += S(9);
        }
        RECT r{x, bar.top + S(5), x + widths[i], bar.bottom - S(5)};
        if ((int)barButtons_.size() == hoverBar_) FillRounded(dc, r, S(7), theme::kBgRaised);
        SetTextColor(dc, theme::kText);
        DrawTextW(dc, items[i].label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        barButtons_.push_back({r, items[i].act, items[i].label});
        x = r.right + S(4);
    }
    SelectObject(dc, of);
}

// Lays the collage out again (after a change to its options or order).
void Editor::ApplyCollage(bool undoable) {
    if (!collage_) return;
    CommitText();
    if (undoable) PushUndo();
    const CollageResult r = LayoutCollage(*collage_, unit_);
    std::vector<Annot> layers, others;
    for (const auto& a : st_.annots)
        if (!(a.layer && a.layer->slot >= 0)) others.push_back(a);
    for (size_t pos = 0; pos < collage_->order.size(); ++pos) {
        const int slot = collage_->order[pos];
        const CollageRect& rc = r.rects[pos];
        Annot a;
        a.type = Tool::Image;
        a.color = kColors[6];
        a.level = 0;
        a.unit = unit_;
        a.pts = {gp::PointF((float)rc.x, (float)rc.y), gp::PointF((float)(rc.x + rc.w), (float)(rc.y + rc.h))};
        auto l = std::make_shared<ImageLayer>();
        l->image = collageImages_[slot];
        l->source = collageSources_[slot];
        l->radius = (float)r.radius;
        l->shadow = collage_->shadow;
        l->slot = slot;
        a.layer = l;
        layers.push_back(a);
    }
    layers.insert(layers.end(), others.begin(), others.end());
    st_.annots = layers;
    st_.crop = gp::RectF(0, 0, (float)r.width, (float)r.height);
    st_.fill = CollageSpec::kBackgrounds[collage_->background].color;
    selected_ = -1;
    Changed(true);
}

void Editor::SwapCollage(int a, int b) {
    auto& o = collage_->order;
    auto i = std::find(o.begin(), o.end(), a), j = std::find(o.begin(), o.end(), b);
    if (i == o.end() || j == o.end()) return;
    std::iter_swap(i, j);
    ApplyCollage(true);
}

// Layout, Gap, Margin, Background, Corners, Shadow, Size.
void Editor::CollageMenu(int which, POINT at) {
    if (!collage_) return;
    CollageSpec& c = *collage_;
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> items;
    int cur = 0;
    switch (which) {
        case 0:
            for (int i = 0; i <= (int)CollageLayout::Feature; ++i) items.push_back(CollageLayoutLabel((CollageLayout)i));
            cur = (int)c.layout;
            break;
        case 1:
        case 2:
            for (auto n : CollageSpec::kStepNames) items.push_back(n);
            cur = which == 1 ? c.gap : c.margin;
            break;
        case 3:
            for (const auto& b : CollageSpec::kBackgrounds) items.push_back(b.name);
            cur = c.background;
            break;
        case 4:
            items = {L"Rounded", L"Square"};
            cur = c.rounded ? 0 : 1;
            break;
        case 5:
            items = {L"No shadow", L"Shadow"};
            cur = c.shadow ? 1 : 0;
            break;
        case 6:
            for (int i = 0; i <= (int)CollageSize::Square; ++i) items.push_back(CollageSizeLabel((CollageSize)i));
            cur = (int)c.size;
            break;
    }
    for (size_t i = 0; i < items.size(); ++i) AppendMenuW(m, MF_STRING, i + 1, items[i].c_str());
    CheckMenuRadioItem(m, 1, (UINT)items.size(), cur + 1, MF_BYCOMMAND);
    SetForegroundWindow(hwnd);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_BOTTOMALIGN, at.x, at.y, 0, hwnd, nullptr);
    DestroyMenu(m);
    if (!id) return;
    const int v = id - 1;
    switch (which) {
        case 0: c.layout = (CollageLayout)v; break;
        case 1: c.gap = v; break;
        case 2: c.margin = v; break;
        case 3: c.background = v; break;
        case 4: c.rounded = v == 0; break;
        case 5: c.shadow = v == 1; break;
        case 6: c.size = (CollageSize)v; break;
    }
    ApplyCollage(true);
}

void Editor::DrawCollageBar(HDC dc) {
    if (!collage_ || tool_ == Tool::Canvas) return;
    const CollageSpec& c = *collage_;
    const std::wstring labels[] = {std::wstring(L"Layout: ") + CollageLayoutLabel(c.layout),
                                   std::wstring(L"Gap: ") + CollageSpec::kStepNames[c.gap],
                                   std::wstring(L"Margin: ") + CollageSpec::kStepNames[c.margin],
                                   std::wstring(L"Background: ") + CollageSpec::kBackgrounds[c.background].name,
                                   c.rounded ? L"Corners: Rounded" : L"Corners: Square",
                                   c.shadow ? L"Shadow" : L"No shadow",
                                   CollageSizeLabel(c.size)};
    HGDIOBJ of = SelectObject(dc, fBar_);
    std::vector<int> widths;
    int total = S(10);
    for (const auto& l : labels) {
        SIZE sz{};
        const std::wstring withArrow = l + L"  \u25BE";
        GetTextExtentPoint32W(dc, withArrow.c_str(), (int)withArrow.size(), &sz);
        widths.push_back(sz.cx + S(16));
        total += sz.cx + S(16) + S(2);
    }
    const RECT cv = Canvas();
    const int h = S(kBarH), x0 = std::max<int>(cv.left + S(8), (cv.left + cv.right) / 2 - total / 2), y0 = cv.bottom - S(14) - h;
    RECT bar{x0, y0, x0 + total, y0 + h};
    FillRounded(dc, bar, S(12), RGB(30, 31, 29), RGB(58, 59, 56));
    int x = bar.left + S(6);
    SetBkMode(dc, TRANSPARENT);
    for (size_t i = 0; i < std::size(labels); ++i) {
        RECT r{x, bar.top + S(5), x + widths[i], bar.bottom - S(5)};
        if ((int)barButtons_.size() == hoverBar_) FillRounded(dc, r, S(7), theme::kBgRaised);
        SetTextColor(dc, theme::kText);
        const std::wstring withArrow = labels[i] + L"  \u25BE";
        DrawTextW(dc, withArrow.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        barButtons_.push_back({r, -10 - (int)i, labels[i]});
        x = r.right + S(2);
    }
    SelectObject(dc, of);
}
void DrawLayerPreview(gp::Graphics& g, const Annot& a) {
    if (!a.layer || !a.layer->image) return;
    const gp::RectF r = NormRect(a.pts[0], a.pts.back());
    const Bitmap& img = *a.layer->image;
    gp::Bitmap src(img.Width(), img.Height(), img.Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(img.Bits()));
    const auto old = g.GetInterpolationMode();
    g.SetInterpolationMode(gp::InterpolationModeBilinear);
    g.DrawImage(&src, r);
    g.SetInterpolationMode(old);
}

void Editor::Paint(HDC hdc) {
    const int w = std::max(1L, client_.right), h = std::max(1L, client_.bottom);
    if (w > backW_ || h > backH_) {
        if (backDC_) {
            SelectObject(backDC_, backOld_);
            DeleteObject(back_);
        } else {
            backDC_ = CreateCompatibleDC(hdc);
        }
        backW_ = std::max(backW_, w);
        backH_ = std::max(backH_, h);
        back_ = CreateCompatibleBitmap(hdc, backW_, backH_);
        backOld_ = SelectObject(backDC_, back_);
    }
    HDC dc = backDC_;
    FillSolid(dc, {0, 0, w, h}, theme::kBg);
    const RECT shown = Shown();
    if (frozen_) {
        // An edge is being dragged: the new frame in its fill, the picture where it was.
        if (st_.fill) FillSolid(dc, shown, *st_.fill);
        else
            for (int y = shown.top; y < shown.bottom; y += S(8))
                for (int x = shown.left; x < shown.right; x += S(8))
                    FillSolid(dc, {x, y, std::min<int>(x + S(8), shown.right), std::min<int>(y + S(8), shown.bottom)},
                              (((x - shown.left) / S(8) + (y - shown.top) / S(8)) % 2) ? RGB(180, 180, 180) : RGB(217, 217, 217));
        MemDC v(frozen_->view, hdc);
        const RECT& o = frozen_->shown;
        BitBlt(dc, o.left, o.top, RectW(o), RectH(o), v, 0, 0, SRCCOPY);
    } else {
        FrameSolid(dc, {ox_ - 1, oy_ - 1, ox_ + viewW_ + 1, oy_ + viewH_ + 1}, theme::kBorder);
        MemDC v(viewCache_, hdc);
        BitBlt(dc, ox_, oy_, viewW_, viewH_, v, 0, 0, SRCCOPY);
    }
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        const RECT canvas = Canvas();
        g.SetClip(gp::Rect(canvas.left, canvas.top, RectW(canvas), RectH(canvas)));
        const float z = Zoom();
        const POINT o = Origin(), m = ViewMin();
        g.TranslateTransform((float)o.x, (float)o.y);
        g.ScaleTransform(z, z);
        g.TranslateTransform((float)-m.x, (float)-m.y);
        const float px = 1 / z;  // one screen pixel in image units
        const RECT f = Frame();
        const gp::RectF fr((float)f.left, (float)f.top, (float)RectW(f), (float)RectH(f));
        if (drawing_ && live_.type == Tool::Crop) {
            gp::RectF r = NormRect(live_.pts[0], live_.pts.back());
            gp::Region outside(fr);
            outside.Exclude(r);
            gp::SolidBrush dim(gp::Color(150, 0, 0, 0));
            g.FillRegion(&dim, &outside);
            gp::Pen pen(GC(theme::kAccent), 2 * s_ * px);
            g.DrawRectangle(&pen, r);
        } else if (drawing_ || moving_ || texting_ || resizing_) {
            if (live_.type == Tool::Image) DrawLayerPreview(g, live_);
            else if (!(texting_ && live_.text.empty())) DrawAnnot(g, live_);
            if (texting_) {
                gp::RectF tb = MeasureText(live_, live_.text);
                const size_t nl = live_.text.find_last_of(L'\n');
                Annot lastLine = live_;
                lastLine.text = nl == std::wstring::npos ? live_.text : live_.text.substr(nl + 1);
                lastLine.wrap = 0;
                gp::RectF lb = MeasureText(lastLine, lastLine.text);
                const float lineH = MeasureText(lastLine, L"").Height;
                const float cx = std::min(tb.GetRight(), live_.pts[0].X + lb.Width) + 2 * px;
                const float cy = live_.pts[0].Y + std::max(tb.Height, lineH) - lineH;
                gp::Pen caret(GC(live_.color), std::max(2.f * s_ * px, 1.f));
                g.DrawLine(&caret, cx, cy, cx, cy + lineH);
                gp::RectF box(live_.pts[0].X - 4 * px, live_.pts[0].Y - 3 * px, std::max(tb.Width, 40 * px) + 10 * px, std::max(tb.Height, lineH) + 6 * px);
                gp::Pen dash(gp::Color(160, 255, 255, 255), px);
                dash.SetDashStyle(gp::DashStyleDash);
                g.DrawRectangle(&dash, box);
                if (live_.wrap > 0) {  // the wrap edge, faintly
                    gp::Pen edge(GC(theme::kAccent, 90), px);
                    edge.SetDashStyle(gp::DashStyleDot);
                    const float ex = live_.pts[0].X + live_.wrap;
                    g.DrawLine(&edge, ex, box.Y, ex, box.GetBottom());
                }
            }
        }
        if (selected_ >= 0 && selected_ < (int)st_.annots.size() && !moving_ && !resizing_) {
            const Annot& a = st_.annots[selected_];
            gp::Pen sel(GC(theme::kAccentSoft), 1.5f * s_ * px);
            sel.SetDashStyle(gp::DashStyleDash);
            gp::RectF b = Bounds(a);
            b.Inflate(2 * px, 2 * px);
            g.DrawRectangle(&sel, b);
            if (a.type == Tool::Image) {
                gp::SolidBrush white(gp::Color(255, 255, 255, 255));
                gp::Pen ring(GC(theme::kBg), 1.2f * px);
                for (const auto& hp : Handles(NormRect(a.pts[0], a.pts.back()))) {
                    g.FillEllipse(&white, hp.X - 5 * px, hp.Y - 5 * px, 10 * px, 10 * px);
                    g.DrawEllipse(&ring, hp.X - 5 * px, hp.Y - 5 * px, 10 * px, 10 * px);
                }
            }
        }
        if (tool_ == Tool::Canvas) {
            gp::Pen dash(gp::Color(128, 255, 255, 255), px);
            dash.SetDashStyle(gp::DashStyleDash);
            g.DrawRectangle(&dash, Extent(*base_));  // where the original screenshot is
            gp::SolidBrush acc(GC(theme::kAccent));
            const float mx = fr.X + fr.Width / 2, my = fr.Y + fr.Height / 2;
            const gp::PointF mids[] = {{mx, fr.Y}, {mx, fr.GetBottom()}, {fr.X, my}, {fr.GetRight(), my}};
            for (int i = 0; i < 4; ++i) {
                const bool horizontal = i < 2;
                const float hw = (horizontal ? 36 : 6) * px, hh = (horizontal ? 6 : 36) * px;
                gp::GraphicsPath p;
                RoundedPath(p, gp::RectF(mids[i].X - hw / 2, mids[i].Y - hh / 2, hw, hh), 3 * px);
                g.FillPath(&acc, &p);
            }
        }
    }
    DrawToolbar(dc);
    DrawStatus(dc);
    DrawCanvasBar(dc);
    DrawCollageBar(dc);
    BitBlt(hdc, 0, 0, w, h, dc, 0, 0, SRCCOPY);
}

LRESULT Editor::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            RECT r{0, 0, minClientW_, S(360)};
            AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)(s_ * 96 + 0.5f));
            mmi->ptMinTrackSize = {RectW(r), RectH(r)};
            return 0;
        }
        case WM_DPICHANGED: {
            s_ = HIWORD(w) / 96.f;
            RebuildFonts();
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) {  // the editor "Add to open editor" uses
                g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), this), g_editors.end());
                g_editors.push_back(this);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                LPCWSTR cur = IDC_ARROW;
                const RECT sh = Shown();
                const bool inView = PtInRect(&sh, p) && p.y >= S(kToolbarH) && p.y < client_.bottom - S(kStatusH);
                bool overBar = false;
                for (const auto& b : barButtons_) overBar = overBar || PtInRect(&b.r, p);
                if (hoverBtn_ >= 0 || overBar) {
                    cur = IDC_HAND;
                } else if (tool_ == Tool::Canvas) {
                    const gp::PointF ip = ToImg(p.x, p.y);
                    const RECT f = Frame();
                    const float t = 10 / Zoom();
                    const bool lr = std::fabs(ip.X - f.left) <= t || std::fabs(ip.X - f.right) <= t;
                    const bool tb = std::fabs(ip.Y - f.top) <= t || std::fabs(ip.Y - f.bottom) <= t;
                    cur = lr && tb ? IDC_SIZENWSE : lr ? IDC_SIZEWE : tb ? IDC_SIZENS : IDC_ARROW;
                } else if (inView) {
                    if (tool_ == Tool::Text) cur = IDC_IBEAM;
                    else if (tool_ == Tool::Select) cur = HitTest(ToImg(p.x, p.y)) >= 0 ? IDC_SIZEALL : IDC_ARROW;
                    else cur = IDC_CROSS;
                }
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: OnLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l), m == WM_LBUTTONDBLCLK); return 0;
        case WM_MOUSEMOVE: {
            OnMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverBtn_ >= 0 || hoverBar_ >= 0) {
                hoverBtn_ = hoverBar_ = -1;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            FlushRedactions();
            return 0;
        case WM_RBUTTONUP: OnRButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
        case WM_MOUSEWHEEL:
            if (!Gesture()) SetLevel(level_ + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1));  // not the shape being dragged
            return 0;
        case WM_KEYDOWN: OnKeyDown(w); return 0;
        case WM_CHAR: OnChar((wchar_t)w); return 0;
        case WM_DROPFILES: OnDropFiles((HDROP)w); return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)l != hwnd && (drawing_ || moving_ || resizing_ || framing_)) CancelLive();
            FlushRedactions();
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE: Close(); return 0;
        case WM_DESTROY:
            if (viewCache_) DeleteObject(viewCache_);
            if (frozen_) DeleteObject(frozen_->view);
            if (backDC_) {
                SelectObject(backDC_, backOld_);
                DeleteObject(back_);
                DeleteDC(backDC_);
            }
            for (HFONT f : {fUi_, fIcon_, fStatus_, fBar_}) DeleteObject(f);
            return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

}  // namespace

void SetEditorDefaults(const EditorOptions& opt) { g_defaults = opt; }

void OpenEditor(BitmapPtr img, const EditorSource& source) {
    if (!img) return;
    auto* e = new Editor(img->Crop({0, 0, img->Width(), img->Height()}), source);
    if (!e->Create()) delete e;
}

bool OpenEditorFile(const std::wstring& path) {
    BitmapPtr img = LoadImageFile(path);
    if (!img) {
        ShowToast(L"Can't open that image", FileNameOf(path), nullptr, nullptr, 2500);
        return false;
    }
    const ItemMeta& m = Library::Shared().Meta(path);
    OpenEditor(img, {path, m.app, m.window});
    return true;
}

int EditorCount() { return (int)g_editors.size(); }

bool AddImagesToEditor(const std::vector<std::wstring>& paths) {
    if (g_editors.empty() || paths.empty()) return false;
    g_editors.back()->InsertImages(paths);
    return true;
}

bool OpenCollage(const std::vector<std::wstring>& paths) {
    std::vector<BitmapPtr> imgs;
    std::vector<std::wstring> sources;
    for (const auto& p : paths)
        if (auto img = LoadImageFile(p)) {
            imgs.push_back(img);
            sources.push_back(p);
        }
    if (imgs.size() < 2) {
        ShowToast(L"Pick at least two images for a collage", L"", nullptr, nullptr, 2500);
        return false;
    }
    auto blank = Bitmap::Create(1, 1);
    blank->Bits()[0] = 0;  // transparent: a collage has no screenshot underneath
    auto* e = new Editor(blank, {});
    std::vector<SIZE> sizes;
    for (const auto& i : imgs) sizes.push_back({i->Width(), i->Height()});
    e->collage_ = CollageSpec(sizes);
    e->collageImages_ = imgs;
    e->collageSources_ = sources;
    e->tool_ = Tool::Select;  // move and swap screenshots first
    POINT pt;
    GetCursorPos(&pt);
    e->unit_ = DpiScaleAt(pt);
    e->ApplyCollage(false);
    e->dirty_ = true;
    if (!e->Create()) {
        delete e;
        return false;
    }
    return true;
}
// Developer tool: `--editor-snapshots <dir>` renders the editor in several states to PNGs.
int EditorSnapshots(const std::wstring& outDir) {
    SHCreateDirectoryExW(nullptr, outDir.c_str(), nullptr);
    auto card = [](int w, int h, COLORREF a, COLORREF b, const wchar_t* title) {
        auto bmp = Bitmap::Create(w, h);
        {
            gp::Bitmap gb(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(bmp->Bits()));
            gp::Graphics g(&gb);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
            gp::LinearGradientBrush bg(gp::Point(0, 0), gp::Point(w, h), GC(a), GC(b));
            g.FillRectangle(&bg, 0, 0, w, h);
            gp::GraphicsPath p;
            RoundedPath(p, gp::RectF(w / 12.f, h / 6.f, w * 5 / 6.f, h * 2 / 3.f), 18);
            gp::SolidBrush white(gp::Color(235, 255, 255, 255));
            g.FillPath(&white, &p);
            gp::Font f(L"Segoe UI", h / 12.f, gp::FontStyleBold, gp::UnitPixel);
            gp::SolidBrush ink(gp::Color(255, 0, 0, 0));
            g.DrawString(title, -1, &f, gp::PointF(w / 8.f, h / 4.f), &ink);
        }
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) bmp->Bits()[i] |= 0xFF000000u;
        return bmp;
    };
    auto* e = new Editor(card(1600, 900, RGB(48, 176, 199), RGB(10, 132, 255), L"Weekly revenue"), {});
    e->hidden_ = true;
    if (!e->Create()) return 1;
    RECT wr{0, 0, std::max(1500, e->minClientW_), 900};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(e->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    e->Layout();
    auto snap = [&](const wchar_t* name) {
        e->Layout();
        RECT rc;
        GetClientRect(e->hwnd, &rc);
        auto out = Bitmap::Create(RectW(rc), RectH(rc));
        {
            MemDC dc(out->Handle());
            e->Paint(dc);
        }
        for (size_t i = 0, n = (size_t)out->Width() * out->Height(); i < n; ++i) out->Bits()[i] |= 0xFF000000u;
        SavePng(*out, outDir + L"\\" + name + L".png");
    };
    auto add = [&](Tool t, std::vector<gp::PointF> pts, int color, int level, const std::wstring& text = L"") {
        Annot a;
        a.type = t;
        a.color = kColors[color];
        a.level = level;
        a.unit = 1;
        a.pts = std::move(pts);
        a.text = text;
        if (t == Tool::Step) a.step = e->st_.nextStep++;
        if (t == Tool::Text) a.wrap = std::max(80.f, 1600 - a.pts[0].X - 12);
        e->st_.annots.push_back(a);
    };
    add(Tool::Arrow, {gp::PointF(1150, 700), gp::PointF(900, 420)}, 0, 2);
    add(Tool::Rect, {gp::PointF(180, 180), gp::PointF(820, 330)}, 5, 1);
    add(Tool::Step, {gp::PointF(160, 170)}, 0, 2);
    add(Tool::Blur, {gp::PointF(200, 520), gp::PointF(700, 640)}, 0, 2);
    add(Tool::Text, {gp::PointF(1180, 640)}, 0, 2, L"Tuesday dips here, and it's the third week in a row");
    e->Changed(true);
    snap(L"editor-annotations");
    e->Insert(card(1200, 800, RGB(255, 59, 48), RGB(255, 149, 0), L"Payment failed"), L"", nullptr);
    snap(L"editor-image-layer");
    e->SetTool(Tool::Canvas);
    e->ExtendFrame(Editor::Extend::Below);
    snap(L"editor-canvas-space");
    e->st_.fill.reset();
    e->ExtendFrame(Editor::Extend::Even);
    snap(L"editor-transparent-margin");
    e->SetTool(Tool::Text);
    e->live_ = Annot{};
    e->live_.type = Tool::Text;
    e->live_.color = kColors[6];
    e->live_.level = 3;
    e->live_.unit = 1;
    e->live_.pts = {gp::PointF(100, 1000)};
    e->live_.wrap = 900;
    e->live_.text = L"Notes wrap at the canvas edge while you type them, so they never run off the side";
    e->texting_ = true;
    snap(L"editor-typing");
    DestroyWindow(e->hwnd);

    // A collage of four screenshots.
    auto blank = Bitmap::Create(1, 1);
    blank->Bits()[0] = 0;
    auto* c = new Editor(blank, {});
    c->hidden_ = true;
    c->collageImages_ = {card(1600, 1000, RGB(255, 59, 48), RGB(255, 149, 0), L"Payment failed"),
                         card(900, 1400, RGB(88, 86, 214), RGB(175, 82, 222), L"Sign in"),
                         card(1400, 900, RGB(52, 199, 89), RGB(48, 176, 199), L"Release notes"),
                         card(1800, 1000, RGB(48, 176, 199), RGB(10, 132, 255), L"Weekly revenue")};
    c->collageSources_ = {L"", L"", L"", L""};
    std::vector<SIZE> sizes;
    for (const auto& i : c->collageImages_) sizes.push_back({i->Width(), i->Height()});
    c->collage_ = CollageSpec(sizes);
    c->collage_->shadow = true;
    c->collage_->background = 1;
    c->tool_ = Tool::Select;
    c->ApplyCollage(false);
    if (!c->Create()) return 1;
    SetWindowPos(c->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    e = c;
    snap(L"editor-collage");
    c->collage_->layout = CollageLayout::Feature;
    c->ApplyCollage(true);
    snap(L"editor-collage-feature");
    DestroyWindow(c->hwnd);
    return 0;
}

// ---- tests (CompositionTests.swift) ----

namespace {

BitmapPtr SolidBmp(int w, int h, uint32_t argb) {
    auto b = Bitmap::Create(w, h);
    std::fill_n(b->Bits(), (size_t)w * h, argb);
    return b;
}

uint32_t At(const Bitmap& b, int x, int y) { return b.Bits()[(size_t)y * b.Width() + x]; }
uint32_t Rgb(uint32_t p) { return p & 0xFFFFFF; }

}  // namespace

ATHER_TEST(editor_added_space_uses_fill_and_keeps_image) {
    auto base = SolidBmp(200, 100, 0xFFFF0000);
    DocState st;
    st.crop = gp::RectF(0, 0, 200, 180);  // 80 px of space below
    st.fill = RGB(255, 255, 255);
    auto out = Compose(*base, st);
    CHECK(out->Width() == 200 && out->Height() == 180);
    CHECK_EQ(Rgb(At(*out, 100, 50)), 0xFF0000u);
    CHECK_EQ(Rgb(At(*out, 100, 150)), 0xFFFFFFu);
    st.crop = gp::RectF(-20, -20, 240, 140);  // even margin, transparent
    st.fill.reset();
    auto clear = Compose(*base, st);
    CHECK_EQ(clear->Width(), 240);
    CHECK_EQ(At(*clear, 5, 5) >> 24, 0u);
    CHECK_EQ(Rgb(At(*clear, 120, 70)), 0xFF0000u);
    CHECK(HasAlpha(*clear));
    // A transparent PNG keeps its alpha.
    const std::wstring png = test::TempDir() + L"\\clear.png";
    CHECK(SavePng(*clear, png));
    int w = 0, h = 0;
    CHECK(ImageSize(png, &w, &h) && w == 240 && h == 140);
}

ATHER_TEST(editor_edge_color_matches_border) {
    CHECK_EQ(EdgeColor(*SolidBmp(50, 50, 0xFF0000FF)), RGB(0, 0, 255));
}

ATHER_TEST(editor_image_layer_is_flattened_under_annotations) {
    auto base = SolidBmp(300, 200, 0xFFFFFFFF);
    Annot a;
    a.type = Tool::Image;
    a.color = kColors[6];
    a.level = 0;
    a.unit = 1;
    a.pts = {gp::PointF(100, 50), gp::PointF(200, 150)};
    auto l = std::make_shared<ImageLayer>();
    l->image = SolidBmp(40, 40, 0xFF00FF00);
    a.layer = l;
    DocState st;
    st.annots = {a};
    auto out = Compose(*base, st);
    CHECK_EQ(Rgb(At(*out, 150, 100)), 0x00FF00u);
    CHECK_EQ(Rgb(At(*out, 50, 100)), 0xFFFFFFu);
    // Blur applies to the layer too, since layers are part of the background.
    Annot px;
    px.type = Tool::Blur;
    px.level = 4;
    px.unit = 1;
    px.pts = {gp::PointF(60, 20), gp::PointF(240, 180)};
    st.annots.push_back(px);
    CHECK(Rgb(At(*Compose(*base, st), 100, 50)) != 0x00FF00u);
    // A rectangle drawn after the blur stays sharp: vector annotations sit above the pixel tools.
    Annot rect;
    rect.type = Tool::Rect;
    rect.color = RGB(255, 0, 0);
    rect.level = 4;
    rect.unit = 1;
    rect.pts = {gp::PointF(80, 40), gp::PointF(220, 160)};
    st.annots.push_back(rect);
    CHECK_EQ(Rgb(At(*Compose(*base, st), 80, 100)), 0xFF0000u);
}

ATHER_TEST(editor_text_wraps_at_width) {
    Annot t;
    t.type = Tool::Text;
    t.level = 1;
    t.unit = 1;
    t.pts = {gp::PointF(0, 0)};
    t.text = L"A long remark that should wrap onto several lines here";
    const gp::RectF one = MeasureText(t, t.text);
    t.wrap = 160;
    const gp::RectF wrapped = MeasureText(t, t.text);
    CHECK(wrapped.Width <= 160.5f);
    CHECK(wrapped.Height > one.Height * 1.5f);
}

ATHER_TEST(editor_crop_outside_image_still_exports_annotations) {
    auto base = SolidBmp(200, 100, 0xFFFFFFFF);
    DocState st;
    Annot pix;
    pix.type = Tool::Pixelate;
    pix.unit = 1;
    pix.pts = {gp::PointF(0, 0), gp::PointF(200, 100)};
    Annot rect;
    rect.type = Tool::Rect;
    rect.color = RGB(255, 0, 0);
    rect.level = 4;
    rect.unit = 1;
    rect.pts = {gp::PointF(10, 10), gp::PointF(190, 90)};
    st.annots = {pix, rect};
    st.crop = gp::RectF(500, 500, 50, 50);  // misses the image entirely: the whole image is kept
    auto out = Compose(*base, st);
    CHECK_EQ(out->Width(), 200);
    CHECK(Rgb(At(*out, 10, 50)) != 0xFFFFFFu);  // the red rectangle is there
}

// Auto-redact finishing in the middle of a drag: the boxes wait for the drag to end, and cancelling the drag
// (which restores the document from before it) doesn't lose them.
namespace {
struct EditorTest {
    static void Redactions();
};
}  // namespace

ATHER_TEST(editor_redactions_survive_a_cancelled_drag) { EditorTest::Redactions(); }

void EditorTest::Redactions() {
    auto img = SolidBmp(400, 300, 0xFFFFFFFF);
    auto* e = new Editor(img, {});
    e->hidden_ = true;
    CHECK(e->Create());
    if (!e->hwnd) return;
    e->PushUndo();  // what a drag start does
    e->drawing_ = true;
    e->live_.type = Tool::Arrow;
    e->live_.pts = {gp::PointF(10, 10), gp::PointF(50, 50)};
    Annot box;
    box.type = Tool::Pixelate;
    box.pts = {gp::PointF(20, 20), gp::PointF(80, 40)};
    e->ReceiveRedactions({box});
    CHECK(std::none_of(e->st_.annots.begin(), e->st_.annots.end(), [](const Annot& a) { return a.type == Tool::Pixelate; }));
    e->OnKeyDown(VK_ESCAPE);  // cancel the drag
    CHECK(!e->Gesture());
    CHECK(std::any_of(e->st_.annots.begin(), e->st_.annots.end(), [](const Annot& a) { return a.type == Tool::Pixelate; }));
    e->dirty_ = false;
    DestroyWindow(e->hwnd);
}

// ---- shared with the video editor (annot.h) ----

namespace annot {

int ColorCount() { return (int)std::size(kColors); }
COLORREF Color(int i) { return kColors[std::clamp(i, 0, ColorCount() - 1)]; }
const wchar_t* ColorName(int i) { return kColorNames[std::clamp(i, 0, ColorCount() - 1)]; }
int Levels() { return kLevels; }
float TextPx(int level) { return kTextPx[std::clamp(level, 0, kLevels - 1)]; }
bool IsDark(COLORREF c) { return ather::IsDark(c); }

static Annot ToAnnot(const Shape& s) {
    Annot a;
    a.color = Color(s.color);
    a.level = std::clamp(s.level, 0, kLevels - 1);
    a.unit = s.unit;
    a.step = s.step;
    a.text = s.text;
    a.wrap = s.wrap;
    switch (s.kind) {
        case Kind::Arrow: a.type = Tool::Arrow; break;
        case Kind::Rect: a.type = Tool::Rect; break;
        case Kind::Ellipse: a.type = Tool::Ellipse; break;
        case Kind::Step: a.type = Tool::Step; break;
        case Kind::Text: a.type = Tool::Text; break;
    }
    a.pts = {gp::PointF(s.ax, s.ay)};
    if (s.kind != Kind::Step && s.kind != Kind::Text) a.pts.push_back(gp::PointF(s.bx, s.by));
    return a;
}

BoxF Bounds(const Shape& s) {
    const gp::RectF r = ather::Bounds(ToAnnot(s));
    return {r.X, r.Y, r.Width, r.Height};
}

float StrokeWidth(const Shape& s) { return StrokeW(ToAnnot(s)); }

void Draw(Bitmap& dst, const Shape& s, float ox, float oy) {
    gp::Bitmap gb(dst.Width(), dst.Height(), dst.Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(dst.Bits()));
    gp::Graphics g(&gb);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    g.TranslateTransform(-ox, -oy);
    DrawAnnot(g, ToAnnot(s));
}

void DrawPartialOutline(Bitmap& dst, const Shape& s, float part, float ox, float oy) {
    const Annot a = ToAnnot(s);
    const gp::RectF r = NormRect(a.pts[0], a.pts.back());
    gp::Bitmap gb(dst.Width(), dst.Height(), dst.Width() * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(dst.Bits()));
    gp::Graphics g(&gb);
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.TranslateTransform(-ox, -oy);
    gp::GraphicsPath path;
    if (s.kind == Kind::Ellipse) path.AddEllipse(r);
    else path.AddRectangle(r);
    path.Flatten(nullptr, 0.25f);
    // Walk the flattened outline and keep the first `part` of its length.
    const int n = path.GetPointCount();
    if (n < 2) return;
    std::vector<gp::PointF> pts(n);
    path.GetPathPoints(pts.data(), n);
    pts.push_back(pts[0]);
    double total = 0;
    for (size_t i = 1; i < pts.size(); ++i) total += std::hypot(pts[i].X - pts[i - 1].X, pts[i].Y - pts[i - 1].Y);
    double left = total * std::clamp(part, 0.f, 1.f);
    std::vector<gp::PointF> keep{pts[0]};
    for (size_t i = 1; i < pts.size() && left > 0; ++i) {
        const double seg = std::hypot(pts[i].X - pts[i - 1].X, pts[i].Y - pts[i - 1].Y);
        if (seg <= left) {
            keep.push_back(pts[i]);
            left -= seg;
        } else {
            const float k = (float)(left / seg);
            keep.push_back(gp::PointF(pts[i - 1].X + (pts[i].X - pts[i - 1].X) * k, pts[i - 1].Y + (pts[i].Y - pts[i - 1].Y) * k));
            left = 0;
        }
    }
    if (keep.size() < 2) return;
    gp::Pen pen(GC(a.color), StrokeW(a));
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    pen.SetLineJoin(gp::LineJoinRound);
    g.DrawLines(&pen, keep.data(), (INT)keep.size());
}

}  // namespace annot

}  // namespace ather
