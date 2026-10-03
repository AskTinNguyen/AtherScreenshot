#pragma once
#include "common.h"

namespace ather {

struct EditorOptions {
    std::wstring capturesFolder;  // where Save / Done write new PNGs
    HICON icon = nullptr;
    bool styledExport = false;  // padding + gradient background + rounded corners + shadow
};

// Call once at startup (needs GDI+ to be running) and whenever settings change.
void SetEditorDefaults(const EditorOptions& opt);

// Opens an annotation editor window on a copy of `img`. Several can be open at once.
// Tools: select/move, arrow, line, rectangle, ellipse, pen, highlighter, text, pixelate,
// step numbers, crop. Undo/redo, Ctrl+C copy, Ctrl+S save, Enter = copy + save + close,
// Ctrl+K opens a palette with every editor command.
void OpenEditor(BitmapPtr img);

}  // namespace ather
