#include "logo.h"

#include <objidl.h>

#include <algorithm>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace ather {
namespace {

namespace gp = Gdiplus;

// Geometry traced from the brand artwork, in its own coordinate space.
// Content spans x 90..540, y 228..690.
const gp::PointF kA[] = {{90, 690}, {270, 296}, {360, 296}, {540, 690}, {460, 690}, {313, 368}, {165, 690}};
const gp::RectF kBar(307, 483, 13, 127);
const gp::RectF kFive(424, 212, 128, 170);  // box the "5" glyph is fitted into
constexpr float kMinX = 90, kMaxX = 552, kMinY = 212, kMaxY = 690;

gp::Color GC(COLORREF c) { return gp::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

void Draw(gp::Graphics& g, gp::RectF box, bool tile) {
    g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
    g.SetTextRenderingHint(gp::TextRenderingHintAntiAlias);
    if (tile) {
        const float r = box.Width * 0.22f;
        gp::GraphicsPath p;
        p.AddArc(box.X, box.Y, 2 * r, 2 * r, 180, 90);
        p.AddArc(box.GetRight() - 2 * r, box.Y, 2 * r, 2 * r, 270, 90);
        p.AddArc(box.GetRight() - 2 * r, box.GetBottom() - 2 * r, 2 * r, 2 * r, 0, 90);
        p.AddArc(box.X, box.GetBottom() - 2 * r, 2 * r, 2 * r, 90, 90);
        p.CloseFigure();
        gp::SolidBrush bg(GC(theme::kBg));
        g.FillPath(&bg, &p);
        if (box.Width >= 32) {
            gp::Pen edge(gp::Color(255, 44, 46, 40), std::max(1.f, box.Width / 64));
            g.DrawPath(&edge, &p);
        }
        box.Inflate(-box.Width * 0.14f, -box.Height * 0.14f);
    }
    // Fit the mark into the box, centered.
    const float w = kMaxX - kMinX, h = kMaxY - kMinY, k = std::min(box.Width / w, box.Height / h);
    const float ox = box.X + (box.Width - w * k) / 2 - kMinX * k, oy = box.Y + (box.Height - h * k) / 2 - kMinY * k;
    auto P = [&](gp::PointF p) { return gp::PointF(ox + p.X * k, oy + p.Y * k); };
    gp::PointF a[7];
    for (int i = 0; i < 7; ++i) a[i] = P(kA[i]);
    gp::SolidBrush gray(GC(theme::kLogoGray)), lime(GC(theme::kAccent));
    g.FillPolygon(&gray, a, 7);
    if (w * k >= 20) {  // the light bar disappears at tiny sizes anyway
        gp::RectF bar(ox + kBar.X * k, oy + kBar.Y * k, std::max(1.f, kBar.Width * k), kBar.Height * k);
        g.FillRectangle(&lime, bar);
    }
    // The superscript 5, fitted to its box using the glyph's real outline.
    gp::FontFamily fam(L"Bahnschrift");
    gp::GraphicsPath five;
    gp::StringFormat sf(gp::StringFormat::GenericTypographic());
    five.AddString(L"5", 1, fam.IsAvailable() ? &fam : gp::FontFamily::GenericSansSerif(), gp::FontStyleBold, 100,
                   gp::PointF(0, 0), &sf);
    gp::RectF gb;
    five.GetBounds(&gb);
    gp::Matrix m;
    const float fk = std::min(kFive.Width * k / gb.Width, kFive.Height * k / gb.Height);
    m.Translate(ox + kFive.X * k + (kFive.Width * k - gb.Width * fk) / 2, oy + kFive.Y * k);
    m.Scale(fk, fk);
    m.Translate(-gb.X, -gb.Y);
    five.Transform(&m);
    g.FillPath(&lime, &five);
}

}  // namespace

BitmapPtr RenderLogo(int size, bool tile) {
    gp::Bitmap canvas(size, size, PixelFormat32bppARGB);
    {
        gp::Graphics g(&canvas);
        g.Clear(gp::Color(0, 0, 0, 0));
        Draw(g, gp::RectF(0, 0, (float)size, (float)size), tile);
    }
    auto out = Bitmap::Create(size, size);
    if (!out) return nullptr;
    gp::BitmapData bd{};
    gp::Rect all(0, 0, size, size);
    if (canvas.LockBits(&all, gp::ImageLockModeRead, PixelFormat32bppARGB, &bd) == gp::Ok) {
        for (int y = 0; y < size; ++y)
            memcpy(out->Bits() + (size_t)y * size, static_cast<BYTE*>(bd.Scan0) + (size_t)y * bd.Stride, (size_t)size * 4);
        canvas.UnlockBits(&bd);
    }
    return out;
}

void DrawLogo(HDC dc, const RECT& box) {
    gp::Graphics g(dc);
    Draw(g, gp::RectF((float)box.left, (float)box.top, (float)RectW(box), (float)RectH(box)), false);
}

HICON CreateLogoIcon(int size) {
    auto bmp = RenderLogo(size, true);
    if (!bmp) return nullptr;
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, bmp->Handle()};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    return icon;
}

bool WriteLogoIco(const std::wstring& path) {
    CLSID png{};
    UINT n = 0, bytes = 0;
    gp::GetImageEncodersSize(&n, &bytes);
    std::vector<BYTE> buf(bytes);
    auto* enc = reinterpret_cast<gp::ImageCodecInfo*>(buf.data());
    gp::GetImageEncoders(n, bytes, enc);
    for (UINT i = 0; i < n; ++i)
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) png = enc[i].Clsid;

    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<std::vector<BYTE>> frames;
    for (int sz : sizes) {
        auto bmp = RenderLogo(sz, true);
        gp::Bitmap frame(sz, sz, sz * 4, PixelFormat32bppARGB, reinterpret_cast<BYTE*>(bmp->Bits()));
        IStream* st = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &st);
        if (!st || frame.Save(st, &png, nullptr) != gp::Ok) {
            if (st) st->Release();
            return false;
        }
        STATSTG stat{};
        st->Stat(&stat, STATFLAG_NONAME);
        std::vector<BYTE> data((size_t)stat.cbSize.QuadPart);
        LARGE_INTEGER zero{};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        st->Read(data.data(), (ULONG)data.size(), &read);
        st->Release();
        frames.push_back(std::move(data));
    }
    // ICONDIR + ICONDIRENTRY[] + PNG payloads (PNG frames are valid in .ico since Vista).
    std::vector<BYTE> ico;
    auto u16 = [&](uint16_t v) { ico.push_back(v & 255); ico.push_back(v >> 8); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) ico.push_back((v >> (8 * i)) & 255); };
    u16(0);
    u16(1);
    u16((uint16_t)frames.size());
    uint32_t offset = 6 + 16 * (uint32_t)frames.size();
    for (size_t i = 0; i < frames.size(); ++i) {
        ico.push_back(sizes[i] >= 256 ? 0 : (BYTE)sizes[i]);
        ico.push_back(sizes[i] >= 256 ? 0 : (BYTE)sizes[i]);
        ico.push_back(0);
        ico.push_back(0);
        u16(1);
        u16(32);
        u32((uint32_t)frames[i].size());
        u32(offset);
        offset += (uint32_t)frames[i].size();
    }
    for (const auto& f : frames) ico.insert(ico.end(), f.begin(), f.end());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, ico.data(), (DWORD)ico.size(), &wr, nullptr) && wr == ico.size();
    CloseHandle(h);
    return ok;
}

}  // namespace ather
