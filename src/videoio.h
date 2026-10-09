#pragma once
#include <memory>

#include "common.h"
#include "videoedit.h"

namespace ather {

// Video in and out for the video editor: Media Foundation decoding (Source Reader, on the GPU where it can), export
// through the frame renderer to H.264/AAC MP4 or GIF (frames made on worker threads; long MP4s in two pieces on two
// encoders at once), a pitch-keeping speed change for the audio, preview playback (IMFMediaEngine in frame-server
// mode) and on-device speech-to-text (SAPI dictation).

// A decoded frame that turns into opaque top-down BGRA only when asked: an export skips the frames it doesn't
// use and converts the others on its worker threads. Copies share one conversion; Bgra is thread-safe.
class VideoFrame {
public:
    VideoFrame() = default;
    explicit VideoFrame(BitmapPtr bgra);
    BitmapPtr Bgra() const;  // null when out of memory
    explicit operator bool() const { return s_ != nullptr; }
    bool operator==(const VideoFrame& o) const { return s_ == o.s_; }

    struct State;
    explicit VideoFrame(std::shared_ptr<State> s) : s_(std::move(s)) {}
    State* state() const { return s_.get(); }

private:
    std::shared_ptr<State> s_;
};

// Decodes video frames in order, as opaque top-down BGRA.
class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    // `convert`: false keeps Media Foundation's own color conversion (slow; for tests comparing against it).
    bool Open(const std::wstring& path, bool convert = true);
    SIZE Size() const;
    double Duration() const;
    double Fps() const;
    bool HasAudio() const;
    bool Seek(double t);  // lands on the key frame before `t`; Read on to reach it
    bool Read(BitmapPtr* frame, double* t);  // false at the end
    // Like Read, without converting yet. With `fit`, Bgra() comes fitted into that size (black bars). With `skipTo`:
    // a frame followed by one at or before that time (so not the newest up to it) comes without its picture (Bgra()
    // is null), which saves copying it off the GPU — for callers after the newest frame up to a time.
    bool ReadFrame(VideoFrame* frame, double* t, SIZE fit = {}, double skipTo = -1e300);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The clips to play as one video, each fitted into `size`.
struct Sequence {
    std::vector<Clip> clips;
    SIZE size{};
    double Duration() const { return ClipsDuration(clips); }
    double Fps() const;     // the highest of the clips (≤ 60), 30 when unknown
    bool HasAudio() const;  // any clip with sound
};
// The whole file as a clip; nothing when it can't be decoded here (a frame is read to make sure: a container can
// open fine while its codec isn't installed). Its sound counts only if it can be decoded too.
std::optional<Clip> ClipOf(const std::wstring& path);
// The edit's clips in its frame, or the whole `source` file when the edit has no clips.
Sequence SequenceOf(const std::wstring& source, const VideoEdit& e);

// Reads a sequence like one video: frames in order with timeline times (Fit puts one into the sequence frame).
class SequenceReader {
public:
    SequenceReader();
    ~SequenceReader();
    bool Open(const Sequence& s);
    SIZE Size() const;
    double Duration() const;
    double Fps() const;  // the highest of the clips (≤ 60)
    bool HasAudio() const;
    bool Seek(double t);
    // Sequence-sized frames. ReadFrame doesn't convert them yet (Bgra() does), so frames a caller skips cost
    // little; Read converts each.
    bool ReadFrame(VideoFrame* frame, double* t, double skipTo = -1e300);  // `skipTo` in timeline time, as for VideoReader
    bool Read(BitmapPtr* frame, double* t);
    BitmapPtr Fit(const BitmapPtr& frame) const;  // any frame into the sequence frame

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
    std::vector<float> in_, win_, tail_, mono_;
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

// Developer tool (--bench-export): when set, sees every frame an export encodes, in order, before encoding.
extern std::function<void(int index, const Bitmap& frame)> g_exportTap;
// `--bench-export <outDir> [tap] <clip>...` and `--bench-compare <dirA> <dirB>`: see videobench.cpp.
int VideoBench(const std::vector<std::wstring>& args);

// `count` frames spread over the sequence, each at most `maxSide` pixels. `cancelled` is asked between frames.
std::vector<BitmapPtr> VideoThumbnails(const Sequence& s, int count, int maxSide, const std::function<bool()>& cancelled = {});

// Speech in [from, to) of the video as caption-sized chunks with word times, on this PC. Blocking: call from
// a worker thread with COM initialized.
bool Transcribe(const Sequence& s, double from, double to, std::vector<Caption>* out, std::wstring* error);

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
    // The newest decoded frame, or null when there is no new one. With `frame`, it comes `frame`-sized, with the
    // picture (`content`: its stored size, or the engine's own) fitted in on black — scaled by the engine.
    BitmapPtr NewFrame(double* t, SIZE frame = {}, SIZE content = {});
    bool Failed() const;  // the engine hit an error: it won't play

private:
    VideoPlayer();
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Preview playback of a sequence: one player per file, handing over at each clip's end. Times are timeline
// times; frames come out fitted into the sequence frame.
class SequencePlayer {
public:
    static std::unique_ptr<SequencePlayer> Open(const Sequence& s, HWND notify, UINT msg, std::wstring* error);
    void SetSequence(const Sequence& s);  // pauses; keeps the players of files still in it
    void Play();
    void Pause();
    bool Playing() const;
    void Seek(double t);
    double Now() const;
    void SetRate(double r);
    void SetMuted(bool m);
    BitmapPtr NewFrame(double* t);

private:
    SequencePlayer() = default;
    VideoPlayer* Cur() const;
    void Start(VideoPlayer* p);
    void Advance();  // on to the next clip that can play (past ones whose engine failed), or the end
    Sequence seq_;
    std::vector<double> starts_;
    std::vector<std::pair<std::wstring, std::unique_ptr<VideoPlayer>>> players_;  // by file
    size_t cur_ = 0;
    bool playing_ = false, ended_ = false, muted_ = false;
    double rate_ = 1;
    HWND notify_ = nullptr;
    UINT msg_ = 0;
};

// Test helper: a clip whose color changes every second (red, green, blue…), optionally with a 440 Hz tone, and
// optionally marked as rotated (as phones do).
bool WriteTestClip(const std::wstring& path, int w, int h, int fps, double seconds, bool tone, int rotation = 0);

}  // namespace ather
