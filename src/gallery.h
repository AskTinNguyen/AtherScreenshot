#pragma once
#include "common.h"

namespace ather {

struct GalleryHost {
    std::wstring folder;  // captures folder
    HICON icon = nullptr;
    std::wstring regionHotkey;  // shown in the empty state
    std::function<void(const std::wstring& path)> upload;
    std::function<void(const std::wstring& path)> openImage;  // annotate a capture (the editor knows its source)
    std::function<void(const std::wstring& path)> openVideo;  // video editor
    std::function<void(const std::vector<std::wstring>& paths)> collage;
    std::function<bool()> editorOpen;
    std::function<void(const std::vector<std::wstring>& paths)> addToEditor;  // insert as image layers
};

// The capture gallery (replaces the old History window): edge-to-edge thumbnails under one floating toolbar,
// filters, tags, collections, smart folders, an inspector, preview, duplicates and "find similar".
// Port of macos/Sources/AtherScreenshot/Gallery.swift.
void ShowGallery(const GalleryHost& host);
bool GalleryVisible();

// Developer tool: renders the gallery in several states to PNGs in `outDir`, from generated captures in a temp
// folder (`--gallery-snapshots <outDir>`, with ATHER_SUPPORT_DIR pointing somewhere disposable).
int GallerySnapshots(const std::wstring& outDir);

}  // namespace ather
