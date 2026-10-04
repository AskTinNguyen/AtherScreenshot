#pragma once
#include "common.h"

namespace ather {

// Pixels are premultiplied BGRA. An image with transparency is copied flattened onto white (CF_DIB) and also
// as "PNG" with its alpha, which most apps prefer.
bool CopyImageToClipboard(HWND owner, const Bitmap& img);
bool HasAlpha(const Bitmap& img);
BitmapPtr Flatten(const Bitmap& img, COLORREF bg = RGB(255, 255, 255));
std::vector<BYTE> EncodePng(const Bitmap& img);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
// CF_HDROP, so the file can be pasted into Explorer, chat apps, etc.
bool CopyFileToClipboard(HWND owner, const std::wstring& path);
bool CopyFilesToClipboard(HWND owner, const std::vector<std::wstring>& paths);

// Decodes any WIC-supported image; transparency is flattened onto white.
BitmapPtr LoadImageFile(const std::wstring& path);

// 24-bit unless the image has transparency (then 32-bit with alpha).
bool SavePng(const Bitmap& img, const std::wstring& path);
// Encodes on a worker thread; `done` runs on the UI thread.
void SavePngAsync(BitmapPtr img, std::wstring path, std::function<void(bool ok)> done);
void WaitForPendingSaves(DWORD timeoutMs);

std::wstring DefaultCapturesFolder();
struct CaptureNameInfo {
    std::wstring window, app;
    int w = 0, h = 0;
};
// Tokens: {yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}
void SetFileNameTemplate(const std::wstring& tmpl);
// <base>\YYYY-MM\<template>.<ext> (creates the month folder; never overwrites an existing file).
std::wstring MakeCapturePath(const std::wstring& baseFolder, const wchar_t* ext = L"png", const CaptureNameInfo& info = {});
// Renames within the same folder, keeping the extension. Returns the new path, or empty on failure.
std::wstring RenameCapture(const std::wstring& path, const std::wstring& newName);
// Moves a file to the Recycle Bin (undoable).
bool RecycleFile(const std::wstring& path);
// Moves files to the Recycle Bin in one undoable step; returns the ones that are gone.
std::vector<std::wstring> RecycleFiles(const std::vector<std::wstring>& paths, HWND owner = nullptr);
// Opens Explorer with these files selected (they should share a folder; otherwise one window per folder).
void RevealInExplorer(const std::vector<std::wstring>& paths);
// Newest first. Images only (png/jpg), or also recordings (gif/mp4).
std::vector<std::wstring> RecentCaptures(const std::wstring& baseFolder, size_t max, bool includeRecordings = false);
bool IsImageFile(const std::wstring& path);

void OpenPath(const std::wstring& path);

}  // namespace ather
