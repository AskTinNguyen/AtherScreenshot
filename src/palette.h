#pragma once
#include "common.h"

namespace ather {

struct PaletteItem {
    int id;
    std::wstring title;
    std::wstring hint;      // right-aligned pill: hotkey, On/Off, "Recent"...
    std::wstring keywords;  // space-separated extra search terms
    wchar_t icon;           // Segoe Fluent Icons glyph
};

struct PaletteOptions {
    std::wstring placeholder;  // empty: "Search commands…"
    // When set, typed text that matches no item exactly is offered as "<createLabel> “text”" (e.g. a new tag).
    std::wstring createLabel;
    std::function<void(std::wstring text)> onCreate;
};

// Raycast-style launcher. Fuzzy search, ↑/↓, Enter, Esc; Ctrl+K toggles it closed.
// Calling while visible hides it (toggle). `onPick` runs after the palette is hidden.
void ShowPalette(std::vector<PaletteItem> items, std::function<void(int id)> onPick, const PaletteOptions& options = {});
// Same window as a single-line text prompt: Enter submits the text, Esc cancels.
void ShowTextPrompt(const std::wstring& message, const std::wstring& initial, std::function<void(std::wstring)> onSubmit);
void HidePalette(bool restoreFocus);
bool PaletteVisible();

}  // namespace ather
