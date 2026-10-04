#pragma once
#include "common.h"

namespace ather {

// Decoding helpers shared by the gallery indexer, thumbnails and the video editor. All of them are safe to
// call from worker threads that have initialized COM.

// Decodes the first frame scaled so the longer side is at most `maxSide` (0 = full size); transparency is
// flattened onto white. `fullW/fullH` receive the original size.
BitmapPtr LoadImageScaled(const std::wstring& path, int maxSide, int* fullW = nullptr, int* fullH = nullptr);
// Pixel size without decoding.
bool ImageSize(const std::wstring& path, int* w, int* h);
// Total playback time of an animated GIF (frames without a delay count as 0.1 s).
double GifDuration(const std::wstring& path);

struct VideoInfo {
    double duration = 0;  // seconds
    int w = 0, h = 0;
    double fps = 0;
    bool hasAudio = false;
};
// Reads the video's size and length, and optionally the frame at `at` seconds scaled to `maxSide`.
bool ProbeVideo(const std::wstring& path, VideoInfo* info, double at = -1, int maxSide = 0, BitmapPtr* frame = nullptr);

// Area-averaging resample (box filter when shrinking, bilinear when growing).
BitmapPtr Resample(const Bitmap& src, int w, int h);
// Starts Media Foundation once per process.
void EnsureMediaFoundation();

// H.264 MP4 writer (+ AAC when `audioRate` > 0). Frames are top-down BGRA; times are 100 ns. Thread-safe.
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();
    HRESULT Begin(const std::wstring& path, int w, int h, int fps, int audioRate = 0, int audioChannels = 2);
    HRESULT WriteFrame(const uint32_t* px, int64_t t, int64_t duration);
    // Interleaved 16-bit PCM at the rate and channel count given to Begin.
    HRESULT WriteAudio(const int16_t* pcm, uint32_t frames, int64_t t);
    HRESULT Finalize();  // fails when no frame was written
    int64_t Frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Animated GIF writer: one palette per frame, loops forever. Frames are top-down BGRA (alpha ignored).
class GifWriter {
public:
    GifWriter();
    ~GifWriter();
    HRESULT Begin(const std::wstring& path, int w, int h);
    HRESULT Add(const uint32_t* px, int delayCs);  // delay in 1/100 s
    HRESULT Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace ather
