#include "quickprompts_win.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <thread>

#include "output.h"
#include "selftest.h"

namespace ather {
namespace {

QuickPromptsHost g_host;
QuickPromptsOptions g_opt;

// ---- listener (its own thread) ----

struct TriggerSet {
    std::vector<TriggerMatcher::Entry> entries;
    std::vector<std::wstring> ids;  // entries[i].tag -> prompt id
    uint64_t version = 0;
};

std::mutex g_mu;  // guards g_triggers and g_listenOpt
std::shared_ptr<const TriggerSet> g_triggers;
QuickPromptsOptions g_listenOpt;
std::atomic<uint64_t> g_keySeq{0};  // physical key presses seen; an insert gives up if it moved on
std::atomic<bool> g_listening{false};
std::atomic<bool> g_acceptInjected{false};
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
constexpr wchar_t kListenerClass[] = L"AtherQuickPromptsListener";

std::wstring ExeOfWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring path;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = (DWORD)std::size(buf);
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) path.assign(buf, n);
        CloseHandle(p);
    }
    return path;
}

bool IsFullscreenWindow(HWND w) {
    if (!w || w == GetDesktopWindow() || w == GetShellWindow()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return false;
    RECT r;
    if (!GetWindowRect(w, &r)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right &&
           r.bottom >= mi.rcMonitor.bottom && !(GetWindowLongW(w, GWL_STYLE) & WS_CAPTION);
}

struct Listener {
    TriggerMatcher matcher;
    uint64_t version = 0;
    HWND lastForeground = nullptr;

    void SyncTriggers() {
        std::shared_ptr<const TriggerSet> t;
        {
            std::lock_guard lock(g_mu);
            t = g_triggers;
        }
        if (t && t->version != version) {
            matcher.SetTriggers(t->entries);
            version = t->version;
        }
    }

    void OnMatch(int tag, int erase) {
        std::shared_ptr<const TriggerSet> t;
        QuickPromptsOptions opt;
        {
            std::lock_guard lock(g_mu);
            t = g_triggers;
            opt = g_listenOpt;
        }
        if (!t || tag < 0 || tag >= (int)t->ids.size()) return;
        HWND fg = GetForegroundWindow();
        if (!fg || IsOwnWindow(fg)) return;  // our own text boxes (the QuickPrompts editor has its own Try it)
        if (opt.skipFullscreen && IsFullscreenWindow(fg)) return;
        if (!opt.excludedApps.empty() && IsExcludedExe(ExeOfWindow(fg), opt.excludedApps)) return;
        const std::wstring id = t->ids[tag];
        const uint64_t seq = g_keySeq.load();
        RunOnUi([id, fg, erase, seq] {
            QuickPrompt p;
            if (QuickPromptStore::Shared().Find(id, &p)) InsertQuickPrompt(p, fg, erase, seq);
        });
    }

    void OnKey(const RAWKEYBOARD& k) {
        if (k.Flags & RI_KEY_BREAK) return;
        UINT vk = k.VKey;
        if (vk == 0 || vk == 0xFF) return;  // fake keys (e.g. the prefix of some extended keys)
        switch (vk) {
            case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_CAPITAL: case VK_NUMLOCK:
            case VK_SCROLL:
                return;  // modifiers alone change nothing
        }
        g_keySeq.fetch_add(1);
        SyncTriggers();
        HWND fg = GetForegroundWindow();
        if (fg != lastForeground) {
            matcher.Reset();
            lastForeground = fg;
        }
        const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0, alt = GetAsyncKeyState(VK_MENU) < 0;
        const bool win = GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
        const bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
        const bool altGr = ctrl && alt && GetAsyncKeyState(VK_RMENU) < 0;
        if (win || ((ctrl || alt) && !altGr)) {  // a shortcut: it may move the caret or change the text
            matcher.Reset();
            return;
        }
        switch (vk) {
            case VK_SPACE: {
                if (shift) {  // Shift+Space: a space, never an expansion
                    matcher.Char(L' ');
                    return;
                }
                int erase = 0;
                const int tag = matcher.Space(&erase);
                if (tag >= 0) OnMatch(tag, erase);
                return;
            }
            case VK_BACK: matcher.Backspace(); return;
            case VK_RETURN: case VK_TAB: case VK_ESCAPE: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT: case VK_DELETE: case VK_INSERT: case VK_PROCESSKEY:
                matcher.Reset();
                return;
        }
        BYTE state[256] = {};
        if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
        if (altGr) state[VK_CONTROL] = state[VK_MENU] = state[VK_LCONTROL] = state[VK_RMENU] = 0x80;
        if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
        HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(fg, nullptr));
        UINT scan = k.MakeCode | ((k.Flags & RI_KEY_E0) ? 0xE000 : 0);
        wchar_t buf[8] = {};
        // 0x4: don't change the keyboard state, so dead keys and accents in the person's typing are untouched.
        const int n = ToUnicodeEx(vk, scan, state, buf, (int)std::size(buf), 0x4, hkl);
        if (n < 0) {  // a dead key: what follows is a composed letter we can't follow
            matcher.Reset();
            return;
        }
        for (int i = 0; i < n; ++i) {
            if (iswspace(buf[i]) || iswcntrl(buf[i])) matcher.Reset();
            else matcher.Char(buf[i]);
        }
    }

    void OnMouse(const RAWMOUSE& m) {
        const USHORT down = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                            RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
        if (m.usButtonFlags & down) matcher.Reset();  // a click can move the caret
    }
};

LRESULT CALLBACK ListenerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_INPUT) {
        auto* listener = reinterpret_cast<Listener*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
        UINT size = sizeof(buf);
        if (listener && GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            const auto* ri = reinterpret_cast<const RAWINPUT*>(buf);
            // No device: SendInput (other apps typing for the person, our own paste, on-screen keyboards). Skip it.
            if (ri->header.hDevice || g_acceptInjected.load()) {
                if (ri->header.dwType == RIM_TYPEKEYBOARD) listener->OnKey(ri->data.keyboard);
                else if (ri->header.dwType == RIM_TYPEMOUSE) listener->OnMouse(ri->data.mouse);
            }
        }
        return DefWindowProcW(h, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

DWORD WINAPI ListenerThread(void* ready) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ListenerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kListenerClass;
    RegisterClassExW(&wc);
    HWND h = CreateWindowExW(0, kListenerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    Listener listener;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&listener);
    RAWINPUTDEVICE rid[2] = {{0x01, 0x06, RIDEV_INPUTSINK, h}, {0x01, 0x02, RIDEV_INPUTSINK, h}};
    const bool ok = h && RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    g_listening = ok;
    SetEvent((HANDLE)ready);
    MSG msg;
    while (ok && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    if (ok) {
        rid[0].dwFlags = rid[1].dwFlags = RIDEV_REMOVE;
        rid[0].hwndTarget = rid[1].hwndTarget = nullptr;
        RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    }
    if (h) DestroyWindow(h);
    g_listening = false;
    return 0;
}

void StartListener() {
    if (g_thread) return;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, ListenerThread, ready, 0, &g_threadId);
    if (g_thread) WaitForSingleObject(ready, 3000);
    CloseHandle(ready);
}

void StopListener() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_threadId = 0;
}

// ---- small timer queue on the UI thread ----

constexpr wchar_t kTimerClass[] = L"AtherQuickPromptsTimers";
HWND g_timerWnd = nullptr;
std::map<UINT_PTR, std::function<void()>> g_timers;
UINT_PTR g_nextTimer = 1;

LRESULT CALLBACK TimerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER) {
        KillTimer(h, w);
        auto it = g_timers.find(w);
        if (it != g_timers.end()) {
            auto fn = std::move(it->second);
            g_timers.erase(it);
            fn();
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void After(int ms, std::function<void()> fn) {
    if (!g_timerWnd) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = TimerProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kTimerClass;
        RegisterClassExW(&wc);
        g_timerWnd = CreateWindowExW(0, kTimerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    }
    const UINT_PTR id = g_nextTimer++;
    g_timers[id] = std::move(fn);
    SetTimer(g_timerWnd, id, std::max<UINT>(USER_TIMER_MINIMUM, (UINT)ms), nullptr);
}

// ---- clipboard ----

bool OpenClip(HWND owner) {
    for (int i = 0; i < 40; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(5);
    }
    return false;
}

struct ClipItem {
    UINT format;
    std::vector<BYTE> data;
};

struct ClipSnapshot {
    std::vector<ClipItem> items;
    bool complete = true;  // false: something couldn't be kept (too big, or not memory-based)
};

bool IsHandleFormat(UINT f) {
    switch (f) {
        case CF_BITMAP: case CF_METAFILEPICT: case CF_PALETTE: case CF_ENHMETAFILE: case CF_DSPBITMAP:
        case CF_DSPMETAFILEPICT: case CF_DSPENHMETAFILE: case CF_OWNERDISPLAY:
            return true;
    }
    return false;
}

ClipSnapshot SaveClipboard(HWND owner) {
    ClipSnapshot s;
    if (!OpenClip(owner)) {
        s.complete = false;
        return s;
    }
    size_t total = 0;
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        if (IsHandleFormat(f)) continue;  // CF_BITMAP comes back from CF_DIB on its own
        HANDLE h = GetClipboardData(f);
        if (!h) continue;
        const SIZE_T size = GlobalSize(h);
        if (!size) continue;
        if (total + size > 96u * 1024 * 1024) {
            s.complete = false;
            continue;
        }
        const void* p = GlobalLock(h);
        if (!p) continue;
        ClipItem it{f, std::vector<BYTE>((const BYTE*)p, (const BYTE*)p + size)};
        GlobalUnlock(h);
        total += size;
        s.items.push_back(std::move(it));
    }
    CloseClipboard();
    return s;
}

void SetGlobal(UINT format, const void* data, size_t size) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(size, 1));
    if (!g) return;
    if (size) memcpy(GlobalLock(g), data, size), GlobalUnlock(g);
    if (!SetClipboardData(format, g)) GlobalFree(g);
}

// Keeps a temporary clipboard entry out of Windows' clipboard history, cloud clipboard and clipboard managers.
void MarkPrivate() {
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    static const UINT cloud = RegisterClipboardFormatW(L"CanUploadToCloudClipboard");
    const DWORD zero = 0;
    SetGlobal(exclude, &zero, sizeof(zero));
    SetGlobal(history, &zero, sizeof(zero));
    SetGlobal(cloud, &zero, sizeof(zero));
}

bool RestoreClipboard(HWND owner, const ClipSnapshot& s) {
    if (!OpenClip(owner)) return false;
    EmptyClipboard();
    for (const auto& it : s.items) SetGlobal(it.format, it.data.data(), it.data.size());
    if (!s.items.empty()) MarkPrivate();  // it's back, not new: don't add it to the history again
    CloseClipboard();
    return true;
}

bool PutText(HWND owner, const std::wstring& text) {
    if (!OpenClip(owner)) return false;
    EmptyClipboard();
    SetGlobal(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    MarkPrivate();
    CloseClipboard();
    return true;
}

bool PutImage(HWND owner, const Bitmap& img) {
    if (!CopyImageToClipboard(owner, img, true)) return false;
    if (OpenClip(owner)) {
        MarkPrivate();
        CloseClipboard();
    }
    return true;
}

std::wstring ClipboardText(HWND owner) {
    std::wstring t;
    if (!OpenClip(owner)) return t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            t = p;
            GlobalUnlock(h);
        }
    CloseClipboard();
    return t;
}

// ---- typing keys ----

void Key(std::vector<INPUT>& v, WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_DELETE) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    v.push_back(in);
}

void Tap(std::vector<INPUT>& v, WORD vk, int times = 1) {
    for (int i = 0; i < times; ++i) {
        Key(v, vk, false);
        Key(v, vk, true);
    }
}

void Send(std::vector<INPUT>& v) {
    if (!v.empty()) SendInput((UINT)v.size(), v.data(), sizeof(INPUT));
    v.clear();
}

void Paste(bool shift) {
    std::vector<INPUT> v;
    Key(v, VK_CONTROL, false);
    if (shift) Key(v, VK_SHIFT, false);
    Tap(v, 'V');
    if (shift) Key(v, VK_SHIFT, true);
    Key(v, VK_CONTROL, true);
    Send(v);
}

bool ModifiersDown() {
    for (int vk : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN})
        if (GetAsyncKeyState(vk) < 0) return true;
    return false;
}

// ---- the insert ----

bool g_busy = false;

struct InsertJob {
    QuickPrompt prompt;
    HWND target = nullptr;
    int erase = 0;
    uint64_t keySeq = 0;
    Expansion exp;
    ClipSnapshot saved;
    DWORD ourSeq = 0;  // the clipboard's sequence number after our last change
    bool shiftPaste = false;
    BitmapPtr image;
};

void Finish(std::shared_ptr<InsertJob> job) {
    // Put back what was on the clipboard, unless someone copied something new meanwhile.
    After(500, [job] {
        if (GetClipboardSequenceNumber() == job->ourSeq) RestoreClipboard(nullptr, job->saved);
        g_busy = false;
    });
}

void RunInsert(std::shared_ptr<InsertJob> job) {
    HWND fg = GetForegroundWindow();
    if (job->target && fg != job->target) {
        SetForegroundWindow(job->target);
        fg = GetForegroundWindow();
    }
    if (job->keySeq && job->keySeq != g_keySeq.load()) {  // the person kept typing: deleting now would hit their text
        g_busy = false;
        return;
    }
    if (!fg || (job->target && fg != job->target)) {
        g_busy = false;
        return;
    }
    job->shiftPaste = PasteNeedsShift(ExeOfWindow(fg));
    job->saved = SaveClipboard(nullptr);
    if (!PutText(nullptr, ToClipboardText(job->exp.text))) {
        g_busy = false;
        return;
    }
    job->ourSeq = GetClipboardSequenceNumber();
    std::vector<INPUT> v;
    Tap(v, VK_BACK, job->erase);
    Send(v);
    Paste(job->shiftPaste);
    QuickPromptStore::Shared().NoteUsed(job->prompt.id);

    auto afterText = [job] {
        if (job->exp.cursorBack > 0) {
            After(120, [job] {
                std::vector<INPUT> keys;
                Tap(keys, VK_LEFT, job->exp.cursorBack);
                Send(keys);
                Finish(job);
            });
        } else {
            Finish(job);
        }
    };
    if (job->image) {
        After(250, [job, afterText] {
            if (PutImage(nullptr, *job->image)) {
                job->ourSeq = GetClipboardSequenceNumber();
                Paste(job->shiftPaste);
            }
            After(250, afterText);
        });
    } else {
        afterText();
    }
}

void WaitForModifiers(std::shared_ptr<InsertJob> job, int triesLeft) {
    if (ModifiersDown() && triesLeft > 0) {
        After(15, [job, triesLeft] { WaitForModifiers(job, triesLeft - 1); });
        return;
    }
    if (ModifiersDown()) {  // held for 3 s: pasting now would turn Ctrl+V into something else
        g_busy = false;
        return;
    }
    RunInsert(job);
}

}  // namespace

bool IsExcludedExe(const std::wstring& exePath, const std::wstring& excludedApps) {
    const std::wstring exe = FileNameOf(exePath);
    if (exe.empty()) return false;
    size_t start = 0;
    while (start <= excludedApps.size()) {
        size_t sep = excludedApps.find(L';', start);
        if (sep == std::wstring::npos) sep = excludedApps.size();
        std::wstring item = excludedApps.substr(start, sep - start);
        while (!item.empty() && iswspace(item.front())) item.erase(item.begin());
        while (!item.empty() && iswspace(item.back())) item.pop_back();
        if (!item.empty()) {
            if (_wcsicmp(item.c_str(), exe.c_str()) == 0) return true;
            // "notepad" matches notepad.exe
            if (item.find(L'.') == std::wstring::npos && _wcsicmp((item + L".exe").c_str(), exe.c_str()) == 0) return true;
        }
        start = sep + 1;
    }
    return false;
}

bool PasteNeedsShift(const std::wstring& exePath) {
    static const wchar_t* const kTerminals[] = {L"WindowsTerminal.exe", L"mintty.exe", L"alacritty.exe", L"wezterm-gui.exe",
                                                L"Hyper.exe", L"Tabby.exe", L"kitty.exe"};
    const std::wstring exe = FileNameOf(exePath);
    for (auto t : kTerminals)
        if (_wcsicmp(exe.c_str(), t) == 0) return true;
    return false;
}

std::wstring ToClipboardText(const std::wstring& text) {
    std::wstring o;
    o.reserve(text.size() + 16);
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n') o += L'\r';
        o += c;
    }
    return o;
}

void SetQuickPromptsHost(QuickPromptsHost host) { g_host = std::move(host); }

void RefreshQuickPromptTriggers() {
    auto t = std::make_shared<TriggerSet>();
    int i = 0;
    for (const auto& p : QuickPromptStore::Shared().Active()) {
        t->entries.push_back({p.trigger, p.caseSensitive, i++});
        t->ids.push_back(p.id);
    }
    static uint64_t version = 0;
    t->version = ++version;
    std::lock_guard lock(g_mu);
    g_triggers = t;
}

void ConfigureQuickPrompts(const QuickPromptsOptions& options) {
    g_opt = options;
    g_acceptInjected = options.acceptInjected;
    {
        std::lock_guard lock(g_mu);
        g_listenOpt = options;
    }
    RefreshQuickPromptTriggers();
    if (options.typing) StartListener();
    else StopListener();
}

void StopQuickPrompts() { StopListener(); }

bool QuickPromptsListening() { return g_listening.load(); }

bool QuickPromptInsertBusy() { return g_busy; }

void InsertQuickPrompt(const QuickPrompt& p, HWND target, int erase, uint64_t keySeq) {
    if (g_busy) return;
    g_busy = true;
    auto job = std::make_shared<InsertJob>();
    job->prompt = p;
    job->target = target;
    job->erase = erase;
    job->keySeq = keySeq;
    ExpandContext ctx;
    ctx.clipboard = ClipboardText(nullptr);
    ctx.lastCapture = g_host.lastCapture ? g_host.lastCapture() : L"";
    const time_t now = time(nullptr);
    localtime_s(&ctx.now, &now);
    job->exp = ExpandPlaceholders(p.text, ctx);
    if (job->exp.pasteImage) {
        if (!ctx.lastCapture.empty()) job->image = LoadImageFile(ctx.lastCapture);
        if (!job->image && g_host.notify)
            g_host.notify(L"No capture to attach", L"“" + (p.title.empty() ? p.trigger : p.title) +
                                                       L"” pastes your last screenshot. Take one first; the text was pasted without it.");
    }
    if (job->exp.text.empty() && !job->image) {
        g_busy = false;
        return;
    }
    WaitForModifiers(job, 200);
}

// ---- tests ----

ATHER_TEST(quickprompts_exclusions_and_paste_keys) {
    CHECK(IsExcludedExe(L"C:\\Program Files\\KeePass\\KeePass.exe", L"Code.exe; keepass.exe"));
    CHECK(IsExcludedExe(L"C:\\Windows\\notepad.exe", L"notepad"));
    CHECK(!IsExcludedExe(L"C:\\Windows\\notepad.exe", L"note;pad.exe;"));
    CHECK(!IsExcludedExe(L"", L"x.exe"));
    CHECK(PasteNeedsShift(L"C:\\x\\WindowsTerminal.exe"));
    CHECK(!PasteNeedsShift(L"C:\\x\\Code.exe"));
    CHECK(ToClipboardText(L"a\nb\r\nc") == L"a\r\nb\r\nc");
}

ATHER_TEST(quickprompts_clipboard_round_trip_keeps_formats) {
    // What a paste leaves behind: the clipboard as it was, text and picture both.
    const ClipSnapshot developers = SaveClipboard(nullptr);  // whoever runs the tests keeps their clipboard
    auto img = Bitmap::Create(8, 6);
    for (int i = 0; i < 48; ++i) img->Bits()[i] = 0xFF336699;
    CHECK(CopyImageToClipboard(nullptr, *img, true));
    CHECK(OpenClip(nullptr));
    SetGlobal(CF_UNICODETEXT, L"before", sizeof(L"before"));
    CloseClipboard();
    ClipSnapshot s = SaveClipboard(nullptr);
    CHECK(s.complete);
    CHECK(PutText(nullptr, L"temporary"));
    CHECK(ClipboardText(nullptr) == L"temporary");
    CHECK(RestoreClipboard(nullptr, s));
    CHECK(ClipboardText(nullptr) == L"before");
    CHECK(IsClipboardFormatAvailable(CF_DIB));
    RestoreClipboard(nullptr, developers);
}

}  // namespace ather
