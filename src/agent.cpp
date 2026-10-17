#include "agent.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <thread>

#include "capture.h"
#include "library.h"
#include "media.h"
#include "ocr.h"
#include "output.h"
#include "recorder.h"
#include "selftest.h"
#include "settings.h"
#include "toast.h"
#include "version.h"
#include "videoedit.h"
#include "videoeditor.h"
#include "videoio.h"

namespace ather {

namespace {

using mcp::ErrorResult;
using mcp::JsonResult;
using mcp::Reply;
using mcp::Tool;

AgentHost g_host;

// ---- small helpers ----

Json Rect(const RECT& r) {
    Json j = Json::Object();
    j.Set("x", (int)r.left);
    j.Set("y", (int)r.top);
    j.Set("width", (int)RectW(r));
    j.Set("height", (int)RectH(r));
    return j;
}

Json Strings(const std::vector<std::wstring>& v) {
    Json a = Json::Array();
    for (const auto& s : v) a.Push(Json(s));
    return a;
}

std::vector<std::wstring> StringList(const Json& j) {
    std::vector<std::wstring> out;
    if (j.IsString() && !j.Str().empty()) out.push_back(j.WStr());
    for (const auto& i : j.Items())
        if (i.IsString() && !i.Str().empty()) out.push_back(i.WStr());
    return out;
}

std::string Utf8(const std::wstring& s) { return ToUtf8(s); }

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::string IsoTime(double secondsSince1970) {
    const time_t t = (time_t)secondsSince1970;
    tm u{};
    if (gmtime_s(&u, &t) != 0) return {};
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &u);
    return buf;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Longest side at most `maxSide` (0 = as is).
BitmapPtr Shrink(const BitmapPtr& img, int maxSide) {
    if (!img || maxSide <= 0 || std::max(img->Width(), img->Height()) <= maxSide) return img;
    const double k = (double)maxSide / std::max(img->Width(), img->Height());
    return Resample(*img, std::max(1, (int)std::lround(img->Width() * k)), std::max(1, (int)std::lround(img->Height() * k)));
}

// "preview" (the default: longest side 1568 px, what vision models read best), "full" or "none".
int ImageSide(const Json& args, const char* key = "image") {
    const std::string v = args[key].Str();
    if (v == "none") return -1;
    if (v == "full") return 0;
    return 1568;
}

Json Picture(const BitmapPtr& img, int side) {
    Json extra = Json::Array();
    if (side < 0 || !img) return extra;
    BitmapPtr small = Shrink(img, side);
    if (small) extra.Push(mcp::ImageContent(EncodePng(*small)));
    return extra;
}

bool WriteUtf8File(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr) && wr == text.size();
    CloseHandle(f);
    return ok;
}

// ---- displays and windows ----

struct Display {
    RECT rect;
    bool primary;
    float scale;
};

std::vector<Display> Displays() {
    std::vector<Display> out;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(m, &mi)) {
            POINT c{(mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2};
            reinterpret_cast<std::vector<Display>*>(p)->push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, DpiScaleAt(c)});
        }
        return TRUE;
    }, (LPARAM)&out);
    // The primary display is 0, then left to right.
    std::stable_sort(out.begin(), out.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    return out;
}

struct WindowEntry {
    HWND hwnd;
    RECT frame;
    std::wstring title, app;
    bool minimized;
};

// Visible top-level app windows, front to back (minimized ones too: they can be brought back).
std::vector<WindowEntry> AppWindows() {
    std::vector<HWND> all;
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(p)->push_back(h);
        return TRUE;
    }, (LPARAM)&all);
    std::vector<WindowEntry> out;
    for (HWND h : all) {
        if (!IsWindowVisible(h) || IsOwnWindow(h) || GetWindow(h, GW_OWNER)) continue;
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) continue;
        int cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        std::wstring title = WindowTitle(h);
        if (title.empty()) continue;
        RECT r{};
        const bool minimized = IsIconic(h) != 0;
        if (!GetWindowFrame(h, &r) || (!minimized && (RectW(r) < 2 || RectH(r) < 2))) continue;
        out.push_back({h, r, title, WindowAppName(h), minimized});
    }
    return out;
}

Json WindowJson(const WindowEntry& w) {
    Json j = Json::Object();
    j.Set("id", (int64_t)(intptr_t)w.hwnd);
    j.Set("title", w.title);
    j.Set("app", w.app);
    j.Set("rect", Rect(w.frame));
    j.Set("minimized", w.minimized);
    j.Set("active", GetForegroundWindow() == w.hwnd);
    return j;
}

// The window `args` names (window_id, else window_title matched against titles and app names), else the active one.
bool FindWindowFor(const Json& args, WindowEntry* out, std::string* error) {
    const auto list = AppWindows();
    if (args.Has("window_id")) {
        const HWND want = (HWND)(intptr_t)args["window_id"].Int();
        for (const auto& w : list)
            if (w.hwnd == want) return *out = w, true;
        return *error = "No window with id " + std::to_string(args["window_id"].Int()) + " (call list_windows for current ids).", false;
    }
    if (const std::wstring q = LowerW(args["window_title"].WStr()); !q.empty()) {
        const WindowEntry* best = nullptr;
        for (const auto& w : list) {  // exact title, then title contains, then app name contains
            const std::wstring t = LowerW(w.title), a = LowerW(w.app);
            if (t == q) return *out = w, true;
            if (!best && (t.find(q) != std::wstring::npos || a.find(q) != std::wstring::npos)) best = &w;
        }
        if (best) return *out = *best, true;
        return *error = "No window matches \"" + args["window_title"].Str() + "\" (call list_windows to see them).", false;
    }
    const HWND fg = GetForegroundWindow();
    for (const auto& w : list)
        if (w.hwnd == fg) return *out = w, true;
    return *error = "There's no active window. Name one with window_id or window_title.", false;
}

// ---- targets: the screen, a display, a window or a region ----

struct Target {
    RECT rect{};
    HWND window = nullptr;  // set for "window"
    CaptureNameInfo info;
    std::string kind;
};

bool ResolveTarget(const Json& args, Target* t, std::string* error) {
    t->kind = args["target"].Str().empty() ? "screen" : args["target"].Str();
    const RECT virt = VirtualScreenRect();
    if (t->kind == "screen") {
        t->rect = virt;
    } else if (t->kind == "display") {
        const auto ds = Displays();
        const int i = (int)args["display"].Int(0);
        if (i < 0 || i >= (int)ds.size())
            return *error = "There is no display " + std::to_string(i) + " (there are " + std::to_string(ds.size()) + "; 0 is the primary).", false;
        t->rect = ds[i].rect;
    } else if (t->kind == "window") {
        WindowEntry w;
        if (!FindWindowFor(args, &w, error)) return false;
        if (w.minimized) {
            if (!args["bring_to_front"].Bool(false)) return *error = "\"" + Utf8(w.title) + "\" is minimized. Pass bring_to_front: true to restore it.", false;
            ShowWindow(w.hwnd, SW_RESTORE);
            Sleep(250);  // the restore animation
            GetWindowFrame(w.hwnd, &w.frame);
        }
        t->window = w.hwnd;
        t->rect = w.frame;
        t->info = {w.title, w.app};
    } else if (t->kind == "region") {
        for (const char* k : {"x", "y", "width", "height"})
            if (!args[k].IsNumber()) return *error = "A region needs x, y, width and height (screen pixels).", false;
        const int x = (int)args["x"].Int(), y = (int)args["y"].Int();
        RECT r{x, y, x + (int)args["width"].Int(), y + (int)args["height"].Int()};
        if (!IntersectRect(&t->rect, &r, &virt)) return *error = "The region is off screen. get_status lists the displays.", false;
    } else {
        return *error = "target must be screen, display, window or region.", false;
    }
    if (t->info.window.empty()) {  // name files after the app in front, like the hotkeys do
        HWND fg = GetForegroundWindow();
        if (fg && !IsOwnWindow(fg)) t->info = {WindowTitle(fg), WindowAppName(fg)};
    }
    return true;
}

// A covered window, drawn by itself (most apps; some games draw black this way).
BitmapPtr PrintWindowFrame(HWND hwnd, const RECT& frame) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr) || RectW(wr) <= 0 || RectH(wr) <= 0) return nullptr;
    auto full = Bitmap::Create(RectW(wr), RectH(wr));
    if (!full) return nullptr;
    {
        MemDC dc(full->Handle());
        if (!PrintWindow(hwnd, dc, 2 /* PW_RENDERFULLCONTENT */)) return nullptr;
    }
    const size_t n = (size_t)full->Width() * full->Height();
    uint32_t* px = full->Bits();
    if (std::all_of(px, px + n, [](uint32_t p) { return (p & 0xFFFFFF) == 0; })) return nullptr;  // drew nothing
    for (size_t i = 0; i < n; ++i) px[i] |= 0xFF000000u;
    return full->Crop({frame.left - wr.left, frame.top - wr.top, frame.right - wr.left, frame.bottom - wr.top});
}

BitmapPtr Grab(const Target& t, const Json& args) {
    const bool cursor = args["include_cursor"].Bool(g_host.settings->captureCursor);
    if (HideToast()) DwmFlush();  // our own notification must not be in the shot
    if (t.window) {
        const bool front = args["bring_to_front"].Bool(false);
        if (front && GetForegroundWindow() != t.window) {
            SetForegroundWindow(t.window);
            Sleep(200);
            DwmFlush();
        }
        if (GetForegroundWindow() != t.window)  // covered, maybe: let it draw itself
            if (BitmapPtr img = PrintWindowFrame(t.window, t.rect)) return img;
    }
    return CaptureScreen(t.rect, cursor);
}

// ---- screenshots ----

struct ShotOptions {
    bool save = true, clipboard = false, redact = false;
    std::vector<std::wstring> tags;
    std::wstring comment;
    int side = 1568;
};

void Deliver(BitmapPtr img, Target t, ShotOptions o, int redacted, Reply reply) {
    if (o.redact && redacted < 0) {
        RecognizeWordsAsync(img, [img, t, o, reply](std::vector<OcrWord> words, std::wstring err) {
            if (!err.empty()) return reply(ErrorResult("Auto-redact failed, so nothing was saved: " + Utf8(err)));
            auto clean = img->Crop({0, 0, img->Width(), img->Height()});
            int n = 0;
            for (const RECT& r : FindSensitive(words)) {
                clean->Pixelate(r, std::max(6, RectH(r) / 3));
                ++n;
            }
            Deliver(clean, t, o, n, reply);
        });
        return;
    }
    Json d = Json::Object();
    d.Set("target", t.kind);
    d.Set("rect", Rect(t.rect));
    d.Set("width", img->Width());
    d.Set("height", img->Height());
    if (redacted >= 0) d.Set("redacted", redacted);
    if (o.clipboard) d.Set("copied_to_clipboard", CopyImageToClipboard(nullptr, *img));
    if (!o.save) {
        g_host.setLast(img, L"");
        d.Set("saved", false);
        return reply(JsonResult(d, Picture(img, o.side)));
    }
    CaptureNameInfo info = t.info;
    info.w = img->Width();
    info.h = img->Height();
    const std::wstring path = MakeCapturePath(g_host.settings->CapturesFolder(), L"png", info);
    Library::Shared().NoteCapture(path, info.app, info.window);
    Library::Shared().NoteCaptureDetails(path, o.tags, o.comment);
    SavePngAsync(img, path, [img, path, d, o, reply](bool ok) mutable {
        if (!ok) return reply(ErrorResult("Couldn't save " + Utf8(path)));
        g_host.setLast(img, path);
        d.Set("saved", true);
        d.Set("path", path);
        if (!o.tags.empty()) d.Set("tags", Strings(o.tags));
        reply(JsonResult(d, Picture(img, o.side)));
    });
}

// Runs `fn` on the UI thread after `seconds` (0: now).
void After(double seconds, std::function<void()> fn) {
    if (seconds <= 0) return fn();
    std::thread([seconds, fn = std::move(fn)]() mutable {
        Sleep((DWORD)(seconds * 1000));
        RunOnUi(std::move(fn));
    }).detach();
}

void TakeScreenshot(const Json& args, Reply reply) {
    Target t;
    std::string err;
    if (!ResolveTarget(args, &t, &err)) return reply(ErrorResult(err));
    ShotOptions o;
    o.save = args["save"].Bool(true);
    o.clipboard = args["copy_to_clipboard"].Bool(false);
    o.redact = args["redact"].Bool(g_host.settings->autoRedact);
    o.tags = StringList(args["tags"]);
    o.comment = args["comment"].WStr();
    o.side = ImageSide(args);
    After(std::clamp(args["delay_seconds"].Num(0), 0.0, 60.0), [t, args, o, reply] {
        Target now = t;
        if (now.window) GetWindowFrame(now.window, &now.rect);  // it may have moved while we waited
        BitmapPtr img = Grab(now, args);
        if (!img) return reply(ErrorResult("The capture failed."));
        Deliver(img, now, o, -1, reply);
    });
}

// ---- recording ----

struct Marker {
    double seconds;
    std::wstring label;
    NoteKind kind;
};

struct AgentRecording {
    bool ours = false;  // started by an agent (its result comes back here)
    std::wstring path;
    RecordFormat format = RecordFormat::Mp4;
    std::vector<Marker> markers;
    std::wstring client;
    uint64_t generation = 0;     // which recording a max_seconds stop belongs to
    std::optional<Json> result;  // the finished recording, until an agent reads it
    std::vector<Reply> waiting;  // stop_recording calls waiting for the file
};
AgentRecording g_rec;
uint64_t g_recGeneration = 0;
std::wstring g_client = L"AI agent";  // the last client that connected (note author)

NoteKind KindOf(const std::string& s) { return NoteKindOf(s); }

// Markers become the video's review notes ("<video>.notes.json"), where the video editor shows them on their frames.
void WriteMarkers(const std::wstring& video, std::vector<Marker> markers, std::wstring author) {
    if (markers.empty()) return;
    std::thread([video, markers = std::move(markers), author = std::move(author)] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const std::vector<double> times = FrameTimes(video);
        std::vector<Note> notes;
        for (const auto& m : markers) {
            Note n;
            n.path = video;
            n.src = m.seconds;
            if (!times.empty()) {  // the frame on screen at that moment
                auto it = std::upper_bound(times.begin(), times.end(), m.seconds + 1e-6);
                n.src = it == times.begin() ? times.front() : *(it - 1);
            }
            n.text = m.label;
            n.author = author;
            n.kind = m.kind;
            n.created = (int64_t)time(nullptr);
            notes.push_back(std::move(n));
        }
        WriteUtf8File(NotesPath(video), NotesJson(video, notes, times, 30));
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

Json MarkersJson(const std::vector<Marker>& ms) {
    Json a = Json::Array();
    for (const auto& m : ms) {
        Json j = Json::Object();
        j.Set("seconds", std::round(m.seconds * 1000) / 1000);
        j.Set("label", m.label);
        j.Set("kind", NoteKindKey(m.kind));
        a.Push(std::move(j));
    }
    return a;
}

void Finished(Json result) {
    auto waiting = std::move(g_rec.waiting);
    g_rec.waiting.clear();
    if (waiting.empty()) g_rec.result = result;  // ended by the user or max_seconds: the next stop_recording reports it
    else g_rec.result.reset();
    for (auto& w : waiting) w(JsonResult(result));
}

void OnAgentRecordingDone(const RecordResult& r) {
    Json d = Json::Object();
    if (r.discarded) {
        d.Set("discarded", true);
    } else if (!r.ok) {
        auto waiting = std::move(g_rec.waiting);
        g_rec.waiting.clear();
        for (auto& w : waiting) w(ErrorResult("The recording failed: " + Utf8(r.error)));
        if (waiting.empty()) {
            d.Set("error", r.error);
            g_rec.result = d;
        }
        ShowToast(L"Recording failed", r.error, nullptr, nullptr, 5000);
        g_rec.ours = false;
        g_rec.markers.clear();
        return;
    } else {
        d.Set("path", r.path);
        d.Set("format", r.format == RecordFormat::Gif ? "gif" : "mp4");
        d.Set("seconds", std::round(r.seconds * 100) / 100);
        d.Set("bytes", (int64_t)r.bytes);
        if (!r.warning.empty()) d.Set("warning", r.warning);
        d.Set("markers", MarkersJson(g_rec.markers));
        if (!g_rec.markers.empty() && r.format == RecordFormat::Mp4) {
            d.Set("notes_file", NotesPath(r.path));
            WriteMarkers(r.path, g_rec.markers, g_rec.client);
        }
        g_host.setLast(nullptr, r.path);
        ShowToast(L"Recording saved", FileNameOf(r.path) + L"\nRecorded by " + g_rec.client, r.thumb,
                  [path = r.path] { g_host.openToEdit(path); }, 4000);
    }
    g_rec.ours = false;  // a recording started later from a shortcut isn't the agent's
    g_rec.markers.clear();
    Finished(d);
}

Json RecordingState() {
    Json j = Json::Object();
    const char* state = IsRecording() ? (RecordingSeconds() <= 0 && !IsRecordingPaused() ? "starting" : IsRecordingPaused() ? "paused" : "recording")
                        : RecorderBusy() ? "saving" : "idle";
    j.Set("state", state);
    if (IsRecording()) {
        j.Set("seconds", std::round(RecordingSeconds() * 100) / 100);
        j.Set("started_by_agent", g_rec.ours);
        if (g_rec.ours) j.Set("path", g_rec.path);
        j.Set("markers", MarkersJson(g_rec.markers));
    }
    if (g_rec.result) j.Set("finished_unread", *g_rec.result);
    return j;
}

void StartRecordingTool(const Json& args, Reply reply) {
    if (IsRecording()) return reply(ErrorResult("A recording is already running. Call stop_recording first."));
    if (RecorderBusy()) return reply(ErrorResult("The previous recording is still being saved. Try again in a moment."));
    Target t;
    std::string err;
    if (!ResolveTarget(args, &t, &err)) return reply(ErrorResult(err));
    if (RectW(t.rect) < 16 || RectH(t.rect) < 16) return reply(ErrorResult("The area is too small to record (16 × 16 at least)."));
    const Settings& s = *g_host.settings;
    const bool gif = args["format"].Str() == "gif";
    RecordOptions o;
    o.rect = t.rect;
    o.window = t.window && args["follow_window"].Bool(true) ? t.window : nullptr;
    o.format = gif ? RecordFormat::Gif : RecordFormat::Mp4;
    o.fps = std::clamp((int)args["fps"].Int(gif ? s.gifFps : s.videoFps), 1, gif ? 50 : 60);
    o.cursor = args["include_cursor"].Bool(s.recordCursor);
    o.systemAudio = args["system_audio"].Bool(s.systemAudio);
    o.microphone = args["microphone"].Bool(s.microphone);
    o.gpuCapture = s.gpuCapture;
    o.showClicks = args["show_clicks"].Bool(s.showClicks);
    o.showKeys = args["show_keys"].Bool(s.showKeys);
    o.showGamepad = args["show_gamepad"].Bool(s.showGamepad);
    o.gamepadCorner = ParsePadCorner(s.gamepadCorner);
    o.gamepadOpacity = s.gamepadOpacity / 100.f;
    o.countdownSeconds = std::clamp((int)args["countdown_seconds"].Int(0), 0, 10);
    CaptureNameInfo info = t.info;
    info.w = RectW(t.rect);
    info.h = RectH(t.rect);
    o.path = MakeCapturePath(s.CapturesFolder(), gif ? L"gif" : L"mp4", info);
    Library::Shared().NoteCapture(o.path, info.app, info.window);
    Library::Shared().NoteCaptureDetails(o.path, StringList(args["tags"]), args["comment"].WStr());
    std::wstring error;
    const uint64_t gen = ++g_recGeneration;
    if (!StartRecording(o, [](const RecordResult& r) { OnAgentRecordingDone(r); }, &error))
        return reply(ErrorResult("Couldn't start recording: " + Utf8(error)));
    g_rec = AgentRecording{};
    g_rec.ours = true;
    g_rec.path = o.path;
    g_rec.format = o.format;
    g_rec.client = g_client;
    g_rec.generation = gen;
    const double maxSeconds = args["max_seconds"].Num(0);
    if (maxSeconds > 0)
        After(maxSeconds + o.countdownSeconds, [gen] {
            if (g_rec.generation == gen && IsRecording()) StopRecording(false);
        });
    Json d = Json::Object();
    d.Set("recording", true);
    d.Set("path", o.path);
    d.Set("format", gif ? "gif" : "mp4");
    d.Set("target", t.kind);
    d.Set("rect", Rect(t.rect));
    if (t.window) d.Set("follows_window", o.window != nullptr);
    d.Set("fps", o.fps);
    d.Set("countdown_seconds", o.countdownSeconds);
    if (maxSeconds > 0) d.Set("max_seconds", maxSeconds);
    reply(JsonResult(d));
}

// A recording someone else started: its file is the newest recording once the recorder is done.
void WaitForRecorder(std::vector<Marker> markers, bool discard, Reply reply, int triesLeft = 2400) {
    if (RecorderBusy() && triesLeft > 0)
        return After(0.25, [markers = std::move(markers), discard, reply, triesLeft]() mutable {
            WaitForRecorder(std::move(markers), discard, reply, triesLeft - 1);
        });
    Json d = Json::Object();
    if (discard) {
        d.Set("discarded", true);
        return reply(JsonResult(d));
    }
    const auto recent = RecentCaptures(g_host.settings->CapturesFolder(), 8, true);
    auto it = std::find_if(recent.begin(), recent.end(), [](const std::wstring& p) { return MediaTypeOf(p) != MediaType::Image; });
    if (it == recent.end()) return reply(ErrorResult("The recording stopped, but its file wasn't found."));
    d.Set("path", *it);
    d.Set("markers", MarkersJson(markers));
    if (!markers.empty() && MediaTypeOf(*it) == MediaType::Video) {
        d.Set("notes_file", NotesPath(*it));
        WriteMarkers(*it, markers, g_client);
    }
    reply(JsonResult(d));
}

void StopRecordingTool(const Json& args, Reply reply) {
    const bool discard = args["discard"].Bool(false);
    if (!IsRecording()) {
        if (g_rec.result) {  // it already ended (the user stopped it, or max_seconds)
            Json r = *g_rec.result;
            g_rec.result.reset();
            if (r.Has("error")) return reply(ErrorResult("The recording failed: " + r["error"].Str()));
            return reply(JsonResult(r));
        }
        if (RecorderBusy() && g_rec.ours) return g_rec.waiting.push_back(reply);  // still saving
        return reply(ErrorResult("Nothing is being recorded."));
    }
    if (g_rec.ours) {
        g_rec.waiting.push_back(reply);
        StopRecording(discard);
        return;
    }
    auto markers = std::move(g_rec.markers);
    g_rec.markers.clear();
    StopRecording(discard);
    WaitForRecorder(std::move(markers), discard, reply);
}

void PauseRecordingTool(const Json& args, Reply reply) {
    if (!IsRecording()) return reply(ErrorResult("Nothing is being recorded."));
    const bool want = args["paused"].Bool(true);
    if (IsRecordingPaused() != want) TogglePauseRecording();
    Json d = Json::Object();
    d.Set("paused", IsRecordingPaused());
    d.Set("seconds", std::round(RecordingSeconds() * 100) / 100);
    if (IsRecordingPaused() != want) d.Set("note", "The recording hasn't started yet (countdown).");
    reply(JsonResult(d));
}

void AddMarkerTool(const Json& args, Reply reply) {
    if (!IsRecording()) return reply(ErrorResult("Markers go on a running recording, and nothing is being recorded."));
    Marker m{args.Has("seconds") ? std::max(0.0, args["seconds"].Num()) : RecordingSeconds(), args["label"].WStr(),
             KindOf(args["kind"].Str())};
    if (m.label.empty()) return reply(ErrorResult("A marker needs a label."));
    g_rec.markers.push_back(m);
    Json d = Json::Object();
    d.Set("seconds", std::round(m.seconds * 1000) / 1000);
    d.Set("label", m.label);
    d.Set("kind", NoteKindKey(m.kind));
    d.Set("markers", (int)g_rec.markers.size());
    reply(JsonResult(d));
}

// ---- the gallery ----

Json CaptureJson(const std::wstring& path, const ItemMeta& m) {
    Json j = Json::Object();
    j.Set("path", path);
    j.Set("type", std::wstring(MediaTypeName(MediaTypeOf(path))));
    if (m.w) {
        j.Set("width", m.w);
        j.Set("height", m.h);
    }
    if (m.duration) j.Set("duration_seconds", std::round(*m.duration * 100) / 100);
    j.Set("bytes", (int64_t)m.size);
    j.Set("modified", IsoTime(m.mtime));
    j.Set("tags", Strings(m.tags));
    if (m.rating) j.Set("rating", m.rating);
    if (!m.comment.empty()) j.Set("comment", m.comment);
    if (!m.app.empty()) j.Set("app", m.app);
    if (!m.window.empty()) j.Set("window", m.window);
    if (m.text && !m.text->empty()) j.Set("text", m.text->size() > 300 ? m.text->substr(0, 300) + L"…" : *m.text);
    if (MediaTypeOf(path) == MediaType::Video && FileExists(NotesPath(path))) j.Set("notes_file", NotesPath(path));
    return j;
}

void ListCapturesTool(const Json& args, Reply reply) {
    Library::Shared().Refresh([args, reply] {
        Library& lib = Library::Shared();
        Filter f;
        f.text = args["query"].WStr();
        const std::string type = args["type"].Str();
        if (type == "image") f.types = {MediaType::Image};
        else if (type == "gif") f.types = {MediaType::Gif};
        else if (type == "video") f.types = {MediaType::Video};
        f.tags = StringList(args["tags"]);
        f.minRating = std::clamp((int)args["min_rating"].Int(0), 0, 5);
        std::vector<std::wstring> hits;
        for (const auto& p : lib.Paths())
            if (f.Matches(p, lib.Meta(p))) hits.push_back(p);
        std::stable_sort(hits.begin(), hits.end(), [&](const std::wstring& a, const std::wstring& b) { return lib.Meta(a).mtime > lib.Meta(b).mtime; });
        const size_t offset = (size_t)std::max<int64_t>(0, args["offset"].Int(0));
        const size_t limit = (size_t)std::clamp<int64_t>(args["limit"].Int(20), 1, 200);
        Json items = Json::Array();
        for (size_t i = offset; i < hits.size() && i < offset + limit; ++i) items.Push(CaptureJson(hits[i], lib.Meta(hits[i])));
        Json d = Json::Object();
        d.Set("total", (int64_t)hits.size());
        d.Set("captures", std::move(items));
        d.Set("captures_folder", g_host.settings->CapturesFolder());
        reply(JsonResult(d));
    });
}

bool CheckMediaPath(const std::wstring& path, std::string* error) {
    if (path.empty()) return *error = "Pass the file's path (list_captures gives them).", false;
    if (!FileExists(path)) return *error = "File not found: " + Utf8(path), false;
    if (!IsMediaFile(path)) return *error = "Not a picture or video: " + Utf8(path), false;
    return true;
}

// Off the UI thread: decoding a big file must not freeze the app.
void GetCaptureTool(const Json& args, Reply reply) {
    const std::wstring path = args["path"].WStr();
    std::string err;
    if (!CheckMediaPath(path, &err)) return reply(ErrorResult(err));
    const int side = args.Has("max_size") ? std::max(0, (int)args["max_size"].Int()) : 1568;
    Json d = Json::Object();
    d.Set("path", path);
    d.Set("type", std::wstring(MediaTypeName(MediaTypeOf(path))));
    BitmapPtr img;
    if (MediaTypeOf(path) == MediaType::Video) {
        VideoInfo vi;
        const double at = std::max(0.0, args["time_seconds"].Num(0));
        if (!ProbeVideo(path, &vi, at, side, &img) || !img) return reply(ErrorResult("Couldn't read a frame from " + Utf8(path)));
        d.Set("width", vi.w);
        d.Set("height", vi.h);
        d.Set("duration_seconds", std::round(vi.duration * 100) / 100);
        d.Set("fps", std::round(vi.fps * 100) / 100);
        d.Set("time_seconds", std::min(at, vi.duration));
    } else {
        int w = 0, h = 0;
        img = LoadImageScaled(path, side, &w, &h);
        if (!img) return reply(ErrorResult("Couldn't open " + Utf8(path)));
        d.Set("width", w);
        d.Set("height", h);
        if (MediaTypeOf(path) == MediaType::Gif) d.Set("duration_seconds", std::round(GifDuration(path) * 100) / 100);
    }
    Json extra = Json::Array();
    extra.Push(mcp::ImageContent(EncodePng(*img)));
    reply(JsonResult(d, extra));
}

// Frames side by side, `cols` across, each fitted into a `cell` × `cellH` box.
BitmapPtr ContactSheetOf(const std::vector<BitmapPtr>& frames, int cols, int cell) {
    if (frames.empty() || !frames[0]) return nullptr;
    const double aspect = (double)frames[0]->Width() / std::max(1, frames[0]->Height());
    const int cellH = std::max(1, (int)std::lround(cell / aspect)), gap = 4;
    const int rows = ((int)frames.size() + cols - 1) / cols;
    auto sheet = Bitmap::Create(cols * cell + (cols + 1) * gap, rows * cellH + (rows + 1) * gap);
    if (!sheet) return nullptr;
    std::fill_n(sheet->Bits(), (size_t)sheet->Width() * sheet->Height(), 0xFF101010u);
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!frames[i]) continue;
        BitmapPtr f = FitInto(*frames[i], cell, cellH);
        if (!f) continue;
        const int x0 = gap + (int)(i % cols) * (cell + gap), y0 = gap + (int)(i / cols) * (cellH + gap);
        for (int y = 0; y < f->Height(); ++y)
            memcpy(sheet->Bits() + (size_t)(y0 + y) * sheet->Width() + x0, f->Bits() + (size_t)y * f->Width(), (size_t)f->Width() * 4);
    }
    return sheet;
}

void VideoFramesTool(const Json& args, Reply reply) {
    const std::wstring path = args["path"].WStr();
    std::string err;
    if (!CheckMediaPath(path, &err)) return reply(ErrorResult(err));
    if (MediaTypeOf(path) != MediaType::Video) return reply(ErrorResult("Not a video. Use get_capture for pictures and GIFs."));
    VideoInfo vi;
    if (!ProbeVideo(path, &vi) || vi.duration <= 0) return reply(ErrorResult("Couldn't read " + Utf8(path)));
    std::vector<double> times;
    for (const auto& t : args["times"].Items())
        if (t.IsNumber()) times.push_back(std::clamp(t.Num(), 0.0, vi.duration));
    if (times.empty()) {  // evenly spread over [start, end]
        const int count = std::clamp((int)args["count"].Int(6), 1, 16);
        const double a = std::clamp(args["start_seconds"].Num(0), 0.0, vi.duration);
        const double b = std::clamp(args["end_seconds"].Num(vi.duration), a, vi.duration);
        for (int i = 0; i < count; ++i) times.push_back(count == 1 ? a : a + (b - a) * (i + 0.5) / count);
    }
    if (times.size() > 16) times.resize(16);
    const bool sheet = args["layout"].Str() == "sheet";
    const int side = std::clamp((int)args["max_size"].Int(sheet ? 1568 : 768), 64, 4096);
    const int cols = sheet ? (int)std::ceil(std::sqrt((double)times.size())) : 1;
    std::vector<BitmapPtr> frames;
    Json list = Json::Array();
    for (double t : times) {
        BitmapPtr f;
        VideoInfo one;
        if (!ProbeVideo(path, &one, t, sheet ? std::max(64, side / cols) : side, &f) || !f) continue;
        frames.push_back(f);
        list.Push(std::round(t * 1000) / 1000);
    }
    if (frames.empty()) return reply(ErrorResult("Couldn't decode frames from " + Utf8(path)));
    Json d = Json::Object();
    d.Set("path", path);
    d.Set("duration_seconds", std::round(vi.duration * 100) / 100);
    d.Set("width", vi.w);
    d.Set("height", vi.h);
    d.Set("fps", std::round(vi.fps * 100) / 100);
    d.Set("times_seconds", list);
    Json extra = Json::Array();
    if (sheet) {
        d.Set("layout", "sheet, left to right then top to bottom, " + std::to_string(cols) + " across");
        if (BitmapPtr s = ContactSheetOf(frames, cols, side / cols)) extra.Push(mcp::ImageContent(EncodePng(*s)));
    } else {
        for (const auto& f : frames) extra.Push(mcp::ImageContent(EncodePng(*f)));
    }
    reply(JsonResult(d, extra));
}

void TagCaptureTool(const Json& args, Reply reply) {
    const std::wstring path = args["path"].WStr();
    Library& lib = Library::Shared();
    if (!lib.IsInLibrary(path) || !FileExists(path)) return reply(ErrorResult("Not a capture in the gallery: " + Utf8(path)));
    if (auto add = StringList(args["add_tags"]); !add.empty()) lib.AddTags(add, {path});
    for (const auto& t : StringList(args["remove_tags"])) lib.RemoveTag(t, {path});
    if (args.Has("rating")) lib.SetRating(std::clamp((int)args["rating"].Int(), 0, 5), {path});
    if (args.Has("comment")) lib.SetComment(args["comment"].WStr(), path);
    reply(JsonResult(CaptureJson(path, lib.Meta(path))));
}

// ---- OCR ----

void OcrTool(const Json& args, Reply reply) {
    BitmapPtr img;
    POINT origin{0, 0};
    Json d = Json::Object();
    if (const std::wstring path = args["path"].WStr(); !path.empty()) {
        std::string err;
        if (!CheckMediaPath(path, &err)) return reply(ErrorResult(err));
        if (MediaTypeOf(path) == MediaType::Video) {
            VideoInfo vi;
            ProbeVideo(path, &vi, std::max(0.0, args["time_seconds"].Num(0)), 0, &img);
        } else {
            img = LoadImageFile(path);
        }
        d.Set("path", path);
    } else {
        Target t;
        std::string err;
        if (!ResolveTarget(args, &t, &err)) return reply(ErrorResult(err));
        img = Grab(t, args);
        origin = {t.rect.left, t.rect.top};
        d.Set("rect", Rect(t.rect));
    }
    if (!img) return reply(ErrorResult("Nothing to read: the picture couldn't be loaded or captured."));
    const bool boxes = args["with_boxes"].Bool(false);
    RecognizeWordsAsync(img, [d, boxes, origin, reply](std::vector<OcrWord> words, std::wstring err) mutable {
        if (!err.empty()) return reply(ErrorResult("OCR failed: " + Utf8(err)));
        std::wstring text;
        int line = -1;
        Json list = Json::Array();
        for (const auto& w : words) {
            if (!text.empty()) text += w.line != line ? L"\n" : L" ";
            text += w.text;
            line = w.line;
            if (boxes) {
                RECT r = w.rect;
                OffsetRect(&r, origin.x, origin.y);
                Json j = Rect(r);
                j.Set("text", w.text);
                j.Set("line", w.line);
                list.Push(std::move(j));
            }
        }
        d.Set("text", text);
        if (boxes) {
            d.Set("words", std::move(list));
            d.Set("coordinates", origin.x || origin.y || d.Has("rect") ? "screen pixels" : "image pixels");
        }
        reply(JsonResult(d));
    });
}

// ---- settings ----

struct SettingKey {
    const char* name;
    const wchar_t* section;
    const wchar_t* key;
    bool boolean;
    int min, max;
};
const SettingKey kSettings[] = {
    {"copy_to_clipboard", L"Capture", L"CopyToClipboard", true, 0, 1},
    {"save_to_file", L"Capture", L"SaveToFile", true, 0, 1},
    {"capture_cursor", L"Capture", L"CaptureCursor", true, 0, 1},
    {"auto_redact", L"Capture", L"AutoRedact", true, 0, 1},
    {"show_notifications", L"Interface", L"ShowToast", true, 0, 1},
    {"video_fps", L"Recording", L"VideoFps", false, 1, 60},
    {"gif_fps", L"Recording", L"GifFps", false, 1, 50},
    {"countdown_seconds", L"Recording", L"CountdownSeconds", false, 0, 10},
    {"record_cursor", L"Recording", L"RecordCursor", true, 0, 1},
    {"system_audio", L"Recording", L"RecordSystemAudio", true, 0, 1},
    {"microphone", L"Recording", L"RecordMicrophone", true, 0, 1},
    {"show_clicks", L"Recording", L"ShowClicks", true, 0, 1},
    {"show_keys", L"Recording", L"ShowKeys", true, 0, 1},
    {"show_gamepad", L"Recording", L"ShowGamepad", true, 0, 1},
    {"gamepad_opacity", L"Recording", L"GamepadOpacity", false, 10, 100},
};

Json SettingsJson() {
    Json j = Json::Object();
    const Settings& s = *g_host.settings;
    for (const auto& k : kSettings) {
        const std::wstring v = s.Raw(k.section, k.key, L"");
        if (k.boolean) j.Set(k.name, v == L"1" || _wcsicmp(v.c_str(), L"true") == 0);
        else j.Set(k.name, _wtoi(v.c_str()));
    }
    j.Set("gamepad_corner", s.gamepadCorner);
    j.Set("after_capture", s.afterCapture);
    return j;
}

void ConfigureTool(const Json& args, Reply reply) {
    Settings& s = *g_host.settings;
    Json changed = Json::Array();
    for (const auto& k : kSettings) {
        if (!args.Has(k.name)) continue;
        if (k.boolean) s.WriteBool(k.section, k.key, args[k.name].Bool());
        else s.WriteString(k.section, k.key, std::to_wstring(std::clamp((int)args[k.name].Int(k.min), k.min, k.max)));
        changed.Push(k.name);
    }
    if (const std::string c = args["gamepad_corner"].Str(); !c.empty()) {
        if (c != "topleft" && c != "topright" && c != "bottomleft" && c != "bottomright")
            return reply(ErrorResult("gamepad_corner is topleft, topright, bottomleft or bottomright."));
        s.WriteString(L"Recording", L"GamepadCorner", FromUtf8(c));
        changed.Push("gamepad_corner");
    }
    g_host.settingsChanged();
    Json d = Json::Object();
    d.Set("changed", std::move(changed));
    d.Set("settings", SettingsJson());
    reply(JsonResult(d));
}

// ---- status ----

void StatusTool(const Json&, Reply reply) {
    Json d = Json::Object();
    d.Set("app", "Ather Screenshot");
    d.Set("version", ATHER_BUILD_STR);
    d.Set("platform", "windows");
    d.Set("captures_folder", g_host.settings->CapturesFolder());
    d.Set("recording", RecordingState());
    Json ds = Json::Array();
    int i = 0;
    for (const auto& disp : Displays()) {
        Json j = Rect(disp.rect);
        j.Set("index", i++);
        j.Set("primary", disp.primary);
        j.Set("scale", std::round(disp.scale * 100) / 100);
        ds.Push(std::move(j));
    }
    d.Set("displays", std::move(ds));
    d.Set("virtual_screen", Rect(VirtualScreenRect()));
    WindowEntry w;
    std::string ignore;
    if (FindWindowFor(Json::Object(), &w, &ignore)) d.Set("active_window", WindowJson(w));
    d.Set("settings", SettingsJson());
    d.Set("coordinates", "Physical screen pixels. The primary display's top-left is 0,0; others can be negative.");
    reply(JsonResult(d));
}

void ListWindowsTool(const Json& args, Reply reply) {
    const std::wstring q = LowerW(args["query"].WStr());
    Json list = Json::Array();
    for (const auto& w : AppWindows())
        if (q.empty() || LowerW(w.title).find(q) != std::wstring::npos || LowerW(w.app).find(q) != std::wstring::npos)
            list.Push(WindowJson(w));
    Json d = Json::Object();
    d.Set("windows", std::move(list));
    reply(JsonResult(d));
}

// ---- the rest ----

void OpenInEditorTool(const Json& args, Reply reply) {
    const std::wstring path = args["path"].WStr();
    std::string err;
    if (!CheckMediaPath(path, &err)) return reply(ErrorResult(err));
    g_host.openToEdit(path);
    Json d = Json::Object();
    d.Set("opened", path);
    d.Set("editor", MediaTypeOf(path) == MediaType::Video ? "video" : "image");
    reply(JsonResult(d));
}

void UploadTool(const Json& args, Reply reply) {
    const std::wstring path = args["path"].WStr();
    std::string err;
    if (!CheckMediaPath(path, &err)) return reply(ErrorResult(err));
    const UploadConfig cfg = g_host.uploadConfig();
    if (std::wstring problem = UploadConfigProblem(cfg); !problem.empty())
        return reply(ErrorResult("Upload isn't set up: " + Utf8(problem) + " (Settings › Upload)."));
    UploadFileAsync(path, cfg, [path, reply](std::wstring url, std::wstring e) {
        if (!e.empty()) return reply(ErrorResult("Upload failed: " + Utf8(e)));
        Json d = Json::Object();
        d.Set("path", path);
        d.Set("url", url);
        reply(JsonResult(d));
    });
}

void RunCommandTool(const Json& args, Reply reply) {
    const std::wstring want = args["command"].WStr();
    for (const auto& c : g_host.commands)
        if (_wcsicmp(c.key.c_str(), want.c_str()) == 0) {
            g_host.runCommand(c.key);
            Json d = Json::Object();
            d.Set("ran", c.key);
            d.Set("title", c.title);
            return reply(JsonResult(d));
        }
    return reply(ErrorResult("Unknown command: " + Utf8(want)));
}

// ---- the tool list ----

const char kTargetProps[] = R"(
    "target": {"type": "string", "enum": ["screen", "display", "window", "region"], "description": "screen = every display; display = one display (see display); window = one app window (see window_id / window_title, else the active window); region = x, y, width, height. Default: screen."},
    "display": {"type": "integer", "minimum": 0, "description": "Display index from get_status (0 = primary)."},
    "window_id": {"type": "integer", "description": "Window id from list_windows."},
    "window_title": {"type": "string", "description": "Part of a window title or app name, e.g. the game's name."},
    "bring_to_front": {"type": "boolean", "description": "Activate (and restore) the window first. Default false: a covered window is drawn by itself, which some games show as black."},
    "x": {"type": "integer"}, "y": {"type": "integer"}, "width": {"type": "integer", "minimum": 1}, "height": {"type": "integer", "minimum": 1},
    "include_cursor": {"type": "boolean", "description": "Draw the mouse pointer. Default: the app's setting."})";

Json Schema(const std::string& props, const char* required = "") {
    std::string s = "{\"type\": \"object\", \"properties\": {" + props + "}";
    if (*required) s += ", \"required\": [" + std::string(required) + "]";
    s += "}";
    bool ok = false;
    Json j = Json::Parse(s, &ok);
    return ok ? j : Json::Parse(R"({"type": "object"})");
}

Tool Make(const char* name, const char* title, const char* description, Json schema, std::function<void(const Json&, Reply)> run,
          bool readOnly = false, bool uiThread = true, int timeoutMs = 60000) {
    Tool t;
    t.name = name;
    t.title = title;
    t.description = description;
    t.inputSchema = std::move(schema);
    t.run = std::move(run);
    t.readOnly = readOnly;
    t.uiThread = uiThread;
    t.timeoutMs = timeoutMs;
    return t;
}

std::vector<Tool> AgentTools() {
    std::string commands;
    std::string commandList;
    for (const auto& c : g_host.commands) {
        commands += (commands.empty() ? "\"" : ", \"") + Utf8(c.key) + "\"";
        commandList += "\n- " + Utf8(c.key) + ": " + Utf8(c.title);
    }
    std::vector<Tool> tools;
    tools.push_back(Make("get_status", "Status",
                         "Ather Screenshot's state: version, captures folder, displays (index, position, size, scale), the active "
                         "window, whether something is being recorded (and for how long, with its markers), and the capture and "
                         "recording settings. Call it first to learn the screen layout.",
                         Schema(""), StatusTool, true));
    tools.push_back(Make("list_windows", "List windows",
                         "Open app windows, front to back, with ids, titles, app names and screen rectangles. Use the id or the "
                         "title as a screenshot or recording target.",
                         Schema(R"("query": {"type": "string", "description": "Only windows whose title or app name contains this."})"),
                         ListWindowsTool, true));
    tools.push_back(Make("take_screenshot", "Take a screenshot",
                         "Captures the screen, a display, a window or a region without any UI, saves it as a PNG in the captures "
                         "folder (it shows in the gallery), and returns its path and the picture. Good for playtesting: tag "
                         "shots (e.g. bug, level-3) and add a comment, then find them with list_captures.",
                         Schema(std::string(kTargetProps) + R"(,
    "delay_seconds": {"type": "number", "minimum": 0, "maximum": 60, "description": "Wait this long first."},
    "redact": {"type": "boolean", "description": "Pixelate emails, keys, card and phone numbers found by OCR. Default: the app's setting."},
    "save": {"type": "boolean", "description": "Save the PNG (default true). false only returns the picture."},
    "copy_to_clipboard": {"type": "boolean", "description": "Also copy it to the clipboard (default false)."},
    "tags": {"type": "array", "items": {"type": "string"}, "description": "Gallery tags for the saved file."},
    "comment": {"type": "string", "description": "A gallery comment for the saved file."},
    "image": {"type": "string", "enum": ["preview", "full", "none"], "description": "What picture to return: preview (default, at most 1568 px), full size, or none."})"),
                         TakeScreenshot, false, true, 120000));
    tools.push_back(Make("start_recording", "Start recording",
                         "Starts recording the screen, a display, a window (follows it as it moves, even when covered) or a "
                         "region to MP4 (H.264, with system audio and microphone if on) or GIF. Returns right away; call "
                         "stop_recording to finish and get the file. Records the game controller, clicks and keystrokes "
                         "when those overlays are on. Use add_marker to note moments while it runs.",
                         Schema(std::string(kTargetProps) + R"(,
    "format": {"type": "string", "enum": ["mp4", "gif"], "description": "Default mp4."},
    "fps": {"type": "integer", "minimum": 1, "maximum": 60, "description": "Frames per second. Default: the app's setting (30 for MP4, 15 for GIF)."},
    "follow_window": {"type": "boolean", "description": "For a window target: follow the window (default true) instead of recording its current area."},
    "countdown_seconds": {"type": "integer", "minimum": 0, "maximum": 10, "description": "On-screen countdown before recording. Default 0."},
    "max_seconds": {"type": "number", "minimum": 1, "description": "Stop by itself after this long."},
    "system_audio": {"type": "boolean"}, "microphone": {"type": "boolean"},
    "show_clicks": {"type": "boolean"}, "show_keys": {"type": "boolean", "description": "Careful: also shows typed passwords."},
    "show_gamepad": {"type": "boolean", "description": "Draw the game controller (XInput) in a corner."},
    "tags": {"type": "array", "items": {"type": "string"}}, "comment": {"type": "string"})"),
                         StartRecordingTool));
    tools.push_back(Make("stop_recording", "Stop recording",
                         "Stops the recording, waits until the file is saved and returns its path, length, size and markers. "
                         "Also reports a recording that already ended (stopped by the user, or max_seconds).",
                         Schema(R"("discard": {"type": "boolean", "description": "Throw the recording away instead of saving it."})"),
                         StopRecordingTool, false, true, 10 * 60 * 1000));
    tools.push_back(Make("pause_recording", "Pause or resume recording", "Pauses (paused: true, the default) or resumes the recording.",
                         Schema(R"("paused": {"type": "boolean"})"), PauseRecordingTool));
    tools.push_back(Make("add_marker", "Mark a moment",
                         "Marks the current moment of the running recording (e.g. \"boss fight starts\", \"clipping through the "
                         "wall\"). Markers come back from stop_recording and are saved with an MP4 as review notes "
                         "(<video>.notes.json), which the video editor shows on their frames.",
                         Schema(R"("label": {"type": "string"},
    "kind": {"type": "string", "enum": ["note", "issue", "question", "good"], "description": "Default note. issue = a bug."},
    "seconds": {"type": "number", "minimum": 0, "description": "When, in recording time. Default: now."})",
                                "\"label\""),
                         AddMarkerTool));
    tools.push_back(Make("list_captures", "Find captures",
                         "Searches the gallery (screenshots, GIFs and recordings in the captures folder), newest first. The "
                         "query works like the gallery's search: words in file names, tags, app and window names and text "
                         "inside pictures, related words (\"graph\" finds charts) and dates (\"today\", \"last week\").",
                         Schema(R"("query": {"type": "string"},
    "type": {"type": "string", "enum": ["image", "gif", "video"]},
    "tags": {"type": "array", "items": {"type": "string"}, "description": "Only captures with all of these tags."},
    "min_rating": {"type": "integer", "minimum": 0, "maximum": 5},
    "limit": {"type": "integer", "minimum": 1, "maximum": 200, "description": "Default 20."},
    "offset": {"type": "integer", "minimum": 0})"),
                         ListCapturesTool, true));
    tools.push_back(Make("get_capture", "View a capture",
                         "Returns a picture (or one frame of a video, at time_seconds) and its size, so you can look at an "
                         "earlier capture.",
                         Schema(R"("path": {"type": "string"},
    "time_seconds": {"type": "number", "minimum": 0, "description": "For videos: which moment. Default 0."},
    "max_size": {"type": "integer", "minimum": 0, "description": "Longest side in pixels (default 1568, 0 = full size)."})",
                                "\"path\""),
                         GetCaptureTool, true, false));
    tools.push_back(Make("get_video_frames", "Look through a video",
                         "Frames from a video: at the given times, or `count` frames spread evenly between start_seconds and "
                         "end_seconds. layout \"sheet\" puts them in one contact-sheet picture (cheaper to read). Use it to "
                         "review a gameplay recording, or the moments around its markers.",
                         Schema(R"("path": {"type": "string"},
    "times": {"type": "array", "items": {"type": "number"}, "maxItems": 16},
    "count": {"type": "integer", "minimum": 1, "maximum": 16, "description": "Default 6."},
    "start_seconds": {"type": "number", "minimum": 0}, "end_seconds": {"type": "number", "minimum": 0},
    "layout": {"type": "string", "enum": ["frames", "sheet"], "description": "Default frames (one picture each)."},
    "max_size": {"type": "integer", "minimum": 64, "maximum": 4096, "description": "Longest side of each frame (default 768), or of the sheet (default 1568)."})",
                                "\"path\""),
                         VideoFramesTool, true, false, 180000));
    tools.push_back(Make("read_text", "Read text (OCR)",
                         "Reads the text in a picture file, a video frame, or a fresh capture of the screen, a display, a "
                         "window or a region (nothing is saved). with_boxes adds each word's rectangle, in screen pixels for "
                         "captures, so you can find a button or a line of dialogue on screen.",
                         Schema(std::string(R"("path": {"type": "string", "description": "A picture or video file. Without it, the target is captured."},
    "time_seconds": {"type": "number", "minimum": 0, "description": "For videos."},
    "with_boxes": {"type": "boolean"},)") + kTargetProps),
                         OcrTool, true));
    tools.push_back(Make("tag_capture", "Tag a capture",
                         "Adds or removes gallery tags, sets the rating (0-5) or the comment of a capture.",
                         Schema(R"("path": {"type": "string"},
    "add_tags": {"type": "array", "items": {"type": "string"}},
    "remove_tags": {"type": "array", "items": {"type": "string"}},
    "rating": {"type": "integer", "minimum": 0, "maximum": 5},
    "comment": {"type": "string"})",
                                "\"path\""),
                         TagCaptureTool));
    tools.push_back(Make("configure", "Change settings",
                         "Changes capture and recording settings (they stay changed, as if set in Settings). Only the ones "
                         "given change; returns them all.",
                         Schema(R"("copy_to_clipboard": {"type": "boolean"}, "save_to_file": {"type": "boolean"},
    "capture_cursor": {"type": "boolean"}, "auto_redact": {"type": "boolean"}, "show_notifications": {"type": "boolean"},
    "video_fps": {"type": "integer", "minimum": 1, "maximum": 60}, "gif_fps": {"type": "integer", "minimum": 1, "maximum": 50},
    "countdown_seconds": {"type": "integer", "minimum": 0, "maximum": 10}, "record_cursor": {"type": "boolean"},
    "system_audio": {"type": "boolean"}, "microphone": {"type": "boolean"}, "show_clicks": {"type": "boolean"},
    "show_keys": {"type": "boolean"}, "show_gamepad": {"type": "boolean"},
    "gamepad_corner": {"type": "string", "enum": ["topleft", "topright", "bottomleft", "bottomright"]},
    "gamepad_opacity": {"type": "integer", "minimum": 10, "maximum": 100})"),
                         ConfigureTool));
    tools.push_back(Make("open_in_editor", "Open in an editor",
                         "Opens a picture in the annotation editor or a video in the video editor, for the person at the "
                         "computer (trim, captions, markup, frame review).",
                         Schema(R"("path": {"type": "string"})", "\"path\""), OpenInEditorTool));
    tools.push_back(Make("upload_capture", "Upload a capture",
                         "Uploads a file with the uploader set up in Settings › Upload (Imgur, a custom endpoint or S3) and "
                         "returns its link. Sends the file off this computer.",
                         Schema(R"("path": {"type": "string"})", "\"path\""), UploadTool, false, true, 5 * 60 * 1000));
    std::string runDesc = "Runs any command from the command palette, like pressing its shortcut. Commands that select a "
                          "region wait for the person at the computer. Commands:" + commandList;
    tools.push_back(Make("run_command", "Run a palette command", runDesc.c_str(),
                         Schema("\"command\": {\"type\": \"string\", \"enum\": [" + commands + "]}", "\"command\""), RunCommandTool));
    return tools;
}

const char kInstructions[] =
    "Ather Screenshot is a screenshot and screen-recording app running on this computer. Use it to see the screen and "
    "to record what happens, e.g. while playtesting a game: take_screenshot to look (and keep tagged evidence), "
    "start_recording / add_marker / stop_recording for gameplay sessions, get_video_frames to review a recording, "
    "read_text to read on-screen text, list_captures to find earlier captures. Coordinates are physical screen pixels; "
    "get_status lists the displays. Everything is saved in the user's captures folder and gallery.";

}  // namespace

std::shared_ptr<mcp::Server> MakeAgentServer(AgentHost host) {
    g_host = std::move(host);
    auto server = std::make_shared<mcp::Server>(mcp::ServerInfo{"ather-screenshot", "Ather Screenshot", ATHER_BUILD_STR, kInstructions},
                                                AgentTools());
    server->onInitialize = [](const std::string& client) {
        RunOnUi([name = client.empty() ? std::wstring(L"AI agent") : FromUtf8(client)] {
            static std::map<std::wstring, ULONGLONG> announced;  // once per client per 10 minutes
            g_client = name;
            ULONGLONG& last = announced[name];
            if (last && GetTickCount64() - last < 10 * 60 * 1000) return;
            last = GetTickCount64();
            ShowToast(name + L" connected", L"It can take screenshots and record with Ather Screenshot. Turn this off in Settings › AI agents.",
                      nullptr, nullptr, 5000);
        });
    };
    return server;
}

// ---- tests ----

ATHER_TEST(agent_tool_schemas_are_valid) {
    Settings s;
    AgentHost h;
    h.settings = &s;
    h.commands = {{L"CaptureRegion", L"Capture region"}, {L"RecordVideo", L"Record screen (MP4)"}};
    auto server = MakeAgentServer(h);
    std::map<std::string, int> seen;
    for (const auto& t : server->Tools()) {
        test::Note(t.name);
        CHECK(t.inputSchema.IsObject());
        CHECK_EQ(t.inputSchema["type"].Str(), std::string("object"));
        CHECK(t.inputSchema["properties"].IsObject());  // a schema that failed to parse falls back to no properties
        CHECK(!t.description.empty());
        CHECK_EQ(++seen[t.name], 1);
    }
    CHECK(seen.size() >= 16u);
    const Tool* run = nullptr;
    for (const auto& t : server->Tools())
        if (t.name == "run_command") run = &t;
    CHECK(run && run->inputSchema["properties"]["command"]["enum"].size() == 2u);
    for (const char* name : {"take_screenshot", "start_recording", "read_text"}) {
        const Tool* t = nullptr;
        for (const auto& x : server->Tools())
            if (x.name == name) t = &x;
        CHECK(t && t->inputSchema["properties"]["target"]["enum"].size() == 4u);
    }
}

ATHER_TEST(agent_targets) {
    Settings s;
    g_host.settings = &s;
    Target t;
    std::string err;
    CHECK(ResolveTarget(Json::Object(), &t, &err));
    CHECK_EQ(t.kind, std::string("screen"));
    const RECT virt = VirtualScreenRect();
    CHECK(EqualRect(&t.rect, &virt) != 0);
    Json bad = Json::Parse(R"({"target": "display", "display": 99})");
    CHECK(!ResolveTarget(bad, &t, &err) && err.find("no display 99") != std::string::npos);
    Json region = Json::Parse(R"({"target": "region", "x": 0, "y": 0, "width": 10, "height": 20})");
    CHECK(ResolveTarget(region, &t, &err));
    CHECK(RectW(t.rect) <= 10 && RectH(t.rect) <= 20);
    CHECK(!ResolveTarget(Json::Parse(R"({"target": "region", "x": 0})"), &t, &err));
    CHECK(!ResolveTarget(Json::Parse(R"({"target": "moon"})"), &t, &err));
    CHECK(!ResolveTarget(Json::Parse(R"({"target": "window", "window_title": "no window is called this 1f3c"})"), &t, &err));
    CHECK(!Displays().empty() && Displays()[0].primary);
}

}  // namespace ather
