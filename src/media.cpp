#include "media.h"

#include <codecapi.h>
#include <emmintrin.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
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

void BgraToNv12(const uint32_t* px, int w, int h, uint8_t* out) {
    const bool hd = h > 576;
    const int yr = hd ? 47 : 66, yg = hd ? 157 : 129, yb = hd ? 16 : 25;
    const int ur = hd ? -26 : -38, ug = hd ? -87 : -74, ub = 112;
    const int vr = 112, vg = hd ? -102 : -94, vb = hd ? -10 : -18;
    // Eight pixels of two rows at a time; per pixel the same integer arithmetic as the scalar tail below.
    const __m128i zero = _mm_setzero_si128();
    const __m128i cy = _mm_setr_epi16((short)yb, (short)yg, (short)yr, 0, (short)yb, (short)yg, (short)yr, 0);
    const __m128i cu = _mm_setr_epi16((short)ub, (short)ug, (short)ur, 0, (short)ub, (short)ug, (short)ur, 0);
    const __m128i cv = _mm_setr_epi16((short)vb, (short)vg, (short)vr, 0, (short)vb, (short)vg, (short)vr, 0);
    const __m128i r128 = _mm_set1_epi32(128), y16 = _mm_set1_epi32(16), two = _mm_set1_epi32(2);
    // Σ coefficient × channel for four pixels (one 32-bit lane each), rounded and shifted like `(s + 128) >> 8`.
    auto dot4 = [&](__m128i p, __m128i c) {
        const __m128i lo = _mm_madd_epi16(_mm_unpacklo_epi8(p, zero), c), hi = _mm_madd_epi16(_mm_unpackhi_epi8(p, zero), c);
        const __m128 a = _mm_castsi128_ps(lo), b = _mm_castsi128_ps(hi);
        const __m128i s = _mm_add_epi32(_mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0))),
                                        _mm_castps_si128(_mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1))));
        return _mm_srai_epi32(_mm_add_epi32(s, r128), 8);
    };
    // Each pair of neighbors summed: lanes (0+1, 2+3) of a and of b.
    auto pairs = [](__m128i a, __m128i b) {
        const __m128 x = _mm_castsi128_ps(a), y = _mm_castsi128_ps(b);
        return _mm_add_epi32(_mm_castps_si128(_mm_shuffle_ps(x, y, _MM_SHUFFLE(2, 0, 2, 0))), _mm_castps_si128(_mm_shuffle_ps(x, y, _MM_SHUFFLE(3, 1, 3, 1))));
    };
    uint8_t* uv = out + (size_t)w * h;
    for (int y = 0; y < h; y += 2) {
        const uint32_t* r0 = px + (size_t)y * w;
        const uint32_t* r1 = r0 + w;
        uint8_t* y0 = out + (size_t)y * w;
        uint8_t* y1 = y0 + w;
        uint8_t* c = uv + (size_t)(y / 2) * w;
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            const __m128i a0 = _mm_loadu_si128((const __m128i*)(r0 + x)), a1 = _mm_loadu_si128((const __m128i*)(r0 + x + 4));
            const __m128i b0 = _mm_loadu_si128((const __m128i*)(r1 + x)), b1 = _mm_loadu_si128((const __m128i*)(r1 + x + 4));
            const __m128i ya = _mm_packs_epi32(_mm_add_epi32(dot4(a0, cy), y16), _mm_add_epi32(dot4(a1, cy), y16));
            const __m128i yb8 = _mm_packs_epi32(_mm_add_epi32(dot4(b0, cy), y16), _mm_add_epi32(dot4(b1, cy), y16));
            _mm_storel_epi64((__m128i*)(y0 + x), _mm_packus_epi16(ya, ya));
            _mm_storel_epi64((__m128i*)(y1 + x), _mm_packus_epi16(yb8, yb8));
            // U and V of each pixel (the +128 offsets cancel out of the rounding: 4 × 128 is added back below).
            const __m128i su = _mm_add_epi32(pairs(dot4(a0, cu), dot4(a1, cu)), pairs(dot4(b0, cu), dot4(b1, cu)));
            const __m128i sv = _mm_add_epi32(pairs(dot4(a0, cv), dot4(a1, cv)), pairs(dot4(b0, cv), dot4(b1, cv)));
            const __m128i off = _mm_set1_epi32(4 * 128);
            const __m128i u = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(su, off), two), 2);
            const __m128i v = _mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(sv, off), two), 2);
            const __m128i uvw = _mm_or_si128(u, _mm_slli_epi32(v, 16));  // U, V as 16-bit pairs
            const __m128i b = _mm_packus_epi16(uvw, uvw);
            _mm_storel_epi64((__m128i*)(c + x), b);
        }
        for (; x < w; x += 2) {
            int su = 0, sv = 0;
            for (int k = 0; k < 4; ++k) {
                const uint32_t p = (k < 2 ? r0 : r1)[x + (k & 1)];
                const int R = (p >> 16) & 255, G = (p >> 8) & 255, B = p & 255;
                (k < 2 ? y0 : y1)[x + (k & 1)] = (uint8_t)(((yr * R + yg * G + yb * B + 128) >> 8) + 16);
                su += ((ur * R + ug * G + ub * B + 128) >> 8) + 128;
                sv += ((vr * R + vg * G + vb * B + 128) >> 8) + 128;
            }
            c[x] = (uint8_t)((su + 2) >> 2);
            c[x + 1] = (uint8_t)((sv + 2) >> 2);
        }
    }
}
BitmapPtr RotateBitmap(const Bitmap& src, int degrees) {
    const int w = src.Width(), h = src.Height();
    auto out = degrees == 180 ? Bitmap::Create(w, h) : Bitmap::Create(h, w);
    if (!out) return nullptr;
    const uint32_t* s = src.Bits();
    uint32_t* d = out->Bits();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t c = s[(size_t)y * w + x];
            if (degrees == 90) d[(size_t)x * h + (h - 1 - y)] = c;
            else if (degrees == 180) d[(size_t)(h - 1 - y) * w + (w - 1 - x)] = c;
            else d[(size_t)(w - 1 - x) * h + y] = c;
        }
    return out;
}

BitmapPtr FitInto(const Bitmap& src, int w, int h) {
    auto out = Bitmap::Create(w, h);
    if (!out) return nullptr;
    std::fill(out->Bits(), out->Bits() + (size_t)w * h, 0xFF000000u);
    const double k = std::min((double)w / std::max(1, src.Width()), (double)h / std::max(1, src.Height()));
    const int fw = std::clamp((int)std::lround(src.Width() * k), 1, w), fh = std::clamp((int)std::lround(src.Height() * k), 1, h);
    const bool same = fw == src.Width() && fh == src.Height();
    BitmapPtr fit = same ? nullptr : Resample(src, fw, fh);
    if (!same && !fit) return nullptr;  // out of memory: no frame rather than the wrong-sized one
    const Bitmap& from = same ? src : *fit;
    const int ox = (w - fw) / 2, oy = (h - fh) / 2;
    for (int y = 0; y < fh; ++y) memcpy(out->Bits() + (size_t)(y + oy) * w + ox, from.Bits() + (size_t)y * fw, (size_t)fw * 4);
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
    // Phone videos are stored sideways with a rotation for players to apply; show them upright.
    UINT32 rotation = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, 0) % 360;
    if (rotation % 90) rotation = 0;
    vi.w = (int)(rotation % 180 ? h : w);
    vi.h = (int)(rotation % 180 ? w : h);
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
    if (rotation && !(full = RotateBitmap(*full, (int)rotation))) return false;
    w = (UINT32)full->Width();
    h = (UINT32)full->Height();
    if (maxSide > 0 && (int)std::max(w, h) > maxSide) {
        const double k = (double)maxSide / std::max(w, h);
        *frame = Resample(*full, std::max(1, (int)std::lround(w * k)), std::max(1, (int)std::lround(h * k)));
    } else {
        *frame = full;
    }
    return *frame != nullptr;
}

// ---- Mp4Writer ----

struct Mp4Writer::Impl {
    ComPtr<IMFSinkWriter> writer;
    std::mutex mu;
    DWORD video = 0, audio = 0;
    int w = 0, h = 0, rate = 0, channels = 0;
    bool nv12 = false, hardware = false;
    int64_t frames = 0;
};

Mp4Writer::Mp4Writer() : p_(std::make_unique<Impl>()) {}
Mp4Writer::~Mp4Writer() = default;
int64_t Mp4Writer::Frames() const { return p_->frames; }
bool Mp4Writer::HardwareEncoder() const { return p_->hardware; }

// The AAC stream of an MP4 (192 kbps), fed 16-bit PCM.
static HRESULT AddAacStream(IMFSinkWriter* w, int rate, int channels, DWORD* index) {
    ComPtr<IMFMediaType> aout, ain;
    HRESULT hr = MFCreateMediaType(&aout);
    if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
    if (SUCCEEDED(hr)) hr = w->AddStream(aout.Get(), index);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&ain);
    if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
    if (SUCCEEDED(hr)) hr = ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(*index, ain.Get(), nullptr);
    return hr;
}

HRESULT Mp4Writer::Begin(const std::wstring& path, int w, int h, int fps, int audioRate, int audioChannels, int rotation, bool nv12) {
    EnsureMediaFoundation();
    p_->nv12 = nv12;
    p_->w = w;
    p_->h = h;
    p_->rate = audioRate;
    p_->channels = audioChannels;
    ComPtr<IMFAttributes> attr;
    ComPtr<IMFMediaType> out, in;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attr.Get(), &p_->writer);
    // Screen content: ~0.12 bits per pixel per frame keeps text crisp.
    const UINT32 bitrate = (UINT32)std::clamp<double>((double)w * h * fps * 0.12, 2e6, 50e6);
    if (w <= 0) {  // sound only
        if (SUCCEEDED(hr)) hr = audioRate > 0 ? AddAacStream(p_->writer.Get(), audioRate, audioChannels, &p_->audio) : E_INVALIDARG;
        if (SUCCEEDED(hr)) hr = p_->writer->BeginWriting();
        if (FAILED(hr)) p_->writer.Reset();
        return hr;
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr) && rotation) hr = out->SetUINT32(MF_MT_VIDEO_ROTATION, (UINT32)rotation);
    if (SUCCEEDED(hr)) hr = p_->writer->AddStream(out.Get(), &p_->video);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * (nv12 ? 1 : 4)));  // positive = top-down
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, w, h);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = p_->writer->SetInputMediaType(p_->video, in.Get(), nullptr);
    if (SUCCEEDED(hr) && audioRate > 0) hr = AddAacStream(p_->writer.Get(), audioRate, audioChannels, &p_->audio);
    ComPtr<IMFTransform> enc;
    if (SUCCEEDED(hr) && SUCCEEDED(p_->writer->GetServiceForStream(p_->video, GUID_NULL, IID_PPV_ARGS(&enc)))) {
        ComPtr<IMFAttributes> ea;
        UINT32 n = 0;
        p_->hardware = SUCCEEDED(enc->GetAttributes(&ea)) && SUCCEEDED(ea->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &n));
    }
    if (SUCCEEDED(hr)) hr = p_->writer->BeginWriting();    if (FAILED(hr)) p_->writer.Reset();
    return hr;
}

// A media buffer over memory someone else owns (kept alive by `owner`), so a frame reaches the encoder uncopied.
class HeldBuffer : public IMFMediaBuffer {
public:
    HeldBuffer(std::shared_ptr<const uint8_t> data, DWORD len) : data_(std::move(data)), len_(len) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaBuffer)) {
            *ppv = static_cast<IMFMediaBuffer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Lock(BYTE** p, DWORD* max, DWORD* cur) override {
        if (!p) return E_POINTER;
        *p = const_cast<BYTE*>(data_.get());  // the encoder only reads it
        if (max) *max = len_;
        if (cur) *cur = len_;
        return S_OK;
    }
    STDMETHODIMP Unlock() override { return S_OK; }
    STDMETHODIMP GetCurrentLength(DWORD* n) override { return n ? (*n = len_, S_OK) : E_POINTER; }
    STDMETHODIMP SetCurrentLength(DWORD n) override { return n <= len_ ? S_OK : E_INVALIDARG; }
    STDMETHODIMP GetMaxLength(DWORD* n) override { return n ? (*n = len_, S_OK) : E_POINTER; }

private:
    std::atomic<ULONG> ref_{1};
    std::shared_ptr<const uint8_t> data_;
    DWORD len_;
};

static HRESULT WriteBuffer(IMFSinkWriter* w, DWORD stream, IMFMediaBuffer* buf, int64_t t, int64_t dur) {
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    if (FAILED(hr)) return hr;
    sample->SetSampleTime(t);
    sample->SetSampleDuration(dur);
    return w->WriteSample(stream, sample.Get());
}

static HRESULT WriteBytes(IMFSinkWriter* w, DWORD stream, const void* data, DWORD bytes, int64_t t, int64_t dur) {
    ComPtr<IMFMediaBuffer> buf;
    ComPtr<IMFSample> sample;
    BYTE* dst = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(bytes, &buf);
    if (SUCCEEDED(hr)) hr = buf->Lock(&dst, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    memcpy(dst, data, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    if (FAILED(hr = MFCreateSample(&sample))) return hr;
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(t);
    sample->SetSampleDuration(dur);
    return w->WriteSample(stream, sample.Get());
}

HRESULT Mp4Writer::WriteFrame(const uint32_t* px, int64_t t, int64_t duration) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || p_->nv12) return E_UNEXPECTED;
    const HRESULT hr = WriteBytes(p_->writer.Get(), p_->video, px, (DWORD)p_->w * p_->h * 4, t, duration);
    if (SUCCEEDED(hr)) ++p_->frames;
    return hr;
}

HRESULT Mp4Writer::WriteNv12(const uint8_t* yuv, int64_t t, int64_t duration) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || !p_->nv12) return E_UNEXPECTED;
    const HRESULT hr = WriteBytes(p_->writer.Get(), p_->video, yuv, (DWORD)((size_t)p_->w * p_->h * 3 / 2), t, duration);
    if (SUCCEEDED(hr)) ++p_->frames;
    return hr;
}

HRESULT Mp4Writer::WriteNv12(std::shared_ptr<const uint8_t> yuv, int64_t t, int64_t duration) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || !p_->nv12 || !yuv) return E_UNEXPECTED;
    ComPtr<IMFMediaBuffer> buf;
    buf.Attach(new HeldBuffer(std::move(yuv), (DWORD)((size_t)p_->w * p_->h * 3 / 2)));
    const HRESULT hr = WriteBuffer(p_->writer.Get(), p_->video, buf.Get(), t, duration);
    if (SUCCEEDED(hr)) ++p_->frames;
    return hr;
}

HRESULT Mp4Writer::WriteAudio(const int16_t* pcm, uint32_t frames, int64_t t) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || p_->rate <= 0 || !frames) return E_UNEXPECTED;
    return WriteBytes(p_->writer.Get(), p_->audio, pcm, frames * p_->channels * 2, t, (int64_t)frames * 10'000'000 / p_->rate);
}

// ---- Mp4Joiner ----

struct Mp4Joiner::Impl {
    ComPtr<IMFSinkWriter> writer;
    std::vector<ComPtr<IMFSourceReader>> parts;
    ComPtr<IMFSourceReader> sound;  // the sound, encoded already (CopyAudio)
    ComPtr<IMFSample> soundNext;    // read and not written yet
    bool soundEnd = false;
    std::vector<bool> started;  // a frame of the part copied already
    std::mutex mu;
    DWORD video = 0, audio = 0;
    int rate = 0, channels = 0;
    int64_t frames = 0;
};

Mp4Joiner::Mp4Joiner() : p_(std::make_unique<Impl>()) {}
Mp4Joiner::~Mp4Joiner() = default;

HRESULT Mp4Joiner::Begin(const std::wstring& path, const std::vector<std::wstring>& parts, int audioRate, int audioChannels, const std::wstring& audioPart) {
    EnsureMediaFoundation();
    p_->rate = audioRate;
    p_->channels = audioChannels;
    ComPtr<IMFMediaType> type;  // the first part's, for the joined stream
    std::vector<UINT8> sets;    // its parameter sets: every part must have the same
    HRESULT hr = parts.empty() ? E_INVALIDARG : S_OK;
    for (size_t i = 0; i < parts.size() && SUCCEEDED(hr); ++i) {
        ComPtr<IMFSourceReader> r;
        ComPtr<IMFMediaType> native;
        hr = MFCreateSourceReaderFromURL(parts[i].c_str(), nullptr, &r);
        if (SUCCEEDED(hr)) hr = r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) hr = r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (SUCCEEDED(hr)) hr = r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
        GUID sub{};
        if (SUCCEEDED(hr) && (FAILED(native->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_H264)) hr = MF_E_INVALIDMEDIATYPE;
        if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native.Get());  // as stored
        UINT32 n = 0;
        std::vector<UINT8> s;
        if (SUCCEEDED(hr) && SUCCEEDED(native->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) && n) {
            s.resize(n);
            hr = native->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, s.data(), n, nullptr);
        }
        if (SUCCEEDED(hr) && i == 0) {
            type = native;
            sets = s;
        } else if (SUCCEEDED(hr) && s != sets) {
            hr = MF_E_INVALIDMEDIATYPE;
        }
        if (SUCCEEDED(hr)) p_->parts.push_back(r);
    }
    p_->started.assign(p_->parts.size(), false);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &p_->writer);
    if (SUCCEEDED(hr)) hr = p_->writer->AddStream(type.Get(), &p_->video);
    if (SUCCEEDED(hr)) hr = p_->writer->SetInputMediaType(p_->video, type.Get(), nullptr);  // copied as is
    if (SUCCEEDED(hr) && !audioPart.empty()) {
        ComPtr<IMFMediaType> native;
        hr = MFCreateSourceReaderFromURL(audioPart.c_str(), nullptr, &p_->sound);
        if (SUCCEEDED(hr)) hr = p_->sound->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (SUCCEEDED(hr)) hr = p_->sound->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        if (SUCCEEDED(hr)) hr = p_->sound->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native);
        if (SUCCEEDED(hr)) hr = p_->sound->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, native.Get());  // as stored
        if (SUCCEEDED(hr)) hr = p_->writer->AddStream(native.Get(), &p_->audio);
        if (SUCCEEDED(hr)) hr = p_->writer->SetInputMediaType(p_->audio, native.Get(), nullptr);
    } else if (SUCCEEDED(hr) && audioRate > 0) {
        hr = AddAacStream(p_->writer.Get(), audioRate, audioChannels, &p_->audio);
    }
    if (SUCCEEDED(hr)) hr = p_->writer->BeginWriting();
    if (FAILED(hr)) {
        p_->writer.Reset();
        p_->parts.clear();
        p_->sound.Reset();
    }
    return hr;
}

HRESULT Mp4Joiner::CopyAudio(int64_t until) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || !p_->sound) return E_UNEXPECTED;
    for (;;) {
        if (!p_->soundNext && !p_->soundEnd) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(p_->sound->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s))) return E_FAIL;
            if (s) p_->soundNext = s;
            else if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) p_->soundEnd = true;
            continue;
        }
        LONGLONG ts = 0;
        if (!p_->soundNext || FAILED(p_->soundNext->GetSampleTime(&ts)) || ts >= until) return S_OK;
        const HRESULT hr = p_->writer->WriteSample(p_->audio, p_->soundNext.Get());
        p_->soundNext.Reset();
        if (FAILED(hr)) return hr;
    }
}

HRESULT Mp4Joiner::CopyFrames(size_t part, int64_t first, int count, int fps) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || part >= p_->parts.size() || fps <= 0) return E_UNEXPECTED;
    IMFSourceReader* r = p_->parts[part].Get();
    for (int j = 0; j < count; ++j) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        do {
            if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) return E_FAIL;
            if (!s && (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) return MF_E_END_OF_STREAM;
        } while (!s);
        const bool key = MFGetAttributeUINT32(s.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
        if (!p_->started[part] && !key) return MF_E_UNEXPECTED;  // a piece that doesn't start on a key frame can't be joined
        p_->started[part] = true;
        // A fresh sample around the same data, at the joined video's time for it.
        ComPtr<IMFMediaBuffer> buf;
        ComPtr<IMFSample> out;
        HRESULT hr = s->ConvertToContiguousBuffer(&buf);
        if (SUCCEEDED(hr)) hr = MFCreateSample(&out);
        if (SUCCEEDED(hr)) hr = out->AddBuffer(buf.Get());
        if (SUCCEEDED(hr) && key) hr = out->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        const int64_t i = first + j;
        if (SUCCEEDED(hr)) hr = out->SetSampleTime(std::llround(i * 1e7 / fps));
        if (SUCCEEDED(hr)) hr = out->SetSampleDuration(std::llround(1e7 / fps));
        if (SUCCEEDED(hr)) hr = p_->writer->WriteSample(p_->video, out.Get());
        if (FAILED(hr)) return hr;
        ++p_->frames;
    }
    return S_OK;
}

HRESULT Mp4Joiner::WriteAudio(const int16_t* pcm, uint32_t frames, int64_t t) {
    std::lock_guard lock(p_->mu);
    if (!p_->writer || p_->rate <= 0 || !frames) return E_UNEXPECTED;
    return WriteBytes(p_->writer.Get(), p_->audio, pcm, frames * p_->channels * 2, t, (int64_t)frames * 10'000'000 / p_->rate);
}

HRESULT Mp4Joiner::Finalize() {
    std::lock_guard lock(p_->mu);
    if (!p_->writer) return E_UNEXPECTED;
    const HRESULT hr = p_->frames ? p_->writer->Finalize() : E_FAIL;
    p_->writer.Reset();
    p_->parts.clear();
    p_->sound.Reset();
    p_->soundNext.Reset();
    return hr;
}

HRESULT Mp4Writer::Finalize() {
    std::lock_guard lock(p_->mu);
    if (!p_->writer) return E_UNEXPECTED;
    const HRESULT hr = p_->frames || p_->w <= 0 ? p_->writer->Finalize() : E_FAIL;
    p_->writer.Reset();
    return hr;
}

// ---- GifWriter ----

struct GifWriter::Impl {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    int w = 0, h = 0;
};

GifWriter::GifWriter() : p_(std::make_unique<Impl>()) {}
GifWriter::~GifWriter() = default;

static void SetBytes(IWICMetadataQueryWriter* w, const wchar_t* name, const void* data, ULONG n) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI1 | VT_VECTOR;
    pv.caub.cElems = n;
    pv.caub.pElems = (UCHAR*)data;
    w->SetMetadataByName(name, &pv);
}

static void SetUShort(IWICMetadataQueryWriter* w, const wchar_t* name, USHORT v) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI2;
    pv.uiVal = v;
    w->SetMetadataByName(name, &pv);
}

HRESULT GifWriter::Begin(const std::wstring& path, int w, int h) {
    p_->w = w;
    p_->h = h;
    ComPtr<IWICMetadataQueryWriter> meta;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p_->f));
    if (SUCCEEDED(hr)) hr = p_->f->CreateStream(&p_->stream);
    if (SUCCEEDED(hr)) hr = p_->stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = p_->f->CreateEncoder(GUID_ContainerFormatGif, nullptr, &p_->enc);
    if (SUCCEEDED(hr)) hr = p_->enc->Initialize(p_->stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr) && SUCCEEDED(p_->enc->GetMetadataQueryWriter(&meta))) {
        SetBytes(meta.Get(), L"/appext/Application", "NETSCAPE2.0", 11);
        const UCHAR loop[] = {3, 1, 0, 0, 0};  // loop forever
        SetBytes(meta.Get(), L"/appext/Data", loop, 5);
        SetUShort(meta.Get(), L"/logscrdesc/Width", (USHORT)w);
        SetUShort(meta.Get(), L"/logscrdesc/Height", (USHORT)h);
    }
    return hr;
}

struct GifWriter::Quantized {
    HRESULT hr = E_FAIL;
    int w = 0, h = 0;
    std::vector<WICColor> colors;
    std::vector<uint8_t> pixels;  // palette indices, w per row
};

std::shared_ptr<GifWriter::Quantized> GifWriter::Quantize(const uint32_t* px, int w, int h) {
    auto q = std::make_shared<Quantized>();
    q->w = w;
    q->h = h;
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> src;
    ComPtr<IWICPalette> pal;
    ComPtr<IWICFormatConverter> conv;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, w * h * 4, (BYTE*)px, &src);
    if (SUCCEEDED(hr)) hr = f->CreatePalette(&pal);
    if (SUCCEEDED(hr)) {
        // Build the palette from a downscaled copy for big frames: much faster, visually the same.
        ComPtr<IWICBitmapScaler> scaler;
        const double area = (double)w * h;
        if (area > 250000 && SUCCEEDED(f->CreateBitmapScaler(&scaler))) {
            const double k = std::sqrt(250000 / area);
            scaler->Initialize(src.Get(), std::max(1, (int)(w * k)), std::max(1, (int)(h * k)), WICBitmapInterpolationModeNearestNeighbor);
            hr = pal->InitializeFromBitmap(scaler.Get(), 256, FALSE);
        } else {
            hr = pal->InitializeFromBitmap(src.Get(), 256, FALSE);
        }
    }
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(src.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeNone, pal.Get(), 0, WICBitmapPaletteTypeCustom);
    UINT n = 0;
    if (SUCCEEDED(hr)) hr = pal->GetColorCount(&n);
    if (SUCCEEDED(hr)) {
        q->colors.resize(n);
        hr = pal->GetColors(n, q->colors.data(), &n);
        q->colors.resize(n);
    }
    if (SUCCEEDED(hr)) {
        q->pixels.resize((size_t)w * h);
        hr = conv->CopyPixels(nullptr, (UINT)w, (UINT)(w * h), q->pixels.data());
    }
    q->hr = hr;
    return q;
}

HRESULT GifWriter::AddQuantized(const Quantized& q, int delayCs) {
    if (!p_->enc) return E_UNEXPECTED;
    if (FAILED(q.hr)) return q.hr;
    if (q.w != p_->w || q.h != p_->h) return E_INVALIDARG;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICPalette> pal;
    ComPtr<IWICMetadataQueryWriter> meta;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat8bppIndexed;
    HRESULT hr = p_->enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(q.w, q.h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = p_->f->CreatePalette(&pal);
    if (SUCCEEDED(hr)) hr = pal->InitializeCustom(const_cast<WICColor*>(q.colors.data()), (UINT)q.colors.size());
    if (SUCCEEDED(hr)) hr = frame->SetPalette(pal.Get());
    if (SUCCEEDED(hr) && SUCCEEDED(frame->GetMetadataQueryWriter(&meta)))
        SetUShort(meta.Get(), L"/grctlext/Delay", (USHORT)std::clamp(delayCs, 2, 65535));
    if (SUCCEEDED(hr)) hr = frame->WritePixels((UINT)q.h, (UINT)q.w, (UINT)q.pixels.size(), const_cast<BYTE*>(q.pixels.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    return hr;
}

HRESULT GifWriter::Add(const uint32_t* px, int delayCs) {
    if (!p_->enc) return E_UNEXPECTED;
    return AddQuantized(*Quantize(px, p_->w, p_->h), delayCs);
}
HRESULT GifWriter::Finish() {
    if (!p_->enc) return E_UNEXPECTED;
    const HRESULT hr = p_->enc->Commit();
    p_->enc.Reset();
    p_->stream.Reset();
    return hr;
}

}  // namespace ather
