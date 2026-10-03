#include "output.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace ather {

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

bool CopyImageToClipboard(HWND owner, const Bitmap& img) {
    const int w = img.Width(), h = img.Height();
    const size_t stride = (size_t)w * 4, bytes = stride * h;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!g) return false;
    auto* p = static_cast<BYTE*>(GlobalLock(g));
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;  // bottom-up: the most compatible layout for consumers
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = (DWORD)bytes;
    memcpy(p, &bih, sizeof(bih));
    BYTE* dst = p + sizeof(bih);
    const BYTE* src = reinterpret_cast<const BYTE*>(img.Bits());
    for (int y = 0; y < h; ++y) memcpy(dst + (size_t)(h - 1 - y) * stride, src + (size_t)y * stride, stride);
    GlobalUnlock(g);

    if (!OpenClipboardRetry(owner)) {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, g) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!g) return false;
    memcpy(GlobalLock(g), text.c_str(), bytes);
    GlobalUnlock(g);
    if (!OpenClipboardRetry(owner)) {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok;
}

bool CopyFileToClipboard(HWND owner, const std::wstring& path) {
    const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);  // double-NUL terminated list
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!g) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(g));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(g);
    if (!OpenClipboardRetry(owner)) {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, g) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok;
}

BitmapPtr LoadImageFile(const std::wstring& path) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr))
        hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                              WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = conv->GetSize(&w, &h);
    if (FAILED(hr)) return nullptr;
    auto bmp = Bitmap::Create((int)w, (int)h);
    if (!bmp || FAILED(conv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(bmp->Bits()))))
        return nullptr;
    uint32_t* p = bmp->Bits();
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {  // premultiplied over white
        uint32_t a = p[i] >> 24, k = 255 - a;
        uint32_t r = ((p[i] >> 16) & 255) + k, g = ((p[i] >> 8) & 255) + k, b = (p[i] & 255) + k;
        p[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    return bmp;
}

bool SavePng(const Bitmap& img, const std::wstring& path) {
    const UINT w = img.Width(), h = img.Height();
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> src;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;  // no alpha channel: smaller files
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr))
        hr = f->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGR, w * 4, w * h * 4,
                                       reinterpret_cast<BYTE*>(img.Bits()), &src);
    if (SUCCEEDED(hr)) hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(src.Get(), nullptr);  // converts to `fmt`
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) {
        stream.Reset();
        DeleteFileW(path.c_str());
    }
    return SUCCEEDED(hr);
}

static std::atomic<int> g_pendingSaves{0};

void SavePngAsync(BitmapPtr img, std::wstring path, std::function<void(bool)> done) {
    ++g_pendingSaves;
    std::thread([img = std::move(img), path = std::move(path), done = std::move(done)]() mutable {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = SavePng(*img, path);
        if (SUCCEEDED(co)) CoUninitialize();
        img.reset();
        if (done) RunOnUi([done = std::move(done), ok] { done(ok); });
        --g_pendingSaves;
    }).detach();
}

void WaitForPendingSaves(DWORD timeoutMs) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while (g_pendingSaves > 0 && GetTickCount64() < end) Sleep(10);
}

std::wstring DefaultCapturesFolder() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r + L"\\AtherScreenshot";
}

static std::wstring g_nameTemplate = L"Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}";

void SetFileNameTemplate(const std::wstring& tmpl) {
    g_nameTemplate = tmpl.empty() ? L"Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}" : tmpl;
}

static std::wstring SanitizeFileName(std::wstring s, size_t maxLen) {
    for (auto& c : s)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    if (s.size() > maxLen) s.resize(maxLen);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();  // Windows rejects these
    while (!s.empty() && s.front() == L' ') s.erase(0, 1);
    return s;
}

static std::wstring UniquePath(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext) {
    std::wstring path = dir + L"\\" + stem + L"." + ext;
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && i < 10000; ++i)
        path = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")." + ext;
    return path;
}

std::wstring MakeCapturePath(const std::wstring& base, const wchar_t* ext, const CaptureNameInfo& info) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t sub[16];
    swprintf_s(sub, L"%04u-%02u", t.wYear, t.wMonth);
    auto num = [](unsigned v, int width) {
        wchar_t b[16];
        swprintf_s(b, L"%0*u", width, v);
        return std::wstring(b);
    };
    const std::pair<const wchar_t*, std::wstring> tokens[] = {
        {L"{yyyy}", num(t.wYear, 4)},   {L"{MM}", num(t.wMonth, 2)},  {L"{dd}", num(t.wDay, 2)},
        {L"{HH}", num(t.wHour, 2)},     {L"{mm}", num(t.wMinute, 2)}, {L"{ss}", num(t.wSecond, 2)},
        {L"{ms}", num(t.wMilliseconds, 3)},
        {L"{app}", info.app.empty() ? L"screen" : info.app},
        {L"{window}", info.window.empty() ? L"screen" : info.window.substr(0, 60)},
        {L"{w}", std::to_wstring(info.w)}, {L"{h}", std::to_wstring(info.h)},
    };
    std::wstring name = g_nameTemplate;
    for (const auto& [tok, val] : tokens)
        for (size_t p; (p = name.find(tok)) != std::wstring::npos;) name.replace(p, wcslen(tok), val);
    name = SanitizeFileName(name, 150);
    if (name.empty()) name = L"Ather";
    std::wstring dir = base + L"\\" + sub;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return UniquePath(dir, name, ext);
}

std::wstring RenameCapture(const std::wstring& path, const std::wstring& newName) {
    size_t slash = path.find_last_of(L'\\'), dot = path.find_last_of(L'.');
    if (slash == std::wstring::npos || dot == std::wstring::npos || dot < slash) return L"";
    std::wstring stem = SanitizeFileName(newName, 150);
    if (stem.empty()) return L"";
    std::wstring dir = path.substr(0, slash), ext = path.substr(dot + 1);
    if (_wcsicmp((dir + L"\\" + stem + L"." + ext).c_str(), path.c_str()) == 0) return path;
    std::wstring target = UniquePath(dir, stem, ext);
    return MoveFileExW(path.c_str(), target.c_str(), 0) ? target : L"";
}

bool RecycleFile(const std::wstring& path) {
    std::wstring from = path + L'\0';  // double-NUL terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

struct Found {
    std::wstring name;
    FILETIME written;
};

static std::vector<Found> List(const std::wstring& pattern, bool dirs) {
    std::vector<Found> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir != dirs || fd.cFileName[0] == L'.') continue;
        out.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

static bool HasExt(const std::wstring& name, std::initializer_list<const wchar_t*> exts) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    for (const wchar_t* e : exts)
        if (_wcsicmp(name.c_str() + dot + 1, e) == 0) return true;
    return false;
}

bool IsImageFile(const std::wstring& path) { return HasExt(path, {L"png", L"jpg", L"jpeg", L"bmp"}); }

std::vector<std::wstring> RecentCaptures(const std::wstring& base, size_t max, bool includeRecordings) {
    std::vector<std::wstring> out;
    auto dirs = List(base + L"\\*", true);  // YYYY-MM folders: name order = time order
    std::sort(dirs.begin(), dirs.end(), [](const Found& a, const Found& b) { return a.name > b.name; });
    for (const auto& dir : dirs) {
        auto files = List(base + L"\\" + dir.name + L"\\*", false);
        // Names come from a template now, so order by modification time, newest first.
        std::sort(files.begin(), files.end(),
                  [](const Found& a, const Found& b) { return CompareFileTime(&a.written, &b.written) > 0; });
        for (const auto& f : files) {
            const bool ok = includeRecordings ? HasExt(f.name, {L"png", L"jpg", L"jpeg", L"bmp", L"gif", L"mp4"})
                                              : IsImageFile(f.name);
            if (!ok) continue;
            out.push_back(base + L"\\" + dir.name + L"\\" + f.name);
            if (out.size() >= max) return out;
        }
    }
    return out;
}

void OpenPath(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace ather
