#pragma once
#include "common.h"

namespace ather {

class Settings;

struct SettingsUiHotkey {
    std::wstring key, title, defaultValue;
};

struct SettingsUiHost {
    Settings* settings = nullptr;
    HICON icon = nullptr;
    std::vector<SettingsUiHotkey> hotkeys;
    std::function<void()> changed;                                  // a value was written: reload quietly
    std::function<void(bool)> suspendHotkeys;                       // while recording a new shortcut
    std::function<std::wstring(const std::wstring&)> hotkeyStatus;  // "" or why a shortcut isn't active
    std::function<bool()> getStartup;
    std::function<void(bool)> setStartup;
};

// Native settings window: search, toggles, choices, inline text/number fields, folder picker and a
// shortcut recorder. Every change is saved immediately (settings.ini stays the storage format).
void ShowSettingsWindow(const SettingsUiHost& host);

}  // namespace ather
