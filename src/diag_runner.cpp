// Temporary diagnostics (not for merging): what Media Foundation on the CI runner does with the app's own test clips.
#include "videoio.h"

#include <shlobj.h>

#include <cstdio>

#include "common.h"
#include "json.h"
#include "media.h"
#include "selftest.h"

namespace ather {
namespace {

std::string Fmt(double v) {
    char b[32];
    snprintf(b, sizeof b, "%.7f", v);
    return b;
}

void Dump(const std::wstring& path, int fps) {
    const std::vector<double> times = FrameTimes(path);
    std::string s = ToUtf8(path) + ": " + std::to_string(times.size()) + " times:";
    for (size_t i = 0; i < times.size(); ++i)
        if (i < 4 || i + 3 >= times.size()) s += " [" + std::to_string(i) + "]" + Fmt(times[i]) + " (" + Fmt(times[i] - (double)i / fps) + ")";
    test::Out(s + "\n");
    VideoInfo vi;
    if (ProbeVideo(path, &vi)) test::Out("  probe: duration " + Fmt(vi.duration) + ", fps " + Fmt(vi.fps) + "\n");
    std::vector<double> dec;
    VideoReader r;
    if (r.Open(path)) {
        BitmapPtr f;
        double t = 0;
        while (r.Read(&f, &t)) dec.push_back(t);
    }
    s = "  decoded " + std::to_string(dec.size()) + ":";
    for (size_t i = 0; i < dec.size(); ++i)
        if (i < 4 || i + 3 >= dec.size()) s += " [" + std::to_string(i) + "]" + Fmt(dec[i]);
    test::Out(s + "\n");
}

}  // namespace

ATHER_TEST(diag_runner_frame_times) {
    const std::wstring dir = test::TempDir();
    {
        Mp4Writer mw;
        const HRESULT hr = mw.Begin(dir + L"\\probe.mp4", 320, 180, 60);
        test::Out("encoder: " + std::string(mw.HardwareEncoder() ? "hardware" : "software") + ", Begin hr " + std::to_string((long)hr) + "\n");
        std::vector<uint32_t> px((size_t)320 * 180, 0xFF336699u);
        for (int i = 0; i < 3; ++i) mw.WriteFrame(px.data(), std::llround(i * 1e7 / 60), std::llround(1e7 / 60));
        mw.Finalize();
    }
    const std::wstring a = dir + L"\\steady60.mp4", b = dir + L"\\steady30-tone.mp4";
    CHECK(WriteTestClip(a, 320, 180, 60, 2, false));
    CHECK(WriteTestClip(b, 320, 180, 30, 2, true));
    Dump(a, 60);
    Dump(b, 30);
    wchar_t out[MAX_PATH];
    if (GetEnvironmentVariableW(L"ATHER_TEST_OUT", out, MAX_PATH)) {
        const std::wstring o = std::wstring(out) + L"\\diag";
        SHCreateDirectoryExW(nullptr, o.c_str(), nullptr);
        CopyFileW(a.c_str(), (o + L"\\steady60.mp4").c_str(), FALSE);
        CopyFileW(b.c_str(), (o + L"\\steady30-tone.mp4").c_str(), FALSE);
    }
}

}  // namespace ather
