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

// The image turned clockwise by 90, 180 or 270 degrees.
BitmapPtr RotateBitmap(const Bitmap& src, int degrees);
// `src` scaled to fit a w × h frame, centered on black (black bars where the shapes differ).
BitmapPtr FitInto(const Bitmap& src, int w, int h);
// Area-averaging resample (box filter when shrinking, bilinear when growing).
BitmapPtr Resample(const Bitmap& src, int w, int h);
// BGRA → NV12 (w × h luma, then h/2 rows of interleaved chroma; w and h even), exactly as Media Foundation's
// video processor makes it for the encoder: BT.709 above 576 rows and BT.601 up to that, studio range, 8-bit fixed
// point, each chroma sample the rounded mean of its 2 × 2 pixels'. `frameHeight`: the whole frame's, when converting a
// part of it (it picks the matrix).
void BgraToNv12(const uint32_t* px, int w, int h, uint8_t* out, int frameHeight = 0);
// Starts Media Foundation once per process.
void EnsureMediaFoundation();

// H.264 MP4 writer (+ AAC when `audioRate` > 0). Frames are top-down BGRA; times are 100 ns. Thread-safe.
class Mp4Writer {
public:
    Mp4Writer();
    ~Mp4Writer();
    // `rotation` (0, 90, 180, 270) is stored for players to apply, as phones do; the frames stay as given.
    // `nv12`: frames come through WriteNv12 (see BgraToNv12) instead of WriteFrame. `w` ≤ 0: sound only.
    HRESULT Begin(const std::wstring& path, int w, int h, int fps, int audioRate = 0, int audioChannels = 2, int rotation = 0, bool nv12 = false);
    HRESULT WriteFrame(const uint32_t* px, int64_t t, int64_t duration);
    // NV12 frames (BgraToNv12's layout), uncopied: `yuv` stays alive, unchanged, until the encoder is done with it.
    HRESULT WriteNv12(std::shared_ptr<const uint8_t> yuv, int64_t t, int64_t duration);
    // Interleaved 16-bit PCM at the rate and channel count given to Begin.
    HRESULT WriteAudio(const int16_t* pcm, uint32_t frames, int64_t t);
    HRESULT Finalize();  // fails when no frame was written
    int64_t Frames() const;
    bool HardwareEncoder() const;  // the video goes through a GPU's encoder

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Joins MP4s of H.264 video (pieces of one video, encoded at the same settings) frame by frame into one, without
// re-encoding, with the sound of another MP4 copied in alongside. Thread-safe.
// Joins MP4s that Mp4Writer made of pieces of one video (H.264, each starting on a key frame, all with the same
// settings), the first `frames[i]` frames of each at `fps`, and `soundPart`'s sound (an MP4 of AAC only, or none):
// the samples are copied as they are, a second of video and its sound at a time, and the index made anew.
// Fails on anything else.
HRESULT JoinMp4(const std::wstring& path, const std::vector<std::wstring>& parts, const std::vector<int>& frames, int fps,
                const std::wstring& soundPart = {});

// Animated GIF writer: one palette per frame, loops forever. Frames are top-down BGRA (alpha ignored).
class GifWriter {
public:
    GifWriter();
    ~GifWriter();
    HRESULT Begin(const std::wstring& path, int w, int h);
    HRESULT Add(const uint32_t* px, int delayCs);  // delay in 1/100 s
    // Add in two steps: Quantize does the slow part (the frame's palette and 8-bit pixels) on any thread, many
    // at once; AddQuantized writes them, in order. Same file as Add.
    struct Quantized;
    static std::shared_ptr<Quantized> Quantize(const uint32_t* px, int w, int h);
    HRESULT AddQuantized(const Quantized& q, int delayCs);
    HRESULT Finish();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace ather
