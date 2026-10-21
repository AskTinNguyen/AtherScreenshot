#include "editor.h"

#include <commctrl.h>  // TaskDialogIndirect: the tray's close prompt
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out
#include <psapi.h>   // the tray's memory test
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

#pragma comment(lib, "comctl32")
#pragma comment(lib, "psapi")

namespace ather {
namespace {

namespace gp = Gdiplus;

constexpr wchar_t kClass[] = L"AtherScreenshotEditor";
constexpr int kToolbarH = 52, kStatusH = 26, kMargin = 16, kBtn = 34, kBarH = 40;
// The capture tray's strip: thumbnails in kThumbW × kThumbH boxes, then the count and Save selected at the right.
constexpr int kStripH = 96, kThumbW = 120, kThumbH = 68, kThumbGap = 12, kStripPanelW = 196, kTrayMax = 100;

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
    ActInsertImage, ActSpaceBelow, ActSpaceRight, ActEvenMargin, ActResetFrame, ActFront, ActBack,
    ActNextPicture, ActPrevPicture, ActSaveSelected
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
    thread_local gp::StringFormat* f = [] {  // per thread: GDI+ objects can't be shared between threads
        auto* sf = gp::StringFormat::GenericTypographic()->Clone();
        sf->SetFormatFlags(sf->GetFormatFlags() | gp::StringFormatFlagsMeasureTrailingSpaces);
        return sf;
    }();
    return f;
}

gp::Graphics& MeasureGraphics() {
    thread_local gp::Bitmap* bmp = new gp::Bitmap(1, 1, PixelFormat32bppARGB);  // per thread, like TypoFormat
    thread_local gp::Graphics* g = [] {
        auto* gr = new gp::Graphics(bmp);
        gr->SetTextRenderingHint(gp::TextRenderingHintAntiAlias);  // measure the way text is drawn
        return gr;
    }();
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
    if (a.type == Tool::Arrow) pad = std::max(pad, std::max(StrokeW(a) * 3.6f, 12 * a.unit) * 0.58f + 2 * a.unit);  // the head's wings
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
        if (img->Width() > tw * 4 / 3 || img->Height() > th * 4 / 3)
            if (auto shrunk = Resample(*img, tw, th)) img = shrunk;  // keep the original if there's no memory for the copy
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
            if ((!IsRasterTool(a.type) || a.type == Tool::Magnify) && a.type != Tool::Spotlight) DrawAnnot(g, a);
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

// ---- the capture tray ----

// One picture's document. A window holds one, or several in a capture tray. The current picture's document lives
// in the editor's own members (base_, st_, undo_...), the others' in their tray slots.
struct Picture {
    BitmapPtr base;
    EditorSource source;
    DocState st;
    std::vector<DocState> undo, redo;
    bool dirty = false;
    uint64_t version = 0;
    // the tray's own
    uint64_t id = 0;     // stays the same when others are removed, for saves that finish later
    bool keep = true;    // ticked: Save selected saves it (new captures come ticked)
    bool saved = false;  // written to a file at least once
    BitmapPtr thumb;
    std::weak_ptr<const Bitmap> thumbOf;  // the composed picture the thumbnail was made from
};
uint64_t g_pictureIds = 0;

// A thumbnail's size in a boxW × boxH box: the picture's own shape, but never narrower than half the box or
// lower than two thirds of it, so the tick, the × and the badge fit on very tall or wide pictures.
SIZE ThumbFit(int w, int h, int boxW, int boxH) {
    const float k = std::min((float)boxW / std::max(1, w), (float)boxH / std::max(1, h));
    return {std::clamp((int)std::lround(w * k), boxW / 2, boxW), std::clamp((int)std::lround(h * k), boxH * 2 / 3, boxH)};
}

// The strip's thumbnail of a composed picture (a very tall or wide one shows its top or left part), flattened
// onto light grey where it's transparent.
BitmapPtr TrayThumb(const Bitmap& img, int boxW, int boxH) {
    const SIZE z = ThumbFit(img.Width(), img.Height(), boxW, boxH);
    const float k = std::max((float)z.cx / img.Width(), (float)z.cy / img.Height());
    const RECT part{0, 0, std::min(img.Width(), (int)std::ceil(z.cx / k)), std::min(img.Height(), (int)std::ceil(z.cy / k))};
    BitmapPtr cut;
    if (part.right < img.Width() || part.bottom < img.Height()) cut = img.Crop(part);
    auto t = Resample(cut ? *cut : img, z.cx, z.cy);
    return t && HasAlpha(*t) ? Flatten(*t, RGB(217, 217, 217)) : t;
}

// A save of several pictures (Save selected): one toast once they're all written.
struct SaveBatch {
    int left = 0;
    std::vector<std::wstring> paths;
};

// ---- the editor window ----

struct EditorTest;

class Editor;
std::vector<Editor*> g_editors;  // most recently active last
bool g_hideNewEditors = false;   // tests: editors they open (through the routing too) stay hidden

class Editor {
public:
    Editor(BitmapPtr img, const EditorSource& src) : base_(std::move(img)), source_(src) {
        st_.fill = EdgeColor(*base_);
        version_ = 0;
        tray_.emplace_back();
        tray_[0].id = ++g_pictureIds;
    }
    bool Create();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    void RunCommand(int id);
    void InsertImages(const std::vector<std::wstring>& paths, const gp::PointF* at = nullptr);
    // A capture for the editor: joins the most recently used window that holds captures, or opens one.
    static Editor* Route(BitmapPtr img, const EditorSource& source);
    HWND hwnd = nullptr;

private:
    int S(int v) const { return Px(s_, v); }
    RECT Canvas() const { return {0, S(kToolbarH), client_.right, client_.bottom - S(kStatusH) - StripH()}; }
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
        ClampFrame();
        if (pixels) pixel_ = PixelBase(*base_, st_, nullptr);
        Compose();
    }
    // Added space can grow without bound (edge drags, "Space below" again and again, huge collages): keep the
    // document within what can be allocated and saved.
    void ClampFrame() {
        if (!st_.crop) return;
        gp::RectF& c = *st_.crop;
        constexpr float kMaxSide = 30000, kMaxArea = 200e6f;
        c.Width = std::clamp(c.Width, 1.f, kMaxSide);
        c.Height = std::clamp(c.Height, 1.f, kMaxSide);
        if (c.Width * c.Height > kMaxArea) {
            if (c.Height >= c.Width) c.Height = kMaxArea / c.Width;
            else c.Width = kMaxArea / c.Height;
        }
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
    // swap) restores a snapshot from before they existed and would silently drop them. `id`: the tray picture
    // they were found in, which may no longer be the current one.
    void ReceiveRedactions(std::vector<Annot> boxes, uint64_t id = 0);
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
    std::vector<std::wstring> Includes(int picture) const;

    void Copy();
    void Save(bool quiet = false);
    void SaveAs();
    void Pin();
    void Done();
    void Close();
    void Action(int act);
    void OpenCommandPalette();
    bool ToClipboard(const Bitmap& img) { return copyForTest_ ? copyForTest_(img) : CopyImageToClipboard(hwnd, img); }
    Library& Lib() { return library_ ? *library_ : Library::Shared(); }

    // ---- the capture tray: a strip of the window's pictures along the bottom, from two pictures on ----
    bool Tray() const { return tray_.size() > 1; }
    int StripH() const { return Tray() ? S(kStripH) : 0; }
    RECT StripRect() const {
        const LONG b = client_.bottom - S(kStatusH);
        return {0, b - StripH(), client_.right, b};
    }
    RECT StripView() const;  // where the thumbnails scroll
    RECT ThumbBox(int i) const;
    RECT ThumbRect(int i) const;  // the picture inside its box
    RECT TickRect(int i) const;
    RECT RemoveRect(int i) const;
    RECT SaveSelectedRect() const;
    int StripHit(POINT p, int* part) const;
    std::wstring StripTip() const;
    void ClampStripScroll();
    void ScrollStripTo(int i);
    void UpdateThumb();
    void DrawStrip(HDC dc);
    // A picture's document, wherever it is: the current one's is in the members.
    const Bitmap& BaseOf(int i) const { return i == cur_ ? *base_ : *tray_[i].base; }
    const DocState& StateOf(int i) const { return i == cur_ ? st_ : tray_[i].st; }
    const EditorSource& SourceOf(int i) const { return i == cur_ ? source_ : tray_[i].source; }
    bool& DirtyOf(int i) { return i == cur_ ? dirty_ : tray_[i].dirty; }
    bool DirtyOf(int i) const { return i == cur_ ? dirty_ : tray_[i].dirty; }
    uint64_t VersionOf(int i) const { return i == cur_ ? version_ : tray_[i].version; }
    bool Unsaved(int i) const { return !tray_[i].saved || DirtyOf(i); }
    int IndexOf(uint64_t id) const;
    int ToSaveCount() const;
    void SwapDoc(Picture& p);
    void Stash();
    void Load();
    bool AddPicture(BitmapPtr img, const EditorSource& src);
    void SwitchTo(int i);
    void Cycle(int step);
    void RemovePicture(int i);
    BitmapPtr ExportPicture(int i);
    bool SavePicture(int i, bool quiet, const std::shared_ptr<SaveBatch>& batch);
    void Saved(uint64_t id, uint64_t version);
    int SaveSelected(bool quiet);
    void TrayDone();
    enum CloseAnswer { CloseSave, CloseDiscard, CloseCancel };
    int AskClose(int unsaved, bool done);
    bool ConfirmDiscard();

    void OnLButtonDown(int x, int y, bool dbl);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnRButtonUp(int x, int y);
    void OnKeyDown(WPARAM vk) {
        // AltGr arrives as Ctrl+Alt: typing "ż" or "@" must not trigger Ctrl shortcuts.
        Key(vk, GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0, GetKeyState(VK_SHIFT) < 0);
    }
    void Key(WPARAM vk, bool ctrl, bool shift);  // split out so tests can pass the modifiers
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

    bool hidden_ = g_hideNewEditors;  // drawn into memory by the snapshot tool, never shown

    // capture tray
    std::vector<Picture> tray_;  // every picture of the window, in order; tray_[cur_] is the current one
    int cur_ = 0;
    int stripScroll_ = 0;
    int hoverThumb_ = -1, hoverPart_ = 0;  // the picture under the mouse (part: 0 itself, 1 its tick, 2 its ×), -2 Save selected
    bool takesCaptures_ = false;           // opened for a capture: later captures join it
    // Tests: their own library, answers to the prompts (a CloseAnswer; true = discard), and a clipboard.
    Library* library_ = nullptr;
    std::function<int(int unsaved, bool done)> answerClose_;
    std::function<bool()> answerDiscard_;
    std::function<bool(const Bitmap&)> copyForTest_;
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
    if (Tray()) t = L"Annotate — picture " + std::to_wstring(cur_ + 1) + L" of " + std::to_wstring(tray_.size()) + L" — " + size;
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
    CommitText();  // first: CancelLive ends text mode and would drop what was typed
    CancelLive();
    if (undo_.empty()) return;
    redo_.push_back(st_);
    DocState st = std::move(undo_.back());
    undo_.pop_back();
    Restore(std::move(st));
}

void Editor::Redo() {
    CommitText();
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
    BitmapPtr out;
    if (committed_) out = styled_ ? StyledFrame(*committed_) : committed_->Crop({0, 0, committed_->Width(), committed_->Height()});
    if (!out) ShowToast(L"Not enough memory for this image", L"Remove some added space or layers and try again.", nullptr, nullptr, 4000);
    return out;
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
    const uint64_t id = tray_[cur_].id;
    RECT f;
    BitmapPtr raster = Raster(*base_, st_, &f);  // includes image layers and added space
    if (raster) raster = Flatten(*raster);
    if (!raster) {
        redacting_ = false;
        return (void)ShowToast(L"Auto-redact failed", L"Not enough memory for this image.", nullptr, nullptr, 4000);
    }
    RecognizeWordsAsync(raster, [h, unit, f, id](std::vector<OcrWord> words, std::wstring err) {
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
        e->ReceiveRedactions(std::move(boxes), id);
    });
}

void Editor::ReceiveRedactions(std::vector<Annot> boxes, uint64_t id) {
    if (const int i = id ? IndexOf(id) : cur_; i != cur_) {  // switched to another picture of the tray meanwhile
        if (i < 0) return;                                    // removed meanwhile
        Picture& p = tray_[i];
        p.undo.push_back(p.st);
        if (p.undo.size() > 200) p.undo.erase(p.undo.begin());
        p.redo.clear();
        const size_t n = boxes.size();
        for (auto& a : boxes) p.st.annots.push_back(std::move(a));
        p.dirty = true;
        ++p.version;
        p.thumb = nullptr;  // made again from the new document
        Invalidate();
        ShowToast(L"Redacted " + std::to_wstring(n) + (n == 1 ? L" item" : L" items") + L" in picture " + std::to_wstring(i + 1),
                  L"Each one is a normal pixelate box: move, delete or undo it.", nullptr, nullptr, 3000);
        return;
    }
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
        const auto files = PickFiles(h, MediaFilter(true, false));
        if (!files.empty()) e->InsertImages(files);
    }, opt);
}

// Ctrl+V: image files from Explorer, or a copied image, become image layers.
bool Editor::PasteImage() {
    if (!OpenClipboard(hwnd)) return false;
    std::vector<std::wstring> files;
    BitmapPtr img;
    if (HANDLE hd = GetClipboardData(CF_HDROP)) {
        for (const auto& p : DroppedFiles((HDROP)hd))
            if (IsMediaFile(p) && MediaTypeOf(p) == MediaType::Image) files.push_back(p);
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
std::vector<std::wstring> Editor::Includes(int picture) const {
    std::vector<std::wstring> out;
    const std::wstring& own = SourceOf(picture).path;
    for (const auto& a : StateOf(picture).annots)
        if (a.layer && !a.layer->source.empty() && _wcsicmp(a.layer->source.c_str(), own.c_str()) != 0 &&
            std::find(out.begin(), out.end(), a.layer->source) == out.end())
            out.push_back(a.layer->source);
    return out;
}

void Editor::Copy() {
    auto img = Export();
    if (!img) return;
    if (ToClipboard(*img))
        ShowToast(L"Copied to clipboard", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
}

void Editor::Save(bool quiet) { SavePicture(cur_, quiet, nullptr); }

// What Save writes for a picture of the tray: the current one's composed view, or a stored document composed
// the same way (fill, base, layers, pixel tools, spotlight, annotations; styled when that's on).
BitmapPtr Editor::ExportPicture(int i) {
    if (i == cur_) return Export();
    BitmapPtr out = ather::Compose(BaseOf(i), StateOf(i));
    if (out && styled_) out = StyledFrame(*out);
    if (!out) ShowToast(L"Not enough memory for this image", L"Remove some added space or layers and try again.", nullptr, nullptr, 4000);
    return out;
}

// Saves picture i as a new PNG in the captures folder. `batch`: one of several (Save selected), with one toast.
bool Editor::SavePicture(int i, bool quiet, const std::shared_ptr<SaveBatch>& batch) {
    auto img = ExportPicture(i);
    if (!img) return false;
    const EditorSource& src = SourceOf(i);
    CaptureNameInfo info{src.window, src.app, img->Width(), img->Height()};
    const std::wstring path = MakeCapturePath(g_defaults.capturesFolder, L"png", info);
    // Joins the gallery with the original's tags and collections, stacked as a version of it. A collage is
    // tagged "collage" and keeps what all of its screenshots share.
    const Bitmap& base = BaseOf(i);
    const bool edited = !StateOf(i).annots.empty() || img->Width() != base.Width() || img->Height() != base.Height();
    Lib().NoteEdit(path, src.path, src.app, src.window, edited, Includes(i), collage_.has_value());
    HWND h = hwnd;
    const uint64_t id = tray_[i].id, version = VersionOf(i);
    if (batch) ++batch->left;
    SavePngAsync(img, path, [path, h, id, version, quiet, img, batch](bool ok) {
        if (!ok) {
            ShowToast(L"Save failed", path, nullptr, nullptr, 4000);
        } else if (batch) {
            batch->paths.push_back(path);
        } else if (!quiet) {
            ShowToast(L"Saved", FileNameOf(path), nullptr, [path] { RevealInExplorer({path}); }, 2200);
        }
        if (batch && --batch->left == 0 && !quiet && !batch->paths.empty()) {
            const std::vector<std::wstring> paths = batch->paths;
            const size_t n = paths.size();
            ShowToast(n == 1 ? std::wstring(L"Saved 1 picture") : L"Saved " + std::to_wstring(n) + L" pictures",
                      FileNameOf(paths[0]) + (n > 1 ? L" and " + std::to_wstring(n - 1) + L" more" : L""), nullptr,
                      [paths] { RevealInExplorer(paths); }, 2600);
        }
        // Only a successful write counts as saved, and only if nothing changed while it was written.
        Editor* e = FromHwnd(h);
        if (e && ok) e->Saved(id, version);
    });
    return true;
}

void Editor::Saved(uint64_t id, uint64_t version) {
    const int i = IndexOf(id);
    if (i < 0) return;
    tray_[i].saved = true;
    if (VersionOf(i) == version) DirtyOf(i) = false;
    Invalidate();
}

void Editor::SaveAs() {
    auto img = Export();
    if (!img) return;
    wchar_t file[MAX_PATH] = L"annotated.png";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    Lib().NoteEdit(file, source_.path, source_.app, source_.window, true, Includes(cur_), collage_.has_value());
    if (SavePng(*img, file)) {  // a failed save must still warn before closing
        dirty_ = false;
        tray_[cur_].saved = true;
    } else {
        ShowToast(L"Save failed", file, nullptr, nullptr, 4000);
    }
}

void Editor::Pin() {
    if (auto img = Export())
        if (auto flat = Flatten(*img)) PinImage(flat, nullptr);
}

void Editor::Done() {
    if (Tray()) return TrayDone();
    auto img = Export();
    if (!img) return;
    const bool copied = ToClipboard(*img);
    if (g_defaults.saveToFile) {
        Save(true);
        ShowToast(copied ? L"Copied and saved" : L"Saved", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 2000);
    } else if (copied) {
        ShowToast(L"Copied to clipboard", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
    } else {
        // Neither copied nor saved: closing would lose the work.
        ShowToast(L"Couldn't copy the image", L"Another app may be using the clipboard. Try again, or save with Ctrl+S.", nullptr, nullptr, 4000);
        return;
    }
    dirty_ = false;
    DestroyWindow(hwnd);
}

void Editor::Close() {
    CommitText();
    if (Tray()) {  // asks when a picture would be lost: never saved, or changed since
        int unsaved = 0;
        for (int i = 0; i < (int)tray_.size(); ++i) unsaved += Unsaved(i);
        if (unsaved) {
            const int a = AskClose(unsaved, false);
            if (a == CloseCancel) return;
            if (a == CloseSave) SaveSelected(false);
        }
        DestroyWindow(hwnd);
        return;
    }
    if (dirty_ && MessageBoxW(hwnd, L"Close the editor and discard your changes?", L"Ather Screenshot", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    DestroyWindow(hwnd);
}

// ---- the capture tray ----

int Editor::IndexOf(uint64_t id) const {
    for (int i = 0; i < (int)tray_.size(); ++i)
        if (tray_[i].id == id) return i;
    return -1;
}

// Ticked pictures that Save selected would write: never saved, or changed since.
int Editor::ToSaveCount() const {
    int n = 0;
    for (int i = 0; i < (int)tray_.size(); ++i) n += tray_[i].keep && Unsaved(i);
    return n;
}

void Editor::SwapDoc(Picture& p) {
    std::swap(base_, p.base);
    std::swap(source_, p.source);
    std::swap(st_, p.st);
    std::swap(undo_, p.undo);
    std::swap(redo_, p.redo);
    std::swap(dirty_, p.dirty);
    std::swap(version_, p.version);
}

// The current picture's document goes back to its slot, with a fresh thumbnail. Its composed bitmaps are
// dropped: only the current picture keeps them, so a full tray holds little more than its screenshots.
void Editor::Stash() {
    UpdateThumb();
    SwapDoc(tray_[cur_]);
    pixel_.reset();
    committed_.reset();
}

// Takes tray_[cur_]'s document into the editor and shows it.
void Editor::Load() {
    SwapDoc(tray_[cur_]);
    selected_ = -1;
    Changed(true);
    ScrollStripTo(cur_);
    UpdateTitle();
    Invalidate();
}

// A capture joins the tray and becomes the current picture. False when the tray is full.
bool Editor::AddPicture(BitmapPtr img, const EditorSource& src) {
    if (!img || collage_ || (int)tray_.size() >= kTrayMax) return false;
    CommitText();
    CancelLive();
    FlushRedactions();
    Picture p;
    p.base = std::move(img);
    p.source = src;
    p.st.fill = EdgeColor(*p.base);
    p.id = ++g_pictureIds;
    Stash();
    tray_.push_back(std::move(p));
    cur_ = (int)tray_.size() - 1;
    Load();
    return true;
}

// Tool, color and size stay with the window; everything else is the picture's.
void Editor::SwitchTo(int i) {
    if (i < 0 || i >= (int)tray_.size() || i == cur_ || Gesture()) return;
    CommitText();  // an unfinished text box stays with its picture
    CancelLive();
    Stash();
    cur_ = i;
    Load();
}

void Editor::Cycle(int step) {
    const int n = (int)tray_.size();
    if (n > 1) SwitchTo(((cur_ + step) % n + n) % n);
}

// ×: takes a picture out of the tray. Asks first when it has changes that aren't saved.
void Editor::RemovePicture(int i) {
    if (!Tray() || i < 0 || i >= (int)tray_.size() || Gesture()) return;
    CommitText();
    if (DirtyOf(i) && !ConfirmDiscard()) return;
    if (i == cur_) SwitchTo(i + 1 < (int)tray_.size() ? i + 1 : i - 1);
    tray_.erase(tray_.begin() + i);
    if (cur_ > i) --cur_;
    hoverThumb_ = -1;
    ClampStripScroll();
    FitView();  // the strip goes when one picture is left
    UpdateTitle();
    Invalidate();
}

// Saves every ticked picture that isn't saved yet, or changed since, as Save would. Returns how many.
int Editor::SaveSelected(bool quiet) {
    CommitText();
    auto batch = std::make_shared<SaveBatch>();
    int n = 0;
    for (int i = 0; i < (int)tray_.size(); ++i)
        if (tray_[i].keep && Unsaved(i)) n += SavePicture(i, quiet, batch);
    if (!n && !quiet)
        ShowToast(L"Nothing new to save", L"Every ticked picture is saved. Tick the ones to save.", nullptr, nullptr, 2200);
    Invalidate();
    return n;
}

// Done with a tray: saves the ticked pictures, copies the current one and closes. Asks first when a picture that
// isn't ticked would be lost.
void Editor::TrayDone() {
    CommitText();
    int left = 0;
    for (int i = 0; i < (int)tray_.size(); ++i) left += !tray_[i].keep && Unsaved(i);
    if (left) {
        const int a = AskClose(left, true);
        if (a == CloseCancel) return;
        if (a == CloseDiscard) return (void)DestroyWindow(hwnd);
    }
    const int n = SaveSelected(true);
    auto img = Export();
    const bool copied = img && ToClipboard(*img);
    const std::wstring saved = n == 1 ? std::wstring(L"1 picture") : std::to_wstring(n) + L" pictures";
    if (n) ShowToast(copied ? L"Copied and saved" : L"Saved", saved + (copied ? L"  ·  the current one is on the clipboard" : L""), nullptr, nullptr, 2400);
    else if (copied) ShowToast(L"Copied to clipboard", std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height()), nullptr, nullptr, 1800);
    DestroyWindow(hwnd);
}

// "3 pictures aren't saved": Save selected and close (the default), Close without saving, or Cancel.
int Editor::AskClose(int unsaved, bool done) {
    if (answerClose_) return answerClose_(unsaved, done);
    const std::wstring head = std::to_wstring(unsaved) + (unsaved == 1 ? L" picture isn't saved" : L" pictures aren't saved");
    const wchar_t* body = done ? L"They aren't ticked, so Done won't save them. Pictures you don't save are gone once the editor closes."
                               : L"Save selected saves the ticked pictures to your captures folder. Pictures you don't save are gone once the editor closes.";
    enum { kSave = 1001, kDiscard = 1002 };
    const TASKDIALOG_BUTTON buttons[] = {{kSave, L"Save selected and close"}, {kDiscard, L"Close without saving"}};
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = hwnd;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = L"Ather Screenshot";
    c.pszMainInstruction = head.c_str();
    c.pszContent = body;
    c.cButtons = (UINT)std::size(buttons);
    c.pButtons = buttons;
    c.nDefaultButton = kSave;  // never "Close without saving"
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) {
        const std::wstring q = head + L"\n\nYes: save the ticked pictures, then close.\nNo: close without saving.";
        const int r = MessageBoxW(hwnd, q.c_str(), L"Ather Screenshot", MB_YESNOCANCEL | MB_ICONQUESTION);
        pressed = r == IDYES ? kSave : r == IDNO ? kDiscard : IDCANCEL;
    }
    return pressed == kSave ? CloseSave : pressed == kDiscard ? CloseDiscard : CloseCancel;
}

bool Editor::ConfirmDiscard() {
    if (answerDiscard_) return answerDiscard_();
    return MessageBoxW(hwnd, L"Remove this picture and discard your changes?", L"Ather Screenshot", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
}

// The current picture's thumbnail, made again whenever its composed picture changed.
void Editor::UpdateThumb() {
    if (!committed_) return;
    Picture& p = tray_[cur_];
    if (p.thumb && p.thumbOf.lock() == committed_) return;
    p.thumb = TrayThumb(*committed_, S(kThumbW), S(kThumbH));
    p.thumbOf = committed_;
}

RECT Editor::StripView() const {
    const RECT s = StripRect();
    return {s.left + S(8), s.top + 1, s.right - S(kStripPanelW), s.bottom};
}

RECT Editor::ThumbBox(int i) const {
    const RECT v = StripView(), s = StripRect();
    const int x = v.left + S(6) + i * (S(kThumbW) + S(kThumbGap)) - stripScroll_;
    const int y = s.top + (RectH(s) - S(kThumbH)) / 2;
    return {x, y, x + S(kThumbW), y + S(kThumbH)};
}

RECT Editor::ThumbRect(int i) const {
    const RECT b = ThumbBox(i);
    const RECT f = i == cur_ ? Frame() : FrameOf(*tray_[i].base, tray_[i].st);
    const SIZE z = ThumbFit(RectW(f), RectH(f), RectW(b), RectH(b));
    const int x = b.left + (RectW(b) - z.cx) / 2, y = b.top + (RectH(b) - z.cy) / 2;
    return {x, y, x + z.cx, y + z.cy};
}

RECT Editor::TickRect(int i) const {
    const RECT r = ThumbRect(i);
    return {r.left + S(5), r.top + S(5), r.left + S(23), r.top + S(23)};
}

RECT Editor::RemoveRect(int i) const {
    const RECT r = ThumbRect(i);
    return {r.right - S(23), r.top + S(5), r.right - S(5), r.top + S(23)};
}

RECT Editor::SaveSelectedRect() const {
    const RECT s = StripRect();
    const int r = s.right - S(14), b = s.bottom - S(16);
    return {r - S(kStripPanelW) + S(28), b - S(kBtn), r, b};
}

// What's under p in the strip: a picture's index (part 0, or 1 its tick, 2 its ×), -2 Save selected, or -1.
int Editor::StripHit(POINT p, int* part) const {
    *part = 0;
    const RECT s = StripRect(), save = SaveSelectedRect(), v = StripView();
    if (!Tray() || !PtInRect(&s, p)) return -1;
    if (PtInRect(&save, p)) return -2;
    if (!PtInRect(&v, p)) return -1;
    for (int i = 0; i < (int)tray_.size(); ++i) {
        const RECT b = ThumbBox(i);
        if (!PtInRect(&b, p)) continue;
        const RECT t = TickRect(i), x = RemoveRect(i);
        if (PtInRect(&t, p)) *part = 1;
        else if (PtInRect(&x, p)) *part = 2;
        return i;
    }
    return -1;
}

std::wstring Editor::StripTip() const {
    if (hoverThumb_ == -2) return L"Save every ticked picture to the captures folder";
    if (hoverPart_ == 1) return L"Tick the pictures to keep  ·  Save selected saves them together";
    if (hoverPart_ == 2) return L"Remove this picture from the tray";
    return L"Picture " + std::to_wstring(hoverThumb_ + 1) + L" of " + std::to_wstring(tray_.size()) +
           L"  —  click to open it  ·  Ctrl+Tab next, Ctrl+Shift+Tab previous";
}

void Editor::ClampStripScroll() {
    const int content = (int)tray_.size() * (S(kThumbW) + S(kThumbGap)) - S(kThumbGap) + 2 * S(6);
    stripScroll_ = std::clamp(stripScroll_, 0, std::max(0, content - RectW(StripView())));
}

void Editor::ScrollStripTo(int i) {
    const RECT v = StripView(), b = ThumbBox(i);
    if (b.left < v.left) stripScroll_ -= v.left - b.left + S(6);
    else if (b.right > v.right) stripScroll_ += b.right - v.right + S(6);
    ClampStripScroll();
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
        case ActNextPicture: Cycle(1); break;
        case ActPrevPicture: Cycle(-1); break;
        case ActSaveSelected: SaveSelected(false); break;
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
    if (Tray()) {
        items.push_back({400 + ActSaveSelected, L"Save selected pictures (" + std::to_wstring(ToSaveCount()) + L")", L"", L"tray strip ticked all", 0xE74E});
        items.push_back({400 + ActNextPicture, L"Next picture", L"Ctrl+Tab", L"tray strip switch", 0xE76C});
        items.push_back({400 + ActPrevPicture, L"Previous picture", L"Ctrl+Shift+Tab", L"tray strip switch", 0xE76B});
    }
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
    if (Tray()) {  // the strip: open a picture, tick or untick it, remove it, Save selected
        int part = 0;
        const int hit = StripHit({x, y}, &part);
        if (hit == -2) return (void)SaveSelected(false);
        if (hit >= 0) {
            if (part == 1) tray_[hit].keep = !tray_[hit].keep;
            else if (part == 2) return RemovePicture(hit);
            else SwitchTo(hit);
            Invalidate();
            return;
        }
    }
    if (y < S(kToolbarH) || y >= Canvas().bottom) return;
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
    int part = 0;
    const int thumb = StripHit({x, y}, &part);
    if (hover != hoverBtn_ || hb != hoverBar_ || thumb != hoverThumb_ || part != hoverPart_) {
        hoverBtn_ = hover;
        hoverBar_ = hb;
        hoverThumb_ = thumb;
        hoverPart_ = part;
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
    if (y < S(kToolbarH) || y >= Canvas().bottom || drawing_ || moving_) return;
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
    for (const auto& p : DroppedFiles(drop))
        if (MediaTypeOf(p) == MediaType::Image && IsMediaFile(p)) files.push_back(p);
    DragFinish(drop);
    if (files.empty()) return (void)ShowToast(L"Drop an image to add it", L"PNG, JPEG, BMP, WebP or TIFF", nullptr, nullptr, 2200);
    const gp::PointF at = ToImg(pt.x, pt.y);
    InsertImages(files, &at);
}

void Editor::Key(WPARAM vk, bool ctrl, bool shift) {
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
            case VK_TAB: Cycle(shift ? -1 : 1); return;  // the tray's next / previous picture
            case VK_NEXT: Cycle(1); return;              // Ctrl+PageDown
            case VK_PRIOR: Cycle(-1); return;            // Ctrl+PageUp
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
    if (hoverThumb_ != -1) left = StripTip();
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

// The capture tray along the bottom: each picture at its own shape, the current one outlined in lime, a tick for
// "save this", a dot for unsaved changes, "Saved" once saved and × on hover; then the count and Save selected.
void Editor::DrawStrip(HDC dc) {
    if (!Tray()) return;
    const RECT s = StripRect();
    FillSolid(dc, s, theme::kSurface);
    FillSolid(dc, {s.left, s.top, s.right, s.top + 1}, theme::kBorder);
    UpdateThumb();
    ClampStripScroll();
    const RECT v = StripView();
    FillSolid(dc, {v.right, s.top + S(14), v.right + 1, s.bottom - S(14)}, theme::kBorder);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, v.left, v.top, v.right, v.bottom);
    SetBkMode(dc, TRANSPARENT);
    SetStretchBltMode(dc, HALFTONE);
    HGDIOBJ of = SelectObject(dc, fStatus_);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        g.SetClip(gp::Rect(v.left, v.top, RectW(v), RectH(v)));
        for (int i = 0; i < (int)tray_.size(); ++i) {
            const RECT b = ThumbBox(i);
            if (b.right + S(4) < v.left || b.left - S(4) > v.right) continue;
            Picture& p = tray_[i];
            if (!p.thumb && i != cur_)  // its document changed while it wasn't current (auto-redact)
                if (auto c = ather::Compose(*p.base, p.st)) p.thumb = TrayThumb(*c, S(kThumbW), S(kThumbH));
            const RECT r = ThumbRect(i);
            const bool hover = i == hoverThumb_;
            if (i == cur_) FillRounded(dc, {r.left - S(3), r.top - S(3), r.right + S(3), r.bottom + S(3)}, S(5), theme::kAccent);
            else FrameSolid(dc, {r.left - 1, r.top - 1, r.right + 1, r.bottom + 1}, hover ? theme::kMuted : theme::kBorder);
            if (p.thumb) {
                MemDC th(p.thumb->Handle(), dc);
                if (p.thumb->Width() == RectW(r) && p.thumb->Height() == RectH(r)) BitBlt(dc, r.left, r.top, RectW(r), RectH(r), th, 0, 0, SRCCOPY);
                else StretchBlt(dc, r.left, r.top, RectW(r), RectH(r), th, 0, 0, p.thumb->Width(), p.thumb->Height(), SRCCOPY);
            } else {
                FillSolid(dc, r, theme::kBgRaised);
            }
            // tick
            const RECT t = TickRect(i);
            if (p.keep) {
                FillRounded(dc, t, S(5), theme::kAccent);
                gp::Pen pen(GC(theme::kOnAccent), 2 * s_);
                pen.SetStartCap(gp::LineCapRound);
                pen.SetEndCap(gp::LineCapRound);
                pen.SetLineJoin(gp::LineJoinRound);
                const float u = RectW(t) / 18.f;
                const gp::PointF check[] = {{t.left + 4.5f * u, t.top + 9.5f * u}, {t.left + 7.5f * u, t.top + 12.5f * u}, {t.left + 13.5f * u, t.top + 5.5f * u}};
                g.DrawLines(&pen, check, 3);
            } else {
                FillRounded(dc, t, S(5), theme::kBg, hover && hoverPart_ == 1 ? theme::kText : theme::kMuted);
            }
            // unsaved changes, or saved
            if (DirtyOf(i)) {
                const float d = 9 * s_, cx = r.right - S(10) - d / 2, cy = r.bottom - S(10) - d / 2;
                gp::SolidBrush ring(GC(theme::kBg)), dot(GC(theme::kText));
                g.FillEllipse(&ring, cx - d / 2 - 2 * s_, cy - d / 2 - 2 * s_, d + 4 * s_, d + 4 * s_);
                g.FillEllipse(&dot, cx - d / 2, cy - d / 2, d, d);
            } else if (p.saved) {
                std::wstring label = L"✓ Saved";
                SIZE z{};
                GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &z);
                if (z.cx + S(12) > RectW(r) - S(10)) {
                    label = L"✓";
                    GetTextExtentPoint32W(dc, label.c_str(), (int)label.size(), &z);
                }
                RECT pill{r.left + S(5), r.bottom - S(5) - S(17), r.left + S(5) + z.cx + S(12), r.bottom - S(5)};
                FillRounded(dc, pill, S(8), theme::kBg);
                SetTextColor(dc, theme::kAccent);
                DrawTextW(dc, label.c_str(), -1, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            // ×
            if (hover) {
                const RECT x = RemoveRect(i);
                FillRounded(dc, x, RectW(x) / 2, hoverPart_ == 2 ? RGB(255, 59, 48) : theme::kBg, theme::kBorder);
                gp::Pen pen(GC(theme::kText), 1.6f * s_);
                pen.SetStartCap(gp::LineCapRound);
                pen.SetEndCap(gp::LineCapRound);
                const float c = RectW(x) / 2.f, k = 3.6f * s_, mx = x.left + c, my = x.top + c;
                g.DrawLine(&pen, mx - k, my - k, mx + k, my + k);
                g.DrawLine(&pen, mx - k, my + k, mx + k, my - k);
            }
        }
    }
    RestoreDC(dc, saved);
    // the count and Save selected
    int ticked = 0;
    for (const auto& p : tray_) ticked += p.keep;
    const int n = ToSaveCount();
    const RECT btn = SaveSelectedRect();
    RECT count{btn.left - S(24), s.top + S(10), btn.right, btn.top - S(4)};
    const std::wstring text = std::to_wstring(tray_.size()) + L" pictures · " + std::to_wstring(ticked) + L" selected";
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, fStatus_);
    SetTextColor(dc, theme::kMuted);
    DrawTextW(dc, text.c_str(), -1, &count, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fBar_);
    FillRounded(dc, btn, S(7), n ? (hoverThumb_ == -2 ? theme::kAccentSoft : theme::kAccent) : theme::kBgRaised);
    SetTextColor(dc, n ? theme::kOnAccent : theme::kMuted);
    const std::wstring label = L"Save selected (" + std::to_wstring(n) + L")";
    RECT lr = btn;
    DrawTextW(dc, label.c_str(), -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
    DrawStrip(dc);
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
                const bool inView = PtInRect(&sh, p) && p.y >= S(kToolbarH) && p.y < Canvas().bottom;
                bool overBar = false;
                for (const auto& b : barButtons_) overBar = overBar || PtInRect(&b.r, p);
                if (hoverBtn_ >= 0 || overBar || hoverThumb_ != -1) {
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
            if (hoverBtn_ >= 0 || hoverBar_ >= 0 || hoverThumb_ != -1) {
                hoverBtn_ = hoverBar_ = hoverThumb_ = -1;
                Invalidate();
            }
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            FlushRedactions();
            return 0;
        case WM_RBUTTONUP: OnRButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &p);
            const RECT strip = StripRect();
            if (Tray() && PtInRect(&strip, p)) {  // the strip scrolls sideways
                const int d = GET_WHEEL_DELTA_WPARAM(w);
                stripScroll_ += (m == WM_MOUSEHWHEEL ? d : -d) * (S(kThumbW) + S(kThumbGap)) / WHEEL_DELTA;
                ClampStripScroll();
                hoverThumb_ = StripHit(p, &hoverPart_);  // what's under the mouse now
                Invalidate();
                return 0;
            }
            if (m == WM_MOUSEWHEEL && !Gesture()) SetLevel(level_ + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1));  // not the shape being dragged
            return 0;
        }
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

// Captures collect in the most recently used window opened for a capture; never in a collage or a window opened
// on a file, so a saved file isn't mixed with session captures. A full tray leaves the capture to a new window.
Editor* Editor::Route(BitmapPtr img, const EditorSource& source) {
    if (!img) return nullptr;
    auto copy = img->Crop({0, 0, img->Width(), img->Height()});
    if (g_defaults.tray)
        for (auto it = g_editors.rbegin(); it != g_editors.rend(); ++it) {
            Editor* e = *it;
            if (!e->takesCaptures_) continue;
            if (e->AddPicture(copy, source)) {
                if (!e->hidden_) {
                    if (IsIconic(e->hwnd)) ShowWindow(e->hwnd, SW_RESTORE);
                    ForceForeground(e->hwnd);
                }
                return e;
            }
            ShowToast(L"The editor holds " + std::to_wstring(kTrayMax) + L" pictures",
                      L"Save or remove some there. This capture opened in a new window.", nullptr, nullptr, 4000);
            break;
        }
    auto* e = new Editor(copy, source);
    e->takesCaptures_ = true;
    if (!e->Create()) {
        delete e;
        return nullptr;
    }
    return e;
}

}  // namespace

void SetEditorDefaults(const EditorOptions& opt) { g_defaults = opt; }

void OpenEditor(BitmapPtr img, const EditorSource& source) {
    if (!img) return;
    auto* e = new Editor(img->Crop({0, 0, img->Width(), img->Height()}), source);
    if (!e->Create()) delete e;
}

void OpenCaptureInEditor(BitmapPtr img, const EditorSource& source) { Editor::Route(std::move(img), source); }

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

    // A capture tray of five pictures: the third current, with notes; the second unticked; the fourth saved; the
    // fifth with unsaved changes; the mouse over the first, showing its ×.
    auto* t = new Editor(card(1600, 900, RGB(48, 176, 199), RGB(10, 132, 255), L"Weekly revenue"), {});
    t->hidden_ = true;
    if (!t->Create()) return 1;
    SetWindowPos(t->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    e = t;
    t->Layout();
    t->AddPicture(card(1600, 1000, RGB(255, 59, 48), RGB(255, 149, 0), L"Payment failed"), {});
    t->AddPicture(card(1400, 900, RGB(52, 199, 89), RGB(48, 176, 199), L"Release notes"), {});
    t->AddPicture(card(900, 1400, RGB(88, 86, 214), RGB(175, 82, 222), L"Sign in"), {});
    t->AddPicture(card(1800, 1000, RGB(255, 204, 0), RGB(255, 149, 0), L"Build 0.0.2.4"), {});
    t->PushUndo();  // the fifth: changes not saved yet
    add(Tool::Rect, {gp::PointF(150, 170), gp::PointF(1400, 360)}, 0, 2);
    t->Changed(false);
    t->SwitchTo(2);
    add(Tool::Arrow, {gp::PointF(1050, 700), gp::PointF(800, 380)}, 0, 2);
    add(Tool::Step, {gp::PointF(140, 160)}, 0, 2);
    add(Tool::Text, {gp::PointF(1000, 730)}, 0, 2, L"New: export to PDF");
    t->Changed(false);
    t->tray_[1].keep = false;
    t->tray_[3].saved = true;
    t->hoverThumb_ = 0;
    snap(L"editor-tray");
    // Fourteen: the strip scrolls to keep the newest (current) in view; the mouse on Save selected.
    const COLORREF tones[][2] = {{RGB(255, 59, 48), RGB(255, 149, 0)}, {RGB(52, 199, 89), RGB(48, 176, 199)}, {RGB(88, 86, 214), RGB(175, 82, 222)}};
    for (int i = 0; i < 9; ++i)
        t->AddPicture(card(1600 - (i % 3) * 300, 900 + (i % 2) * 200, tones[i % 3][0], tones[i % 3][1], (L"Capture " + std::to_wstring(i + 6)).c_str()), {});
    t->hoverThumb_ = -2;
    snap(L"editor-tray-scrolled");
    DestroyWindow(t->hwnd);
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
    static void UndoWhileTyping();
    // the capture tray
    static Editor* Open(BitmapPtr img, const EditorSource& src = {});
    static POINT ToClient(Editor* e, gp::PointF p);
    static void Drag(Editor* e, Tool t, gp::PointF a, gp::PointF b);
    static void TrayRouting();
    static void TrayKeepsEachDocument();
    static void TraySaveSelectedAndDone();
    static void TrayClosePrompt();
    static void TrayStrip();
    static void TrayLimit();
    static void TrayMemory();
};
}  // namespace

ATHER_TEST(editor_redactions_survive_a_cancelled_drag) { EditorTest::Redactions(); }
ATHER_TEST(editor_undo_while_typing_keeps_the_previous_step) { EditorTest::UndoWhileTyping(); }
ATHER_TEST(editor_tray_routes_captures_to_the_open_editor) { EditorTest::TrayRouting(); }
ATHER_TEST(editor_tray_keeps_each_pictures_document) { EditorTest::TrayKeepsEachDocument(); }
ATHER_TEST(editor_tray_save_selected_and_done) { EditorTest::TraySaveSelectedAndDone(); }
ATHER_TEST(editor_tray_close_prompt_answers) { EditorTest::TrayClosePrompt(); }
ATHER_TEST(editor_tray_strip_clicks_keys_and_wheel) { EditorTest::TrayStrip(); }
ATHER_TEST(editor_tray_holds_at_most_100_pictures) { EditorTest::TrayLimit(); }
ATHER_TEST(editor_tray_memory_for_40_captures) { EditorTest::TrayMemory(); }

void EditorTest::UndoWhileTyping() {
    auto* e = new Editor(SolidBmp(400, 300, 0xFFFFFFFF), {});
    e->hidden_ = true;
    CHECK(e->Create());
    if (!e->hwnd) return;
    Annot arrow;
    arrow.type = Tool::Arrow;
    arrow.pts = {gp::PointF(10, 10), gp::PointF(90, 90)};
    e->PushUndo();
    e->st_.annots.push_back(arrow);
    e->texting_ = true;  // typing a note
    e->live_ = Annot{};
    e->live_.type = Tool::Text;
    e->live_.pts = {gp::PointF(20, 200)};
    e->live_.text = L"Important";
    e->Undo();  // undoes the note, not the arrow
    CHECK(e->st_.annots.size() == 1 && e->st_.annots[0].type == Tool::Arrow);
    e->Redo();  // and the note comes back
    CHECK(std::any_of(e->st_.annots.begin(), e->st_.annots.end(), [](const Annot& x) { return x.text == L"Important"; }));
    e->dirty_ = false;
    DestroyWindow(e->hwnd);
}

// A spotlight dims everything else; the area itself is left exactly as it was (no preview tint or border).
ATHER_TEST(editor_spotlight_leaves_its_area_untouched) {
    auto base = SolidBmp(200, 100, 0xFF808080);
    DocState st;
    Annot spot;
    spot.type = Tool::Spotlight;
    spot.pts = {gp::PointF(50, 20), gp::PointF(150, 80)};
    st.annots.push_back(spot);
    auto out = Compose(*base, st);
    CHECK(out != nullptr);
    if (!out) return;
    CHECK_EQ(Rgb(At(*out, 100, 50)), 0x808080u);  // inside
    CHECK_EQ(Rgb(At(*out, 50, 50)), 0x808080u);   // on its edge: no dashed border
    CHECK(Rgb(At(*out, 10, 50)) != 0x808080u);    // outside is dimmed
}

// Arrow bounds cover the arrowhead, so video markup (drawn into a bitmap of this size) isn't clipped.
ATHER_TEST(editor_arrow_bounds_include_the_head) {
    annot::Shape a;
    a.kind = annot::Kind::Arrow;
    a.level = 4;
    a.unit = 1;
    a.ax = 0, a.ay = 100, a.bx = 300, a.by = 100;
    const annot::BoxF b = annot::Bounds(a);
    const float half = std::max(annot::StrokeWidth(a) * 3.6f, 12.f) * 0.58f;
    CHECK(b.y <= 100 - half && b.y + b.h >= 100 + half);
}

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

// ---- the capture tray (A1–A4 of docs/intent/capture-tray) ----

namespace {

// For a test's scope: editors stay hidden, save into a temp folder, and trays are on (or off).
struct TrayTestScope {
    EditorOptions old = g_defaults;
    explicit TrayTestScope(const std::wstring& folder, bool tray = true) {
        g_defaults.capturesFolder = folder;
        g_defaults.tray = tray;
        g_hideNewEditors = true;
    }
    ~TrayTestScope() {
        g_defaults = old;
        g_hideNewEditors = false;
    }
};

// Saves report back through the UI dispatcher, which the self-tests don't run otherwise.
struct UiForTest {
    HWND h = StartUiDispatcher();
    ~UiForTest() { StopUiDispatcher(h); }
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

void FinishSaves() {
    WaitForPendingSaves(15000);
    Pump(150);
}

bool Same(const Bitmap& a, const Bitmap& b) {
    return a.Width() == b.Width() && a.Height() == b.Height() && memcmp(a.Bits(), b.Bits(), (size_t)a.Width() * a.Height() * 4) == 0;
}

BitmapPtr Copy(const Bitmap& b) { return b.Crop({0, 0, b.Width(), b.Height()}); }

POINT Center(const RECT& r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }

std::vector<std::wstring> SavedApps(Library& lib, const std::wstring& dir) {
    const auto files = Library::ListCaptures(dir);
    lib.Apply(files);
    std::vector<std::wstring> apps;
    for (const auto& f : files) apps.push_back(lib.Meta(f.path).app);
    std::sort(apps.begin(), apps.end());
    return apps;
}

}  // namespace

// A hidden editor opened for a capture, sized like a window on a 1400 × 900 screen.
Editor* EditorTest::Open(BitmapPtr img, const EditorSource& src) {
    auto* e = new Editor(std::move(img), src);
    e->hidden_ = true;
    e->takesCaptures_ = true;
    if (!e->Create()) {
        delete e;
        return nullptr;
    }
    RECT wr{0, 0, 1400, 900};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    SetWindowPos(e->hwnd, nullptr, 0, 0, RectW(wr), RectH(wr), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    e->Layout();
    return e;
}

POINT EditorTest::ToClient(Editor* e, gp::PointF p) {
    const float z = e->Zoom();
    const POINT o = e->Origin(), m = e->ViewMin();
    return {(LONG)std::lround(o.x + (p.X - m.x) * z), (LONG)std::lround(o.y + (p.Y - m.y) * z)};
}

// A drag with the mouse from a to b (image coordinates) with tool t.
void EditorTest::Drag(Editor* e, Tool t, gp::PointF a, gp::PointF b) {
    e->SetTool(t);
    const POINT p = ToClient(e, a), q = ToClient(e, b);
    e->OnLButtonDown(p.x, p.y, false);
    e->OnMouseMove(q.x, q.y);
    e->OnLButtonUp(q.x, q.y);
}

// A1: with an editor open, a capture for the editor joins it (no new window) and becomes the current picture. A
// collage or a window opened on a file never receives captures, and a file opens its own window. Trays off: one
// window per capture, as before.
void EditorTest::TrayRouting() {
    TrayTestScope scope(test::TempDir());
    const int before = EditorCount();
    Editor* a = Editor::Route(SolidBmp(400, 300, 0xFFFFFFFF), {L"", L"Notepad", L"notes.txt"});
    CHECK(a != nullptr);
    if (!a) return;
    CHECK_EQ(EditorCount(), before + 1);
    CHECK(a->takesCaptures_ && a->tray_.size() == 1 && !a->Tray());
    auto shot = SolidBmp(320, 200, 0xFF3366CC);
    CHECK(Editor::Route(shot, {L"", L"Paint", L"Untitled"}) == a);
    CHECK_EQ(EditorCount(), before + 1);
    CHECK(a->Tray() && a->tray_.size() == 2 && a->cur_ == 1);
    CHECK(a->base_ != shot && a->base_->Width() == 320 && Rgb(At(*a->base_, 5, 5)) == 0x3366CCu);  // a copy, now current
    CHECK(a->source_.app == L"Paint" && a->tray_[0].source.app == L"Notepad");

    // A newer collage and a newer window on a saved file: captures still go to the capture window.
    auto blank = Bitmap::Create(1, 1);
    blank->Bits()[0] = 0;
    auto* c = new Editor(blank, {});
    c->hidden_ = true;
    c->collageImages_ = {SolidBmp(200, 100, 0xFFFF0000), SolidBmp(100, 200, 0xFF00FF00)};
    c->collageSources_ = {L"", L""};
    c->collage_ = CollageSpec(std::vector<SIZE>{{200, 100}, {100, 200}});
    c->ApplyCollage(false);
    CHECK(c->Create());
    const std::wstring file = test::TempDir() + L"\\saved.png";
    CHECK(SavePng(*SolidBmp(300, 200, 0xFF00AA00), file));
    CHECK(OpenEditorFile(file));  // its own window
    CHECK_EQ(EditorCount(), before + 3);
    Editor* f = g_editors.back();
    CHECK(f != a && f != c && f->source_.path == file && !f->takesCaptures_);
    CHECK(Editor::Route(SolidBmp(64, 48, 0xFFFF0000), {}) == a);
    CHECK(a->tray_.size() == 3 && a->cur_ == 2);
    CHECK(c->tray_.size() == 1 && f->tray_.size() == 1);
    CHECK_EQ(EditorCount(), before + 3);

    // Trays off: a new window, as before.
    g_defaults.tray = false;
    Editor* own = Editor::Route(SolidBmp(64, 48, 0xFF00FF00), {});
    CHECK(own != nullptr && own != a);
    CHECK_EQ(EditorCount(), before + 4);
    CHECK_EQ(a->tray_.size(), 3u);
    for (Editor* x : {a, c, f, own})
        if (x) DestroyWindow(x->hwnd);
    CHECK_EQ(EditorCount(), before);
}

// A2: each picture keeps its own document. Switching away and back shows the same pixels, undo stack, crop and
// canvas, and undo goes on where it was. Tool, color and size stay with the window; a text box being typed is
// committed into its own picture first.
void EditorTest::TrayKeepsEachDocument() {
    TrayTestScope scope(test::TempDir());
    Editor* e = Open(SolidBmp(400, 300, 0xFFFFFFFF), {L"", L"Notepad", L""});
    if (!e) return CHECK(false);
    Drag(e, Tool::Rect, {40, 40}, {200, 160});
    Drag(e, Tool::Arrow, {300, 250}, {220, 120});
    CHECK(e->st_.annots.size() == 2 && e->undo_.size() == 2 && e->dirty_);
    const BitmapPtr pixels1 = Copy(*e->committed_);
    const size_t undo1 = e->undo_.size();

    CHECK(e->AddPicture(SolidBmp(500, 260, 0xFF3366CC), {L"", L"Paint", L""}));
    CHECK(e->cur_ == 1 && e->st_.annots.empty() && e->undo_.empty() && !e->dirty_);
    Drag(e, Tool::Ellipse, {50, 50}, {250, 200});
    Drag(e, Tool::Crop, {20, 10}, {420, 230});
    CHECK(std::abs(RectW(e->Frame()) - 400) <= 2 && std::abs(RectH(e->Frame()) - 220) <= 2);
    e->ExtendFrame(Editor::Extend::Below);  // and space below it
    const RECT frame2 = e->Frame();
    CHECK(RectH(frame2) > 230);
    const BitmapPtr pixels2 = Copy(*e->committed_);
    const size_t undo2 = e->undo_.size();
    CHECK_EQ(undo2, 3u);
    e->SetColor(3);
    e->SetLevel(4);

    e->Key(VK_TAB, true, true);  // Ctrl+Shift+Tab: the previous picture
    CHECK_EQ(e->cur_, 0);
    CHECK(Same(*e->committed_, *pixels1));
    CHECK_EQ(e->undo_.size(), undo1);
    CHECK(RectW(e->Frame()) == 400 && RectH(e->Frame()) == 300);
    CHECK(e->color_ == 3 && e->level_ == 4);  // these stay with the window
    e->Key('Z', true, false);                 // undo: this picture's last step, the arrow
    CHECK(e->st_.annots.size() == 1 && e->st_.annots[0].type == Tool::Rect);

    // A text box being typed when switching away goes into its own picture.
    e->SetTool(Tool::Text);
    const POINT at = ToClient(e, {60, 220});
    e->OnLButtonDown(at.x, at.y, false);
    e->OnChar(L'H');
    e->OnChar(L'i');
    CHECK(e->texting_);
    e->Key(VK_TAB, true, false);  // Ctrl+Tab: the next picture
    CHECK(e->cur_ == 1 && !e->texting_);
    CHECK(Same(*e->committed_, *pixels2));
    CHECK_EQ(e->undo_.size(), undo2);
    const RECT back2 = e->Frame();
    CHECK(EqualRect(&frame2, &back2) != 0);
    e->Key('Z', true, false);  // undoes the space below, not anything of the first picture
    CHECK(RectH(e->Frame()) < RectH(frame2) && e->st_.annots.size() == 1);
    e->Key(VK_PRIOR, true, false);  // Ctrl+PageUp
    CHECK_EQ(e->cur_, 0);
    CHECK(e->st_.annots.size() == 2 && e->st_.annots.back().text == L"Hi");
    e->Undo();
    CHECK(e->st_.annots.size() == 1 && e->st_.annots[0].type == Tool::Rect);
    DestroyWindow(e->hwnd);
}

// A3: Save selected writes one PNG per ticked picture that isn't saved yet, pixel for pixel what Save writes for
// it, noted in the library with its app and window; unticked ones are left alone. Saved badges and dirty dots
// follow. Done saves the ticked ones, copies the current one and closes, asking first about an unticked picture
// that would be lost.
void EditorTest::TraySaveSelectedAndDone() {
    const std::wstring dir = test::TempDir();
    TrayTestScope scope(dir);
    UiForTest ui;
    Library lib(false);
    Editor* e = Open(SolidBmp(400, 300, 0xFFFFFFFF), {L"", L"Notepad", L"notes.txt"});
    if (!e) return CHECK(false);
    e->library_ = &lib;
    CHECK(e->AddPicture(SolidBmp(320, 240, 0xFF3366CC), {L"", L"Paint", L"Untitled"}));
    CHECK(e->AddPicture(SolidBmp(360, 200, 0xFFCC6633), {L"", L"Excel", L"Book1"}));
    Drag(e, Tool::Arrow, {300, 30}, {80, 150});
    e->SwitchTo(0);
    Drag(e, Tool::Rect, {40, 40}, {200, 160});
    CHECK(e->tray_[0].keep && e->tray_[1].keep && e->tray_[2].keep);  // new captures come ticked
    const POINT tick = Center(e->TickRect(1));
    e->OnLButtonDown(tick.x, tick.y, false);  // untick the second
    CHECK(!e->tray_[1].keep);
    CHECK_EQ(e->ToSaveCount(), 2);
    const DocState second = e->tray_[1].st;

    e->SwitchTo(2);  // the first isn't current while Save selected writes it
    const POINT save = Center(e->SaveSelectedRect());
    e->OnLButtonDown(save.x, save.y, false);
    FinishSaves();
    const auto files = Library::ListCaptures(dir);
    CHECK_EQ(files.size(), 2u);
    CHECK((SavedApps(lib, dir) == std::vector<std::wstring>{L"Excel", L"Notepad"}));
    std::wstring first;
    for (const auto& f : files)
        if (lib.Meta(f.path).app == L"Notepad") first = f.path;
    CHECK(!first.empty() && lib.Meta(first).window == L"notes.txt");
    CHECK(e->tray_[0].saved && !e->DirtyOf(0) && e->tray_[2].saved && !e->DirtyOf(2));  // "Saved" badges, no dots
    CHECK(!e->tray_[1].saved && e->tray_[1].st.annots.size() == second.annots.size());   // untouched
    CHECK_EQ(e->ToSaveCount(), 0);

    // Ctrl+S on the first writes exactly what Save selected wrote for it (it wasn't current then).
    e->SwitchTo(0);
    e->Key('S', true, false);
    FinishSaves();
    std::wstring again;
    for (const auto& f : Library::ListCaptures(dir))
        if (std::none_of(files.begin(), files.end(), [&](const FileStat& x) { return x.path == f.path; })) again = f.path;
    CHECK(!again.empty());
    auto a = LoadImageFile(first), b = LoadImageFile(again);
    CHECK(a && b && Same(*a, *b));
    CHECK(a && a->Width() == 400 && Rgb(At(*a, 40, 100)) != 0xFFFFFFu);  // with its rectangle

    // Edited again: dirty again, saved again.
    Drag(e, Tool::Ellipse, {250, 60}, {380, 200});
    CHECK(e->DirtyOf(0) && e->ToSaveCount() == 1);

    // Done: the second isn't ticked and was never saved, so it asks first. Cancel leaves everything as it was.
    int asked = -1;
    bool askedDone = false;
    BitmapPtr copied;
    e->answerClose_ = [&](int n, bool done) {
        asked = n;
        askedDone = done;
        return (int)Editor::CloseCancel;
    };
    e->copyForTest_ = [&](const Bitmap& img) {
        copied = Copy(img);
        return true;
    };
    const HWND h = e->hwnd;
    e->Key(VK_RETURN, false, false);
    CHECK(asked == 1 && askedDone && IsWindow(h) && !copied);
    const BitmapPtr current = e->Export();
    e->answerClose_ = [&](int, bool) { return (int)Editor::CloseSave; };
    e->Key(VK_RETURN, false, false);
    CHECK(!IsWindow(h));
    FinishSaves();
    CHECK_EQ(Library::ListCaptures(dir).size(), 4u);  // the first again; nothing of the second
    CHECK((SavedApps(lib, dir) == std::vector<std::wstring>{L"Excel", L"Notepad", L"Notepad", L"Notepad"}));
    CHECK(copied && current && Same(*copied, *current));
}

// A3: closing with pictures that would be lost asks "n pictures aren't saved": Cancel keeps the window, Close
// without saving closes, Save selected and close saves the ticked ones first. Nothing to lose: no question.
void EditorTest::TrayClosePrompt() {
    const std::wstring dir = test::TempDir();
    TrayTestScope scope(dir);
    UiForTest ui;
    Library lib(false);
    auto make = [&]() -> Editor* {
        Editor* e = Open(SolidBmp(400, 300, 0xFFFFFFFF), {L"", L"Notepad", L""});
        if (!e) return nullptr;
        e->library_ = &lib;
        e->AddPicture(SolidBmp(320, 240, 0xFF3366CC), {L"", L"Paint", L""});
        e->AddPicture(SolidBmp(360, 200, 0xFFCC6633), {L"", L"Excel", L""});
        e->tray_[1].keep = false;
        return e;
    };
    int asked = -1;
    bool askedDone = true;
    int answer = Editor::CloseCancel;
    auto hook = [&](Editor* e) {
        e->answerClose_ = [&](int n, bool done) {
            asked = n;
            askedDone = done;
            return answer;
        };
    };

    Editor* e = make();
    if (!e) return CHECK(false);
    hook(e);
    HWND h = e->hwnd;
    e->Key(VK_ESCAPE, false, false);  // Cancel
    CHECK(asked == 3 && !askedDone && IsWindow(h));
    answer = Editor::CloseDiscard;
    e->Proc(WM_CLOSE, 0, 0);  // Close without saving
    CHECK(!IsWindow(h));
    FinishSaves();
    CHECK(Library::ListCaptures(dir).empty());

    e = make();
    if (!e) return CHECK(false);
    hook(e);
    h = e->hwnd;
    asked = -1;
    answer = Editor::CloseSave;
    e->Key('W', true, false);  // Save selected and close
    CHECK(asked == 3 && !IsWindow(h));
    FinishSaves();
    CHECK((SavedApps(lib, dir) == std::vector<std::wstring>{L"Excel", L"Notepad"}));

    // Every picture saved: closing asks nothing; Done with every picture ticked asks nothing either.
    e = make();
    if (!e) return CHECK(false);
    hook(e);
    h = e->hwnd;
    asked = -1;
    e->tray_[1].keep = true;
    e->SaveSelected(true);
    FinishSaves();
    CHECK_EQ(e->ToSaveCount(), 0);
    e->Close();
    CHECK(asked == -1 && !IsWindow(h));
    e = make();
    if (!e) return CHECK(false);
    hook(e);
    h = e->hwnd;
    e->tray_[1].keep = true;
    e->copyForTest_ = [](const Bitmap&) { return true; };
    e->Done();
    CHECK(asked == -1 && !IsWindow(h));
    FinishSaves();
    CHECK_EQ(Library::ListCaptures(dir).size(), 8u);  // 2 + 3 + 3
}

// A4: the strip shows from two pictures; a click opens a picture, the tick toggles, × removes (asking first when
// the picture has changes); Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PageDown and Ctrl+PageUp go round; the wheel scrolls
// a full strip sideways.
void EditorTest::TrayStrip() {
    TrayTestScope scope(test::TempDir());
    Editor* e = Open(SolidBmp(400, 300, 0xFFFFFFFF));
    if (!e) return CHECK(false);
    const RECT one = e->Canvas();
    CHECK(!e->Tray() && e->StripH() == 0 && one.bottom == e->client_.bottom - e->S(kStatusH));  // exactly as before
    int part = 0;
    CHECK_EQ(e->StripHit({one.right / 2, one.bottom - 4}, &part), -1);
    e->Key(VK_TAB, true, false);
    CHECK_EQ(e->cur_, 0);

    CHECK(e->AddPicture(SolidBmp(320, 240, 0xFF3366CC), {}));
    CHECK(e->Tray() && e->Canvas().bottom == one.bottom - e->S(kStripH));
    POINT p = Center(e->ThumbRect(0));
    e->OnLButtonDown(p.x, p.y, false);
    e->OnLButtonUp(p.x, p.y);
    CHECK_EQ(e->cur_, 0);
    p = Center(e->TickRect(1));
    e->OnLButtonDown(p.x, p.y, false);
    CHECK(!e->tray_[1].keep && e->cur_ == 0);
    e->OnLButtonDown(p.x, p.y, false);
    CHECK(e->tray_[1].keep);

    CHECK(e->AddPicture(SolidBmp(200, 400, 0xFFCC6633), {}));
    CHECK_EQ(e->cur_, 2);
    e->Key(VK_TAB, true, false);
    CHECK_EQ(e->cur_, 0);
    e->Key(VK_TAB, true, true);
    CHECK_EQ(e->cur_, 2);
    e->Key(VK_NEXT, true, false);
    CHECK_EQ(e->cur_, 0);
    e->Key(VK_PRIOR, true, false);
    CHECK_EQ(e->cur_, 2);

    // ×: shown under the mouse; a picture without changes goes at once.
    p = Center(e->RemoveRect(0));
    e->OnMouseMove(p.x, p.y);
    CHECK(e->hoverThumb_ == 0 && e->hoverPart_ == 2);
    e->OnLButtonDown(p.x, p.y, false);
    CHECK(e->tray_.size() == 2 && e->cur_ == 1);
    // One with changes asks first.
    Drag(e, Tool::Rect, {20, 20}, {150, 150});
    int asks = 0;
    bool discard = false;
    e->answerDiscard_ = [&] {
        ++asks;
        return discard;
    };
    p = Center(e->RemoveRect(1));
    e->OnLButtonDown(p.x, p.y, false);
    CHECK(asks == 1 && e->tray_.size() == 2);
    discard = true;
    e->OnLButtonDown(p.x, p.y, false);
    CHECK(asks == 2 && e->tray_.size() == 1 && e->cur_ == 0);
    CHECK(!e->Tray() && e->Canvas().bottom == one.bottom);  // one picture left: no strip
    CHECK(e->base_->Width() == 320);

    // Twenty pictures overflow the strip: the newest is scrolled into view, and the wheel over the strip scrolls
    // it sideways instead of changing the size.
    for (int i = 0; i < 19; ++i) e->AddPicture(SolidBmp(160, 90, 0xFF000000 | (uint32_t)(i * 0x0A1B2C)), {});
    CHECK(e->tray_.size() == 20 && e->cur_ == 19 && e->stripScroll_ > 0);
    const RECT v = e->StripView(), last = e->ThumbBox(19);
    CHECK(last.right <= v.right);
    e->stripScroll_ = 0;
    const int level = e->level_;
    POINT sp = Center(e->StripView());
    ClientToScreen(e->hwnd, &sp);
    e->Proc(WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)-WHEEL_DELTA), MAKELPARAM(sp.x, sp.y));
    CHECK(e->stripScroll_ == e->S(kThumbW) + e->S(kThumbGap) && e->level_ == level);
    e->Proc(WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)WHEEL_DELTA), MAKELPARAM(sp.x, sp.y));
    CHECK_EQ(e->stripScroll_, 0);
    e->level_ = 1;
    POINT cp = Center(e->Canvas());
    ClientToScreen(e->hwnd, &cp);
    e->Proc(WM_MOUSEWHEEL, MAKEWPARAM(0, (WORD)(short)WHEEL_DELTA), MAKELPARAM(cp.x, cp.y));
    CHECK_EQ(e->level_, 2);  // over the picture the wheel still sets the size
    DestroyWindow(e->hwnd);
}

// A4: a tray holds 100 pictures; the 101st capture opens a window of its own instead of being lost.
void EditorTest::TrayLimit() {
    TrayTestScope scope(test::TempDir());
    const int before = EditorCount();
    Editor* e = Open(SolidBmp(64, 48, 0xFFFFFFFF));
    if (!e) return CHECK(false);
    for (int i = 1; i < kTrayMax; ++i) e->AddPicture(SolidBmp(64, 48, 0xFF000000 | (uint32_t)i), {});
    CHECK_EQ((int)e->tray_.size(), kTrayMax);
    CHECK(!e->AddPicture(SolidBmp(64, 48, 0xFF00FF00), {}));
    Editor* other = Editor::Route(SolidBmp(64, 48, 0xFFFF0000), {});
    CHECK(other != nullptr && other != e);
    CHECK_EQ((int)e->tray_.size(), kTrayMax);
    CHECK_EQ(EditorCount(), before + 2);
    if (other) DestroyWindow(other->hwnd);
    DestroyWindow(e->hwnd);
}

// A4 / D6: 40 captures of 2560 × 1440 in one tray keep the process's private bytes under about 1.5 GB. Only the
// current picture keeps its composed bitmaps; the others hold their screenshot and a thumbnail.
void EditorTest::TrayMemory() {
    TrayTestScope scope(test::TempDir());
    auto privateBytes = [] {
        PROCESS_MEMORY_COUNTERS_EX m{};
        m.cb = sizeof(m);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
        return (double)m.PrivateUsage;
    };
    const double before = privateBytes();
    Editor* e = Open(SolidBmp(2560, 1440, 0xFF336699));
    if (!e) return CHECK(false);
    for (int i = 1; i < 40; ++i) CHECK(e->AddPicture(SolidBmp(2560, 1440, 0xFF000000 | (uint32_t)(i * 0x050607)), {}));
    const double after = privateBytes(), raw = 40.0 * 2560 * 1440 * 4;
    char line[160];
    sprintf_s(line, "  (40 pictures of 2560 x 1440 in one tray: private bytes %.0f MB, %.0f MB more than before)\n", after / 1048576,
              (after - before) / 1048576);
    test::Out(line);
    CHECK_EQ(e->tray_.size(), 40u);
    CHECK(after < 1.5e9);
    CHECK(after - before > raw * 0.9);  // the measure sees the screenshots (a sanity check of the test itself)
    int stored = 0, thumbs = 0;
    for (const auto& p : e->tray_) {
        stored += p.base != nullptr;
        thumbs += p.thumb != nullptr;
    }
    CHECK_EQ(stored, 39);  // the current picture's screenshot is in the editor itself
    CHECK_EQ(thumbs, 39);
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
