#include "bugreport.h"

#include <sapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "audio.h"
#include "capture.h"
#include "json.h"
#include "library.h"
#include "media.h"
#include "output.h"
#include "selftest.h"
#include "textdraw.h"
#include "toast.h"
#include "videoio.h"

#pragma comment(lib, "sapi")

using Microsoft::WRL::ComPtr;

namespace ather {

namespace {

// A word as the commands compare it: lower case, letters and digits only.
std::wstring Bare(const std::wstring& w) {
    std::wstring s;
    for (wchar_t c : w)
        if (iswalnum(c)) s += (wchar_t)towlower(c);
    return s;
}

BugCommand CommandWord(const std::wstring& bare) {
    if (bare == L"start") return BugCommand::Start;
    if (bare == L"stop") return BugCommand::Stop;
    if (bare == L"cancel") return BugCommand::Cancel;
    return BugCommand::None;
}

double NowEpoch() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
}

double EpochOf(const FILETIME& ft) {
    return ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
}

std::wstring LocalTime(double epoch, const wchar_t* format) {
    const time_t t = (time_t)epoch;
    tm local{};
    if (localtime_s(&local, &t)) return L"";
    wchar_t buf[64];
    return wcsftime(buf, std::size(buf), format, &local) ? buf : L"";
}

std::string IsoUtc(double epoch) {
    const time_t t = (time_t)epoch;
    tm u{};
    if (gmtime_s(&u, &t)) return "";
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
}

// Recording time as people read it: "1:23.4", "1:02:03.4".
std::wstring Timecode(double s) {
    s = std::max(0.0, s);
    const int tenths = (int)std::lround(s * 10);
    const int h = tenths / 36000, m = tenths / 600 % 60, sec = tenths / 10 % 60, t = tenths % 10;
    return h ? std::format(L"{}:{:02}:{:02}.{}", h, m, sec, t) : std::format(L"{}:{:02}.{}", m, sec, t);
}

std::wstring Size(uint64_t bytes) {
    if (bytes >= (1ull << 20)) return std::format(L"{:.1f} MB", bytes / 1048576.0);
    if (bytes >= 1024) return std::format(L"{} KB", (bytes + 512) / 1024);
    return std::format(L"{} bytes", bytes);
}

double Round(double v, double unit = 100) { return std::round(v * unit) / unit; }

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool WriteText(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (iswspace(s.back()) || s.back() == L'"')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (iswspace(s[a]) || s[a] == L'"')) ++a;
    return s.substr(a);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

}  // namespace

// ---- the bug logic ----

double BugClock() { return GetTickCount64() / 1000.0; }

BugCommand FindBugCommand(const std::vector<CaptionWord>& words, size_t from, size_t* at) {
    for (size_t i = from; i < words.size(); ++i) {
        const std::wstring w = Bare(words[i].text);
        if (w != L"bug" || i + 1 == words.size()) continue;
        if (const BugCommand c = CommandWord(Bare(words[i + 1].text)); c != BugCommand::None) {
            if (at) *at = i;
            return c;
        }
    }
    if (at) *at = words.size();
    return BugCommand::None;
}

std::wstring Bug::Description() const {
    std::wstring s;
    for (const auto& w : words) s += (s.empty() ? L"" : L" ") + w.text;
    return s;
}

BugEvent BugTracker::Begin(double t) {
    Bug b;
    b.id = nextId_++;
    b.start = t;
    // The recording running then: the newest sample up to `t` (a voice command is heard a moment after it was said).
    const Sample* s = samples_.empty() ? nullptr : &samples_.front();
    for (const auto& x : samples_)
        if (x.t <= t + 1e-9) s = &x;
    if (s) b.recording = s->recording;
    open_ = b;
    return {BugEvent::Started, b};
}

void BugTracker::Close(double t) {
    open_->stop = std::max(t, open_->start);
    closing_ = std::move(open_);
    open_.reset();
}

BugEvent BugTracker::Finish() {
    Bug b = std::move(*closing_);
    closing_.reset();
    if (b.recording) {
        b.recStart = RecordingTime(b.recording, b.start);
        b.recStop = std::max(b.recStart, RecordingTime(b.recording, b.stop));
        for (CaptionWord w : b.words) {
            const double end = RecordingTime(b.recording, w.end);
            w.start = RecordingTime(b.recording, w.start);
            w.end = std::max(w.start + 0.05, end);
            b.recWords.push_back(w);
        }
    }
    return {BugEvent::Stopped, b};
}

std::vector<BugEvent> BugTracker::Heard(const std::vector<CaptionWord>& words) {
    std::vector<BugEvent> out;
    for (size_t i = 0;;) {
        size_t at = 0;
        const BugCommand c = FindBugCommand(words, i, &at);
        for (size_t k = i; k < at; ++k) {
            if (open_ && words[k].start >= open_->start - 1e-6) open_->words.push_back(words[k]);
            else if (!open_ && closing_ && words[k].start < closing_->stop) closing_->words.push_back(words[k]);  // said before the shortcut
        }
        if (c == BugCommand::None) break;
        const double t = words[at].start;
        if (c == BugCommand::Start) {
            if (open_) {
                out.push_back({BugEvent::Ignored, *open_});
            } else {
                if (closing_) out.push_back(Finish());
                out.push_back(Begin(t));
            }
        } else if (open_ && c == BugCommand::Stop) {
            if (closing_) out.push_back(Finish());
            Close(t);
            out.push_back(Finish());  // the words before it were heard first: nothing is on its way
        } else if (open_ && c == BugCommand::Cancel) {
            out.push_back({BugEvent::Cancelled, *open_});
            open_.reset();
        }
        i = at + 2;
    }
    return out;
}

std::vector<BugEvent> BugTracker::Toggle(double t) {
    std::vector<BugEvent> out;
    if (open_) {
        if (closing_) out.push_back(Finish());
        Close(t);
    } else {
        out.push_back(Begin(t));
    }
    return out;
}

std::vector<BugEvent> BugTracker::Tick(double t, uint64_t recording, double recSeconds) {
    samples_.push_back({t, recording, recSeconds});
    while (samples_.size() > 2 && samples_.front().t < t - 2 * kMaxSeconds) samples_.erase(samples_.begin());
    std::vector<BugEvent> out;
    if (open_ && t - open_->start >= kMaxSeconds) {
        if (closing_) out.push_back(Finish());
        open_->autoStopped = true;
        Close(open_->start + kMaxSeconds);
    }
    if (closing_ && t >= closing_->stop + kGrace) out.push_back(Finish());
    return out;
}

double BugTracker::RecordingTime(uint64_t recording, double t) const {
    const Sample *before = nullptr, *after = nullptr;
    for (const auto& s : samples_) {
        if (s.recording != recording) continue;
        if (s.t <= t) before = &s;
        else if (!after) after = &s;
    }
    if (before && after) return before->rec + std::clamp(t - before->t, 0.0, after->rec - before->rec);  // flat while paused
    if (before) {  // on from the last sample, but not past the recording's end (the first sample without it)
        double until = t;
        for (const auto& s : samples_)
            if (s.t > before->t && s.recording != recording) {
                until = std::min(t, s.t);
                break;
            }
        return before->rec + (until - before->t);
    }
    if (after) return std::max(0.0, after->rec - (after->t - t));
    return 0;
}

// ---- logs ----

std::vector<std::wstring> FindBugLogs(const std::wstring& paths, double now, std::vector<std::wstring>* missing) {
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;
    auto add = [&](const std::wstring& p) {
        if (seen.insert(Lower(p)).second) out.push_back(p);
    };
    for (size_t start = 0; start <= paths.size();) {
        size_t semi = paths.find(L';', start);
        if (semi == std::wstring::npos) semi = paths.size();
        const std::wstring entry = Trim(paths.substr(start, semi - start));
        start = semi + 1;
        if (entry.empty()) continue;
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = ExpandEnvironmentStringsW(entry.c_str(), buf, (DWORD)std::size(buf));
        std::wstring path = n && n <= std::size(buf) ? buf : entry;
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            add(path);
            continue;
        }
        std::wstring dir, pattern;
        if (attr != INVALID_FILE_ATTRIBUTES) {
            dir = path;
            pattern = path + L"\\*";
        } else if (path.find_first_of(L"*?") != std::wstring::npos) {
            const size_t slash = path.find_last_of(L"\\/");
            dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
            pattern = path;
        } else {
            if (missing) missing->push_back(entry);
            continue;
        }
        struct File {
            std::wstring path;
            double mtime;
        };
        std::vector<File> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring p = dir + L"\\" + fd.cFileName;
                // The listing's time can lag for a file that's open for writing; the file's own is current.
                WIN32_FILE_ATTRIBUTE_DATA fa{};
                const FILETIME ft = GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) ? fa.ftLastWriteTime : fd.ftLastWriteTime;
                files.push_back({p, EpochOf(ft)});
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (files.empty()) {
            if (missing) missing->push_back(entry);
            continue;
        }
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.mtime > b.mtime; });
        const size_t recent = (size_t)std::count_if(files.begin(), files.end(), [&](const File& f) { return f.mtime >= now - 30 * 60; });
        for (size_t i = 0; i < std::min<size_t>(recent ? recent : 1, 20); ++i) add(files[i].path);
    }
    return out;
}

std::vector<BugLog> CopyBugLogs(const std::vector<std::wstring>& files, const std::wstring& dir, uint64_t maxBytes) {
    std::vector<BugLog> out;
    if (files.empty()) return out;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::set<std::wstring> names;
    std::vector<char> buf(1 << 20);
    for (const auto& src : files) {
        BugLog l;
        l.source = src;
        std::wstring name = FileNameOf(src);
        const size_t dot = name.find_last_of(L'.');
        const std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot), ext = dot == std::wstring::npos ? L"" : name.substr(dot);
        for (int k = 2; names.count(Lower(name)); ++k) name = stem + L" (" + std::to_wstring(k) + L")" + ext;
        names.insert(Lower(name));
        // Shared read and write (and delete): the game keeps writing its log.
        HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            l.error = std::format(L"couldn't be read (error {})", GetLastError());
            out.push_back(l);
            continue;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(in, &size);
        l.size = (uint64_t)size.QuadPart;
        uint64_t from = 0;
        if (l.size > maxBytes) {  // keep the end, from the start of a line
            l.truncated = true;
            from = l.size - maxBytes;
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)from;
            DWORD got = 0;
            if (SetFilePointerEx(in, pos, nullptr, FILE_BEGIN) && ReadFile(in, buf.data(), (DWORD)std::min<uint64_t>(64 << 10, maxBytes), &got, nullptr))
                if (const char* nl = (const char*)memchr(buf.data(), '\n', got)) from += (uint64_t)(nl - buf.data()) + 1;
        }
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)from;
        SetFilePointerEx(in, pos, nullptr, FILE_BEGIN);
        const std::wstring dst = dir + L"\\" + name;
        HANDLE o = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (o == INVALID_HANDLE_VALUE) {
            l.error = std::format(L"couldn't be copied (error {})", GetLastError());
            CloseHandle(in);
            out.push_back(l);
            continue;
        }
        const uint64_t want = l.size - from;  // up to the size seen: the game may still be adding to it
        while (l.copied < want) {
            DWORD got = 0, wrote = 0;
            if (!ReadFile(in, buf.data(), (DWORD)std::min<uint64_t>(buf.size(), want - l.copied), &got, nullptr) || !got) break;
            if (!WriteFile(o, buf.data(), got, &wrote, nullptr) || wrote != got) break;
            l.copied += got;
        }
        CloseHandle(o);
        CloseHandle(in);
        l.file = L"logs\\" + name;
        out.push_back(l);
    }
    return out;
}

// ---- the picture ----

BitmapPtr DrawBugOverlay(const Bitmap& shot, const std::wstring& header, const std::wstring& description, BugOverlay* layout) {
    const int W = shot.Width(), H = shot.Height();
    BitmapPtr out = shot.Crop({0, 0, W, H});
    if (!out) return nullptr;
    // Sized to the picture like captions are to a video, and still readable when a chat shows it small.
    const float fs = std::max(14.f, std::min(H * 0.034f, W / 40.f)), pad = fs * 0.75f, width = std::max(1.f, W - 2 * pad);
    textdraw::Style hs;
    hs.size = fs * 0.62f;
    hs.weight = 700;
    hs.center = false;
    hs.color = theme::kTextDim;
    if (header.rfind(L"BUG", 0) == 0) hs.ranges.push_back({0, 3, theme::kAccent});
    textdraw::Style ds;
    ds.size = fs;
    ds.weight = 600;
    ds.center = false;
    ds.color = RGB(255, 255, 255);
    std::wstring text;
    for (wchar_t c : description) {  // one paragraph: line breaks become spaces
        if (iswspace(c)) c = L' ';
        if (c != L' ' || (!text.empty() && text.back() != L' ')) text += c;
    }
    while (!text.empty() && text.back() == L' ') text.pop_back();
    const float lineH = textdraw::Measure(L"Ag", ds, 1e6f).h;
    BugOverlay lay;
    float textH = 0;
    if (!text.empty()) {
        auto height = [&](const std::wstring& t) { return textdraw::Measure(t, ds, width).h; };
        const float maxH = lineH * (BugOverlay::kMaxLines + 0.5f);
        if (height(text) > maxH) {  // the most whole words that fit with "…"
            lay.cut = true;
            std::vector<size_t> ends;
            for (size_t i = 1; i <= text.size(); ++i)
                if (i == text.size() || text[i] == L' ') ends.push_back(i);
            size_t lo = 0, hi = ends.size();
            while (lo < hi) {
                const size_t mid = (lo + hi + 1) / 2;
                if (height(text.substr(0, ends[mid - 1]) + L"…") <= maxH) lo = mid;
                else hi = mid - 1;
            }
            text = (lo ? text.substr(0, ends[lo - 1]) : std::wstring()) + L"…";
        }
        textH = height(text);
        lay.lines = std::max(1, (int)std::lround(textH / std::max(1.f, lineH)));
    }
    const float headH = textdraw::Measure(header, hs, width).h, gap = text.empty() ? 0 : fs * 0.3f;
    const int top = std::clamp((int)std::floor(H - (pad + headH + gap + textH + pad)), 0, H);
    textdraw::FillRounded(*out, 0, (float)top, (float)W, (float)(H - top), 0, RGB(0, 0, 0), 0.72f);
    textdraw::Draw(*out, header, hs, pad, top + pad, width);
    if (!text.empty()) textdraw::Draw(*out, text, ds, pad, top + pad + headH + gap, width);
    for (size_t i = 0, n = (size_t)W * H; i < n; ++i) out->Bits()[i] |= 0xFF000000u;  // a screenshot stays opaque
    lay.bar = {0, top, W, H};
    if (layout) *layout = lay;
    return out;
}

// ---- the bundle ----

std::wstring BugBundle::Title() const {
    std::wstring d = bug.Description();
    size_t end = d.size();
    for (size_t i = 0; i < d.size(); ++i)
        if ((d[i] == L'.' || d[i] == L'!' || d[i] == L'?') && (i + 1 == d.size() || d[i + 1] == L' ')) {
            end = d[i] == L'.' ? i : i + 1;
            break;
        }
    std::wstring t = Trim(d.substr(0, end));
    if (t.size() > 80) {
        const size_t sp = t.rfind(L' ', 80);
        t = t.substr(0, sp == std::wstring::npos || sp < 40 ? 80 : sp) + L"…";
    }
    if (t.empty()) return L"Bug at " + LocalTime(wallStart, L"%H:%M:%S");
    t[0] = (wchar_t)towupper(t[0]);
    return t;
}

std::wstring BugBundle::Header() const {
    return L"BUG · " + LocalTime(wallStart, L"%Y-%m-%d %H:%M:%S") + (app.empty() ? L"" : L" · " + app);
}

std::wstring BugFolderName(double wallStart, const std::wstring& description) {
    const std::wstring stamp = LocalTime(wallStart, L"%Y-%m-%d %H-%M-%S");
    std::wstring words;
    for (wchar_t c : description) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L' ';
        if (c == L' ' && (words.empty() || words.back() == L' ')) continue;
        words += c;
    }
    if (words.size() > 40) {
        const size_t sp = words.rfind(L' ', 40);
        words = words.substr(0, sp == std::wstring::npos || sp < 20 ? 40 : sp);
    }
    while (!words.empty() && (words.back() == L' ' || words.back() == L'.')) words.pop_back();
    return words.empty() ? stamp : stamp + L" " + words;
}

namespace {

std::wstring ClipLine(const BugBundle& b) {
    switch (b.clip) {
        case BugBundle::Clip::Saved:
            return L"`clip.mp4`, " + Timecode(b.clipFrom) + L" to " + Timecode(b.clipTo) + L" of the recording, with captions";
        case BugBundle::Clip::Pending: return L"when the recording stops";
        case BugBundle::Clip::Failed: return L"couldn't be made: " + b.clipNote;
        default: return L"none: " + (b.clipNote.empty() ? std::wstring(L"nothing was being recorded") : b.clipNote);
    }
}

std::vector<std::wstring> BundleFiles(const BugBundle& b) {
    std::vector<std::wstring> f;
    for (const wchar_t* n : {L"bug.png", L"screenshot.png", L"clip.mp4", L"voice.wav"})
        if (Exists(b.folder + L"\\" + n)) f.push_back(n);
    for (const auto& l : b.logs)
        if (!l.file.empty()) f.push_back(l.file);
    f.push_back(L"bug.md");
    f.push_back(L"bug.json");
    return f;
}

}  // namespace

std::string BugMarkdown(const BugBundle& b) {
    const Bug& g = b.bug;
    const double seconds = std::max(0.0, g.stop - g.start);
    std::wstring s = L"# " + b.Title() + L"\n\n";
    s += L"- **When:** " + LocalTime(b.wallStart, L"%Y-%m-%d %H:%M:%S") + std::format(L", for {:.0f} s", seconds) +
         (g.autoStopped ? L" (it stopped itself after 2 minutes)" : L"") + L"\n";
    if (!b.app.empty() || !b.window.empty())
        s += L"- **App:** " + (b.app.empty() ? std::wstring(L"?") : b.app) + (b.window.empty() ? L"" : L", “" + b.window + L"”") + L"\n";
    if (g.recording)
        s += L"- **Recording:** " + (b.recordingPath.empty() ? std::wstring(L"the screen recording") : L"`" + FileNameOf(b.recordingPath) + L"`") +
             L", " + Timecode(g.recStart) + L" to " + Timecode(g.recStop) + L"\n";
    s += L"- **Clip:** " + ClipLine(b) + L"\n\n";
    s += L"## What was said\n\n";
    const std::wstring d = g.Description();
    s += d.empty() ? (b.voice ? L"Nothing was recognized: listen to `voice.wav`." : L"Nothing: voice commands were off.") : d;
    s += L"\n\n## Files\n\n";
    if (Exists(b.folder + L"\\bug.png")) s += L"- `bug.png`: the screenshot with what was said\n";
    if (Exists(b.folder + L"\\screenshot.png")) s += L"- `screenshot.png`: the screenshot as it was\n";
    if (b.clip == BugBundle::Clip::Saved) s += L"- `clip.mp4`: the recording around the bug\n";
    if (b.voice) s += L"- `voice.wav`: what the microphone heard\n";
    for (const auto& l : b.logs) {
        if (!l.error.empty()) s += L"- Couldn't copy `" + l.source + L"`: " + l.error + L"\n";
        else
            s += L"- `" + l.file + L"`: from `" + l.source + L"`, " + Size(l.size) +
                 (l.truncated ? L" (only its last " + Size(l.copied) + L")" : L"") + L"\n";
    }
    for (const auto& m : b.missingLogs) s += L"- Not found: `" + m + L"`\n";
    return ToUtf8(s);
}

std::string BugJson(const BugBundle& b) {
    const Bug& g = b.bug;
    Json j = Json::Object();
    j.Set("app", "Ather Screenshot");
    j.Set("format", 1);
    j.Set("kind", "bug");
    j.Set("id", b.id);
    j.Set("title", b.Title());
    j.Set("description", g.Description());
    j.Set("started", IsoUtc(b.wallStart));
    j.Set("started_local", LocalTime(b.wallStart, L"%Y-%m-%d %H:%M:%S"));
    j.Set("stopped", IsoUtc(b.wallStart + (g.stop - g.start)));
    j.Set("seconds", Round(g.stop - g.start));
    j.Set("auto_stopped", g.autoStopped);
    Json words = Json::Array();
    for (size_t i = 0; i < g.words.size(); ++i) {
        Json w = Json::Object();
        w.Set("text", g.words[i].text);
        w.Set("start", Round(g.words[i].start - g.start));  // seconds after "bug start"
        w.Set("end", Round(g.words[i].end - g.start));
        if (i < g.recWords.size()) w.Set("recording_time", Round(g.recWords[i].start));
        words.Push(std::move(w));
    }
    j.Set("words", std::move(words));
    Json fg = Json::Object();
    fg.Set("app", b.app);
    fg.Set("window", b.window);
    j.Set("foreground", std::move(fg));
    if (Exists(b.folder + L"\\screenshot.png")) {
        Json sh = Json::Object();
        sh.Set("file", "screenshot.png");
        sh.Set("overlay", "bug.png");
        Json area = Json::Object();
        area.Set("x", (int)b.area.left);
        area.Set("y", (int)b.area.top);
        area.Set("width", (int)RectW(b.area));
        area.Set("height", (int)RectH(b.area));
        sh.Set("area", std::move(area));
        j.Set("screenshot", std::move(sh));
    } else {
        j.Set("screenshot", nullptr);
    }
    if (g.recording) {
        Json r = Json::Object();
        if (b.recordingPath.empty()) r.Set("path", nullptr);
        else r.Set("path", b.recordingPath);
        r.Set("start", Round(g.recStart));
        r.Set("stop", Round(g.recStop));
        j.Set("recording", std::move(r));
    } else {
        j.Set("recording", nullptr);
    }
    Json clip = Json::Object();
    const char* states[] = {"none", "pending", "saved", "failed"};
    clip.Set("state", states[(int)b.clip]);
    if (b.clip == BugBundle::Clip::Saved) {
        clip.Set("file", "clip.mp4");
        clip.Set("from", Round(b.clipFrom));
        clip.Set("to", Round(b.clipTo));
        clip.Set("captions", !g.recWords.empty());
    }
    if (!b.clipNote.empty()) clip.Set("note", b.clipNote);
    else if (b.clip == BugBundle::Clip::None) clip.Set("note", "nothing was being recorded");
    j.Set("clip", std::move(clip));
    if (b.voice) j.Set("voice", "voice.wav");
    else j.Set("voice", nullptr);
    Json logs = Json::Array();
    for (const auto& l : b.logs) {
        Json e = Json::Object();
        e.Set("source", l.source);
        if (!l.file.empty()) e.Set("file", l.file);
        e.Set("size", (int64_t)l.size);
        e.Set("copied", (int64_t)l.copied);
        e.Set("truncated", l.truncated);
        if (l.truncated) e.Set("note", "Only the end of the log was kept: it was over the size limit ([Bugs] LogMaxMB).");
        if (!l.error.empty()) e.Set("error", l.error);
        logs.Push(std::move(e));
    }
    j.Set("logs", std::move(logs));
    Json missing = Json::Array();
    for (const auto& m : b.missingLogs) missing.Push(Json(m));
    j.Set("missing_logs", std::move(missing));
    Json files = Json::Array();
    for (const auto& f : BundleFiles(b)) files.Push(Json(f));
    j.Set("files", std::move(files));
    // One member per line, so it reads in a text editor too.
    std::string out = "{";
    const auto& m = j.Members();
    for (size_t i = 0; i < m.size(); ++i) {
        Json one = Json::Object();
        one.Set(m[i].first, m[i].second);
        const std::string kv = one.Dump();
        out += (i ? ",\n  " : "\n  ") + kv.substr(1, kv.size() - 2);
    }
    return out + "\n}\n";
}

bool WriteBugBundle(BugBundle& b, const std::vector<int16_t>& voice, const std::wstring& logPaths, uint64_t logMaxBytes, std::wstring* error) {
    const int made = SHCreateDirectoryExW(nullptr, b.folder.c_str(), nullptr);
    if (made != ERROR_SUCCESS) {
        if (error) *error = L"Couldn't make the folder " + b.folder;
        return false;
    }
    if (BitmapPtr shot = b.screenshot.valid() ? b.screenshot.get() : nullptr) {
        SavePng(*shot, b.folder + L"\\screenshot.png");
        if (BitmapPtr over = DrawBugOverlay(*shot, b.Header(), b.bug.Description())) SavePng(*over, b.folder + L"\\bug.png");
    }
    b.voice = !voice.empty() && WriteWav16(b.folder + L"\\voice.wav", voice, 16000, false);
    b.missingLogs.clear();
    b.logs = CopyBugLogs(FindBugLogs(logPaths, NowEpoch(), &b.missingLogs), b.folder + L"\\logs", logMaxBytes);
    const bool ok = WriteText(b.folder + L"\\bug.json", BugJson(b)) && WriteText(b.folder + L"\\bug.md", BugMarkdown(b));
    if (!ok && error) *error = L"Couldn't write bug.md in " + b.folder;
    return ok;
}

void FinishBugClip(BugBundle& b, const RecordResult& r, double preRoll) {
    static std::mutex notesLock;  // bugs of one recording finish one by one: their notes go into one sidecar
    if (!r.ok || r.discarded || r.format != RecordFormat::Mp4) {
        b.clip = BugBundle::Clip::None;
        b.clipNote = r.discarded ? L"the recording was discarded" : L"the recording wasn't saved";
    } else {
        b.recordingPath = r.path;
        VideoInfo vi;
        ProbeVideo(r.path, &vi);
        const double length = vi.duration > 0 ? vi.duration : r.seconds;
        b.clipFrom = std::clamp(b.bug.recStart - preRoll, 0.0, length);
        b.clipTo = std::clamp(b.bug.recStop, b.clipFrom, length);
        VideoEdit e;
        e.trimStart = b.clipFrom;
        e.trimEnd = b.clipTo;
        e.captions = ChunkCaptions(b.bug.recWords);
        std::wstring err;
        if (b.clipTo - b.clipFrom >= 0.1 && ExportMp4(r.path, e, b.folder + L"\\clip.mp4", &err)) {
            b.clip = BugBundle::Clip::Saved;
            b.clipNote.clear();
        } else {
            b.clip = BugBundle::Clip::Failed;
            b.clipNote = err.empty() ? L"the bug is past the end of the recording" : err;
        }
        Note n;
        n.src = b.bug.recStart;
        if (b.bug.recStop > b.bug.recStart) n.srcEnd = b.bug.recStop;
        n.kind = NoteKind::Issue;
        n.author = L"Bug report";
        n.text = L"Bug: " + (b.bug.words.empty() ? FileNameOf(b.folder) : b.bug.Description());
        n.created = (int64_t)time(nullptr);
        std::lock_guard l(notesLock);
        AddVideoNotes(r.path, {n}, vi.fps > 0 ? vi.fps : 30);
    }
    WriteText(b.folder + L"\\bug.json", BugJson(b));
    WriteText(b.folder + L"\\bug.md", BugMarkdown(b));
}

// ---- voice: SAPI commands and dictation, and the microphone for voice.wav ----

namespace {

void ClearSpEvent(SPEVENT& ev) {
    if (ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) reinterpret_cast<IUnknown*>(ev.lParam)->Release();
    else if ((ev.elParamType == SPET_LPARAM_IS_POINTER || ev.elParamType == SPET_LPARAM_IS_STRING) && ev.lParam) CoTaskMemFree((void*)ev.lParam);
    ev = {};
}

// The voice commands' recognizer: SAPI in-process with the default engine (as auto captions use), the command grammar
// ("bug start", "bug stop", "bug cancel") always on and dictation while a bug is open. Input: the default microphone, or
// a 16 kHz mono WAV (tests). Times: BugClock() for the microphone, the file's own seconds for a file.
class VoiceCommands {
public:
    struct Heard {
        std::vector<CaptionWord> words;
        bool command = false;  // from the command grammar (otherwise dictation)
        int confidence = 0;    // SP_LOW_CONFIDENCE, SP_NORMAL_CONFIDENCE or SP_HIGH_CONFIDENCE
    };

    ~VoiceCommands() {
        if (rec_) rec_->SetRecoState(SPRST_INACTIVE);
    }

    bool Open(const std::wstring& wav, std::wstring* error) {
        rec_.Attach(NewSpeechRecognizer(error));
        if (!rec_) return false;
        file_ = !wav.empty();
        HRESULT hr = S_OK;
        if (file_) {
            ComPtr<ISpStream> stream;
            WAVEFORMATEX wfx{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
            hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
            if (SUCCEEDED(hr)) hr = stream->BindToFile(wav.c_str(), SPFM_OPEN_READONLY, &SPDFID_WaveFormatEx, &wfx, SPFEI_ALL_EVENTS);
            if (SUCCEEDED(hr)) hr = rec_->SetInput(stream.Get(), TRUE);
        } else {  // Windows' default recording device
            ComPtr<ISpObjectTokenCategory> cat;
            ComPtr<ISpObjectToken> token;
            LPWSTR id = nullptr;
            hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat));
            if (SUCCEEDED(hr)) hr = cat->SetId(SPCAT_AUDIOIN, FALSE);
            if (SUCCEEDED(hr)) hr = cat->GetDefaultTokenId(&id);
            if (SUCCEEDED(hr) && !id) hr = E_FAIL;
            if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token));
            if (SUCCEEDED(hr)) hr = token->SetId(nullptr, id, FALSE);
            CoTaskMemFree(id);
            if (SUCCEEDED(hr)) hr = rec_->SetInput(token.Get(), TRUE);
            if (FAILED(hr)) {
                if (error) *error = L"There's no microphone to listen to (Settings › System › Sound › Input).";
                return false;
            }
        }
        if (SUCCEEDED(hr)) hr = rec_->CreateRecoContext(&ctx_);
        const ULONGLONG interest = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM);
        if (SUCCEEDED(hr)) hr = ctx_->SetNotifyWin32Event();
        if (SUCCEEDED(hr)) hr = ctx_->SetInterest(interest, interest);
        SPSTATEHANDLE top = nullptr;
        if (SUCCEEDED(hr)) hr = ctx_->CreateGrammar(kCommands, &cmd_);
        if (SUCCEEDED(hr)) hr = cmd_->GetRule(L"BugCommand", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &top);
        for (const wchar_t* phrase : {L"bug start", L"bug stop", L"bug cancel"})
            if (SUCCEEDED(hr)) hr = cmd_->AddWordTransition(top, nullptr, phrase, L" ", SPWT_LEXICAL, 1.0f, nullptr);
        if (SUCCEEDED(hr)) hr = cmd_->Commit(0);
        // Auto-pause: after a command the engine waits until dictation is switched to match, so the words right after
        // "bug start" are dictated, not lost.
        if (SUCCEEDED(hr)) autoPause_ = SUCCEEDED(cmd_->SetRuleState(nullptr, nullptr, SPRS_ACTIVE_WITH_AUTO_PAUSE));
        if (SUCCEEDED(hr) && !autoPause_) hr = cmd_->SetRuleState(nullptr, nullptr, SPRS_ACTIVE);
        if (SUCCEEDED(hr)) hr = ctx_->CreateGrammar(kDictation, &dict_);
        if (SUCCEEDED(hr)) hr = dict_->LoadDictation(nullptr, SPLO_STATIC);
        if (SUCCEEDED(hr)) hr = dict_->SetDictationState(SPRS_INACTIVE);
        if (SUCCEEDED(hr)) hr = rec_->SetRecoState(SPRST_ACTIVE_ALWAYS);
        if (FAILED(hr)) {
            if (error) *error = std::format(L"Speech recognition couldn't start (0x{:08X}).", (unsigned long)hr);
            ctx_.Reset();
            return false;
        }
        return true;
    }

    HANDLE Event() const { return ctx_ ? ctx_->GetNotifyEventHandle() : nullptr; }
    bool Dictation() const { return dictation_; }
    void SetDictation(bool on) {
        if (dict_ && on != dictation_ && SUCCEEDED(dict_->SetDictationState(on ? SPRS_ACTIVE : SPRS_INACTIVE))) dictation_ = on;
    }

    // What was recognized since the last call (waiting up to `waitMs` for something); false once a file has ended.
    // Commands from the microphone that the engine isn't sure of are left out: game chatter mustn't open a bug.
    bool Next(DWORD waitMs, std::vector<Heard>* out) {
        if (!ctx_ || ended_) return false;
        if (waitMs) ctx_->WaitForNotifyEvent(waitMs);
        SPEVENT ev{};
        ULONG got = 0;
        while (SUCCEEDED(ctx_->GetEvents(1, &ev, &got)) && got == 1) {
            if (ev.eEventId == SPEI_END_SR_STREAM) ended_ = true;
            if (ev.eEventId == SPEI_RECOGNITION && ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) {
                auto* r = reinterpret_cast<ISpRecoResult*>(ev.lParam);
                SPRECORESULTTIMES times{};
                SPPHRASE* ph = nullptr;
                if (SUCCEEDED(r->GetResultTimes(&times)) && SUCCEEDED(r->GetPhrase(&ph)) && ph) {
                    Heard h;
                    h.command = ph->ullGrammarID == kCommands;
                    h.confidence = ph->Rule.Confidence;
                    const double t0 = PhraseStart(times);
                    for (ULONG i = 0; i < ph->Rule.ulCountOfElements; ++i) {
                        const SPPHRASEELEMENT& el = ph->pElements[i];
                        if (!el.pszDisplayText || !*el.pszDisplayText) continue;
                        const double s = t0 + el.ulAudioTimeOffset / 1e7, e = std::max(s + 0.05, s + el.ulAudioSizeTime / 1e7);
                        if ((el.bDisplayAttributes & SPAF_CONSUME_LEADING_SPACES) && !h.words.empty()) {  // punctuation joins the word before
                            h.words.back().text += el.pszDisplayText;
                            h.words.back().end = std::max(h.words.back().end, e);
                        } else {
                            h.words.push_back({s, e, el.pszDisplayText});
                        }
                    }
                    CoTaskMemFree(ph);
                    const bool unsure = h.command && !file_ && h.confidence == SP_LOW_CONFIDENCE;
                    if (h.command && !unsure) {
                        const BugCommand c = FindBugCommand(h.words, 0, nullptr);
                        if (c != BugCommand::None) SetDictation(c == BugCommand::Start);
                    }
                    if (h.command && autoPause_) ctx_->Resume(0);
                    if (!unsure) out->push_back(std::move(h));
                }
            }
            ClearSpEvent(ev);
        }
        return !ended_;
    }

private:
    static constexpr ULONGLONG kCommands = 1, kDictation = 2;
    double PhraseStart(const SPRECORESULTTIMES& times) const {
        if (file_) return times.ullStart / 1e7;
        const ULONGLONG now = GetTickCount64();
        const DWORD age = (DWORD)now - times.dwTickCount;  // the tick count when the phrase began (32-bit, wraps)
        return age < 600000 ? (now - age) / 1000.0 : now / 1000.0;
    }
    ComPtr<ISpRecognizer> rec_;
    ComPtr<ISpRecoContext> ctx_;
    ComPtr<ISpRecoGrammar> cmd_, dict_;
    bool file_ = false, autoPause_ = false, dictation_ = false, ended_ = false;
};

// ---- MARKETING DEMO ONLY (branch intent/marketing-0.0.2.5, never merged) ----
// The GitHub runner that records the marketing video has no microphone. With ATHER_DEMO_VOICE_WAV set to a 16 kHz mono
// WAV (made with Windows text-to-speech), the listener hears that file instead of the microphone, as if it were played
// into it when "<wav>.go" appears: the same recognizer, grammar and dictation as the microphone path, results handed on
// at the moment each phrase ends in the file. voice.wav is cut from the same file.
std::wstring DemoVoiceWav() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"ATHER_DEMO_VOICE_WAV", buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}
std::atomic<double> g_demoVoiceT0{-1};  // BugClock() when the file "plays"
std::vector<int16_t> DemoVoicePcm() {
    std::vector<int16_t> pcm;
    HANDLE f = CreateFileW(DemoVoiceWav().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return pcm;
    std::string d;
    char chunk[65536];
    DWORD got = 0;
    while (ReadFile(f, chunk, sizeof chunk, &got, nullptr) && got) d.append(chunk, got);
    CloseHandle(f);
    for (size_t i = 12; i + 8 <= d.size();) {  // RIFF chunks: find "data"
        const uint32_t len = *(const uint32_t*)(d.data() + i + 4);
        if (d.compare(i, 4, "data") == 0) {
            const size_t n = std::min<size_t>(len, d.size() - i - 8) / 2;
            pcm.assign((const int16_t*)(d.data() + i + 8), (const int16_t*)(d.data() + i + 8) + n);
            break;
        }
        i += 8 + len + (len & 1);
    }
    return pcm;
}

// What the microphone hears while a bug is open, for voice.wav: WASAPI (as recordings use) at 16 kHz mono.
class VoiceTape {
public:
    ~VoiceTape() { Stop(); }
    bool Start() {
        if (!DemoVoiceWav().empty()) {  // MARKETING DEMO ONLY: the tape is cut from the demo file
            demoStart_ = BugClock();
            running_ = true;
            return true;
        }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        clock_.freq = f.QuadPart;
        clock_.start = RecClock::Now();
        std::wstring err;
        if (!cap_.Open(false, true, &err, nullptr)) return false;
        cap_.Run(&clock_, [this](const int16_t* pcm, uint32_t frames, int64_t) {
            for (uint32_t i = 0; i < frames && pcm_.size() < kMax; ++i) {  // 48 kHz stereo → 16 kHz mono
                sum_ += pcm[i * 2] + pcm[i * 2 + 1];
                if (++n_ == 3) {
                    pcm_.push_back((int16_t)(sum_ / 6));
                    sum_ = n_ = 0;
                }
            }
        });
        running_ = true;
        return true;
    }
    std::vector<int16_t> Stop() {
        if (demoStart_ >= 0) {  // MARKETING DEMO ONLY
            std::vector<int16_t> out;
            const double t0 = g_demoVoiceT0;
            if (running_ && t0 >= 0) {
                const std::vector<int16_t> all = DemoVoicePcm();
                const size_t a = (size_t)std::clamp((demoStart_ - t0) * 16000, 0.0, (double)all.size());
                const size_t b = (size_t)std::clamp((BugClock() - t0) * 16000, (double)a, (double)all.size());
                out.assign(all.begin() + a, all.begin() + b);
            }
            running_ = false;
            demoStart_ = -1;
            return out;
        }
        if (running_) cap_.Stop();
        running_ = false;
        return std::move(pcm_);
    }

private:
    static constexpr size_t kMax = 16000 * 135;  // the longest bug, and some
    RecClock clock_;
    AudioCapture cap_;
    std::vector<int16_t> pcm_;
    int sum_ = 0, n_ = 0;
    bool running_ = false;
    double demoStart_ = -1;  // MARKETING DEMO ONLY
};

// ---- the app's side ----

struct Listener {  // VoiceCommands on a thread of its own
    std::thread thread;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr), wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<int> dictation{0};
    ~Listener() {
        SetEvent(stop);
        if (thread.joinable()) thread.join();
        CloseHandle(stop);
        CloseHandle(wake);
    }
};

struct StartInfo {  // what "bug start" saw
    double wall = 0;
    std::wstring app, window, recordingPath;
    RECT area{};
    std::shared_future<BitmapPtr> shot;
    std::shared_ptr<VoiceTape> tape;
};

struct RecordingBugs {
    std::vector<std::shared_ptr<BugBundle>> written;  // waiting for the recording to stop
    std::optional<RecordResult> finished;
};

struct BugApp {
    BugReportOptions opt;
    BugReportHost host;
    BugTracker tracker;
    std::map<uint64_t, StartInfo> starts;  // by bug id
    std::unique_ptr<Listener> listener;
    bool listening = false, voiceTried = false, dictationWanted = false;
    UINT_PTR timer = 0;
    std::map<uint64_t, RecordingBugs> recordings;
    uint64_t watched = 0;
    std::shared_ptr<BugBundle> last;
    std::atomic<int> busy{0};
};
BugApp& App() {
    static BugApp a;
    return a;
}

// Tests drive the recording clock: {RecordingId(), RecordingSeconds()}.
std::function<std::pair<uint64_t, double>()> g_testRecording;

std::pair<uint64_t, double> RecordingNow() {
    if (g_testRecording) return g_testRecording();
    RecordFormat f;
    if (!RecordingTarget(nullptr, &f) || f != RecordFormat::Mp4) return {0, 0};
    return {RecordingId(), RecordingSeconds()};
}

// Work off the UI thread (with COM), then `then` on it. BugReportsBusy() holds until both are done (or dropped).
void Work(std::function<void()> job, std::function<void()> then) {
    struct Token {
        Token() { ++App().busy; }
        ~Token() { --App().busy; }
    };
    auto token = std::make_shared<Token>();
    std::thread([job = std::move(job), then = std::move(then), token] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        job();
        if (SUCCEEDED(co)) CoUninitialize();
        if (then) RunOnUi([then, token] { then(); });
    }).detach();
}

void Handle(const std::vector<BugEvent>& events);

void SyncListening() {
    BugApp& a = App();
    const bool want = a.tracker.Busy();
    if (a.listener && want != a.dictationWanted) {  // on a change only: a command may have switched it on its thread
        a.listener->dictation = want;
        SetEvent(a.listener->wake);
    }
    a.dictationWanted = want;
    const bool tick = a.listening || a.tracker.Busy();
    if (tick && !a.timer) {
        a.timer = SetTimer(nullptr, 0, 250, [](HWND, UINT, UINT_PTR, DWORD) {
            const auto [rec, secs] = RecordingNow();
            Handle(App().tracker.Tick(BugClock(), rec, secs));
        });
    } else if (!tick && a.timer) {
        KillTimer(nullptr, a.timer);
        a.timer = 0;
    }
}

void NoteInGallery(const BugBundle& b, const std::vector<std::wstring>& names) {
    for (const auto& n : names) {
        const std::wstring p = b.folder + L"\\" + n;
        Library::Shared().NoteCapture(p, b.app, b.window);
        Library::Shared().NoteCaptureDetails(p, {L"bug"}, b.bug.Description());
    }
}

void CutClips(std::vector<std::shared_ptr<BugBundle>> bundles, const RecordResult& r) {
    if (bundles.empty()) return;
    if (r.ok && !r.discarded)
        for (const auto& b : bundles) NoteInGallery(*b, {L"clip.mp4"});
    const double preRoll = App().opt.preRollSeconds;
    Work([bundles, r, preRoll] {
        for (const auto& b : bundles) FinishBugClip(*b, r, preRoll);
    }, nullptr);
}

void WatchRecording(uint64_t id) {
    BugApp& a = App();
    if (a.watched == id || g_testRecording || id != RecordingId() || !IsRecording()) return;
    a.watched = id;
    WhenRecordingFinishes([id](const RecordResult& r) { BugRecordingFinished(id, r); });
}

std::wstring HotkeyHint() {
    const std::wstring& hk = App().opt.hotkey;
    return hk.empty() ? L"“Bug report: start / stop”" : hk;
}

void OnStarted(const Bug& bug) {
    BugApp& a = App();
    StartInfo s;
    s.wall = NowEpoch() - (BugClock() - bug.start);
    HWND fg = GetForegroundWindow();
    if (fg && !IsOwnWindow(fg)) {
        s.app = WindowAppName(fg);
        s.window = WindowTitle(fg);
    }
    // What's recorded, or else the monitor with the app in front.
    RECT area{};
    RecordFormat fmt;
    std::wstring path;
    if (RecordingTarget(&area, &fmt, &path)) {
        s.area = area;
        if (bug.recording) s.recordingPath = path;
    } else {
        POINT pt;
        GetCursorPos(&pt);
        MONITORINFO mi{sizeof(mi)};
        s.area = fg && !IsOwnWindow(fg) && GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi) ? mi.rcMonitor : MonitorRectAt(pt);
    }
    s.shot = std::async(std::launch::async, [rc = s.area, cursor = a.opt.cursor] { return CaptureScreen(rc, cursor); }).share();
    if (a.listening) {
        s.tape = std::make_shared<VoiceTape>();
        if (!s.tape->Start()) s.tape.reset();
    }
    a.starts[bug.id] = s;
    if (bug.recording) WatchRecording(bug.recording);
    ShowToast(L"Bug started",
              a.listening ? L"Describe it, then say “bug stop” (or press " + HotkeyHint() + L")." : L"Press " + HotkeyHint() + L" again to save it.",
              nullptr, nullptr, 3000);
}

std::wstring UniqueFolder(const std::wstring& path) {
    std::wstring p = path;
    for (int k = 2; Exists(p); ++k) p = path + L" (" + std::to_wstring(k) + L")";
    return p;
}

void OnSaved(const std::shared_ptr<BugBundle>& b, bool ok, const std::wstring& error) {
    BugApp& a = App();
    if (!ok) {
        ShowToast(L"The bug couldn't be saved", error, nullptr, nullptr, 6000);
        return;
    }
    a.last = b;
    std::wstring body = b->Title() + L"\n";
    if (b->bug.autoStopped) body += L"It stopped itself after 2 minutes. ";
    if (b->clip == BugBundle::Clip::Pending) body += L"The clip is cut when the recording stops. ";
    BitmapPtr thumb = b->screenshot.valid() ? b->screenshot.get() : nullptr;
    const std::wstring folder = b->folder;
    ShowToast(L"Bug saved", body + L"Click to open the folder.", thumb, [folder] {
        if (App().host.openPath) App().host.openPath(folder);
    }, 4500);
    if (b->bug.recording) {
        RecordingBugs& rb = a.recordings[b->bug.recording];
        if (rb.finished) CutClips({b}, *rb.finished);
        else rb.written.push_back(b);
    }
}

void OnStopped(const Bug& bug) {
    BugApp& a = App();
    StartInfo s;
    if (auto it = a.starts.find(bug.id); it != a.starts.end()) {
        s = std::move(it->second);
        a.starts.erase(it);
    }
    auto b = std::make_shared<BugBundle>();
    b->bug = bug;
    b->id = NewUuid();
    b->wallStart = s.wall ? s.wall : NowEpoch() - (BugClock() - bug.start);
    b->app = s.app;
    b->window = s.window;
    b->area = s.area;
    b->screenshot = s.shot;
    b->recordingPath = s.recordingPath;
    b->clip = bug.recording ? BugBundle::Clip::Pending : BugBundle::Clip::None;
    b->folder = UniqueFolder(a.opt.folder + L"\\Bugs\\" + BugFolderName(b->wallStart, bug.Description()));
    std::vector<int16_t> voice = s.tape ? s.tape->Stop() : std::vector<int16_t>{};
    NoteInGallery(*b, {L"bug.png", L"screenshot.png"});  // before the files appear, so the gallery tags them
    const std::wstring logPaths = a.opt.logPaths;
    const uint64_t maxBytes = (uint64_t)std::max(1, a.opt.logMaxMB) << 20;
    auto result = std::make_shared<std::pair<bool, std::wstring>>();
    Work([b, voice = std::move(voice), logPaths, maxBytes, result] { result->first = WriteBugBundle(*b, voice, logPaths, maxBytes, &result->second); },
         [b, result] { OnSaved(b, result->first, result->second); });
}

void Handle(const std::vector<BugEvent>& events) {
    for (const auto& e : events) {
        switch (e.kind) {
            case BugEvent::Started: OnStarted(e.bug); break;
            case BugEvent::Stopped: OnStopped(e.bug); break;
            case BugEvent::Cancelled:
                App().starts.erase(e.bug.id);  // and its voice.wav with it
                ShowToast(L"Bug cancelled", L"Nothing was saved.", nullptr, nullptr, 2500);
                break;
            case BugEvent::Ignored: break;  // "bug start" while one is open
        }
    }
    SyncListening();
}

void ListenerStarted(Listener* l, bool ok, const std::wstring& error) {
    BugApp& a = App();
    if (l != a.listener.get()) return;
    if (!ok) {
        a.listener.reset();
        ShowToast(L"Voice commands aren't available", error + L"\nThe " + HotkeyHint() + L" shortcut still makes bug reports.", nullptr, nullptr, 9000);
        return;
    }
    a.listening = true;
    a.dictationWanted = false;
    SyncListening();
    if (a.host.listeningChanged) a.host.listeningChanged();
}

void OnHeard(Listener* l, const std::vector<VoiceCommands::Heard>& heard) {
    BugApp& a = App();
    if (l != a.listener.get()) return;
    const auto [rec, secs] = RecordingNow();
    Handle(a.tracker.Tick(BugClock(), rec, secs));
    for (const auto& h : heard) Handle(a.tracker.Heard(h.words));
}

void StartListening() {
    BugApp& a = App();
    a.listener = std::make_unique<Listener>();
    Listener* l = a.listener.get();
    if (const std::wstring wav = DemoVoiceWav(); !wav.empty()) {  // MARKETING DEMO ONLY: see DemoVoiceWav
        l->thread = std::thread([l, wav] {
            const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            RunOnUi([l] { ListenerStarted(l, true, L""); });
            const std::wstring go = wav + L".go";
            while (GetFileAttributesW(go.c_str()) == INVALID_FILE_ATTRIBUTES)
                if (WaitForSingleObject(l->stop, 100) == WAIT_OBJECT_0) return;
            const double t0 = BugClock();
            g_demoVoiceT0 = t0;
            VoiceCommands vc;
            std::wstring err;
            std::vector<VoiceCommands::Heard> all;
            if (vc.Open(wav, &err))
                for (const ULONGLONG end = GetTickCount64() + 120000; GetTickCount64() < end;)
                    if (!vc.Next(2000, &all)) break;
            for (auto& h : all) {
                double last = 0;
                for (auto& w : h.words) {
                    w.start += t0;
                    w.end += t0;
                    last = std::max(last, w.end);
                }
                const double wait = last + 0.35 - BugClock();
                if (wait > 0 && WaitForSingleObject(l->stop, (DWORD)(wait * 1000)) == WAIT_OBJECT_0) break;
                RunOnUi([l, h] { OnHeard(l, {h}); });
            }
            if (SUCCEEDED(co)) CoUninitialize();
        });
        return;
    }
    l->thread = std::thread([l] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            VoiceCommands vc;
            std::wstring err;
            const bool ok = vc.Open(L"", &err);
            RunOnUi([l, ok, err] { ListenerStarted(l, ok, err); });
            HANDLE wait[3] = {l->stop, l->wake, vc.Event()};
            while (ok) {
                const DWORD w = WaitForMultipleObjects(3, wait, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
                if (w == WAIT_OBJECT_0 + 1) vc.SetDictation(l->dictation != 0);
                std::vector<VoiceCommands::Heard> heard;
                vc.Next(0, &heard);
                if (!heard.empty()) RunOnUi([l, heard = std::move(heard)] { OnHeard(l, heard); });
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
    });
}

}  // namespace

void ConfigureBugReports(const BugReportOptions& o, const BugReportHost& h) {
    BugApp& a = App();
    a.opt = o;
    a.host = h;
    if (!o.voice) {
        a.voiceTried = false;
        if (a.listener) {
            a.listener.reset();
            a.listening = false;
            if (a.host.listeningChanged) a.host.listeningChanged();
        }
    } else if (!a.listener && !a.voiceTried) {  // a missing recognizer or microphone is said once, not at every reload
        a.voiceTried = true;
        StartListening();
    }
    SyncListening();
}

void ToggleBugReport() {
    BugApp& a = App();
    const auto [rec, secs] = RecordingNow();
    const double now = BugClock();
    Handle(a.tracker.Tick(now, rec, secs));
    const bool wasOpen = a.tracker.IsOpen();
    Handle(a.tracker.Toggle(now));
    if (wasOpen) ShowToast(L"Saving the bug…", L"", nullptr, nullptr, 3000);
}

std::wstring LastBugReport() {
    BugApp& a = App();
    if (a.last) return a.last->folder;
    // From an earlier session: the newest folder in Bugs with a bug.md.
    const std::wstring dir = a.opt.folder + L"\\Bugs";
    std::wstring best;
    FILETIME bestTime{};
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return L"";
    do {
        const std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.cFileName[0] == L'.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Exists(p + L"\\bug.md")) continue;
        if (best.empty() || CompareFileTime(&fd.ftCreationTime, &bestTime) > 0) {
            best = p;
            bestTime = fd.ftCreationTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

bool CopyLastBugReport() {
    const std::wstring dir = LastBugReport();
    if (dir.empty()) {
        ShowToast(L"No bug report yet", L"Start one with " + HotkeyHint() + L".", nullptr, nullptr, 3000);
        return false;
    }
    std::vector<std::wstring> files;
    for (const wchar_t* n : {L"bug.png", L"clip.mp4", L"bug.md"})
        if (Exists(dir + L"\\" + n)) files.push_back(dir + L"\\" + n);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\logs\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    for (int logs = 0; h != INVALID_HANDLE_VALUE && logs < 10;) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files.push_back(dir + L"\\logs\\" + fd.cFileName);
            ++logs;
        }
        if (!FindNextFileW(h, &fd)) break;
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    if (!CopyFilesToClipboard(App().host.owner, files)) {
        ShowToast(L"Couldn't copy the bug report", L"Another app is using the clipboard. Try again.", nullptr, nullptr, 3500);
        return false;
    }
    ShowToast(L"Bug report copied", std::to_wstring(files.size()) + L" files: paste them into Slack or your tracker.", nullptr, nullptr, 3000);
    return true;
}

void OpenBugReportsFolder() {
    const std::wstring dir = App().opt.folder + L"\\Bugs";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (App().host.openPath) App().host.openPath(dir);
}

bool BugVoiceListening() { return App().listening; }
bool BugReportsBusy() { return App().busy > 0; }

void StopBugReports() {
    BugApp& a = App();
    // A bug still open is saved, not lost (quitting waits for BugReportsBusy).
    const auto [rec, secs] = RecordingNow();
    const double now = BugClock();
    Handle(a.tracker.Tick(now, rec, secs));
    if (a.tracker.IsOpen()) a.tracker.Toggle(now);
    Handle(a.tracker.Tick(now + BugTracker::kGrace, rec, secs));
    a.listener.reset();
    a.listening = false;
    a.starts.clear();  // stops the microphone
    if (a.timer) KillTimer(nullptr, a.timer);
    a.timer = 0;
}

void BugRecordingFinished(uint64_t recording, const RecordResult& r) {
    RecordingBugs& rb = App().recordings[recording];
    rb.finished = r;
    CutClips(std::move(rb.written), r);
    rb.written.clear();
}

// ---- tests ----

namespace {

// Words said one after another from `t`, `step` seconds apart.
std::vector<CaptionWord> Said(double t, const std::wstring& text, double step = 0.3) {
    std::vector<CaptionWord> out;
    for (size_t i = 0; i < text.size();) {
        size_t sp = text.find(L' ', i);
        if (sp == std::wstring::npos) sp = text.size();
        if (sp > i) {
            out.push_back({t, t + step * 0.8, text.substr(i, sp - i)});
            t += step;
        }
        i = sp + 1;
    }
    return out;
}

std::wstring TestOut() {  // ATHER_TEST_OUT: where snapshots go (CI keeps them)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ATHER_TEST_OUT", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    SHCreateDirectoryExW(nullptr, buf, nullptr);
    return buf;
}

std::string ReadAll(const std::wstring& path) {
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    char buf[1 << 16];
    DWORD n = 0;
    while (f != INVALID_HANDLE_VALUE && ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return text;
}

bool Has(const std::string& text, const std::wstring& part) { return text.find(ToUtf8(part)) != std::string::npos; }

// A stand-in for a game: sky, ground, a closed door and a health bar.
BitmapPtr TestScene(int w, int h) {
    auto img = Bitmap::Create(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int k = y * 255 / h;
            img->Bits()[(size_t)y * w + x] = y < h * 2 / 3 ? 0xFF000000u | (40 + k / 3) << 16 | (90 + k / 3) << 8 | (170 + k / 4)
                                                          : 0xFF000000u | 70u << 16 | 110u << 8 | 50u;
        }
    const float u = h / 100.f;
    textdraw::FillRounded(*img, w * 0.42f, h * 0.3f, w * 0.16f, h * 0.37f, 2 * u, RGB(110, 70, 40), 1);
    textdraw::FillRounded(*img, w * 0.53f, h * 0.48f, 2 * u, 2 * u, u, RGB(230, 200, 60), 1);
    textdraw::FillRounded(*img, 3 * u, 3 * u, 30 * u, 4 * u, 2 * u, RGB(30, 30, 30), 0.8f);
    textdraw::FillRounded(*img, 3.5f * u, 3.5f * u, 20 * u, 3 * u, 1.5f * u, RGB(220, 60, 60), 1);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) img->Bits()[i] |= 0xFF000000u;
    return img;
}

std::shared_future<BitmapPtr> Ready(BitmapPtr img) {
    std::promise<BitmapPtr> p;
    p.set_value(std::move(img));
    return p.get_future().share();
}

void SetAge(const std::wstring& path, double seconds) {
    const uint64_t t = (uint64_t)((NowEpoch() - seconds) * 1e7 + 116444736000000000.0);
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

// A log of `bytes` bytes ("line 000001\n" …), last written `age` seconds ago.
void MakeLog(const std::wstring& path, size_t bytes, double age) {
    std::string text;
    for (int i = 1; text.size() < bytes; ++i) text += std::format("line {:06}\n", i);
    text.resize(bytes);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wrote = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(f);
    SetAge(path, age);
}

bool PumpUntil(const std::function<bool()>& done, int ms) {
    for (const ULONGLONG end = GetTickCount64() + ms; GetTickCount64() < end;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done()) return true;
        Sleep(10);
    }
    return done();
}

int DarkPixels(const Bitmap& img, RECT r) {  // dark pixels in `r`: a caption's box on the test clip's bright colors
    int n = 0;
    for (int y = r.top; y < r.bottom; y += 2)
        for (int x = r.left; x < r.right; ++x) {
            const uint32_t p = img.Pixel(x, y);
            n += std::max({(p >> 16) & 255, (p >> 8) & 255, p & 255}) < 153;
        }
    return n;
}

}  // namespace

ATHER_TEST(bug_commands_and_words_by_voice) {
    BugTracker tr;
    CHECK(tr.Tick(0, 0, 0).empty());
    auto ev = tr.Heard(Said(10, L"bug start"));
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Started && ev[0].bug.start == 10);
    CHECK(tr.IsOpen());
    CHECK(tr.Heard(Said(11, L"the door does not open.")).empty());
    ev = tr.Heard(Said(13, L"bug start"));  // one is open: ignored
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Ignored);
    ev = tr.Heard(Said(14, L"after the boss Bug stop. thanks"));  // a command inside dictation
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped);
    if (ev.size() == 1) {
        CHECK(ev[0].bug.Description() == L"the door does not open. after the boss");  // no command words, nothing after "bug stop"
        CHECK_NEAR(ev[0].bug.stop, 14.9, 1e-9);
        CHECK_EQ(ev[0].bug.recording, 0u);
    }
    CHECK(!tr.IsOpen() && !tr.Busy());
    CHECK(tr.Heard(Said(20, L"bug stop")).empty());  // nothing open
    ev = tr.Heard(Said(30, L"hello bug start it crashes bug cancel later"));
    CHECK(ev.size() == 2 && ev[0].kind == BugEvent::Started && ev[1].kind == BugEvent::Cancelled);
    if (ev.size() == 2) CHECK(ev[1].bug.Description() == L"it crashes");
    CHECK(!tr.Busy());
    // Case and punctuation don't matter; a bug with nothing said is still a bug.
    ev = tr.Heard({{40, 40.2, L"Bug,"}, {40.3, 40.5, L"START!"}});
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Started);
    ev = tr.Heard(Said(41, L"BUG Stop"));
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped && ev[0].bug.words.empty());
    size_t at = 0;
    CHECK(FindBugCommand(Said(0, L"a bugs start bug"), 0, &at) == BugCommand::None && at == 4);
    CHECK(FindBugCommand(Said(0, L"bug bug cancel"), 0, &at) == BugCommand::Cancel && at == 1);
}

ATHER_TEST(bug_shortcut_keeps_late_words_and_stops_itself) {
    BugTracker tr;
    tr.Tick(0, 0, 0);
    auto ev = tr.Toggle(1);
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Started);
    tr.Heard(Said(1.5, L"water is missing"));
    CHECK(tr.Toggle(4).empty());  // stopped: waits for words still on their way
    CHECK(!tr.IsOpen() && tr.Busy());
    tr.Heard({{3.7, 3.9, L"again"}, {4.2, 4.4, L"later"}});  // said before and after the stop, heard after it
    CHECK(tr.Tick(5, 0, 0).empty());
    ev = tr.Tick(4 + BugTracker::kGrace, 0, 0);
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped);
    if (ev.size() == 1) {
        CHECK(ev[0].bug.Description() == L"water is missing again");
        CHECK_EQ(ev[0].bug.stop, 4.0);
        CHECK(!ev[0].bug.autoStopped);
    }
    // Left open, a bug stops itself after 2 minutes.
    CHECK(tr.Toggle(100).size() == 1);
    CHECK(tr.Tick(219, 0, 0).empty());
    CHECK(tr.IsOpen());
    CHECK(tr.Tick(220, 0, 0).empty());
    CHECK(!tr.IsOpen());
    ev = tr.Tick(221.6, 0, 0);
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped);
    if (ev.size() == 1) CHECK(ev[0].bug.autoStopped && ev[0].bug.stop == 220);
    // The shortcut stops a bug started by voice.
    CHECK(tr.Heard(Said(300, L"bug start")).size() == 1);
    CHECK(tr.Toggle(305).empty());
    ev = tr.Tick(306.6, 0, 0);
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped);
    // "Bug start" while the last one waits for its words: that one is saved first.
    tr.Toggle(400);
    tr.Toggle(402);
    ev = tr.Heard(Said(402.5, L"bug start"));
    CHECK(ev.size() == 2 && ev[0].kind == BugEvent::Stopped && ev[1].kind == BugEvent::Started);
    if (ev.size() == 2) CHECK(ev[0].bug.stop == 402 && ev[1].bug.start == 402.5 && ev[0].bug.id != ev[1].bug.id);
}

ATHER_TEST(bug_times_follow_the_recording_and_its_pauses) {
    BugTracker tr;
    // Recording 7 starts at t = 100, is paused from 112 to 117 and ends at 126.
    auto rec = [](double t) { return t < 112 ? t - 100 : t < 117 ? 12.0 : t - 105; };
    for (double t = 99; t <= 145; t += 0.25) tr.Tick(t, t >= 100 && t <= 126 ? 7 : 0, t >= 100 && t <= 126 ? rec(t) : 0);
    auto ev = tr.Heard(Said(110, L"bug start"));
    CHECK(ev.size() == 1 && ev[0].bug.recording == 7);
    tr.Heard(Said(111, L"jump"));
    tr.Heard(Said(118, L"falls through"));
    ev = tr.Heard(Said(120, L"bug stop"));
    CHECK(ev.size() == 1 && ev[0].kind == BugEvent::Stopped);
    if (ev.size() == 1) {
        const Bug& b = ev[0].bug;
        CHECK_NEAR(b.recStart, 10, 1e-9);
        CHECK_NEAR(b.recStop, 15, 1e-9);  // the 5 paused seconds aren't in the recording
        CHECK_EQ(b.recWords.size(), 3u);
        if (b.recWords.size() == 3) {
            CHECK_NEAR(b.recWords[0].start, 11, 1e-9);
            CHECK_NEAR(b.recWords[1].start, 13, 1e-9);
            CHECK_NEAR(b.recWords[2].start, 13.3, 1e-9);
            CHECK(b.recWords[2].end > b.recWords[2].start);
        }
    }
    // Past the recording's end: it ends there.
    tr.Heard(Said(124, L"bug start"));
    ev = tr.Heard(Said(130, L"bug stop"));
    CHECK(ev.size() == 1);
    if (ev.size() == 1) {
        CHECK_NEAR(ev[0].bug.recStart, 19, 1e-9);
        CHECK(ev[0].bug.recStop >= 21 && ev[0].bug.recStop <= 21.25 + 1e-9);
    }
    ev = tr.Heard(Said(140, L"bug start"));  // nothing records now
    CHECK(ev.size() == 1 && ev[0].bug.recording == 0);
}

ATHER_TEST(bug_overlay_bar_header_and_lines) {
    auto shot = Bitmap::Create(1280, 720);
    std::fill_n(shot->Bits(), 1280 * 720, 0xFFC8C8C8u);
    const std::wstring header = L"BUG · 2026-10-21 14:03:22 · Game";
    std::wstring longText;
    for (int i = 0; i < 60; ++i) longText += L"The door still does not open after the boss dies. ";
    BugOverlay a, b, c;
    auto none = DrawBugOverlay(*shot, header, L"", &a);
    auto some = DrawBugOverlay(*shot, header, L"The door does not open after the boss dies.", &b);
    auto many = DrawBugOverlay(*shot, header, longText, &c);
    CHECK(none && some && many);
    if (!none || !some || !many) return;
    CHECK(a.bar.left == 0 && a.bar.right == 1280 && a.bar.bottom == 720 && a.bar.top > 600);  // along the bottom, full width
    CHECK(a.lines == 0 && b.lines == 1 && c.lines == BugOverlay::kMaxLines);
    CHECK(c.cut && !b.cut);
    CHECK(RectH(a.bar) < RectH(b.bar) && RectH(b.bar) < RectH(c.bar));
    CHECK(RectH(c.bar) < 720 / 2);
    auto lum = [](uint32_t p) { return (int)(((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255)) / 3; };
    auto count = [&](const Bitmap& img, RECT r, const std::function<bool(uint32_t)>& f) {
        int n = 0;
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) n += f(img.Pixel(x, y));
        return n;
    };
    auto white = [&](uint32_t p) { return lum(p) > 235; };
    auto lime = [](uint32_t p) { return ((p >> 8) & 255) > 200 && ((p >> 16) & 255) > 150 && (p & 255) < 90; };
    CHECK_EQ(some->Pixel(10, b.bar.top - 2), 0xFFC8C8C8u);  // above the bar: as it was
    CHECK(lum(some->Pixel(2, 718)) < 80);                   // the bar: dark, translucent
    CHECK(lum(some->Pixel(2, 718)) > 20);
    CHECK(count(*some, b.bar, white) > 300);  // the description in white
    CHECK(count(*none, a.bar, white) < 20);   // header only
    CHECK(count(*none, a.bar, lime) > 10);    // "BUG" in lime
    CHECK(count(*many, {0, 0, 1280, c.bar.top - 1}, [](uint32_t p) { return p != 0xFFC8C8C8u; }) == 0);
    CHECK_EQ(shot->Pixel(2, 718), 0xFFC8C8C8u);  // the screenshot itself stays as it was
    if (const std::wstring out = TestOut(); !out.empty()) {
        auto scene = TestScene(1600, 900);
        auto snap = DrawBugOverlay(*scene, header,
                                   L"The door does not open after the boss dies. I tried it twice, and the key is still in the "
                                   L"inventory. It happens in level three only, right after the cut scene ends and the music changes.");
        CHECK(snap && SavePng(*snap, out + L"\\bug-overlay.png"));
    }
}

ATHER_TEST(bug_logs_recent_newest_tail_and_open_files) {
    const std::wstring dir = test::TempDir(), logs = dir + L"\\game";
    for (const wchar_t* d : {L"", L"\\old", L"\\recent", L"\\other"}) CreateDirectoryW((logs + d).c_str(), nullptr);
    MakeLog(logs + L"\\old\\a.log", 100, 7200);
    MakeLog(logs + L"\\old\\b.log", 100, 3600);  // the newest, though older than 30 minutes
    MakeLog(logs + L"\\recent\\c.log", 100, 0);
    MakeLog(logs + L"\\recent\\d.log", 100, 600);
    MakeLog(logs + L"\\recent\\e.log", 100, 7200);
    MakeLog(logs + L"\\recent\\notes.txt", 50, 0);
    MakeLog(logs + L"\\other\\c.log", 40, 0);
    MakeLog(logs + L"\\big.log", 3 << 20, 0);
    // The game's own log, still open for writing.
    HANDLE game = CreateFileW((logs + L"\\game.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0, nullptr);
    CHECK(game != INVALID_HANDLE_VALUE);
    const std::string gameText = "started\nloaded level 3\n";
    DWORD wrote = 0;
    WriteFile(game, gameText.data(), (DWORD)gameText.size(), &wrote, nullptr);
    SetEnvironmentVariableW(L"ATHER_TEST_LOGS", logs.c_str());
    const std::wstring paths = L"%ATHER_TEST_LOGS%\\game.log; " + logs + L"\\old\\ ;\"" + logs + L"\\recent\\*.log\";" + logs +
                               L"\\big.log;C:\\nope\\x.log;;" + logs + L"\\none\\*.log";
    std::vector<std::wstring> missing;
    const auto found = FindBugLogs(paths, NowEpoch(), &missing);
    std::vector<std::wstring> names;
    for (const auto& f : found) names.push_back(f.substr(logs.size() + 1));
    test::Note("found " + std::to_string(found.size()));
    CHECK(names == std::vector<std::wstring>({L"game.log", L"old\\b.log", L"recent\\c.log", L"recent\\d.log", L"big.log"}));
    CHECK(missing == std::vector<std::wstring>({L"C:\\nope\\x.log", logs + L"\\none\\*.log"}));
    const auto copied = CopyBugLogs(found, dir + L"\\logs", 1 << 20);
    CHECK_EQ(copied.size(), 5u);
    if (copied.size() == 5) {
        CHECK(copied[0].error.empty() && copied[0].file == L"logs\\game.log");  // open for writing, still copied
        CHECK_EQ(ReadAll(dir + L"\\logs\\game.log"), gameText);
        CHECK(copied[0].size == gameText.size() && copied[0].copied == gameText.size() && !copied[0].truncated);
        CHECK(copied[2].file == L"logs\\c.log");
        const BugLog& big = copied[4];
        CHECK(big.truncated && big.size == (3u << 20));
        CHECK(big.copied <= (1u << 20) && big.copied > (1u << 20) - 64);
        const std::string tail = ReadAll(dir + L"\\logs\\big.log");
        CHECK_EQ(tail.size(), big.copied);
        CHECK(tail.rfind("line ", 0) == 0);  // from the start of a line
    }
    CloseHandle(game);
    // The same name from two folders: both kept.
    const auto twice = CopyBugLogs({logs + L"\\recent\\c.log", logs + L"\\other\\c.log"}, dir + L"\\logs2", 1 << 20);
    CHECK(twice.size() == 2 && twice[0].file == L"logs\\c.log" && twice[1].file == L"logs\\c (2).log");
    CHECK_EQ(ReadAll(dir + L"\\logs2\\c (2).log").size(), 40u);
}

ATHER_TEST(bug_bundle_files_clip_and_note) {
    const std::wstring dir = test::TempDir();
    MakeLog(dir + L"\\game.log", 300, 0);
    BugBundle b;
    b.bug.id = 1;
    b.bug.start = 50;
    b.bug.words = Said(50.5, L"The door does not open after the boss dies. Tried twice.");
    b.bug.stop = 54;
    b.bug.recording = 3;
    b.bug.recStart = 12;
    b.bug.recStop = 16;
    for (CaptionWord w : b.bug.words) {
        w.start -= 38;
        w.end -= 38;
        b.bug.recWords.push_back(w);
    }
    b.id = L"test-bug";
    b.wallStart = 1792000000;
    b.app = L"Game";
    b.window = L"Level 3";
    b.area = {0, 0, 640, 360};
    const BitmapPtr shot = TestScene(640, 360);
    b.screenshot = Ready(shot);
    b.folder = dir + L"\\Bugs\\" + BugFolderName(b.wallStart, b.bug.Description());
    b.clip = BugBundle::Clip::Pending;
    CHECK(FileNameOf(b.folder) == LocalTime(b.wallStart, L"%Y-%m-%d %H-%M-%S") + L" The door does not open after the boss");
    std::vector<int16_t> voice(16000);
    for (size_t i = 0; i < voice.size(); ++i) voice[i] = (int16_t)(8000 * std::sin(i * 0.15));
    std::wstring err;
    CHECK(WriteBugBundle(b, voice, dir + L"\\game.log;" + dir + L"\\missing.log", 1 << 20, &err));
    test::Note(ToUtf8(err));
    for (const wchar_t* f : {L"bug.png", L"screenshot.png", L"voice.wav", L"logs\\game.log", L"bug.md", L"bug.json"})
        CHECK(Exists(b.folder + L"\\" + f));
    CHECK(!Exists(b.folder + L"\\clip.mp4"));
    // screenshot.png as it was; bug.png with the bar
    const BitmapPtr clean = LoadImageFile(b.folder + L"\\screenshot.png"), over = LoadImageFile(b.folder + L"\\bug.png");
    CHECK(clean && over);
    if (clean) {
        CHECK(clean->Width() == 640 && clean->Height() == 360);
        CHECK(clean->Width() == 640 && clean->Height() == 360 && std::equal(clean->Bits(), clean->Bits() + 640 * 360, shot->Bits()));
    }
    if (over) {
        CHECK(over->Width() == 640 && over->Height() == 360);
        CHECK_EQ(over->Pixel(5, 5), shot->Pixel(5, 5));
        CHECK(over->Pixel(2, 357) != shot->Pixel(2, 357));
    }
    const std::string wav = ReadAll(b.folder + L"\\voice.wav");
    CHECK(wav.size() == 44 + 32000 && wav.rfind("RIFF", 0) == 0 && wav.substr(8, 4) == "WAVE");
    if (wav.size() >= 44) CHECK(*(const uint32_t*)(wav.data() + 24) == 16000);
    std::string md = ReadAll(b.folder + L"\\bug.md");
    CHECK(Has(md, L"# The door does not open after the boss dies\n"));
    CHECK(Has(md, L"**App:** Game, “Level 3”"));
    CHECK(Has(md, L"**Recording:** the screen recording, 0:12.0 to 0:16.0"));
    CHECK(Has(md, L"**Clip:** when the recording stops"));
    CHECK(Has(md, L"The door does not open after the boss dies. Tried twice."));
    CHECK(Has(md, L"`logs\\game.log`: from `" + dir + L"\\game.log`, 300 bytes"));
    CHECK(Has(md, L"Not found: `" + dir + L"\\missing.log`"));
    bool ok = false;
    Json j = Json::Parse(ReadAll(b.folder + L"\\bug.json"), &ok);
    CHECK(ok);
    CHECK(j["format"].Int() == 1 && j["id"].Str() == "test-bug" && j["kind"].Str() == "bug");
    CHECK(j["description"].WStr() == b.bug.Description());
    CHECK(j["started"].Str() == IsoUtc(b.wallStart) && j["seconds"].Num() == 4);
    CHECK_EQ(j["words"].size(), 11u);
    CHECK(j["words"][0]["text"].Str() == "The" && j["words"][0]["start"].Num() == 0.5 && j["words"][0]["recording_time"].Num() == 12.5);
    CHECK(j["foreground"]["app"].Str() == "Game" && j["foreground"]["window"].Str() == "Level 3");
    CHECK(j["screenshot"]["area"]["width"].Int() == 640 && j["screenshot"]["file"].Str() == "screenshot.png");
    CHECK(j["recording"]["start"].Num() == 12 && j["recording"]["stop"].Num() == 16);
    CHECK(j["clip"]["state"].Str() == "pending");
    CHECK(j["voice"].Str() == "voice.wav");
    CHECK(j["logs"].size() == 1 && j["logs"][0]["source"].WStr() == dir + L"\\game.log" && j["logs"][0]["size"].Int() == 300 &&
          !j["logs"][0]["truncated"].Bool(true));
    CHECK(j["missing_logs"].size() == 1);

    // The recording stops: the clip with its captions, and the bug as a note on the recording.
    const std::wstring rec = dir + L"\\rec.mp4";
    CHECK(WriteTestClip(rec, 320, 180, 30, 20, true));
    RecordResult r;
    r.ok = true;
    r.path = rec;
    r.seconds = 20;
    FinishBugClip(b, r, 10);
    test::Note(ToUtf8(b.clipNote));
    CHECK(b.clip == BugBundle::Clip::Saved);
    CHECK(b.clipFrom == 2 && b.clipTo == 16);
    const std::wstring clip = b.folder + L"\\clip.mp4";
    VideoInfo vi;
    CHECK(ProbeVideo(clip, &vi));
    test::Note("clip duration " + std::to_string(vi.duration));
    CHECK_NEAR(vi.duration, 14, 1 / 30.0 + 1e-3);  // 10 s before "bug start" to "bug stop", within a frame
    // The words are captions at their times: "The door…" from 12.5 s in the recording, 10.5 s into the clip.
    BitmapPtr before, during;
    CHECK(ProbeVideo(clip, nullptr, 5, 0, &before) && before);
    CHECK(ProbeVideo(clip, nullptr, 11.2, 0, &during) && during);
    const RECT low{60, 120, 260, 178};
    if (before) CHECK_EQ(DarkPixels(*before, low), 0);
    if (during) CHECK(DarkPixels(*during, low) > 20);
    if (const std::wstring out = TestOut(); !out.empty() && during) CHECK(SavePng(*during, out + L"\\bug-clip-frame.png"));
    std::vector<Note> notes;
    CHECK(ParseNotes(ReadAll(NotesPath(rec)), rec, {}, 30, &notes));
    CHECK_EQ(notes.size(), 1u);
    if (notes.size() == 1) {
        CHECK(notes[0].kind == NoteKind::Issue);
        CHECK(notes[0].src <= 12 + 1e-6 && notes[0].src > 12 - 1 / 30.0 - 1e-3);  // the frame on screen at "bug start"
        CHECK(notes[0].srcEnd && *notes[0].srcEnd <= 16 + 1e-6 && *notes[0].srcEnd > 16 - 1 / 30.0 - 1e-3);
        CHECK(notes[0].text == L"Bug: " + b.bug.Description());
    }
    md = ReadAll(b.folder + L"\\bug.md");
    CHECK(Has(md, L"**Recording:** `rec.mp4`, 0:12.0 to 0:16.0"));
    CHECK(Has(md, L"**Clip:** `clip.mp4`, 0:02.0 to 0:16.0 of the recording, with captions"));
    j = Json::Parse(ReadAll(b.folder + L"\\bug.json"), &ok);
    CHECK(ok && j["clip"]["state"].Str() == "saved" && j["clip"]["from"].Num() == 2 && j["clip"]["to"].Num() == 16);
    CHECK(j["recording"]["path"].WStr() == rec);

    // A bug 4 s into the recording: the pre-roll stops at its start. A second bug's note joins the first.
    BugBundle early = b;
    early.folder = dir + L"\\Bugs\\early";
    early.bug.recStart = 4;
    early.bug.recStop = 7;
    early.bug.recWords.clear();
    CHECK(WriteBugBundle(early, {}, L"", 1 << 20, &err));
    FinishBugClip(early, r, 10);
    CHECK(early.clip == BugBundle::Clip::Saved && early.clipFrom == 0 && early.clipTo == 7);
    CHECK(ProbeVideo(early.folder + L"\\clip.mp4", &vi));
    CHECK_NEAR(vi.duration, 7, 1 / 30.0 + 1e-3);
    notes.clear();
    CHECK(ParseNotes(ReadAll(NotesPath(rec)), rec, {}, 30, &notes));
    CHECK_EQ(notes.size(), 2u);
}

ATHER_TEST(bug_without_a_recording_has_no_clip) {
    const std::wstring dir = test::TempDir();
    BugBundle b;
    b.bug.start = 5;
    b.bug.stop = 8;
    b.id = L"no-rec";
    b.wallStart = 1792000000;
    b.screenshot = Ready(TestScene(400, 300));
    b.folder = dir + L"\\Bugs\\" + BugFolderName(b.wallStart, L"");
    CHECK(FileNameOf(b.folder) == LocalTime(b.wallStart, L"%Y-%m-%d %H-%M-%S"));
    std::wstring err;
    CHECK(WriteBugBundle(b, {}, L"", 1 << 20, &err));
    CHECK(!Exists(b.folder + L"\\clip.mp4") && !Exists(b.folder + L"\\voice.wav"));
    const std::string md = ReadAll(b.folder + L"\\bug.md");
    CHECK(Has(md, L"# Bug at " + LocalTime(b.wallStart, L"%H:%M:%S")));
    CHECK(Has(md, L"**Clip:** none: nothing was being recorded"));
    CHECK(!Has(md, L"**Recording:**"));
    CHECK(Has(md, L"Nothing: voice commands were off."));
    bool ok = false;
    const Json j = Json::Parse(ReadAll(b.folder + L"\\bug.json"), &ok);
    CHECK(ok && j["recording"].IsNull() && j["clip"]["state"].Str() == "none" && j["voice"].IsNull());
    CHECK(j["clip"]["note"].Str() == "nothing was being recorded");
    // A bug in a recording that was then discarded: no clip, no note.
    BugBundle d = b;
    d.folder = dir + L"\\Bugs\\discarded";
    d.bug.recording = 9;
    d.bug.recStart = 1;
    d.bug.recStop = 2;
    d.clip = BugBundle::Clip::Pending;
    CHECK(WriteBugBundle(d, {}, L"", 1 << 20, &err));
    RecordResult r;
    r.discarded = true;
    r.path = dir + L"\\gone.mp4";
    FinishBugClip(d, r, 10);
    CHECK(d.clip == BugBundle::Clip::None && !Exists(d.folder + L"\\clip.mp4") && !Exists(NotesPath(r.path)));
    CHECK(Has(ReadAll(d.folder + L"\\bug.md"), L"**Clip:** none: the recording was discarded"));
}

// "Bug start … the door does not open … bug stop" made by Windows text-to-speech, through the same recognizer setup the
// app listens to the microphone with (command grammar, dictation while a bug is open), bound to the file instead.
ATHER_TEST(bug_voice_commands_from_speech) {
    const std::wstring wav = test::TempDir() + L"\\bug-speech.wav";
    ComPtr<ISpVoice> voice;
    ComPtr<ISpStream> stream;
    WAVEFORMATEX wfx{WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&voice));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
    if (SUCCEEDED(hr)) hr = stream->BindToFile(wav.c_str(), SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfx, 0);
    if (SUCCEEDED(hr)) hr = voice->SetOutput(stream.Get(), TRUE);
    if (SUCCEEDED(hr))
        hr = voice->Speak(L"<silence msec=\"700\"/>Bug start.<silence msec=\"1200\"/>The door does not open.<silence msec=\"1200\"/>"
                          L"Bug stop.<silence msec=\"700\"/>",
                          SPF_IS_XML, nullptr);
    if (stream) stream->Close();
    if (FAILED(hr)) {
        test::Out("  (no text-to-speech voice: skipped)\n");
        return;
    }
    VoiceCommands vc;
    std::wstring err;
    if (!vc.Open(wav, &err)) {
        test::Out("  (no speech recognizer: " + ToUtf8(err.substr(0, 80)) + ")\n");
        CHECK(!err.empty());  // a clear message instead of silence
        return;
    }
    BugTracker tr;
    std::vector<BugEvent> events;
    std::string heard;
    for (const ULONGLONG end = GetTickCount64() + 120000; GetTickCount64() < end;) {
        std::vector<VoiceCommands::Heard> got;
        const bool more = vc.Next(2000, &got);
        for (const auto& h : got) {
            heard += h.command ? std::format("[command, confidence {}]", h.confidence) : std::string("[dictation]");
            for (const auto& w : h.words) heard += std::format(" {}@{:.2f}", ToUtf8(w.text), w.start);
            heard += " | ";
            for (auto& e : tr.Heard(h.words)) events.push_back(std::move(e));
        }
        if (!more) break;
    }
    test::Out("  heard: " + heard + "\n");
    test::Note("heard: " + heard);
    CHECK(events.size() >= 2);
    if (events.size() < 2) return;
    CHECK(events[0].kind == BugEvent::Started);
    const auto stop = std::find_if(events.begin(), events.end(), [](const BugEvent& e) { return e.kind == BugEvent::Stopped; });
    CHECK(stop != events.end());
    if (stop == events.end()) return;
    const std::wstring said = LowerText(stop->bug.Description());
    int hits = 0;
    for (const wchar_t* k : {L"door", L"does", L"not", L"open"}) hits += said.find(k) != std::wstring::npos;
    CHECK(hits >= 3);
    CHECK(said.find(L"bug") == std::wstring::npos);  // the commands aren't part of it
    CHECK(stop->bug.start < stop->bug.stop);
    for (const auto& w : stop->bug.words) CHECK(w.start >= stop->bug.start && w.start < stop->bug.stop);
}

// Through the app's side: the shortcut, the screenshot, the bundle, the clip when the recording stops, Copy last bug
// report and the folder.
ATHER_TEST(bug_reports_in_the_app) {
    HWND dispatcher = StartUiDispatcher();
    HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr);
    const std::wstring dir = test::TempDir();
    MakeLog(dir + L"\\game.log", 120, 0);
    BugReportOptions o;
    o.folder = dir;
    o.hotkey = L"Ctrl+Alt+B";
    o.logPaths = dir + L"\\game.log";
    std::wstring opened;
    BugReportHost h;
    h.owner = owner;
    h.openPath = [&opened](const std::wstring& p) { opened = p; };
    ConfigureBugReports(o, h);
    CHECK(!BugVoiceListening());  // listening is opt-in
    const double t0 = BugClock();
    g_testRecording = [t0] { return std::pair<uint64_t, double>{42, 30 + BugClock() - t0}; };  // 30 s into a recording
    ToggleBugReport();
    PumpUntil([] { return false; }, 300);
    ToggleBugReport();
    CHECK(PumpUntil([] { return !LastBugReport().empty() && !BugReportsBusy(); }, 20000));
    const std::wstring first = LastBugReport();
    CHECK(Library::IsInside(first, dir + L"\\Bugs"));
    CHECK(Exists(first + L"\\screenshot.png") && Exists(first + L"\\bug.png") && Exists(first + L"\\logs\\game.log"));
    CHECK(Has(ReadAll(first + L"\\bug.md"), L"**Clip:** when the recording stops"));
    // A bug with nothing recording.
    g_testRecording = [] { return std::pair<uint64_t, double>{0, 0}; };
    ToggleBugReport();
    PumpUntil([] { return false; }, 200);
    ToggleBugReport();
    CHECK(PumpUntil([&] { return LastBugReport() != first && !BugReportsBusy(); }, 20000));
    const std::wstring second = LastBugReport();
    CHECK(Has(ReadAll(second + L"\\bug.md"), L"**Clip:** none: nothing was being recorded"));
    // The recording stops: the first bug gets its clip and note.
    const std::wstring rec = dir + L"\\rec.mp4";
    CHECK(WriteTestClip(rec, 160, 90, 30, 34, false));
    RecordResult r;
    r.ok = true;
    r.path = rec;
    r.seconds = 34;
    BugRecordingFinished(42, r);
    CHECK(PumpUntil([] { return !BugReportsBusy(); }, 60000));
    CHECK(Exists(first + L"\\clip.mp4"));
    CHECK(Exists(NotesPath(rec)));
    CHECK(Has(ReadAll(first + L"\\bug.md"), L"**Clip:** `clip.mp4`, 0:20."));
    CHECK(!Exists(second + L"\\clip.mp4"));
    // Copy last bug report: the second bug's files on the clipboard.
    CHECK(CopyLastBugReport());
    std::vector<std::wstring> pasted;
    if (OpenClipboard(owner)) {
        if (HANDLE drop = GetClipboardData(CF_HDROP)) {
            const UINT n = DragQueryFileW((HDROP)drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t f[MAX_PATH];
                if (DragQueryFileW((HDROP)drop, i, f, MAX_PATH)) pasted.push_back(f);
            }
        }
        CloseClipboard();
    }
    CHECK(pasted == std::vector<std::wstring>({second + L"\\bug.png", second + L"\\bug.md", second + L"\\logs\\game.log"}));
    OpenBugReportsFolder();
    CHECK(opened == dir + L"\\Bugs");
    g_testRecording = nullptr;
    StopBugReports();
    DestroyWindow(owner);
    StopUiDispatcher(dispatcher);
}

}  // namespace ather
