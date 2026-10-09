// Developer tool: times the video export on real recordings, and checks that a faster export still makes the
// same pictures.
//
//   AtherScreenshot.exe --bench-export <outDir> [tap] <clip>...
//     Exports each clip in a few typical edits and prints how long each took. With `tap`, also hashes every frame
//     the export encodes and keeps every 30th one (raw BGRA) in <outDir>, for --bench-compare.
//   AtherScreenshot.exe --bench-compare <dirA> <dirB>
//     Compares two tapped runs: identical frames, and the PSNR of the kept frames that differ.
// Reads the clips only; everything it writes goes to <outDir>.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "media.h"
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11")
#include <objbase.h>
#include "selftest.h"
#include "videoio.h"

namespace ather {
extern double g_prof[8];  // PROF-TEMP

namespace {

void Say(const std::string& s) {
    test::Out(s);
    test::FlushOut();
}

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

std::wstring BaseName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    std::wstring n = s == std::wstring::npos ? p : p.substr(s + 1);
    if (const size_t d = n.find_last_of(L'.'); d != std::wstring::npos) n.resize(d);
    return n;
}

struct Scenario {
    const wchar_t* name;
    bool gif;
    VideoEdit edit;
};

Mark M(MarkKind k, double x0, double y0, double x1, double y1, double start, double end, AnimStyle st = AnimStyle::Auto, std::wstring text = L"") {
    Mark m;
    m.kind = k;
    m.a = {x0, y0};
    m.b = {x1, y1};
    m.start = start;
    m.end = end;
    m.style = st;
    m.text = std::move(text);
    m.color = 0;
    return m;
}

// Marks and captions like a typical tutorial edit, placed in a W × H frame from `t0` on.
void AddMarkup(VideoEdit& e, double W, double H, double t0, double len) {
    auto at = [&](double f) { return t0 + len * f; };
    e.marks.push_back(M(MarkKind::Title, 0, 0, W, H, at(0), at(0.12), AnimStyle::Auto, L"Release 1.2"));
    e.marks.back().subtitle = L"What's new";
    e.marks.push_back(M(MarkKind::Box, W * 0.1, H * 0.2, W * 0.4, H * 0.45, at(0.1), at(0.4), AnimStyle::DrawOn));
    e.marks.push_back(M(MarkKind::Arrow, W * 0.7, H * 0.8, W * 0.45, H * 0.5, at(0.15), at(0.45), AnimStyle::DrawOn));
    e.marks.push_back(M(MarkKind::Text, W * 0.55, H * 0.1, W * 0.9, H * 0.2, at(0.2), at(0.6), AnimStyle::Typewriter, L"Click Deploy to ship it"));
    e.marks.push_back(M(MarkKind::Blur, W * 0.6, H * 0.6, W * 0.85, H * 0.75, at(0.05), at(0.95)));
    e.marks.push_back(M(MarkKind::Pixelate, W * 0.05, H * 0.75, W * 0.3, H * 0.92, at(0.3), at(0.6)));
    e.marks.push_back(M(MarkKind::Zoom, W * 0.3, H * 0.3, W * 0.5, H * 0.5, at(0.55), at(0.75)));
    Mark emoji = M(MarkKind::Emoji, W * 0.8, H * 0.3, W * 0.8 + H * 0.1, H * 0.4, at(0.4), at(0.8), AnimStyle::Pop, L"✅");
    emoji.emphasis = Emphasis::Ping;
    e.marks.push_back(emoji);
    Mark bubble = M(MarkKind::Bubble, W * 0.2, H * 0.55, W * 0.45, H * 0.65, at(0.6), at(0.9), AnimStyle::Pop, L"Saved!");
    bubble.emphasis = Emphasis::Pulse;
    e.marks.push_back(bubble);
    const wchar_t* lines[] = {L"Open the project settings", L"Pick the build target", L"Then click deploy",
                              L"Wait for the green check", L"That's it, it's live", L"Thanks for watching"};
    for (int i = 0; i < 6; ++i) {
        Caption c;
        c.start = at(i / 6.0);
        c.end = at((i + 0.9) / 6.0);
        c.text = lines[i];
        e.captions.push_back(c);
    }
}

std::vector<Scenario> Scenarios(const Clip& c) {
    const double D = c.length, W = c.w, H = c.h;
    std::vector<Scenario> out;
    {
        Scenario s{L"plain", false, {}};
        s.edit.trimEnd = std::min(D, 20.0);
        out.push_back(s);
    }
    {
        Scenario s{L"edits", false, {}};
        s.edit.trimStart = std::min(1.0, D / 10);
        s.edit.trimEnd = std::min(D, s.edit.trimStart + 20);
        s.edit.crop = VRect{W * 0.05, H * 0.05, W * 0.9, H * 0.9};
        AddMarkup(s.edit, W, H, s.edit.trimStart, s.edit.trimEnd - s.edit.trimStart);
        out.push_back(s);
    }
    {
        Scenario s{L"speed2", false, {}};
        s.edit.trimEnd = std::min(D, 30.0);
        s.edit.speed = 2;
        out.push_back(s);
    }
    {
        Scenario s{L"gif", true, {}};
        s.edit.trimEnd = std::min(D, 8.0);
        AddMarkup(s.edit, W, H, 0, s.edit.trimEnd);
        out.push_back(s);
    }
    return out;
}

uint64_t Hash(const Bitmap& b) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t* p = reinterpret_cast<const uint64_t*>(b.Bits());
    const size_t n = (size_t)b.Width() * b.Height() / 2;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return v;
    fseek(f, 0, SEEK_END);
    v.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    v.resize(fread(v.data(), 1, v.size(), f));
    fclose(f);
    return v;
}

// ---- experiment: decode options (EXP-TEMP) ----
int DecodeExp(const std::wstring& path, int mode) {
    EnsureMediaFoundation();
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> mgr;
    UINT token = 0;
    Microsoft::WRL::ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 4);
    if (mode >= 2) {
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                          D3D11_SDK_VERSION, &dev, nullptr, nullptr);
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (dev && SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        MFCreateDXGIDeviceManager(&token, &mgr);
        mgr->ResetDevice(dev.Get(), token);
        attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.Get());
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    } else if (mode == 0) {
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    Microsoft::WRL::ComPtr<IMFSourceReader> r;
    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), &r);
    if (FAILED(hr)) return Say("open failed\n"), 1;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    Microsoft::WRL::ComPtr<IMFMediaType> mtp;
    MFCreateMediaType(&mtp);
    mtp->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mtp->SetGUID(MF_MT_SUBTYPE, (mode == 0 || mode == 2) ? MFVideoFormat_RGB32 : MFVideoFormat_NV12);
    hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mtp.Get());
    if (FAILED(hr)) return Say("set type failed\n"), 1;
    const auto start = std::chrono::steady_clock::now();
    int n = 0;
    uint64_t sum = 0;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        Microsoft::WRL::ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!s) continue;
        Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
        s->GetBufferByIndex(0, &buf);
        Microsoft::WRL::ComPtr<IMF2DBuffer> b2;
        BYTE* p = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
            sum += p[0] + p[pitch * 100];
            b2->Unlock2D();
        }
        ++n;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    char line[128];
    sprintf_s(line, "mode %d: %d frames in %.2f s = %.1f fps (%llu)\n", mode, n, secs, n / secs, (unsigned long long)sum);
    Say(line);
    return 0;
}
// ---- experiment: which YUV→RGB conversion the old path used (EXP-TEMP) ----
int ColorExp(const std::wstring& path, const std::wstring& refRaw) {
    EnsureMediaFoundation();
    Microsoft::WRL::ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &r))) return 1;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    Microsoft::WRL::ComPtr<IMFMediaType> native, mtp, cur;
    r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native);
    char line[512];
    sprintf_s(line, "native matrix %u range %u primaries %u transfer %u chroma %u\n", MFGetAttributeUINT32(native.Get(), MF_MT_YUV_MATRIX, 99),
              MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 99), MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_PRIMARIES, 99),
              MFGetAttributeUINT32(native.Get(), MF_MT_TRANSFER_FUNCTION, 99), MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_CHROMA_SITING, 99));
    Say(line);
    MFCreateMediaType(&mtp);
    mtp->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mtp->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mtp.Get()))) return 1;
    r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 cw = 0, ch = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &cw, &ch);
    sprintf_s(line, "nv12 %ux%u stride %u matrix %u range %u\n", cw, ch, MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0),
              MFGetAttributeUINT32(cur.Get(), MF_MT_YUV_MATRIX, 99), MFGetAttributeUINT32(cur.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 99));
    Say(line);
    Microsoft::WRL::ComPtr<IMFSample> s;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s)) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) return 1;
        if (s) break;
    }
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    s->ConvertToContiguousBuffer(&buf);
    Microsoft::WRL::ComPtr<IMF2DBuffer> b2;
    BYTE* p0 = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    bool twoD = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p0, &pitch));
    if (!twoD) {
        buf->Lock(&p0, nullptr, &len);
        pitch = (LONG)cw;
    }
    buf->GetCurrentLength(&len);
    sprintf_s(line, "pitch %ld len %lu (pitch*h*1.5 = %ld) twoD %d\n", pitch, len, (long)(pitch * ch * 3 / 2), twoD);
    Say(line);
    auto ref = ReadAll(refRaw);
    const int W = ((int32_t*)ref.data())[0], H = ((int32_t*)ref.data())[1];
    const uint32_t* rp = (const uint32_t*)(ref.data() + 8);
    const BYTE* Y = p0;
    const BYTE* UV = p0 + (size_t)pitch * (len * 2 / 3 / pitch);
    for (int mat = 0; mat < 2; ++mat)
        for (int full = 0; full < 2; ++full)
            for (int up = 0; up < 3; ++up) {
                double kr = mat ? 0.2126 : 0.299, kb = mat ? 0.0722 : 0.114, kg = 1 - kr - kb;
                double ys = full ? 1.0 : 255.0 / 219, cs = full ? 1.0 : 255.0 / 224, yo = full ? 0 : 16;
                double se = 0;
                int maxe = 0;
                size_t n = 0;
                for (int y = 0; y < H; y += 3)
                    for (int x = 0; x < W; x += 3) {
                        double u, v;
                        auto cu = [&](int cx, int cy) { cx = std::clamp(cx, 0, (int)cw / 2 - 1); cy = std::clamp(cy, 0, (int)ch / 2 - 1); return std::pair<double, double>(UV[(size_t)cy * pitch + cx * 2], UV[(size_t)cy * pitch + cx * 2 + 1]); };
                        if (up == 0) {
                            auto c = cu(x / 2, y / 2);
                            u = c.first, v = c.second;
                        } else {
                            // up 1: left-sited horizontally, centered vertically; up 2: centered both ways
                            const double fx = up == 1 ? x / 2.0 : (x - 0.5) / 2.0, fy = (y - 0.5) / 2.0;
                            const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
                            const double tx = fx - x0, ty = fy - y0;
                            auto a = cu(x0, y0), b = cu(x0 + 1, y0), c = cu(x0, y0 + 1), d = cu(x0 + 1, y0 + 1);
                            u = (a.first * (1 - tx) + b.first * tx) * (1 - ty) + (c.first * (1 - tx) + d.first * tx) * ty;
                            v = (a.second * (1 - tx) + b.second * tx) * (1 - ty) + (c.second * (1 - tx) + d.second * tx) * ty;
                        }
                        const double yy = (Y[(size_t)y * pitch + x] - yo) * ys, uu = (u - 128) * cs, vv = (v - 128) * cs;
                        const double R = yy + 2 * (1 - kr) * vv, B = yy + 2 * (1 - kb) * uu, G = (yy - kr * R - kb * B) / kg;
                        const uint32_t q = rp[(size_t)y * W + x];
                        const double ref3[3] = {(double)((q >> 16) & 255), (double)((q >> 8) & 255), (double)(q & 255)};
                        const double got[3] = {std::clamp(std::round(R), 0.0, 255.0), std::clamp(std::round(G), 0.0, 255.0), std::clamp(std::round(B), 0.0, 255.0)};
                        for (int k = 0; k < 3; ++k, ++n) {
                            const double d = got[k] - ref3[k];
                            se += d * d;
                            maxe = std::max(maxe, (int)std::fabs(d));
                        }
                    }
                sprintf_s(line, "matrix %s %s chroma %d: PSNR %.2f max %d\n", mat ? "709" : "601", full ? "full" : "limited", up,
                          10 * std::log10(255.0 * 255 / (se / n)), maxe);
                Say(line);
            }
    // Integer variants: [shift, y, rv, gu, gv, bu, round]
    struct V { const char* name; int sh, y, rv, gu, gv, bu, rnd; };
    const V vs[] = {{"16bit", 16, 76309, 117489, 13975, 34925, 138438, 32768}, {"8bit-709", 8, 298, 459, 55, 136, 541, 128},
                    {"8bit-709b", 8, 298, 460, 55, 137, 542, 128}, {"8bit-709 trunc", 8, 298, 459, 55, 136, 541, 0},
                    {"10bit", 10, 1192, 1836, 218, 546, 2163, 512}, {"13bit", 13, 9539, 14686, 1747, 4366, 17305, 4096},
                    {"16bit trunc", 16, 76309, 117489, 13975, 34925, 138438, 0}};
    for (const V& v : vs) {
        size_t n = 0, eq = 0;
        int maxe = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 1) {
                const int yy = Y[(size_t)y * pitch + x] - 16, u = UV[(size_t)(y / 2) * pitch + (x & ~1)] - 128, vv = UV[(size_t)(y / 2) * pitch + (x & ~1) + 1] - 128;
                const int c = yy * v.y + v.rnd;
                const int R = std::clamp((c + v.rv * vv) >> v.sh, 0, 255), G = std::clamp((c - v.gu * u - v.gv * vv) >> v.sh, 0, 255),
                          B = std::clamp((c + v.bu * u) >> v.sh, 0, 255);
                const uint32_t q = rp[(size_t)y * W + x];
                const int ref3[3] = {(int)((q >> 16) & 255), (int)((q >> 8) & 255), (int)(q & 255)}, got[3] = {R, G, B};
                for (int k = 0; k < 3; ++k, ++n) {
                    eq += got[k] == ref3[k];
                    maxe = std::max(maxe, std::abs(got[k] - ref3[k]));
                }
            }
        sprintf_s(line, "%-16s exact %.4f%% max %d\n", v.name, eq * 100.0 / n, maxe);
        Say(line);
    }
    if (twoD) b2->Unlock2D();
    else buf->Unlock();
    return 0;
}
int Bench(const std::vector<std::wstring>& args) {
    if (args.size() < 2) return 2;
    const std::wstring dir = args[0];
    CreateDirectoryW(dir.c_str(), nullptr);
    size_t first = 1;
    const bool tap = args[1] == L"tap";
    if (tap) ++first;
    double total = 0;
    for (size_t a = first; a < args.size(); ++a) {
        const auto clip = ClipOf(args[a]);
        if (!clip) {
            Say("can't open " + Narrow(args[a]) + "\n");
            return 1;
        }
        char head[256];
        sprintf_s(head, "%s  %dx%d  %.0f fps  %.1f s%s\n", Narrow(BaseName(args[a])).c_str(), clip->w, clip->h, clip->fps, clip->length,
                  clip->hasAudio ? "  audio" : "");
        Say(head);
        for (const auto& s : Scenarios(*clip)) {
            const std::wstring tag = BaseName(args[a]) + L"_" + s.name;
            const std::wstring out = dir + L"\\" + tag + (s.gif ? L".gif" : L".mp4");
            std::vector<uint64_t> hashes;
            int frames = 0;
            if (tap)
                g_exportTap = [&](int i, const Bitmap& b) {
                    hashes.push_back(Hash(b));
                    if (i % 30 == 0) {
                        std::vector<uint8_t> raw(8 + (size_t)b.Width() * b.Height() * 4);
                        const int32_t wh[2] = {b.Width(), b.Height()};
                        memcpy(raw.data(), wh, 8);
                        memcpy(raw.data() + 8, b.Bits(), raw.size() - 8);
                        WriteAll(dir + L"\\" + tag + L"_f" + std::to_wstring(i) + L".raw", raw.data(), raw.size());
                    }
                };
            std::wstring err;
            std::fill(g_prof, g_prof + 8, 0.0);  // PROF-TEMP
            const auto start = std::chrono::steady_clock::now();
            VideoEdit e = s.edit;
            e.clips = {*clip};
            const bool done = s.gif ? ExportGif(L"", e, out, &err, 12, [&](double) { ++frames; return true; })
                                    : ExportMp4(L"", e, out, &err, [&](double) { ++frames; return true; });
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            g_exportTap = nullptr;
            total += secs;
            char line[256];
            sprintf_s(line, "  %-7s %7.2f s  %5d frames  %7.1f fps%s%s\n", Narrow(s.name).c_str(), secs, frames, frames / std::max(1e-9, secs),
                      done ? "" : "  FAILED: ", done ? "" : Narrow(err).c_str());
            Say(line);
            sprintf_s(line, "          decode %.2f fit %.2f render %.2f write %.2f audio %.2f final %.2f gif %.2f\n", g_prof[0], g_prof[1], g_prof[2], g_prof[3], g_prof[4], g_prof[5], g_prof[6]);  // PROF-TEMP
            Say(line);  // PROF-TEMP
            if (tap) {
                std::string text;
                for (uint64_t h : hashes) text += std::to_string(h) + "\n";
                WriteAll(dir + L"\\" + tag + L".hashes", text.data(), text.size());
            }
        }
    }
    char line[64];
    sprintf_s(line, "total %.2f s\n", total);
    Say(line);
    return 0;
}

int Compare(const std::wstring& a, const std::wstring& b) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((a + L"\\*.hashes").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 2;
    int bad = 0;
    do {
        const std::wstring tag = std::wstring(fd.cFileName).substr(0, wcslen(fd.cFileName) - 7);
        auto ha = ReadAll(a + L"\\" + fd.cFileName), hb = ReadAll(b + L"\\" + fd.cFileName);
        const std::string sa(ha.begin(), ha.end()), sb(hb.begin(), hb.end());
        const auto na = std::count(sa.begin(), sa.end(), '\n'), nb = std::count(sb.begin(), sb.end(), '\n');
        int same = 0;
        for (size_t i = 0, j = 0; i < sa.size() && j < sb.size();) {
            const size_t ei = sa.find('\n', i), ej = sb.find('\n', j);
            same += sa.compare(i, ei - i, sb, j, ej - j) == 0;
            i = ei + 1;
            j = ej + 1;
        }
        double worst = 1e9;
        int maxDiff = 0;
        double off = 0, all = 0;
        for (int i = 0; i < 100000; i += 30) {
            const std::wstring f = L"\\" + tag + L"_f" + std::to_wstring(i) + L".raw";
            auto ra = ReadAll(a + f), rb = ReadAll(b + f);
            if (ra.empty() && rb.empty()) break;
            if (ra.size() != rb.size()) {
                worst = 0;
                continue;
            }
            double se = 0;
            size_t n = 0;
            for (size_t k = 8; k < ra.size(); k += 4)
                for (int c = 0; c < 3; ++c, ++n) {
                    const double d = (double)ra[k + c] - rb[k + c];
                    se += d * d;
                    maxDiff = std::max(maxDiff, (int)std::fabs(d));
                    off += std::fabs(d) > 1;
                    all += 1;
                }
            const double psnr = se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / (se / n));
            worst = std::min(worst, psnr);
        }
        char line[256];
        sprintf_s(line, "%-28s frames %lld/%lld  identical %d  worst kept-frame PSNR %.1f dB, max diff %d, off by >1: %.4f%%\n", Narrow(tag).c_str(), (long long)na, (long long)nb, same,
                  worst == 1e9 ? 99.0 : worst, maxDiff, all ? off * 100 / all : 0.0);
        Say(line);
        bad += na != nb || worst < 40;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return bad;
}

}  // namespace

int VideoBench(const std::vector<std::wstring>& args) {
    int code = 0;
    std::thread([&] {  // like the editor's save: a worker thread in the multithreaded apartment
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!args.empty() && args[0] == L"--bench-compare") code = args.size() == 3 ? Compare(args[1], args[2]) : 2;
        else if (args[0] == L"--bench-decode") code = DecodeExp(args[1], _wtoi(args[2].c_str()));  // EXP-TEMP
        else if (args[0] == L"--bench-color") code = ColorExp(args[1], args[2]);  // EXP-TEMP
        else code = Bench(std::vector<std::wstring>(args.begin() + 1, args.end()));
        CoUninitialize();
    }).join();
    return code;
}

}  // namespace ather
