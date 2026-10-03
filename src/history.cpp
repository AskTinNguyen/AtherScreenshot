#include "history.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "editor.h"
#include "ocr.h"
#include "output.h"
#include "palette.h"
#include "pin.h"
#include "toast.h"

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotHistory";
constexpr UINT WM_THUMB = WM_APP + 10, WM_INDEXED = WM_APP + 11;
constexpr int kCardW = 224, kCardH = 188, kThumbH = 136, kGap = 14, kTopH = 60, kFooterH = 30, kPad = 18;
constexpr size_t kMaxThumbs = 400;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---- persistent OCR index: path -> recognized text ----

class OcrIndex {
public:
    void Load() {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) file_ = std::wstring(p) + L"\\AtherScreenshot\\ocr-index.txt";
        CoTaskMemFree(p);
        HANDLE f = CreateFileW(file_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER size;
        GetFileSizeEx(f, &size);
        std::wstring data((size_t)size.QuadPart / sizeof(wchar_t), L'\0');
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)size.QuadPart, &read, nullptr);
        CloseHandle(f);
        std::lock_guard lock(mu_);
        for (size_t start = 0; start < data.size();) {
            size_t nl = data.find(L'\n', start);
            std::wstring line = data.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
            size_t tab = line.find(L'\t');
            if (tab != std::wstring::npos) map_[line.substr(0, tab)] = line.substr(tab + 1);
            if (nl == std::wstring::npos) break;
            start = nl + 1;
        }
    }
    void Save() {
        std::wstring data;
        {
            std::lock_guard lock(mu_);
            for (const auto& [k, v] : map_)
                if (GetFileAttributesW(k.c_str()) != INVALID_FILE_ATTRIBUTES) data += k + L'\t' + v + L'\n';
        }
        HANDLE f = CreateFileW(file_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        DWORD wr;
        WriteFile(f, data.data(), (DWORD)(data.size() * sizeof(wchar_t)), &wr, nullptr);
        CloseHandle(f);
    }
    bool Has(const std::wstring& path) {
        std::lock_guard lock(mu_);
        return map_.count(Lower(path)) != 0;
    }
    std::wstring Get(const std::wstring& path) {
        std::lock_guard lock(mu_);
        auto it = map_.find(Lower(path));
        return it == map_.end() ? L"" : it->second;
    }
    void Set(const std::wstring& path, std::wstring text) {
        for (auto& c : text)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        std::lock_guard lock(mu_);
        map_[Lower(path)] = std::move(text);
    }

private:
    std::mutex mu_;
    std::unordered_map<std::wstring, std::wstring> map_;
    std::wstring file_;
};

OcrIndex& Index() {
    static OcrIndex* idx = [] {
        auto* i = new OcrIndex();
        i->Load();
        return i;
    }();
    return *idx;
}

// ---- background workers (shared state outlives the window, threads never block closing) ----

struct Shared {
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> alive{true};
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::wstring> thumbQueue;
    int thumbSize = 256;
};

struct ThumbMsg {
    std::wstring path;
    BitmapPtr bmp;
};

BitmapPtr ShellThumbnail(const std::wstring& path, int size) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) return nullptr;
    HBITMAP hbm = nullptr;
    HRESULT hr = f->GetImage({size, size}, SIIGBF_BIGGERSIZEOK, &hbm);
    f->Release();
    if (FAILED(hr) || !hbm) return nullptr;
    BITMAP bm{};
    GetObjectW(hbm, sizeof(bm), &bm);
    auto out = Bitmap::Create(bm.bmWidth, bm.bmHeight);
    if (out) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = bm.bmWidth;
        bi.bmiHeader.biHeight = -bm.bmHeight;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, hbm, 0, bm.bmHeight, out->Bits(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        // Thumbnails may carry premultiplied alpha; flatten onto the card colour.
        uint32_t* p = out->Bits();
        const size_t n = (size_t)bm.bmWidth * bm.bmHeight;
        bool anyAlpha = false;
        for (size_t i = 0; i < n && !anyAlpha; ++i) anyAlpha = (p[i] >> 24) != 0;
        const uint32_t bg = theme::kBgRaised;  // COLORREF: 0x00BBGGRR
        const uint32_t br = bg & 255, bgc = (bg >> 8) & 255, bb = (bg >> 16) & 255;
        for (size_t i = 0; i < n; ++i) {
            if (!anyAlpha) {
                p[i] |= 0xFF000000u;
                continue;
            }
            const uint32_t a = p[i] >> 24, k = 255 - a;
            const uint32_t r = ((p[i] >> 16) & 255) + br * k / 255, g = ((p[i] >> 8) & 255) + bgc * k / 255,
                           b = (p[i] & 255) + bb * k / 255;
            p[i] = 0xFF000000u | std::min(r, 255u) << 16 | std::min(g, 255u) << 8 | std::min(b, 255u);
        }
    }
    DeleteObject(hbm);
    return out;
}

void ThumbWorker(std::shared_ptr<Shared> sh) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // shell thumbnail providers want STA
    while (sh->alive) {
        std::wstring path;
        int size;
        {
            std::unique_lock lock(sh->mu);
            sh->cv.wait(lock, [&] { return !sh->thumbQueue.empty() || !sh->alive; });
            if (!sh->alive) break;
            path = std::move(sh->thumbQueue.front());
            sh->thumbQueue.pop_front();
            size = sh->thumbSize;
        }
        auto* msg = new ThumbMsg{path, ShellThumbnail(path, size)};
        if (!sh->alive || !PostMessageW(sh->hwnd, WM_THUMB, 0, (LPARAM)msg)) delete msg;
    }
    CoUninitialize();
}

void OcrWorker(std::shared_ptr<Shared> sh, std::vector<std::wstring> images) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int sinceSave = 0;
    for (const auto& path : images) {
        if (!sh->alive) break;
        if (Index().Has(path)) continue;
        std::wstring text;
        if (auto img = LoadImageFile(path)) text = RecognizeTextBlocking(*img);
        Index().Set(path, text);
        if (++sinceSave >= 10) {
            Index().Save();
            sinceSave = 0;
        }
        if (sh->alive) PostMessageW(sh->hwnd, WM_INDEXED, 0, 0);
        Sleep(30);  // stay in the background
    }
    if (sinceSave) Index().Save();
    CoUninitialize();
}

// ---- window ----

struct Item {
    std::wstring path, name, date, kind, haystack;
    bool video = false;
};

enum HAction { HOpen, HCopy, HPin, HRename, HUpload, HCopyText, HExplorer, HDelete };

class History {
public:
    HWND hwnd = nullptr;
    HistoryHost host;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::vector<Item> items;
    std::vector<int> view;  // filtered indices into items
    std::wstring query;
    int sel = 0, scrollY = 0;
    float s = 1;
    HFONT fSearch = nullptr, fName = nullptr, fSmall = nullptr, fIcon = nullptr, fBadge = nullptr;
    std::unordered_map<std::wstring, BitmapPtr> thumbs;
    std::unordered_set<std::wstring> requested;
    bool caretOn = true;

    int S(int v) const { return Px(s, v); }

    void Scan() {
        std::wstring keep = view.empty() || sel >= (int)view.size() ? L"" : items[view[sel]].path;
        items.clear();
        for (const auto& p : RecentCaptures(host.folder, 5000, true)) {
            Item it;
            it.path = p;
            it.name = FileNameOf(p);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa);
            FILETIME local;
            SYSTEMTIME t{};
            FileTimeToLocalFileTime(&fa.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &t);
            static const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                                     L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
            wchar_t d[64];
            swprintf_s(d, L"%s %u, %u  %02u:%02u", kMonths[(t.wMonth + 11) % 12], t.wDay, t.wYear, t.wHour, t.wMinute);
            it.date = d;
            std::wstring ext = Lower(p.substr(p.find_last_of(L'.') + 1));
            it.video = ext == L"mp4" || ext == L"gif";
            it.kind = ext == L"mp4" ? L"video" : ext == L"gif" ? L"gif animation" : L"image screenshot";
            items.push_back(std::move(it));
        }
        Filter();
        for (int i = 0; i < (int)view.size(); ++i)
            if (items[view[i]].path == keep) sel = i;
    }

    void Filter() {
        view.clear();
        std::vector<std::wstring> tokens;
        std::wstring q = Lower(query);
        for (size_t start = 0; start < q.size();) {
            size_t sp = q.find(L' ', start);
            std::wstring t = q.substr(start, sp == std::wstring::npos ? std::wstring::npos : sp - start);
            if (!t.empty()) tokens.push_back(t);
            if (sp == std::wstring::npos) break;
            start = sp + 1;
        }
        for (int i = 0; i < (int)items.size(); ++i) {
            if (!tokens.empty()) {
                Item& it = items[i];
                std::wstring hay = Lower(it.name + L" " + it.date + L" " + it.kind + L" " + Index().Get(it.path));
                bool all = true;
                for (const auto& t : tokens)
                    if (hay.find(t) == std::wstring::npos) {
                        all = false;
                        break;
                    }
                if (!all) continue;
            }
            view.push_back(i);
        }
        sel = std::clamp(sel, 0, std::max(0, (int)view.size() - 1));
    }

    RECT Client() const {
        RECT rc;
        GetClientRect(hwnd, &rc);
        return rc;
    }
    int Cols() const {
        const int w = Client().right - 2 * S(kPad) + S(kGap);
        return std::max(1, w / (S(kCardW) + S(kGap)));
    }
    int GridLeft() const {
        const int cols = Cols(), used = cols * S(kCardW) + (cols - 1) * S(kGap);
        return (Client().right - used) / 2;
    }
    int ViewTop() const { return S(kTopH) + 1; }
    int ViewBottom() const { return Client().bottom - S(kFooterH); }
    int ContentHeight() const {
        const int rows = ((int)view.size() + Cols() - 1) / Cols();
        return S(kPad) * 2 + rows * (S(kCardH) + S(kGap));
    }
    RECT CardRect(int i) const {
        const int cols = Cols(), x = GridLeft() + (i % cols) * (S(kCardW) + S(kGap));
        const int y = ViewTop() + S(kPad) + (i / cols) * (S(kCardH) + S(kGap)) - scrollY;
        return {x, y, x + S(kCardW), y + S(kCardH)};
    }
    void ClampScroll() {
        scrollY = std::clamp(scrollY, 0, std::max(0, ContentHeight() - (ViewBottom() - ViewTop())));
    }
    void EnsureVisible() {
        if (view.empty()) return;
        RECT r = CardRect(sel);
        if (r.top < ViewTop()) scrollY -= ViewTop() - r.top + S(kPad);
        if (r.bottom > ViewBottom()) scrollY += r.bottom - ViewBottom() + S(kPad);
        ClampScroll();
    }
    int HitCard(POINT p) const {
        if (p.y < ViewTop() || p.y >= ViewBottom()) return -1;
        for (int i = 0; i < (int)view.size(); ++i) {
            RECT r = CardRect(i);
            if (r.top > ViewBottom()) break;
            if (PtInRect(&r, p)) return i;
        }
        return -1;
    }

    void RequestThumb(const std::wstring& path) {
        if (requested.count(path)) return;
        requested.insert(path);
        std::lock_guard lock(shared->mu);
        shared->thumbQueue.push_front(path);  // most recently painted first
        shared->cv.notify_one();
    }

    void Paint(HDC hdc);
    void Action(int a);
    void OnKey(WPARAM vk);
    void OnChar(wchar_t c);
    void ActionPalette();
    void Changed() {
        Filter();
        ClampScroll();
        caretOn = true;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    const Item* Selected() const { return view.empty() ? nullptr : &items[view[sel]]; }
};

History* g_hist = nullptr;

void History::Paint(HDC hdc) {
    RECT rc = Client();
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bb);
    FillSolid(dc, rc, theme::kBg);
    SetBkMode(dc, TRANSPARENT);

    // cards
    HRGN clip = CreateRectRgn(0, ViewTop(), rc.right, ViewBottom());
    SelectClipRgn(dc, clip);
    const int thumbW = S(kCardW) - S(16), thumbH = S(kThumbH);
    for (int i = 0; i < (int)view.size(); ++i) {
        RECT r = CardRect(i);
        if (r.bottom < ViewTop()) continue;
        if (r.top > ViewBottom()) break;
        const Item& it = items[view[i]];
        const bool selected = i == sel;
        FillRounded(dc, r, S(14), selected ? theme::kSelected : theme::kSurface, selected ? theme::kAccent : theme::kBorder);
        RECT ta{r.left + S(8), r.top + S(8), r.left + S(8) + thumbW, r.top + S(8) + thumbH};
        FillSolid(dc, ta, theme::kBgRaised);
        auto tIt = thumbs.find(it.path);
        if (tIt != thumbs.end() && tIt->second) {
            const Bitmap& t = *tIt->second;
            const double k = std::min({1.0, (double)thumbW / t.Width(), (double)thumbH / t.Height()});
            const int w = std::max(1, (int)(t.Width() * k)), h = std::max(1, (int)(t.Height() * k));
            MemDC tdc(t.Handle(), hdc);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, ta.left + (thumbW - w) / 2, ta.top + (thumbH - h) / 2, w, h, tdc, 0, 0, t.Width(), t.Height(), SRCCOPY);
        } else {
            RequestThumb(it.path);
        }
        if (it.video) {
            const wchar_t* badge = it.kind == L"video" ? L"MP4" : L"GIF";
            RECT b{ta.left + S(6), ta.top + S(6), ta.left + S(46), ta.top + S(24)};
            FillRounded(dc, b, S(4), RGB(20, 20, 26), theme::kBorder);
            HGDIOBJ of = SelectObject(dc, fBadge);
            SetTextColor(dc, theme::kText);
            DrawTextW(dc, badge, -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
        }
        HGDIOBJ of = SelectObject(dc, fName);
        SetTextColor(dc, theme::kText);
        RECT nr{r.left + S(10), ta.bottom + S(6), r.right - S(10), ta.bottom + S(24)};
        DrawTextW(dc, it.name.c_str(), -1, &nr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, fSmall);
        SetTextColor(dc, theme::kMuted);
        RECT dr{r.left + S(10), nr.bottom, r.right - S(10), nr.bottom + S(18)};
        DrawTextW(dc, it.date.c_str(), -1, &dr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, of);
    }
    if (view.empty()) {
        HGDIOBJ of = SelectObject(dc, fName);
        SetTextColor(dc, theme::kMuted);
        RECT er{0, ViewTop(), rc.right, ViewBottom()};
        DrawTextW(dc, items.empty() ? L"No captures yet" : L"No captures match your search", -1, &er,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // search bar
    const int th = S(kTopH);
    FillSolid(dc, {0, 0, rc.right, th}, theme::kSurface);
    FillSolid(dc, {0, th, rc.right, th + 1}, theme::kBorder);
    HGDIOBJ of = SelectObject(dc, fIcon);
    SetTextColor(dc, theme::kMuted);
    RECT ir{S(18), 0, S(44), th};
    DrawTextW(dc, L"\xE721", 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fSearch);
    RECT qr{S(54), 0, rc.right - S(220), th};
    int caretX = qr.left;
    if (query.empty()) {
        DrawTextW(dc, L"Search by name, date, app or text in the image…", -1, &qr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    } else {
        SetTextColor(dc, theme::kText);
        DrawTextW(dc, query.c_str(), -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SIZE sz{};
        GetTextExtentPoint32W(dc, query.c_str(), (int)query.size(), &sz);
        caretX = std::min<int>(qr.left + sz.cx, qr.right);
    }
    if (caretOn) FillSolid(dc, {caretX + 1, th / 2 - S(11), caretX + 1 + std::max(1, S(2)), th / 2 + S(11)}, theme::kAccent);
    SelectObject(dc, fSmall);
    SetTextColor(dc, theme::kMuted);
    wchar_t count[64];
    swprintf_s(count, query.empty() ? L"%d captures" : L"%d of %d", (int)view.size(), (int)items.size());
    RECT cr{rc.right - S(220), 0, rc.right - S(18), th};
    DrawTextW(dc, count, -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // footer
    const int fy = ViewBottom();
    FillSolid(dc, {0, fy, rc.right, rc.bottom}, theme::kSurface);
    FillSolid(dc, {0, fy, rc.right, fy + 1}, theme::kBorder);
    RECT fr{S(18), fy, rc.right - S(18), rc.bottom};
    DrawTextW(dc,
              L"↵ annotate/open   Ctrl+C copy   Ctrl+P pin   Ctrl+R rename   Ctrl+U upload   Ctrl+T copy text   "
              L"Del recycle   Ctrl+K more",
              -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bb);
    DeleteDC(dc);
}

void History::Action(int a) {
    const Item* it = Selected();
    if (!it) return;
    const std::wstring path = it->path;
    const bool video = it->video;
    switch (a) {
        case HOpen:
            if (video) OpenPath(path);
            else if (auto img = LoadImageFile(path)) OpenEditor(img);
            break;
        case HCopy:
            if (video) {
                if (CopyFileToClipboard(hwnd, path)) ShowToast(L"Copied as a file", FileNameOf(path), nullptr, nullptr, 1800);
            } else if (auto img = LoadImageFile(path); img && CopyImageToClipboard(hwnd, *img)) {
                ShowToast(L"Copied to clipboard", FileNameOf(path), nullptr, nullptr, 1800);
            }
            break;
        case HPin:
            if (!video)
                if (auto img = LoadImageFile(path)) PinImage(img, nullptr);
            break;
        case HRename: {
            std::wstring stem = FileNameOf(path);
            stem = stem.substr(0, stem.find_last_of(L'.'));
            HWND h = hwnd;
            ShowTextPrompt(L"Rename  ·  " + FileNameOf(path), stem, [h, path](std::wstring name) {
                std::wstring np = RenameCapture(path, name);
                if (np.empty()) return (void)ShowToast(L"Rename failed", FileNameOf(path), nullptr, nullptr, 3000);
                Index().Set(np, Index().Get(path));
                if (g_hist && g_hist->hwnd == h) {
                    g_hist->Scan();
                    for (int i = 0; i < (int)g_hist->view.size(); ++i)
                        if (g_hist->items[g_hist->view[i]].path == np) g_hist->sel = i;
                    g_hist->EnsureVisible();
                    InvalidateRect(h, nullptr, FALSE);
                }
            });
            break;
        }
        case HUpload:
            if (host.upload) host.upload(path);
            break;
        case HCopyText: {
            std::wstring text = Index().Get(path);
            if (!text.empty() && CopyTextToClipboard(hwnd, text)) {
                ShowToast(L"Text copied", text.substr(0, 200), nullptr, nullptr, 2500);
            } else if (!video) {
                if (auto img = LoadImageFile(path))
                    RecognizeTextAsync(img, [](std::wstring t, std::wstring err) {
                        if (!err.empty() || t.empty()) return (void)ShowToast(L"No text found", err, nullptr, nullptr, 2500);
                        CopyTextToClipboard(nullptr, t);
                        ShowToast(L"Text copied", t.substr(0, 200), nullptr, nullptr, 2500);
                    });
            }
            break;
        }
        case HExplorer: {
            std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case HDelete:
            if (RecycleFile(path)) {
                thumbs.erase(path);
                Scan();
                EnsureVisible();
                InvalidateRect(hwnd, nullptr, FALSE);
                ShowToast(L"Moved to Recycle Bin", FileNameOf(path), nullptr, nullptr, 2000);
            }
            break;
    }
}

void History::ActionPalette() {
    if (!Selected()) return;
    std::vector<PaletteItem> acts = {
        {HOpen, Selected()->video ? L"Open" : L"Annotate", L"Enter", L"edit editor view play", 0xE70F},
        {HCopy, L"Copy", L"Ctrl+C", L"clipboard", 0xE8C8},
        {HPin, L"Pin to screen", L"Ctrl+P", L"float", 0xE840},
        {HRename, L"Rename…", L"Ctrl+R", L"name", 0xE8AC},
        {HUpload, L"Upload and copy link", L"Ctrl+U", L"share imgur s3 url", 0xE898},
        {HCopyText, L"Copy text (OCR)", L"Ctrl+T", L"ocr recognize", 0xE8D2},
        {HExplorer, L"Show in Explorer", L"Ctrl+O", L"folder file reveal", 0xE838},
        {HDelete, L"Move to Recycle Bin", L"Del", L"delete remove trash", 0xE74D},
    };
    HWND h = hwnd;
    ShowPalette(std::move(acts), [h](int id) {
        if (g_hist && g_hist->hwnd == h) g_hist->Action(id);
    });
}

void History::OnKey(WPARAM vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    const int cols = Cols(), n = (int)view.size();
    auto move = [&](int d) {
        if (!n) return;
        sel = std::clamp(sel + d, 0, n - 1);
        EnsureVisible();
        InvalidateRect(hwnd, nullptr, FALSE);
    };
    if (ctrl) {
        switch (vk) {
            case 'C': Action(HCopy); return;
            case 'P': Action(HPin); return;
            case 'R': Action(HRename); return;
            case 'U': Action(HUpload); return;
            case 'T': Action(HCopyText); return;
            case 'O': Action(HExplorer); return;
            case 'E': Action(HOpen); return;
            case 'K': ActionPalette(); return;
            case 'W': DestroyWindow(hwnd); return;
            case VK_BACK:
                query.clear();
                Changed();
                return;
        }
        return;
    }
    switch (vk) {
        case VK_ESCAPE:
            if (!query.empty()) {
                query.clear();
                Changed();
            } else {
                DestroyWindow(hwnd);
            }
            return;
        case VK_RETURN: Action(HOpen); return;
        case VK_DELETE: Action(HDelete); return;
        case VK_LEFT: move(-1); return;
        case VK_RIGHT: move(1); return;
        case VK_UP: move(-cols); return;
        case VK_DOWN: move(cols); return;
        case VK_PRIOR: move(-cols * 3); return;
        case VK_NEXT: move(cols * 3); return;
        case VK_HOME: move(-n); return;
        case VK_END: move(n); return;
        case VK_F5:
            Scan();
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
    }
}

void History::OnChar(wchar_t c) {
    if (GetKeyState(VK_CONTROL) < 0) return;
    if (c == 8) {
        if (!query.empty()) query.pop_back();
    } else if (c >= 32 && c != 127 && query.size() < 120) {
        query += c;
    } else {
        return;
    }
    sel = 0;
    scrollY = 0;
    Changed();
}

LRESULT CALLBACK HistoryProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    History* H = g_hist && g_hist->hwnd == h ? g_hist : nullptr;
    switch (m) {
        case WM_THUMB: {
            auto* msg = reinterpret_cast<ThumbMsg*>(l);
            if (H) {
                if (H->thumbs.size() >= kMaxThumbs) {  // keep memory bounded: forget everything off screen
                    H->thumbs.clear();
                    H->requested.clear();
                }
                H->thumbs[msg->path] = msg->bmp;
                H->requested.insert(msg->path);
                InvalidateRect(h, nullptr, FALSE);
            }
            delete msg;
            return 0;
        }
        case WM_INDEXED:
            if (H && !H->query.empty()) H->Changed();  // new OCR text may match the current search
            return 0;
        case WM_TIMER:
            if (H) {
                H->caretOn = !H->caretOn;
                RECT top{0, 0, H->Client().right, H->S(kTopH)};
                InvalidateRect(h, &top, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (H) H->OnKey(w);
            return 0;
        case WM_CHAR:
            if (H) H->OnChar((wchar_t)w);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (H) {
                SetFocus(h);
                int i = H->HitCard({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
                if (i >= 0) {
                    H->sel = i;
                    InvalidateRect(h, nullptr, FALSE);
                    if (m == WM_LBUTTONDBLCLK) H->Action(HOpen);
                    if (m == WM_RBUTTONDOWN) H->ActionPalette();
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (H) {
                H->scrollY -= GET_WHEEL_DELTA_WPARAM(w) * (H->S(kCardH) + H->S(kGap)) / WHEEL_DELTA;
                H->ClampScroll();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_SIZE:
            if (H) {
                H->ClampScroll();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            if (H) {
                H->s = HIWORD(w) / 96.f;
                for (HFONT* f : {&H->fSearch, &H->fName, &H->fSmall, &H->fIcon, &H->fBadge}) DeleteObject(*f);
                H->fSearch = MakeFont(H->S(17));
                H->fName = MakeFont(H->S(13), FW_SEMIBOLD);
                H->fSmall = MakeFont(H->S(12));
                H->fIcon = MakeFont(H->S(17), FW_NORMAL, L"Segoe Fluent Icons");
                H->fBadge = MakeFont(H->S(11), FW_BOLD);
            }
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (H) H->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (H) {
                H->shared->alive = false;
                H->shared->cv.notify_all();
                for (HFONT f : {H->fSearch, H->fName, H->fSmall, H->fIcon, H->fBadge}) DeleteObject(f);
                g_hist = nullptr;
                delete H;
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void ShowHistory(const HistoryHost& host) {
    if (g_hist) {
        g_hist->host = host;
        g_hist->Scan();
        if (IsIconic(g_hist->hwnd)) ShowWindow(g_hist->hwnd, SW_RESTORE);
        ForceForeground(g_hist->hwnd);
        InvalidateRect(g_hist->hwnd, nullptr, FALSE);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = HistoryProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = host.icon;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* H = new History();
    H->host = host;
    POINT pt;
    GetCursorPos(&pt);
    H->s = DpiScaleAt(pt);
    H->fSearch = MakeFont(H->S(17));
    H->fName = MakeFont(H->S(13), FW_SEMIBOLD);
    H->fSmall = MakeFont(H->S(12));
    H->fIcon = MakeFont(H->S(17), FW_NORMAL, L"Segoe Fluent Icons");
    H->fBadge = MakeFont(H->S(11), FW_BOLD);
    H->shared->thumbSize = H->S(kCardW);
    g_hist = H;

    RECT work = MonitorRectAt(pt, true);
    const int w = std::min(H->S(1180), (int)(RectW(work) * 0.9)), h = std::min(H->S(820), (int)(RectH(work) * 0.9));
    H->hwnd = CreateWindowExW(0, kClass, L"Ather Screenshot — History", WS_OVERLAPPEDWINDOW,
                              work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!H->hwnd) {
        g_hist = nullptr;
        delete H;
        return;
    }
    H->shared->hwnd = H->hwnd;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(H->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(H->hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    H->Scan();
    SetTimer(H->hwnd, 1, 530, nullptr);
    std::thread(ThumbWorker, H->shared).detach();
    std::vector<std::wstring> images;
    for (const auto& it : H->items)
        if (!it.video && images.size() < 500) images.push_back(it.path);
    std::thread(OcrWorker, H->shared, std::move(images)).detach();
    ShowWithoutFlash(H->hwnd, true);
}

}  // namespace ather
