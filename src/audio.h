#pragma once
#include <atomic>
#include <memory>

#include "common.h"

namespace ather {

// Shared recording timeline in QPC ticks; paused time is excluded so audio and video stay in sync.
struct RecClock {
    int64_t freq = 1;
    std::atomic<int64_t> start{0};
    std::atomic<int64_t> pausedTotal{0};
    std::atomic<int64_t> pauseStart{0};

    static int64_t Now() {
        LARGE_INTEGER v;
        QueryPerformanceCounter(&v);
        return v.QuadPart;
    }
    int64_t Active(int64_t now) const {
        const int64_t ps = pauseStart.load();
        return now - start.load() - pausedTotal.load() - (ps ? now - ps : 0);
    }
    int64_t ActiveTicks100ns(int64_t now) const { return Active(now) * 10'000'000 / freq; }
    bool Paused() const { return pauseStart.load() != 0; }
    void Pause() {
        int64_t expected = 0;
        pauseStart.compare_exchange_strong(expected, Now());
    }
    void Resume() {
        const int64_t ps = pauseStart.exchange(0);
        if (ps) pausedTotal += Now() - ps;
    }
};

// Captures system audio (WASAPI loopback) and/or the default microphone, resamples both to
// 48 kHz stereo and mixes them. Output is delivered as 16-bit PCM on the capture thread.
class AudioCapture {
public:
    static constexpr int kRate = 48000, kChannels = 2;
    using Callback = std::function<void(const int16_t* pcm, uint32_t frames, int64_t time100ns)>;

    AudioCapture();
    ~AudioCapture();
    // Opens the devices. Returns false (with a reason) if none of the requested sources could be opened;
    // a single unavailable source is reported through `warning` instead.
    bool Open(bool systemAudio, bool microphone, std::wstring* error, std::wstring* warning);
    void Run(const RecClock* clock, Callback cb);  // starts the capture thread
    void Stop();                                   // flushes and joins

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ather
