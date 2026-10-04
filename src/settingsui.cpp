#include "settingsui.h"

#include <dwmapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

#include "logo.h"
#include "output.h"
#include "settings.h"
#include "toast.h"

using Microsoft::WRL::ComPtr;

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotSettings";
constexpr int kWidth = 800, kTopH = 60, kFooterH = 32, kRowH = 66, kHeaderH = 46, kPad = 28, kCtrlW = 280;
constexpr int kEditId = 100;
constexpr UINT WM_EDIT_DONE = WM_APP + 20;  // wParam: 1 = commit, 0 = cancel

enum class Kind { Header, Toggle, Choice, Number, Text, Secret, Folder, Hotkey };

struct Row {
    Kind kind;
    std::wstring section, key, label, hint, def;
    std::vector<std::pair<std::wstring, std::wstring>> choices;  // value, label
    int min = 0, max = 0;
    std::function<std::wstring()> get;               // overrides the ini (e.g. run at startup)
    std::function<void(const std::wstring&)> set;
    std::wstring value;
    RECT rect{}, ctrl{}, browse{};  // layout from the last paint (empty = not visible)
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

class Window {
public:
    HWND hwnd = nullptr, edit = nullptr;
    SettingsUiHost host;
    std::vector<Row> rows;
    std::wstring query;
    int scrollY = 0, contentH = 0, hover = -1, editing = -1, recording = -1;
    UINT recMods = 0;
    float s = 1;
    HFONT fSearch{}, fLabel{}, fHint{}, fValue{}, fIcon{}, fHeader{};
    HBRUSH editBrush = CreateSolidBrush(theme::kBgRaised);
    WNDPROC editProc = nullptr;
    bool caretOn = true;

    int S(int v) const { return Px(s, v); }

    void Fonts() {
        for (HFONT* f : {&fSearch, &fLabel, &fHint, &fValue, &fIcon, &fHeader})
            if (*f) DeleteObject(*f);
        fSearch = MakeFont(S(17));
        fLabel = MakeFont(S(14), FW_SEMIBOLD);
        fHint = MakeFont(S(12));
        fValue = MakeFont(S(13));
        fIcon = MakeFont(S(12), FW_NORMAL, L"Segoe Fluent Icons");
        fHeader = MakeEyebrowFont(S(13));
        if (fHero) DeleteObject(fHero);
        fHero = MakeDisplayFont(S(44));
    }
    HFONT fHero = nullptr;
    RECT hero{};  // brand header area (only while not searching)

    void Build();
    void Load() {
        for (auto& r : rows) {
            if (r.kind == Kind::Header) continue;
            r.value = r.get ? r.get() : host.settings->Raw(r.section.c_str(), r.key.c_str(), r.def.c_str());
        }
    }
    bool Matches(const Row& r) const {
        if (query.empty()) return true;
        std::wstring hay = Lower(r.label + L" " + r.hint + L" " + r.section + L" " + r.value);
        std::wstring q = Lower(query);
        for (size_t start = 0; start < q.size();) {  // every word must match
            size_t sp = q.find(L' ', start);
            std::wstring t = q.substr(start, sp == std::wstring::npos ? std::wstring::npos : sp - start);
            if (!t.empty() && hay.find(t) == std::wstring::npos) return false;
            if (sp == std::wstring::npos) break;
            start = sp + 1;
        }
        return true;
    }
    void Write(int i, std::wstring v);
    void Paint(HDC hdc);
    void Click(POINT p, bool right);
    void BeginEdit(int i);
    void EndEdit(bool commit);
    void PickFolder(int i);
    void ShowChoices(int i);
    void ResetRow(int i);
    bool OnRecordKey(UINT vk, bool down);
    void StopRecording() {
        recording = -1;
        recMods = 0;
        if (host.suspendHotkeys) host.suspendHotkeys(false);
    }
    RECT Client() const {
        RECT rc;
        GetClientRect(hwnd, &rc);
        return rc;
    }
    void ClampScroll() {
        const int view = Client().bottom - S(kTopH) - S(kFooterH);
        scrollY = std::clamp(scrollY, 0, std::max(0, contentH - view));
    }
    int RowAt(POINT p) const {
        for (int i = 0; i < (int)rows.size(); ++i)
            if (rows[i].kind != Kind::Header && PtInRect(&rows[i].rect, p)) return i;
        return -1;
    }
};

Window* g_win = nullptr;

void Window::Build() {
    rows.clear();
    auto header = [&](const wchar_t* t) { rows.push_back({Kind::Header, L"", L"", t}); };
    auto add = [&](Kind k, const wchar_t* sec, const wchar_t* key, const wchar_t* label, const wchar_t* hint,
                   const wchar_t* def, int mn = 0, int mx = 0) {
        Row r{k, sec, key, label, hint, def};
        r.min = mn;
        r.max = mx;
        rows.push_back(std::move(r));
        return &rows.back();
    };
    header(L"General");
    {
        Row* r = add(Kind::Toggle, L"", L"", L"Run at Windows startup", L"Start Ather Screenshot when you sign in", L"0");
        r->get = [this] { return host.getStartup && host.getStartup() ? std::wstring(L"1") : std::wstring(L"0"); };
        r->set = [this](const std::wstring& v) {
            if (host.setStartup) host.setStartup(v == L"1");
        };
    }
    add(Kind::Toggle, L"Interface", L"ShowToast", L"Show notification after capture", L"Thumbnail + file name in the corner", L"1");
    add(Kind::Number, L"Interface", L"ToastDurationMs", L"Notification duration", L"Milliseconds", L"2500", 500, 15000);
    add(Kind::Toggle, L"Interface", L"Crosshair", L"Crosshair in the region selector", L"Full-screen guide lines at the cursor", L"1");
    add(Kind::Toggle, L"Interface", L"Magnifier", L"Magnifier in the region selector", L"Zoomed pixels, color and coordinates", L"1");

    header(L"Capture");
    add(Kind::Toggle, L"Capture", L"CopyToClipboard", L"Copy to clipboard", L"After every capture", L"1");
    add(Kind::Toggle, L"Capture", L"SaveToFile", L"Save to file", L"PNG in the captures folder", L"1");
    add(Kind::Folder, L"Capture", L"SaveFolder", L"Captures folder", L"Empty = Pictures\\AtherScreenshot. Environment variables work.", L"");
    add(Kind::Text, L"Capture", L"FileNameTemplate", L"File name template",
        L"{yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}", L"Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}");
    add(Kind::Toggle, L"Capture", L"AskForName", L"Ask for a name after capture", L"Shows a rename prompt once the file is saved", L"0");
    {
        Row* r = add(Kind::Choice, L"Capture", L"AfterCapture", L"After capture", L"Extra action for every capture", L"none");
        r->choices = {{L"none", L"Nothing else"}, {L"pin", L"Pin to screen"}, {L"edit", L"Open in editor"},
                      {L"open", L"Open the file"}, {L"upload", L"Upload and copy link"}};
    }
    add(Kind::Toggle, L"Capture", L"CaptureCursor", L"Include mouse cursor", L"Full-screen and window captures", L"0");
    add(Kind::Number, L"Capture", L"DelaySeconds", L"Delayed capture", L"Seconds to wait for the delayed commands", L"3", 1, 30);
    add(Kind::Toggle, L"Capture", L"AutoRedact", L"Auto-redact every capture",
        L"Pixelate emails, IPs, keys, card and phone numbers found by OCR", L"0");

    header(L"Recording");
    add(Kind::Number, L"Recording", L"VideoFps", L"Video frame rate", L"MP4 frames per second", L"30", 1, 60);
    add(Kind::Number, L"Recording", L"GifFps", L"GIF frame rate", L"GIF frames per second", L"15", 1, 50);
    add(Kind::Number, L"Recording", L"CountdownSeconds", L"Countdown", L"Seconds before recording starts (0 = none)", L"3", 0, 10);
    add(Kind::Toggle, L"Recording", L"RecordSystemAudio", L"Record system audio", L"What you hear, into the MP4", L"1");
    add(Kind::Toggle, L"Recording", L"RecordMicrophone", L"Record microphone", L"Default input device, mixed in", L"0");
    add(Kind::Toggle, L"Recording", L"RecordCursor", L"Record mouse cursor", L"", L"1");
    add(Kind::Toggle, L"Recording", L"ShowClicks", L"Show clicks", L"Ripples where you click", L"1");
    add(Kind::Toggle, L"Recording", L"ShowKeys", L"Show keystrokes", L"Careful: also shows passwords you type", L"0");
    add(Kind::Toggle, L"Recording", L"GpuCapture", L"GPU capture", L"Windows.Graphics.Capture; off = classic GDI", L"1");

    header(L"Scrolling capture & editor");
    add(Kind::Number, L"Scrolling", L"DelayMs", L"Scroll delay", L"Milliseconds to wait after each scroll", L"400", 100, 3000);
    add(Kind::Number, L"Scrolling", L"MaxFrames", L"Maximum frames", L"Stop after this many scroll steps", L"60", 2, 400);
    add(Kind::Toggle, L"Editor", L"StyledExport", L"Styled export by default",
        L"Background, padding, shadow and rounded corners (Ctrl+E in the editor)", L"0");

    header(L"Upload");
    {
        Row* r = add(Kind::Choice, L"Upload", L"Uploader", L"Uploader", L"Where uploads go", L"none");
        r->choices = {{L"none", L"Not configured"}, {L"imgur", L"Imgur"}, {L"custom", L"Custom (multipart POST)"},
                      {L"s3", L"S3-compatible (AWS, R2, MinIO)"}};
    }
    add(Kind::Secret, L"Upload", L"ImgurClientId", L"Imgur client ID", L"From api.imgur.com/oauth2/addclient", L"");
    add(Kind::Text, L"Upload", L"CustomUrl", L"Custom: URL", L"Receives a multipart POST", L"");
    add(Kind::Text, L"Upload", L"CustomFileField", L"Custom: file field", L"Form field name for the file", L"file");
    add(Kind::Secret, L"Upload", L"CustomHeaders", L"Custom: headers", L"Name: value; Name2: value2", L"");
    add(Kind::Text, L"Upload", L"CustomResponseUrl", L"Custom: link JSON path", L"e.g. data.link (empty = plain-text reply)", L"");
    add(Kind::Text, L"Upload", L"S3Endpoint", L"S3: endpoint", L"e.g. https://<account>.r2.cloudflarestorage.com", L"");
    add(Kind::Text, L"Upload", L"S3Bucket", L"S3: bucket", L"", L"");
    add(Kind::Text, L"Upload", L"S3Region", L"S3: region", L"auto for R2", L"auto");
    add(Kind::Secret, L"Upload", L"S3AccessKey", L"S3: access key", L"", L"");
    add(Kind::Secret, L"Upload", L"S3SecretKey", L"S3: secret key", L"", L"");
    add(Kind::Text, L"Upload", L"S3PublicUrl", L"S3: public URL", L"Base URL files are served from", L"");

    header(L"Shortcuts");
    for (const auto& hk : host.hotkeys)
        add(Kind::Hotkey, L"Hotkeys", hk.key.c_str(), hk.title.c_str(), L"", hk.defaultValue.c_str());
    Load();
}

void Window::Write(int i, std::wstring v) {
    Row& r = rows[i];
    if (r.kind == Kind::Number) {
        if (v.empty() || v.find_first_not_of(L"0123456789") != std::wstring::npos) {
            ShowToast(L"Not a number", r.label + L" needs a whole number between " + std::to_wstring(r.min) + L" and " +
                                           std::to_wstring(r.max) + L".", nullptr, nullptr, 3000);
            return;
        }
        v = std::to_wstring(std::clamp(_wtoi(v.c_str()), r.min, r.max));
    }
    if (r.kind == Kind::Hotkey && !v.empty()) {  // one shortcut, one command: take it away from any other
        for (int j = 0; j < (int)rows.size(); ++j)
            if (j != i && rows[j].kind == Kind::Hotkey && _wcsicmp(rows[j].value.c_str(), v.c_str()) == 0) {
                rows[j].value.clear();
                host.settings->WriteString(L"Hotkeys", rows[j].key.c_str(), L"");
                ShowToast(L"Shortcut moved", v + L" was assigned to “" + rows[j].label + L"”.", nullptr, nullptr, 3500);
            }
    }
    r.value = v;
    if (r.set) r.set(v);
    else host.settings->WriteString(r.section.c_str(), r.key.c_str(), v);
    if (host.changed) host.changed();
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Window::ResetRow(int i) {
    Row& r = rows[i];
    if (r.kind == Kind::Header || r.set) return;
    Write(i, r.def);
}

void Window::PickFolder(int i) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose the captures folder");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(hwnd)) && SUCCEEDED(dlg->GetResult(&item)) &&
        SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        Write(i, path);
        CoTaskMemFree(path);
    }
}

void Window::ShowChoices(int i) {
    Row& r = rows[i];
    HMENU menu = CreatePopupMenu();
    for (size_t k = 0; k < r.choices.size(); ++k)
        AppendMenuW(menu, MF_STRING | (_wcsicmp(r.choices[k].first.c_str(), r.value.c_str()) == 0 ? MF_CHECKED : 0),
                    k + 1, r.choices[k].second.c_str());
    POINT pt{r.ctrl.left, r.ctrl.bottom};
    ClientToScreen(hwnd, &pt);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (id > 0) Write(i, r.choices[id - 1].first);
}

LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_win) return DefWindowProcW(h, m, w, l);
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) {
        PostMessageW(g_win->hwnd, WM_EDIT_DONE, w != VK_ESCAPE, 0);
        return 0;
    }
    if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) return 0;  // no beep
    if (m == WM_KILLFOCUS) PostMessageW(g_win->hwnd, WM_EDIT_DONE, 1, 0);
    return CallWindowProcW(g_win->editProc, h, m, w, l);
}

void Window::BeginEdit(int i) {
    EndEdit(true);
    Row& r = rows[i];
    editing = i;
    const int h = S(22);
    RECT c = r.ctrl;
    if (r.kind == Kind::Folder) c.right = r.browse.left - S(6);
    edit = CreateWindowExW(0, L"EDIT", r.value.c_str(),
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (r.kind == Kind::Number ? ES_NUMBER : 0), c.left + S(10),
                           (c.top + c.bottom - h) / 2, RectW(c) - S(20), h, hwnd, (HMENU)(INT_PTR)kEditId,
                           GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, (WPARAM)fValue, TRUE);
    SendMessageW(edit, EM_SETSEL, 0, -1);
    editProc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)EditSubclass);
    SetFocus(edit);
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Window::EndEdit(bool commit) {
    if (!edit) return;
    HWND e = edit;
    const int i = editing;
    edit = nullptr;  // first: DestroyWindow sends WM_KILLFOCUS, which must not re-enter
    editing = -1;
    const int len = GetWindowTextLengthW(e);
    std::wstring text(len, L'\0');
    GetWindowTextW(e, text.data(), len + 1);
    DestroyWindow(e);
    SetFocus(hwnd);
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    while (!text.empty() && iswspace(text.front())) text.erase(0, 1);
    if (commit && i >= 0 && text != rows[i].value) Write(i, text);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Returns true if the key was consumed by the shortcut recorder.
bool Window::OnRecordKey(UINT vk, bool down) {
    if (recording < 0) return false;
    auto mods = [] {
        UINT m = 0;
        if (GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
        if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
        return m;
    };
    const bool isMod = vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN ||
                       vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT ||
                       vk == VK_RSHIFT;
    if (isMod) {
        recMods = mods();
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }
    if (!down && vk != VK_SNAPSHOT) return true;  // PrintScreen only ever arrives as key-up
    const UINT m = mods();
    const int i = recording;
    if (m == 0 && vk == VK_ESCAPE) {
        StopRecording();
    } else if (m == 0 && (vk == VK_BACK || vk == VK_DELETE)) {
        StopRecording();
        Write(i, L"");
    } else {
        std::wstring text = HotkeyToText(m, vk);
        if (text.empty()) return true;  // unsupported key: keep listening
        if (!HotkeyAllowed(m, vk)) {    // would swallow ordinary typing: keep listening
            ShowToast(L"Add Ctrl, Alt or Win", text + L" on its own would stop that key from typing. F-keys and PrintScreen work alone.",
                      nullptr, nullptr, 3500);
            return true;
        }
        StopRecording();
        Write(i, text);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    return true;
}

void Window::Click(POINT p, bool right) {
    EndEdit(true);
    const int i = RowAt(p);
    if (recording >= 0 && i != recording) StopRecording();
    if (i < 0) return;
    Row& r = rows[i];
    if (right) {
        HMENU menu = CreatePopupMenu();
        const bool canReset = !r.set;
        AppendMenuW(menu, MF_STRING | (canReset ? 0 : MF_GRAYED), 1, (L"Reset to default" + (r.def.empty() ? L"" : L"  (" + r.def + L")")).c_str());
        if (r.kind == Kind::Hotkey || r.kind == Kind::Text || r.kind == Kind::Secret || r.kind == Kind::Folder)
            AppendMenuW(menu, MF_STRING, 2, L"Clear");
        POINT sp = p;
        ClientToScreen(hwnd, &sp);
        int id = TrackPopupMenu(menu, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (id == 1) ResetRow(i);
        if (id == 2) Write(i, L"");
        return;
    }
    switch (r.kind) {
        case Kind::Toggle: Write(i, r.value == L"1" ? L"0" : L"1"); break;
        case Kind::Choice: ShowChoices(i); break;
        case Kind::Folder:
            if (PtInRect(&r.browse, p)) PickFolder(i);
            else BeginEdit(i);
            break;
        case Kind::Number:
        case Kind::Text:
        case Kind::Secret: BeginEdit(i); break;
        case Kind::Hotkey:
            if (recording == i) {
                StopRecording();
            } else {
                recording = i;
                recMods = 0;
                if (host.suspendHotkeys) host.suspendHotkeys(true);  // so pressing an existing shortcut doesn't fire it
                SetFocus(hwnd);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        default: break;
    }
}

void Window::Paint(HDC hdc) {
    RECT rc = Client();
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bb);
    FillSolid(dc, rc, theme::kBg);
    SetBkMode(dc, TRANSPARENT);
    const int top = S(kTopH) + 1, bottom = rc.bottom - S(kFooterH);
    const int left = std::max<int>(S(kPad), (rc.right - S(kWidth)) / 2), right = rc.right - left;

    // Brand header (eyebrow + condensed headline), hidden while searching.
    int y = top + S(12) - scrollY;
    hero = RECT{};
    if (query.empty()) {
        hero = {left, y + S(14), right, y + S(118)};
        y = hero.bottom;
    }
    // Lay out visible rows: headers only when something under them matches.
    for (size_t i = 0; i < rows.size(); ++i) {
        Row& r = rows[i];
        r.rect = r.ctrl = r.browse = RECT{};
        if (r.kind == Kind::Header) {
            bool any = false;
            for (size_t j = i + 1; j < rows.size() && rows[j].kind != Kind::Header; ++j) any = any || Matches(rows[j]);
            if (!any) continue;
            r.rect = {left, y, right, y + S(kHeaderH)};
            y += S(kHeaderH);
            continue;
        }
        if (!Matches(r)) continue;
        r.rect = {left, y, right, y + S(kRowH)};
        const int cy = y + S(kRowH) / 2;
        switch (r.kind) {
            case Kind::Toggle: r.ctrl = {right - S(46), cy - S(12), right, cy + S(12)}; break;
            case Kind::Folder:
                r.browse = {right - S(36), cy - S(17), right, cy + S(17)};
                r.ctrl = {right - S(kCtrlW), cy - S(17), right, cy + S(17)};
                break;
            default: r.ctrl = {right - S(kCtrlW), cy - S(17), right, cy + S(17)};
        }
        y += S(kRowH);
    }
    contentH = y + scrollY - top + S(12);

    HRGN clip = CreateRectRgn(0, top, rc.right, bottom);
    SelectClipRgn(dc, clip);
    if (!IsRectEmpty(&hero)) {
        const int ls = S(64);
        DrawLogo(dc, {hero.left, hero.top + S(12), hero.left + ls, hero.top + S(12) + ls});
        const int tx = hero.left + ls + S(22);
        HGDIOBJ of = SelectObject(dc, fHeader);
        SetTextColor(dc, theme::kAccent);
        DrawSpacedText(dc, L"ATHER SCREENSHOT", {tx, hero.top + S(8), hero.right, hero.top + S(30)},
                       DT_LEFT | DT_TOP | DT_SINGLELINE, S(3));
        SelectObject(dc, fHero);
        SetTextColor(dc, theme::kText);
        RECT hr{tx, hero.top + S(28), hero.right, hero.bottom};
        DrawTextW(dc, L"SETTINGS", -1, &hr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        if (IsRectEmpty(&r.rect) || r.rect.bottom < top || r.rect.top > bottom) continue;
        if (r.kind == Kind::Header) {
            HGDIOBJ of = SelectObject(dc, fHeader);
            SetTextColor(dc, theme::kAccent);
            RECT hr{r.rect.left, r.rect.top + S(14), r.rect.right, r.rect.bottom};
            DrawSpacedText(dc, Upper(r.label), hr, DT_LEFT | DT_TOP | DT_SINGLELINE, S(3));
            FillSolid(dc, {r.rect.left, r.rect.bottom - 1, r.rect.right, r.rect.bottom}, theme::kBorder);
            SelectObject(dc, of);
            continue;
        }
        if ((int)i == hover || (int)i == recording || (int)i == editing) {
            RECT hl = r.rect;
            InflateRect(&hl, S(10), 0);
            FillRounded(dc, hl, S(10), theme::kSurface);
        }
        // label + hint (+ shortcut status)
        const int textRight = r.ctrl.left - S(18);
        std::wstring status = r.kind == Kind::Hotkey && host.hotkeyStatus ? host.hotkeyStatus(r.key) : L"";
        const bool hasHint = !r.hint.empty() || !status.empty();
        const int cy = (r.rect.top + r.rect.bottom) / 2;
        HGDIOBJ of = SelectObject(dc, fLabel);
        SetTextColor(dc, theme::kText);
        RECT lr{r.rect.left, hasHint ? cy - S(20) : cy - S(10), textRight, hasHint ? cy + S(1) : cy + S(10)};
        DrawTextW(dc, r.label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (hasHint) {
            SelectObject(dc, fHint);
            SetTextColor(dc, status.empty() ? theme::kMuted : RGB(255, 120, 110));
            RECT hr{r.rect.left, cy + S(2), textRight, cy + S(22)};
            const std::wstring& h = status.empty() ? r.hint : status;
            DrawTextW(dc, h.c_str(), -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        // control
        const RECT c = r.ctrl;
        switch (r.kind) {
            case Kind::Toggle: {
                const bool on = r.value == L"1";
                FillRounded(dc, c, RectH(c) / 2, on ? theme::kAccent : theme::kBgRaised, on ? theme::kAccent : theme::kBorder);
                const int k = RectH(c) - S(8), kx = on ? c.right - S(4) - k : c.left + S(4);
                FillRounded(dc, {kx, c.top + S(4), kx + k, c.top + S(4) + k}, k / 2, on ? theme::kOnAccent : theme::kMuted);
                break;
            }
            case Kind::Hotkey: {
                const bool rec = (int)i == recording;
                FillRounded(dc, c, S(7), theme::kBgRaised, rec ? theme::kAccent : theme::kBorder);
                SelectObject(dc, fValue);
                std::wstring t = rec ? (recMods ? HotkeyToText(recMods, 'A').substr(0, HotkeyToText(recMods, 'A').size() - 1) + L"…"
                                                : L"Press a shortcut…  (Esc cancel, ⌫ clear)")
                                     : (r.value.empty() ? L"Not set" : r.value);
                SetTextColor(dc, rec ? theme::kAccentSoft : (r.value.empty() ? theme::kMuted : theme::kText));
                RECT tr = c;
                DrawTextW(dc, t.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                break;
            }
            default: {
                if ((int)i == editing) {
                    FillRounded(dc, r.kind == Kind::Folder ? RECT{c.left, c.top, r.browse.left - S(6), c.bottom} : c, S(7),
                                theme::kBgRaised, theme::kAccent);
                } else {
                    RECT box = r.kind == Kind::Folder ? RECT{c.left, c.top, r.browse.left - S(6), c.bottom} : c;
                    FillRounded(dc, box, S(7), theme::kBgRaised, theme::kBorder);
                    std::wstring t = r.value;
                    COLORREF col = theme::kText;
                    if (r.kind == Kind::Choice) {
                        for (const auto& [v, label] : r.choices)
                            if (_wcsicmp(v.c_str(), r.value.c_str()) == 0) t = label;
                    } else if (r.kind == Kind::Secret && !t.empty()) {
                        t = std::wstring(std::min<size_t>(t.size(), 12), L'•');
                    }
                    if (t.empty()) {
                        t = r.kind == Kind::Folder ? L"Default (Pictures\\AtherScreenshot)" : L"Not set";
                        col = theme::kMuted;
                    }
                    SelectObject(dc, fValue);
                    SetTextColor(dc, col);
                    RECT tr{box.left + S(12), box.top, box.right - (r.kind == Kind::Choice ? S(30) : S(12)), box.bottom};
                    DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
                    if (r.kind == Kind::Choice) {
                        SelectObject(dc, fIcon);
                        SetTextColor(dc, theme::kMuted);
                        RECT ar{box.right - S(30), box.top, box.right - S(8), box.bottom};
                        DrawTextW(dc, L"\xE70D", 1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    }
                }
                if (r.kind == Kind::Folder) {
                    FillRounded(dc, r.browse, S(7), theme::kBgRaised, theme::kBorder);
                    SelectObject(dc, fIcon);
                    SetTextColor(dc, theme::kText);
                    RECT br = r.browse;
                    DrawTextW(dc, L"\xE838", 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
        }
        SelectObject(dc, of);
    }
    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    // search bar
    const int th = S(kTopH);
    FillSolid(dc, {0, 0, rc.right, th}, theme::kSurface);
    FillSolid(dc, {0, th, rc.right, th + 1}, theme::kBorder);
    HFONT searchIcon = MakeFont(S(17), FW_NORMAL, L"Segoe Fluent Icons");
    HGDIOBJ of = SelectObject(dc, searchIcon);
    SetTextColor(dc, theme::kMuted);
    RECT ir{S(18), 0, S(44), th};
    DrawTextW(dc, L"\xE721", 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fSearch);
    RECT qr{S(54), 0, rc.right - S(18), th};
    int caretX = qr.left;
    if (query.empty()) {
        DrawTextW(dc, L"Search settings…", -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        SetTextColor(dc, theme::kText);
        DrawTextW(dc, query.c_str(), -1, &qr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SIZE sz{};
        GetTextExtentPoint32W(dc, query.c_str(), (int)query.size(), &sz);
        caretX = qr.left + sz.cx;
    }
    if (caretOn && !edit && recording < 0)
        FillSolid(dc, {caretX + 1, th / 2 - S(11), caretX + 1 + std::max(1, S(2)), th / 2 + S(11)}, theme::kAccent);

    // footer
    FillSolid(dc, {0, bottom, rc.right, rc.bottom}, theme::kSurface);
    FillSolid(dc, {0, bottom, rc.right, bottom + 1}, theme::kBorder);
    SelectObject(dc, fHint);
    SetTextColor(dc, theme::kMuted);
    RECT fr{S(18), bottom, rc.right - S(18), rc.bottom};
    DrawTextW(dc, L"Changes are saved automatically", -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextW(dc, L"Right-click a setting to reset it", -1, &fr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(searchIcon);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bb);
    DeleteDC(dc);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* W = g_win && g_win->hwnd == h ? g_win : nullptr;
    if (!W) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (W->OnRecordKey((UINT)w, true)) return 0;
            if (m == WM_SYSKEYDOWN) break;
            if (w == VK_ESCAPE) {
                if (!W->query.empty()) {
                    W->query.clear();
                    W->scrollY = 0;
                    InvalidateRect(h, nullptr, FALSE);
                } else {
                    DestroyWindow(h);
                }
            } else if (w == 'W' && GetKeyState(VK_CONTROL) < 0) {
                DestroyWindow(h);
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (W->OnRecordKey((UINT)w, false)) return 0;
            break;
        case WM_CHAR:
            if (W->recording >= 0 || GetKeyState(VK_CONTROL) < 0) return 0;
            if (w == 8) {
                if (!W->query.empty()) W->query.pop_back();
            } else if (w >= 32 && W->query.size() < 60) {
                W->query += (wchar_t)w;
            } else {
                return 0;
            }
            W->scrollY = 0;
            W->caretOn = true;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SYSCHAR: return W->recording >= 0 ? 0 : DefWindowProcW(h, m, w, l);  // no beep while recording Alt+…
        case WM_LBUTTONDOWN:
        case WM_RBUTTONUP:
            SetFocus(h);
            W->Click({GET_X_LPARAM(l), GET_Y_LPARAM(l)}, m == WM_RBUTTONUP);
            return 0;
        case WM_MOUSEMOVE: {
            int i = W->RowAt({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            if (i != W->hover) {
                W->hover = i;
                InvalidateRect(h, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            W->hover = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            W->EndEdit(true);
            W->scrollY -= GET_WHEEL_DELTA_WPARAM(w) * W->S(kRowH) / WHEEL_DELTA;
            W->ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_EDIT_DONE: W->EndEdit(w != 0); return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)w, theme::kText);
            SetBkColor((HDC)w, theme::kBgRaised);
            return (LRESULT)W->editBrush;
        case WM_TIMER:
            W->caretOn = !W->caretOn;
            {
                RECT top{0, 0, W->Client().right, W->S(kTopH)};
                InvalidateRect(h, &top, FALSE);
            }
            return 0;
        case WM_SIZE:
            W->EndEdit(true);
            W->ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, W->hover >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_DPICHANGED: {
            W->EndEdit(true);
            W->s = HIWORD(w) / 96.f;
            W->Fonts();
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            W->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (W->recording >= 0) W->StopRecording();
            for (HFONT f : {W->fSearch, W->fLabel, W->fHint, W->fValue, W->fIcon, W->fHeader, W->fHero}) DeleteObject(f);
            DeleteObject(W->editBrush);
            g_win = nullptr;
            delete W;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void ShowSettingsWindow(const SettingsUiHost& host) {
    if (g_win) {
        g_win->host = host;
        g_win->Load();
        if (IsIconic(g_win->hwnd)) ShowWindow(g_win->hwnd, SW_RESTORE);
        ForceForeground(g_win->hwnd);
        InvalidateRect(g_win->hwnd, nullptr, FALSE);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = host.icon;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* W = new Window();
    W->host = host;
    POINT pt;
    GetCursorPos(&pt);
    W->s = DpiScaleAt(pt);
    W->Fonts();
    W->Build();
    g_win = W;
    RECT work = MonitorRectAt(pt, true);
    const int w = std::min(W->S(kWidth + 2 * kPad), (int)(RectW(work) * 0.9)), h = std::min(W->S(860), (int)(RectH(work) * 0.9));
    W->hwnd = CreateWindowExW(0, kClass, L"Ather Screenshot — Settings", WS_OVERLAPPEDWINDOW,
                              work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!W->hwnd) {
        g_win = nullptr;
        delete W;
        return;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(W->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(W->hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    SetTimer(W->hwnd, 1, 530, nullptr);
    ShowWithoutFlash(W->hwnd, true);
}

}  // namespace ather
