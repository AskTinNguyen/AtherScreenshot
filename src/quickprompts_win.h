#pragma once
#include "quickprompts.h"

namespace ather {

// The Windows side of QuickPrompts: the listener that watches typing for triggers, and the paste that inserts a
// prompt into another app. Everything here runs on the UI thread except the listener's own thread.
//
// The listener reads keys through Raw Input on a thread of its own (no keyboard hook): it never delays anyone's
// typing, can't be dropped by Windows for being slow, and only sees physical keys, so text other apps type
// (Unikey/EVKey, our own paste) never counts. It keeps the last 64 typed characters in memory and nothing else;
// they are never written anywhere.

struct QuickPromptsOptions {
    bool typing = true;            // listen for typed triggers
    bool skipFullscreen = true;    // ...except in fullscreen apps
    std::wstring excludedApps;     // ...and in these exe names (separated by ;)
    bool acceptInjected = false;   // developer tool only (--quickprompts-dev): count typed-by-software keys too
};

struct QuickPromptsHost {
    std::function<std::wstring()> lastCapture;      // path of the last capture, for {lastcapture}
    std::function<void(const std::wstring&, const std::wstring&)> notify;  // title, body
};

void SetQuickPromptsHost(QuickPromptsHost host);
// Starts or stops the listener to match, and gives it the current triggers.
void ConfigureQuickPrompts(const QuickPromptsOptions& options);
// The store changed: give the listener the new triggers.
void RefreshQuickPromptTriggers();
void StopQuickPrompts();
bool QuickPromptsListening();

// Pastes `p` into `target` (the foreground window when null) once the modifier keys are up, first deleting
// `erase` typed characters. With `keySeq` (from the listener), it gives up if anything was typed since.
void InsertQuickPrompt(const QuickPrompt& p, HWND target = nullptr, int erase = 0, uint64_t keySeq = 0);
bool QuickPromptInsertBusy();

// Exposed for tests.
bool IsExcludedExe(const std::wstring& exePath, const std::wstring& excludedApps);
bool PasteNeedsShift(const std::wstring& exePath);  // terminals paste with Ctrl+Shift+V
std::wstring ToClipboardText(const std::wstring& text);  // \n -> \r\n

}  // namespace ather
