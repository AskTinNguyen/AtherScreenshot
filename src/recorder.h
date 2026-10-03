#pragma once
#include "common.h"

namespace ather {

enum class RecordFormat { Mp4, Gif };

struct RecordOptions {
    RECT rect{};           // screen coordinates (region mode)
    HWND window = nullptr;  // set = follow this window instead of a fixed region
    RecordFormat format = RecordFormat::Mp4;
    int fps = 30;
    bool cursor = true;
    bool systemAudio = false;  // MP4 only
    bool microphone = false;   // MP4 only
    bool gpuCapture = true;    // Windows.Graphics.Capture when possible, GDI otherwise
    bool showClicks = false;
    bool showKeys = false;
    int countdownSeconds = 0;
    std::wstring path;
};

struct RecordResult {
    bool ok = false, discarded = false;
    RecordFormat format = RecordFormat::Mp4;
    std::wstring path, error, warning;
    BitmapPtr thumb;  // first frame
    double seconds = 0;
    uint64_t bytes = 0;
};

// Starts recording on a background thread, with a red frame + control bar (Pause / Stop / Discard)
// around the region; both are excluded from the capture. MP4 = H.264 (+ AAC audio) via Media
// Foundation, GIF = WIC with per-frame palettes and duplicate-frame merging. `done` runs on the UI thread.
bool StartRecording(const RecordOptions& opt, std::function<void(const RecordResult&)> done, std::wstring* error);
void StopRecording(bool discard);
void TogglePauseRecording();
bool IsRecording();        // counting down, capturing or paused
bool IsRecordingPaused();
bool RecorderBusy();       // recording or still finalizing the file

}  // namespace ather
