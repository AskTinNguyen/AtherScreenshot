#pragma once
#include "quickprompts.h"

namespace ather {

class Settings;

struct QuickPromptsUiHost {
    HICON icon = nullptr;
    std::function<bool()> typing;                    // is "Expand triggers as I type" on
    std::function<void(bool)> setTyping;             // the switch was flipped
    std::function<void()> changed;                   // the store changed: refresh triggers and per-prompt hotkeys
    std::function<std::wstring(const std::wstring& id)> hotkeyStatus;  // "" or why a prompt's shortcut isn't active
    std::function<std::wstring()> teamFile;
    std::function<void(const std::wstring&)> setTeamFile;
    std::function<std::wstring()> pickerHotkey;      // e.g. Ctrl+Alt+Space, for the hint under the switch
    std::function<std::wstring()> lastCapture;       // for the Try it box
};

// The QuickPrompts window: the list (team starters, team, mine), the editor, the switch and a Try it box.
// Changes save as you type. `selectId` selects a prompt; "new" starts a new one.
void ShowQuickPromptsWindow(const QuickPromptsUiHost& host, const std::wstring& selectId = L"");
bool QuickPromptsWindowOpen();

// The one-time welcome card (Q3): says what QuickPrompts does and that the app listens for triggers.
// `done(true)` when the person chose "Open QuickPrompts".
void ShowQuickPromptsWelcome(HICON icon, const std::wstring& pickerHotkey, std::function<void(bool openPage)> done);

// Tests and snapshots: the window or the card painted into a picture without showing it. `select` as above;
// `rows` gets the list's visible rows ("# header" for group headers, triggers otherwise).
BitmapPtr QuickPromptsSnapshot(const QuickPromptsUiHost& host, const std::wstring& select, std::vector<std::wstring>* rows = nullptr);
BitmapPtr QuickPromptsWelcomeSnapshot(const std::wstring& pickerHotkey);

// The Try it box's logic: if `text` ends with "<trigger> ", the text with it expanded and where the caret goes.
bool TryExpand(const std::wstring& text, const std::vector<QuickPrompt>& active, const ExpandContext& ctx, std::wstring* out,
               size_t* caret);

}  // namespace ather
