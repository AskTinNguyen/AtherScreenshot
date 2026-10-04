#include "textdraw.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

using Microsoft::WRL::ComPtr;

namespace ather::textdraw {
namespace {

ID2D1Factory* D2D() {
    static ComPtr<ID2D1Factory> f = [] {
        ComPtr<ID2D1Factory> x;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, x.GetAddressOf());
        return x;
    }();
    return f.Get();
}

IDWriteFactory* DW() {
    static ComPtr<IDWriteFactory> f = [] {
        ComPtr<IDWriteFactory> x;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(x.GetAddressOf()));
        return x;
    }();
    return f.Get();
}

D2D1_COLOR_F ColorF(COLORREF c, float a = 1) { return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a); }

ComPtr<IDWriteTextLayout> MakeLayout(const std::wstring& text, const Style& st, float maxWidth) {
    ComPtr<IDWriteTextFormat> fmt;
    ComPtr<IDWriteTextLayout> layout;
    if (!DW() || FAILED(DW()->CreateTextFormat(st.family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)st.weight, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, std::max(1.f, st.size), L"", &fmt)))
        return nullptr;
    fmt->SetTextAlignment(st.center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (FAILED(DW()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), std::max(1.f, maxWidth), 100000.f, &layout))) return nullptr;
    return layout;
}

// A Direct2D render target over a copy of the bitmap's pixels; the result is copied back on End.
class Canvas {
public:
    explicit Canvas(Bitmap& dst) : dst_(dst) {
        const UINT w = (UINT)dst.Width(), h = (UINT)dst.Height();
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))) ||
            FAILED(wic_->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4, reinterpret_cast<BYTE*>(dst.Bits()), &bmp_)))
            return;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (!D2D() || FAILED(D2D()->CreateWicBitmapRenderTarget(bmp_.Get(), props, &rt_))) return;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt_->BeginDraw();
    }
    ~Canvas() {
        if (!rt_) return;
        const UINT w = (UINT)dst_.Width(), h = (UINT)dst_.Height();
        if (SUCCEEDED(rt_->EndDraw())) bmp_->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(dst_.Bits()));
    }
    ID2D1RenderTarget* rt() const { return rt_.Get(); }
    ComPtr<ID2D1SolidColorBrush> Brush(COLORREF c, float a = 1) const {
        ComPtr<ID2D1SolidColorBrush> b;
        rt_->CreateSolidColorBrush(ColorF(c, a), &b);
        return b;
    }

private:
    Bitmap& dst_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<IWICBitmap> bmp_;
    ComPtr<ID2D1RenderTarget> rt_;
};

}  // namespace

Extent Measure(const std::wstring& text, const Style& st, float maxWidth) {
    auto layout = MakeLayout(text, st, maxWidth);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return {};
    return {m.widthIncludingTrailingWhitespace, m.height};
}

void Draw(Bitmap& dst, const std::wstring& text, const Style& st, float x, float y, float width) {
    if (text.empty()) return;
    Canvas c(dst);
    if (!c.rt()) return;
    const auto opts = D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT;
    if (st.edge) {  // the edge: the text stamped around itself in the edge color
        auto plain = MakeLayout(text, st, width);
        auto eb = c.Brush(*st.edge, st.alpha);
        const float r = std::max(0.75f, st.edgeWidth);
        if (plain && eb)
            for (int i = 0; i < 12; ++i) {
                const double a = i * 3.14159265358979 / 6;
                c.rt()->DrawTextLayout(D2D1::Point2F(x + r * (float)std::cos(a), y + r * (float)std::sin(a)), plain.Get(), eb.Get(), opts);
            }
    }
    auto layout = MakeLayout(text, st, width);
    auto brush = c.Brush(st.color, st.alpha);
    if (!layout || !brush) return;
    std::vector<ComPtr<ID2D1SolidColorBrush>> keep;
    for (const auto& r : st.ranges) {
        if (r.start >= text.size()) continue;
        keep.push_back(c.Brush(r.color, st.alpha));
        layout->SetDrawingEffect(keep.back().Get(), DWRITE_TEXT_RANGE{(UINT32)r.start, (UINT32)std::min(r.length, text.size() - r.start)});
    }
    c.rt()->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush.Get(), opts);
}

void FillRounded(Bitmap& dst, float x, float y, float w, float h, float radius, COLORREF col, float alpha) {
    Canvas c(dst);
    if (!c.rt()) return;
    auto b = c.Brush(col, alpha);
    if (radius < 0.5f) c.rt()->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b.Get());
    else c.rt()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), b.Get());
}

void FillBubble(Bitmap& dst, float x, float y, float w, float h, float radius, float tail, COLORREF col) {
    ComPtr<ID2D1RoundedRectangleGeometry> body;
    ComPtr<ID2D1PathGeometry> tri, both;
    if (FAILED(D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), radius, radius), &body)) ||
        FAILED(D2D()->CreatePathGeometry(&tri)) || FAILED(D2D()->CreatePathGeometry(&both)))
        return;
    {
        ComPtr<ID2D1GeometrySink> s;
        tri->Open(&s);
        s->BeginFigure(D2D1::Point2F(x + w * 0.18f, y + h - 1), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1::Point2F(x + w * 0.1f, y + h + tail - 2));
        s->AddLine(D2D1::Point2F(x + w * 0.32f, y + h - 1));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
    }
    {
        ComPtr<ID2D1GeometrySink> s;
        both->Open(&s);
        body->CombineWithGeometry(tri.Get(), D2D1_COMBINE_MODE_UNION, nullptr, s.Get());
        s->Close();
    }
    Canvas c(dst);
    if (!c.rt()) return;
    auto b = c.Brush(col);
    c.rt()->FillGeometry(both.Get(), b.Get());
}

void StrokeEllipse(Bitmap& dst, float x, float y, float w, float h, float width, COLORREF col) {
    Canvas c(dst);
    if (!c.rt()) return;
    auto b = c.Brush(col);
    c.rt()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x + w / 2, y + h / 2), w / 2, h / 2), b.Get(), width);
}

void DropShadow(Bitmap& img, float blur, float dx, float dy, float alpha) {
    const int w = img.Width(), h = img.Height();
    if (w < 2 || h < 2) return;
    std::vector<float> a((size_t)w * h), tmp((size_t)std::max(w, h));
    uint32_t* px = img.Bits();
    for (size_t i = 0; i < a.size(); ++i) a[i] = (px[i] >> 24) / 255.f;
    const int r = std::max(1, (int)std::lround(blur / 2));
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
    for (int k = 0; k < 3; ++k) {
        for (int y = 0; y < h; ++y) pass(a.data() + (size_t)y * w, w, 1);
        for (int x = 0; x < w; ++x) pass(a.data() + x, h, w);
    }
    const int ox = (int)std::lround(dx), oy = (int)std::lround(dy);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox, sy = y - oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            const float s = a[(size_t)sy * w + sx] * alpha;
            if (s <= 0.002f) continue;
            uint32_t& p = px[(size_t)y * w + x];
            const float ca = (p >> 24) / 255.f;
            const uint32_t na = (uint32_t)std::lround((ca + (1 - ca) * s) * 255);
            p = (p & 0x00FFFFFFu) | std::min(255u, na) << 24;  // black shadow: color channels unchanged (premultiplied)
        }
}

}  // namespace ather::textdraw
