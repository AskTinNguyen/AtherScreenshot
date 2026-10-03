#include "editor.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "output.h"
#include "palette.h"
#include "pin.h"
#include "ocr.h"
#include "toast.h"

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotEditor";
constexpr int kToolbarH = 52, kStatusH = 26, kMargin = 16, kBtn = 36;

enum class Tool { Select, Arrow, Line, Rect, Ellipse, Pen, Highlight, Text, Step, Blur, Pixelate, Spotlight, Magnify, Crop };

// Tools applied directly to pixels in Compose() rather than drawn with GDI+.
bool IsPixelTool(Tool t) { return t == Tool::Blur || t == Tool::Pixelate || t == Tool::Magnify; }
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
    {Tool::Select, L"Select", 'V', L"Click to select, drag to move, Delete to remove"},
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
    ActStyle, ActRedact, ActCopyAnnots, ActPasteAnnots, ActDuplicate
};

struct Annot {
    Tool type = Tool::Arrow;
    COLORREF color = 0;
    int level = 1;
    float unit = 1;  // display scale when created, so strokes look the same on high-DPI captures
    std::vector<gp::PointF> pts;
    std::wstring text;
    int step = 0;
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
    MeasureGraphics().MeasureString(t.c_str(), (INT)t.size(), font.get(), a.pts[0], TypoFormat(), &r);
    if (text.empty()) r.Width = 0;
    return r;
}

gp::RectF Bounds(const Annot& a) {
    gp::RectF r;
    switch (a.type) {
        case Tool::Text: r = MeasureText(a, a.text); break;
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

// Three box-blur passes ≈ Gaussian. Samples outside the area are clamped to its edge so
// nothing outside the selection bleeds in or gets modified.
void GaussBlur(Bitmap& b, const gp::RectF& area, int radius) {
    const RECT rc = ClampRect(area, b);
    const int w = RectW(rc), h = RectH(rc);
    if (w <= 1 || h <= 1 || radius < 1) return;
    std::vector<float> ch[3];
    for (auto& c : ch) c.resize((size_t)w * h);
    uint32_t* px = b.Bits();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = px[(size_t)(rc.top + y) * b.Width() + rc.left + x];
            ch[0][(size_t)y * w + x] = (float)((p >> 16) & 255);
            ch[1][(size_t)y * w + x] = (float)((p >> 8) & 255);
            ch[2][(size_t)y * w + x] = (float)(p & 255);
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
            px[(size_t)(rc.top + y) * b.Width() + rc.left + x] =
                0xFF000000u | (uint32_t)std::lround(ch[0][i]) << 16 | (uint32_t)std::lround(ch[1][i]) << 8 |
                (uint32_t)std::lround(ch[2][i]);
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
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                float v = ((q[0] >> sh) & 255) * (1 - fx) * (1 - fy) + ((q[1] >> sh) & 255) * fx * (1 - fy) +
                          ((q[2] >> sh) & 255) * (1 - fx) * fy + ((q[3] >> sh) & 255) * fx * fy;
                out |= (uint32_t)std::lround(v) << sh;
            }
            b.Bits()[(size_t)y * W + x] = out;
        }
}

// Dims everything outside the union of `areas` to 40%.
void Spotlight(Bitmap& b, const std::vector<gp::RectF>& areas) {
    std::vector<RECT> keep;
    for (const auto& a : areas) keep.push_back(ClampRect(a, b));
    for (int y = 0; y < b.Height(); ++y) {
        uint32_t* row = b.Bits() + (size_t)y * b.Width();
        for (int x = 0; x < b.Width(); ++x) {
            bool inside = false;
            for (const auto& k : keep)
                if (x >= k.left && x < k.right && y >= k.top && y < k.bottom) {
                    inside = true;
                    break;
                }
            if (!inside) {
                uint32_t p = row[x];
                row[x] = 0xFF000000u | ((((p >> 16) & 255) * 2 / 5) << 16) | ((((p >> 8) & 255) * 2 / 5) << 8) |
                         ((p & 255) * 2 / 5);
            }
        }
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
            uint64_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) {
                    uint32_t p = px[(size_t)y * stride + x];
                    sr += (p >> 16) & 255;
                    sg += (p >> 8) & 255;
                    sb += p & 255;
                    ++n;
                }
            uint32_t avg = 0xFF000000u | (uint32_t)(sr / n) << 16 | (uint32_t)(sg / n) << 8 | (uint32_t)(sb / n);
            for (int y = by; y < ey; ++y) std::fill_n(px + (size_t)y * stride + bx, ex - bx, avg);
        }
    }
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

// Blur / pixelate / spotlight are applied by Compose() directly on pixels; here they only draw a live
// preview. For the magnifier this draws the frame (rings + connector) around the zoomed bubble.
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
            g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), gp::PointF(a.pts[0].X + off, a.pts[0].Y + off),
                         TypoFormat(), &shadow);
            g.DrawString(a.text.c_str(), (INT)a.text.size(), font.get(), a.pts[0], TypoFormat(), &br);
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
            gp::SolidBrush tb(IsDark(a.color) || GetGValue(a.color) < 200 ? gp::Color(255, 255, 255, 255)
                                                                         : gp::Color(255, 20, 20, 20));
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
            gp::PointF pts[] = {ip.P(3, 1), ip.P(3, 14), ip.P(6.5f, 10.5f), ip.P(9, 15.5f), ip.P(11, 14.5f),
                                ip.P(8.5f, 9.5f), ip.P(13, 9.5f)};
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
    }
}

// ---- the editor window ----

class Editor {
public:
    explicit Editor(BitmapPtr img) : base_(std::move(img)) {}
    bool Create();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    void RunCommand(int id);
    HWND hwnd = nullptr;

private:
    struct State {
        BitmapPtr base;
        std::vector<Annot> annots;
        int nextStep;
    };

    int S(int v) const { return Px(s_, v); }
    gp::PointF ToImg(int x, int y) const { return {(x - ox_) / zoom_, (y - oy_) / zoom_}; }
    bool InImageView(int x, int y) const {
        return x >= ox_ && y >= oy_ && x < ox_ + viewW_ && y < oy_ + viewH_;
    }
    RECT Canvas() const { return {0, S(kToolbarH), client_.right, client_.bottom - S(kStatusH)}; }
    const ToolInfo& CurTool() const { return kTools[(int)tool_]; }

    void Layout();
    void FitView();
    void RebuildViewCache();
    void Compose();
    void Paint(HDC hdc);
    void DrawToolbar(HDC dc);
    void DrawStatus(HDC dc);
    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }
    void RebuildFonts();

    void PushUndo();
    void Undo();
    void Redo();
    void Restore(State st);
    State Current() const { return {base_, annots_, nextStep_}; }

    void SetTool(Tool t);
    void SetColor(int i);
    void SetLevel(int l);
    gp::PointF Constrain(gp::PointF a, gp::PointF b) const;
    int HitTest(gp::PointF p) const;
    void CommitText();
    void CancelLive();
    void ApplyCrop(const gp::RectF& r);
    BitmapPtr Export();
    BitmapPtr StyledFrame(const Bitmap& img) const;
    void AutoRedact();
    void CopyAnnotations();
    void PasteAnnotations();
    void DuplicateSelected();

    void Copy();
    void Save();
    void SaveAs();
    void Pin();
    void Done();
    void Close();
    void Action(int act);
    void OpenCommandPalette();

    void OnLButtonDown(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnKeyDown(WPARAM vk);
    void OnChar(wchar_t ch);

    // document
    bool styled_ = g_defaults.styledExport;
    bool redacting_ = false;
    BitmapPtr base_, committed_;
    std::vector<Annot> annots_;
    int nextStep_ = 1;
    std::vector<State> undo_, redo_;
    bool dirty_ = false;

    // tool state
    Tool tool_ = Tool::Arrow;
    int color_ = 0, level_ = 1;
    float unit_ = 1;

    // interaction
    bool drawing_ = false, moving_ = false, texting_ = false;
    Annot live_;
    int movingIndex_ = -1, selected_ = -1, hoverBtn_ = -1;
    gp::PointF lastPt_{};

    // view
    float s_ = 1, zoom_ = 1;
    RECT client_{};
    int ox_ = 0, oy_ = 0, viewW_ = 1, viewH_ = 1;
    HBITMAP viewCache_ = nullptr;
    HDC backDC_ = nullptr;
    HBITMAP back_ = nullptr;
    HGDIOBJ backOld_ = nullptr;
    int backW_ = 0, backH_ = 0;
    HFONT fUi_ = nullptr, fIcon_ = nullptr, fStatus_ = nullptr;
    std::vector<Button> buttons_;
    int minClientW_ = 600;
};

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
        delete e;
        return 0;
    }
    return e->Proc(m, w, l);
}

bool Editor::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
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
    Compose();

    RECT work = MonitorRectAt(pt, true);
    int cw = base_->Width() + 2 * S(kMargin), ch = base_->Height() + S(kToolbarH) + S(kStatusH) + 2 * S(kMargin);
    cw = std::clamp(cw, S(1340), std::max(S(1340), (int)(RectW(work) * 0.9)));  // full toolbar always fits
    ch = std::clamp(ch, S(420), (int)(RectH(work) * 0.9));
    RECT r{0, 0, cw, ch};
    UINT dpi = (UINT)(s_ * 96 + 0.5f);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = RectW(r), h = RectH(r);
    if (!CreateWindowExW(0, kClass, L"Ather Screenshot — Editor", WS_OVERLAPPEDWINDOW,
                         work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr,
                         GetModuleHandleW(nullptr), this))
        return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    ShowWithoutFlash(hwnd, true);
    return true;
}

void Editor::RebuildFonts() {
    for (HFONT* f : {&fUi_, &fIcon_, &fStatus_})
        if (*f) DeleteObject(*f);
    fUi_ = MakeDisplayFont(S(18));
    fIcon_ = MakeFont(S(16), FW_NORMAL, L"Segoe Fluent Icons");
    fStatus_ = MakeFont(S(12));
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
    x += S(14);
    const int sw = S(24);
    for (int i = 0; i < (int)std::size(kColors); ++i) {
        buttons_.push_back({Button::KColor, i, {x, cy - sw / 2, x + sw, cy + sw / 2},
                            std::wstring(kColorNames[i]) + L"  (" + std::to_wstring(i + 1) + L")"});
        x += sw + S(4);
    }
    x += S(14);
    const int ww = S(26);
    for (int i = 0; i < kLevels; ++i) {
        buttons_.push_back({Button::KWidth, i, {x, cy - ww / 2, x + ww, cy + ww / 2},
                            L"Size " + std::to_wstring(i + 1) + L"  ([ and ] or mouse wheel)"});
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

void Editor::FitView() {
    RECT c = Canvas();
    int aw = std::max(1, RectW(c) - 2 * S(kMargin)), ah = std::max(1, RectH(c) - 2 * S(kMargin));
    zoom_ = std::min({1.f, (float)aw / base_->Width(), (float)ah / base_->Height()});
    viewW_ = std::max(1, (int)std::lround(base_->Width() * zoom_));
    viewH_ = std::max(1, (int)std::lround(base_->Height() * zoom_));
    ox_ = c.left + (RectW(c) - viewW_) / 2;
    oy_ = c.top + (RectH(c) - viewH_) / 2;
    RebuildViewCache();
}

void Editor::RebuildViewCache() {
    if (!hwnd || !committed_) return;
    if (viewCache_) DeleteObject(viewCache_);
    HDC screen = GetDC(hwnd);
    viewCache_ = CreateCompatibleBitmap(screen, viewW_, viewH_);
    {
        MemDC d(viewCache_, screen), s(committed_->Handle(), screen);
        if (viewW_ == committed_->Width() && viewH_ == committed_->Height()) {
            BitBlt(d, 0, 0, viewW_, viewH_, s, 0, 0, SRCCOPY);
        } else {
            SetStretchBltMode(d, HALFTONE);
            SetBrushOrgEx(d, 0, 0, nullptr);
            StretchBlt(d, 0, 0, viewW_, viewH_, s, 0, 0, committed_->Width(), committed_->Height(), SRCCOPY);
        }
    }
    ReleaseDC(hwnd, screen);
    Invalidate();
}

// base + annotations -> committed_ (full resolution), then refresh the scaled view.
void Editor::Compose() {
    const int w = base_->Width(), h = base_->Height();
    auto out = Bitmap::Create(w, h);
    memcpy(out->Bits(), base_->Bits(), (size_t)w * h * 4);
    std::vector<gp::RectF> spots;  // spotlights dim everything else, so they're applied last
    size_t i = 0;
    while (i < annots_.size()) {
        {
            gp::Bitmap gb(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(out->Bits()));
            gp::Graphics g(&gb);
            g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
            g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
            for (; i < annots_.size() && !IsPixelTool(annots_[i].type); ++i) {
                const Annot& a = annots_[i];
                if (a.type == Tool::Spotlight) spots.push_back(NormRect(a.pts[0], a.pts.back()));
                else DrawAnnot(g, a);
            }
        }  // Graphics flushed into our pixels before any pixel pass
        for (; i < annots_.size() && IsPixelTool(annots_[i].type); ++i) {
            const Annot& a = annots_[i];
            const gp::RectF r = NormRect(a.pts[0], a.pts.back());
            switch (a.type) {
                case Tool::Pixelate: Pixelate(*out, r, std::max(4, (int)std::lround(10 * a.unit))); break;
                case Tool::Blur: GaussBlur(*out, r, std::max(1, (int)std::lround(kBlurR[a.level] * a.unit))); break;
                case Tool::Magnify: {
                    Magnify(*out, a.pts[0], a.pts.back(), kMagR[a.level] * a.unit);
                    gp::Bitmap gb(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(out->Bits()));
                    gp::Graphics g(&gb);
                    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
                    DrawAnnot(g, a);  // rings + connector
                    break;
                }
                default: break;
            }
        }
    }
    if (!spots.empty()) Spotlight(*out, spots);
    committed_ = out;
    RebuildViewCache();
}

void Editor::PushUndo() {
    undo_.push_back(Current());
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
    dirty_ = true;
}

void Editor::Restore(State st) {
    bool sizeChanged = st.base->Width() != base_->Width() || st.base->Height() != base_->Height();
    base_ = st.base;
    annots_ = std::move(st.annots);
    nextStep_ = st.nextStep;
    selected_ = -1;
    Compose();
    if (sizeChanged) FitView();
    dirty_ = true;
}

void Editor::Undo() {
    CancelLive();
    if (undo_.empty()) return;
    redo_.push_back(Current());
    State st = std::move(undo_.back());
    undo_.pop_back();
    Restore(std::move(st));
}

void Editor::Redo() {
    CancelLive();
    if (redo_.empty()) return;
    undo_.push_back(Current());
    State st = std::move(redo_.back());
    redo_.pop_back();
    Restore(std::move(st));
}

void Editor::SetTool(Tool t) {
    CommitText();
    tool_ = t;
    if (t != Tool::Select) selected_ = -1;
    Invalidate();
}

void Editor::SetColor(int i) {
    color_ = i;
    if (texting_) live_.color = kColors[i];
    else if (selected_ >= 0 && tool_ == Tool::Select) {
        PushUndo();
        annots_[selected_].color = kColors[i];
        Compose();
    }
    Invalidate();
}

void Editor::SetLevel(int l) {
    level_ = std::clamp(l, 0, kLevels - 1);
    if (texting_) live_.level = level_;
    else if (selected_ >= 0 && tool_ == Tool::Select) {
        PushUndo();
        annots_[selected_].level = level_;
        Compose();
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
    for (int i = (int)annots_.size() - 1; i >= 0; --i)
        if (Bounds(annots_[i]).Contains(p)) return i;
    return -1;
}

void Editor::CommitText() {
    if (!texting_) return;
    texting_ = false;
    if (!live_.text.empty()) {
        PushUndo();
        annots_.push_back(live_);
        Compose();
    }
    Invalidate();
}

void Editor::CancelLive() {
    if (moving_) {  // put the annotation back where it was
        annots_.insert(annots_.begin() + movingIndex_, live_);
        undo_.pop_back();
        Compose();
    }
    drawing_ = moving_ = texting_ = false;
    if (GetCapture() == hwnd) ReleaseCapture();
    Invalidate();
}

void Editor::ApplyCrop(const gp::RectF& r) {
    RECT rc{(LONG)std::floor(r.X), (LONG)std::floor(r.Y), (LONG)std::ceil(r.GetRight()), (LONG)std::ceil(r.GetBottom())};
    RECT bounds{0, 0, base_->Width(), base_->Height()};
    if (!IntersectRect(&rc, &rc, &bounds) || RectW(rc) < 2 || RectH(rc) < 2) return;
    PushUndo();
    base_ = committed_->Crop(rc);  // bakes annotations; undo restores them as objects
    annots_.clear();
    selected_ = -1;
    Compose();
    FitView();
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
        gp::LinearGradientBrush bg(gp::Point(0, 0), gp::Point(W, H), gp::Color(255, 124, 108, 255),
                                   gp::Color(255, 64, 170, 255));
        g.FillRectangle(&bg, 0, 0, W, H);
        const float r = 12 * u;
        auto rounded = [&](gp::GraphicsPath& p, float x, float y, float w, float h, float rr) {
            p.AddArc(x, y, 2 * rr, 2 * rr, 180, 90);
            p.AddArc(x + w - 2 * rr, y, 2 * rr, 2 * rr, 270, 90);
            p.AddArc(x + w - 2 * rr, y + h - 2 * rr, 2 * rr, 2 * rr, 0, 90);
            p.AddArc(x, y + h - 2 * rr, 2 * rr, 2 * rr, 90, 90);
            p.CloseFigure();
        };
        // Soft shadow: stacked translucent rounded rects growing outward.
        const float sh = 18 * u;
        for (int k = 8; k >= 1; --k) {
            const float grow = sh * k / 8;
            gp::GraphicsPath p;
            rounded(p, pad - grow, pad - grow + 6 * u, img.Width() + 2 * grow, img.Height() + 2 * grow, r + grow);
            gp::SolidBrush s(gp::Color((BYTE)(10 + (8 - k) * 3), 0, 0, 0));
            g.FillPath(&s, &p);
        }
        gp::GraphicsPath clip;
        rounded(clip, (float)pad, (float)pad, (float)img.Width(), (float)img.Height(), r);
        g.SetClip(&clip);
        gp::Bitmap src(img.Width(), img.Height(), img.Width() * 4, PixelFormat32bppRGB,
                       reinterpret_cast<BYTE*>(img.Bits()));
        g.DrawImage(&src, pad, pad, img.Width(), img.Height());
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
    RecognizeWordsAsync(base_, [h, unit](std::vector<OcrWord> words, std::wstring err) {
        auto* e = IsWindow(h) ? reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA)) : nullptr;
        if (!e) return;
        e->redacting_ = false;
        if (!err.empty()) return (void)ShowToast(L"Auto-redact failed", err, nullptr, nullptr, 4000);
        auto rects = FindSensitive(words);
        if (rects.empty())
            return (void)ShowToast(L"Nothing sensitive found", std::to_wstring(words.size()) + L" words checked", nullptr,
                                   nullptr, 2500);
        e->PushUndo();
        for (const RECT& r : rects) {
            Annot a;
            a.type = Tool::Pixelate;
            a.unit = unit;
            a.pts = {gp::PointF((float)r.left, (float)r.top), gp::PointF((float)r.right, (float)r.bottom)};
            e->annots_.push_back(a);
        }
        e->Compose();
        ShowToast(L"Redacted " + std::to_wstring(rects.size()) + (rects.size() == 1 ? L" item" : L" items"),
                  L"Each one is a normal pixelate box: move, delete or undo it.", nullptr, nullptr, 3000);
    });
}

void Editor::CopyAnnotations() {
    CommitText();
    if (selected_ >= 0 && selected_ < (int)annots_.size()) g_annotClipboard = {annots_[selected_]};
    else g_annotClipboard = annots_;
    ShowToast(L"Copied " + std::to_wstring(g_annotClipboard.size()) + L" annotation(s)",
              L"Paste into any editor with Ctrl+Shift+V", nullptr, nullptr, 1800);
}

void Editor::PasteAnnotations() {
    if (g_annotClipboard.empty()) return;
    CommitText();
    PushUndo();
    for (const auto& a : g_annotClipboard) annots_.push_back(a);
    selected_ = (int)annots_.size() - 1;
    tool_ = Tool::Select;
    Compose();
}

void Editor::DuplicateSelected() {
    if (selected_ < 0 || selected_ >= (int)annots_.size()) return;
    PushUndo();
    Annot a = annots_[selected_];
    for (auto& p : a.pts) {
        p.X += 16 * unit_;
        p.Y += 16 * unit_;
    }
    if (a.type == Tool::Step) a.step = nextStep_++;
    annots_.push_back(a);
    selected_ = (int)annots_.size() - 1;
    Compose();
}

void Editor::Copy() {
    auto img = Export();
    if (CopyImageToClipboard(hwnd, *img)) {
        dirty_ = false;
        ShowToast(L"Copied to clipboard",
                  std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
    }
}

void Editor::Save() {
    auto img = Export();
    std::wstring path = MakeCapturePath(g_defaults.capturesFolder);
    dirty_ = false;
    SavePngAsync(img, path, [path](bool ok) {
        if (ok) ShowToast(L"Saved", FileNameOf(path), nullptr, [path] { OpenPath(path); }, 2200);
        else ShowToast(L"Save failed", path, nullptr, nullptr, 4000);
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
    if (GetSaveFileNameW(&ofn) && SavePng(*img, file)) dirty_ = false;
}

void Editor::Pin() { PinImage(Export(), nullptr); }

void Editor::Done() {
    Copy();
    Save();
    DestroyWindow(hwnd);
}

void Editor::Close() {
    CommitText();
    if (dirty_ && MessageBoxW(hwnd, L"Close the editor and discard your changes?", L"Ather Screenshot",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
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
                      L"Background, padding, shadow and rounded corners when you copy, save or pin.", nullptr, nullptr,
                      2200);
            Invalidate();
            break;
        case ActRedact: AutoRedact(); break;
        case ActCopyAnnots: CopyAnnotations(); break;
        case ActPasteAnnots: PasteAnnotations(); break;
        case ActDuplicate: DuplicateSelected(); break;
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
                      {ActStyle, L"Styled export (background, shadow, rounded corners)", styled_ ? L"On" : L"Off",
                       L"beautify pretty frame padding share", 0xE771},
                      {ActCopyAnnots, L"Copy annotations", L"Ctrl+Shift+C", L"shapes markup clipboard", 0xE8C8},
                      {ActPasteAnnots, L"Paste annotations", L"Ctrl+Shift+V", L"shapes markup clipboard", 0xE77F},
                      {ActDuplicate, L"Duplicate selected annotation", L"Ctrl+D", L"clone copy", 0xE8C8},
                      {ActClose, L"Close editor", L"Esc", L"discard quit", 0xE711}};
    for (const auto& a : acts) items.push_back({400 + a.act, a.title, a.hint, a.kw, a.icon});
    for (int i = 0; i < (int)std::size(kTools); ++i)
        items.push_back({100 + i, std::wstring(L"Tool: ") + kTools[i].name, std::wstring(1, kTools[i].key),
                         L"tool draw annotate", 0xE70F});
    for (int i = 0; i < (int)std::size(kColors); ++i)
        items.push_back({200 + i, std::wstring(L"Color: ") + kColorNames[i], std::to_wstring(i + 1), L"colour", 0xE790});
    for (int i = 0; i < kLevels; ++i)
        items.push_back({300 + i, L"Size " + std::to_wstring(i + 1), i == level_ ? L"current" : L"",
                         L"thickness width stroke font", 0xE9E9});
    HWND h = hwnd;
    ShowPalette(std::move(items), [h](int id) {
        if (auto* e = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA)); e && IsWindow(h)) e->RunCommand(id);
    });
}

// ---- input ----

void Editor::OnLButtonDown(int x, int y) {
    SetFocus(hwnd);
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
    if (texting_) {
        CommitText();
        if (tool_ != Tool::Text) return;
    }
    Annot a;
    a.type = tool_;
    a.color = kColors[color_];
    a.level = level_;
    a.unit = unit_;
    a.pts = {p, p};
    switch (tool_) {
        case Tool::Select: {
            selected_ = HitTest(p);
            if (selected_ >= 0) {
                PushUndo();
                movingIndex_ = selected_;
                live_ = annots_[selected_];
                annots_.erase(annots_.begin() + selected_);
                Compose();
                moving_ = true;
                lastPt_ = p;
                SetCapture(hwnd);
            }
            Invalidate();
            return;
        }
        case Tool::Step:
            a.pts = {p};
            a.step = nextStep_;
            PushUndo();
            annots_.push_back(a);
            ++nextStep_;
            Compose();
            return;
        case Tool::Text:
            a.pts = {p};
            live_ = a;
            texting_ = true;
            Invalidate();
            return;
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
    if (hover != hoverBtn_) {
        hoverBtn_ = hover;
        Invalidate();
    }
    const gp::PointF p = ToImg(x, y);
    if (drawing_) {
        if (live_.type == Tool::Pen) {
            const gp::PointF& last = live_.pts.back();
            if (std::hypot(p.X - last.X, p.Y - last.Y) * zoom_ >= 1.5f) live_.pts.push_back(p);
        } else {
            live_.pts[1] = Constrain(live_.pts[0], p);
        }
        Invalidate();
    } else if (moving_) {
        float dx = p.X - lastPt_.X, dy = p.Y - lastPt_.Y;
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
    if (GetCapture() == hwnd) ReleaseCapture();
    if (wasMoving) {
        annots_.insert(annots_.begin() + movingIndex_, live_);
        selected_ = movingIndex_;
        Compose();
        return;
    }
    if (!wasDrawing) return;
    gp::RectF r = NormRect(live_.pts[0], live_.pts.back());
    const float minSize = 3 / zoom_;
    if (live_.type == Tool::Crop) {
        if (r.Width >= minSize && r.Height >= minSize) ApplyCrop(r);
        Invalidate();
        return;
    }
    if (live_.type != Tool::Pen && r.Width < minSize && r.Height < minSize) {  // just a click: nothing to add
        Invalidate();
        return;
    }
    PushUndo();
    annots_.push_back(live_);
    Compose();
}

void Editor::OnKeyDown(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (ctrl) {
        switch (vk) {
            case 'Z': shift ? Redo() : Undo(); return;
            case 'Y': Redo(); return;
            case 'C': shift ? CopyAnnotations() : Copy(); return;
            case 'V':
                if (shift) PasteAnnotations();
                return;
            case 'D': DuplicateSelected(); return;
            case 'E': Action(ActStyle); return;
            case 'R': AutoRedact(); return;
            case 'S': shift ? SaveAs() : Save(); return;
            case 'P': Pin(); return;
            case 'K': OpenCommandPalette(); return;
            case 'W': Close(); return;
        }
        return;
    }
    if (texting_) {
        if (vk == VK_ESCAPE) {
            texting_ = false;
            Invalidate();
        }
        return;  // characters arrive through WM_CHAR
    }
    if (drawing_ || moving_) {
        if (vk == VK_ESCAPE) CancelLive();
        return;
    }
    switch (vk) {
        case VK_ESCAPE: Close(); return;
        case VK_RETURN: Done(); return;
        case VK_DELETE:
        case VK_BACK:
            if (selected_ >= 0) {
                PushUndo();
                annots_.erase(annots_.begin() + selected_);
                selected_ = -1;
                Compose();
            }
            return;
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
                static const wchar_t glyphs[] = {0xE7A7, 0xE7A6, 0xE8C8, 0xE74E, 0xE74E, 0xE840};
                HGDIOBJ of = SelectObject(dc, fIcon_);
                SetTextColor(dc, enabled ? (hover ? theme::kText : theme::kTextDim) : theme::kBorder);
                DrawTextW(dc, &glyphs[b.value], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    std::wstring left = hoverBtn_ >= 0 ? buttons_[hoverBtn_].tip
                                       : std::wstring(CurTool().name) + L"  —  " + CurTool().hint;
    wchar_t right[160];
    swprintf_s(right, L"%s%d × %d   ·   %d%%   ·   Ctrl+K commands",
               styled_ ? L"Styled export   ·   " : L"", base_->Width(), base_->Height(),
               (int)std::lround(zoom_ * 100));
    DrawTextW(dc, right, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SIZE rs{};
    GetTextExtentPoint32W(dc, right, (int)wcslen(right), &rs);
    r.right -= rs.cx + S(24);
    DrawTextW(dc, left.c_str(), -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
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
    FrameSolid(dc, {ox_ - 1, oy_ - 1, ox_ + viewW_ + 1, oy_ + viewH_ + 1}, theme::kBorder);
    {
        MemDC v(viewCache_, hdc);
        BitBlt(dc, ox_, oy_, viewW_, viewH_, v, 0, 0, SRCCOPY);
    }
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
        g.SetClip(gp::Rect(ox_, oy_, viewW_, viewH_));
        g.TranslateTransform((float)ox_, (float)oy_);
        g.ScaleTransform(zoom_, zoom_);
        const float px = 1 / zoom_;  // one screen pixel in image units
        if (drawing_ && live_.type == Tool::Crop) {
            gp::RectF r = NormRect(live_.pts[0], live_.pts.back());
            gp::Region outside(gp::RectF(0, 0, (float)base_->Width(), (float)base_->Height()));
            outside.Exclude(r);
            gp::SolidBrush dim(gp::Color(150, 0, 0, 0));
            g.FillRegion(&dim, &outside);
            gp::Pen pen(GC(theme::kAccent), 2 * s_ * px);
            g.DrawRectangle(&pen, r);
        } else if (drawing_ || moving_ || texting_) {
            if (!(texting_ && live_.text.empty())) DrawAnnot(g, live_);
            if (texting_) {
                gp::RectF tb = MeasureText(live_, live_.text);
                size_t nl = live_.text.find_last_of(L'\n');
                std::wstring lastLine = nl == std::wstring::npos ? live_.text : live_.text.substr(nl + 1);
                gp::RectF lb = MeasureText(live_, lastLine);
                float lineH = MeasureText(live_, L"").Height;
                float cx = live_.pts[0].X + lb.Width + 2 * px;
                float cy = live_.pts[0].Y + std::max(tb.Height, lineH) - lineH;
                gp::Pen caret(GC(live_.color), std::max(2.f * s_ * px, 1.f));
                g.DrawLine(&caret, cx, cy, cx, cy + lineH);
                gp::RectF box(live_.pts[0].X - 4 * px, live_.pts[0].Y - 3 * px,
                              std::max(tb.Width, 40 * px) + 10 * px, std::max(tb.Height, lineH) + 6 * px);
                gp::Pen dash(gp::Color(160, 255, 255, 255), px);
                dash.SetDashStyle(gp::DashStyleDash);
                g.DrawRectangle(&dash, box);
            }
        }
        if (tool_ == Tool::Select && selected_ >= 0 && selected_ < (int)annots_.size() && !moving_) {
            gp::Pen sel(GC(theme::kAccentSoft), 1.5f * s_ * px);
            sel.SetDashStyle(gp::DashStyleDash);
            g.DrawRectangle(&sel, Bounds(annots_[selected_]));
        }
    }
    DrawToolbar(dc);
    DrawStatus(dc);
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
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT p;
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                LPCWSTR cur = IDC_ARROW;
                if (InImageView(p.x, p.y) && !(hoverBtn_ >= 0)) {
                    if (tool_ == Tool::Text) cur = IDC_IBEAM;
                    else if (tool_ == Tool::Select) cur = HitTest(ToImg(p.x, p.y)) >= 0 ? IDC_SIZEALL : IDC_ARROW;
                    else cur = IDC_CROSS;
                } else if (hoverBtn_ >= 0) {
                    cur = IDC_HAND;
                }
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
        case WM_MOUSEMOVE: {
            OnMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hoverBtn_ >= 0) {
                hoverBtn_ = -1;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONUP: OnLButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
        case WM_MOUSEWHEEL: SetLevel(level_ + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1)); return 0;
        case WM_KEYDOWN: OnKeyDown(w); return 0;
        case WM_CHAR: OnChar((wchar_t)w); return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)l != hwnd && (drawing_ || moving_)) CancelLive();
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
            if (backDC_) {
                SelectObject(backDC_, backOld_);
                DeleteObject(back_);
                DeleteDC(backDC_);
            }
            for (HFONT f : {fUi_, fIcon_, fStatus_}) DeleteObject(f);
            return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

}  // namespace

void SetEditorDefaults(const EditorOptions& opt) { g_defaults = opt; }

void OpenEditor(BitmapPtr img) {
    if (!img) return;
    auto* e = new Editor(img->Crop({0, 0, img->Width(), img->Height()}));
    if (!e->Create()) delete e;
}

}  // namespace ather
