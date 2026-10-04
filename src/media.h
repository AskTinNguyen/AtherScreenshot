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

}  // namespace ather
