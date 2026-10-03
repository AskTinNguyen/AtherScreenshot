#include "installer.h"

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#pragma comment(lib, "shlwapi")
#include <wrl/client.h>

#include "logo.h"
#include "version.h"

using Microsoft::WRL::ComPtr;

namespace ather {
namespace {

constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\AtherScreenshot";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kMainClass[] = L"AtherScreenshotMain";
constexpr ULONG_PTR kCopyDataCli = 0xA7E1;  // must match main.cpp

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_UserProgramFiles) + L"\\AtherScreenshot"; }
std::wstring ShortcutPath() { return KnownFolder(FOLDERID_Programs) + L"\\Ather Screenshot.lnk"; }

std::wstring SelfPath() {
    wchar_t p[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
    return p;
}

std::wstring InstalledVersion() {
    wchar_t v[64] = {};
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS)
        return L"";
    return v;
}

// Asks a running copy to quit (via the command-line channel) and waits for it to exit.
void StopRunningInstance() {
    HWND other = FindWindowW(kMainClass, nullptr);
    if (!other) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    std::wstring cmd = L"AtherScreenshot.exe quit";
    COPYDATASTRUCT cds{kCopyDataCli, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (PVOID)cmd.c_str()};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &result);
    if (proc) {
        if (WaitForSingleObject(proc, 8000) == WAIT_TIMEOUT) {  // hung: last resort
            if (HANDLE kill = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                TerminateProcess(kill, 0);
                CloseHandle(kill);
            }
        }
        CloseHandle(proc);
    }
}

void SetString(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void SetDword(HKEY k, const wchar_t* name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

bool CreateShortcut(const std::wstring& target, const std::wstring& lnk) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(InstallDir().c_str());
    link->SetDescription(L"Ather Screenshot — fast captures, recordings and annotation");
    link->SetIconLocation(target.c_str(), 0);
    return SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
}

// Returns an error message, or empty on success.
std::wstring Install(bool startWithWindows) {
    const std::wstring dir = InstallDir(), target = InstalledExePath();
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"Could not create " + dir;
    StopRunningInstance();
    bool copied = false;
    for (int i = 0; i < 20 && !(copied = CopyFileW(SelfPath().c_str(), target.c_str(), FALSE)); ++i) Sleep(250);
    if (!copied) return L"Could not copy the app (is it still running?).";
    CreateShortcut(target, ShortcutPath());

    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t date[16];
        swprintf_s(date, L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &fa);
        SetString(k, L"DisplayName", kProductName);
        SetString(k, L"DisplayVersion", ATHER_VERSION_WSTR);
        SetString(k, L"Publisher", L"Ather");
        SetString(k, L"DisplayIcon", target + L",0");
        SetString(k, L"InstallLocation", dir);
        SetString(k, L"InstallDate", date);
        SetString(k, L"UninstallString", L"\"" + target + L"\" --uninstall");
        SetDword(k, L"EstimatedSize", fa.nFileSizeLow / 1024 + 1);
        SetDword(k, L"NoModify", 1);
        SetDword(k, L"NoRepair", 1);
        RegCloseKey(k);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &k, nullptr) ==
        ERROR_SUCCESS) {
        if (startWithWindows) {
            SetString(k, kAppName, L"\"" + target + L"\"");
        } else {  // only remove an entry that launches the installed copy, never someone's portable copy
            wchar_t v[MAX_PATH * 2] = {};
            DWORD size = sizeof(v);
            if (RegQueryValueExW(k, kAppName, nullptr, nullptr, reinterpret_cast<BYTE*>(v), &size) == ERROR_SUCCESS &&
                StrStrIW(v, target.c_str()))
                RegDeleteValueW(k, kAppName);
        }
        RegCloseKey(k);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // refresh Start menu
    return L"";
}

void Uninstall() {
    StopRunningInstance();
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        wchar_t v[MAX_PATH * 2] = {};
        DWORD size = sizeof(v);
        // Only remove the startup entry if it launches the installed copy (not a portable one).
        if (RegQueryValueExW(k, kAppName, nullptr, nullptr, reinterpret_cast<BYTE*>(v), &size) == ERROR_SUCCESS &&
            StrStrIW(v, InstalledExePath().c_str()))
            RegDeleteValueW(k, kAppName);
        RegCloseKey(k);
    }
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
    DeleteFileW(ShortcutPath().c_str());
    // This exe may be the one being deleted: remove the folder from a helper once we've exited.
    // (`timeout` refuses to run without a console, so wait with ping; retry in case the exe is still closing.)
    const std::wstring rd = L"rmdir /s /q \"" + InstallDir() + L"\" 2>nul";
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & " + rd + L" & ping -n 3 127.0.0.1 >nul & " + rd +
                       L" & ping -n 5 127.0.0.1 >nul & " + rd;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---- branded install / uninstall window ----

enum class Mode { Install, Uninstall };
enum class Phase { Ask, Working, Done, Failed };

struct Ui {
    HWND hwnd = nullptr;
    Mode mode = Mode::Install;
    Phase phase = Phase::Ask;
    bool startup = true;
    std::wstring headline, body, error, installedVersion;
    float s = 1;
    HFONT fEyebrow{}, fHeadline{}, fBody{}, fButton{}, fSmall{};
    RECT primary{}, secondary{}, toggle{};
    int hover = 0;  // 1 primary, 2 secondary, 3 toggle
    int result = 0; // 1 = installed/uninstalled, 2 = run portable, 0 = closed
    int S(int v) const { return Px(s, v); }
} g;

void Layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int pad = g.S(36), bh = g.S(46);
    g.primary = {pad, rc.bottom - pad - bh, pad + g.S(180), rc.bottom - pad};
    g.secondary = {g.primary.right + g.S(12), g.primary.top, g.primary.right + g.S(12) + g.S(210), g.primary.bottom};
    g.toggle = {pad, g.primary.top - g.S(58), pad + g.S(46), g.primary.top - g.S(34)};
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ ob = SelectObject(dc, bb);
    FillSolid(dc, rc, theme::kBg);
    SetBkMode(dc, TRANSPARENT);
    const int pad = g.S(36);
    // faint concentric "ripple" motif, bottom-right (from the Ather identity)
    for (int i = 1; i <= 6; ++i) {
        const int r = g.S(60) * i;
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(24 + i, 25 + i, 22 + i));
        HGDIOBJ op = SelectObject(dc, pen), obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, rc.right - g.S(40) - r, rc.bottom + g.S(20) - r, rc.right - g.S(40) + r, rc.bottom + g.S(20) + r);
        SelectObject(dc, op);
        SelectObject(dc, obr);
        DeleteObject(pen);
    }
    DrawLogo(dc, {pad, pad, pad + g.S(76), pad + g.S(76)});
    const int tx = pad + g.S(100);
    HGDIOBJ of = SelectObject(dc, g.fEyebrow);
    SetTextColor(dc, theme::kAccent);
    DrawSpacedText(dc, L"ATHER SCREENSHOT  ·  V" ATHER_VERSION_WSTR, {tx, pad, rc.right - pad, pad + g.S(22)},
                   DT_LEFT | DT_TOP | DT_SINGLELINE, g.S(3));
    SelectObject(dc, g.fHeadline);
    SetTextColor(dc, theme::kText);
    RECT hr{tx, pad + g.S(20), rc.right - pad, pad + g.S(84)};
    DrawTextW(dc, g.headline.c_str(), -1, &hr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g.fBody);
    SetTextColor(dc, g.phase == Phase::Failed ? RGB(255, 120, 110) : theme::kTextDim);
    RECT br{pad, pad + g.S(104), rc.right - pad, g.toggle.top - g.S(10)};
    const std::wstring& body = g.phase == Phase::Failed ? g.error : g.body;
    DrawTextW(dc, body.c_str(), -1, &br, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    if (g.mode == Mode::Install && g.phase == Phase::Ask) {  // "Start with Windows" switch
        const RECT& t = g.toggle;
        FillRounded(dc, t, RectH(t) / 2, g.startup ? theme::kAccent : theme::kBgRaised, g.startup ? theme::kAccent : theme::kBorder);
        const int k = RectH(t) - g.S(8), kx = g.startup ? t.right - g.S(4) - k : t.left + g.S(4);
        FillRounded(dc, {kx, t.top + g.S(4), kx + k, t.top + g.S(4) + k}, k / 2, g.startup ? theme::kOnAccent : theme::kMuted);
        SelectObject(dc, g.fBody);
        SetTextColor(dc, theme::kText);
        RECT lr{t.right + g.S(12), t.top - g.S(4), rc.right - pad, t.bottom + g.S(4)};
        DrawTextW(dc, L"Start with Windows", -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (g.phase == Phase::Ask || g.phase == Phase::Failed) {
        const wchar_t* primaryText = g.mode == Mode::Uninstall ? L"UNINSTALL" : (g.installedVersion.empty() ? L"INSTALL" : L"UPDATE");
        FillRounded(dc, g.primary, g.S(8), g.hover == 1 ? theme::kAccentSoft : theme::kAccent);
        SelectObject(dc, g.fButton);
        SetTextColor(dc, theme::kOnAccent);
        RECT pr = g.primary;
        DrawTextW(dc, primaryText, -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        FillRounded(dc, g.secondary, g.S(8), g.hover == 2 ? theme::kBgRaised : theme::kBg, theme::kBorder);
        SelectObject(dc, g.fBody);
        SetTextColor(dc, theme::kText);
        RECT sr = g.secondary;
        DrawTextW(dc, g.mode == Mode::Uninstall ? L"Cancel" : L"Run without installing", -1, &sr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bb);
    DeleteDC(dc);
}

void Act(int which) {
    if (which == 3 && g.mode == Mode::Install) {
        g.startup = !g.startup;
    } else if (which == 2) {
        g.result = g.mode == Mode::Install ? 2 : 0;
        DestroyWindow(g.hwnd);
        return;
    } else if (which == 1) {
        g.phase = Phase::Working;
        g.headline = g.mode == Mode::Install ? L"INSTALLING…" : L"REMOVING…";
        InvalidateRect(g.hwnd, nullptr, FALSE);
        UpdateWindow(g.hwnd);
        if (g.mode == Mode::Install) {
            g.error = Install(g.startup);
            if (!g.error.empty()) {
                g.phase = Phase::Failed;
                g.headline = L"INSTALL FAILED";
            } else {
                g.result = 1;
                DestroyWindow(g.hwnd);
                return;
            }
        } else {
            Uninstall();
            g.phase = Phase::Done;
            g.headline = L"UNINSTALLED";
            g.body = L"Ather Screenshot has been removed. Your captures (Pictures\\AtherScreenshot) and settings were kept.";
            g.result = 1;
            SetTimer(g.hwnd, 1, 2200, nullptr);
        }
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hv = PtInRect(&g.primary, p) ? 1 : PtInRect(&g.secondary, p) ? 2 : PtInRect(&g.toggle, p) ? 3 : 0;
            if (hv != g.hover) {
                g.hover = hv;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (g.phase == Phase::Ask || g.phase == Phase::Failed) {
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (PtInRect(&g.primary, p)) Act(1);
                else if (PtInRect(&g.secondary, p)) Act(2);
                else if (PtInRect(&g.toggle, p)) Act(3);
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_RETURN && g.phase == Phase::Ask) Act(1);
            else if (w == VK_ESCAPE) DestroyWindow(h);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, g.hover ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_TIMER: DestroyWindow(h); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int RunUi(Mode mode, HICON icon) {
    g.mode = mode;
    g.installedVersion = InstalledVersion();
    if (mode == Mode::Uninstall) {
        g.headline = L"UNINSTALL?";
        g.body = L"Removes the app, its Start menu shortcut and startup entry. Your captures and settings are kept.";
    } else if (g.installedVersion.empty()) {
        g.headline = L"INSTALL FOR YOU";
        g.body = L"Installs just for your Windows account — no admin rights needed. Adds a Start menu shortcut "
                 L"and an uninstall entry in Settings › Apps.";
    } else {
        g.headline = g.installedVersion == ATHER_VERSION_WSTR ? L"REINSTALL" : L"UPDATE";
        g.body = L"Version " + g.installedVersion + L" is installed. This replaces it with v" ATHER_VERSION_WSTR
                 L" and keeps your settings and captures.";
    }
    POINT pt;
    GetCursorPos(&pt);
    g.s = DpiScaleAt(pt);
    g.fEyebrow = MakeEyebrowFont(g.S(13));
    g.fHeadline = MakeDisplayFont(g.S(52));
    g.fBody = MakeFont(g.S(14));
    g.fButton = MakeDisplayFont(g.S(22));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.lpszClassName = L"AtherScreenshotInstaller";
    RegisterClassExW(&wc);
    RECT work = MonitorRectAt(pt, true);
    RECT r{0, 0, g.S(620), g.S(360)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, (UINT)(g.s * 96 + 0.5f));
    const int w = RectW(r), h = RectH(r);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Ather Screenshot Setup", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr,
                             wc.hInstance, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kBg;
    DwmSetWindowAttribute(g.hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    Layout();
    ShowWithoutFlash(g.hwnd, true);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    for (HFONT f : {g.fEyebrow, g.fHeadline, g.fBody, g.fButton}) DeleteObject(f);
    return g.result;
}

bool HasArg(const std::wstring& cmdline, const wchar_t* flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    bool found = false;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], flag) == 0) found = true;
    LocalFree(argv);
    return found;
}

}  // namespace

std::wstring InstalledExePath() { return InstallDir() + L"\\AtherScreenshot.exe"; }

bool RunInstallFlow(const std::wstring& cmdline, HICON icon, int* exitCode) {
    *exitCode = 0;
    if (HasArg(cmdline, L"--uninstall")) {
        RunUi(Mode::Uninstall, icon);
        return true;
    }
    int argc = 0;
    LocalFree(CommandLineToArgvW(cmdline.c_str(), &argc));
    const std::wstring self = SelfPath();
    if (argc > 1 || _wcsicmp(self.c_str(), InstalledExePath().c_str()) == 0) return false;  // CLI use, or already installed
    // A portable marker next to the exe (e.g. the dev build folder, or "Run without installing") skips the installer.
    const std::wstring marker = self.substr(0, self.find_last_of(L'\\')) + L"\\AtherScreenshot.portable";
    if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) return false;

    switch (RunUi(Mode::Install, icon)) {
        case 1: {  // installed: hand over to the installed copy
            ShellExecuteW(nullptr, L"open", InstalledExePath().c_str(), nullptr, InstallDir().c_str(), SW_SHOWNORMAL);
            return true;
        }
        case 2: {  // run portable from here, and don't ask again
            HANDLE f = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            return false;
        }
        default: return true;  // closed the window: do nothing
    }
}

}  // namespace ather
