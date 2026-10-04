#include "palette.h"

#include <windowsx.h>

#include "logo.h"

#include <algorithm>
#include <cwctype>

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotPalette";
constexpr int kWidth = 640, kInputH = 56, kRowH = 42, kMaxRows = 8, kPad = 8, kFooterH = 32, kEmptyH = 64;
constexpr UINT_PTR kCaretTimer = 1;
constexpr wchar_t kIconFont[] = L"Segoe Fluent Icons";

struct Match {
    int item;  // -1: the "create" row
    int score;
    std::vector<int> pos;  // matched character positions in the title
};

struct State {
    HWND hwnd = nullptr;
    std::vector<PaletteItem> items;
    std::vector<Match> results;
    std::wstring query;
    int sel = 0, scroll = 0;
    POINT lastMouse{-1, -1};
    bool caretOn = true, hiding = false;
    std::function<void(int)> onPick;
    PaletteOptions opt;
    // text-prompt mode
    bool prompt = false;
    std::wstring promptMessage;
    std::function<void(std::wstring)> onSubmit;
    HWND prevForeground = nullptr;
    float scale = 0;
    HFONT fInput = nullptr, fTitle = nullptr, fHint = nullptr, fIcon = nullptr, fIconBig = nullptr,
          fFooter = nullptr, fBrand = nullptr;
} g;

int S(int v) { return Px(g.scale, v); }

// ---- fuzzy matching ----

bool IsWordStart(const std::wstring& t, size_t i) {
    if (i == 0) return true;
    wchar_t p = t[i - 1];
    return !iswalnum(p) || (iswlower(p) && iswupper(t[i]));
}

int FuzzyPass(const std::wstring& q, const std::wstring& t, std::vector<int>* pos, bool smart) {
    if (pos) pos->clear();
    int score = 0, prev = -2;
    size_t ti = 0;
    for (wchar_t qc0 : q) {
        const wchar_t qc = (wchar_t)towlower(qc0);
        size_t found = std::wstring::npos;
        if (smart) {
            // Prefer continuing a run, then the start of a word, then any occurrence.
            if (ti < t.size() && (int)ti == prev + 1 && towlower(t[ti]) == qc) found = ti;
            for (size_t i = ti; found == std::wstring::npos && i < t.size(); ++i)
                if (towlower(t[i]) == qc && IsWordStart(t, i)) found = i;
        }
        for (size_t i = ti; found == std::wstring::npos && i < t.size(); ++i)
            if (towlower(t[i]) == qc) found = i;
        if (found == std::wstring::npos) return -1;
        score += 1;
        if ((int)found == prev + 1) score += 6;
        if (IsWordStart(t, found)) score += 8;
        if (found == 0) score += 4;
        score -= std::min<int>((int)(found - ti), 4);
        prev = (int)found;
        ti = found + 1;
        if (pos) pos->push_back((int)found);
    }
    return score;
}

int Fuzzy(const std::wstring& q, const std::wstring& t, std::vector<int>* pos) {
    int s = FuzzyPass(q, t, pos, true);
    return s >= 0 ? s : FuzzyPass(q, t, pos, false);
}

// Keywords only match as word prefixes, so they don't make everything match everything.
bool KeywordMatch(const std::wstring& q, const std::wstring& kw) {
    for (size_t i = 0; i < kw.size(); ++i) {
        if (i > 0 && kw[i - 1] != L' ') continue;
        size_t k = 0;
        while (k < q.size() && i + k < kw.size() && towlower(kw[i + k]) == towlower(q[k])) ++k;
        if (k == q.size()) return true;
    }
    return false;
}

void Filter() {
    g.results.clear();
    g.sel = g.scroll = 0;
    if (g.prompt) return;  // a text prompt has no list
    std::wstring q;
    for (wchar_t c : g.query)
        if (!iswspace(c)) q += c;
    for (int i = 0; i < (int)g.items.size(); ++i) {
        Match m{i, 0, {}};
        if (!q.empty()) {
            int s = Fuzzy(q, g.items[i].title, &m.pos);
            if (s < 0) m.pos.clear();
            if (KeywordMatch(q, g.items[i].keywords)) s = std::max(s, 6 + (int)q.size() * 3);
            if (s < 0) continue;
            m.score = s;
        }
        g.results.push_back(std::move(m));
    }
    if (!q.empty())
        std::stable_sort(g.results.begin(), g.results.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (g.opt.onCreate && !q.empty()) {  // offer the typed text itself unless an item already has that name
        std::wstring typed = g.query;
        while (!typed.empty() && iswspace(typed.back())) typed.pop_back();
        while (!typed.empty() && iswspace(typed.front())) typed.erase(0, 1);
        const bool exists = std::any_of(g.items.begin(), g.items.end(),
                                        [&](const PaletteItem& it) { return _wcsicmp(it.title.c_str(), typed.c_str()) == 0; });
        if (!typed.empty() && !exists) g.results.insert(g.results.begin(), Match{-1, 0, {}});
    }
    g.sel = 0;
    g.scroll = 0;
}

// ---- layout & paint ----

int VisibleRows() { return std::min<int>((int)g.results.size(), kMaxRows); }
int ListTop() { return S(kInputH) + 1; }
int ListH() { return g.results.empty() ? S(kEmptyH) : VisibleRows() * S(kRowH) + 2 * S(kPad); }
int TotalH() { return ListTop() + ListH() + S(kFooterH); }

void RebuildFonts() {
    for (HFONT* f : {&g.fInput, &g.fTitle, &g.fHint, &g.fIcon, &g.fIconBig, &g.fFooter, &g.fBrand})
        if (*f) DeleteObject(*f);
    g.fInput = MakeFont(S(18));
    g.fTitle = MakeFont(S(14));
    g.fHint = MakeFont(S(12));
    g.fFooter = MakeFont(S(12));
    g.fBrand = MakeEyebrowFont(S(11));
    g.fIcon = MakeFont(S(16), FW_NORMAL, kIconFont);
    g.fIconBig = MakeFont(S(18), FW_NORMAL, kIconFont);
}

void Resize() {
    SetWindowPos(g.hwnd, nullptr, 0, 0, S(kWidth), TotalH(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EnsureVisible() {
    if (g.sel < g.scroll) g.scroll = g.sel;
    if (g.sel >= g.scroll + kMaxRows) g.scroll = g.sel - kMaxRows + 1;
}

void DrawHighlighted(HDC dc, const std::wstring& t, const std::vector<int>& pos, int x, int y, int right,
                     bool selected) {
    RECT clip{x, y - S(4), right, y + S(30)};
    std::vector<bool> matched(t.size());
    for (int p : pos) matched[p] = true;
    size_t i = 0;
    while (i < t.size() && x < right) {
        const bool hit = matched[i];
        size_t j = i;
        while (j < t.size() && matched[j] == hit) ++j;
        SetTextColor(dc, hit ? theme::kAccent : (selected ? theme::kText : theme::kTextDim));
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, t.c_str() + i, (UINT)(j - i), nullptr);
        SIZE sz{};
        GetTextExtentPoint32W(dc, t.c_str() + i, (int)(j - i), &sz);
        x += sz.cx;
        i = j;
    }
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bb = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bb);
    HGDIOBJ oldFont = SelectObject(dc, g.fInput);
    FillSolid(dc, rc, theme::kSurface);
    SetBkMode(dc, TRANSPARENT);

    // Search input
    const int ih = S(kInputH);
    SelectObject(dc, g.fIconBig);
    SetTextColor(dc, theme::kMuted);
    RECT ir{S(18), 0, S(18) + S(24), ih};
    DrawTextW(dc, L"\xE721", 1, &ir, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, g.fInput);
    const int tx = S(54);
    RECT qr{tx, 0, rc.right - S(18), ih};
    int caretX = tx;
    if (g.query.empty()) {
        SetTextColor(dc, theme::kMuted);
        DrawTextW(dc, g.prompt ? L"Type a name…" : g.opt.placeholder.empty() ? L"Search commands…" : g.opt.placeholder.c_str(), -1, &qr,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    } else {
        SIZE sz{};
        GetTextExtentPoint32W(dc, g.query.c_str(), (int)g.query.size(), &sz);
        bool overflow = sz.cx > RectW(qr) - S(4);
        SetTextColor(dc, theme::kText);
        DrawTextW(dc, g.query.c_str(), (int)g.query.size(), &qr,
                  DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (overflow ? DT_RIGHT : DT_LEFT));
        caretX = overflow ? qr.right : tx + sz.cx;
    }
    if (g.caretOn) FillSolid(dc, {caretX + 1, ih / 2 - S(11), caretX + 1 + std::max(1, S(2)), ih / 2 + S(11)}, theme::kAccent);
    FillSolid(dc, {0, ih, rc.right, ih + 1}, theme::kBorder);

    // Results
    const int top = ListTop() + S(kPad), rowH = S(kRowH);
    if (g.results.empty()) {
        SelectObject(dc, g.fTitle);
        SetTextColor(dc, theme::kMuted);
        RECT er{0, ListTop(), rc.right, ListTop() + ListH()};
        const std::wstring msg = g.prompt ? g.promptMessage : L"No matching commands";
        DrawTextW(dc, msg.c_str(), -1, &er, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    for (int r = 0; r < VisibleRows(); ++r) {
        const int idx = g.scroll + r;
        if (idx >= (int)g.results.size()) break;
        const Match& m = g.results[idx];
        PaletteItem created;
        if (m.item < 0) created = {-1, g.opt.createLabel + L" “" + g.query + L"”", L"↵", L"", 0xE710};
        const PaletteItem& it = m.item < 0 ? created : g.items[m.item];
        const bool selected = idx == g.sel;
        RECT row{S(kPad), top + r * rowH, rc.right - S(kPad), top + (r + 1) * rowH};
        const int cy = (row.top + row.bottom) / 2;
        if (selected) {
            FillRounded(dc, row, S(8), theme::kSelected);
            FillRounded(dc, {row.left, row.top + S(9), row.left + S(3), row.bottom - S(9)}, S(1), theme::kAccent);
        }

        SelectObject(dc, g.fIcon);
        SetTextColor(dc, selected ? theme::kAccent : theme::kMuted);
        RECT icr{row.left + S(10), row.top, row.left + S(34), row.bottom};
        DrawTextW(dc, &it.icon, 1, &icr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

        int right = row.right - S(10);
        if (!it.hint.empty()) {
            SelectObject(dc, g.fHint);
            SIZE hs{};
            GetTextExtentPoint32W(dc, it.hint.c_str(), (int)it.hint.size(), &hs);
            RECT pill{right - hs.cx - S(16), cy - S(11), right, cy + S(11)};
            FillRounded(dc, pill, S(5), selected ? theme::kBorder : theme::kBgRaised);
            SetTextColor(dc, selected ? theme::kText : theme::kMuted);
            DrawTextW(dc, it.hint.c_str(), (int)it.hint.size(), &pill, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            right = pill.left - S(12);
        }
        SelectObject(dc, g.fTitle);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        DrawHighlighted(dc, it.title, m.pos, row.left + S(44), cy - tm.tmHeight / 2, right, selected);
    }
    if ((int)g.results.size() > kMaxRows) {  // scroll indicator
        int trackTop = top, trackH = kMaxRows * rowH;
        int thumbH = std::max(S(20), trackH * kMaxRows / (int)g.results.size());
        int thumbY = trackTop + (trackH - thumbH) * g.scroll / ((int)g.results.size() - kMaxRows);
        FillSolid(dc, {rc.right - S(4), thumbY, rc.right - S(2), thumbY + thumbH}, theme::kBorder);
    }

    // Footer
    const int fy = rc.bottom - S(kFooterH);
    FillSolid(dc, {0, fy, rc.right, fy + 1}, theme::kBorder);
    SelectObject(dc, g.fFooter);
    SetTextColor(dc, theme::kMuted);
    RECT fr{S(18), fy, rc.right - S(18), rc.bottom};
    DrawTextW(dc, g.prompt ? L"↵ save     esc cancel" : L"↑↓ navigate     ↵ run     esc close", -1, &fr,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // Brand mark: A⁵ + letter-spaced eyebrow, as in the Ather identity.
    SelectObject(dc, g.fBrand);
    SetTextColor(dc, theme::kAccent);
    const std::wstring brand = L"ATHER SCREENSHOT";
    const int spacing = S(2);
    SIZE bs{};
    GetTextExtentPoint32W(dc, brand.c_str(), (int)brand.size(), &bs);
    const int bw = bs.cx + spacing * (int)brand.size();
    RECT br{fr.right - bw, fy, fr.right, rc.bottom};
    DrawSpacedText(dc, brand, br, DT_SINGLELINE | DT_VCENTER | DT_LEFT, spacing);
    const int ls = S(16);
    DrawLogo(dc, {br.left - S(8) - ls, (fy + rc.bottom) / 2 - ls / 2, br.left - S(8), (fy + rc.bottom) / 2 + ls / 2});

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bb);
    DeleteDC(dc);
}

// ---- behaviour ----

void Refresh(bool refilter) {
    if (refilter) {
        Filter();
        Resize();
    }
    g.caretOn = true;
    SetTimer(g.hwnd, kCaretTimer, 530, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void Pick(int idx) {
    if (idx < 0 || idx >= (int)g.results.size()) return;
    if (g.results[idx].item < 0) {
        auto create = g.opt.onCreate;
        std::wstring text = g.query;
        HidePalette(true);
        if (create) create(text);
        return;
    }
    int id = g.items[g.results[idx].item].id;
    auto cb = g.onPick;
    HidePalette(true);
    if (cb) cb(id);
}

int RowAt(int y) {
    int top = ListTop() + S(kPad);
    if (y < top) return -1;
    int idx = g.scroll + (y - top) / S(kRowH);
    return idx < g.scroll + VisibleRows() ? idx : -1;
}

void Move(int delta) {
    int n = (int)g.results.size();
    if (!n) return;
    g.sel = ((g.sel + delta) % n + n) % n;
    EnsureVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

void PasteText() {
    if (!OpenClipboard(g.hwnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            for (; *p && g.query.size() < 200; ++p)
                if (*p >= 32) g.query += *p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    Refresh(true);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) HidePalette(false);
            return 0;
        case WM_KEYDOWN: {
            const bool ctrl = GetKeyState(VK_CONTROL) < 0;
            switch (w) {
                case VK_ESCAPE:
                    if (!g.query.empty() && !g.prompt) {
                        g.query.clear();
                        Refresh(true);
                    } else {
                        HidePalette(true);
                    }
                    return 0;
                case VK_RETURN:
                    if (g.prompt) {
                        auto cb = std::move(g.onSubmit);
                        std::wstring text = g.query;
                        HidePalette(true);
                        if (cb) cb(text);
                    } else {
                        Pick(g.sel);
                    }
                    return 0;
                case VK_UP: Move(-1); return 0;
                case VK_DOWN: Move(1); return 0;
                case VK_TAB: Move(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0;
                case VK_PRIOR: Move(-std::min(g.sel, kMaxRows)); return 0;
                case VK_NEXT: Move(std::min((int)g.results.size() - 1 - g.sel, kMaxRows)); return 0;
                case VK_BACK:
                    if (g.query.empty()) return 0;
                    if (ctrl) {
                        size_t p = g.query.find_last_not_of(L' ');
                        p = p == std::wstring::npos ? 0 : g.query.find_last_of(L' ', p);
                        g.query.erase(p == std::wstring::npos ? 0 : p + 1);
                    } else {
                        g.query.pop_back();
                    }
                    Refresh(true);
                    return 0;
                case 'K':
                    if (ctrl) HidePalette(true);
                    return 0;
                case 'V':
                    if (ctrl) PasteText();
                    return 0;
                case 'J':
                case 'N':
                    if (ctrl) Move(1);
                    return 0;
                case 'P':
                    if (ctrl) Move(-1);
                    return 0;
            }
            return 0;
        }
        case WM_CHAR:
            if (w >= 32 && w != 127 && GetKeyState(VK_CONTROL) >= 0 && g.query.size() < 200) {
                g.query += (wchar_t)w;
                Refresh(true);
            }
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p.x == g.lastMouse.x && p.y == g.lastMouse.y) return 0;  // ignore synthetic moves after scrolling
            g.lastMouse = p;
            int r = RowAt(p.y);
            if (r >= 0 && r != g.sel) {
                g.sel = r;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int r = RowAt(GET_Y_LPARAM(l));
            if (r >= 0) Pick(r);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int maxScroll = std::max(0, (int)g.results.size() - kMaxRows);
            g.scroll = std::clamp(g.scroll + (GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1), 0, maxScroll);
            g.sel = std::clamp(g.sel, g.scroll, g.scroll + kMaxRows - 1);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (w == kCaretTimer) {
                g.caretOn = !g.caretOn;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DPICHANGED: return 0;  // we size ourselves for the target monitor
    }
    return DefWindowProcW(h, m, w, l);
}

void EnsureWindow() {
    if (g.hwnd) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"AtherScreenshot", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, wc.hInstance, nullptr);
    PrepareChromeless(g.hwnd, true);
    ExcludeFromCapture(g.hwnd);  // never appears in captures or recordings
}

void Present();

}  // namespace

void ShowPalette(std::vector<PaletteItem> items, std::function<void(int)> onPick, const PaletteOptions& options) {
    EnsureWindow();
    if (IsWindowVisible(g.hwnd)) {
        HidePalette(true);
        return;
    }
    g.prompt = false;
    g.onSubmit = nullptr;
    g.items = std::move(items);
    g.onPick = std::move(onPick);
    g.opt = options;
    g.query.clear();
    Present();
}

void ShowTextPrompt(const std::wstring& message, const std::wstring& initial, std::function<void(std::wstring)> onSubmit) {
    EnsureWindow();
    if (IsWindowVisible(g.hwnd)) HidePalette(false);
    g.prompt = true;
    g.promptMessage = message;
    g.onSubmit = std::move(onSubmit);
    g.items.clear();
    g.onPick = nullptr;
    g.opt = {};
    g.query = initial;
    Present();
}

namespace {
void Present() {
    HWND fg = GetForegroundWindow();
    g.prevForeground = (fg && fg != g.hwnd) ? fg : nullptr;  // may be our own editor window
    Filter();

    POINT pt;
    GetCursorPos(&pt);
    float s = DpiScaleAt(pt);
    if (s != g.scale) {
        g.scale = s;
        RebuildFonts();
    }
    RECT mon = MonitorRectAt(pt, true);
    int w = S(kWidth);
    SetWindowPos(g.hwnd, HWND_TOPMOST, mon.left + (RectW(mon) - w) / 2, mon.top + RectH(mon) / 5, w, TotalH(),
                 SWP_NOACTIVATE);
    GetCursorPos(&g.lastMouse);
    ScreenToClient(g.hwnd, &g.lastMouse);
    g.caretOn = true;
    SetTimer(g.hwnd, kCaretTimer, 530, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
    ShowWithoutFlash(g.hwnd, true);
}
}  // namespace

void HidePalette(bool restoreFocus) {
    if (!g.hwnd || !IsWindowVisible(g.hwnd) || g.hiding) return;
    g.hiding = true;
    KillTimer(g.hwnd, kCaretTimer);
    ShowWindow(g.hwnd, SW_HIDE);
    if (restoreFocus && g.prevForeground && IsWindow(g.prevForeground)) SetForegroundWindow(g.prevForeground);
    g.hiding = false;
}

bool PaletteVisible() { return g.hwnd && IsWindowVisible(g.hwnd); }

}  // namespace ather
