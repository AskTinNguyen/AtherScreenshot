// Developer tool: times the video export on real recordings, and checks that a faster export still makes the
// same pictures.
//
//   AtherScreenshot.exe --bench-export <outDir> [tap] <clip>...
//     Exports each clip in a few typical edits and prints how long each took. With `tap`, also hashes every frame
//     the export encodes and keeps every 30th one (raw BGRA) in <outDir>, for --bench-compare.
//   AtherScreenshot.exe --bench-compare <dirA> <dirB>
//     Compares two tapped runs: identical frames, and the PSNR of the kept frames that differ.
// Reads the clips only; everything it writes goes to <outDir>. Environment switches, for comparing: ATHER_ENCODERS=n
// (encoders at once), ATHER_NO_GPU_DECODE=1 (Media Foundation's software decoder), ATHER_BENCH_ONLY=<edit name>.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

#include "json.h"
#include "media.h"
#include "selftest.h"
#include "videoio.h"

// After common.h (in media.h), which sets up windows.h.
#include <objbase.h>
#include <psapi.h>

#pragma comment(lib, "psapi")

namespace ather {

namespace {

void Say(const std::string& s) {
    test::Out(s);
    test::FlushOut();
}

std::wstring BaseName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    std::wstring n = s == std::wstring::npos ? p : p.substr(s + 1);
    if (const size_t d = n.find_last_of(L'.'); d != std::wstring::npos) n.resize(d);
    return n;
}

struct Scenario {
    const wchar_t* name;
    bool gif;
    VideoEdit edit;
};

Mark M(MarkKind k, double x0, double y0, double x1, double y1, double start, double end, AnimStyle st = AnimStyle::Auto, std::wstring text = L"") {
    Mark m;
    m.kind = k;
    m.a = {x0, y0};
    m.b = {x1, y1};
    m.start = start;
    m.end = end;
    m.style = st;
    m.text = std::move(text);
    m.color = 0;
    return m;
}

// Marks and captions like a typical tutorial edit, placed in a W × H frame from `t0` on.
void AddMarkup(VideoEdit& e, double W, double H, double t0, double len) {
    auto at = [&](double f) { return t0 + len * f; };
    e.marks.push_back(M(MarkKind::Title, 0, 0, W, H, at(0), at(0.12), AnimStyle::Auto, L"Release 1.2"));
    e.marks.back().subtitle = L"What's new";
    e.marks.push_back(M(MarkKind::Box, W * 0.1, H * 0.2, W * 0.4, H * 0.45, at(0.1), at(0.4), AnimStyle::DrawOn));
    e.marks.push_back(M(MarkKind::Arrow, W * 0.7, H * 0.8, W * 0.45, H * 0.5, at(0.15), at(0.45), AnimStyle::DrawOn));
    e.marks.push_back(M(MarkKind::Text, W * 0.55, H * 0.1, W * 0.9, H * 0.2, at(0.2), at(0.6), AnimStyle::Typewriter, L"Click Deploy to ship it"));
    e.marks.push_back(M(MarkKind::Blur, W * 0.6, H * 0.6, W * 0.85, H * 0.75, at(0.05), at(0.95)));
    e.marks.push_back(M(MarkKind::Pixelate, W * 0.05, H * 0.75, W * 0.3, H * 0.92, at(0.3), at(0.6)));
    e.marks.push_back(M(MarkKind::Zoom, W * 0.3, H * 0.3, W * 0.5, H * 0.5, at(0.55), at(0.75)));
    Mark emoji = M(MarkKind::Emoji, W * 0.8, H * 0.3, W * 0.8 + H * 0.1, H * 0.4, at(0.4), at(0.8), AnimStyle::Pop, L"✅");
    emoji.emphasis = Emphasis::Ping;
    e.marks.push_back(emoji);
    Mark bubble = M(MarkKind::Bubble, W * 0.2, H * 0.55, W * 0.45, H * 0.65, at(0.6), at(0.9), AnimStyle::Pop, L"Saved!");
    bubble.emphasis = Emphasis::Pulse;
    e.marks.push_back(bubble);
    const wchar_t* lines[] = {L"Open the project settings", L"Pick the build target", L"Then click deploy",
                              L"Wait for the green check", L"That's it, it's live", L"Thanks for watching"};
    for (int i = 0; i < 6; ++i) {
        Caption c;
        c.start = at(i / 6.0);
        c.end = at((i + 0.9) / 6.0);
        c.text = lines[i];
        e.captions.push_back(c);
    }
}

std::vector<Scenario> Scenarios(const Clip& c) {
    const double D = c.length, W = c.w, H = c.h;
    std::vector<Scenario> out;
    {
        Scenario s{L"plain", false, {}};
        s.edit.trimEnd = std::min(D, 20.0);
        out.push_back(s);
    }
    {
        Scenario s{L"edits", false, {}};
        s.edit.trimStart = std::min(1.0, D / 10);
        s.edit.trimEnd = std::min(D, s.edit.trimStart + 20);
        s.edit.crop = VRect{W * 0.05, H * 0.05, W * 0.9, H * 0.9};
        AddMarkup(s.edit, W, H, s.edit.trimStart, s.edit.trimEnd - s.edit.trimStart);
        out.push_back(s);
    }
    {
        Scenario s{L"speed2", false, {}};
        s.edit.trimEnd = std::min(D, 30.0);
        s.edit.speed = 2;
        out.push_back(s);
    }
    {  // captions all along, nothing else
        Scenario s{L"captions", false, {}};
        s.edit.trimEnd = std::min(D, 20.0);
        const wchar_t* lines[] = {L"Open the project settings", L"Pick the build target", L"Then click deploy",
                                  L"Wait for the green check", L"That's it, it's live", L"Thanks for watching"};
        for (int i = 0; i < 6; ++i) {
            Caption cap;
            cap.start = s.edit.trimEnd * i / 6;
            cap.end = s.edit.trimEnd * (i + 0.95) / 6;
            cap.text = lines[i];
            s.edit.captions.push_back(cap);
        }
        out.push_back(s);
    }
    {  // a typical tutorial: captions all along, a box and an arrow now and then, a small blur hiding an address
        Scenario s{L"light", false, {}};
        s.edit.trimEnd = std::min(D, 20.0);
        const double T = s.edit.trimEnd;
        const wchar_t* lines[] = {L"Open the project settings", L"Pick the build target", L"Then click deploy",
                                  L"Wait for the green check", L"That's it, it's live", L"Thanks for watching"};
        for (int i = 0; i < 6; ++i) {
            Caption cap;
            cap.start = T * i / 6;
            cap.end = T * (i + 0.95) / 6;
            cap.text = lines[i];
            s.edit.captions.push_back(cap);
        }
        s.edit.marks.push_back(M(MarkKind::Box, W * 0.1, H * 0.2, W * 0.3, H * 0.3, T * 0.1, T * 0.3, AnimStyle::DrawOn));
        s.edit.marks.push_back(M(MarkKind::Arrow, W * 0.6, H * 0.6, W * 0.45, H * 0.45, T * 0.5, T * 0.7, AnimStyle::DrawOn));
        s.edit.marks.push_back(M(MarkKind::Blur, W * 0.7, H * 0.06, W * 0.85, H * 0.1, 0, T));
        out.push_back(s);
    }
    if (D >= 40) {  // a whole long recording, as it is
        Scenario s{L"long", false, {}};
        s.edit.trimEnd = D;
        out.push_back(s);
    }
    {
        Scenario s{L"gif", true, {}};
        s.edit.trimEnd = std::min(D, 8.0);
        AddMarkup(s.edit, W, H, 0, s.edit.trimEnd);
        out.push_back(s);
    }
    return out;
}

double CpuSeconds() {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto s = [](FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return s(k) + s(u);
}

uint64_t Hash(const Bitmap& b) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t* p = reinterpret_cast<const uint64_t*>(b.Bits());
    const size_t n = (size_t)b.Width() * b.Height() / 2;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

std::vector<uint8_t> ReadAll(const std::wstring& path) {
    std::vector<uint8_t> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return v;
    fseek(f, 0, SEEK_END);
    v.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    v.resize(fread(v.data(), 1, v.size(), f));
    fclose(f);
    return v;
}

int Bench(const std::vector<std::wstring>& args) {
    if (args.size() < 2) return 2;
    const std::wstring dir = args[0];
    CreateDirectoryW(dir.c_str(), nullptr);
    size_t first = 1;
    const bool tap = args[1] == L"tap";
    if (tap) ++first;
    double total = 0;
    for (size_t a = first; a < args.size(); ++a) {
        const auto clip = ClipOf(args[a]);
        if (!clip) {
            Say("can't open " + ToUtf8(args[a]) + "\n");
            return 1;
        }
        char head[256];
        sprintf_s(head, "%s  %dx%d  %.0f fps  %.1f s%s\n", ToUtf8(BaseName(args[a])).c_str(), clip->w, clip->h, clip->fps, clip->length,
                  clip->hasAudio ? "  audio" : "");
        Say(head);
        wchar_t only[32] = L"";
        GetEnvironmentVariableW(L"ATHER_BENCH_ONLY", only, 32);  // one scenario (e.g. to see its peak memory)
        for (const auto& s : Scenarios(*clip)) {
            if (*only && wcscmp(only, s.name) != 0) continue;
            const std::wstring tag = BaseName(args[a]) + L"_" + s.name;
            const std::wstring out = dir + L"\\" + tag + (s.gif ? L".gif" : L".mp4");
            std::vector<uint64_t> hashes;
            std::mutex hashMu;  // an export in pieces taps from several threads
            int frames = 0;
            if (tap)
                g_exportTap = [&](int i, const Bitmap& b) {
                    const uint64_t h = Hash(b);
                    {
                        std::lock_guard l(hashMu);
                        if (hashes.size() <= (size_t)i) hashes.resize((size_t)i + 1);
                        hashes[i] = h;
                    }
                    if (i % 30 == 0) {
                        std::vector<uint8_t> raw(8 + (size_t)b.Width() * b.Height() * 4);
                        const int32_t wh[2] = {b.Width(), b.Height()};
                        memcpy(raw.data(), wh, 8);
                        memcpy(raw.data() + 8, b.Bits(), raw.size() - 8);
                        WriteAll(dir + L"\\" + tag + L"_f" + std::to_wstring(i) + L".raw", raw.data(), raw.size());
                    }
                };
            std::wstring err;
            const double cpu0 = CpuSeconds();
            const auto start = std::chrono::steady_clock::now();
            VideoEdit e = s.edit;
            e.clips = {*clip};
            const bool done = s.gif ? ExportGif(L"", e, out, &err, 12, [&](double) { ++frames; return true; })
                                    : ExportMp4(L"", e, out, &err, [&](double) { ++frames; return true; });
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double cpu = CpuSeconds() - cpu0;
            g_exportTap = nullptr;
            total += secs;
            char line[256];
            sprintf_s(line, "  %-7s %7.2f s  %5d frames  %7.1f fps  cpu %6.2f s%s%s\n", ToUtf8(s.name).c_str(), secs, frames, frames / std::max(1e-9, secs), cpu,
                      done ? "" : "  FAILED: ", done ? "" : ToUtf8(err).c_str());
            Say(line);
            if (tap) {
                std::string text;
                for (uint64_t h : hashes) text += std::to_string(h) + "\n";
                WriteAll(dir + L"\\" + tag + L".hashes", text.data(), text.size());
            }
        }
    }
    PROCESS_MEMORY_COUNTERS pmc{sizeof(pmc)};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    char line[96];
    sprintf_s(line, "total %.2f s, peak memory %.0f MB\n", total, pmc.PeakWorkingSetSize / 1048576.0);
    Say(line);
    return 0;
}

int Compare(const std::wstring& a, const std::wstring& b) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((a + L"\\*.hashes").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 2;
    int bad = 0;
    do {
        const std::wstring tag = std::wstring(fd.cFileName).substr(0, wcslen(fd.cFileName) - 7);
        auto ha = ReadAll(a + L"\\" + fd.cFileName), hb = ReadAll(b + L"\\" + fd.cFileName);
        const std::string sa(ha.begin(), ha.end()), sb(hb.begin(), hb.end());
        const auto na = std::count(sa.begin(), sa.end(), '\n'), nb = std::count(sb.begin(), sb.end(), '\n');
        int same = 0;
        for (size_t i = 0, j = 0; i < sa.size() && j < sb.size();) {
            const size_t ei = sa.find('\n', i), ej = sb.find('\n', j);
            same += sa.compare(i, ei - i, sb, j, ej - j) == 0;
            i = ei + 1;
            j = ej + 1;
        }
        double worst = 1e9;
        int maxDiff = 0;
        double off = 0, all = 0;
        for (int i = 0; i < 100000; i += 30) {
            const std::wstring f = L"\\" + tag + L"_f" + std::to_wstring(i) + L".raw";
            auto ra = ReadAll(a + f), rb = ReadAll(b + f);
            if (ra.empty() && rb.empty()) break;
            if (ra.size() != rb.size()) {
                worst = 0;
                continue;
            }
            double se = 0;
            size_t n = 0;
            for (size_t k = 8; k < ra.size(); k += 4)
                for (int c = 0; c < 3; ++c, ++n) {
                    const double d = (double)ra[k + c] - rb[k + c];
                    se += d * d;
                    maxDiff = std::max(maxDiff, (int)std::fabs(d));
                    off += std::fabs(d) > 1;
                    all += 1;
                }
            const double psnr = se == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / (se / n));
            worst = std::min(worst, psnr);
        }
        char line[256];
        sprintf_s(line, "%-28s frames %lld/%lld  identical %d  worst kept-frame PSNR %.1f dB, max diff %d, off by >1: %.4f%%\n", ToUtf8(tag).c_str(), (long long)na, (long long)nb, same,
                  worst == 1e9 ? 99.0 : worst, maxDiff, all ? off * 100 / all : 0.0);
        Say(line);
        bad += na != nb || worst < 40;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return bad;
}

}  // namespace

int VideoBench(const std::vector<std::wstring>& args) {
    int code = 0;
    if (wchar_t v[8]; GetEnvironmentVariableW(L"ATHER_ENCODERS", v, 8)) g_exportEncoders = std::max(1, _wtoi(v));
    g_noGpuDecode = GetEnvironmentVariableW(L"ATHER_NO_GPU_DECODE", nullptr, 0) > 0;
    std::thread([&] {  // like the editor's save: a worker thread in the multithreaded apartment
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (!args.empty() && args[0] == L"--bench-compare") code = args.size() == 3 ? Compare(args[1], args[2]) : 2;
        else code = Bench(std::vector<std::wstring>(args.begin() + 1, args.end()));
        CoUninitialize();
    }).join();
    return code;
}

}  // namespace ather
