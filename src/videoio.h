#pragma once
#include <memory>

#include "common.h"
#include "videoedit.h"

namespace ather {

// Video in and out for the video editor: Media Foundation decoding (Source Reader), export through the frame
// renderer to H.264/AAC MP4 or GIF, a pitch-keeping speed change for the audio, preview playback
// (IMFMediaEngine in frame-server mode) and on-device speech-to-text (SAPI dictation).

// Decodes video frames in order, as opaque top-down BGRA.
class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    bool Open(const std::wstring& path);
    SIZE Size() const;
    double Duration() const;
    double Fps() const;
    bool HasAudio() const;
    bool Seek(double t);  // lands on the key frame before `t`; Read on to reach it
    bool Read(BitmapPtr* frame, double* t);  // false at the end

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Changes the speed of audio without changing its pitch (WSOLA: overlapping windows, each placed where it
// lines up best with the previous one). Interleaved float samples; feed and drain as you go.
class TimeStretch {
public:
    TimeStretch(int channels, int rate, double speed);
    void Push(const float* data, size_t frames);
    void Pull(std::vector<float>& out);    // appends what is ready
    void Finish(std::vector<float>& out);  // appends the rest

private:
    void Run(bool final, std::vector<float>& out);
    int ch_, n_, hop_, delta_;
    double speed_;
    std::vector<float> in_, win_, tail_;
    int64_t base_ = 0;  // absolute frame index of in_[0]
    int64_t k_ = 0, prev_ = 0;
    bool started_ = false;
};

// Progress 0…1; return false to cancel.
using ExportProgress = std::function<bool(double)>;
// Writes the edit as a new MP4 (H.264 + AAC, speed with the pitch kept) or GIF (`fps`, at most 960 px).
bool ExportMp4(const std::wstring& source, const VideoEdit& e, const std::wstring& out, std::wstring* error, ExportProgress progress = {});
bool ExportGif(const std::wstring& source, const VideoEdit& e, const std::wstring& out, std::wstring* error, double fps = 12,
               ExportProgress progress = {});

// `count` frames spread over the video, each at most `maxSide` pixels.
std::vector<BitmapPtr> VideoThumbnails(const std::wstring& path, int count, int maxSide);

// Speech in [from, to) of the video as caption-sized chunks with word times, on this PC. Blocking: call from
// a worker thread with COM initialized.
bool Transcribe(const std::wstring& path, double from, double to, std::vector<Caption>* out, std::wstring* error);

// Playback for the editor preview. Frames come out through NewFrame, so the window can run them through the
// frame renderer before showing them. Events are posted to `notify` as `msg` (wParam = MF_MEDIA_ENGINE_EVENT).
class VideoPlayer {
public:
    static std::unique_ptr<VideoPlayer> Open(const std::wstring& path, HWND notify, UINT msg, std::wstring* error);
    ~VideoPlayer();
    void Play();
    void Pause();
    bool Playing() const;
    void Seek(double t);
    double Now() const;
    void SetRate(double r);
    void SetMuted(bool m);
    BitmapPtr NewFrame(double* t);  // the newest decoded frame, or null when there is no new one

private:
    VideoPlayer();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Test helper: a clip whose color changes every second (red, green, blue…), optionally with a 440 Hz tone, and
// optionally marked as rotated (as phones do).
bool WriteTestClip(const std::wstring& path, int w, int h, int fps, double seconds, bool tone, int rotation = 0);

}  // namespace ather
