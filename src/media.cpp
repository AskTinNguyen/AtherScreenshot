#include "media.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "propsys")

using Microsoft::WRL::ComPtr;

namespace ather {

void EnsureMediaFoundation() {
    static std::once_flag once;
    std::call_once(once, [] { MFStartup(MF_VERSION); });
}

static ComPtr<IWICImagingFactory> Wic() {
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool ImageSize(const std::wstring& path, int* w, int* h) {
    auto f = Wic();
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT fw = 0, fh = 0;
    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(frame->GetSize(&fw, &fh)))
        return false;
    *w = (int)fw;
    *h = (int)fh;
    return true;
}

BitmapPtr LoadImageScaled(const std::wstring& path, int maxSide, int* fullW, int* fullH) {
    auto f = Wic();
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> src;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (!f) return nullptr;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || !w || !h) return nullptr;
    if (fullW) *fullW = (int)w;
    if (fullH) *fullH = (int)h;
    src = frame;
    UINT ow = w, oh = h;
    if (maxSide > 0 && (int)std::max(w, h) > maxSide) {
        const double k = (double)maxSide / std::max(w, h);
        ow = std::max(1u, (UINT)std::lround(w * k));
        oh = std::max(1u, (UINT)std::lround(h * k));
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(frame.Get(), ow, oh, WICBitmapInterpolationModeFant)))
            src = sc;
        else
            ow = w, oh = h;
    }
    hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return nullptr;
    auto bmp = Bitmap::Create((int)ow, (int)oh);
    if (!bmp || FAILED(conv->CopyPixels(nullptr, ow * 4, ow * oh * 4, reinterpret_cast<BYTE*>(bmp->Bits())))) return nullptr;
    uint32_t* p = bmp->Bits();
    for (size_t i = 0, n = (size_t)ow * oh; i < n; ++i) {  // premultiplied over white
        const uint32_t a = p[i] >> 24, k = 255 - a;
        p[i] = 0xFF000000u | (((p[i] >> 16) & 255) + k) << 16 | (((p[i] >> 8) & 255) + k) << 8 | ((p[i] & 255) + k);
    }
    return bmp;
}

double GifDuration(const std::wstring& path) {
    auto f = Wic();
    ComPtr<IWICBitmapDecoder> dec;
    UINT n = 0;
    if (!f || FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrameCount(&n)))
        return 0;
    double total = 0;
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICMetadataQueryReader> q;
        double d = 0.1;
        if (SUCCEEDED(dec->GetFrame(i, &fr)) && SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(q->GetMetadataByName(L"/grctlext/Delay", &v)) && v.vt == VT_UI2 && v.uiVal > 0) d = v.uiVal / 100.0;
            PropVariantClear(&v);
        }
        total += d;
    }
    return total;
}

BitmapPtr Resample(const Bitmap& src, int w, int h) {
    auto out = Bitmap::Create(w, h);
    if (!out) return nullptr;
    const int sw = src.Width(), sh = src.Height();
    const uint32_t* s = src.Bits();
    uint32_t* d = out->Bits();
    const double kx = (double)sw / w, ky = (double)sh / h;
    if (kx < 1 || ky < 1) {  // growing in at least one direction: bilinear
        for (int y = 0; y < h; ++y) {
            const double fy = std::clamp((y + 0.5) * ky - 0.5, 0.0, sh - 1.0);
            const int y0 = (int)fy, y1 = std::min(y0 + 1, sh - 1);
            const double ty = fy - y0;
            for (int x = 0; x < w; ++x) {
                const double fx = std::clamp((x + 0.5) * kx - 0.5, 0.0, sw - 1.0);
                const int x0 = (int)fx, x1 = std::min(x0 + 1, sw - 1);
                const double tx = fx - x0;
                const uint32_t q[4] = {s[(size_t)y0 * sw + x0], s[(size_t)y0 * sw + x1], s[(size_t)y1 * sw + x0], s[(size_t)y1 * sw + x1]};
                uint32_t px = 0;
                for (int sh8 = 0; sh8 <= 24; sh8 += 8) {
                    const double v = ((q[0] >> sh8) & 255) * (1 - tx) * (1 - ty) + ((q[1] >> sh8) & 255) * tx * (1 - ty) +
                                     ((q[2] >> sh8) & 255) * (1 - tx) * ty + ((q[3] >> sh8) & 255) * tx * ty;
                    px |= (uint32_t)std::lround(v) << sh8;
                }
                d[(size_t)y * w + x] = px;
            }
        }
        return out;
    }
    for (int y = 0; y < h; ++y) {
        const int ya = (int)(y * ky), yb = std::max(ya + 1, (int)((y + 1) * ky));
        for (int x = 0; x < w; ++x) {
            const int xa = (int)(x * kx), xb = std::max(xa + 1, (int)((x + 1) * kx));
            uint64_t acc[4] = {};
            for (int yy = ya; yy < yb && yy < sh; ++yy)
                for (int xx = xa; xx < xb && xx < sw; ++xx) {
                    const uint32_t p = s[(size_t)yy * sw + xx];
                    acc[0] += p & 255;
                    acc[1] += (p >> 8) & 255;
                    acc[2] += (p >> 16) & 255;
                    acc[3] += p >> 24;
                }
            const uint64_t n = (uint64_t)(std::min(yb, sh) - ya) * (std::min(xb, sw) - xa);
            d[(size_t)y * w + x] = (uint32_t)(acc[0] / n) | (uint32_t)(acc[1] / n) << 8 | (uint32_t)(acc[2] / n) << 16 |
                                   (uint32_t)(acc[3] / n) << 24;
        }
    }
    return out;
}

bool ProbeVideo(const std::wstring& path, VideoInfo* info, double at, int maxSide, BitmapPtr* frame) {
    EnsureMediaFoundation();
    ComPtr<IMFAttributes> attr;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), &reader);
    if (FAILED(hr)) return false;
    VideoInfo vi;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
        vi.duration = var.uhVal.QuadPart / 1e7;
    PropVariantClear(&var);
    ComPtr<IMFMediaType> native, audio;
    if (FAILED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) return false;
    vi.hasAudio = SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio));
    UINT32 w = 0, h = 0, num = 0, den = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFVideoArea area{};
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)) && area.Area.cx > 0) {
        w = (UINT32)area.Area.cx;  // 1920x1088 coded, 1920x1080 shown
        h = (UINT32)area.Area.cy;
    }
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den) vi.fps = (double)num / den;
    vi.w = (int)w;
    vi.h = (int)h;
    if (info) *info = vi;
    if (!frame) return true;

    ComPtr<IMFMediaType> rgb;
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    hr = MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
    if (FAILED(hr)) return false;
    ComPtr<IMFMediaType> cur;
    UINT32 cw = 0, ch = 0;
    if (FAILED(reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) ||
        FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch)))
        return false;
    if (at > 0) {
        PROPVARIANT pos;
        InitPropVariantFromInt64((LONGLONG)(at * 1e7), &pos);
        reader->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
    }
    ComPtr<IMFSample> sample;
    for (int i = 0; i < 400; ++i) {  // a seek lands on the key frame before `at`: decode forward to it
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (s) sample = s;
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || (s && ts / 1e7 >= at - 0.02)) break;
    }
    if (!sample) return false;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
    auto full = Bitmap::Create((int)w, (int)h);
    if (!full) return false;
    ComPtr<IMF2DBuffer> b2;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* data = nullptr;
    DWORD len = 0;
    const bool twoD = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
    if (!twoD) {
        if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
        scan0 = data;
        pitch = (LONG)cw * 4;
    }
    for (UINT32 y = 0; y < h && y < ch; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(scan0 + (LONG_PTR)pitch * (LONG)y);
        uint32_t* dst = full->Bits() + (size_t)y * w;
        for (UINT32 x = 0; x < w && x < cw; ++x) dst[x] = row[x] | 0xFF000000u;
    }
    if (twoD) b2->Unlock2D();
    else buf->Unlock();
    if (maxSide > 0 && (int)std::max(w, h) > maxSide) {
        const double k = (double)maxSide / std::max(w, h);
        *frame = Resample(*full, std::max(1, (int)std::lround(w * k)), std::max(1, (int)std::lround(h * k)));
    } else {
        *frame = full;
    }
    return *frame != nullptr;
}

}  // namespace ather
