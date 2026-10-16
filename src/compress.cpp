#include "compress.h"

#include "common.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

#include "json.h"
#include "library.h"
#include "media.h"
#include "output.h"
#include "palette.h"
#include "selftest.h"
#include "toast.h"
#include "videoedit.h"
#include "videoio.h"

using Microsoft::WRL::ComPtr;

namespace ather {

namespace {

double g_videoMB = kCompressDefaultMB, g_pictureMB = kCompressPictureDefaultMB;
std::function<void(bool, double)> g_saveSize;

// Names handed out for results not moved in yet, so two at once never share one.
std::mutex g_reservedMu;
std::set<std::wstring> g_reserved;

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

double FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (double)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

std::wstring TempFile(const wchar_t* ext) {
    static std::atomic<int> n{0};
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"ather-compress-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
           std::to_wstring(++n) + L"." + ext;
}

void Unreserve(const std::wstring& path) {
    std::lock_guard l(g_reservedMu);
    g_reserved.erase(path);
}

bool WriteBytes(const std::wstring& path, const std::vector<BYTE>& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wr, nullptr) && wr == bytes.size();
    CloseHandle(f);
    return ok;
}

// The picture as a baseline JPEG at `quality` (0…1). Empty on failure.
std::vector<BYTE> EncodeJpeg(const Bitmap& img, float quality) {
    std::vector<BYTE> out;
    const UINT w = img.Width(), h = img.Height();
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> src;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    ComPtr<IStream> stream;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr))
        hr = f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, w * h * 4, reinterpret_cast<BYTE*>(img.Bits()), &src);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        hr = props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(src.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) return out;
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    out.resize((size_t)st.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(out.data(), (ULONG)out.size(), &read);
    out.resize(read);
    return out;
}

std::wstring Seconds(double s) {
    wchar_t b[32];
    if (s < 90) swprintf(b, 32, L"%.0f s", s);
    else swprintf(b, 32, L"%d:%02d", (int)s / 60, (int)s % 60);
    return b;
}

bool IsVideo(const std::wstring& path) { return MediaTypeOf(path) == MediaType::Video; }

}  // namespace

std::wstring SizeText(double bytes) {
    wchar_t b[32];
    if (bytes < 1024 * 1024) swprintf(b, 32, L"%.0f KB", bytes / 1024);
    else swprintf(b, 32, L"%.1f MB", bytes / (1024 * 1024));
    return b;
}

std::wstring CompressedPath(const std::wstring& source, const wchar_t* ext) {
    const size_t slash = source.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"" : source.substr(0, slash + 1);
    const std::wstring name = slash == std::wstring::npos ? source : source.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    std::lock_guard l(g_reservedMu);
    for (int i = 1;; ++i) {
        const std::wstring p = dir + stem + L"_small" + (i > 1 ? L"-" + std::to_wstring(i) : L"") + L"." + ext;
        if (!Exists(p) && !g_reserved.count(p)) {
            g_reserved.insert(p);
            return p;
        }
    }
}

// The edit (of `source`) encoded under the cap into r.tmp: the plan for its length, frame and sound, then up to two
// smaller tries when it comes out over. `w` × `h`: the edit's output frame.
static void EncodeUnderCap(const std::wstring& source, const VideoEdit& e, double seconds, int w, int h, bool audio, double capBytes,
                           const CompressProgress& progress, CompressResult& r) {
    r.tmp = TempFile(L"mp4");
    double scale = 1;
    for (int attempt = 0; attempt <= kCompressRetries; ++attempt) {
        const VideoPlan p = PlanCompressedVideo(capBytes, seconds, w, h, audio, scale);
        if (!p.fits) {
            r.error = attempt == 0 ? Seconds(seconds) + L" of video doesn't fit in " + MegabytesLabel(capBytes / (1024 * 1024)) +
                                         L". Trim it first, or pick a larger size."
                                   : L"It still came out at " + SizeText(r.bytesAfter) + L". Trim it first, or pick a larger size.";
            break;
        }
        std::wstring err;
        const Mp4Target target{{p.w, p.h}, (uint32_t)p.videoBps, (uint32_t)p.audioBps};
        DeleteFileW(r.tmp.c_str());
        const bool ok = ExportMp4Sized(source, e, target, r.tmp, &err, [&](double x) {
            if (progress) progress(x, attempt);
            return true;
        });
        ++r.attempts;
        if (!ok) {
            r.error = err.empty() ? L"Encoding failed." : err;
            break;
        }
        r.bytesAfter = FileBytes(r.tmp);
        r.w = p.w, r.h = p.h;
        if (r.bytesAfter > 0 && r.bytesAfter <= capBytes) {
            r.ok = true;
            break;
        }
        if (attempt == kCompressRetries)
            r.error = L"It came out at " + SizeText(r.bytesAfter) + L", over " + MegabytesLabel(capBytes / (1024 * 1024)) +
                      L". Trim it first, or pick a larger size.";
        scale *= RetryScale(capBytes, r.bytesAfter);
    }
    if (!r.ok) {
        DeleteFileW(r.tmp.c_str());
        r.tmp.clear();
    }
}

CompressResult CompressVideo(const std::wstring& path, double capBytes, CompressProgress progress) {
    CompressResult r;
    r.bytesBefore = FileBytes(path);
    const auto clip = ClipOf(path);  // upright size, length, and whether its sound decodes here
    if (!clip || clip->Duration() <= 0) {
        r.error = L"Can't read this video.";
        return r;
    }
    if (r.bytesBefore > 0 && r.bytesBefore <= capBytes) {
        r.ok = r.skipped = true;
        return r;
    }
    VideoEdit e;
    e.clips = {*clip};
    e.trimEnd = clip->Duration();
    EncodeUnderCap(L"", e, clip->Duration(), clip->w, clip->h, clip->hasAudio, capBytes, progress, r);
    if (r.ok) r.out = CompressedPath(path, L"mp4");
    return r;
}

CompressResult CompressEdit(const std::wstring& source, const VideoEdit& e, double capBytes, CompressProgress progress) {
    CompressResult r;
    const Sequence seq = SequenceOf(source, e);
    const double seconds = e.OutputDuration();
    if (seq.clips.empty() || seconds <= 0) {
        r.error = L"Can't read this video.";
        return r;
    }
    const SIZE sz = FrameRenderer(e, seq.size, false).Out();  // after the crop
    EncodeUnderCap(source, e, seconds, sz.cx, sz.cy, !e.muted && seq.HasAudio(), capBytes, progress, r);
    return r;
}

void RememberCompressSize(bool picture, double mb) {
    if (mb <= 0) return;
    (picture ? g_pictureMB : g_videoMB) = mb;
    if (g_saveSize) g_saveSize(picture, mb);
}

CompressResult CompressPicture(const std::wstring& path, double capBytes) {
    CompressResult r;
    r.bytesBefore = FileBytes(path);
    const BitmapPtr img = LoadImageFile(path);  // transparency flattened onto white: JPEG has none
    if (!img) {
        r.error = L"Can't read this picture.";
        return r;
    }
    if (r.bytesBefore > 0 && r.bytesBefore <= capBytes) {
        r.ok = r.skipped = true;
        return r;
    }
    const int w = img->Width(), h = img->Height();
    BitmapPtr scaled = img;
    double scaledBy = 1;
    for (const PictureStep& s : PictureSteps(w, h)) {
        if (s.scale != scaledBy) {
            scaledBy = s.scale;
            scaled = Resample(*img, std::max(1, (int)std::lround(w * s.scale)), std::max(1, (int)std::lround(h * s.scale)));
            if (!scaled) break;
        }
        const std::vector<BYTE> jpeg = EncodeJpeg(*scaled, (float)s.quality);
        ++r.attempts;
        if (jpeg.empty()) break;
        r.bytesAfter = (double)jpeg.size();
        r.w = scaled->Width(), r.h = scaled->Height();
        if (r.bytesAfter <= capBytes) {
            r.tmp = TempFile(L"jpg");
            if (!WriteBytes(r.tmp, jpeg)) {
                DeleteFileW(r.tmp.c_str());
                r.tmp.clear();
                r.error = L"Can't write the picture.";
                return r;
            }
            r.ok = true;
            r.out = CompressedPath(path, L"jpg");
            return r;
        }
    }
    r.error = r.attempts && r.bytesAfter > 0 ? L"The smallest it got was " + SizeText(r.bytesAfter) + L". Pick a larger size."
                                             : L"Can't encode the picture.";
    return r;
}

CompressResult CompressFile(const std::wstring& path, double capBytes, CompressProgress progress) {
    switch (MediaTypeOf(path)) {
        case MediaType::Video: return CompressVideo(path, capBytes, std::move(progress));
        case MediaType::Image: return CompressPicture(path, capBytes);
        default: {
            CompressResult r;
            r.error = L"GIFs can't be compressed yet.";
            return r;
        }
    }
}

bool FinishCompress(CompressResult& r) {
    if (!r.ok || r.skipped || r.tmp.empty() || r.out.empty()) return r.ok;
    const bool moved = MoveFileExW(r.tmp.c_str(), r.out.c_str(), MOVEFILE_COPY_ALLOWED);  // never over an existing file
    if (!moved) {
        DeleteFileW(r.tmp.c_str());
        r.ok = false;
        r.error = L"Can't save next to the original (" + r.out + L").";
    }
    Unreserve(r.out);
    r.tmp.clear();
    return r.ok;
}

void SetCompressSizes(double videoMB, double pictureMB, std::function<void(bool picture, double mb)> save) {
    if (videoMB > 0) g_videoMB = videoMB;
    if (pictureMB > 0) g_pictureMB = pictureMB;
    g_saveSize = std::move(save);
}

std::vector<double> CompressSizesFor(const std::vector<std::wstring>& paths) {
    const bool video = std::any_of(paths.begin(), paths.end(), IsVideo);
    std::vector<double> sizes = {video ? g_videoMB : g_pictureMB};
    if (video) {
        for (double mb : kCompressVideoPresetsMB)
            if (mb != sizes[0]) sizes.push_back(mb);
    } else {
        for (double mb : kCompressPicturePresetsMB)
            if (mb != sizes[0]) sizes.push_back(mb);
    }
    return sizes;
}

void CompressFiles(const std::vector<std::wstring>& paths, double capMB) {
    if (paths.empty() || capMB <= 0) return;
    const bool pictures = std::none_of(paths.begin(), paths.end(), IsVideo);
    RememberCompressSize(pictures, capMB);
    const std::wstring label = MegabytesLabel(capMB);
    const uint64_t toast = ShowToast(L"Compressing to under " + label + L"…", paths.size() == 1 ? FileNameOf(paths[0]) : std::to_wstring(paths.size()) + L" files",
                                     nullptr, nullptr, 3600000);
    std::thread([paths, capMB, label, toast] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Tally {
            std::vector<std::wstring> made;
            double before = 0, after = 0;
            int skipped = 0;
            std::wstring firstError, firstErrorFile;
        };
        auto tally = std::make_shared<Tally>();
        for (size_t i = 0; i < paths.size(); ++i) {
            const std::wstring src = paths[i], name = FileNameOf(src);
            const std::wstring of = paths.size() > 1 ? L" (" + std::to_wstring(i + 1) + L" of " + std::to_wstring(paths.size()) + L")" : L"";
            int lastPct = -1;
            CompressResult r = CompressFile(src, MegabytesToBytes(capMB), [&](double p, int attempt) {
                const int pct = (int)(p * 100);
                if (pct / 5 == lastPct / 5) return;
                lastPct = pct;
                const std::wstring body = name + of + L"  ·  " + std::to_wstring(pct) + L"%" + (attempt ? L", smaller try " + std::to_wstring(attempt + 1) : L"");
                RunOnUi([toast, body] { UpdateToastBody(toast, body); });
            });
            // In place on the UI thread: the gallery learns it's a version of the original before it sees the file.
            RunOnUi([r, src, tally]() mutable {
                if (r.ok && !r.skipped) {
                    Library::Shared().NoteEdit(r.out, src, L"", L"", false);
                    FinishCompress(r);
                }
                if (r.skipped) {
                    ++tally->skipped;
                } else if (r.ok) {
                    tally->made.push_back(r.out);
                    tally->before += r.bytesBefore;
                    tally->after += FileBytes(r.out);
                } else if (tally->firstError.empty()) {
                    tally->firstError = r.error;
                    tally->firstErrorFile = FileNameOf(src);
                }
            });
        }
        CoUninitialize();
        const size_t total = paths.size();
        RunOnUi([tally, total, label] {
            HideToast();
            const auto& t = *tally;
            if (t.made.empty()) {
                if (!t.firstError.empty())
                    return (void)ShowToast(L"Couldn't compress " + t.firstErrorFile, t.firstError, nullptr, nullptr, 8000);
                return (void)ShowToast(total == 1 ? L"Already under " + label : L"All already under " + label, L"Nothing to make smaller.", nullptr,
                                       nullptr, 4000);
            }
            std::wstring body = (t.made.size() == 1 ? FileNameOf(t.made[0]) + L", was " : L"Was ") + SizeText(t.before) + L"  ·  click to show in Explorer";
            if (t.skipped) body += L"\n" + std::to_wstring(t.skipped) + L" already under " + label;
            if (!t.firstError.empty()) body += L"\n" + t.firstErrorFile + L": " + t.firstError;
            const std::vector<std::wstring> made = t.made;
            ShowToast((t.made.size() == 1 ? L"Compressed to " : L"Compressed " + std::to_wstring(t.made.size()) + L" files to ") + SizeText(t.after), body,
                      nullptr, [made] { RevealInExplorer(made); }, 8000);
        });
    }).detach();
}

void ShowCompressPicker(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    for (const auto& p : paths)
        if (MediaTypeOf(p) == MediaType::Gif) return (void)ShowToast(L"GIFs can't be compressed yet", FileNameOf(p), nullptr, nullptr, 3000);
    const std::vector<double> sizes = CompressSizesFor(paths);
    const bool video = std::any_of(paths.begin(), paths.end(), IsVideo);
    std::vector<PaletteItem> items;
    for (size_t i = 0; i < sizes.size(); ++i) {
        const double mb = sizes[i];
        std::wstring hint = mb == 8 && video ? L"Discord" : mb == 25 && video ? L"Email" : L"";
        if (i == 0) hint = mb == (video ? kCompressDefaultMB : kCompressPictureDefaultMB) ? L"Default" : L"Last used";
        items.push_back({(int)i, L"Under " + MegabytesLabel(mb), hint, video ? L"smaller shrink mp4 h264 discord email size" : L"smaller shrink jpeg jpg size",
                         (wchar_t)0xE73F});
    }
    PaletteOptions opt;
    opt.placeholder = L"Compress " + (paths.size() == 1 ? FileNameOf(paths[0]) : std::to_wstring(paths.size()) + L" files") +
                      L" to under…  (or type a size in MB)";
    opt.createLabel = L"Compress to under";
    opt.onCreate = [paths](std::wstring text) {
        const double mb = ParseMegabytes(text);
        if (mb <= 0) return (void)ShowToast(L"Type a size in MB", L"For example 10 or 2.5", nullptr, nullptr, 3000);
        CompressFiles(paths, mb);
    };
    ShowPalette(std::move(items), [paths, sizes](int id) {
        if (id >= 0 && id < (int)sizes.size()) CompressFiles(paths, sizes[(size_t)id]);
    }, opt);
}

// ---- tests ----

ATHER_TEST(compress_plan_follows_the_handoff_math) {
    // 30 MB over 80.2 s with sound, from 2560 wide: ~2.9 Mbps in all, 96 kbps of it sound, 1920 wide.
    VideoPlan p = PlanCompressedVideo(MegabytesToBytes(30), 80.2, 2560, 1440, true);
    CHECK(p.fits);
    CHECK_NEAR(p.videoBps + p.audioBps, 30 * 1024 * 1024 * 0.9 * 8 / 80.2, 1);
    CHECK_NEAR(p.audioBps, 96000, 0);
    CHECK(p.w == 1920 && p.h == 1080);
    // No sound: all of it video. Smaller than 1920: never scaled up.
    p = PlanCompressedVideo(MegabytesToBytes(30), 80.2, 1280, 720, false);
    CHECK_NEAR(p.audioBps, 0, 0);
    CHECK(p.w == 1280 && p.h == 720);
    // Under 500 kbps: 1280 wide; under 250 kbps: 854; under 100 kbps it doesn't fit.
    p = PlanCompressedVideo(MegabytesToBytes(8), 120, 2560, 1440, true);
    CHECK(p.videoBps < 500000 && p.videoBps >= 250000 && p.w == 1280 && p.h == 720);
    p = PlanCompressedVideo(MegabytesToBytes(8), 240, 2560, 1440, true);
    CHECK(p.videoBps < 250000 && p.w == 854 && p.h == 480);
    CHECK(!PlanCompressedVideo(MegabytesToBytes(1), 300, 1920, 1080, true).fits);
    // Portrait phone video: the long side counts, and the sides stay even.
    p = PlanCompressedVideo(MegabytesToBytes(25), 30, 2160, 3840, true);
    CHECK(p.h == 1920 && p.w == 1080);
    p = PlanCompressedVideo(MegabytesToBytes(25), 30, 1001, 777, true);
    CHECK(p.w % 2 == 0 && p.h % 2 == 0);
    // A retry lowers the rate by cap / actual × 0.95.
    CHECK_NEAR(RetryScale(30, 33), 30.0 / 33 * 0.95, 1e-12);
    const VideoPlan again = PlanCompressedVideo(MegabytesToBytes(30), 80.2, 2560, 1440, true, RetryScale(30, 33));
    CHECK_NEAR(again.videoBps, PlanCompressedVideo(MegabytesToBytes(30), 80.2, 2560, 1440, true).videoBps * 30 / 33 * 0.95, 1);
}

ATHER_TEST(compress_sizes_and_names) {
    CHECK_NEAR(ParseMegabytes(L"12"), 12, 0);
    CHECK_NEAR(ParseMegabytes(L"2,5 MB"), 2.5, 1e-12);
    CHECK_NEAR(ParseMegabytes(L"8mb"), 8, 0);
    CHECK_EQ(ParseMegabytes(L"ten"), 0);
    CHECK_EQ(ParseMegabytes(L"12 GB"), 0);
    CHECK_EQ(ParseMegabytes(L"0"), 0);
    CHECK(MegabytesLabel(8) == L"8 MB" && MegabytesLabel(2.5) == L"2.5 MB");
    const std::wstring dir = test::TempDir();
    const std::wstring a = CompressedPath(dir + L"\\clip.mp4", L"mp4");
    CHECK(a == dir + L"\\clip_small.mp4");
    CHECK(CompressedPath(dir + L"\\clip.mp4", L"mp4") == dir + L"\\clip_small-2.mp4");  // the first is reserved meanwhile
    SetCompressSizes(30, 2, nullptr);
    const auto v = CompressSizesFor({dir + L"\\a.mp4", dir + L"\\b.png"});
    CHECK(v.size() == 4 && v[0] == 30 && v[1] == 8);
    const auto pic = CompressSizesFor({dir + L"\\b.png"});
    CHECK(pic.size() == 3 && pic[0] == 2 && pic[1] == 1 && pic[2] == 5);
}

// A moving gradient with grain, and a tone: ~2 Mbps as Mp4Writer makes it, so the rate has to come down.
// `screen`: like a screen recording instead: a still gradient with a white box moving across it.
static bool WriteNoisyClip(const std::wstring& path, int w, int h, int fps, double seconds, bool tone, bool screen = false) {
    constexpr int kRate = 48000;
    constexpr double kTicks = 1e7;
    Mp4Writer mw;
    if (FAILED(mw.Begin(path, w, h, fps, tone ? kRate : 0, 2))) return false;
    std::vector<uint32_t> px((size_t)w * h);
    std::vector<int16_t> pcm;
    uint32_t x = 99;
    int64_t audio = 0;
    const int n = (int)std::lround(seconds * fps);
    for (int i = 0; i < n; ++i) {
        // A gradient that moves every frame, with light grain: busy enough to need the bits, unlike flat colour,
        // and unlike pure noise still something an encoder can squeeze to a low rate (as screen recordings are).
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                const int grain = screen ? 0 : (int)(((x = x * 1664525 + 1013904223) >> 24) % 25) - 12;
                auto c = [grain](int v) { return (uint32_t)std::clamp(v + grain, 0, 255); };
                const int t = screen ? 0 : i;  // the screen's gradient stays put
                px[(size_t)yy * w + xx] = 0xFF000000 | c((xx * 255 / w + t * 4) & 255) << 16 | c(yy * 255 / h) << 8 | c(((xx + yy) / 2 + t * 2) & 255);
            }
        if (screen) {  // the box: a sixth of the width, moving across a third of the way down
            const int bw = w / 6, x0 = (int)((int64_t)i * w / n % (w - bw)), y0 = h / 3;
            for (int yy = y0; yy < std::min(h, y0 + bw); ++yy) std::fill_n(&px[(size_t)yy * w + x0], bw, 0xFFFFFFFFu);
        }
        if (FAILED(mw.WriteFrame(px.data(), std::llround(i * kTicks / fps), std::llround(kTicks / fps)))) return false;
        if (!tone) continue;
        pcm.clear();
        for (const int64_t until = std::llround((i + 1) * (double)kRate / fps); audio < until; ++audio) {
            const int16_t v = (int16_t)std::lround(std::sin(2 * 3.14159265358979 * 440 * audio / kRate) * 12000);
            pcm.push_back(v);
            pcm.push_back(v);
        }
        if (FAILED(mw.WriteAudio(pcm.data(), (uint32_t)(pcm.size() / 2), std::llround((audio - (int64_t)pcm.size() / 2) * kTicks / kRate)))) return false;
    }
    return SUCCEEDED(mw.Finalize());
}

ATHER_TEST(compress_video_lands_under_the_cap) {
    const std::wstring dir = test::TempDir();
    const std::wstring src = dir + L"\\big.mp4";
    CHECK(WriteNoisyClip(src, 640, 360, 30, 4, true));
    const double before = FileBytes(src), cap = 400 * 1024;  // 4 s in 400 KB: ~640 kbps of video
    test::Note("source: " + std::to_string((int)before) + " bytes");
    CHECK(before > cap);
    int calls = 0;
    CompressResult r = CompressVideo(src, cap, [&](double, int) { ++calls; });
    test::Note("error: " + ToUtf8(r.error) + ", " + std::to_string((int)r.bytesAfter) + " bytes in " + std::to_string(r.attempts) + " tries");
    CHECK(r.ok && !r.skipped && calls > 0);
    CHECK(FinishCompress(r));
    CHECK(r.out == dir + L"\\big_small.mp4");
    CHECK(FileBytes(r.out) > cap / 2 && FileBytes(r.out) <= cap);  // under the cap, and not far under it
    CHECK(FileBytes(src) == before);  // the original stays as it was
    VideoInfo vi;
    CHECK(ProbeVideo(r.out, &vi) && vi.w == 640 && vi.h == 360 && vi.hasAudio && std::fabs(vi.duration - 4) < 0.2);
    // Already small enough: nothing made.
    CompressResult again = CompressVideo(r.out, cap);
    CHECK(again.ok && again.skipped && again.tmp.empty());
    // Too little room for the length: refused, with nothing left behind.
    CompressResult tiny = CompressVideo(src, 30 * 1024);
    CHECK(!tiny.ok && !tiny.error.empty() && tiny.tmp.empty() && tiny.attempts == 0);
    // 1920 × 1080 at a cap that leaves it under 500 kbps of video: 1280 wide.
    const std::wstring hd = dir + L"\\hd.mp4";
    CHECK(WriteNoisyClip(hd, 1920, 1080, 30, 4, false, true));  // grain at 720p needs more bits than this asks; screen content doesn't
    // Under 500 kbps of video it's scaled down, keeping its shape. The cap: 200 KB (~370 kbps: 1280 wide), or less
    // when the encoder made the source smaller than twice that.
    const double hdCap = std::min(200.0 * 1024, FileBytes(hd) * 0.5);
    CHECK(PlanCompressedVideo(hdCap, 4, 1920, 1080, false).fits);
    CompressResult s = CompressVideo(hd, hdCap);
    test::Note("error: " + ToUtf8(s.error) + ", source " + std::to_string((int)FileBytes(hd)) + " bytes, cap " + std::to_string((int)hdCap) +
               ", " + std::to_string((int)s.bytesAfter) + " bytes in " + std::to_string(s.attempts) + " tries");
    CHECK(s.ok && !s.skipped && FinishCompress(s));
    CHECK(s.w < 1920 && s.w >= 854 && std::abs(s.w * 9 - s.h * 16) <= 16 && FileBytes(s.out) <= hdCap);
    CHECK(ProbeVideo(s.out, &vi) && vi.w == s.w && vi.h == s.h && !vi.hasAudio);
}

ATHER_TEST(compress_picture_makes_a_smaller_jpeg) {
    const std::wstring dir = test::TempDir();
    const std::wstring src = dir + L"\\shot.png";
    // Noise compresses badly as PNG, so the JPEG has to work for it.
    BitmapPtr img = Bitmap::Create(1600, 1000);
    uint32_t x = 12345;
    for (int i = 0; i < 1600 * 1000; ++i) {
        x = x * 1664525 + 1013904223;
        img->Bits()[i] = 0xFF000000 | (x >> 8);
    }
    CHECK(SavePng(*img, src));
    const double before = FileBytes(src);
    const double cap = 400 * 1024;
    CHECK(before > cap);
    CompressResult r = CompressPicture(src, cap);
    test::Note("error: " + ToUtf8(r.error));
    CHECK(r.ok && FinishCompress(r));
    CHECK(r.out == dir + L"\\shot_small.jpg");
    CHECK(FileBytes(r.out) <= cap && FileBytes(src) == before);  // the original stays as it was
    int w = 0, h = 0;
    CHECK(ImageSize(r.out, &w, &h) && w == r.w && h == r.h && w <= 1600);
}

}  // namespace ather
