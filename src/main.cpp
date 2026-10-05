#include "common.h"  // first: sets NOMINMAX before <windows.h>

#include <dwmapi.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <commdlg.h>
#include <objidl.h>

#include <algorithm>
#include <cmath>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "capture.h"
#include "editor.h"
#include "videoeditor.h"
#include "gallery.h"
#include "installer.h"
#include "library.h"
#include "logo.h"
#include "version.h"
#include "ocr.h"
#include "scroll.h"
#include "selftest.h"
#include "updater.h"
#include "upload.h"
#include "output.h"
#include "overlay.h"
#include "palette.h"
#include "pin.h"
#include "recorder.h"
#include "settings.h"
#include "settingsui.h"
#include "toast.h"

#include <bcrypt.h>
#include <aclapi.h>
#include <sddl.h>

#include <cwctype>
#include <map>

#pragma comment(lib, "gdiplus")
#pragma comment(lib, "bcrypt")

using namespace ather;

namespace ather {
int FeatureStats(const std::wstring& folder);
}

namespace {

enum Cmd : int {
    CmdPalette = 1,
    CmdRegion,
    CmdRegionEdit,
    CmdRecordVideo,
    CmdRecordGif,
    CmdStopRecording,
    CmdRecordWindow,
    CmdPauseRecording,
    CmdRuler,
    CmdToggleSystemAudio,
    CmdToggleMic,
    CmdToggleClicks,
    CmdToggleKeys,
    CmdToggleGamepad,
    CmdRegionRedact,
    CmdRenameLast,
    CmdToggleAutoRedact,
    CmdRegionUpload,
    CmdUploadLast,
    CmdUploadFile,
    CmdHistory,
    CmdScrolling,
    CmdEditLast,
    CmdOpenImage,
    CmdFullscreen,
    CmdMonitor,
    CmdWindow,
    CmdLastRegion,
    CmdRegionPin,
    CmdOcr,
    CmdColorPicker,
    CmdRegionDelayed,
    CmdFullscreenDelayed,
    CmdPinLast,
    CmdCopyLast,
    CmdOpenLast,
    CmdOpenFolder,
    CmdCloseAllPins,
    CmdToggleClipboard,
    CmdToggleSave,
    CmdToggleCursor,
    CmdToggleStartup,
    CmdEditSettings,
    CmdReloadSettings,
    CmdAbout,
    CmdCheckUpdates,
    CmdInstallUpdate,  // tray and notification only: never from a link or the command line
    CmdExit,
    CmdRecentBase = 1000,
};

struct CmdDef {
    int id;
    const wchar_t* key;  // settings.ini [Hotkeys] key
    const wchar_t* title;
    const wchar_t* keywords;
    wchar_t icon;
    const wchar_t* defaultHotkey;
    bool capture;  // needs other UI out of the way before it runs
};

const CmdDef kCmds[] = {
    {CmdPalette, L"CommandPalette", L"Command palette", L"", 0xE721, L"Ctrl+Alt+K", false},
    {CmdRegion, L"CaptureRegion", L"Capture region", L"screenshot snip area select crop window", 0xE7A8, L"PrintScreen", true},
    {CmdRegionEdit, L"CaptureRegionEdit", L"Capture region and annotate", L"edit editor draw arrow markup blur", 0xE70F, L"Ctrl+Alt+E", true},
    {CmdRecordVideo, L"RecordVideo", L"Record screen (MP4)", L"video screencast capture movie", 0xE714, L"Shift+PrintScreen", true},
    {CmdRecordGif, L"RecordGif", L"Record GIF", L"animation animated screencast", 0xE714, L"Ctrl+Alt+PrintScreen", true},
    {CmdStopRecording, L"StopRecording", L"Stop recording", L"finish end save video gif", 0xE71A, L"", false},
    {CmdRecordWindow, L"RecordWindow", L"Record a window (follows it)", L"video mp4 app track follow", 0xE8A7, L"", true},
    {CmdPauseRecording, L"PauseRecording", L"Pause / resume recording", L"break hold continue", 0xE769, L"", false},
    {CmdRuler, L"ScreenRuler", L"Screen ruler", L"measure pixels distance size length angle", 0xECC6, L"", true},
    {CmdToggleSystemAudio, L"ToggleSystemAudio", L"Record system audio", L"toggle sound speaker loopback", 0xE767, L"", false},
    {CmdToggleMic, L"ToggleMicrophone", L"Record microphone", L"toggle mic voice", 0xE720, L"", false},
    {CmdToggleClicks, L"ToggleShowClicks", L"Show clicks in recordings", L"toggle mouse ripple", 0xE962, L"", false},
    {CmdToggleKeys, L"ToggleShowKeys", L"Show keystrokes in recordings", L"toggle keyboard keys", 0xE765, L"", false},
    {CmdToggleGamepad, L"ToggleShowGamepad", L"Show game controller in recordings", L"toggle gamepad controller xbox joystick input overlay", 0xE7FC, L"", false},
    {CmdRegionRedact, L"CaptureRegionRedact", L"Capture region with auto-redact", L"privacy hide email key token password ocr pixelate", 0xE72E, L"", true},
    {CmdRenameLast, L"RenameLastCapture", L"Rename last capture…", L"name file title", 0xE8AC, L"", false},
    {CmdToggleAutoRedact, L"ToggleAutoRedact", L"Auto-redact every capture", L"toggle privacy ocr pixelate", 0xE72E, L"", false},
    {CmdRegionUpload, L"CaptureRegionUpload", L"Capture region and upload", L"share link imgur s3 url", 0xE898, L"Ctrl+Alt+U", true},
    {CmdUploadLast, L"UploadLastCapture", L"Upload last capture", L"share link imgur s3 url", 0xE898, L"", false},
    {CmdUploadFile, L"UploadFile", L"Upload a file…", L"share link imgur s3 url", 0xE898, L"", false},
    {CmdHistory, L"History", L"Capture gallery", L"history browse recent search thumbnails library tags collections organize", 0xE81C, L"Ctrl+Alt+H", false},
    {CmdScrolling, L"CaptureScrolling", L"Scrolling capture (long page)", L"scroll stitch full page chat long", 0xE8CB, L"Ctrl+Alt+S", true},
    {CmdEditLast, L"EditLastCapture", L"Annotate last capture", L"edit editor draw markup", 0xE70F, L"", false},
    {CmdOpenImage, L"OpenImageInEditor", L"Open a picture or video to edit…", L"edit file annotate load image video mp4 import", 0xE8E5, L"", false},
    {CmdFullscreen, L"CaptureFullscreen", L"Capture full screen", L"all monitors desktop entire everything", 0xE7F4, L"Ctrl+PrintScreen", true},
    {CmdMonitor, L"CaptureMonitor", L"Capture current monitor", L"display screen", 0xE7F4, L"Ctrl+Shift+PrintScreen", true},
    {CmdWindow, L"CaptureWindow", L"Capture active window", L"app foreground focused", 0xE8A7, L"Alt+PrintScreen", true},
    {CmdLastRegion, L"CaptureLastRegion", L"Repeat last region", L"again previous same", 0xE72C, L"", true},
    {CmdRegionPin, L"CaptureRegionPin", L"Capture region and pin to screen", L"float sticky reference overlay", 0xE840, L"", true},
    {CmdOcr, L"CaptureText", L"Capture text (OCR)", L"ocr recognize extract read copy text", 0xE8D2, L"", true},
    {CmdColorPicker, L"PickColor", L"Pick color from screen", L"eyedropper hex rgb colour", 0xE790, L"", true},
    {CmdRegionDelayed, L"CaptureRegionDelayed", L"Capture region after delay", L"timer wait delay", 0xE916, L"", true},
    {CmdFullscreenDelayed, L"CaptureFullscreenDelayed", L"Capture full screen after delay", L"timer wait delay menu", 0xE916, L"", true},
    {CmdPinLast, L"PinLastCapture", L"Pin last capture", L"float sticky", 0xE840, L"", false},
    {CmdCopyLast, L"CopyLastCapture", L"Copy last capture", L"clipboard again", 0xE8C8, L"", false},
    {CmdOpenLast, L"OpenLastCapture", L"Open last capture", L"view image file", 0xEB9F, L"", false},
    {CmdOpenFolder, L"OpenCapturesFolder", L"Open captures folder", L"explorer directory history files", 0xE838, L"", false},
    {CmdCloseAllPins, L"CloseAllPins", L"Close all pinned images", L"unpin remove", 0xE711, L"", false},
    {CmdToggleClipboard, L"ToggleCopyToClipboard", L"Copy to clipboard after capture", L"toggle setting", 0xE8C8, L"", false},
    {CmdToggleSave, L"ToggleSaveToFile", L"Save to file after capture", L"toggle setting disk png", 0xE74E, L"", false},
    {CmdToggleCursor, L"ToggleCaptureCursor", L"Include mouse cursor", L"toggle setting pointer", 0xE962, L"", false},
    {CmdToggleStartup, L"ToggleRunAtStartup", L"Run at Windows startup", L"toggle login boot autostart", 0xE768, L"", false},
    {CmdEditSettings, L"EditSettings", L"Settings", L"preferences options config hotkeys shortcuts keyboard", 0xE713, L"", false},
    {CmdReloadSettings, L"ReloadSettings", L"Reload settings", L"config refresh", 0xE72C, L"", false},
    {CmdAbout, L"About", L"About Ather Screenshot", L"version info help", 0xE946, L"", false},
    {CmdCheckUpdates, L"CheckForUpdates", L"Check for updates…", L"update upgrade new version download latest", 0xE895, L"", false},
    {CmdExit, L"Quit", L"Quit Ather Screenshot", L"exit close", 0xE7E8, L"", false},
};

enum class After { Default, Pin, Ocr, Edit, Redact, Upload };

constexpr wchar_t kMainClass[] = L"AtherScreenshotMain";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_PALETTE = WM_APP + 2;
constexpr ULONG_PTR kCopyDataCli = 0xA7E1;  // WM_COPYDATA tag for forwarded command lines
constexpr UINT_PTR kDeferTimer = 1, kDelayTimer = 2, kLibraryTimer = 3, kUpdateTimer = 4;

HWND g_hwnd = nullptr;
HICON g_icon = nullptr;     // small: tray
HICON g_iconBig = nullptr;  // windows, alt-tab, installer
UINT g_msgTaskbarCreated = 0;
Settings g_settings;
std::wstring g_hotkeyErrors;

BitmapPtr g_last;
std::wstring g_lastPath;
CaptureNameInfo g_nameInfo;  // window/app of the current capture, for file-name templates
RECT g_lastRegion{};
bool g_haveLastRegion = false;
int g_deferredCmd = 0, g_delayedCmd = 0;
bool g_deferredUntrusted = false, g_delayedUntrusted = false;
// False while running a command a link or another app asked for (see RunCli). Captures read it when they
// start, so a later command can't change it for a capture that's already under way.
bool g_uploadAllowed = true;
std::vector<int> g_mru;
std::vector<std::wstring> g_recents;

void Execute(int id, bool deferCapture, bool untrusted = false);
void CheckForUpdates(bool manual);
void InstallUpdate();

const CmdDef* FindCmd(int id) {
    for (const auto& c : kCmds)
        if (c.id == id) return &c;
    return nullptr;
}

std::vector<HotkeyDef> HotkeyDefs() {
    std::vector<HotkeyDef> defs;
    for (const auto& c : kCmds) defs.push_back({c.key, c.defaultHotkey});
    return defs;
}

void Notify(const std::wstring& title, const std::wstring& body = L"") {
    ShowToast(title, body, nullptr, nullptr, g_settings.toastMs);
}

// ---- startup registration ----

bool IsRunAtStartup() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool on = RegQueryValueExW(k, kAppName, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

void SetRunAtStartup(bool on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// ---- hotkeys & settings ----

std::map<std::wstring, std::wstring> g_hotkeyStatus;  // ini key -> why it isn't active
bool g_hotkeysSuspended = false;                       // while the settings window records a shortcut

void RegisterHotkeys() {
    g_hotkeyErrors.clear();
    g_hotkeyStatus.clear();
    for (const auto& c : kCmds) {
        UnregisterHotKey(g_hwnd, c.id);
        std::wstring hk = g_settings.Hotkey(c.key);
        if (hk.empty()) continue;
        UINT mods, vk;
        if (!ParseHotkey(hk, mods, vk)) {
            g_hotkeyErrors += L"Invalid: " + hk + L" (" + c.title + L")\n";
            g_hotkeyStatus[c.key] = L"“" + hk + L"” isn’t a valid shortcut";
        } else if (!RegisterHotKey(g_hwnd, c.id, mods | MOD_NOREPEAT, vk)) {
            g_hotkeyErrors += L"In use: " + hk + L" (" + c.title + L")\n";
            g_hotkeyStatus[c.key] = hk + L" is taken by another app or Windows";
        }
    }
}

void UnregisterAllHotkeys() {
    for (const auto& c : kCmds) UnregisterHotKey(g_hwnd, c.id);
}

void ShowHotkeyErrors() {
    if (g_hotkeyErrors.empty()) return;
    std::wstring body = g_hotkeyErrors;
    if (body.find(L"PrintScreen") != std::wstring::npos)
        body += L"Tip: turn off Settings › Accessibility › Keyboard › \"Use the Print screen key to open screen capture\".";
    ShowToast(L"Some hotkeys are unavailable", body, nullptr, nullptr, 9000);
}

void ApplyEditorDefaults() {
    SetEditorDefaults({g_settings.CapturesFolder(), g_iconBig, g_settings.styledExport, g_settings.saveToFile});
    SetVideoEditorOptions(g_settings.CapturesFolder(), g_iconBig);
    SetFileNameTemplate(g_settings.fileNameTemplate);
    Library::Shared().SetFolder(g_settings.CapturesFolder());
    Library::Shared().SetAutoTag(g_settings.autoTag);
}

void ReloadSettings(bool notify) {
    g_settings.Load(HotkeyDefs());
    ApplyEditorDefaults();
    RegisterHotkeys();
    if (!g_hotkeyErrors.empty()) ShowHotkeyErrors();
    else if (notify) Notify(L"Settings reloaded");
}

bool ToggleState(int id, bool* on) {
    switch (id) {
        case CmdToggleClipboard: *on = g_settings.copyToClipboard; return true;
        case CmdToggleSave: *on = g_settings.saveToFile; return true;
        case CmdToggleCursor: *on = g_settings.captureCursor; return true;
        case CmdToggleStartup: *on = IsRunAtStartup(); return true;
        case CmdToggleSystemAudio: *on = g_settings.systemAudio; return true;
        case CmdToggleMic: *on = g_settings.microphone; return true;
        case CmdToggleClicks: *on = g_settings.showClicks; return true;
        case CmdToggleKeys: *on = g_settings.showKeys; return true;
        case CmdToggleGamepad: *on = g_settings.showGamepad; return true;
        case CmdToggleAutoRedact: *on = g_settings.autoRedact; return true;
    }
    return false;
}

// ---- capture pipeline ----

void OnToastClick() {
    if (g_last) OpenEditor(g_last);
}

UploadConfig MakeUploadConfig() {
    const Settings& s = g_settings;
    return {s.uploader,       s.imgurClientId, s.customUrl, s.customFileField, s.customHeaders, s.customResponseUrl,
            s.s3Endpoint,     s.s3Bucket,      s.s3Region,  s.s3AccessKey,     s.s3SecretKey,   s.s3PublicUrl};
}

void UploadAndCopyLink(const std::wstring& path, bool deleteAfter = false) {
    const UploadConfig cfg = MakeUploadConfig();
    if (std::wstring problem = UploadConfigProblem(cfg); !problem.empty()) return Notify(L"Upload not configured", problem);
    ShowToast(L"Uploading…", FileNameOf(path), nullptr, nullptr, 60000);
    UploadFileAsync(path, cfg, [path, deleteAfter](std::wstring url, std::wstring err) {
        if (deleteAfter) DeleteFileW(path.c_str());
        if (!err.empty()) return Notify(L"Upload failed", err);
        CopyTextToClipboard(g_hwnd, url);
        ShowToast(L"Link copied", url, nullptr, [url] { OpenPath(url); }, g_settings.toastMs + 2500);
    });
}

void PromptRename(const std::wstring& path) {
    std::wstring stem = FileNameOf(path);
    stem = stem.substr(0, stem.find_last_of(L'.'));
    ShowTextPrompt(L"Name this capture  ·  Enter to rename, Esc to keep the current name", stem,
                   [path](std::wstring name) {
                       std::wstring np = RenameCapture(path, name);
                       if (np.empty()) return Notify(L"Rename failed", FileNameOf(path));
                       Library::Shared().Moved(path, np);  // keeps tags, rating... (also updates g_lastPath)
                       Notify(L"Renamed", FileNameOf(np));
                   });
}

// redactedCount < 0: auto-redact hasn't run yet for this image.
// allowUpload: false for commands a link or another app asked for; then nothing leaves the machine, even
// when "After capture" is set to upload.
void Deliver(BitmapPtr img, const RECT& where, After after, int redactedCount = -1, bool allowUpload = true) {
    if (!img) {
        Notify(L"Capture failed");
        return;
    }
    if (redactedCount < 0 && (after == After::Redact || (g_settings.autoRedact && after != After::Ocr))) {
        ShowToast(L"Redacting sensitive text…", L"", nullptr, nullptr, 10000);
        RecognizeWordsAsync(img, [img, where, after, allowUpload](std::vector<OcrWord> words, std::wstring err) {
            HideToast();
            if (!err.empty()) {
                // Never hand out an unredacted image when redaction was asked for: let the user do it by hand.
                ShowToast(L"Auto-redact failed — nothing was copied, saved or uploaded",
                          err + L"\nOpened in the editor so you can redact it yourself.", nullptr, nullptr, 9000);
                OpenEditor(img, {L"", g_nameInfo.app, g_nameInfo.window});
                return;
            }
            auto clean = img->Crop({0, 0, img->Width(), img->Height()});
            int n = 0;
            for (const RECT& r : FindSensitive(words)) {
                clean->Pixelate(r, std::max(6, RectH(r) / 3));
                ++n;
            }
            Deliver(clean, where, after == After::Redact ? After::Default : after, n, allowUpload);
        });
        return;
    }
    g_last = img;
    g_lastPath.clear();
    std::wstring dims = std::to_wstring(img->Width()) + L" × " + std::to_wstring(img->Height());
    if (redactedCount > 0) dims += L"  ·  " + std::to_wstring(redactedCount) + L" redacted";

    if (after == After::Ocr) {
        ShowToast(L"Reading text…", dims, nullptr, nullptr, 15000);
        RecognizeTextAsync(img, [](std::wstring text, std::wstring err) {
            if (!err.empty()) return Notify(L"OCR failed", err);
            if (text.empty()) return Notify(L"No text found", L"Try a larger or sharper region.");
            CopyTextToClipboard(g_hwnd, text);
            std::wstring preview = text.size() > 280 ? text.substr(0, 280) + L"…" : text;
            ShowToast(L"Text copied", preview, nullptr, nullptr, g_settings.toastMs + 2000);
        });
        return;
    }

    if (after == After::Edit || (after == After::Default && g_settings.afterCapture == L"edit")) {
        OpenEditor(img, {L"", g_nameInfo.app, g_nameInfo.window});  // the editor copies/saves the annotated result on Done
        return;
    }

    const bool copied = g_settings.copyToClipboard && CopyImageToClipboard(g_hwnd, *img);
    const bool pin = after == After::Pin || g_settings.afterCapture == L"pin";
    if (pin) PinImage(img, &where);

    const bool upload = allowUpload && (after == After::Upload || g_settings.afterCapture == L"upload");
    uint64_t toast = 0;
    if (g_settings.showToast && !pin && !upload) {
        std::wstring body = dims + (g_settings.saveToFile ? L"  ·  saving…" : L"");
        toast = ShowToast(copied ? L"Copied to clipboard" : L"Captured", body, img, OnToastClick, g_settings.toastMs);
    }
    if (g_settings.saveToFile || upload) {
        CaptureNameInfo info = g_nameInfo;
        info.w = img->Width();
        info.h = img->Height();
        // Uploading without saving still needs a file: use a temp one and delete it afterwards.
        std::wstring path;
        if (g_settings.saveToFile) {
            path = MakeCapturePath(g_settings.CapturesFolder(), L"png", info);
            Library::Shared().NoteCapture(path, info.app, info.window);  // the gallery shows where it came from
        } else {
            wchar_t tmp[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            path = MakeCapturePath(std::wstring(tmp) + L"AtherScreenshot", L"png", info);
        }
        const bool temp = !g_settings.saveToFile;
        SavePngAsync(img, path, [img, path, toast, dims, upload, temp](bool ok) {
            if (!ok) return Notify(L"Save failed", path);
            if (!temp && g_last == img) g_lastPath = path;
            UpdateToastBody(toast, dims + L"  ·  " + FileNameOf(path));
            if (upload) UploadAndCopyLink(path, temp);
            if (temp) return;
            if (g_settings.afterCapture == L"open") OpenPath(path);
            if (g_settings.askForName) PromptRename(path);
        });
    }
}

CaptureNameInfo NameInfoFor(HWND w) {
    if (!w || IsOwnWindow(w)) return {};
    return {WindowTitle(w), WindowAppName(w)};
}

// Make sure none of our own transient UI ends up in the shot.
void PrepareCapture() {
    g_nameInfo = NameInfoFor(GetForegroundWindow());  // before our UI takes focus
    HidePalette(false);
    if (HideToast()) DwmFlush();
}

void CaptureRect(RECT r, After after = After::Default) {
    RECT virt = VirtualScreenRect();
    if (!IntersectRect(&r, &r, &virt)) return Notify(L"Nothing to capture");
    PrepareCapture();
    Deliver(CaptureScreen(r, g_settings.captureCursor), r, after, -1, g_uploadAllowed);
}

OverlayOptions MakeOverlayOptions() {
    OverlayOptions o;
    o.crosshair = g_settings.crosshair;
    o.magnifier = g_settings.magnifier;
    return o;
}

void StartRegion(After after) {
    if (OverlayActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    BitmapPtr shot = CaptureScreen(virt, false);
    const bool allow = g_uploadAllowed;
    ShowOverlay(OverlayMode::Region, shot, virt, MakeOverlayOptions(), [shot, virt, after, allow](const OverlayResult& r) {
        if (!r.ok) return;
        RECT local = r.rect;
        OffsetRect(&local, -virt.left, -virt.top);
        g_lastRegion = r.rect;
        g_haveLastRegion = true;
        if (r.window) g_nameInfo = NameInfoFor(r.window);  // clicked a window: name the file after it
        Deliver(shot->Crop(local), r.rect, after, -1, allow);
    });
}

void StartColorPicker() {
    if (OverlayActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    ShowOverlay(OverlayMode::ColorPick, CaptureScreen(virt, false), virt, MakeOverlayOptions(), [](const OverlayResult& r) {
        if (!r.ok) return;
        wchar_t hex[16];
        swprintf_s(hex, L"#%02X%02X%02X", GetRValue(r.color), GetGValue(r.color), GetBValue(r.color));
        CopyTextToClipboard(g_hwnd, hex);
        auto swatch = Bitmap::Create(300, 56);
        if (swatch) {
            uint32_t px = 0xFF000000u | (GetRValue(r.color) << 16) | (GetGValue(r.color) << 8) | GetBValue(r.color);
            std::fill_n(swatch->Bits(), 300 * 56, px);
        }
        wchar_t rgb[48];
        swprintf_s(rgb, L"rgb(%d, %d, %d)", GetRValue(r.color), GetGValue(r.color), GetBValue(r.color));
        ShowToast(std::wstring(hex) + L" copied", rgb, swatch, nullptr, g_settings.toastMs);
    });
}

// ---- recording ----

std::wstring FormatBytes(uint64_t b) {
    wchar_t s[32];
    if (b >= 1024 * 1024) swprintf_s(s, L"%.1f MB", b / (1024.0 * 1024.0));
    else swprintf_s(s, L"%llu KB", (unsigned long long)(b / 1024));
    return s;
}

void OnRecordingDone(const RecordResult& r) {
    if (r.discarded) return Notify(L"Recording discarded");
    if (!r.ok) return Notify(L"Recording failed", r.error);
    CopyFileToClipboard(g_hwnd, r.path);
    wchar_t dur[32];
    swprintf_s(dur, L"%d:%02d", (int)r.seconds / 60, (int)r.seconds % 60);
    std::wstring body = std::wstring(r.format == RecordFormat::Gif ? L"GIF" : L"MP4") + L"  ·  " + dur +
                        L"  ·  " + FormatBytes(r.bytes) + L"\nCopied as a file — paste it anywhere.";
    if (!r.warning.empty()) body += L"\n" + r.warning;
    std::wstring path = r.path;
    const bool gif = r.format == RecordFormat::Gif;
    ShowToast(L"Recording saved", body, r.thumb, [path, gif] { gif ? OpenPath(path) : (void)OpenVideoEditor(path); }, g_settings.toastMs + 2500);
}

void BeginRecording(RecordFormat fmt, const RECT& rect, HWND window, const std::wstring& title) {
    RecordOptions o;
    o.rect = rect;
    o.window = window;
    o.format = fmt;
    o.fps = fmt == RecordFormat::Gif ? g_settings.gifFps : g_settings.videoFps;
    o.cursor = g_settings.recordCursor;
    o.systemAudio = g_settings.systemAudio;
    o.microphone = g_settings.microphone;
    o.gpuCapture = g_settings.gpuCapture;
    o.showClicks = g_settings.showClicks;
    o.showKeys = g_settings.showKeys;
    o.showGamepad = g_settings.showGamepad;
    o.gamepadCorner = ParsePadCorner(g_settings.gamepadCorner);
    o.gamepadOpacity = g_settings.gamepadOpacity / 100.f;
    o.countdownSeconds = g_settings.countdownSeconds;
    o.path = MakeCapturePath(g_settings.CapturesFolder(), fmt == RecordFormat::Gif ? L"gif" : L"mp4",
                             {title, WindowAppName(window), RectW(rect), RectH(rect)});
    Library::Shared().NoteCapture(o.path, WindowAppName(window), title);
    std::wstring err;
    if (!StartRecording(o, OnRecordingDone, &err)) Notify(L"Could not start recording", err);
}

// followWindow: a click on a window records that window (wherever it moves); a drag records a region.
void StartRecord(RecordFormat fmt, bool followWindow = false) {
    if (IsRecording()) return StopRecording(false);
    if (RecorderBusy()) return Notify(L"Still saving the previous recording…");
    if (OverlayActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    ShowOverlay(OverlayMode::Region, CaptureScreen(virt, false), virt, MakeOverlayOptions(),
                [fmt, followWindow](const OverlayResult& r) {
                    if (!r.ok) return;
                    if (RectW(r.rect) < 16 || RectH(r.rect) < 16) return Notify(L"Region is too small to record");
                    HWND w = followWindow ? r.window : nullptr;
                    BeginRecording(fmt, r.rect, w, WindowTitle(w ? w : r.window));
                });
}

void OpenSettings() {
    SettingsUiHost h;
    h.settings = &g_settings;
    h.icon = g_iconBig;
    for (const auto& c : kCmds) h.hotkeys.push_back({c.key, c.title, c.defaultHotkey});
    h.changed = [] {
        g_settings.Load(HotkeyDefs());
        ApplyEditorDefaults();
        if (!g_hotkeysSuspended) RegisterHotkeys();  // status shows inline in the window: no toast
    };
    h.suspendHotkeys = [](bool on) {
        g_hotkeysSuspended = on;
        if (on) UnregisterAllHotkeys();
        else RegisterHotkeys();
    };
    h.hotkeyStatus = [](const std::wstring& key) {
        auto it = g_hotkeyStatus.find(key);
        return it == g_hotkeyStatus.end() ? std::wstring() : it->second;
    };
    h.getStartup = [] { return IsRunAtStartup(); };
    h.setStartup = [](bool on) { SetRunAtStartup(on); };
    ShowSettingsWindow(h);
}

void OpenHistory() {
    GalleryHost h;
    h.folder = g_settings.CapturesFolder();
    h.icon = g_iconBig;
    h.regionHotkey = g_settings.Hotkey(L"CaptureRegion");
    h.upload = [](const std::wstring& path) { UploadAndCopyLink(path); };
    h.openImage = [](const std::wstring& path) { OpenEditorFile(path); };
    h.editorOpen = [] { return EditorCount() > 0; };
    h.addToEditor = [](const std::vector<std::wstring>& paths) { AddImagesToEditor(paths); };
    h.collage = [](const std::vector<std::wstring>& paths) { OpenCollage(paths); };
    h.openVideo = [](const std::wstring& path) { OpenVideoEditor(path); };
    ShowGallery(h);
}

void StartScrolling() {
    if (OverlayActive() || ScrollingCaptureActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    const bool allow = g_uploadAllowed;
    ShowOverlay(OverlayMode::Region, CaptureScreen(virt, false), virt, MakeOverlayOptions(), [allow](const OverlayResult& r) {
        if (!r.ok) return;
        if (RectH(r.rect) < 80) return Notify(L"Region is too short for a scrolling capture");
        if (r.window) g_nameInfo = NameInfoFor(r.window);
        ShowToast(L"Scrolling capture…", L"Keep the mouse still. Press Esc to stop early.", nullptr, nullptr, 4000);
        RECT where = r.rect;
        StartScrollingCapture(r.rect, g_settings.scrollDelayMs, g_settings.scrollMaxFrames,
                              [where, allow](BitmapPtr img, int frames, std::wstring err) {
                                  HideToast();
                                  if (!err.empty()) return Notify(L"Scrolling capture failed", err);
                                  if (!img) return Notify(L"Nothing captured");
                                  if (frames < 2) Notify(L"The page didn't scroll", L"Saved a single frame instead.");
                                  Deliver(img, where, After::Default, -1, allow);
                              });
    });
}

void StartRuler() {
    if (OverlayActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    ShowOverlay(OverlayMode::Ruler, CaptureScreen(virt, false), virt, MakeOverlayOptions(), [](const OverlayResult&) {});
}

// Any picture or video, wherever it came from: pictures open in the editor, videos in the video editor.
void OpenFileToEdit(const std::wstring& file) {
    if (IsVideoFile(file)) OpenVideoEditor(file);
    else OpenEditorFile(file);
}

void OpenImageInEditor() {
    wchar_t file[MAX_PATH] = L"";
    std::wstring dir = g_settings.CapturesFolder();
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Pictures and videos\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic;*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv\0"
                      L"Pictures\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff;*.heic\0Videos\0*.mp4;*.mov;*.m4v;*.wmv;*.avi;*.mkv\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SetForegroundWindow(g_hwnd);
    if (!GetOpenFileNameW(&ofn)) return;
    OpenFileToEdit(file);
}

void CaptureActiveWindow(After after) {
    HWND fg = GetForegroundWindow();
    RECT r;
    if (!fg || IsOwnWindow(fg) || !GetWindowFrame(fg, &r)) return Notify(L"No active window to capture");
    CaptureRect(r, after);
}

// ---- palette ----

std::wstring HintFor(const CmdDef& c) {
    bool on;
    if (ToggleState(c.id, &on)) return on ? L"On" : L"Off";
    return g_settings.Hotkey(c.key);
}

std::vector<PaletteItem> BuildPaletteItems() {
    std::vector<PaletteItem> items;
    for (const auto& c : kCmds)
        if (c.id != CmdPalette && c.id != CmdStopRecording) items.push_back({c.id, c.title, HintFor(c), c.keywords, c.icon});
    auto rank = [](int id) {
        auto it = std::find(g_mru.begin(), g_mru.end(), id);
        return it == g_mru.end() ? INT_MAX : (int)(it - g_mru.begin());
    };
    std::stable_sort(items.begin(), items.end(), [&](const PaletteItem& a, const PaletteItem& b) { return rank(a.id) < rank(b.id); });
    if (IsRecording()) {
        const CmdDef* c = FindCmd(CmdStopRecording);
        items.insert(items.begin(), {c->id, c->title, HintFor(*c), c->keywords, c->icon});
    }
    g_recents = RecentCaptures(g_settings.CapturesFolder(), 8, true);
    for (size_t i = 0; i < g_recents.size(); ++i) {
        const bool video = IsVideoFile(g_recents[i]);
        items.push_back({CmdRecentBase + (int)i, FileNameOf(g_recents[i]), L"Recent", video ? L"recent open history video recording file" : L"recent open history image file",
                         (wchar_t)(video ? 0xE714 : 0xEB9F)});
    }
    return items;
}

void TogglePalette() {
    if (PaletteVisible()) return HidePalette(true);
    if (OverlayActive()) return;
    if (g_settings.ChangedOnDisk()) ReloadSettings(false);
    ShowPalette(BuildPaletteItems(), [](int id) { Execute(id, true); });
}

// ---- commands ----

void Toggle(bool& value, const wchar_t* section, const wchar_t* key, const wchar_t* label) {
    value = !value;
    g_settings.WriteBool(section, key, value);
    Notify(std::wstring(label) + (value ? L": On" : L": Off"));
}

// Post-capture action requested on the command line (--pin, --edit, ...), consumed by the next capture.
After g_cliAfterPending = After::Default;
After TakeCliAfter() {
    After a = g_cliAfterPending;
    g_cliAfterPending = After::Default;
    return a;
}

// ---- updates ----

std::optional<UpdateInfo> g_update;  // a newer version found by the last check
std::wstring g_updateAnnounced;      // the version the automatic check already announced
bool g_updating = false;
bool g_quitting = false;  // the app is shutting down: a finished download must not restart it
constexpr wchar_t kDownloadPage[] = L"https://github.com/AskTinNguyen/AtherScreenshot#download";

void CheckForUpdates(bool manual) {
    CheckForUpdateAsync([manual](std::optional<UpdateInfo> info, std::wstring err) {
        if (!err.empty()) {
            if (manual) Notify(L"Couldn't check for updates", err);
            return;
        }
        g_update = info;
        if (!info) {
            if (manual) Notify(L"Ather Screenshot is up to date", L"Version " ATHER_VERSION_WSTR);
            return;
        }
        if (!manual && g_updateAnnounced == info->version) return;  // once per version, unless asked
        g_updateAnnounced = info->version;
        ShowToast(L"Ather Screenshot " + info->version + L" is available", (info->notes.empty() ? L"" : info->notes + L"\n") +
                      L"Click to update (" + FormatBytes(info->size) + L"). Settings, saved captures and the gallery stay.",
                  nullptr, [] { InstallUpdate(); }, 15000);
    });
}

// The update restarts the app, so it waits until nothing is in progress, and asks before closing pins
// (they live only on screen).
bool UpdateBlocked() {
    if (RecorderBusy()) return Notify(L"Finish the recording first", L"Then update from the tray menu."), true;
    if (OverlayActive() || ScrollingCaptureActive()) return Notify(L"Finish the capture first", L"Then update from the tray menu."), true;
    if (EditorCount() > 0 || VideoEditorCount() > 0)
        return Notify(L"Close the editors first", L"Updating restarts Ather Screenshot. Save and close the editors, then update from the tray menu."), true;
    if (const int pins = PinCount()) {
        const std::wstring q = L"Updating restarts Ather Screenshot, which closes " + std::to_wstring(pins) +
                               (pins == 1 ? L" pinned screenshot." : L" pinned screenshots.") + L"\n\nUpdate now?";
        SetForegroundWindow(g_hwnd);
        if (MessageBoxW(nullptr, q.c_str(), kProductName, MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDOK) return true;
    }
    return false;
}

void InstallUpdate() {
    if (!g_update || g_updating || g_quitting || UpdateBlocked()) return;
    g_updating = true;
    const UpdateInfo info = *g_update;
    const uint64_t toast = ShowToast(L"Downloading Ather Screenshot " + info.version + L"…", L"0%", nullptr, nullptr, 600000);
    DownloadUpdateAsync(info, [toast](double p) { UpdateToastBody(toast, std::to_wstring((int)std::lround(p * 100)) + L"%"); },
                        [toast](std::wstring staged, std::wstring e) {
                            g_updating = false;
                            if (e.empty() && (g_quitting || UpdateBlocked())) {  // quitting, or something started while it downloaded
                                DeleteFileW(staged.c_str());
                                if (!g_quitting) HideToast();
                                return;
                            }
                            if (e.empty() && InstallStagedUpdate(staged, &e)) {
                                UpdateToastBody(toast, L"Restarting…");
                                Execute(CmdExit, false);  // the new version starts once this one has quit
                                return;
                            }
                            HideToast();
                            ShowToast(L"The update didn't install", e + L"\nClick to download it from GitHub instead.", nullptr,
                                      [] { OpenPath(kDownloadPage); },
                                      12000);
                        });
}

void Execute(int id, bool deferCapture, bool untrusted) {
    g_uploadAllowed = !untrusted;
    if (id >= CmdRecentBase) {
        size_t i = id - CmdRecentBase;
        if (i < g_recents.size()) IsVideoFile(g_recents[i]) ? (void)OpenVideoEditor(g_recents[i]) : OpenPath(g_recents[i]);
        return;
    }
    const CmdDef* def = FindCmd(id);
    if (!def) return;
    if (id != CmdPalette) {
        g_mru.erase(std::remove(g_mru.begin(), g_mru.end(), id), g_mru.end());
        g_mru.insert(g_mru.begin(), id);
        if (g_mru.size() > 5) g_mru.pop_back();
    }
    if (deferCapture && def->capture) {
        // Let the palette / menu disappear from the screen before we grab it.
        g_deferredCmd = id;
        g_deferredUntrusted = untrusted;
        SetTimer(g_hwnd, kDeferTimer, 60, nullptr);
        return;
    }
    switch (id) {
        case CmdPalette: TogglePalette(); break;
        case CmdRegion: StartRegion(TakeCliAfter()); break;
        case CmdRegionEdit: StartRegion(After::Edit); break;
        case CmdRecordVideo: StartRecord(RecordFormat::Mp4); break;
        case CmdRecordGif: StartRecord(RecordFormat::Gif); break;
        case CmdStopRecording: StopRecording(false); break;
        case CmdRecordWindow: StartRecord(RecordFormat::Mp4, true); break;
        case CmdPauseRecording:
            if (IsRecording()) TogglePauseRecording();
            else Notify(L"Not recording");
            break;
        case CmdRuler: StartRuler(); break;
        case CmdRegionRedact: StartRegion(After::Redact); break;
        case CmdRenameLast: {
            std::wstring path = g_lastPath;
            if (path.empty())
                if (auto recent = RecentCaptures(g_settings.CapturesFolder(), 1, true); !recent.empty()) path = recent[0];
            if (path.empty()) Notify(L"No saved capture yet");
            else PromptRename(path);
            break;
        }
        case CmdToggleAutoRedact: Toggle(g_settings.autoRedact, L"Capture", L"AutoRedact", L"Auto-redact"); break;
        case CmdRegionUpload: StartRegion(After::Upload); break;
        case CmdUploadLast: {
            std::wstring path = g_lastPath;
            if (path.empty())
                if (auto recent = RecentCaptures(g_settings.CapturesFolder(), 1, true); !recent.empty()) path = recent[0];
            if (path.empty()) Notify(L"No saved capture yet");
            else UploadAndCopyLink(path);
            break;
        }
        case CmdUploadFile: {
            wchar_t file[MAX_PATH] = L"";
            std::wstring dir = g_settings.CapturesFolder();
            OPENFILENAMEW ofn{sizeof(ofn)};
            ofn.hwndOwner = g_hwnd;
            ofn.lpstrFilter = L"Images and videos\0*.png;*.jpg;*.jpeg;*.gif;*.mp4\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrInitialDir = dir.c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            SetForegroundWindow(g_hwnd);
            if (GetOpenFileNameW(&ofn)) UploadAndCopyLink(file);
            break;
        }
        case CmdHistory: OpenHistory(); break;
        case CmdScrolling: StartScrolling(); break;
        case CmdToggleSystemAudio: Toggle(g_settings.systemAudio, L"Recording", L"RecordSystemAudio", L"System audio"); break;
        case CmdToggleMic: Toggle(g_settings.microphone, L"Recording", L"RecordMicrophone", L"Microphone"); break;
        case CmdToggleClicks: Toggle(g_settings.showClicks, L"Recording", L"ShowClicks", L"Show clicks"); break;
        case CmdToggleKeys: Toggle(g_settings.showKeys, L"Recording", L"ShowKeys", L"Show keystrokes"); break;
        case CmdToggleGamepad: Toggle(g_settings.showGamepad, L"Recording", L"ShowGamepad", L"Show game controller"); break;
        case CmdEditLast:
            if (g_last) OpenEditor(g_last);
            else if (auto recent = RecentCaptures(g_settings.CapturesFolder(), 1); !recent.empty())
                OpenEditorFile(recent[0]);
            else Notify(L"No capture yet");
            break;
        case CmdOpenImage: OpenImageInEditor(); break;
        case CmdFullscreen: CaptureRect(VirtualScreenRect(), TakeCliAfter()); break;
        case CmdMonitor: {
            POINT pt;
            GetCursorPos(&pt);
            CaptureRect(MonitorRectAt(pt), TakeCliAfter());
            break;
        }
        case CmdWindow: CaptureActiveWindow(TakeCliAfter()); break;
        case CmdLastRegion:
            if (g_haveLastRegion) CaptureRect(g_lastRegion, TakeCliAfter());
            else StartRegion(TakeCliAfter());
            break;
        case CmdRegionPin: StartRegion(After::Pin); break;
        case CmdOcr: StartRegion(After::Ocr); break;
        case CmdColorPicker: StartColorPicker(); break;
        case CmdRegionDelayed:
        case CmdFullscreenDelayed: {
            g_delayedCmd = id == CmdRegionDelayed ? CmdRegion : CmdFullscreen;
            g_delayedUntrusted = untrusted;
            int ms = g_settings.delaySeconds * 1000;
            ShowToast(L"Capturing in " + std::to_wstring(g_settings.delaySeconds) + L" s…", L"", nullptr, nullptr,
                      std::max(300, ms - 700));
            SetTimer(g_hwnd, kDelayTimer, ms, nullptr);
            break;
        }
        case CmdPinLast:
            if (g_last) PinImage(g_last, nullptr);
            else Notify(L"No capture yet");
            break;
        case CmdCopyLast:
            if (g_last && CopyImageToClipboard(g_hwnd, *g_last)) Notify(L"Copied to clipboard");
            else Notify(L"No capture yet");
            break;
        case CmdOpenLast:
            if (!g_lastPath.empty()) IsVideoFile(g_lastPath) ? (void)OpenVideoEditor(g_lastPath) : OpenPath(g_lastPath);
            else if (auto recent = RecentCaptures(g_settings.CapturesFolder(), 1); !recent.empty()) OpenPath(recent[0]);
            else Notify(L"No saved capture yet");
            break;
        case CmdOpenFolder: {
            std::wstring dir = g_settings.CapturesFolder();
            CreateDirectoryW(dir.c_str(), nullptr);
            OpenPath(dir);
            break;
        }
        case CmdCloseAllPins: CloseAllPins(); break;
        case CmdToggleClipboard: Toggle(g_settings.copyToClipboard, L"Capture", L"CopyToClipboard", L"Copy to clipboard"); break;
        case CmdToggleSave:
            Toggle(g_settings.saveToFile, L"Capture", L"SaveToFile", L"Save to file");
            ApplyEditorDefaults();  // open editors' Done follows it too
            break;
        case CmdToggleCursor: Toggle(g_settings.captureCursor, L"Capture", L"CaptureCursor", L"Include cursor"); break;
        case CmdToggleStartup: {
            bool on = !IsRunAtStartup();
            SetRunAtStartup(on);
            Notify(on ? L"Run at startup: On" : L"Run at startup: Off");
            break;
        }
        case CmdEditSettings: OpenSettings(); break;
        case CmdReloadSettings: ReloadSettings(true); break;
        case CmdAbout: {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring where = _wcsicmp(InstalledExePath().c_str(), self) == 0 ? L"Installed for this user" : L"Running portable";
            ShowToast(L"Ather Screenshot v" ATHER_VERSION_WSTR, where + L"  ·  Ctrl+Alt+K for every command",
                      RenderLogo(160, true), nullptr, g_settings.toastMs + 1500);
            break;
        }
        case CmdCheckUpdates: CheckForUpdates(true); break;
        case CmdExit: DestroyWindow(g_hwnd); break;
    }
}

// ---- command line: AtherScreenshot.exe <command> [--pin|--edit|--upload|--redact|--ocr] [--delay N] ----
//
// Trust: commands typed in a console are trusted. Commands from atherscreenshot:// links, or from processes
// without a console (scheduled tasks, launchers, other apps), ask first and can never upload, edit, pin or
// open files. A forwarded command proves it came from a console through the per-install cli-token.

struct CliRequest {
    int cmd = 0;
    bool fromLink = false;  // came from an atherscreenshot:// link
    After after = After::Default;
    int delayMs = 0;
    std::wstring file;  // edit / upload / pin <file>
    std::wstring error;
    std::vector<std::wstring> args;  // as given, for the confirmation prompt
};

std::wstring LowerStr(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Started from a console (cmd, PowerShell, Windows Terminal, or a script attached to one), as opposed to
// being launched by another app, a link or the scheduler. A GUI-subsystem exe can only tell by attaching.
bool InteractiveLaunch() {
    static const bool interactive = [] {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
        FreeConsole();
        return true;
    }();
    return interactive;
}

// A per-install secret in the support folder, readable only by this user. WM_COPYDATA can be sent by any
// process on the desktop, so forwarded commands without it are treated like links from the web.
std::wstring CliToken() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    const std::wstring path = SupportFolder() + L"\\cli-token";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[256] = {};
        DWORD read = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &read, nullptr);
        CloseHandle(f);
        std::wstring t(buf, buf + read);
        if (t.size() >= 32) return cached = t;
    }
    BYTE rnd[32];
    if (BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return L"";
    std::string hex;
    for (BYTE b : rnd) {
        char h[3];
        sprintf_s(h, "%02x", b);
        hex += h;
    }
    // Owner-only ACL: full access for this user, nobody else, no inherited entries.
    SECURITY_ATTRIBUTES sa{sizeof(sa)};
    PSECURITY_DESCRIPTOR sd = nullptr;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE info[256];
        DWORD len = 0;
        LPWSTR sid = nullptr;
        if (GetTokenInformation(tok, TokenUser, info, sizeof(info), &len) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info)->User.Sid, &sid)) {
            std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
            ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
            LocalFree(sid);
        }
        CloseHandle(tok);
    }
    sa.lpSecurityDescriptor = sd;
    f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, sd ? &sa : nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (sd) LocalFree(sd);
    if (f == INVALID_HANDLE_VALUE) return L"";
    DWORD wr = 0;
    WriteFile(f, hex.data(), (DWORD)hex.size(), &wr, nullptr);
    CloseHandle(f);
    return cached = std::wstring(hex.begin(), hex.end());
}

// atherscreenshot://region?pin&delay=3 -> {"region", "--pin", "--delay", "3"}
std::vector<std::wstring> ArgsFromUrl(const std::wstring& url) {
    std::vector<std::wstring> out;
    std::wstring rest = url.substr(url.find(L':') + 1);
    while (!rest.empty() && rest[0] == L'/') rest.erase(0, 1);
    const size_t q = rest.find(L'?');
    std::wstring host = rest.substr(0, q);
    while (!host.empty() && host.back() == L'/') host.pop_back();
    out.push_back(host);
    if (q == std::wstring::npos) return out;
    std::wstring query = rest.substr(q + 1);
    for (size_t start = 0; start <= query.size();) {
        size_t amp = query.find(L'&', start);
        std::wstring item = query.substr(start, amp == std::wstring::npos ? std::wstring::npos : amp - start);
        if (!item.empty()) {
            size_t eq = item.find(L'=');
            out.push_back(L"--" + item.substr(0, eq));
            if (eq != std::wstring::npos) out.push_back(item.substr(eq + 1));
        }
        if (amp == std::wstring::npos) break;
        start = amp + 1;
    }
    return out;
}

// The arguments after the program name. A link (atherscreenshot://…) is turned into the same form and is
// never trusted, whoever started it.
std::vector<std::wstring> SplitArgs(const std::wstring& cmdline, bool* fromLink) {
    std::vector<std::wstring> args;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    LocalFree(argv);
    *fromLink = !args.empty() && _wcsnicmp(args[0].c_str(), L"atherscreenshot:", 16) == 0;
    if (*fromLink) args = ArgsFromUrl(args[0]);
    return args;
}

// A file argument, judged by its shape only (never by touching the disk: from a link, even checking whether
// \\host\share\x exists would connect to that host). Flags like --pin or /pin don't qualify.
bool LooksLikePath(const std::wstring& a) {
    if (a.empty() || a[0] == L'-') return false;
    if (a[0] == L'/' && a.find_first_of(L"/\\.:", 1) == std::wstring::npos) return false;  // /pin
    return a.find_first_of(L"\\/:.") != std::wstring::npos;
}

CliRequest ParseCli(const std::vector<std::wstring>& args) {
    CliRequest req;
    req.args = args;
    static const std::pair<const wchar_t*, int> kAliases[] = {
        {L"region", CmdRegion},       {L"fullscreen", CmdFullscreen}, {L"screen", CmdFullscreen},
        {L"monitor", CmdMonitor},     {L"window", CmdWindow},         {L"last", CmdLastRegion},
        {L"ocr", CmdOcr},             {L"text", CmdOcr},              {L"color", CmdColorPicker},
        {L"record", CmdRecordVideo},  {L"video", CmdRecordVideo},     {L"mp4", CmdRecordVideo},
        {L"gif", CmdRecordGif},       {L"recordwindow", CmdRecordWindow}, {L"stop", CmdStopRecording},
        {L"pause", CmdPauseRecording}, {L"palette", CmdPalette},     {L"history", CmdHistory},
        {L"gallery", CmdHistory},     {L"settings", CmdEditSettings},
        {L"scroll", CmdScrolling},    {L"scrolling", CmdScrolling},   {L"ruler", CmdRuler},
        {L"upload", CmdUploadLast},   {L"edit", CmdEditLast},         {L"pin", CmdPinLast},
        {L"open", CmdOpenLast},       {L"folder", CmdOpenFolder},     {L"quit", CmdExit},
        {L"exit", CmdExit},
    };
    for (size_t i = 0; i < args.size(); ++i) {
        std::wstring a = LowerStr(args[i]);
        while (!a.empty() && (a[0] == L'-' || a[0] == L'/')) a.erase(0, 1);
        if (a == L"pin") {
            if (req.cmd) req.after = After::Pin;
            else req.cmd = CmdPinLast;
        } else if (a == L"edit" && req.cmd) req.after = After::Edit;
        else if (a == L"upload" && req.cmd) req.after = After::Upload;
        else if (a == L"redact") req.after = After::Redact;
        else if (a == L"ocr" && req.cmd) req.after = After::Ocr;
        else if (a == L"delay" && i + 1 < args.size()) req.delayMs = std::clamp(_wtoi(args[++i].c_str()), 0, 60) * 1000;
        else if (req.cmd && LooksLikePath(args[i])) req.file = args[i];  // checked only once trusted (RunCli)
        else if (!req.cmd) {
            for (const auto& [name, id] : kAliases)
                if (a == name) req.cmd = id;
            for (const auto& c : kCmds)  // any settings.ini hotkey name works too, e.g. CaptureRegionPin
                if (!req.cmd && _wcsicmp(a.c_str(), c.key) == 0) req.cmd = c.id;
            if (!req.cmd && i == 0 && LooksLikePath(args[i])) {
                req.cmd = CmdEditLast;  // AtherScreenshot.exe picture.png opens it in the editor
                req.file = args[i];
            }
            if (!req.cmd) req.error = L"Unknown command: " + args[i];
        }
    }
    return req;
}

// What an untrusted request may not do at all: send anything off the machine, or touch files.
bool CliTouchesFiles(const CliRequest& req) {
    if (!req.file.empty() || req.after == After::Upload) return true;
    switch (req.cmd) {
        case CmdRegionUpload: case CmdUploadLast: case CmdUploadFile: case CmdEditLast: case CmdPinLast:
        case CmdOpenLast: case CmdOpenImage: case CmdRenameLast: return true;
    }
    for (const auto& a : req.args)
        if (_wcsicmp(a.c_str(), L"--upload") == 0 || _wcsicmp(a.c_str(), L"/upload") == 0) return true;
    return false;
}

std::wstring JoinArgs(const std::vector<std::wstring>& args) {
    std::wstring s;
    for (const auto& a : args) s += (s.empty() ? L"" : L" ") + a;
    return s.size() > 200 ? s.substr(0, 200) + L"…" : s;
}

// Commands that use --pin/--edit/--upload/--redact/--ocr: a capture that the "after" action applies to.
bool UsesAfter(int cmd) {
    return cmd == CmdRegion || cmd == CmdFullscreen || cmd == CmdMonitor || cmd == CmdWindow || cmd == CmdLastRegion;
}

void RunCli(const CliRequest& req, bool trusted) {
    if (!req.error.empty())
        return ShowToast(L"Ather Screenshot command line", req.error +
                             L"\nUsage: AtherScreenshot.exe region|fullscreen|window|record|gif|stop|history|scroll|ruler|... "
                             L"[--pin|--edit|--upload|--redact|--ocr] [--delay N]",
                         nullptr, nullptr, 8000), void();
    if (!req.cmd) return;
    // Explorer's "Open with" and dragging a picture onto the exe launch it with just a path: opening that file in
    // the editor is what the user asked for, and nothing leaves the machine. Links can't produce this shape.
    const bool openWith = !trusted && !req.fromLink && req.cmd == CmdEditLast && !req.file.empty() && req.after == After::Default &&
                          req.args.size() == 1;
    if (!trusted && !openWith) {
        if (CliTouchesFiles(req))
            return ShowToast(L"Blocked a request from a link or another app",
                             L"“" + JoinArgs(req.args) + L"” can only be run from a command prompt.", nullptr, nullptr, 8000),
                   void();
        const std::wstring q = L"Run “" + JoinArgs(req.args) + L"”?\n\nA link or another app asked Ather Screenshot to run this command.";
        SetForegroundWindow(g_hwnd);
        if (MessageBoxW(nullptr, q.c_str(), kProductName, MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST |
                                                               MB_SETFOREGROUND) != IDOK)
            return;
    }
    if (!req.file.empty()) {  // file-based commands: never fall back to "the last capture" for a file that isn't there
        if (GetFileAttributesW(req.file.c_str()) == INVALID_FILE_ATTRIBUTES)
            return Notify(L"File not found", req.file);
        if (req.cmd == CmdUploadLast) return UploadAndCopyLink(req.file);
        if (req.cmd == CmdEditLast && IsVideoFile(req.file)) return (void)OpenVideoEditor(req.file);
        if (req.cmd == CmdPinLast && IsVideoFile(req.file)) return Notify(L"Videos can't be pinned", FileNameOf(req.file));
        if (req.cmd == CmdEditLast || req.cmd == CmdPinLast) {
            BitmapPtr img = LoadImageFile(req.file);
            if (!img) return Notify(L"Could not open image", FileNameOf(req.file));
            if (req.cmd == CmdEditLast) OpenEditor(img, {req.file, L"", L""});
            else PinImage(img, nullptr);
            return;
        }
    }
    // Only captures use an "after" action; for anything else it would linger and hijack the next capture.
    g_cliAfterPending = UsesAfter(req.cmd) ? req.after : After::Default;
    if (req.delayMs) {
        g_delayedCmd = req.cmd;
        g_delayedUntrusted = !trusted;
        SetTimer(g_hwnd, kDelayTimer, req.delayMs, nullptr);
    } else {
        Execute(req.cmd, false, !trusted);
    }
}

// The command line with relative file arguments made absolute, for forwarding to the running instance.
std::wstring AbsoluteCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return GetCommandLineW();
    std::wstring out;
    for (int i = 0; i < argc; ++i) {
        std::wstring a = argv[i];
        if (i > 0 && LooksLikePath(a) && _wcsnicmp(a.c_str(), L"atherscreenshot:", 16) != 0) {
            wchar_t full[32768];
            const DWORD n = GetFullPathNameW(a.c_str(), 32768, full, nullptr);
            if (n > 0 && n < 32768) a = full;
        }
        if (!out.empty()) out += L' ';
        out += L'"' + a + L'"';  // paths can't contain quotes
    }
    LocalFree(argv);
    return out;
}

// ---- tray ----

void AddTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icon;
    std::wstring tip = std::wstring(kProductName) + L"\nCommands: " + g_settings.Hotkey(L"CommandPalette");
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    auto add = [&](int id) {
        const CmdDef* c = FindCmd(id);
        std::wstring text = c->title;
        bool on = false;
        bool toggle = ToggleState(id, &on);
        std::wstring hk = g_settings.Hotkey(c->key);
        if (!hk.empty()) text += L"\t" + hk;
        AppendMenuW(menu, MF_STRING | (toggle && on ? MF_CHECKED : 0), id, text.c_str());
    };
    auto sep = [&] { AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); };
    if (IsRecording()) {
        add(CmdStopRecording);
        sep();
    }
    for (int id : {CmdRegion, CmdRegionEdit, CmdFullscreen, CmdMonitor, CmdLastRegion, CmdScrolling, CmdOcr,
                   CmdColorPicker, CmdRuler})
        add(id);
    sep();
    for (int id : {CmdRecordVideo, CmdRecordGif, CmdRecordWindow}) add(id);
    sep();
    for (int id : {CmdPalette, CmdHistory, CmdOpenFolder, CmdEditLast, CmdOpenImage, CmdUploadLast}) add(id);
    sep();
    for (int id : {CmdToggleClipboard, CmdToggleSave, CmdToggleCursor, CmdToggleAutoRedact, CmdToggleSystemAudio,
                   CmdToggleMic, CmdToggleStartup})
        add(id);
    sep();
    if (g_update) {
        const std::wstring label = L"Update to version " + g_update->version + L"…";
        AppendMenuW(menu, MF_STRING, CmdInstallUpdate, label.c_str());
    } else {
        add(CmdCheckUpdates);
    }
    for (int id : {CmdEditSettings, CmdExit}) add(id);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (id == CmdInstallUpdate) InstallUpdate();  // not a palette command, so not in kCmds
    else if (id) Execute(id, true);
}

// ---- main window ----

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_HOTKEY: Execute((int)w, false); return 0;
        case WM_APP_RUN: {
            auto* fn = reinterpret_cast<std::function<void()>*>(l);
            (*fn)();
            delete fn;
            return 0;
        }
        case WM_APP_PALETTE:
            if (!PaletteVisible()) TogglePalette();
            return 0;
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(l);
            if (cds->dwData != kCopyDataCli || !cds->lpData) return FALSE;
            std::wstring payload(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            payload = payload.c_str();  // drop the terminator
            // "<token>\x1e<command line>": only our own CLI started from a console knows the token.
            const size_t sep = payload.find(L'\x1e');
            if (sep == std::wstring::npos) return FALSE;
            const std::wstring token = CliToken();
            const bool tokenOk = !token.empty() && payload.compare(0, sep, token) == 0 && sep == token.size();
            const std::wstring cmd = payload.substr(sep + 1);
            RunOnUi([cmd, tokenOk] {  // don't keep the sender blocked
                bool fromLink = false;
                auto args = SplitArgs(cmd, &fromLink);
                CliRequest req = ParseCli(args);
                req.fromLink = fromLink;
                RunCli(req, tokenOk && !fromLink);
            });
            return TRUE;
        }
        case WM_APP_TRAY:
            switch (LOWORD(l)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    if (IsRecording()) StopRecording(false);  // tray click = stop, like ShareX
                    else TogglePalette();
                    break;
                case WM_CONTEXTMENU: ShowTrayMenu(); break;
            }
            return 0;
        case WM_TIMER:
            KillTimer(h, w);
            if (w == kLibraryTimer) Library::Shared().Refresh();
            else if (w == kDeferTimer) Execute(g_deferredCmd, false, g_deferredUntrusted);
            else if (w == kDelayTimer) Execute(g_delayedCmd, false, g_delayedUntrusted);
            else if (w == kUpdateTimer) {
                if (g_settings.checkUpdates) CheckForUpdates(false);
                SetTimer(h, kUpdateTimer, 24 * 60 * 60 * 1000, nullptr);  // and again tomorrow
            }
            return 0;
        case WM_DESTROY:
            g_quitting = true;
            RemoveTrayIcon();
            for (const auto& c : kCmds) UnregisterHotKey(h, c.id);
            PostQuitMessage(0);
            return 0;
    }
    if (m == g_msgTaskbarCreated && m) {
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // OLE, not just COM: dragging captures out of the gallery (DoDragDrop) needs it. Single-threaded apartment.
    OleInitialize(nullptr);
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    g_icon = CreateLogoIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()));
    g_iconBig = CreateLogoIcon(GetSystemMetricsForDpi(SM_CXICON, GetDpiForSystem()));

    {  // build tool: regenerate res\app.ico from the vector logo
        int n = 0;
        LPWSTR* av = CommandLineToArgvW(GetCommandLineW(), &n);
        const bool writeIcon = n == 3 && _wcsicmp(av[1], L"--write-icon") == 0;
        const bool ok = writeIcon && WriteLogoIco(av[2]);
        const bool selftest = n >= 2 && _wcsicmp(av[1], L"--selftest") == 0;
        const bool featureStats = n >= 3 && _wcsicmp(av[1], L"--feature-stats") == 0;
        const bool gallerySnaps = n >= 3 && _wcsicmp(av[1], L"--gallery-snapshots") == 0;
        const bool editorSnaps = n >= 3 && _wcsicmp(av[1], L"--editor-snapshots") == 0;
        const bool videoSnaps = n >= 3 && _wcsicmp(av[1], L"--video-snapshots") == 0;
        const bool editVideo = n >= 3 && _wcsicmp(av[1], L"--edit-video") == 0;
        const std::wstring filter = (selftest || featureStats || gallerySnaps || editorSnaps || videoSnaps || editVideo) && n >= 3 ? av[2] : L"";
        LocalFree(av);
        if (writeIcon) return ok ? 0 : 1;
        if (featureStats) return FeatureStats(filter);  // developer tool, see library_tests.cpp
        if (gallerySnaps) return GallerySnapshots(filter);  // developer tool, see gallery.cpp
        if (editorSnaps) return EditorSnapshots(filter);    // developer tool, see editor.cpp
        if (videoSnaps) return VideoEditorSnapshots(filter);  // developer tool, see videoeditor.cpp
        if (editVideo) {  // developer tool: just the video editor, without the rest of the app
            SetVideoEditorOptions(DefaultCapturesFolder(), nullptr);
            if (!OpenVideoEditor(filter)) return 1;
            MSG msg;
            while (VideoEditorCount() > 0 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            return 0;
        }
        if (selftest) {  // unit tests (see selftest.h)
            const int failures = test::Run(filter);
            Gdiplus::GdiplusShutdown(gdipToken);
            OleUninitialize();
            return failures;
        }
    }
    // Install / update / uninstall flow (when launched from Downloads etc.).
    if (int code = 0; RunInstallFlow(GetCommandLineW(), g_iconBig, &code)) return code;
    // Started by the in-app updater: wait for the old version to quit, then run as usual.
    const bool justUpdated = FinishUpdate(GetCommandLineW());

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AtherScreenshot.Instance");
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;  // read before any other API call
    int argc = 0;
    LocalFree(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (alreadyRunning && justUpdated) return 0;  // the old version never quit: leave it running, don't hand it our flag
    if (!alreadyRunning) CleanUpUpdateFiles();  // only the running instance owns these (another may be downloading)
    if (alreadyRunning) {
        // Already running: forward the command line, or with no arguments open the palette.
        if (HWND other = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (argc > 1) {
                // Only a command typed in a console carries the token; launches by other apps and links don't.
                std::wstring cmd = (InteractiveLaunch() ? CliToken() : std::wstring()) + L'\x1e' + AbsoluteCommandLine();
                COPYDATASTRUCT cds{kCopyDataCli, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (PVOID)cmd.c_str()};
                DWORD_PTR result = 0;
                SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &result);
            } else {
                PostMessageW(other, WM_APP_PALETTE, 0, 0);
            }
        }
        return 0;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_icon;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    HWND dispatcher = StartUiDispatcher();  // outlives g_hwnd, so work posted while quitting still runs
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    bool firstRun = g_settings.Load(HotkeyDefs());
    ApplyEditorDefaults();
    Library& lib = Library::Shared();
    lib.LoadIfNeeded();
    // Keeps "last capture" commands pointing at the right file after a rename or delete elsewhere.
    lib.onMoved = [](const std::wstring& from, const std::wstring& to) {
        if (g_lastPath == from) g_lastPath = to;
    };
    lib.onRemoved = [](const std::vector<std::wstring>& gone) {
        if (std::find(gone.begin(), gone.end(), g_lastPath) != gone.end()) g_lastPath.clear();
    };
    SetTimer(g_hwnd, kLibraryTimer, 4000, nullptr);  // scan and index in the background once things settle
    SetTimer(g_hwnd, kUpdateTimer, 60 * 1000, nullptr);  // look for a new version once things have settled
    RegisterHotkeys();
    AddTrayIcon();
    if (!g_hotkeyErrors.empty()) {
        ShowHotkeyErrors();
    } else if (firstRun) {
        Notify(L"Ather Screenshot is running",
               g_settings.Hotkey(L"CaptureRegion") + L" to capture  ·  " + g_settings.Hotkey(L"CommandPalette") +
                   L" for commands");
    }
    RefreshInstallRecord();  // after an update (or if a launch raced the updated copy): the version in Apps & features
    if (justUpdated) {
        Notify(L"Updated to Ather Screenshot " ATHER_VERSION_WSTR, L"Your settings, captures and gallery are just as you left them.");
    } else if (argc > 1) {  // first launch with a command: run it too
        bool fromLink = false;
        auto args = SplitArgs(GetCommandLineW(), &fromLink);
        CliRequest req = ParseCli(args);
        req.fromLink = fromLink;
        RunCli(req, InteractiveLaunch() && !fromLink);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Let an in-flight recording finalize so the file isn't left truncated, and let video exports finish
    // (their completion registers the saved file with the library). Work posted back meanwhile still runs:
    // the dispatcher window outlives g_hwnd.
    StopRecording(false);
    auto pump = [&] {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    };
    for (ULONGLONG end = GetTickCount64() + 20000; RecorderBusy() && GetTickCount64() < end;) {
        pump();
        Sleep(10);
    }
    for (ULONGLONG end = GetTickCount64() + 120000; VideoEditorsBusy() && GetTickCount64() < end;) {
        pump();
        Sleep(10);
    }
    WaitForPendingSaves(5000);  // first: their completions may register library changes
    pump();
    lib.Flush();  // writes anything not yet on disk, synchronously
    lib.StopBackgroundWork();
    StopUiDispatcher(dispatcher);
    Gdiplus::GdiplusShutdown(gdipToken);
    OleUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}

// ---- tests ----

ATHER_TEST(cli_link_arguments) {
    auto a = ArgsFromUrl(L"atherscreenshot://region?pin&delay=3");
    CHECK_EQ(a.size(), 4u);
    CHECK(a[0] == L"region" && a[1] == L"--pin" && a[2] == L"--delay" && a[3] == L"3");
    CHECK(ArgsFromUrl(L"atherscreenshot:fullscreen/") == std::vector<std::wstring>{L"fullscreen"});
    bool link = false;
    auto b = SplitArgs(L"AtherScreenshot.exe atherscreenshot://upload", &link);
    CHECK(link && b.size() == 1 && b[0] == L"upload");
    auto c = SplitArgs(L"\"C:\\x\\AtherScreenshot.exe\" region --pin", &link);
    CHECK(!link && c.size() == 2 && c[1] == L"--pin");
}

ATHER_TEST(cli_untrusted_requests_never_touch_files) {
    // Blocked outright: uploads, and anything that edits, pins or opens files.
    for (const auto& args : std::vector<std::vector<std::wstring>>{
             {L"upload"}, {L"region", L"--upload"}, {L"CaptureRegionUpload"}, {L"UploadFile"}, {L"edit"}, {L"pin"},
             {L"open"}, {L"OpenImageInEditor"}, {L"RenameLastCapture"}, {L"edit", L"C:\\Windows\\win.ini"}})
        CHECK(CliTouchesFiles(ParseCli(args)));
    // Allowed after confirmation: captures, including --pin / --edit of the new capture.
    for (const auto& args : std::vector<std::vector<std::wstring>>{
             {L"region"}, {L"region", L"--pin"}, {L"fullscreen", L"--edit"}, {L"record"}, {L"history"}, {L"stop"}})
        CHECK(!CliTouchesFiles(ParseCli(args)));
    CHECK_EQ(ParseCli({L"region", L"--pin"}).after, After::Pin);
    CHECK_EQ(ParseCli({L"gallery"}).cmd, (int)CmdHistory);
    CHECK(!ParseCli({L"nonsense"}).error.empty());
    // A link's network path is taken as a file by its shape (so it's blocked) without ever being opened.
    bool link = false;
    const CliRequest unc = ParseCli(SplitArgs(L"AtherScreenshot.exe \"atherscreenshot://region?x=//attacker.invalid/s/a.png\"", &link));
    CHECK(link);
    CHECK(unc.file == L"//attacker.invalid/s/a.png");
    CHECK(CliTouchesFiles(unc));
    CHECK(!LooksLikePath(L"--pin") && !LooksLikePath(L"/pin") && LooksLikePath(L"a.png") && LooksLikePath(L"C:\\x"));
    // "Open with": a bare path opens it in the editor.
    CHECK_EQ(ParseCli({L"C:\\shots\\a.png"}).cmd, (int)CmdEditLast);
    // --pin and friends only apply to captures.
    CHECK(UsesAfter(CmdRegion) && !UsesAfter(CmdRuler) && !UsesAfter(CmdScrolling) && !UsesAfter(CmdRecordVideo));
}

ATHER_TEST(cli_token_is_private_and_stable) {
    const std::wstring a = CliToken();
    CHECK(a.size() >= 32);
    CHECK(a == CliToken());
    const std::wstring path = SupportFolder() + L"\\cli-token";
    CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    // The file's DACL is protected and has one entry: this user.
    PSECURITY_DESCRIPTOR sd = nullptr;
    PACL dacl = nullptr;
    CHECK(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl,
                                nullptr, &sd) == ERROR_SUCCESS);
    CHECK(dacl && dacl->AceCount == 1);
    if (sd) LocalFree(sd);
}