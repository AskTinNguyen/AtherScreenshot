#pragma once
#include "common.h"

namespace ather {

struct EditorOptions {
    std::wstring capturesFolder;  // where Save / Done write new PNGs
    HICON icon = nullptr;
    bool styledExport = false;  // padding + gradient background + rounded corners + shadow
    bool saveToFile = true;     // Done also saves (Settings › Save to file)
};

// Call once at startup (needs GDI+ to be running) and whenever settings change.
void SetEditorDefaults(const EditorOptions& opt);

// Where the image came from: the file being edited (saved edits stack with it in the gallery) and the app
// and window it was captured from.
struct EditorSource {
    std::wstring path;
    std::wstring app, window;
};

// Opens an annotation editor window on a copy of `img`. Several can be open at once.
// Tools: select/move, arrow, line, rectangle, ellipse, pen, highlighter, text, step numbers, blur, pixelate,
// spotlight, magnifier, crop, canvas (add space around the screenshot) and image layers. Undo/redo,
// Ctrl+C copy, Ctrl+S save, Enter = copy + save + close, Ctrl+K opens a palette with every editor command.
void OpenEditor(BitmapPtr img, const EditorSource& source = {});
bool OpenEditorFile(const std::wstring& path);
// A collage: an empty canvas with one image layer per screenshot, laid out automatically.
bool OpenCollage(const std::vector<std::wstring>& paths);
int EditorCount();
// Inserts screenshots as image layers into the most recently used editor.
bool AddImagesToEditor(const std::vector<std::wstring>& paths);
// Developer tool: renders the editor in several states to PNGs (`--editor-snapshots <dir>`).
int EditorSnapshots(const std::wstring& outDir);

}  // namespace ather
