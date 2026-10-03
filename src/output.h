#pragma once
#include "common.h"

namespace ather {

bool CopyImageToClipboard(HWND owner, const Bitmap& img);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);
// CF_HDROP, so the file can be pasted into Explorer, chat apps, etc.
bool CopyFileToClipboard(HWND owner, const std::wstring& path);

// Decodes any WIC-supported image; transparency is flattened onto white.
BitmapPtr LoadImageFile(const std::wstring& path);

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
// Newest first. Images only (png/jpg), or also recordings (gif/mp4).
std::vector<std::wstring> RecentCaptures(const std::wstring& baseFolder, size_t max, bool includeRecordings = false);
bool IsImageFile(const std::wstring& path);

void OpenPath(const std::wstring& path);

}  // namespace ather
