#include "settings.h"

#include <shlobj.h>

#include <algorithm>
#include <cwctype>

#include "output.h"
#include "selftest.h"

namespace ather {

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static UINT KeyFromName(const std::wstring& n) {
    if (n.size() == 1) {
        wchar_t c = n[0];
        if (c >= L'a' && c <= L'z') return 'A' + (c - L'a');
        if (c >= L'0' && c <= L'9') return c;
        switch (c) {
            case L'`': return VK_OEM_3;
            case L'-': return VK_OEM_MINUS;
            case L'=': return VK_OEM_PLUS;
            case L'[': return VK_OEM_4;
            case L']': return VK_OEM_6;
            case L'\\': return VK_OEM_5;
            case L';': return VK_OEM_1;
            case L'\'': return VK_OEM_7;
            case L',': return VK_OEM_COMMA;
            case L'.': return VK_OEM_PERIOD;
            case L'/': return VK_OEM_2;
        }
        return 0;
    }
    if (n[0] == L'f' && n.size() <= 3 && iswdigit(n[1])) {
        int f = _wtoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) return VK_F1 + f - 1;
    }
    static const struct {
        const wchar_t* name;
        UINT vk;
    } kNames[] = {
        {L"printscreen", VK_SNAPSHOT}, {L"prtsc", VK_SNAPSHOT}, {L"prtscn", VK_SNAPSHOT}, {L"print", VK_SNAPSHOT},
        {L"space", VK_SPACE},          {L"enter", VK_RETURN},   {L"tab", VK_TAB},        {L"esc", VK_ESCAPE},
        {L"escape", VK_ESCAPE},        {L"insert", VK_INSERT},  {L"ins", VK_INSERT},     {L"delete", VK_DELETE},
        {L"del", VK_DELETE},           {L"home", VK_HOME},      {L"end", VK_END},        {L"pageup", VK_PRIOR},
        {L"pgup", VK_PRIOR},           {L"pagedown", VK_NEXT},  {L"pgdn", VK_NEXT},      {L"up", VK_UP},
        {L"down", VK_DOWN},            {L"left", VK_LEFT},      {L"right", VK_RIGHT},    {L"pause", VK_PAUSE},
        {L"scrolllock", VK_SCROLL},    {L"backspace", VK_BACK},
    };
    for (const auto& k : kNames)
        if (n == k.name) return k.vk;
    return 0;
}

std::wstring HotkeyToText(UINT mods, UINT vk) {
    std::wstring key;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) key = (wchar_t)vk;
    else if (vk >= VK_F1 && vk <= VK_F24) key = L"F" + std::to_wstring(vk - VK_F1 + 1);
    else {
        static const struct {
            UINT vk;
            const wchar_t* name;
        } kNames[] = {
            {VK_SNAPSHOT, L"PrintScreen"}, {VK_SPACE, L"Space"},   {VK_RETURN, L"Enter"},      {VK_TAB, L"Tab"},
            {VK_INSERT, L"Insert"},        {VK_DELETE, L"Delete"}, {VK_HOME, L"Home"},         {VK_END, L"End"},
            {VK_PRIOR, L"PageUp"},         {VK_NEXT, L"PageDown"}, {VK_UP, L"Up"},             {VK_DOWN, L"Down"},
            {VK_LEFT, L"Left"},            {VK_RIGHT, L"Right"},   {VK_PAUSE, L"Pause"},       {VK_SCROLL, L"ScrollLock"},
            {VK_BACK, L"Backspace"},       {VK_OEM_3, L"`"},       {VK_OEM_MINUS, L"-"},       {VK_OEM_PLUS, L"="},
            {VK_OEM_4, L"["},              {VK_OEM_6, L"]"},       {VK_OEM_5, L"\\"},          {VK_OEM_1, L";"},
            {VK_OEM_7, L"'"},              {VK_OEM_COMMA, L","},   {VK_OEM_PERIOD, L"."},      {VK_OEM_2, L"/"},
        };
        for (const auto& k : kNames)
            if (k.vk == vk) key = k.name;
    }
    if (key.empty()) return L"";
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    return s + key;
}

// A global shortcut must not steal ordinary typing: it needs Ctrl, Alt or Win, unless the key never
// types anything on its own (F-keys, PrintScreen, Pause, ScrollLock).
bool HotkeyAllowed(UINT mods, UINT vk) {
    if (mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) return true;
    return (vk >= VK_F1 && vk <= VK_F24) || vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_SCROLL;
}

// A key that produces text when pressed with these modifiers (so a global hotkey on it would eat typing).
static bool TypesText(UINT mods, UINT vk) {
    if (mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) return false;
    return (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_BACK ||
           (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || (vk >= VK_OEM_1 && vk <= VK_OEM_3) || (vk >= VK_OEM_4 && vk <= VK_OEM_8) || vk == VK_OEM_102;
}

bool ParseHotkey(const std::wstring& text, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find(L'+', start);
        std::wstring part = Lower(Trim(text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start)));
        if (part == L"ctrl" || part == L"control") mods |= MOD_CONTROL;
        else if (part == L"alt") mods |= MOD_ALT;
        else if (part == L"shift") mods |= MOD_SHIFT;
        else if (part == L"win" || part == L"cmd" || part == L"meta" || part == L"super") mods |= MOD_WIN;
        else {
            if (vk || part.empty()) return false;
            vk = KeyFromName(part);
            if (!vk) return false;
        }
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    // The recorder only offers HotkeyAllowed shortcuts. Values already in settings.ini keep working unless the key
    // would swallow typing (letters, digits, space, punctuation); e.g. Insert or Home bound on its own still works.
    return vk != 0 && (HotkeyAllowed(mods, vk) || !TypesText(mods, vk));
}

std::wstring Settings::Get(const wchar_t* section, const wchar_t* key, const wchar_t* def) const {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, def, buf, (DWORD)std::size(buf), path_.c_str());
    return Trim(buf);
}

bool Settings::GetBool(const wchar_t* section, const wchar_t* key, bool def) const {
    std::wstring v = Lower(Get(section, key, def ? L"1" : L"0"));
    return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
}

FILETIME Settings::Stamp() const {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fa);
    return fa.ftLastWriteTime;
}

bool Settings::ChangedOnDisk() const {
    FILETIME now = Stamp();
    return CompareFileTime(&stamp_, &now) != 0;
}

void Settings::WriteTemplate(const std::vector<HotkeyDef>& defs) const {
    std::wstring t =
        L"; AtherScreenshot settings.\r\n"
        L"; Save this file and it is picked up the next time the command palette opens\r\n"
        L"; (or run \"Reload settings\").\r\n\r\n"
        L"[Hotkeys]\r\n"
        L"; Format: Ctrl+Alt+Shift+Win+Key, e.g. PrintScreen, Ctrl+Shift+S, Alt+F1. Empty = unbound.\r\n";
    for (const auto& d : defs) t += std::wstring(d.key) + L"=" + d.defaultValue + L"\r\n";
    t +=
        L"\r\n[Capture]\r\n"
        L"CopyToClipboard=1\r\n"
        L"SaveToFile=1\r\n"
        L"; Empty = Pictures\\AtherScreenshot. Environment variables work, e.g. %USERPROFILE%\\Desktop\\Shots\r\n"
        L"SaveFolder=\r\n"
        L"; After every capture: none | pin | open | edit | upload\r\n"
        L"AfterCapture=none\r\n"
        L"CaptureCursor=0\r\n"
        L"DelaySeconds=3\r\n"
        L"; Tokens: {yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}\r\n"
        L"FileNameTemplate=Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}\r\n"
        L"; Ask for a file name after every capture\r\n"
        L"AskForName=0\r\n"
        L"; Pixelate emails, IPs, keys, card numbers found by OCR before copying/saving\r\n"
        L"AutoRedact=0\r\n"
        L"\r\n[Recording]\r\n"
        L"VideoFps=30\r\n"
        L"GifFps=15\r\n"
        L"RecordCursor=1\r\n"
        L"RecordSystemAudio=1\r\n"
        L"RecordMicrophone=0\r\n"
        L"CountdownSeconds=3\r\n"
        L"ShowClicks=1\r\n"
        L"; Shows typed keys in the video. Off by default: it would also show passwords you type.\r\n"
        L"ShowKeys=0\r\n"
        L"; Draws a connected game controller (Xbox-style, XInput) in a corner of the video\r\n"
        L"ShowGamepad=0\r\n"
        L"; topleft | topright | bottomleft | bottomright\r\n"
        L"GamepadCorner=bottomright\r\n"
        L"; How visible the controller is, 10-100 (percent)\r\n"
        L"GamepadOpacity=100\r\n"
        L"; Windows.Graphics.Capture (GPU). 0 = classic GDI capture\r\n"
        L"GpuCapture=1\r\n"
        L"\r\n[Scrolling]\r\n"
        L"DelayMs=400\r\n"
        L"MaxFrames=60\r\n"
        L"\r\n[Editor]\r\n"
        L"; Export with padding, rounded corners and a shadow\r\n"
        L"StyledExport=0\r\n"
        L"\r\n[Updates]\r\n"
        L"; Look for a new version on GitHub once a day. Updates install only when you click.\r\n"
        L"CheckAutomatically=1\r\n"
        L"\r\n[Gallery]\r\n"
        L"; Add suggested tags (chat, code, error, receipt...) to captures automatically\r\n"
        L"AutoTag=0\r\n"
        L"\r\n[Upload]\r\n"
        L"; none | imgur | custom | s3\r\n"
        L"Uploader=none\r\n"
        L"ImgurClientId=\r\n"
        L"; custom: multipart POST. Headers as Name: value; Name2: value2. ResponseUrl = JSON path (e.g. data.link) or empty for plain-text responses\r\n"
        L"CustomUrl=\r\n"
        L"CustomFileField=file\r\n"
        L"CustomHeaders=\r\n"
        L"CustomResponseUrl=\r\n"
        L"; s3: any S3-compatible store (AWS, Cloudflare R2, MinIO...). PublicUrl = base URL the files are served from\r\n"
        L"S3Endpoint=\r\n"
        L"S3Bucket=\r\n"
        L"S3Region=auto\r\n"
        L"S3AccessKey=\r\n"
        L"S3SecretKey=\r\n"
        L"S3PublicUrl=\r\n"
        L"\r\n[Interface]\r\n"
        L"ShowToast=1\r\n"
        L"ToastDurationMs=2500\r\n"
        L"Crosshair=1\r\n"
        L"Magnifier=1\r\n";
    HANDLE f = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    const wchar_t bom = 0xFEFF;  // UTF-16 so Get/WritePrivateProfileString keep Unicode paths intact
    WriteFile(f, &bom, sizeof(bom), &wr, nullptr);
    WriteFile(f, t.data(), (DWORD)(t.size() * sizeof(wchar_t)), &wr, nullptr);
    CloseHandle(f);
}

bool Settings::Load(const std::vector<HotkeyDef>& defs) {
    bool created = false;
    if (path_.empty()) path_ = SupportFolder() + L"\\settings.ini";
    if (GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteTemplate(defs);
        created = true;
    }
    // Files from older versions: add keys introduced since, so they're discoverable in the file.
    auto ensure = [&](const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
        wchar_t buf[8];
        if (GetPrivateProfileStringW(sec, key, L"\x1", buf, 8, path_.c_str()) == 1 && buf[0] == 1)
            WritePrivateProfileStringW(sec, key, def, path_.c_str());
    };
    for (const auto& d : defs) ensure(L"Hotkeys", d.key, d.defaultValue);
    static const wchar_t* const kNewKeys[][3] = {
        {L"Capture", L"FileNameTemplate", L"Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}"},
        {L"Capture", L"AskForName", L"0"},
        {L"Capture", L"AutoRedact", L"0"},
        {L"Recording", L"RecordSystemAudio", L"1"},
        {L"Recording", L"RecordMicrophone", L"0"},
        {L"Recording", L"CountdownSeconds", L"3"},
        {L"Recording", L"ShowClicks", L"1"},
        {L"Recording", L"ShowKeys", L"0"},
        {L"Recording", L"ShowGamepad", L"0"},
        {L"Recording", L"GamepadCorner", L"bottomright"},
        {L"Recording", L"GamepadOpacity", L"100"},
        {L"Recording", L"GpuCapture", L"1"},
        {L"Scrolling", L"DelayMs", L"400"},
        {L"Scrolling", L"MaxFrames", L"60"},
        {L"Editor", L"StyledExport", L"0"},
        {L"Gallery", L"AutoTag", L"0"},
        {L"Updates", L"CheckAutomatically", L"1"},
        {L"Upload", L"Uploader", L"none"},
        {L"Upload", L"ImgurClientId", L""},
        {L"Upload", L"CustomUrl", L""},
        {L"Upload", L"CustomFileField", L"file"},
        {L"Upload", L"CustomHeaders", L""},
        {L"Upload", L"CustomResponseUrl", L""},
        {L"Upload", L"S3Endpoint", L""},
        {L"Upload", L"S3Bucket", L""},
        {L"Upload", L"S3Region", L"auto"},
        {L"Upload", L"S3AccessKey", L""},
        {L"Upload", L"S3SecretKey", L""},
        {L"Upload", L"S3PublicUrl", L""},
    };
    for (const auto& k : kNewKeys) ensure(k[0], k[1], k[2]);

    hotkeys_.clear();
    for (const auto& d : defs) hotkeys_.emplace_back(d.key, Get(L"Hotkeys", d.key, d.defaultValue));

    copyToClipboard = GetBool(L"Capture", L"CopyToClipboard", true);
    saveToFile = GetBool(L"Capture", L"SaveToFile", true);
    captureCursor = GetBool(L"Capture", L"CaptureCursor", false);
    saveFolder = Get(L"Capture", L"SaveFolder", L"");
    afterCapture = Lower(Get(L"Capture", L"AfterCapture", L"none"));
    delaySeconds = std::max(1, _wtoi(Get(L"Capture", L"DelaySeconds", L"3").c_str()));
    videoFps = std::clamp(_wtoi(Get(L"Recording", L"VideoFps", L"30").c_str()), 1, 60);
    gifFps = std::clamp(_wtoi(Get(L"Recording", L"GifFps", L"15").c_str()), 1, 50);
    recordCursor = GetBool(L"Recording", L"RecordCursor", true);
    systemAudio = GetBool(L"Recording", L"RecordSystemAudio", true);
    microphone = GetBool(L"Recording", L"RecordMicrophone", false);
    countdownSeconds = std::clamp(_wtoi(Get(L"Recording", L"CountdownSeconds", L"3").c_str()), 0, 10);
    showClicks = GetBool(L"Recording", L"ShowClicks", true);
    showKeys = GetBool(L"Recording", L"ShowKeys", false);
    showGamepad = GetBool(L"Recording", L"ShowGamepad", false);
    gamepadCorner = Lower(Get(L"Recording", L"GamepadCorner", L"bottomright"));
    gamepadOpacity = std::clamp(_wtoi(Get(L"Recording", L"GamepadOpacity", L"100").c_str()), 10, 100);
    gpuCapture = GetBool(L"Recording", L"GpuCapture", true);
    fileNameTemplate = Get(L"Capture", L"FileNameTemplate", L"Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}");
    askForName = GetBool(L"Capture", L"AskForName", false);
    autoRedact = GetBool(L"Capture", L"AutoRedact", false);
    scrollDelayMs = std::clamp(_wtoi(Get(L"Scrolling", L"DelayMs", L"400").c_str()), 100, 3000);
    scrollMaxFrames = std::clamp(_wtoi(Get(L"Scrolling", L"MaxFrames", L"60").c_str()), 2, 400);
    styledExport = GetBool(L"Editor", L"StyledExport", false);
    autoTag = GetBool(L"Gallery", L"AutoTag", false);
    checkUpdates = GetBool(L"Updates", L"CheckAutomatically", true);
    uploader = Lower(Get(L"Upload", L"Uploader", L"none"));
    imgurClientId = Get(L"Upload", L"ImgurClientId", L"");
    customUrl = Get(L"Upload", L"CustomUrl", L"");
    customFileField = Get(L"Upload", L"CustomFileField", L"file");
    customHeaders = Get(L"Upload", L"CustomHeaders", L"");
    customResponseUrl = Get(L"Upload", L"CustomResponseUrl", L"");
    s3Endpoint = Get(L"Upload", L"S3Endpoint", L"");
    s3Bucket = Get(L"Upload", L"S3Bucket", L"");
    s3Region = Get(L"Upload", L"S3Region", L"auto");
    s3AccessKey = Get(L"Upload", L"S3AccessKey", L"");
    s3SecretKey = Get(L"Upload", L"S3SecretKey", L"");
    s3PublicUrl = Get(L"Upload", L"S3PublicUrl", L"");
    showToast = GetBool(L"Interface", L"ShowToast", true);
    toastMs = std::max(500, _wtoi(Get(L"Interface", L"ToastDurationMs", L"2500").c_str()));
    crosshair = GetBool(L"Interface", L"Crosshair", true);
    magnifier = GetBool(L"Interface", L"Magnifier", true);
    stamp_ = Stamp();
    return created;
}

std::wstring Settings::Hotkey(const wchar_t* key) const {
    for (const auto& [k, v] : hotkeys_)
        if (k == key) return v;
    return L"";
}

std::wstring Settings::CapturesFolder() const {
    if (saveFolder.empty()) return DefaultCapturesFolder();
    wchar_t buf[MAX_PATH * 2];
    DWORD n = ExpandEnvironmentStringsW(saveFolder.c_str(), buf, (DWORD)std::size(buf));
    std::wstring r = (n && n <= std::size(buf)) ? buf : saveFolder;
    while (!r.empty() && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
    return r;
}

void Settings::WriteBool(const wchar_t* section, const wchar_t* key, bool value) {
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", path_.c_str());
    stamp_ = Stamp();
}

void Settings::WriteString(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), path_.c_str());
    stamp_ = Stamp();
}

// ---- tests ----

ATHER_TEST(hotkeys_need_a_modifier_unless_special) {
    UINT m, vk;
    CHECK(ParseHotkey(L"Ctrl+Alt+K", m, vk) && vk == 'K' && m == (MOD_CONTROL | MOD_ALT));
    CHECK(ParseHotkey(L"PrintScreen", m, vk) && vk == VK_SNAPSHOT && m == 0);
    CHECK(ParseHotkey(L"F9", m, vk) && vk == VK_F9);
    CHECK(ParseHotkey(L"Shift+F2", m, vk));
    CHECK(ParseHotkey(L"Win+Shift+S", m, vk));
    CHECK(!ParseHotkey(L"K", m, vk));          // would stop K from typing
    CHECK(!ParseHotkey(L"Shift+K", m, vk));    // so would Shift+K
    CHECK(!ParseHotkey(L"Space", m, vk));
    CHECK(!ParseHotkey(L"Ctrl+Nope", m, vk));
    // Bindings saved by earlier versions keep working when they can't swallow typing.
    CHECK(ParseHotkey(L"Insert", m, vk) && vk == VK_INSERT);
    CHECK(ParseHotkey(L"Shift+Home", m, vk));
    CHECK(!HotkeyAllowed(0, VK_INSERT));  // but the shortcut recorder doesn't offer them
    CHECK(HotkeyToText(MOD_CONTROL | MOD_SHIFT, 'S') == L"Ctrl+Shift+S");
}
}  // namespace ather
