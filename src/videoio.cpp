#include "videoio.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <sapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "json.h"
#include "library.h"
#include "media.h"
#include "selftest.h"

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "propsys")
#pragma comment(lib, "sapi")

using Microsoft::WRL::ComPtr;

namespace ather {

namespace {

constexpr int kRate = 48000;  // export audio: 48 kHz stereo
constexpr double kTicks = 1e7;

bool SetPosition(IMFSourceReader* r, double t) {
    PROPVARIANT pos;
    InitPropVariantFromInt64((LONGLONG)(std::max(0.0, t) * kTicks), &pos);
    const HRESULT hr = r->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    return SUCCEEDED(hr);
}

std::wstring HrText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[32];
    swprintf_s(buf, L" (0x%08X)", (unsigned)hr);
    return std::wstring(what) + buf;
}

}  // namespace

// ---------- VideoReader ----------

struct VideoReader::Impl {
    ComPtr<IMFSourceReader> reader;
    UINT32 w = 0, h = 0, cw = 0, ch = 0;  // shown size (before rotation), decoded size
    UINT32 rotation = 0;                  // clockwise degrees to show it upright (phone videos)
    double duration = 0, fps = 0;
    bool audio = false;
};

VideoReader::VideoReader() : p_(std::make_unique<Impl>()) {}
VideoReader::~VideoReader() = default;
SIZE VideoReader::Size() const {
    return p_->rotation % 180 ? SIZE{(LONG)p_->h, (LONG)p_->w} : SIZE{(LONG)p_->w, (LONG)p_->h};
}
double VideoReader::Duration() const { return p_->duration; }
double VideoReader::Fps() const { return p_->fps; }
bool VideoReader::HasAudio() const { return p_->audio; }

bool VideoReader::Open(const std::wstring& path) {
    EnsureMediaFoundation();
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), attr.Get(), &p_->reader);
    if (FAILED(hr)) return false;
    IMFSourceReader* r = p_->reader.Get();
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) p_->duration = var.uhVal.QuadPart / kTicks;
    PropVariantClear(&var);
    ComPtr<IMFMediaType> native, audio, rgb, cur;
    if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) return false;
    p_->audio = SUCCEEDED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &audio));
    UINT32 num = 0, den = 0;
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &p_->w, &p_->h);
    MFVideoArea area{};
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)) && area.Area.cx > 0) {
        p_->w = (UINT32)area.Area.cx;  // 1920x1088 coded, 1920x1080 shown
        p_->h = (UINT32)area.Area.cy;
    }
    if (SUCCEEDED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) && den) p_->fps = (double)num / den;
    // Turned like the preview player turns it (IMFMediaEngine applies this itself).
    p_->rotation = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_ROTATION, 0) % 360;
    if (p_->rotation % 90) p_->rotation = 0;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    hr = MFCreateMediaType(&rgb);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgb.Get());
    if (SUCCEEDED(hr)) hr = r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &p_->cw, &p_->ch);
    return SUCCEEDED(hr) && p_->w > 0 && p_->h > 0;
}

bool VideoReader::Seek(double t) { return p_->reader && SetPosition(p_->reader.Get(), t); }

bool VideoReader::Read(BitmapPtr* frame, double* t) {
    if (!p_->reader) return false;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        if (FAILED(p_->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> cur;
            if (SUCCEEDED(p_->reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
                MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &p_->cw, &p_->ch);
        }
        if (!s) {
            if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return false;
            continue;
        }
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(s->ConvertToContiguousBuffer(&buf))) return false;
        auto out = Bitmap::Create((int)p_->w, (int)p_->h);
        if (!out) return false;
        ComPtr<IMF2DBuffer> b2;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* data = nullptr;
        DWORD len = 0;
        const bool twoD = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
        if (!twoD) {
            if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
            scan0 = data;
            pitch = (LONG)p_->cw * 4;
        }
        const UINT32 rows = std::min(p_->h, p_->ch), cols = std::min(p_->w, p_->cw);
        for (UINT32 y = 0; y < rows; ++y) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(scan0 + (LONG_PTR)pitch * (LONG)y);
            uint32_t* dst = out->Bits() + (size_t)y * p_->w;
            for (UINT32 x = 0; x < cols; ++x) dst[x] = row[x] | 0xFF000000u;
        }
        if (twoD) b2->Unlock2D();
        else buf->Unlock();
        if (p_->rotation && !(out = RotateBitmap(*out, (int)p_->rotation))) return false;
        *frame = out;
        *t = ts / kTicks;
        return true;
    }
}

// ---------- sequences ----------

bool AudioDecodes(const std::wstring& path);

std::optional<Clip> ClipOf(const std::wstring& path) {
    VideoInfo vi;
    if (!ProbeVideo(path, &vi) || vi.w <= 0 || vi.h <= 0 || vi.duration <= 0) return std::nullopt;
    {
        VideoReader r;
        BitmapPtr f;
        double t = 0;
        if (!r.Open(path) || !r.Read(&f, &t)) return std::nullopt;
    }
    Clip c;
    c.source = c.id;  // pieces split from it share this
    c.path = path;
    c.out = c.length = vi.duration;
    c.w = vi.w;
    c.h = vi.h;
    c.fps = vi.fps;
    c.hasAudio = vi.hasAudio && AudioDecodes(path);
    return c;
}

Sequence SequenceOf(const std::wstring& source, const VideoEdit& e) {
    Sequence s;
    s.clips = e.clips;
    if (s.clips.empty())
        if (auto c = ClipOf(source)) s.clips.push_back(*c);
    if (e.frameW > 0 && e.frameH > 0) s.size = {e.frameW, e.frameH};
    else if (!s.clips.empty()) s.size = {s.clips[0].w, s.clips[0].h};
    return s;
}

struct SequenceReader::Impl {
    Sequence seq;
    std::vector<double> starts;
    size_t cur = 0;
    std::unique_ptr<VideoReader> reader;
    BitmapPtr pre;  // the last frame before the clip's in point: shown until the first frame inside it
    std::deque<std::pair<BitmapPtr, double>> queue;

    void Enter(size_t i, double src) {
        cur = i;
        pre = nullptr;
        reader = std::make_unique<VideoReader>();
        if (!reader->Open(seq.clips[i].path)) reader.reset();  // unreadable: the clip is skipped
        else reader->Seek(src);
    }
};

SequenceReader::SequenceReader() : p_(std::make_unique<Impl>()) {}
SequenceReader::~SequenceReader() = default;
SIZE SequenceReader::Size() const { return p_->seq.size; }
double SequenceReader::Duration() const { return p_->seq.Duration(); }

double Sequence::Fps() const {
    double f = 0;
    for (const auto& c : clips) f = std::max(f, c.fps);
    return f > 1 ? std::min(f, 60.0) : 30;
}

bool Sequence::HasAudio() const {
    for (const auto& c : clips)
        if (c.hasAudio) return true;
    return false;
}

double SequenceReader::Fps() const { return p_->seq.Fps(); }

BitmapPtr SequenceReader::Fit(const BitmapPtr& f) const {
    const SIZE sz = p_->seq.size;
    if (!f || (f->Width() == sz.cx && f->Height() == sz.cy)) return f;
    return FitInto(*f, sz.cx, sz.cy);
}
bool SequenceReader::HasAudio() const { return p_->seq.HasAudio(); }

bool SequenceReader::Open(const Sequence& s) {
    if (s.clips.empty() || s.size.cx <= 0 || s.size.cy <= 0) return false;
    p_->seq = s;
    p_->starts.clear();
    for (size_t i = 0; i < s.clips.size(); ++i) p_->starts.push_back(ClipStart(s.clips, i));
    p_->queue.clear();
    p_->Enter(0, s.clips[0].in);
    return p_->reader != nullptr;
}

bool SequenceReader::Seek(double t) {
    const auto spot = LocateClip(p_->seq.clips, t);
    if (!spot) return false;
    p_->queue.clear();
    if (p_->reader && spot->first == p_->cur) {  // same file still open: just seek it
        p_->pre = nullptr;
        return p_->reader->Seek(spot->second);
    }
    p_->Enter(spot->first, spot->second);
    return p_->reader != nullptr;
}

bool SequenceReader::Read(BitmapPtr* frame, double* t) {
    Impl& p = *p_;
    const size_t n = p.seq.clips.size();
    for (;;) {
        if (!p.queue.empty()) {
            *frame = p.queue.front().first;
            *t = p.queue.front().second;
            p.queue.pop_front();
            return *frame != nullptr;
        }
        if (p.cur >= n) return false;
        const Clip& c = p.seq.clips[p.cur];
        BitmapPtr f;
        double ts = 0;
        if (!p.reader || !p.reader->Read(&f, &ts) || ts >= c.out - 1e-4) {  // this clip is done
            if (p.pre) p.queue.push_back({p.pre, p.starts[p.cur]});
            p.pre = nullptr;
            if (p.cur + 1 < n) p.Enter(p.cur + 1, p.seq.clips[p.cur + 1].in);
            else p.cur = n, p.reader.reset();
            continue;
        }
        if (ts < c.in - 1e-4) {
            p.pre = f;
            continue;
        }
        if (p.pre && ts > c.in + 1e-3) p.queue.push_back({p.pre, p.starts[p.cur]});
        p.pre = nullptr;
        p.queue.push_back({f, p.starts[p.cur] + (ts - c.in)});
    }
}

// ---------- audio in ----------

namespace {

// Decodes the first audio stream to interleaved float at its own rate and channel count.
class AudioReader {
public:
    bool Open(const std::wstring& path) {
        EnsureMediaFoundation();
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader_))) return false;
        reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) return false;
        ComPtr<IMFMediaType> f, cur;
        HRESULT hr = MFCreateMediaType(&f);
        if (SUCCEEDED(hr)) hr = f->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = f->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(hr)) hr = reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, f.Get());
        if (SUCCEEDED(hr)) hr = reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
        if (FAILED(hr)) return false;
        rate_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        ch_ = (int)MFGetAttributeUINT32(cur.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
        return rate_ > 0 && ch_ > 0;
    }
    int Rate() const { return rate_; }
    int Channels() const { return ch_; }
    bool Seek(double t) { return SetPosition(reader_.Get(), t); }
    // One decoded buffer and its start time; false at the end.
    bool Read(std::vector<float>& out, double* t) {
        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s))) return false;
            if (!s) {
                if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return false;
                continue;
            }
            ComPtr<IMFMediaBuffer> buf;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (FAILED(s->ConvertToContiguousBuffer(&buf)) || FAILED(buf->Lock(&data, nullptr, &len))) return false;
            out.assign(reinterpret_cast<const float*>(data), reinterpret_cast<const float*>(data) + len / sizeof(float));
            buf->Unlock();
            *t = ts / kTicks;
            return true;
        }
    }

private:
    ComPtr<IMFSourceReader> reader_;
    int rate_ = 0, ch_ = 0;
};

// Any rate and channel count → `outCh` channels at `outRate` (linear interpolation; enough for speech and UI sounds).
class Resampler {
public:
    Resampler(int inRate, int inCh, int outRate, int outCh) : step_((double)inRate / outRate), inCh_(inCh), outCh_(outCh) {}
    void Process(const float* in, size_t frames, std::vector<float>& out) {
        for (size_t i = 0; i < frames; ++i) {
            const float* f = in + i * inCh_;
            for (int c = 0; c < outCh_; ++c) {
                float v;
                if (outCh_ == 1) {
                    v = 0;
                    for (int k = 0; k < inCh_; ++k) v += f[k];
                    v /= inCh_;
                } else {
                    v = f[std::min(c, inCh_ - 1)];
                }
                buf_.push_back(v);
            }
        }
        const size_t n = buf_.size() / outCh_;
        while (pos_ + 1 < (double)n) {
            const size_t i = (size_t)pos_;
            const double k = pos_ - i;
            for (int c = 0; c < outCh_; ++c) out.push_back((float)(buf_[i * outCh_ + c] * (1 - k) + buf_[(i + 1) * outCh_ + c] * k));
            pos_ += step_;
        }
        const size_t drop = std::min((size_t)pos_, n);
        buf_.erase(buf_.begin(), buf_.begin() + drop * outCh_);
        pos_ -= drop;
    }

private:
    double step_, pos_ = 0;
    int inCh_, outCh_;
    std::vector<float> buf_;
};

// A timeline range of a sequence's sound at one rate and channel count: each clip's audio in turn, and silence
// for clips without sound (or where a file's audio runs short), so it stays in step with the pictures.
class SequenceAudio {
public:
    SequenceAudio(const Sequence& s, double from, double to, int rate, int channels) : seq_(s), rate_(rate), ch_(channels) {
        double at = 0;
        for (size_t i = 0; i < s.clips.size(); ++i) {
            const Clip& c = s.clips[i];
            const double a = std::max(from, at), b = std::min(to, at + c.Duration());
            if (b > a) {
                const int64_t f0 = std::llround((a - from) * rate), f1 = std::llround((b - from) * rate);
                segs_.push_back({i, c.in + (a - at), c.in + (b - at), f1 - f0});
            }
            at += c.Duration();
        }
    }
    bool AnyAudio() const {
        for (const auto& c : seq_.clips)
            if (c.hasAudio) return true;
        return false;
    }
    // Appends the next piece; false once everything is out.
    bool Read(std::vector<float>& out) {
        while (seg_ < segs_.size()) {
            const Seg& sg = segs_[seg_];
            if (!started_) {
                started_ = true;
                made_ = 0;
                reader_.reset();
                const Clip& c = seq_.clips[sg.clip];
                if (c.hasAudio) {
                    reader_ = std::make_unique<AudioReader>();
                    if (reader_->Open(c.path)) {
                        reader_->Seek(sg.from);  // when it can't seek, it reads from the start and skips to `from`
                        rs_ = std::make_unique<Resampler>(reader_->Rate(), reader_->Channels(), rate_, ch_);
                    }
                    else reader_.reset();
                }
            }
            const int64_t left = sg.frames - made_;
            if (left <= 0) {
                ++seg_;
                started_ = false;
                continue;
            }
            if (reader_) {
                std::vector<float> in, conv;
                double t = 0;
                if (reader_->Read(in, &t) && t < sg.to) {
                    const int ch = reader_->Channels(), rate = reader_->Rate();
                    const int64_t frames = (int64_t)in.size() / ch;
                    const int64_t skip = std::clamp<int64_t>(std::llround((sg.from - t) * rate), 0, frames);
                    const int64_t end = std::clamp<int64_t>(std::llround((sg.to - t) * rate), 0, frames);
                    if (end > skip) rs_->Process(in.data() + skip * ch, (size_t)(end - skip), conv);
                    const int64_t take = std::min<int64_t>((int64_t)conv.size() / ch_, left);
                    out.insert(out.end(), conv.begin(), conv.begin() + take * ch_);
                    made_ += take;
                    if (take > 0) return true;
                    continue;
                }
                reader_.reset();  // the file's sound ended early: silence for the rest of the clip
            }
            const int64_t n = std::min<int64_t>(left, 4096);
            out.insert(out.end(), (size_t)(n * ch_), 0.f);
            made_ += n;
            return true;
        }
        return false;
    }

private:
    struct Seg {
        size_t clip;
        double from, to;  // source seconds
        int64_t frames;   // exactly this many frames come out for it
    };
    Sequence seq_;
    int rate_, ch_;
    std::vector<Seg> segs_;
    size_t seg_ = 0;
    bool started_ = false;
    int64_t made_ = 0;
    std::unique_ptr<AudioReader> reader_;
    std::unique_ptr<Resampler> rs_;
};

// The edit's audio: the trimmed range, as 48 kHz stereo, at the edit's speed.
class AudioPipe {
public:
    bool Open(const Sequence& s, double from, double to, double speed) {
        audio_ = std::make_unique<SequenceAudio>(s, from, to, kRate, 2);
        if (!audio_->AnyAudio()) return false;
        if (std::fabs(speed - 1) > 1e-6) ts_ = std::make_unique<TimeStretch>(2, kRate, speed);
        return true;
    }

    // Writes audio up to `target` output frames, padding with silence when the source runs out.
    HRESULT WriteUntil(Mp4Writer& w, int64_t target) {
        while ((int64_t)ready_.size() / 2 < target - written_ && !eof_) Fill();
        std::vector<int16_t> pcm;
        while (written_ < target) {
            const int64_t n = std::min<int64_t>(1024, target - written_);
            pcm.assign((size_t)n * 2, 0);
            const size_t have = std::min(ready_.size(), (size_t)n * 2);
            for (size_t i = 0; i < have; ++i) pcm[i] = (int16_t)std::lround(std::clamp(ready_[i], -1.f, 1.f) * 32767);
            ready_.erase(ready_.begin(), ready_.begin() + have);
            const HRESULT hr = w.WriteAudio(pcm.data(), (uint32_t)n, (int64_t)std::llround(written_ * kTicks / kRate));
            if (FAILED(hr)) return hr;
            written_ += n;
        }
        return S_OK;
    }

private:
    void Fill() {
        std::vector<float> chunk;
        if (!audio_->Read(chunk)) {
            eof_ = true;
            if (ts_) ts_->Finish(ready_);
            return;
        }
        if (ts_) {
            ts_->Push(chunk.data(), chunk.size() / 2);
            ts_->Pull(ready_);
        } else {
            ready_.insert(ready_.end(), chunk.begin(), chunk.end());
        }
    }

    std::unique_ptr<SequenceAudio> audio_;
    std::unique_ptr<TimeStretch> ts_;
    std::vector<float> ready_;
    int64_t written_ = 0;
    bool eof_ = false;
};

}  // namespace

bool AudioDecodes(const std::wstring& path) {
    AudioReader r;
    return r.Open(path);
}

// ---------- TimeStretch ----------

TimeStretch::TimeStretch(int channels, int rate, double speed) : ch_(std::max(1, channels)), speed_(speed) {
    n_ = std::max(64, (int)(rate * 0.04) & ~1);  // 40 ms windows, half overlapping
    hop_ = n_ / 2;
    delta_ = std::max(8, (int)(rate * 0.012));  // ±12 ms search
    win_.resize(n_);
    for (int i = 0; i < n_; ++i) win_[i] = 0.5f - 0.5f * (float)std::cos(2 * 3.14159265358979 * i / n_);  // sums to 1 at half overlap
    tail_.assign((size_t)hop_ * ch_, 0.f);
}

void TimeStretch::Push(const float* data, size_t frames) { in_.insert(in_.end(), data, data + frames * ch_); }
void TimeStretch::Pull(std::vector<float>& out) { Run(false, out); }

void TimeStretch::Finish(std::vector<float>& out) {
    Run(true, out);
    out.insert(out.end(), tail_.begin(), tail_.end());
    std::fill(tail_.begin(), tail_.end(), 0.f);
    in_.clear();
}

void TimeStretch::Run(bool final, std::vector<float>& out) {
    auto at = [&](int64_t i, int c) -> float {
        const int64_t k = i - base_;
        return k < 0 || k >= (int64_t)(in_.size() / ch_) ? 0.f : in_[(size_t)k * ch_ + c];
    };
    auto mono = [&](int64_t i) {
        float v = 0;
        for (int c = 0; c < ch_; ++c) v += at(i, c);
        return v;
    };
    for (;;) {
        const int64_t avail = base_ + (int64_t)(in_.size() / ch_);
        const int64_t nominal = std::llround(k_ * hop_ * speed_);
        if (final && nominal >= avail) break;
        int64_t pos = 0;
        if (!started_) {
            if (!final && avail < n_) break;
        } else {
            const int64_t lo = std::max(base_, nominal - delta_), hi = nominal + delta_, target = prev_ + hop_;
            if (!final && (hi + n_ > avail || target + hop_ > avail)) break;
            // The window that best continues what the previous one left off (step 2 keeps this cheap).
            double best = -1e300;
            pos = std::max(lo, nominal);
            for (int64_t p = lo; p <= hi; p += 2) {
                double num = 0, den = 1e-9;
                for (int j = 0; j < hop_; j += 2) {
                    const float a = mono(target + j), b = mono(p + j);
                    num += (double)a * b;
                    den += (double)b * b;
                }
                const double score = num / std::sqrt(den);
                if (score > best) {
                    best = score;
                    pos = p;
                }
            }
        }
        for (int j = 0; j < n_; ++j)
            for (int c = 0; c < ch_; ++c) {
                const float s = at(pos + j, c) * win_[j];
                if (j < hop_) out.push_back(tail_[(size_t)j * ch_ + c] + s);
                else tail_[(size_t)(j - hop_) * ch_ + c] = s;
            }
        prev_ = pos;
        started_ = true;
        ++k_;
        const int64_t keep = std::min(std::llround(k_ * hop_ * speed_) - delta_, prev_ + hop_);
        if (keep > base_) {
            const size_t drop = (size_t)std::min<int64_t>(keep - base_, (int64_t)(in_.size() / ch_));
            in_.erase(in_.begin(), in_.begin() + drop * ch_);
            base_ += (int64_t)drop;
        }
    }
}

// ---------- export ----------

namespace {

// The edited video's frames in output order: output time i/fps shows the source frame at
// trimStart + i/fps × speed, run through the frame renderer.
class EditFrames {
public:
    bool Open(const Sequence& seq, const VideoEdit& e, double fps, std::wstring* error) {
        if (!reader_.Open(seq)) {
            if (error) *error = L"Can't read this video.";
            return false;
        }
        e_ = e;
        fps_ = fps;
        renderer_ = std::make_unique<FrameRenderer>(e, reader_.Size(), false);
        reader_.Seek(e.trimStart);
        Advance();
        return true;
    }
    SIZE Out() const { return renderer_->Out(); }
    double SourceFps() const { return reader_.Fps(); }
    bool HasAudio() const { return reader_.HasAudio(); }
    int Count(bool roundUp) const {
        const double n = e_.OutputDuration() * fps_;
        return std::max(1, roundUp ? (int)std::ceil(n - 1e-6) : (int)std::floor(n + 1e-6));
    }

    BitmapPtr Frame(int i) {
        const double st = e_.trimStart + i / fps_ * e_.speed;
        while (next_ && nextT_ <= st + 1e-3) {
            cur_ = next_;
            curT_ = nextT_;
            Advance();
        }
        const BitmapPtr& src = cur_ ? cur_ : next_;  // before the first frame (a seek that landed late): the first one
        if (!src) return nullptr;
        if (src != fitFor_) {  // fitted once per source frame, and only for frames that are used
            fitFor_ = src;
            fit_ = reader_.Fit(src);
        }
        return fit_ ? renderer_->Render(*fit_, st) : nullptr;
    }

private:
    void Advance() {
        BitmapPtr f;
        double t = 0;
        if (reader_.Read(&f, &t)) {
            next_ = f;
            nextT_ = t;
        } else {
            next_ = nullptr;
        }
    }

    SequenceReader reader_;
    BitmapPtr fitFor_, fit_;
    VideoEdit e_;
    double fps_ = 30;
    std::unique_ptr<FrameRenderer> renderer_;
    BitmapPtr cur_, next_;
    double curT_ = 0, nextT_ = 0;
};

}  // namespace

bool ExportMp4(const std::wstring& source, const VideoEdit& e, const std::wstring& out, std::wstring* error, ExportProgress progress) {
    const Sequence seq = SequenceOf(source, e);
    const int fps = (int)std::lround(seq.Fps());
    EditFrames frames;
    if (!frames.Open(seq, e, fps, error)) return false;  // "Can't read this video."
    const SIZE sz = frames.Out();
    std::unique_ptr<AudioPipe> audio;
    if (!e.muted && seq.HasAudio()) {
        audio = std::make_unique<AudioPipe>();
        if (!audio->Open(seq, e.trimStart, e.trimEnd, e.speed)) audio.reset();
    }
    Mp4Writer w;
    HRESULT hr = w.Begin(out, sz.cx, sz.cy, fps, audio ? kRate : 0, 2);
    if (FAILED(hr)) {
        if (error) *error = HrText(L"Can't start the MP4 encoder", hr);
        return false;
    }
    const int n = frames.Count(true);
    const int64_t audioTotal = std::llround(e.OutputDuration() * kRate);
    for (int i = 0; i < n; ++i) {
        BitmapPtr f = frames.Frame(i);
        if (!f) break;
        hr = w.WriteFrame(f->Bits(), std::llround(i * kTicks / fps), std::llround(kTicks / fps));
        if (SUCCEEDED(hr) && audio) hr = audio->WriteUntil(w, std::min(audioTotal, std::llround((i + 1) * (double)kRate / fps)));
        if (FAILED(hr)) {
            if (error) *error = HrText(L"Encoding failed", hr);
            w.Finalize();
            return false;
        }
        if (progress && !progress((i + 1.0) / n)) {
            w.Finalize();
            if (error) *error = L"Cancelled.";
            return false;
        }
    }
    if (audio) audio->WriteUntil(w, audioTotal);
    hr = w.Finalize();
    if (FAILED(hr) && error) *error = HrText(L"Couldn't finish the MP4", hr);
    return SUCCEEDED(hr);
}

bool ExportGif(const std::wstring& source, const VideoEdit& e, const std::wstring& out, std::wstring* error, double fps, ExportProgress progress) {
    EditFrames frames;
    if (!frames.Open(SequenceOf(source, e), e, fps, error)) return false;
    const SIZE sz = frames.Out();
    const double k = std::min({1.0, 960.0 / sz.cx, 960.0 / sz.cy});
    const int gw = std::max(1, (int)std::lround(sz.cx * k)), gh = std::max(1, (int)std::lround(sz.cy * k));
    GifWriter g;
    HRESULT hr = g.Begin(out, gw, gh);
    const int n = frames.Count(false);
    for (int i = 0; i < n && SUCCEEDED(hr); ++i) {
        BitmapPtr f = frames.Frame(i);
        if (!f) break;
        if (f->Width() != gw || f->Height() != gh) f = Resample(*f, gw, gh);
        const int delay = (int)std::lround((i + 1) * 100 / fps) - (int)std::lround(i * 100 / fps);  // 1/100 s, drift-free
        hr = f ? g.Add(f->Bits(), delay) : E_OUTOFMEMORY;
        if (progress && !progress((i + 1.0) / n)) {
            g.Finish();
            if (error) *error = L"Cancelled.";
            return false;
        }
    }
    const HRESULT fin = g.Finish();
    if (SUCCEEDED(hr)) hr = fin;
    if (FAILED(hr) && error) *error = HrText(L"Couldn't write the GIF", hr);
    return SUCCEEDED(hr);
}

std::vector<BitmapPtr> VideoThumbnails(const Sequence& s, int count, int maxSide, const std::function<bool()>& cancelled) {
    std::vector<BitmapPtr> out;
    SequenceReader r;
    if (!r.Open(s) || r.Duration() <= 0) return out;
    for (int i = 0; i < count; ++i) {
        if (cancelled && cancelled()) return {};
        const double t = r.Duration() * (i + 0.5) / count;
        r.Seek(t);
        BitmapPtr f, last;
        double ft = 0;
        while (r.Read(&f, &ft)) {
            last = f;
            if (ft >= t - 0.04) break;
        }
        if (!last || !(last = r.Fit(last))) continue;
        const double k = std::min(1.0, (double)maxSide / std::max(last->Width(), last->Height()));
        out.push_back(k < 1 ? Resample(*last, std::max(1, (int)std::lround(last->Width() * k)), std::max(1, (int)std::lround(last->Height() * k))) : last);
    }
    return out;
}

// ---------- speech to text ----------

namespace {

bool WriteWav16(const std::wstring& path, const std::vector<int16_t>& pcm, int rate) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint32_t data = (uint32_t)(pcm.size() * 2);
    struct {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t size;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt[4] = {'f', 'm', 't', ' '};
        uint32_t fmtSize = 16;
        uint16_t format = 1, channels = 1;
        uint32_t rate, bytesPerSec;
        uint16_t align = 2, bits = 16;
        char dataTag[4] = {'d', 'a', 't', 'a'};
        uint32_t dataSize;
    } h;
    h.size = 36 + data;
    h.rate = (uint32_t)rate;
    h.bytesPerSec = (uint32_t)rate * 2;
    h.dataSize = data;
    DWORD wrote = 0;
    bool ok = WriteFile(f, &h, sizeof(h), &wrote, nullptr) && WriteFile(f, pcm.data(), data, &wrote, nullptr);
    CloseHandle(f);
    return ok;
}

void ClearEvent(SPEVENT& ev) {
    if (ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) reinterpret_cast<IUnknown*>(ev.lParam)->Release();
    else if ((ev.elParamType == SPET_LPARAM_IS_POINTER || ev.elParamType == SPET_LPARAM_IS_STRING) && ev.lParam) CoTaskMemFree((void*)ev.lParam);
    ev = {};
}

// SAPI dictation over a 16 kHz mono WAV file. Word times are relative to the file, plus `offset`.
bool TranscribeWav(const std::wstring& wav, double offset, std::vector<CaptionWord>* words, std::wstring* error) {
    const std::wstring noEngine =
        L"Auto captions use Windows speech recognition, which isn't installed for your language. Add a speech pack in "
        L"Settings › Time & language › Speech, or add captions by hand with T.";
    ComPtr<ISpRecognizer> rec;
    ComPtr<ISpObjectTokenCategory> cat;
    ComPtr<ISpObjectToken> token;
    LPWSTR id = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&rec))) ||
        FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat))) || FAILED(cat->SetId(SPCAT_RECOGNIZERS, FALSE)) ||
        FAILED(cat->GetDefaultTokenId(&id)) || !id) {
        if (error) *error = noEngine;
        return false;
    }
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token));
    if (SUCCEEDED(hr)) hr = token->SetId(nullptr, id, FALSE);
    CoTaskMemFree(id);
    if (SUCCEEDED(hr)) hr = rec->SetRecognizer(token.Get());
    if (FAILED(hr)) {
        if (error) *error = noEngine;
        return false;
    }
    ComPtr<ISpStream> stream;
    hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    WAVEFORMATEX wfx{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    if (SUCCEEDED(hr)) hr = stream->BindToFile(wav.c_str(), SPFM_OPEN_READONLY, &SPDFID_WaveFormatEx, &wfx, SPFEI_ALL_EVENTS);
    if (SUCCEEDED(hr)) hr = rec->SetInput(stream.Get(), TRUE);
    ComPtr<ISpRecoContext> ctx;
    ComPtr<ISpRecoGrammar> grammar;
    if (SUCCEEDED(hr)) hr = rec->CreateRecoContext(&ctx);
    const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
    if (SUCCEEDED(hr)) hr = ctx->SetNotifyWin32Event();
    if (SUCCEEDED(hr)) hr = ctx->SetInterest(interest, interest);
    if (SUCCEEDED(hr)) hr = ctx->CreateGrammar(0, &grammar);
    if (SUCCEEDED(hr)) hr = grammar->LoadDictation(nullptr, SPLO_STATIC);
    if (SUCCEEDED(hr)) hr = grammar->SetDictationState(SPRS_ACTIVE);
    if (SUCCEEDED(hr)) hr = rec->SetRecoState(SPRST_ACTIVE_ALWAYS);
    if (FAILED(hr)) {
        if (error) *error = HrText(L"Speech recognition couldn't start", hr);
        return false;
    }
    bool done = false;
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    while (!done && GetTickCount64() < deadline) {
        if (ctx->WaitForNotifyEvent(2000) != S_OK) continue;
        SPEVENT ev{};
        ULONG got = 0;
        while (SUCCEEDED(ctx->GetEvents(1, &ev, &got)) && got == 1) {
            if (ev.eEventId == SPEI_END_SR_STREAM) done = true;
            if (ev.eEventId == SPEI_RECOGNITION && ev.elParamType == SPET_LPARAM_IS_OBJECT) {
                auto* r = reinterpret_cast<ISpRecoResult*>(ev.lParam);
                SPRECORESULTTIMES times{};
                SPPHRASE* ph = nullptr;
                if (SUCCEEDED(r->GetResultTimes(&times)) && SUCCEEDED(r->GetPhrase(&ph)) && ph) {
                    for (ULONG i = 0; i < ph->Rule.ulCountOfElements; ++i) {
                        const SPPHRASEELEMENT& el = ph->pElements[i];
                        if (!el.pszDisplayText || !*el.pszDisplayText) continue;
                        const double s = offset + (times.ullStart + el.ulAudioTimeOffset) / kTicks;
                        const double e = s + el.ulAudioSizeTime / kTicks;
                        if ((el.bDisplayAttributes & SPAF_CONSUME_LEADING_SPACES) && !words->empty()) {  // punctuation joins the word before
                            words->back().text += el.pszDisplayText;
                            words->back().end = std::max(words->back().end, e);
                        } else {
                            words->push_back({s, std::max(e, s + 0.05), el.pszDisplayText});
                        }
                    }
                    CoTaskMemFree(ph);
                }
            }
            ClearEvent(ev);
        }
    }
    rec->SetRecoState(SPRST_INACTIVE);
    return true;
}

}  // namespace

bool Transcribe(const Sequence& s, double from, double to, std::vector<Caption>* out, std::wstring* error) {
    SequenceAudio audio(s, from, to, 16000, 1);
    if (!audio.AnyAudio()) {
        if (error) *error = L"This video has no sound. For recordings, turn on system audio or the microphone in Settings › Recording.";
        return false;
    }
    std::vector<float> mono;
    while (audio.Read(mono)) {}
    std::vector<int16_t> pcm(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) pcm[i] = (int16_t)std::lround(std::clamp(mono[i], -1.f, 1.f) * 32767);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring wav = std::wstring(tmp) + L"ather-speech-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".wav";
    if (!WriteWav16(wav, pcm, 16000)) {
        if (error) *error = L"Couldn't read the audio.";
        return false;
    }
    std::vector<CaptionWord> words;
    const bool ok = TranscribeWav(wav, from, &words, error);
    DeleteFileW(wav.c_str());
    if (!ok) return false;
    *out = ChunkCaptions(words);
    return true;
}

// ---------- preview playback ----------

namespace {

class EngineNotify : public IMFMediaEngineNotify {
public:
    EngineNotify(HWND hwnd, UINT msg, std::shared_ptr<std::atomic<bool>> failed) : hwnd_(hwnd), msg_(msg), failed_(std::move(failed)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
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
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR) *failed_ = true;  // playback stopped for good
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) SetEvent((HANDLE)param1);
        else if (IsWindow(hwnd_)) PostMessageW(hwnd_, msg_, event, (LPARAM)param1);
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    HWND hwnd_;
    UINT msg_;
    std::shared_ptr<std::atomic<bool>> failed_;
};

}  // namespace

struct VideoPlayer::Impl {
    ComPtr<IMFMediaEngine> engine;
    std::shared_ptr<std::atomic<bool>> failed = std::make_shared<std::atomic<bool>>(false);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> target;
    DWORD w = 0, h = 0;
};

VideoPlayer::VideoPlayer() : p_(std::make_unique<Impl>()) {}

VideoPlayer::~VideoPlayer() {
    if (p_->engine) p_->engine->Shutdown();
}

std::unique_ptr<VideoPlayer> VideoPlayer::Open(const std::wstring& path, HWND notify, UINT msg, std::wstring* error) {
    EnsureMediaFoundation();
    std::unique_ptr<VideoPlayer> vp(new VideoPlayer());
    ComPtr<IMFMediaEngineClassFactory> factory;
    ComPtr<IMFAttributes> attr;
    ComPtr<EngineNotify> cb;
    cb.Attach(new EngineNotify(notify, msg, vp->p_->failed));
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, cb.Get());
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 87 /* DXGI_FORMAT_B8G8R8A8_UNORM */);
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(0, attr.Get(), &vp->p_->engine);
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&vp->p_->wic));
    if (SUCCEEDED(hr)) {
        BSTR url = SysAllocString(path.c_str());
        hr = vp->p_->engine->SetSource(url);
        SysFreeString(url);
    }
    if (FAILED(hr)) {
        if (error) *error = HrText(L"Can't play this video", hr);
        return nullptr;
    }
    vp->p_->engine->SetAutoPlay(FALSE);
    vp->p_->engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    return vp;
}

void VideoPlayer::Play() { p_->engine->Play(); }
void VideoPlayer::Pause() { p_->engine->Pause(); }
bool VideoPlayer::Playing() const { return !*p_->failed && !p_->engine->IsPaused() && !p_->engine->IsEnded(); }
void VideoPlayer::Seek(double t) { p_->engine->SetCurrentTime(std::max(0.0, t)); }
double VideoPlayer::Now() const { return p_->engine->GetCurrentTime(); }
void VideoPlayer::SetMuted(bool m) { p_->engine->SetMuted(m); }

void VideoPlayer::SetRate(double r) {
    p_->engine->SetDefaultPlaybackRate(r);
    p_->engine->SetPlaybackRate(r);
}

bool VideoPlayer::Failed() const { return *p_->failed; }

BitmapPtr VideoPlayer::NewFrame(double* t, SIZE frame, SIZE content) {
    LONGLONG pts = 0;
    if (p_->engine->OnVideoStreamTick(&pts) != S_OK) return nullptr;
    DWORD nw = 0, nh = 0;
    if (FAILED(p_->engine->GetNativeVideoSize(&nw, &nh)) || !nw || !nh) return nullptr;
    // The engine's native size is corrected for non-square pixels, while decoded frames (paused, export) keep the
    // stored pixels; fitting the stored size keeps the playing picture where the paused one is.
    const DWORD w = frame.cx > 0 ? (DWORD)frame.cx : nw, h = frame.cy > 0 ? (DWORD)frame.cy : nh;
    RECT dst{0, 0, (LONG)w, (LONG)h};
    if (frame.cx > 0) {
        const double cw = content.cx > 0 ? content.cx : nw, ch = content.cy > 0 ? content.cy : nh;
        const double k = std::min(w / cw, h / ch);
        const LONG fw = std::clamp((LONG)std::lround(cw * k), 1L, (LONG)w), fh = std::clamp((LONG)std::lround(ch * k), 1L, (LONG)h);
        dst = {((LONG)w - fw) / 2, ((LONG)h - fh) / 2, ((LONG)w - fw) / 2 + fw, ((LONG)h - fh) / 2 + fh};
    }
    if (!p_->target || w != p_->w || h != p_->h) {
        p_->target.Reset();
        if (FAILED(p_->wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnDemand, &p_->target))) return nullptr;
        p_->w = w;
        p_->h = h;
    }
    MFARGB border{0, 0, 0, 255};
    if (FAILED(p_->engine->TransferVideoFrame(p_->target.Get(), nullptr, &dst, &border))) return nullptr;
    auto out = Bitmap::Create((int)w, (int)h);
    if (!out || FAILED(p_->target->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(out->Bits())))) return nullptr;
    for (LONG y = 0; y < (LONG)h; ++y) {  // opaque, and black around the picture
        uint32_t* row = out->Bits() + (size_t)y * w;
        const bool bar = y < dst.top || y >= dst.bottom;
        for (LONG x = 0; x < (LONG)w; ++x) row[x] = bar || x < dst.left || x >= dst.right ? 0xFF000000u : row[x] | 0xFF000000u;
    }
    *t = pts / kTicks;
    return out;
}

// ---------- sequence playback ----------

std::unique_ptr<SequencePlayer> SequencePlayer::Open(const Sequence& s, HWND notify, UINT msg, std::wstring* error) {
    std::unique_ptr<SequencePlayer> sp(new SequencePlayer());
    sp->notify_ = notify;
    sp->msg_ = msg;
    sp->SetSequence(s);
    if (!sp->Cur()) {
        if (error && error->empty()) *error = L"Can't play this video";
        return nullptr;
    }
    return sp;
}

VideoPlayer* SequencePlayer::Cur() const {
    if (cur_ >= seq_.clips.size()) return nullptr;
    for (const auto& [path, p] : players_)
        if (_wcsicmp(path.c_str(), seq_.clips[cur_].path.c_str()) == 0) return p.get();
    return nullptr;
}

void SequencePlayer::SetSequence(const Sequence& s) {
    for (auto& [path, p] : players_)
        if (p) p->Pause();
    std::vector<std::pair<std::wstring, std::unique_ptr<VideoPlayer>>> keep;
    for (const auto& c : s.clips) {
        bool have = false;
        for (const auto& k : keep) have = have || _wcsicmp(k.first.c_str(), c.path.c_str()) == 0;
        if (have) continue;
        std::unique_ptr<VideoPlayer> p;
        for (auto& [path, old] : players_)
            if (old && _wcsicmp(path.c_str(), c.path.c_str()) == 0) p = std::move(old);
        if (!p) p = VideoPlayer::Open(c.path, notify_, msg_, nullptr);
        if (p) {
            p->SetRate(rate_);
            p->SetMuted(muted_);
        }
        keep.emplace_back(c.path, std::move(p));
    }
    players_ = std::move(keep);
    seq_ = s;
    starts_.clear();
    for (size_t i = 0; i < s.clips.size(); ++i) starts_.push_back(ClipStart(s.clips, i));
    cur_ = 0;
    playing_ = ended_ = false;
}

void SequencePlayer::Start(VideoPlayer* p) {
    if (!p) return;
    p->SetRate(rate_);
    p->SetMuted(muted_);
    p->Play();
}

void SequencePlayer::Play() {
    playing_ = true;
    ended_ = false;
    if (VideoPlayer* p = Cur(); !p || p->Failed()) return Advance();
    Start(Cur());
}

void SequencePlayer::Advance() {
    VideoPlayer* const old = Cur();
    const size_t from = cur_;
    for (size_t i = cur_ + 1; i < seq_.clips.size(); ++i) {
        cur_ = i;
        VideoPlayer* p = Cur();
        if (!p || p->Failed()) continue;  // can't play: on to the next one
        // The same file going on where the last piece stopped (a split): it just keeps playing, no seek and no
        // stutter. Anything else is a jump.
        if (p == old && i == from + 1 && std::fabs(seq_.clips[i].in - seq_.clips[from].out) < 0.05) return;
        if (old && old != p) old->Pause();
        p->Seek(seq_.clips[i].in);
        Start(p);
        return;
    }
    if (old) old->Pause();
    ended_ = true;
}

void SequencePlayer::Pause() {
    playing_ = false;
    if (auto* p = Cur()) p->Pause();
}

bool SequencePlayer::Playing() const {
    const VideoPlayer* p = Cur();
    return playing_ && !ended_ && p && p->Playing();
}

void SequencePlayer::Seek(double t) {
    const auto spot = LocateClip(seq_.clips, t);
    if (!spot) return;
    if (spot->first != cur_) {
        if (auto* p = Cur()) p->Pause();
        cur_ = spot->first;
    }
    ended_ = false;
    if (auto* p = Cur()) {
        p->Seek(spot->second);
        if (playing_) Start(p);
    }
}

double SequencePlayer::Now() const {
    if (cur_ >= seq_.clips.size()) return seq_.Duration();
    const Clip& c = seq_.clips[cur_];
    const VideoPlayer* p = Cur();
    return starts_[cur_] + (p ? std::clamp(p->Now() - c.in, 0.0, c.Duration()) : 0);
}

void SequencePlayer::SetRate(double r) {
    rate_ = r;
    for (auto& [path, p] : players_)
        if (p) p->SetRate(r);
}

void SequencePlayer::SetMuted(bool m) {
    muted_ = m;
    for (auto& [path, p] : players_)
        if (p) p->SetMuted(m);
}

BitmapPtr SequencePlayer::NewFrame(double* t) {
    VideoPlayer* p = Cur();
    if (!p) return nullptr;
    if (playing_ && !ended_) {
        const Clip& c = seq_.clips[cur_];
        if (p->Now() >= c.out - 0.02 || !p->Playing()) {  // this clip is over: hand over to the next one
            Advance();
            p = Cur();
            if (!p || ended_) return nullptr;
        }
    }
    const Clip& c = seq_.clips[cur_];
    double ts = 0;
    BitmapPtr f = p->NewFrame(&ts, seq_.size, {c.w, c.h});  // fitted into the sequence frame by the engine
    // Frames from outside the clip are the engine catching up after a hand-over or a seek, or a part that was cut.
    if (!f || ts < c.in - 0.25 || ts > c.out + 0.001) return nullptr;
    *t = starts_[cur_] + std::clamp(ts - c.in, 0.0, c.Duration());
    return f;
}

// ---------- test clip ----------

bool WriteTestClip(const std::wstring& path, int w, int h, int fps, double seconds, bool tone, int rotation) {
    Mp4Writer mw;
    if (FAILED(mw.Begin(path, w, h, fps, tone ? kRate : 0, 2, rotation))) return false;
    const uint32_t colors[] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu};
    std::vector<uint32_t> px((size_t)w * h);
    const int n = (int)std::lround(seconds * fps);
    int64_t audio = 0;
    std::vector<int16_t> pcm;
    for (int i = 0; i < n; ++i) {
        std::fill(px.begin(), px.end(), colors[(i / fps) % 3]);
        if (FAILED(mw.WriteFrame(px.data(), std::llround(i * kTicks / fps), std::llround(kTicks / fps)))) return false;
        if (tone) {
            const int64_t until = std::llround((i + 1) * (double)kRate / fps);
            pcm.clear();
            for (; audio < until; ++audio) {
                const int16_t v = (int16_t)std::lround(std::sin(2 * 3.14159265358979 * 440 * audio / kRate) * 12000);
                pcm.push_back(v);
                pcm.push_back(v);
            }
            if (!pcm.empty()) mw.WriteAudio(pcm.data(), (uint32_t)(pcm.size() / 2), std::llround((audio - (int64_t)pcm.size() / 2) * kTicks / kRate));
        }
    }
    return SUCCEEDED(mw.Finalize());
}

// ---------- tests (VideoTests.swift) ----------

// Clips of different files and shapes play back to back: cut points, black bars, sound only where there is some.
ATHER_TEST(video_sequence_joins_clips_into_one_video) {
    const std::wstring dir = test::TempDir();
    const std::wstring a = dir + L"/a.mp4", b = dir + L"/b.mp4", out = dir + L"/joined.mp4";
    CHECK(WriteTestClip(a, 640, 360, 30, 3, true));   // red, green, blue seconds, with a tone
    CHECK(WriteTestClip(b, 320, 320, 30, 2, false));  // square and silent
    auto ca = ClipOf(a), cb = ClipOf(b);
    CHECK(ca && cb);
    if (!ca || !cb) return;
    CHECK(ca->hasAudio && !cb->hasAudio);
    Clip a1 = *ca, b1 = *cb, a2 = *ca;
    a1.in = 1, a1.out = 2;  // green
    b1.in = 0, b1.out = 1;  // red, square
    a2.id = NewItemId();
    a2.in = 2, a2.out = 3;  // blue
    VideoEdit e;
    e.clips = {a1, b1, a2};
    e.frameW = 640;
    e.frameH = 360;
    e.trimEnd = ClipsDuration(e.clips);
    CHECK_NEAR(e.trimEnd, 3, 1e-9);
    std::wstring err;
    CHECK(ExportMp4(L"", e, out, &err));
    VideoInfo vi;
    CHECK(ProbeVideo(out, &vi) && std::fabs(vi.duration - 3) < 0.15 && vi.w == 640 && vi.h == 360 && vi.hasAudio);
    VideoReader r;
    CHECK(r.Open(out));
    auto at = [&](double t, int x, int y) {
        r.Seek(t);
        BitmapPtr f, last;
        double ft = 0;
        while (r.Read(&f, &ft)) {
            last = f;
            if (ft >= t - 0.02) break;
        }
        return last ? last->Bits()[(size_t)y * last->Width() + x] & 0xFFFFFF : 0x123456u;
    };
    auto is = [](uint32_t c, int ch) { return ((c >> (16 - 8 * ch)) & 255) > 180 && ((c >> (16 - 8 * ((ch + 1) % 3))) & 255) < 80; };
    CHECK(is(at(0.5, 320, 180), 1));  // green from a
    CHECK(is(at(1.5, 320, 180), 0));  // red from b, in the middle
    CHECK((at(1.5, 20, 180) & 0xF0F0F0) == 0);  // with black bars at the sides
    CHECK(is(at(2.5, 320, 180), 2));  // blue from a again
    // The sequence reader reports timeline times in order.
    SequenceReader sr;
    CHECK(sr.Open(SequenceOf(L"", e)));
    BitmapPtr f;
    double t = -1, prev = -1;
    int n = 0;
    bool ordered = true;
    while (sr.Read(&f, &t)) {
        ordered = ordered && t >= prev - 1e-9 && sr.Fit(f)->Width() == 640;
        prev = t;
        ++n;
    }
    CHECK(ordered);
    CHECK(n >= 85 && n <= 95);
    CHECK(prev > 2.9 && prev < 3.0);
}

ATHER_TEST(video_rotated_phone_clip_reads_upright_like_the_preview) {
    for (int rot : {90, 270}) {
        const std::wstring path = test::TempDir() + L"/rot" + std::to_wstring(rot) + L".mp4";
        CHECK(WriteTestClip(path, 320, 160, 10, 1, false, rot));
        VideoReader vr;
        CHECK(vr.Open(path));
        CHECK(vr.Size().cx == 160 && vr.Size().cy == 320);
        BitmapPtr f;
        double t = 0;
        CHECK(vr.Read(&f, &t) && f && f->Width() == 160 && f->Height() == 320);
        VideoInfo vi;
        BitmapPtr thumb;
        CHECK(ProbeVideo(path, &vi, 0.2, 0, &thumb) && vi.w == 160 && vi.h == 320 && thumb && thumb->Width() == 160);
    }
    // Which way it turns: a mark in the top-left corner of the stored picture ends up top-right after 90°.
    auto src = Bitmap::Create(4, 2);
    std::fill(src->Bits(), src->Bits() + 8, 0xFF000000u);
    src->Bits()[0] = 0xFFFFFFFFu;
    auto r90 = RotateBitmap(*src, 90), r270 = RotateBitmap(*src, 270), r180 = RotateBitmap(*src, 180);
    CHECK(r90->Width() == 2 && r90->Height() == 4 && r90->Bits()[1] == 0xFFFFFFFFu);
    CHECK(r270->Bits()[3 * 2] == 0xFFFFFFFFu);  // bottom-left
    CHECK(r180->Bits()[7] == 0xFFFFFFFFu);
}

ATHER_TEST(video_time_stretch_keeps_pitch) {
    const int rate = 48000;
    std::vector<float> in((size_t)rate * 2);
    for (int i = 0; i < rate; ++i) in[(size_t)i * 2] = in[(size_t)i * 2 + 1] = (float)std::sin(2 * 3.14159265358979 * 440 * i / rate);
    for (double speed : {2.0, 0.5}) {
        TimeStretch ts(2, rate, speed);
        std::vector<float> out;
        for (size_t at = 0; at < (size_t)rate; at += 4000) {  // fed in pieces, like the export does
            ts.Push(in.data() + at * 2, std::min<size_t>(4000, rate - at));
            ts.Pull(out);
        }
        ts.Finish(out);
        const double frames = out.size() / 2.0;
        test::Note("speed " + std::to_string(speed) + " frames " + std::to_string(frames));
        CHECK_NEAR(frames, rate / speed, rate / speed * 0.06);
        // Same pitch: zero crossings per second in the middle stay ~880.
        const size_t a = (size_t)(frames * 0.2), b = (size_t)(frames * 0.8);
        int crossings = 0;
        for (size_t i = a + 1; i < b; ++i) crossings += (out[(i - 1) * 2] < 0) != (out[i * 2] < 0);
        CHECK_NEAR(crossings / ((b - a) / (double)rate), 880, 60);
    }
}

ATHER_TEST(video_export_trim_speed_crop_captions) {
    const std::wstring dir = test::TempDir();
    const std::wstring clip = dir + L"\\clip.mp4", out = dir + L"\\out.mp4", gif = dir + L"\\out.gif";
    CHECK(WriteTestClip(clip, 640, 360, 30, 3, true));
    VideoEdit e;
    e.trimStart = 1;
    e.trimEnd = 3;
    e.speed = 2;
    e.crop = VRect{100, 50, 321, 201};
    Caption c;
    c.start = 1.2;
    c.end = 2.5;
    c.text = L"Click Deploy";
    e.captions = {c};
    std::wstring err;
    const bool ok = ExportMp4(clip, e, out, &err);
    test::Note("export error: " + ToUtf8(err));
    CHECK(ok);
    VideoInfo vi;
    CHECK(ProbeVideo(out, &vi));
    CHECK_NEAR(vi.duration, 1, 0.1);  // 2 s trimmed, played at 2×
    CHECK_EQ(vi.w, 320);  // crop, rounded down to even
    CHECK_EQ(vi.h, 200);
    CHECK(vi.hasAudio);
    auto pixel = [](const Bitmap& b, int x, int y) { return b.Bits()[(size_t)y * b.Width() + x]; };
    // The first frame comes from second 1 of the source (green), not 0 (red).
    BitmapPtr first, mid, after;
    CHECK(ProbeVideo(out, nullptr, 0.05, 0, &first) && first);
    if (first) {
        const uint32_t p = pixel(*first, 10, 10);
        CHECK(((p >> 8) & 255) > 150);
        CHECK(((p >> 16) & 255) < 100);
    }
    // The caption (1.2–2.5 s in the source → 0.1–0.75 s out) is burned in: a dark pill near the bottom.
    auto darkPixels = [&](const Bitmap& img) {
        int n = 0;
        for (int y = 150; y < 198; y += 4)
            for (int x = 100; x < 220; ++x) {
                const uint32_t p = pixel(img, x, y);
                n += std::max({(p >> 16) & 255, (p >> 8) & 255, p & 255}) < 153;
            }
        return n;
    };
    CHECK(ProbeVideo(out, nullptr, 0.4, 0, &mid) && mid);
    if (mid) CHECK(darkPixels(*mid) > 10);
    CHECK(ProbeVideo(out, nullptr, 0.9, 0, &after) && after);
    if (after) CHECK_EQ(darkPixels(*after), 0);

    CHECK(ExportGif(clip, e, gif, &err, 10));
    CHECK_NEAR(GifDuration(gif), 1.0, 0.02);
    int gw = 0, gh = 0;
    CHECK(ImageSize(gif, &gw, &gh) && gw == 320 && gh == 200);
}

// Speech made by Windows text-to-speech, transcribed by Windows dictation. Skipped where no recognizer is installed.
ATHER_TEST(video_transcribe_speech_with_word_times) {
    const std::wstring wav = test::TempDir() + L"\\speech.wav";
    ComPtr<ISpVoice> voice;
    ComPtr<ISpStream> stream;
    WAVEFORMATEX wfx{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    if (SUCCEEDED(hr)) hr = stream->BindToFile(wav.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
    if (SUCCEEDED(hr)) hr = voice->SetOutput(stream.Get(), TRUE);
    if (SUCCEEDED(hr)) hr = voice->Speak(L"Open the settings. Then click save.", SPF_DEFAULT, nullptr);
    if (stream) stream->Close();
    if (FAILED(hr)) {
        test::Note("no text-to-speech voice: skipped");
        return;
    }
    std::vector<CaptionWord> words;
    std::wstring err;
    if (!TranscribeWav(wav, 10, &words, &err)) {
        test::Out("  (no speech recognizer: " + ToUtf8(err.substr(0, 60)) + "…)\n");
        CHECK(!err.empty());  // a clear message instead of silence
        return;
    }
    std::wstring all;
    for (const auto& w : words) all += LowerText(w.text) + L" ";
    test::Note("heard: " + ToUtf8(all));
    int hits = 0;
    for (const wchar_t* k : {L"open", L"settings", L"click", L"save"}) hits += all.find(k) != std::wstring::npos;
    CHECK(hits >= 2);
    CHECK(!words.empty());
    for (size_t i = 0; i < words.size(); ++i) {
        CHECK(words[i].start >= 10);  // offset applied
        CHECK(words[i].end > words[i].start);
        if (i) CHECK(words[i].start >= words[i - 1].start);
    }
}

ATHER_TEST(video_thumbnails_spread_over_the_clip) {
    const std::wstring clip = test::TempDir() + L"\\clip.mp4";
    CHECK(WriteTestClip(clip, 320, 180, 30, 3, false));
    const auto th = VideoThumbnails(SequenceOf(clip, {}), 6, 64);
    CHECK_EQ(th.size(), 6u);
    if (th.size() == 6) {
        CHECK(th[0]->Width() <= 64);
        CHECK(((th[0]->Bits()[0] >> 16) & 255) > 150);  // red first
        CHECK((th[5]->Bits()[0] & 255) > 150);          // blue last
    }
}

}  // namespace ather
