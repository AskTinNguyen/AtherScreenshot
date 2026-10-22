#pragma once
#include <future>
#include <optional>

#include "common.h"
#include "recorder.h"
#include "videoedit.h"

namespace ather {

// Bug reports for playtests (README "Bug reports"). "Bug start" … "bug stop", said (Settings › Bug reports, opt-in) or
// with the BugReport shortcut, make one folder, <captures>\Bugs\<yyyy-MM-dd HH-mm-ss> <first words>\, holding:
//   bug.png         the screenshot from "bug start" with what was said burnt in as a bar along the bottom
//   screenshot.png  the same screenshot as it was
//   voice.wav       what the microphone heard in between (with voice commands on)
//   logs\…          the game's logs ([Bugs] LogPaths)
//   bug.md, bug.json
//   clip.mp4        with a recording running: from 10 s before "bug start" to "bug stop", the words as captions. Cut once
//                   the recording stops (its file can't be read before); the bug is also an Issue note on the recording.
// Mac: macos/Sources/AtherScreenshot/BugReport.swift.

// ---- the bug logic: timed events in, what happened out (no microphone, screen or clock of its own) ----

enum class BugCommand { None, Start, Stop, Cancel };
// The first "bug start", "bug stop" or "bug cancel" in words[from…] (case and punctuation ignored), and the index of its "bug".
BugCommand FindBugCommand(const std::vector<CaptionWord>& words, size_t from, size_t* at);

double BugClock();  // seconds, steady: the bug logic's times

struct Bug {
    uint64_t id = 0;
    double start = 0, stop = 0;      // BugClock()
    bool autoStopped = false;        // left open for BugTracker::kMaxSeconds
    std::vector<CaptionWord> words;  // what was said between start and stop
    // The MP4 recording that ran at "bug start", in its own time (pauses left out). recording 0: none.
    uint64_t recording = 0;
    double recStart = 0, recStop = 0;
    std::vector<CaptionWord> recWords;  // `words` on the recording's time
    std::wstring Description() const;   // the words joined
};

struct BugEvent {
    enum Kind { Started, Stopped, Cancelled, Ignored } kind = Started;
    Bug bug;  // as it was then
};

class BugTracker {
public:
    static constexpr double kMaxSeconds = 120;  // a bug left open stops itself
    static constexpr double kGrace = 1.5;       // after a stop by shortcut: words still on their way are kept

    // Something recognized (a command phrase or dictation). Commands inside the words count too; the command words
    // never become part of the description.
    std::vector<BugEvent> Heard(const std::vector<CaptionWord>& words);
    // The shortcut or the palette: starts a bug, or stops the open one (kept kGrace for the words still coming).
    std::vector<BugEvent> Toggle(double t);
    // The recording's clock at `t` (recording 0: none runs), several times a second while listening or a bug is open.
    // Also finishes stops and stops a bug left open too long.
    std::vector<BugEvent> Tick(double t, uint64_t recording, double recSeconds);

    bool IsOpen() const { return open_.has_value(); }
    bool Busy() const { return open_ || closing_; }  // open, or stopped and waiting for its last words
    // Recording time at `t` for `recording`, from the samples Tick kept (flat while it was paused).
    double RecordingTime(uint64_t recording, double t) const;

private:
    BugEvent Begin(double t);
    void Close(double t);
    BugEvent Finish();
    struct Sample {
        double t;
        uint64_t recording;
        double rec;
    };
    std::vector<Sample> samples_;
    std::optional<Bug> open_, closing_;
    uint64_t nextId_ = 1;
};

// ---- the bundle ----

struct BugLog {
    std::wstring source;      // the game's file
    std::wstring file;        // the copy, in the bundle: logs\<name>
    uint64_t size = 0;        // the source's size
    uint64_t copied = 0;      // bytes in the copy
    bool truncated = false;   // only its end was kept
    std::wstring error;       // couldn't be copied
};
// The files [Bugs] LogPaths names now: each file named; from a folder or wildcard the files changed in the last 30
// minutes before `now` (seconds since 1970), or the newest if none was, at most 20. `missing`: entries matching nothing.
std::vector<std::wstring> FindBugLogs(const std::wstring& paths, double now, std::vector<std::wstring>* missing);
// Copies the logs into `dir`. Games keep their logs open, so they're read sharing read and write. A file over `maxBytes`
// keeps its last `maxBytes`, from the start of a line.
std::vector<BugLog> CopyBugLogs(const std::vector<std::wstring>& files, const std::wstring& dir, uint64_t maxBytes);

struct BugOverlay {
    RECT bar{};       // where the bar went
    int lines = 0;    // description lines drawn
    bool cut = false; // the description was longer than kMaxLines
    static constexpr int kMaxLines = 6;
};
// bug.png: the screenshot with a bar along the bottom, a header line ("BUG · 2026-10-21 14:03:22 · Game") and the
// description wrapped below it (at most kMaxLines, then "…"), white on dark translucent, sized to the picture.
// Without words just the header. Needs COM (DirectWrite).
BitmapPtr DrawBugOverlay(const Bitmap& shot, const std::wstring& header, const std::wstring& description, BugOverlay* layout = nullptr);

struct BugBundle {
    Bug bug;
    std::wstring id;                 // for tools (bug.json)
    double wallStart = 0;            // seconds since 1970
    std::wstring app, window;        // the foreground app at "bug start"
    RECT area{};                     // the screenshot's area on screen
    std::shared_future<BitmapPtr> screenshot;
    std::wstring folder;
    std::vector<BugLog> logs;
    std::vector<std::wstring> missingLogs;
    bool voice = false;              // voice.wav was written
    enum class Clip { None, Pending, Saved, Failed } clip = Clip::None;
    std::wstring recordingPath;
    double clipFrom = 0, clipTo = 0;  // recording seconds
    std::wstring clipNote;            // why there's no clip, or why it failed

    std::wstring Title() const;   // the first sentence, or "Bug at 14:03:22"
    std::wstring Header() const;  // the overlay's header line
};
// "2026-10-21 14-03-22 the door does not open" (file-name safe, the words cut to about 40 characters).
std::wstring BugFolderName(double wallStart, const std::wstring& description);
std::string BugMarkdown(const BugBundle& b);
std::string BugJson(const BugBundle& b);
// Writes everything but the clip into b.folder (made here; it must not exist): the pictures, voice.wav (`voice`: 16 kHz
// mono, may be empty), the logs, bug.md and bug.json. Blocking: a worker thread with COM.
bool WriteBugBundle(BugBundle& b, const std::vector<int16_t>& voice, const std::wstring& logPaths, uint64_t logMaxBytes,
                    std::wstring* error);
// Once the recording has stopped: the clip from `preRoll` seconds before the bug to its end, with the words as
// captions, and the bug as an Issue note on the recording; then bug.md and bug.json again. A recording that wasn't
// saved (`r`) gets neither. Blocking: a worker thread with COM.
void FinishBugClip(BugBundle& b, const RecordResult& r, double preRoll);

// ---- in the app (UI thread) ----

struct BugReportOptions {
    std::wstring folder;      // the captures folder: bugs go in <folder>\Bugs
    bool voice = false;       // [Bugs] VoiceCommands
    std::wstring logPaths;    // [Bugs] LogPaths
    int preRollSeconds = 10;  // [Bugs] PreRollSeconds
    int logMaxMB = 20;        // [Bugs] LogMaxMB
    bool cursor = false;      // the mouse cursor in the screenshot
    std::wstring hotkey;      // the BugReport shortcut, for the notifications
};
struct BugReportHost {
    HWND owner = nullptr;                                // the clipboard's
    std::function<void(const std::wstring&)> openPath;   // shows a folder
    std::function<void()> listeningChanged;              // the tray tooltip says when it listens
};
// Applies the settings: starts or stops listening to match (a recognizer or microphone that's missing is said once).
void ConfigureBugReports(const BugReportOptions& o, const BugReportHost& h);
void ToggleBugReport();     // the shortcut and the palette
bool CopyLastBugReport();   // bug.png, clip.mp4, bug.md and up to 10 logs on the clipboard as files
void OpenBugReportsFolder();
bool BugVoiceListening();
bool BugReportsBusy();      // bundles or clips still being written (quitting waits for them)
void StopBugReports();      // quitting: stops listening
// The recording `recording` (a RecordingId()) ended: clips for its bugs. The recorder calls it; tests too.
void BugRecordingFinished(uint64_t recording, const RecordResult& r);
std::wstring LastBugReport();  // the folder of the newest bug saved, or empty

}  // namespace ather
