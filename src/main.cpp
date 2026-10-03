#include "common.h"  // first: sets NOMINMAX before <windows.h>

#include <dwmapi.h>
#include <objbase.h>
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
#include "history.h"
#include "installer.h"
#include "logo.h"
#include "version.h"
#include "ocr.h"
#include "scroll.h"
#include "upload.h"
#include "output.h"
#include "overlay.h"
#include "palette.h"
#include "pin.h"
#include "recorder.h"
#include "settings.h"
#include "settingsui.h"
#include "toast.h"

#include <map>

#pragma comment(lib, "gdiplus")

using namespace ather;

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
    {CmdRegionRedact, L"CaptureRegionRedact", L"Capture region with auto-redact", L"privacy hide email key token password ocr pixelate", 0xE72E, L"", true},
    {CmdRenameLast, L"RenameLastCapture", L"Rename last capture…", L"name file title", 0xE8AC, L"", false},
    {CmdToggleAutoRedact, L"ToggleAutoRedact", L"Auto-redact every capture", L"toggle privacy ocr pixelate", 0xE72E, L"", false},
    {CmdRegionUpload, L"CaptureRegionUpload", L"Capture region and upload", L"share link imgur s3 url", 0xE898, L"Ctrl+Alt+U", true},
    {CmdUploadLast, L"UploadLastCapture", L"Upload last capture", L"share link imgur s3 url", 0xE898, L"", false},
    {CmdUploadFile, L"UploadFile", L"Upload a file…", L"share link imgur s3 url", 0xE898, L"", false},
    {CmdHistory, L"History", L"Capture history", L"browse gallery recent search thumbnails library", 0xE81C, L"Ctrl+Alt+H", false},
    {CmdScrolling, L"CaptureScrolling", L"Scrolling capture (long page)", L"scroll stitch full page chat long", 0xE8CB, L"Ctrl+Alt+S", true},
    {CmdEditLast, L"EditLastCapture", L"Annotate last capture", L"edit editor draw markup", 0xE70F, L"", false},
    {CmdOpenImage, L"OpenImageInEditor", L"Open image in editor…", L"edit file annotate load", 0xE8E5, L"", false},
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
    {CmdExit, L"Quit", L"Quit Ather Screenshot", L"exit close", 0xE7E8, L"", false},
};

enum class After { Default, Pin, Ocr, Edit, Redact, Upload };

constexpr wchar_t kMainClass[] = L"AtherScreenshotMain";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_PALETTE = WM_APP + 2;
constexpr ULONG_PTR kCopyDataCli = 0xA7E1;  // WM_COPYDATA tag for forwarded command lines
constexpr UINT_PTR kDeferTimer = 1, kDelayTimer = 2;

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
std::vector<int> g_mru;
std::vector<std::wstring> g_recents;

void Execute(int id, bool deferCapture);

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
    SetEditorDefaults({g_settings.CapturesFolder(), g_iconBig, g_settings.styledExport});
    SetFileNameTemplate(g_settings.fileNameTemplate);
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
                       if (g_lastPath == path) g_lastPath = np;
                       Notify(L"Renamed", FileNameOf(np));
                   });
}

// redactedCount < 0: auto-redact hasn't run yet for this image.
void Deliver(BitmapPtr img, const RECT& where, After after, int redactedCount = -1) {
    if (!img) {
        Notify(L"Capture failed");
        return;
    }
    if (redactedCount < 0 && (after == After::Redact || (g_settings.autoRedact && after != After::Ocr))) {
        ShowToast(L"Redacting sensitive text…", L"", nullptr, nullptr, 10000);
        RecognizeWordsAsync(img, [img, where, after](std::vector<OcrWord> words, std::wstring err) {
            auto clean = img->Crop({0, 0, img->Width(), img->Height()});
            int n = 0;
            if (err.empty())
                for (const RECT& r : FindSensitive(words)) {
                    clean->Pixelate(r, std::max(6, RectH(r) / 3));
                    ++n;
                }
            HideToast();
            Deliver(clean, where, after == After::Redact ? After::Default : after, n);
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
        OpenEditor(img);  // the editor copies/saves the annotated result when you press Done
        return;
    }

    const bool copied = g_settings.copyToClipboard && CopyImageToClipboard(g_hwnd, *img);
    const bool pin = after == After::Pin || g_settings.afterCapture == L"pin";
    if (pin) PinImage(img, &where);

    const bool upload = after == After::Upload || g_settings.afterCapture == L"upload";
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
    Deliver(CaptureScreen(r, g_settings.captureCursor), r, after);
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
    ShowOverlay(OverlayMode::Region, shot, virt, MakeOverlayOptions(), [shot, virt, after](const OverlayResult& r) {
        if (!r.ok) return;
        RECT local = r.rect;
        OffsetRect(&local, -virt.left, -virt.top);
        g_lastRegion = r.rect;
        g_haveLastRegion = true;
        if (r.window) g_nameInfo = NameInfoFor(r.window);  // clicked a window: name the file after it
        Deliver(shot->Crop(local), r.rect, after);
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
    ShowToast(L"Recording saved", body, r.thumb, [path] { OpenPath(path); }, g_settings.toastMs + 2500);
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
    o.countdownSeconds = g_settings.countdownSeconds;
    o.path = MakeCapturePath(g_settings.CapturesFolder(), fmt == RecordFormat::Gif ? L"gif" : L"mp4",
                             {title, WindowAppName(window), RectW(rect), RectH(rect)});
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
    ShowHistory({g_settings.CapturesFolder(), g_iconBig, [](const std::wstring& path) { UploadAndCopyLink(path); }});
}

void StartScrolling() {
    if (OverlayActive() || ScrollingCaptureActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    ShowOverlay(OverlayMode::Region, CaptureScreen(virt, false), virt, MakeOverlayOptions(), [](const OverlayResult& r) {
        if (!r.ok) return;
        if (RectH(r.rect) < 80) return Notify(L"Region is too short for a scrolling capture");
        if (r.window) g_nameInfo = NameInfoFor(r.window);
        ShowToast(L"Scrolling capture…", L"Keep the mouse still. Press Esc to stop early.", nullptr, nullptr, 4000);
        RECT where = r.rect;
        StartScrollingCapture(r.rect, g_settings.scrollDelayMs, g_settings.scrollMaxFrames,
                              [where](BitmapPtr img, int frames, std::wstring err) {
                                  HideToast();
                                  if (!err.empty()) return Notify(L"Scrolling capture failed", err);
                                  if (!img) return Notify(L"Nothing captured");
                                  if (frames < 2) Notify(L"The page didn't scroll", L"Saved a single frame instead.");
                                  Deliver(img, where, After::Default);
                              });
    });
}

void StartRuler() {
    if (OverlayActive()) return;
    PrepareCapture();
    const RECT virt = VirtualScreenRect();
    ShowOverlay(OverlayMode::Ruler, CaptureScreen(virt, false), virt, MakeOverlayOptions(), [](const OverlayResult&) {});
}

void OpenImageInEditor() {
    wchar_t file[MAX_PATH] = L"";
    std::wstring dir = g_settings.CapturesFolder();
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    SetForegroundWindow(g_hwnd);
    if (!GetOpenFileNameW(&ofn)) return;
    if (auto img = LoadImageFile(file)) OpenEditor(img);
    else Notify(L"Could not open image", FileNameOf(file));
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
    g_recents = RecentCaptures(g_settings.CapturesFolder(), 8);
    for (size_t i = 0; i < g_recents.size(); ++i)
        items.push_back({CmdRecentBase + (int)i, FileNameOf(g_recents[i]), L"Recent", L"recent open history image file", 0xEB9F});
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

void Execute(int id, bool deferCapture) {
    if (id >= CmdRecentBase) {
        size_t i = id - CmdRecentBase;
        if (i < g_recents.size()) OpenPath(g_recents[i]);
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
        case CmdEditLast:
            if (g_last) OpenEditor(g_last);
            else if (auto recent = RecentCaptures(g_settings.CapturesFolder(), 1); !recent.empty())
                OpenEditor(LoadImageFile(recent[0]));
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
            if (!g_lastPath.empty()) OpenPath(g_lastPath);
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
        case CmdToggleSave: Toggle(g_settings.saveToFile, L"Capture", L"SaveToFile", L"Save to file"); break;
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
        case CmdExit: DestroyWindow(g_hwnd); break;
    }
}

// ---- command line: AtherScreenshot.exe <command> [--pin|--edit|--upload|--redact|--ocr] [--delay N] ----

struct CliRequest {
    int cmd = 0;
    After after = After::Default;
    int delayMs = 0;
    std::wstring file;  // edit / upload / pin <file>
    std::wstring error;
};

CliRequest ParseCli(const std::wstring& cmdline) {
    CliRequest req;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    static const std::pair<const wchar_t*, int> kAliases[] = {
        {L"region", CmdRegion},       {L"fullscreen", CmdFullscreen}, {L"screen", CmdFullscreen},
        {L"monitor", CmdMonitor},     {L"window", CmdWindow},         {L"last", CmdLastRegion},
        {L"ocr", CmdOcr},             {L"text", CmdOcr},              {L"color", CmdColorPicker},
        {L"record", CmdRecordVideo},  {L"video", CmdRecordVideo},     {L"mp4", CmdRecordVideo},
        {L"gif", CmdRecordGif},       {L"recordwindow", CmdRecordWindow}, {L"stop", CmdStopRecording},
        {L"pause", CmdPauseRecording}, {L"palette", CmdPalette},     {L"history", CmdHistory},
        {L"scroll", CmdScrolling},    {L"scrolling", CmdScrolling},   {L"ruler", CmdRuler},
        {L"upload", CmdUploadLast},   {L"edit", CmdEditLast},         {L"pin", CmdPinLast},
        {L"open", CmdOpenLast},       {L"folder", CmdOpenFolder},     {L"quit", CmdExit},
        {L"exit", CmdExit},
    };
    for (int i = 1; i < argc; ++i) {  // argv[0] is the program
        std::wstring a = argv[i];
        for (auto& c : a) c = (wchar_t)towlower(c);
        while (!a.empty() && (a[0] == L'-' || a[0] == L'/')) a.erase(0, 1);
        if (a == L"pin") {
            if (req.cmd) req.after = After::Pin;
            else req.cmd = CmdPinLast;
        } else if (a == L"edit" && req.cmd) req.after = After::Edit;
        else if (a == L"upload" && req.cmd) req.after = After::Upload;
        else if (a == L"redact") req.after = After::Redact;
        else if (a == L"ocr" && req.cmd) req.after = After::Ocr;
        else if (a == L"delay" && i + 1 < argc) req.delayMs = std::clamp(_wtoi(argv[++i]), 0, 60) * 1000;
        else if (req.cmd && GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) req.file = argv[i];
        else if (!req.cmd) {
            for (const auto& [name, id] : kAliases)
                if (a == name) req.cmd = id;
            for (const auto& c : kCmds)  // any settings.ini hotkey name works too, e.g. CaptureRegionPin
                if (!req.cmd && _wcsicmp(a.c_str(), c.key) == 0) req.cmd = c.id;
            if (!req.cmd) req.error = L"Unknown command: " + std::wstring(argv[i]);
        }
    }
    LocalFree(argv);
    return req;
}

void RunCli(const CliRequest& req) {
    if (!req.error.empty())
        return ShowToast(L"Ather Screenshot command line", req.error +
                             L"\nUsage: AtherScreenshot.exe region|fullscreen|window|record|gif|stop|history|scroll|ruler|... "
                             L"[--pin|--edit|--upload|--redact|--ocr] [--delay N]",
                         nullptr, nullptr, 8000), void();
    if (!req.cmd) return;
    if (!req.file.empty()) {  // file-based commands
        if (req.cmd == CmdUploadLast) return UploadAndCopyLink(req.file);
        if (req.cmd == CmdEditLast || req.cmd == CmdPinLast) {
            BitmapPtr img = LoadImageFile(req.file);
            if (!img) return Notify(L"Could not open image", FileNameOf(req.file));
            if (req.cmd == CmdEditLast) OpenEditor(img);
            else PinImage(img, nullptr);
            return;
        }
    }
    g_cliAfterPending = req.after;
    if (req.delayMs) {
        g_delayedCmd = req.cmd;
        SetTimer(g_hwnd, kDelayTimer, req.delayMs, nullptr);
    } else {
        Execute(req.cmd, false);
    }
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
    for (int id : {CmdEditSettings, CmdExit}) add(id);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (id) Execute(id, true);
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
            std::wstring cmd(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            cmd = cmd.c_str();  // drop the terminator
            RunOnUi([cmd] { RunCli(ParseCli(cmd)); });  // don't keep the sender blocked
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
            if (w == kDeferTimer) Execute(g_deferredCmd, false);
            else if (w == kDelayTimer) Execute(g_delayedCmd, false);
            return 0;
        case WM_DESTROY:
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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
        LocalFree(av);
        if (writeIcon) return ok ? 0 : 1;
    }
    // Install / update / uninstall flow (when launched from Downloads etc.).
    if (int code = 0; RunInstallFlow(GetCommandLineW(), g_iconBig, &code)) return code;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AtherScreenshot.Instance");
    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;  // read before any other API call
    int argc = 0;
    LocalFree(CommandLineToArgvW(GetCommandLineW(), &argc));
    if (alreadyRunning) {
        // Already running: forward the command line, or with no arguments open the palette.
        if (HWND other = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            if (argc > 1) {
                std::wstring cmd = GetCommandLineW();
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
    SetUiWindow(g_hwnd);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    bool firstRun = g_settings.Load(HotkeyDefs());
    ApplyEditorDefaults();
    RegisterHotkeys();
    AddTrayIcon();
    if (!g_hotkeyErrors.empty()) {
        ShowHotkeyErrors();
    } else if (firstRun) {
        Notify(L"Ather Screenshot is running",
               g_settings.Hotkey(L"CaptureRegion") + L" to capture  ·  " + g_settings.Hotkey(L"CommandPalette") +
                   L" for commands");
    }
    if (argc > 1) RunCli(ParseCli(GetCommandLineW()));  // first launch with a command: run it too

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Let an in-flight recording finalize so the file isn't left truncated.
    StopRecording(false);
    for (ULONGLONG end = GetTickCount64() + 15000; RecorderBusy() && GetTickCount64() < end;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
    WaitForPendingSaves(5000);
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
