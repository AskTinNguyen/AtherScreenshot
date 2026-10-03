#pragma once
#include "common.h"

namespace ather {

struct HistoryHost {
    std::wstring folder;  // captures folder
    HICON icon = nullptr;
    std::function<void(const std::wstring& path)> upload;
};

// Thumbnail grid of every capture (images, GIFs, MP4s), newest first. Type to search by file name,
// date, app, or the text inside screenshots (OCR index built in the background).
// Enter = annotate/open, Ctrl+C copy, Ctrl+P pin, Ctrl+R rename, Ctrl+U upload, Ctrl+T copy text,
// Ctrl+O show in Explorer, Del = Recycle Bin, Ctrl+K = all actions.
void ShowHistory(const HistoryHost& host);

}  // namespace ather
