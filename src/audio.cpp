#include "audio.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <thread>
#include <vector>

#include <ks.h>
#include <ksmedia.h>

using Microsoft::WRL::ComPtr;

namespace ather {

namespace {

// One WASAPI endpoint, converted to 48 kHz interleaved stereo float in `fifo`.
struct Source {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* wf = nullptr;
    bool isFloat = false;
    int channels = 2, bits = 32, rate = 48000;
    std::vector<float> pending;  // stereo frames at the device rate, not yet resampled
    double pos = 0;              // resampler read position within `pending`
    std::vector<float> fifo;     // stereo frames at 48 kHz

    ~Source() {
        if (client) client->Stop();
        if (wf) CoTaskMemFree(wf);
    }

    HRESULT Open(EDataFlow flow, bool loopback) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&wf);
        if (FAILED(hr)) return hr;
        channels = wf->nChannels;
        bits = wf->wBitsPerSample;
        rate = (int)wf->nSamplesPerSec;
        isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;  // container size drives parsing
        }
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 10'000'000, 0, wf,
                                nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = client->Start();
        return hr;
    }

    float Sample(const BYTE* p) const {
        if (isFloat) return *reinterpret_cast<const float*>(p);
        switch (bits) {
            case 16: return *reinterpret_cast<const int16_t*>(p) / 32768.f;
            case 24: return ((int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8) / 8388608.f;
            case 32: return *reinterpret_cast<const int32_t*>(p) / 2147483648.f;
        }
        return 0;
    }

    // Pulls every available packet. While paused the data is read and thrown away.
    void Drain(bool discard) {
        UINT32 packet = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (!discard) {
                const int stride = wf->nBlockAlign, bytesPer = bits / 8;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                for (UINT32 i = 0; i < frames; ++i) {
                    float l = 0, r = 0;
                    if (!silent) {
                        const BYTE* f = data + (size_t)i * stride;
                        l = Sample(f);
                        r = channels > 1 ? Sample(f + bytesPer) : l;
                    }
                    pending.push_back(l);
                    pending.push_back(r);
                }
            }
            capture->ReleaseBuffer(frames);
        }
        Resample();
    }

    void Resample() {
        if (rate == AudioCapture::kRate) {
            fifo.insert(fifo.end(), pending.begin(), pending.end());
            pending.clear();
            return;
        }
        const double step = (double)rate / AudioCapture::kRate;
        const size_t n = pending.size() / 2;
        while (pos + 1 < (double)n) {
            const size_t i = (size_t)pos;
            const float f = (float)(pos - i);
            fifo.push_back(pending[i * 2] * (1 - f) + pending[i * 2 + 2] * f);
            fifo.push_back(pending[i * 2 + 1] * (1 - f) + pending[i * 2 + 3] * f);
            pos += step;
        }
        const size_t used = std::min((size_t)pos, n ? n - 1 : 0);  // keep one frame for interpolation
        pending.erase(pending.begin(), pending.begin() + used * 2);
        pos -= (double)used;
    }

    size_t Frames() const { return fifo.size() / 2; }
};

}  // namespace

struct AudioCapture::Impl {
    std::vector<std::unique_ptr<Source>> sources;
    std::thread thread;
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Impl() { CloseHandle(stopEvent); }
};

AudioCapture::AudioCapture() : impl_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Open(bool systemAudio, bool microphone, std::wstring* error, std::wstring* warning) {
    auto add = [&](EDataFlow flow, bool loopback, const wchar_t* name) {
        auto s = std::make_unique<Source>();
        HRESULT hr = s->Open(flow, loopback);
        if (SUCCEEDED(hr)) {
            impl_->sources.push_back(std::move(s));
        } else if (warning) {
            wchar_t msg[96];
            swprintf_s(msg, L"%s unavailable (0x%08lX). ", name, (unsigned long)hr);
            *warning += msg;
        }
    };
    if (systemAudio) add(eRender, true, L"System audio");
    if (microphone) add(eCapture, false, L"Microphone");
    if (impl_->sources.empty() && error) *error = L"No audio device could be opened.";
    return !impl_->sources.empty();
}

void AudioCapture::Run(const RecClock* clock, Callback cb) {
    // The WASAPI objects are free-threaded, so they can be used from this worker after Open() on the caller.
    impl_->thread = std::thread([this, clock, cb = std::move(cb)] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Impl& m = *impl_;
        constexpr int64_t kLag = kRate * 60 / 1000;  // stay 60 ms behind real time so slow devices can catch up
        int64_t produced = 0;
        std::vector<int16_t> out;
        bool stopping = false;
        while (!stopping) {
            stopping = WaitForSingleObject(m.stopEvent, 10) == WAIT_OBJECT_0;
            const bool paused = clock->Paused();
            for (auto& s : m.sources) s->Drain(paused);
            if (paused) continue;
            const int64_t target = clock->Active(RecClock::Now()) * kRate / clock->freq - (stopping ? 0 : kLag);
            if (target <= produced) continue;
            const size_t n = (size_t)std::min<int64_t>(target - produced, kRate);
            out.assign(n * 2, 0);
            std::vector<float> mix(n * 2, 0.f);
            for (auto& s : m.sources) {
                const size_t take = std::min(n, s->Frames());  // a quiet loopback device delivers nothing: pad silence
                for (size_t i = 0; i < take * 2; ++i) mix[i] += s->fifo[i];
                s->fifo.erase(s->fifo.begin(), s->fifo.begin() + take * 2);
                const size_t maxBacklog = (size_t)kRate / 5 * 2;  // >200 ms queued means clock drift: drop it
                if (s->fifo.size() > maxBacklog) s->fifo.erase(s->fifo.begin(), s->fifo.end() - maxBacklog);
            }
            for (size_t i = 0; i < n * 2; ++i)
                out[i] = (int16_t)std::clamp((int)std::lround(mix[i] * 32767.f), -32768, 32767);
            cb(out.data(), (uint32_t)n, produced * 10'000'000 / kRate);
            produced += (int64_t)n;
        }
        m.sources.clear();
        if (SUCCEEDED(co)) CoUninitialize();
    });
}

void AudioCapture::Stop() {
    if (!impl_->thread.joinable()) {
        impl_->sources.clear();
        return;
    }
    SetEvent(impl_->stopEvent);
    impl_->thread.join();
}

}  // namespace ather
