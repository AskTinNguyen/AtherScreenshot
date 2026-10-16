#pragma once
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace ather {

// "Compress to under N MB": a smaller copy of a video or picture next to the original (<name>_small.mp4 / .jpg), sized
// to fit a chat app's or email's limit. The original is never changed.
//
// Videos: H.264 + AAC MP4 (plays everywhere, unlike HEVC) at a constant bit rate worked out from the cap and the length:
// 90% of the cap (room for the container and the encoder's drift) over the duration, less 96 kbps of sound. Wider than
// 1920 is scaled down (never up); when the video gets under 500 kbps, 1280; under 250 kbps, 854. When the result still
// comes out over the cap, the bit rate drops by cap / actual × 0.95 and it's encoded again, at most twice.
// Pictures: JPEG, quality 85% and then lower, then smaller, until it fits.
// The math here is shared with the Mac app (macos/Sources/AtherScreenshot/Compress.swift) and has no Windows parts.

constexpr double kCompressDefaultMB = 30;         // videos
constexpr double kCompressPictureDefaultMB = 2;
constexpr double kCompressVideoPresetsMB[] = {8, 25, 50};  // Discord (free), email, Discord Nitro Basic and the like
constexpr double kCompressPicturePresetsMB[] = {1, 2, 5};
constexpr double kCompressMargin = 0.9;        // of the cap, for the container and rate-control drift
constexpr double kCompressAudioBps = 96000;    // AAC
constexpr double kCompressFloorBps = 100000;   // less video than this looks too poor: trim the clip instead
constexpr int kCompressRetries = 2;

inline double MegabytesToBytes(double mb) { return mb * 1024 * 1024; }

struct VideoPlan {
    bool fits = false;  // false: the cap is too small for this length, even at the lowest size
    int w = 0, h = 0;   // the output frame: even, the source's shape, never larger than the source
    double videoBps = 0, audioBps = 0;
};

// The largest side the video keeps at `videoBps` bits a second.
inline int CompressLongSide(double videoBps) { return videoBps >= 500000 ? 1920 : videoBps >= 250000 ? 1280 : 854; }

// `w` × `h`: the source frame as shown (upright). `videoScale`: a retry's factor on the video rate (RetryScale).
inline VideoPlan PlanCompressedVideo(double capBytes, double seconds, int w, int h, bool audio, double videoScale = 1) {
    VideoPlan p;
    if (capBytes <= 0 || seconds <= 0 || w <= 0 || h <= 0) return p;
    const double total = capBytes * kCompressMargin * 8 / seconds;
    p.audioBps = audio ? kCompressAudioBps : 0;
    p.videoBps = (total - p.audioBps) * videoScale;
    p.fits = p.videoBps >= kCompressFloorBps;
    const double scale = std::min(1.0, (double)CompressLongSide(p.videoBps) / std::max(w, h));
    p.w = std::max(2, (int)std::lround(w * scale / 2) * 2);  // H.264 needs even sides
    p.h = std::max(2, (int)std::lround(h * scale / 2) * 2);
    return p;
}

// After an attempt that came out at `actualBytes` over `capBytes`: the factor for the next attempt's video rate.
inline double RetryScale(double capBytes, double actualBytes) { return actualBytes > 0 ? capBytes / actualBytes * 0.95 : 0.5; }

// JPEG attempts for a picture, in order, until one fits: each quality at full size, then each again at ¾, ½ and ⅜
// of the size (`scale`), never below 320 px on the long side.
struct PictureStep {
    double quality = 0.85, scale = 1;
};
inline std::vector<PictureStep> PictureSteps(int w, int h) {
    std::vector<PictureStep> steps;
    for (double scale : {1.0, 0.75, 0.5, 0.375}) {
        if (scale < 1 && std::max(w, h) * scale < 320) break;
        for (double q : {0.85, 0.75, 0.6, 0.45}) steps.push_back({q, scale});
    }
    return steps;
}

// "12", "12 MB", "12mb", "1.5", "0,5" → megabytes; 0 when it isn't a size from 0.1 MB to 4 GB.
inline double ParseMegabytes(std::wstring s) {
    std::replace(s.begin(), s.end(), L',', L'.');
    double mb = 0;
    try {
        size_t used = 0;
        mb = std::stod(s, &used);
        std::wstring rest = s.substr(used);
        rest.erase(std::remove(rest.begin(), rest.end(), L' '), rest.end());
        std::transform(rest.begin(), rest.end(), rest.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
        if (!rest.empty() && rest != L"mb" && rest != L"m") return 0;
    } catch (...) {
        return 0;
    }
    return mb >= 0.1 && mb <= 4096 ? mb : 0;
}

// "8 MB", "1.5 MB", "0.5 MB".
inline std::wstring MegabytesLabel(double mb) {
    wchar_t b[32];
    if (std::fabs(mb - std::round(mb)) < 1e-9) swprintf(b, 32, L"%.0f MB", mb);
    else swprintf(b, 32, L"%g MB", mb);
    return b;
}

// ---- Windows ----

// Where the smaller copy goes: <folder>\<name>_small.<ext>, or _small-2, _small-3… when that's taken.
std::wstring CompressedPath(const std::wstring& source, const wchar_t* ext);

struct CompressResult {
    bool ok = false;
    bool skipped = false;   // already under the cap: nothing made
    std::wstring tmp;       // the new file, in the temp folder until FinishCompress moves it to `out`
    std::wstring out;       // where it goes (CompressedPath, reserved until FinishCompress)
    std::wstring error;
    double bytesBefore = 0, bytesAfter = 0;
    int w = 0, h = 0;       // the new file's frame
    int attempts = 0;       // encodes (videos) or JPEG tries (pictures)
};

// Progress 0…1 of one encode; `attempt` 0, then 1 and 2 for the retries.
using CompressProgress = std::function<void(double progress, int attempt)>;
// Run on a worker thread with COM initialized. They write only temp files; FinishCompress puts the result in place.
CompressResult CompressVideo(const std::wstring& path, double capBytes, CompressProgress progress = {});
CompressResult CompressPicture(const std::wstring& path, double capBytes);
// Either, by the file's type.
CompressResult CompressFile(const std::wstring& path, double capBytes, CompressProgress progress = {});
// Moves the temp file to `out` (never over an existing file). False, with `error` set, when it can't.
bool FinishCompress(CompressResult& r);

// The UI flow (UI thread): each file compressed in turn on a worker thread, with a progress toast, the result in a
// toast (click: show it in Explorer), and the new files stacked with their originals in the gallery.
void CompressFiles(const std::vector<std::wstring>& paths, double capMB);
// A palette of sizes for these files: the last size used for their kind first, then the presets (videos' when any is a
// video, else pictures'). Typing a number offers that size too.
void ShowCompressPicker(const std::vector<std::wstring>& paths);
// The sizes ShowCompressPicker offers, in order.
std::vector<double> CompressSizesFor(const std::vector<std::wstring>& paths);
// The last sizes used (settings.ini [Compress] VideoMB and PictureMB), and where a newly used one is saved.
void SetCompressSizes(double videoMB, double pictureMB, std::function<void(bool picture, double mb)> save);

}  // namespace ather
