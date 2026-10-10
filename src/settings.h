#pragma once
#include "common.h"

namespace ather {

struct HotkeyDef {
    const wchar_t* key;
    const wchar_t* defaultValue;
};

// Needs Ctrl, Alt or Win, unless the key is an F-key, PrintScreen, Pause or ScrollLock.
bool HotkeyAllowed(UINT mods, UINT vk);
// "Ctrl+Shift+K", "PrintScreen", "Alt+F1" ... -> MOD_* flags + virtual key. Fails for keys HotkeyAllowed rejects.
bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk);
// Inverse of ParseHotkey; empty if the key can't be used as a hotkey.
std::wstring HotkeyToText(UINT mods, UINT vk);

// Backed by %APPDATA%\AtherScreenshot\settings.ini (UTF-16, hand-editable).
class Settings {
public:
    bool copyToClipboard = true;
    bool saveToFile = true;
    bool captureCursor = false;
    bool showToast = true;
    bool crosshair = true;
    bool magnifier = true;
    int toastMs = 2500;
    int delaySeconds = 3;
    std::wstring saveFolder;            // empty = Pictures\AtherScreenshot
    std::wstring afterCapture = L"none";  // none | pin | open | edit | upload
    std::wstring fileNameTemplate;       // tokens: {yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}
    bool askForName = false;
    bool autoRedact = false;
    // recording
    int videoFps = 30;
    int gifFps = 15;
    bool recordCursor = true;
    bool systemAudio = true;
    bool microphone = false;
    int countdownSeconds = 3;
    bool showClicks = true;
    bool showKeys = false;
    bool showGamepad = false;
    std::wstring gamepadCorner = L"bottomright";  // topleft | topright | bottomleft | bottomright
    int gamepadOpacity = 100;                      // percent
    bool gpuCapture = true;
    // scrolling capture
    int scrollDelayMs = 400;
    int scrollMaxFrames = 60;
    // editor
    bool styledExport = false;
    // video editor
    std::wstring noteAuthor;  // [VideoEditor] NoteAuthor: the name on review notes (empty: Windows' display name)

    // gallery
    bool autoTag = false;  // [Gallery] AutoTag: apply suggested tags
    bool checkUpdates = true;  // [Updates] CheckAutomatically
    // upload
    std::wstring uploader = L"none";  // none | imgur | custom | s3
    std::wstring imgurClientId;
    std::wstring customUrl, customFileField = L"file", customHeaders, customResponseUrl;
    std::wstring s3Endpoint, s3Bucket, s3Region = L"auto", s3AccessKey, s3SecretKey, s3PublicUrl;

    // Returns true if the file did not exist and was created with defaults.
    bool Load(const std::vector<HotkeyDef>& hotkeyDefs);
    bool ChangedOnDisk() const;
    const std::wstring& Path() const { return path_; }
    std::wstring Hotkey(const wchar_t* key) const;
    std::wstring CapturesFolder() const;
    std::wstring Raw(const wchar_t* section, const wchar_t* key, const wchar_t* def) const { return Get(section, key, def); }
    void WriteBool(const wchar_t* section, const wchar_t* key, bool value);
    void WriteString(const wchar_t* section, const wchar_t* key, const std::wstring& value);

private:
    std::wstring Get(const wchar_t* section, const wchar_t* key, const wchar_t* def) const;
    bool GetBool(const wchar_t* section, const wchar_t* key, bool def) const;
    void WriteTemplate(const std::vector<HotkeyDef>& defs) const;
    FILETIME Stamp() const;

    std::wstring path_;
    std::vector<std::pair<std::wstring, std::wstring>> hotkeys_;
    FILETIME stamp_{};
};

}  // namespace ather
