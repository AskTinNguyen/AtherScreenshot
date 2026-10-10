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
#include <cstring>
#include <mutex>
#include <optional>

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

void BgraToNv12(const uint32_t* px, int w, int h, uint8_t* out, int frameHeight) {
    const bool hd = (frameHeight > 0 ? frameHeight : h) > 576;
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
            _mm_storel_epi64((__m128i*)(c + x), _mm_packus_epi16(uvw, uvw));
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
    if (SUCCEEDED(hr)) hr = p_->writer->BeginWriting();
    if (FAILED(hr)) p_->writer.Reset();
    return hr;
}

// A media buffer over memory someone else owns (kept alive here), so a frame reaches the encoder uncopied.
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

// ---- JoinMp4 ----

namespace {

struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
};

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
constexpr uint32_t Fcc(const char (&s)[5]) { return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; }

struct BoxRef {
    uint32_t type = 0;
    Span whole, body;
};

// The boxes one after another in `s`; false if they don't add up to it.
bool Boxes(Span s, std::vector<BoxRef>* out) {
    size_t at = 0;
    while (at < s.n) {
        if (s.n - at < 8) return false;
        uint64_t size = Be32(s.p + at);
        size_t hdr = 8;
        if (size == 1) {
            if (s.n - at < 16) return false;
            size = Be64(s.p + at + 8);
            hdr = 16;
        } else if (size == 0) {
            size = s.n - at;
        }
        if (size < hdr || size > s.n - at) return false;
        out->push_back({Be32(s.p + at + 4), {s.p + at, (size_t)size}, {s.p + at + hdr, (size_t)size - hdr}});
        at += (size_t)size;
    }
    return true;
}

std::optional<BoxRef> Child(Span s, uint32_t type) {
    std::vector<BoxRef> all;
    if (!Boxes(s, &all)) return std::nullopt;
    for (const auto& b : all)
        if (b.type == type) return b;
    return std::nullopt;
}

class InFile {
public:
    explicit InFile(const std::wstring& path)
        : h_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~InFile() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    InFile(const InFile&) = delete;
    InFile& operator=(const InFile&) = delete;
    uint64_t Size() const {
        LARGE_INTEGER s{};
        return h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &s) ? (uint64_t)s.QuadPart : 0;
    }
    bool Read(uint64_t at, void* dst, size_t n) const {
        OVERLAPPED o{};
        o.Offset = (DWORD)at;
        o.OffsetHigh = (DWORD)(at >> 32);
        DWORD got = 0;
        return h_ != INVALID_HANDLE_VALUE && ReadFile(h_, dst, (DWORD)n, &got, &o) && got == n;
    }

private:
    HANDLE h_;
};

// One track of an MP4 as Mp4Writer makes them: its boxes that are copied as they are, and every sample's place, size
// and timing.
struct Track {
    Span tkhd, mdhd, hdlr, header, dinf, stsd;  // header: vmhd or smhd
    uint32_t timescale = 0;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> sizes, deltas;
    std::vector<int32_t> shifts;  // composition offsets (ctts), if any
    std::vector<bool> sync;       // empty: every sample is a sync sample
};

// The top-level boxes `ftyp` and `moov` of an MP4 file, read into memory.
bool ReadHead(const InFile& f, std::vector<uint8_t>* ftyp, std::vector<uint8_t>* moov) {
    const uint64_t size = f.Size();
    for (uint64_t at = 0; at + 8 <= size;) {
        uint8_t h[16];
        if (!f.Read(at, h, 8)) return false;
        uint64_t len = Be32(h);
        if (len == 1) {
            if (!f.Read(at + 8, h + 8, 8)) return false;
            len = Be64(h + 8);
        } else if (len == 0) {
            len = size - at;
        }
        if (len < 8 || len > size - at) return false;
        const uint32_t type = Be32(h + 4);
        if (type == Fcc("ftyp") || type == Fcc("moov")) {
            if (len > (64u << 20)) return false;
            auto& dst = type == Fcc("ftyp") ? *ftyp : *moov;
            dst.resize((size_t)len);
            if (!f.Read(at, dst.data(), dst.size())) return false;
        }
        at += len;
    }
    return !ftyp->empty() && !moov->empty();
}

// The track of `moov` whose handler is `handler` ('vide', 'soun'), its samples inside a file of `fileSize` bytes.
bool ReadTrack(const std::vector<uint8_t>& moov, uint32_t handler, uint64_t fileSize, Track* t) {
    std::vector<BoxRef> top, traks;
    if (!Boxes({moov.data(), moov.size()}, &top) || top.size() != 1 || !Boxes(top[0].body, &traks)) return false;
    for (const auto& trak : traks) {
        if (trak.type != Fcc("trak")) continue;
        const auto tkhd = Child(trak.body, Fcc("tkhd"));
        const auto mdia = Child(trak.body, Fcc("mdia"));
        const auto mdhd = mdia ? Child(mdia->body, Fcc("mdhd")) : std::nullopt;
        const auto hdlr = mdia ? Child(mdia->body, Fcc("hdlr")) : std::nullopt;
        const auto minf = mdia ? Child(mdia->body, Fcc("minf")) : std::nullopt;
        if (!tkhd || !mdhd || !hdlr || !minf || hdlr->body.n < 12 || Be32(hdlr->body.p + 8) != handler) continue;
        const auto header = Child(minf->body, handler == Fcc("vide") ? Fcc("vmhd") : Fcc("smhd"));
        const auto dinf = Child(minf->body, Fcc("dinf"));
        const auto stbl = Child(minf->body, Fcc("stbl"));
        if (!header || !dinf || !stbl) return false;
        const auto stsd = Child(stbl->body, Fcc("stsd"));
        const auto stts = Child(stbl->body, Fcc("stts"));
        const auto stsc = Child(stbl->body, Fcc("stsc"));
        const auto stsz = Child(stbl->body, Fcc("stsz"));
        const auto stco = Child(stbl->body, Fcc("stco"));
        const auto co64 = Child(stbl->body, Fcc("co64"));
        const auto stss = Child(stbl->body, Fcc("stss"));
        const auto ctts = Child(stbl->body, Fcc("ctts"));
        if (!stsd || !stts || !stsc || !stsz || (!stco && !co64)) return false;
        t->tkhd = tkhd->whole;
        t->mdhd = mdhd->whole;
        t->hdlr = hdlr->whole;
        t->header = header->whole;
        t->dinf = dinf->whole;
        t->stsd = stsd->whole;
        const Span md = mdhd->body;
        if (md.n < 24) return false;
        t->timescale = Be32(md.p + (md.p[0] == 1 ? 20 : 12));
        // Sample sizes.
        const Span sz = stsz->body;
        if (sz.n < 12) return false;
        const uint32_t fixed = Be32(sz.p + 4), count = Be32(sz.p + 8);
        if (!fixed && sz.n < 12 + (size_t)count * 4) return false;
        t->sizes.resize(count);
        for (uint32_t i = 0; i < count; ++i) t->sizes[i] = fixed ? fixed : Be32(sz.p + 12 + (size_t)i * 4);
        // Durations.
        const Span ts = stts->body;
        if (ts.n < 8 || ts.n < 8 + (size_t)Be32(ts.p + 4) * 8) return false;
        for (uint32_t e = 0, n = Be32(ts.p + 4); e < n; ++e)
            t->deltas.insert(t->deltas.end(), Be32(ts.p + 8 + (size_t)e * 8), Be32(ts.p + 12 + (size_t)e * 8));
        if (t->deltas.size() != count) return false;
        if (ctts) {
            const Span cs = ctts->body;
            if (cs.n < 8 || cs.n < 8 + (size_t)Be32(cs.p + 4) * 8) return false;
            for (uint32_t e = 0, n = Be32(cs.p + 4); e < n; ++e)
                t->shifts.insert(t->shifts.end(), Be32(cs.p + 8 + (size_t)e * 8), (int32_t)Be32(cs.p + 12 + (size_t)e * 8));
            if (t->shifts.size() != count) return false;
        }
        if (stss) {
            const Span ss = stss->body;
            if (ss.n < 8 || ss.n < 8 + (size_t)Be32(ss.p + 4) * 4) return false;
            t->sync.assign(count, false);
            for (uint32_t e = 0, n = Be32(ss.p + 4); e < n; ++e) {
                const uint32_t s = Be32(ss.p + 8 + (size_t)e * 4);
                if (s < 1 || s > count) return false;
                t->sync[s - 1] = true;
            }
        }
        // Where each sample is: chunks (stco/co64) of so many samples each (stsc).
        std::vector<uint64_t> chunks;
        const Span co = co64 ? co64->body : stco->body;
        if (co.n < 8) return false;
        const uint32_t nc = Be32(co.p + 4);
        if (co.n < 8 + (size_t)nc * (co64 ? 8 : 4)) return false;
        for (uint32_t c = 0; c < nc; ++c) chunks.push_back(co64 ? Be64(co.p + 8 + (size_t)c * 8) : Be32(co.p + 8 + (size_t)c * 4));
        const Span sc = stsc->body;
        if (sc.n < 8 || sc.n < 8 + (size_t)Be32(sc.p + 4) * 12) return false;
        const uint32_t runs = Be32(sc.p + 4);
        t->offsets.reserve(count);
        for (uint32_t r = 0; r < runs; ++r) {
            const uint32_t first = Be32(sc.p + 8 + (size_t)r * 12), per = Be32(sc.p + 12 + (size_t)r * 12);
            const uint32_t last = r + 1 < runs ? Be32(sc.p + 8 + (size_t)(r + 1) * 12) : nc + 1;
            if (first < 1 || last < first || last > nc + 1) return false;
            for (uint32_t c = first; c < last; ++c) {
                uint64_t at = chunks[c - 1];
                for (uint32_t k = 0; k < per && t->offsets.size() < count; ++k) {
                    t->offsets.push_back(at);
                    at += t->sizes[t->offsets.size() - 1];
                }
            }
        }
        if (t->offsets.size() != count) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (t->offsets[i] + t->sizes[i] > fileSize) return false;
        return true;
    }
    return false;
}

void Put32(std::vector<uint8_t>& o, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    o.insert(o.end(), b, b + 4);
}
void Put64(std::vector<uint8_t>& o, uint64_t v) {
    Put32(o, (uint32_t)(v >> 32));
    Put32(o, (uint32_t)v);
}
void PutSpan(std::vector<uint8_t>& o, Span s) { o.insert(o.end(), s.p, s.p + s.n); }
void Set32(std::vector<uint8_t>& o, size_t at, uint32_t v) {
    o[at] = (uint8_t)(v >> 24);
    o[at + 1] = (uint8_t)(v >> 16);
    o[at + 2] = (uint8_t)(v >> 8);
    o[at + 3] = (uint8_t)v;
}

// A box around what `body` appends.
template <class F>
void PutBox(std::vector<uint8_t>& o, uint32_t type, F body) {
    const size_t at = o.size();
    Put32(o, 0);
    Put32(o, type);
    body();
    Set32(o, at, (uint32_t)(o.size() - at));
}

// A copy of a whole mvhd, tkhd or mdhd box with its duration set (and a tkhd's track number).
bool PutHeader(std::vector<uint8_t>& o, Span whole, uint64_t duration, uint32_t track = 0) {
    if (whole.n < 12) return false;
    const size_t at = o.size();
    PutSpan(o, whole);
    const uint32_t type = Be32(whole.p + 4);
    const bool v1 = whole.p[8] == 1;
    const size_t body = at + 8;
    const size_t dur = body + (type == Fcc("tkhd") ? (v1 ? 28 : 20) : (v1 ? 24 : 16));  // mvhd and mdhd: after the timescale
    if (dur + (v1 ? 8 : 4) > at + whole.n) return false;
    if (type == Fcc("tkhd")) Set32(o, body + (v1 ? 20 : 12), track);
    if (v1) {
        Set32(o, dur, (uint32_t)(duration >> 32));
        Set32(o, dur + 4, (uint32_t)duration);
    } else {
        if (duration > 0xFFFFFFFFu) return false;
        Set32(o, dur, (uint32_t)duration);
    }
    return true;
}

// Where a joined track's samples come from, and its new chunks.
struct Placed {
    const Track* track = nullptr;
    std::vector<std::pair<const InFile*, size_t>> samples;  // the file and sample index of each
    std::vector<uint32_t> sizes, deltas;
    std::vector<int32_t> shifts;
    std::vector<uint32_t> syncs;           // 1-based
    std::vector<uint64_t> chunkAt;         // offset of each chunk in the joined file
    std::vector<uint32_t> chunkSamples;    // samples in each
};

void PutTables(std::vector<uint8_t>& o, const Placed& p, bool video) {
    PutSpan(o, p.track->stsd);
    PutBox(o, Fcc("stts"), [&] {
        Put32(o, 0);
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t d : p.deltas)
            if (!runs.empty() && runs.back().second == d) ++runs.back().first;
            else runs.push_back({1, d});
        Put32(o, (uint32_t)runs.size());
        for (auto& r : runs) Put32(o, r.first), Put32(o, r.second);
    });
    if (!p.shifts.empty())
        PutBox(o, Fcc("ctts"), [&] {
            Put32(o, 0);
            std::vector<std::pair<uint32_t, int32_t>> runs;
            for (int32_t s : p.shifts)
                if (!runs.empty() && runs.back().second == s) ++runs.back().first;
                else runs.push_back({1, s});
            Put32(o, (uint32_t)runs.size());
            for (auto& r : runs) Put32(o, r.first), Put32(o, (uint32_t)r.second);
        });
    PutBox(o, Fcc("stsc"), [&] {
        Put32(o, 0);
        std::vector<std::pair<uint32_t, uint32_t>> runs;  // first chunk, samples per chunk
        for (size_t c = 0; c < p.chunkSamples.size(); ++c)
            if (runs.empty() || runs.back().second != p.chunkSamples[c]) runs.push_back({(uint32_t)c + 1, p.chunkSamples[c]});
        Put32(o, (uint32_t)runs.size());
        for (auto& r : runs) Put32(o, r.first), Put32(o, r.second), Put32(o, 1);
    });
    PutBox(o, Fcc("stsz"), [&] {
        Put32(o, 0);
        Put32(o, 0);
        Put32(o, (uint32_t)p.sizes.size());
        for (uint32_t s : p.sizes) Put32(o, s);
    });
    const bool wide = !p.chunkAt.empty() && p.chunkAt.back() > 0xFFFFFFFFu;
    PutBox(o, wide ? Fcc("co64") : Fcc("stco"), [&] {
        Put32(o, 0);
        Put32(o, (uint32_t)p.chunkAt.size());
        for (uint64_t at : p.chunkAt)
            if (wide) Put64(o, at);
            else Put32(o, (uint32_t)at);
    });
    if (video && p.syncs.size() < p.sizes.size())
        PutBox(o, Fcc("stss"), [&] {
            Put32(o, 0);
            Put32(o, (uint32_t)p.syncs.size());
            for (uint32_t s : p.syncs) Put32(o, s);
        });
}

bool PutTrak(std::vector<uint8_t>& o, const Placed& p, uint32_t id, uint32_t movieScale, bool video) {
    uint64_t length = 0;  // in the track's timescale
    for (uint32_t d : p.deltas) length += d;
    const uint64_t movieLength = (uint64_t)std::llround((double)length * movieScale / p.track->timescale);
    bool ok = true;
    PutBox(o, Fcc("trak"), [&] {
        ok = ok && PutHeader(o, p.track->tkhd, movieLength, id);
        PutBox(o, Fcc("mdia"), [&] {
            ok = ok && PutHeader(o, p.track->mdhd, length);
            PutSpan(o, p.track->hdlr);
            PutBox(o, Fcc("minf"), [&] {
                PutSpan(o, p.track->header);
                PutSpan(o, p.track->dinf);
                PutBox(o, Fcc("stbl"), [&] { PutTables(o, p, video); });
            });
        });
    });
    return ok;
}

class OutFile {
public:
    explicit OutFile(const std::wstring& path)
        : h_(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~OutFile() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    OutFile(const OutFile&) = delete;
    OutFile& operator=(const OutFile&) = delete;
    bool ok() const { return h_ != INVALID_HANDLE_VALUE && ok_; }
    uint64_t At() const { return at_ + buf_.size(); }
    void Write(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
        if (buf_.size() >= (4u << 20)) Flush();
    }
    void Flush() {
        if (!buf_.empty()) Put(at_, buf_.data(), buf_.size());
        at_ += buf_.size();
        buf_.clear();
    }
    void WriteAt(uint64_t at, const void* p, size_t n) {  // over what is written already
        Flush();
        Put(at, p, n);
    }

private:
    void Put(uint64_t at, const void* p, size_t n) {  // every write says where (so one back doesn't move the next)
        OVERLAPPED o{};
        o.Offset = (DWORD)at;
        o.OffsetHigh = (DWORD)(at >> 32);
        DWORD put = 0;
        if (h_ == INVALID_HANDLE_VALUE || !WriteFile(h_, p, (DWORD)n, &put, &o) || put != n) ok_ = false;
    }

    HANDLE h_;
    std::vector<uint8_t> buf_;
    uint64_t at_ = 0;
    bool ok_ = true;
};

}  // namespace

HRESULT JoinMp4(const std::wstring& path, const std::vector<std::wstring>& parts, const std::vector<int>& frames, int fps, const std::wstring& soundPart) {
    if (parts.empty() || parts.size() != frames.size() || fps <= 0) return E_INVALIDARG;
    std::vector<std::unique_ptr<InFile>> files;
    std::vector<std::vector<uint8_t>> moovs(parts.size() + 1);
    std::vector<Track> video(parts.size());
    std::vector<uint8_t> ftyp, unused;
    for (size_t i = 0; i < parts.size(); ++i) {
        files.push_back(std::make_unique<InFile>(parts[i]));
        std::vector<uint8_t> f;
        if (!ReadHead(*files[i], &f, &moovs[i]) || !ReadTrack(moovs[i], Fcc("vide"), files[i]->Size(), &video[i])) return MF_E_INVALIDMEDIATYPE;
        if (i == 0) ftyp = f;
        const Track& v = video[i];
        // Joinable: the same encoder settings, all frames shown in the order stored, starting on a key frame.
        if (v.stsd.n != video[0].stsd.n || memcmp(v.stsd.p, video[0].stsd.p, v.stsd.n) != 0 || v.timescale != video[0].timescale) return MF_E_INVALIDMEDIATYPE;
        for (int32_t s : v.shifts)
            if (s) return MF_E_INVALIDMEDIATYPE;
        if ((int)v.sizes.size() < frames[i] || frames[i] < 1 || (!v.sync.empty() && !v.sync[0])) return MF_E_INVALIDMEDIATYPE;
    }
    const uint32_t scale = video[0].timescale;
    if (!scale || scale % (uint32_t)fps) return MF_E_INVALIDMEDIATYPE;  // whole ticks a frame
    Track sound;
    std::unique_ptr<InFile> soundFile;
    if (!soundPart.empty()) {
        soundFile = std::make_unique<InFile>(soundPart);
        std::vector<uint8_t> f;
        if (!ReadHead(*soundFile, &f, &moovs.back()) || !ReadTrack(moovs.back(), Fcc("soun"), soundFile->Size(), &sound) || !sound.timescale)
            return MF_E_INVALIDMEDIATYPE;
    }
    // The movie header: the first part's.
    std::vector<BoxRef> top;
    if (!Boxes({moovs[0].data(), moovs[0].size()}, &top) || top.size() != 1) return MF_E_INVALIDMEDIATYPE;
    const auto mvhd = Child(top[0].body, Fcc("mvhd"));
    if (!mvhd || mvhd->body.n < 20) return MF_E_INVALIDMEDIATYPE;
    const uint32_t movieScale = Be32(mvhd->body.p + (mvhd->body.p[0] == 1 ? 20 : 12));
    if (!movieScale) return MF_E_INVALIDMEDIATYPE;

    OutFile out(path);
    out.Write(ftyp.data(), ftyp.size());
    const uint64_t mdatAt = out.At();
    const uint8_t mdatHead[16] = {0, 0, 0, 1, 'm', 'd', 'a', 't'};  // its size filled in at the end
    out.Write(mdatHead, sizeof(mdatHead));
    // A second of video, then the sound up to there, and so on (as the players read them).
    Placed pv, pa;
    pv.track = &video[0];
    pa.track = &sound;
    std::vector<uint8_t> chunk;
    auto copy = [&](const InFile& f, const Track& t, size_t first, size_t count) {  // samples [first, first + count)
        for (size_t i = first; i < first + count;) {
            size_t j = i + 1;  // samples stored one after another: read at once
            uint64_t bytes = t.sizes[i];
            while (j < first + count && t.offsets[j] == t.offsets[j - 1] + t.sizes[j - 1] && bytes < (8u << 20)) bytes += t.sizes[j++];
            chunk.resize((size_t)bytes);
            if (!f.Read(t.offsets[i], chunk.data(), chunk.size())) return false;
            out.Write(chunk.data(), chunk.size());
            i = j;
        }
        return true;
    };
    size_t soundNext = 0;
    uint64_t soundTime = 0;  // in the sound's timescale: where sample `soundNext` starts
    auto copySound = [&](double until) {  // seconds
        const size_t from = soundNext;
        while (soundNext < sound.sizes.size() && (double)soundTime / sound.timescale < until) soundTime += sound.deltas[soundNext++];
        if (soundNext == from) return true;
        pa.chunkAt.push_back(out.At());
        pa.chunkSamples.push_back((uint32_t)(soundNext - from));
        for (size_t i = from; i < soundNext; ++i) {
            pa.sizes.push_back(sound.sizes[i]);
            pa.deltas.push_back(sound.deltas[i]);
            if (!sound.shifts.empty()) pa.shifts.push_back(sound.shifts[i]);
        }
        return copy(*soundFile, sound, from, soundNext - from);
    };
    int64_t made = 0;
    bool ok = true;
    for (size_t s = 0; s < parts.size() && ok; ++s)
        for (int first = 0; first < frames[s] && ok; first += fps) {
            const int count = std::min(fps, frames[s] - first);
            const Track& v = video[s];
            pv.chunkAt.push_back(out.At());
            pv.chunkSamples.push_back((uint32_t)count);
            for (int i = first; i < first + count; ++i) {
                pv.sizes.push_back(v.sizes[i]);
                pv.deltas.push_back(scale / (uint32_t)fps);
                if (v.sync.empty() || v.sync[i]) pv.syncs.push_back((uint32_t)(made + (i - first) + 1));
            }
            ok = copy(*files[s], v, (size_t)first, (size_t)count);
            made += count;
            if (ok && soundFile) ok = copySound((double)made / fps);
        }
    if (ok && soundFile) ok = copySound(1e300);
    // The mdat's size, then the index.
    out.Flush();
    const uint64_t mdatSize = out.At() - mdatAt;
    uint8_t size[8];
    for (int i = 0; i < 8; ++i) size[i] = (uint8_t)(mdatSize >> (56 - 8 * i));
    out.WriteAt(mdatAt + 8, size, 8);
    std::vector<uint8_t> moov;
    PutBox(moov, Fcc("moov"), [&] {
        const uint64_t videoLength = (uint64_t)std::llround((double)made / fps * movieScale);
        uint64_t soundLength = 0;
        for (uint32_t d : pa.deltas) soundLength += d;
        soundLength = sound.timescale ? (uint64_t)std::llround((double)soundLength * movieScale / sound.timescale) : 0;
        const size_t at = moov.size();
        ok = ok && PutHeader(moov, mvhd->whole, std::max(videoLength, soundLength));
        if (ok) Set32(moov, at + mvhd->whole.n - 4, soundFile ? 3 : 2);  // the next track number
        ok = ok && PutTrak(moov, pv, 1, movieScale, true);
        if (soundFile) ok = ok && PutTrak(moov, pa, 2, movieScale, false);
    });
    out.Write(moov.data(), moov.size());
    out.Flush();
    ok = ok && out.ok();
    return ok ? S_OK : E_FAIL;
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
