#pragma once
#include "common.h"

namespace ather {

// A small video editor for screen recordings: trim, crop, speed, mute, captions (typed or transcribed on this
// PC) and markup (text, emoji, callouts, blur, zoom, title cards), saved as a new MP4 or GIF next to the
// original. Port of macos/Sources/AtherScreenshot/VideoEditor.swift.

// Where saved videos go, and the window icon. Call at startup and whenever settings change.
void SetVideoEditorOptions(const std::wstring& capturesFolder, HICON icon);
// The name on new review notes (empty: Windows' display name), and where a name typed in the editor is remembered.
void SetVideoEditorAuthor(const std::wstring& author, std::function<void(const std::wstring&)> remember);

// Opens (or brings back) the editor for an MP4.
bool OpenVideoEditor(const std::wstring& path);
bool IsVideoFile(const std::wstring& path);
int VideoEditorCount();
// True while an editor is saving a video or GIF (quitting waits for it).
bool VideoEditorsBusy();
// Developer tool: renders the editor in a few states to PNGs (`--video-snapshots <dir>`).
int VideoEditorSnapshots(const std::wstring& outDir);

}  // namespace ather
