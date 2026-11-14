#include "quickpromptsui.h"

#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <map>

#include "output.h"
#include "selftest.h"
#include "settings.h"

#pragma comment(lib, "uxtheme")

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherQuickPrompts";
constexpr wchar_t kWelcomeClass[] = L"AtherQuickPromptsWelcome";
constexpr int kW = 1040, kH = 700, kTop = 64, kPad = 24, kLeftW = 360, kBtnH = 30, kFieldH = 32, kLabelH = 18;
constexpr UINT_PTR kSaveTimer = 1, kFilterTimer = 2;

enum Ctl : int {
    IdSearch = 100, IdList, IdTrigger, IdTitle, IdText, IdHotkey, IdTags, IdTry,
    IdNew = 200, IdDuplicate, IdDelete, IdImport, IdExport, IdTeam,
    IdChipBase = 300,  // one per placeholder chip
};

const wchar_t* const kChips[] = {L"{cursor}", L"{clipboard}", L"{date}", L"{lastcapture}", L"{lastcapture:image}"};

// A dialog filter literal with its embedded nulls (and the final double null).
template <size_t N>
std::wstring Filter(const wchar_t (&s)[N]) {
    return std::wstring(s, N);
}

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring FirstLine(const std::wstring& s) {
    size_t e = s.find_first_of(L"\r\n");
    std::wstring l = s.substr(0, e);
    return l.size() > 140 ? l.substr(0, 140) + L"…" : l;
}

std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, s.data(), n + 1);
    return s;
}

std::wstring JoinTags(const std::vector<std::wstring>& t) {
    std::wstring s;
    for (const auto& x : t) s += (s.empty() ? L"" : L" ") + x;
    return s;
}

std::vector<std::wstring> SplitTags(const std::wstring& s) {
    std::vector<std::wstring> v;
    std::wstring cur;
    for (wchar_t c : s + L" ") {
        if (iswspace(c) || c == L',') {
            if (!cur.empty()) v.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    return v;
}

void DrawTextIn(HDC dc, const std::wstring& t, RECT r, HFONT f, COLORREF c, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

int TextWidth(HDC dc, HFONT f, const std::wstring& t) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

// A switch like Settings' toggles.
void DrawSwitch(HDC dc, RECT r, bool on, float s) {
    FillRounded(dc, r, RectH(r), on ? theme::kAccent : theme::kBgRaised, on ? theme::kAccent : theme::kBorder);
    const int d = RectH(r) - Px(s, 6);
    const int x = on ? r.right - Px(s, 3) - d : r.left + Px(s, 3);
    RECT k{x, r.top + Px(s, 3), x + d, r.top + Px(s, 3) + d};
    FillRounded(dc, k, d, on ? theme::kOnAccent : theme::kMuted);
}

struct Row {
    bool header = false;
    std::wstring label;  // header text
    QuickPrompt p;
};

class Window {
public:
    HWND hwnd = nullptr;
    HWND search{}, list{}, trigger{}, title{}, text{}, hotkey{}, tags{}, tryBox{};
    std::vector<HWND> buttons;  // owner-drawn
    QuickPromptsUiHost host;
    std::vector<Row> rows;
    std::wstring currentId, status;
    bool statusError = false, loading = false, tryGuard = false, snapshot = false;
    float s = 1;
    HFONT fTitle{}, fLabel{}, fBody{}, fSmall{}, fMono{}, fMonoBold{}, fEyebrow{};
    HBRUSH bgBrush = CreateSolidBrush(theme::kBg), surfaceBrush = CreateSolidBrush(theme::kSurface),
           raisedBrush = CreateSolidBrush(theme::kBgRaised);
    RECT switchRect{}, headerRect{}, enabledRect{}, editorRect{};
    uint64_t storeVersion = 0;
    std::map<HWND, RECT> box;  // each field's drawn box; the edit sits inside it

    // Puts a field in its box: single-line edits are centred vertically (an EDIT draws its text at the top).
    void Place(HWND e, int x, int y, int w, int h) {
        box[e] = {x, y, x + w, y + h};
        if (e == text) {
            MoveWindow(e, x + S(2), y + S(5), w - S(4), h - S(10), TRUE);
            return;
        }
        const int ih = S(20);
        MoveWindow(e, x + S(2), y + (h - ih) / 2, w - S(4), ih, TRUE);
    }
    RECT BoxOf(HWND e) const {
        auto it = box.find(e);
        return it == box.end() ? RECT{} : it->second;
    }

    int S(int v) const { return Px(s, v); }
    RECT Client() const {
        RECT rc;
        GetClientRect(hwnd, &rc);
        return rc;
    }

    ~Window() {
        for (HFONT f : {fTitle, fLabel, fBody, fSmall, fMono, fMonoBold, fEyebrow})
            if (f) DeleteObject(f);
        DeleteObject(bgBrush);
        DeleteObject(surfaceBrush);
        DeleteObject(raisedBrush);
    }

    void Fonts() {
        for (HFONT* f : {&fTitle, &fLabel, &fBody, &fSmall, &fMono, &fMonoBold, &fEyebrow})
            if (*f) DeleteObject(*f);
        fTitle = MakeDisplayFont(S(30));
        fLabel = MakeEyebrowFont(S(12));
        fBody = MakeFont(S(14));
        fSmall = MakeFont(S(12));
        fMono = MakeFont(S(14), FW_NORMAL, L"Cascadia Mono");
        fMonoBold = MakeFont(S(14), FW_SEMIBOLD, L"Cascadia Mono");
        fEyebrow = MakeEyebrowFont(S(11));
    }

    HWND Edit(int id, DWORD style) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, hwnd,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)(id == IdText || id == IdTrigger ? fMono : fBody), TRUE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
        return e;
    }

    HWND Button(int id, const wchar_t* label) {
        HWND b = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, hwnd,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)fSmall, TRUE);
        buttons.push_back(b);
        return b;
    }

    void Create() {
        search = Edit(IdSearch, ES_AUTOHSCROLL);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search prompts");
        list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWVARIABLE | LBS_NOINTEGRALHEIGHT,
                               0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IdList, GetModuleHandleW(nullptr), nullptr);
        trigger = Edit(IdTrigger, ES_AUTOHSCROLL);
        title = Edit(IdTitle, ES_AUTOHSCROLL);
        text = Edit(IdText, ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | WS_VSCROLL);
        hotkey = Edit(IdHotkey, ES_AUTOHSCROLL);
        SendMessageW(hotkey, EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. Ctrl+Alt+1");
        tags = Edit(IdTags, ES_AUTOHSCROLL);
        SendMessageW(tags, EM_SETCUEBANNER, TRUE, (LPARAM)L"agent review");
        tryBox = Edit(IdTry, ES_AUTOHSCROLL);
        SendMessageW(tryBox, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type a trigger and Space, e.g. doitforme ");
        SendMessageW(text, EM_SETLIMITTEXT, 200000, 0);
        for (HWND h : {list, text}) SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        Button(IdNew, L"New");
        Button(IdDuplicate, L"Duplicate");
        Button(IdDelete, L"Delete");
        Button(IdImport, L"Import…");
        Button(IdExport, L"Export…");
        Button(IdTeam, L"Team file…");
        for (int i = 0; i < (int)std::size(kChips); ++i) Button(IdChipBase + i, kChips[i]);
    }

    void Layout() {
        const RECT rc = Client();
        const int L = S(kPad), top = S(kTop) + S(12), leftW = S(kLeftW);
        switchRect = {rc.right - S(kPad) - S(44), S(20), rc.right - S(kPad), S(44)};
        // Left: search, list, two rows of buttons.
        Place(search, L, top, leftW, S(kFieldH));
        const int btnTop = rc.bottom - S(kPad) - 2 * S(kBtnH) - S(8);
        MoveWindow(list, L, top + S(kFieldH) + S(10), leftW, btnTop - S(12) - (top + S(kFieldH) + S(10)), TRUE);
        const int bw = (leftW - 2 * S(8)) / 3;
        for (int i = 0; i < 6; ++i) {
            const int r = i / 3, c = i % 3;
            MoveWindow(buttons[i], L + c * (bw + S(8)), btnTop + r * (S(kBtnH) + S(8)), bw, S(kBtnH), TRUE);
        }
        // Right: the editor.
        const int x = L + leftW + S(28), right = rc.right - S(kPad), w = right - x;
        editorRect = {x, top, right, rc.bottom - S(kPad)};
        headerRect = {x, top, right, top + S(kFieldH)};
        int y = top + S(kFieldH) + S(14);
        const int trigW = std::min(S(240), w / 3);
        Place(trigger, x, y + S(kLabelH), trigW, S(kFieldH));
        Place(title, x + trigW + S(16), y + S(kLabelH), w - trigW - S(16), S(kFieldH));
        y += S(kLabelH) + S(kFieldH) + S(14);
        const int bottomBlock = S(kLabelH) + S(kFieldH) + S(14) + S(22) + S(10) + S(kLabelH) + S(kFieldH);  // shortcut/tags, status, try
        const int textTop = y + S(kLabelH), chipsH = S(26);
        const int textBottom = editorRect.bottom - bottomBlock - chipsH - S(20);
        Place(text, x, textTop, w, std::max(S(60), textBottom - textTop));
        int cx = x;
        HDC dc = GetDC(hwnd);
        for (int i = 0; i < (int)std::size(kChips); ++i) {
            const int cw = TextWidth(dc, fSmall, kChips[i]) + S(20);
            MoveWindow(buttons[6 + i], cx, textBottom + S(8), cw, chipsH, TRUE);
            cx += cw + S(6);
        }
        ReleaseDC(hwnd, dc);
        y = textBottom + S(8) + chipsH + S(14);
        Place(hotkey, x, y + S(kLabelH), trigW, S(kFieldH));
        Place(tags, x + trigW + S(16), y + S(kLabelH), w - trigW - S(16), S(kFieldH));
        y += S(kLabelH) + S(kFieldH) + S(14) + S(22) + S(10);
        Place(tryBox, x, y + S(kLabelH), w, S(kFieldH));
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Field labels sit just above their edit.
    void Label(HDC dc, HWND field, const std::wstring& t) {
        const RECT r = BoxOf(field);
        RECT l{r.left, r.top - S(kLabelH), r.right, r.top - S(3)};
        DrawSpacedText(dc, Upper(t), l, DT_LEFT | DT_BOTTOM | DT_SINGLELINE, 1);
    }

    void Paint(HDC dc) {
        const RECT rc = Client();
        FillSolid(dc, rc, theme::kBg);
        // Top bar
        RECT bar{0, 0, rc.right, S(kTop)};
        FillSolid(dc, bar, theme::kSurface);
        FillSolid(dc, {0, S(kTop) - 1, rc.right, S(kTop)}, theme::kBorder);
        DrawTextIn(dc, L"QUICKPROMPTS", {S(kPad), 0, S(420), S(kTop)}, fTitle, theme::kText);
        const bool on = host.typing && host.typing();
        DrawSwitch(dc, switchRect, on, s);
        RECT sl{rc.right / 2 - S(80), S(10), switchRect.left - S(12), S(34)};
        DrawTextIn(dc, L"Expand triggers as I type", sl, fBody, theme::kText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT sh{rc.right / 2 - S(220), S(32), switchRect.left - S(12), S(56)};
        const std::wstring picker = host.pickerHotkey ? host.pickerHotkey() : L"";
        DrawTextIn(dc, on ? L"Type a trigger, then Space, in any app" + (picker.empty() ? L"" : L"  ·  " + picker + L" lists them all")
                          : L"Off: triggers stay as typed" + (picker.empty() ? L"" : L"  ·  " + picker + L" still lists every prompt"),
                   sh, fSmall, theme::kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Editor header: source, enabled switch, uses.
        QuickPrompt p;
        const bool have = !currentId.empty() && QuickPromptStore::Shared().Find(currentId, &p);
        HGDIOBJ oldFont = SelectObject(dc, fLabel);
        SetTextColor(dc, theme::kMuted);
        SetBkMode(dc, TRANSPARENT);
        if (have) {
            Label(dc, trigger, L"Trigger");
            Label(dc, title, L"Title");
            Label(dc, text, L"Prompt");
            Label(dc, hotkey, L"Shortcut");
            Label(dc, tags, L"Tags");
        }
        Label(dc, tryBox, L"Try it");
        SelectObject(dc, oldFont);
        if (!have) {
            RECT r = editorRect;
            r.bottom = r.top + S(120);
            DrawTextIn(dc, L"Pick a prompt on the left, or New to write one.", r, fBody, theme::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else {
            const std::wstring kind = p.source == L"builtin" ? L"TEAM STARTER" : p.source == L"team" ? L"TEAM" : L"MY PROMPT";
            HDC m = dc;
            const int kw = TextWidth(m, fEyebrow, kind) + S(18);
            RECT pill{headerRect.left, headerRect.top + S(5), headerRect.left + kw, headerRect.bottom - S(5)};
            FillRounded(dc, pill, RectH(pill), p.source == L"user" ? theme::kSelected : theme::kBgRaised, theme::kBorder);
            DrawTextIn(dc, kind, pill, fEyebrow, p.source == L"user" ? theme::kAccent : theme::kTextDim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            std::wstring note = p.source == L"builtin" ? L"Built into the app. Duplicate it to change the text."
                              : p.source == L"team"    ? L"From the team file. Duplicate it to change the text."
                                                       : L"Saved as you type.";
            if (p.uses) note += L"  ·  Used " + std::to_wstring(p.uses) + (p.uses == 1 ? L" time" : L" times");
            RECT nr{pill.right + S(12), headerRect.top, headerRect.right - S(140), headerRect.bottom};
            DrawTextIn(dc, note, nr, fSmall, theme::kMuted);
            enabledRect = {headerRect.right - S(44), headerRect.top + S(5), headerRect.right, headerRect.bottom - S(5)};
            DrawSwitch(dc, enabledRect, p.enabled, s);
            RECT el{headerRect.right - S(140), headerRect.top, enabledRect.left - S(10), headerRect.bottom};
            DrawTextIn(dc, p.enabled ? L"On" : L"Off", el, fSmall, theme::kTextDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // Status line (validation, hotkey problems, unknown placeholders), above Try it.
        const RECT tr = BoxOf(tryBox);
        RECT st{editorRect.left, tr.top - S(kLabelH) - S(32), editorRect.right, tr.top - S(kLabelH) - S(8)};
        if (!status.empty()) DrawTextIn(dc, status, st, fSmall, statusError ? RGB(255, 120, 100) : theme::kMuted);
        // The fields' boxes (the edits sit inside, on the same colour).
        for (HWND e : {search, trigger, title, text, hotkey, tags, tryBox}) {
            if (!(GetWindowLongW(e, GWL_STYLE) & WS_VISIBLE)) continue;
            FillRounded(dc, BoxOf(e), S(6), theme::kBgRaised, GetFocus() == e ? theme::kAccent : theme::kBorder);
        }
    }

    void DrawButton(const DRAWITEMSTRUCT* d) {
        const int id = (int)d->CtlID;
        const bool chip = id >= IdChipBase;
        const bool disabled = d->itemState & ODS_DISABLED, pressed = d->itemState & ODS_SELECTED;
        FillSolid(d->hDC, d->rcItem, theme::kBg);
        RECT r = d->rcItem;
        const bool primary = id == IdNew;
        COLORREF fill = primary ? theme::kAccent : chip ? theme::kSurface : theme::kBgRaised;
        if (pressed) fill = primary ? theme::kAccentSoft : theme::kSelected;
        FillRounded(d->hDC, r, chip ? RectH(r) : S(6), fill, primary ? theme::kAccent : theme::kBorder);
        wchar_t label[64] = {};
        GetWindowTextW(d->hwndItem, label, 64);
        DrawTextIn(d->hDC, label, r, chip ? fSmall : fSmall,
                   disabled ? theme::kMuted : primary ? theme::kOnAccent : chip ? theme::kAccent : theme::kText,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void MeasureItem(MEASUREITEMSTRUCT* m) {
        const int i = (int)m->itemID;
        m->itemHeight = (i >= 0 && i < (int)rows.size() && rows[i].header) ? S(34) : S(56);
    }

    void DrawItem(const DRAWITEMSTRUCT* d) {
        if (d->itemID == (UINT)-1 || d->itemID >= rows.size()) return;
        const Row& row = rows[d->itemID];
        RECT r = d->rcItem;
        const bool sel = (d->itemState & ODS_SELECTED) && !row.header;
        FillSolid(d->hDC, r, sel ? theme::kSelected : theme::kSurface);
        if (row.header) {
            RECT t{r.left + S(12), r.top + S(10), r.right - S(12), r.bottom};
            HGDIOBJ old = SelectObject(d->hDC, fEyebrow);
            SetTextColor(d->hDC, theme::kMuted);
            SetBkMode(d->hDC, TRANSPARENT);
            DrawSpacedText(d->hDC, Upper(row.label), t, DT_LEFT | DT_VCENTER | DT_SINGLELINE, 1);
            SelectObject(d->hDC, old);
            return;
        }
        if (sel) FillSolid(d->hDC, {r.left, r.top + S(6), r.left + S(3), r.bottom - S(6)}, theme::kAccent);
        const QuickPrompt& p = row.p;
        const COLORREF trig = p.enabled ? theme::kAccent : theme::kMuted;
        RECT t1{r.left + S(14), r.top + S(7), r.right - S(60), r.top + S(29)};
        const int tw = std::min<int>(TextWidth(d->hDC, fMonoBold, p.trigger), RectW(t1) / 2);
        DrawTextIn(d->hDC, p.trigger, {t1.left, t1.top, t1.left + tw, t1.bottom}, fMonoBold, trig);
        DrawTextIn(d->hDC, p.title, {t1.left + tw + S(10), t1.top, t1.right, t1.bottom}, fBody, p.enabled ? theme::kText : theme::kMuted);
        RECT t2{r.left + S(14), r.top + S(29), r.right - S(14), r.bottom - S(7)};
        DrawTextIn(d->hDC, FirstLine(p.text), t2, fSmall, theme::kMuted);
        if (!p.enabled) {
            RECT pill{r.right - S(50), r.top + S(9), r.right - S(12), r.top + S(27)};
            FillRounded(d->hDC, pill, RectH(pill), theme::kBgRaised, theme::kBorder);
            DrawTextIn(d->hDC, L"Off", pill, fSmall, theme::kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (!p.hotkey.empty()) {
            RECT hk{r.right - S(130), t1.top, r.right - S(12), t1.bottom};
            DrawTextIn(d->hDC, p.hotkey, hk, fSmall, theme::kTextDim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    bool Matches(const QuickPrompt& p, const std::wstring& q) const {
        if (q.empty()) return true;
        const std::wstring hay = LowerW(p.trigger + L" " + p.title + L" " + p.text + L" " + JoinTags(p.tags));
        for (const auto& w : SplitTags(LowerW(q)))
            if (hay.find(w) == std::wstring::npos) return false;
        return true;
    }

    void Rebuild() {
        loading = true;
        storeVersion = QuickPromptStore::Shared().Version();
        const std::wstring q = GetText(search);
        rows.clear();
        auto all = QuickPromptStore::Shared().All();
        auto group = [&](const wchar_t* source, const wchar_t* label) {
            bool any = false;
            for (const auto& p : all) {
                if (p.source != source || !Matches(p, q)) continue;
                if (!any) rows.push_back({true, label, {}});
                any = true;
                rows.push_back({false, L"", p});
            }
        };
        group(L"builtin", L"Team starters");
        group(L"team", L"Team");
        group(L"user", L"My prompts");
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        const int topIndex = (int)SendMessageW(list, LB_GETTOPINDEX, 0, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        int sel = -1;
        for (int i = 0; i < (int)rows.size(); ++i) {
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"");
            if (!rows[i].header && rows[i].p.id == currentId) sel = i;
        }
        SendMessageW(list, LB_SETTOPINDEX, topIndex, 0);
        SendMessageW(list, LB_SETCURSEL, sel, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        loading = false;
    }

    void Select(const std::wstring& id) {
        KillTimer(hwnd, kSaveTimer);
        currentId = id;
        status.clear();
        statusError = false;
        QuickPrompt p;
        const bool have = !id.empty() && QuickPromptStore::Shared().Find(id, &p);
        loading = true;
        SetWindowTextW(trigger, have ? p.trigger.c_str() : L"");
        SetWindowTextW(title, have ? p.title.c_str() : L"");
        SetWindowTextW(text, have ? ToClipboardTextLocal(p.text).c_str() : L"");
        SetWindowTextW(hotkey, have ? p.hotkey.c_str() : L"");
        SetWindowTextW(tags, have ? JoinTags(p.tags).c_str() : L"");
        loading = false;
        const bool editable = have && !p.ReadOnly();
        for (HWND e : {trigger, title, text, hotkey, tags}) {
            SendMessageW(e, EM_SETREADONLY, !editable, 0);
            ShowWindow(e, have ? SW_SHOW : SW_HIDE);
        }
        for (int i = 0; i < (int)std::size(kChips); ++i) ShowWindow(buttons[6 + i], editable ? SW_SHOW : SW_HIDE);
        EnableWindow(buttons[1], have);                // Duplicate
        EnableWindow(buttons[2], editable);            // Delete
        if (have) UpdateStatus(p);
        for (int i = 0; i < (int)rows.size(); ++i)
            if (!rows[i].header && rows[i].p.id == id) SendMessageW(list, LB_SETCURSEL, i, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    static std::wstring ToClipboardTextLocal(const std::wstring& t) {
        std::wstring o;
        for (wchar_t c : t) {
            if (c == L'\r') continue;
            if (c == L'\n') o += L'\r';
            o += c;
        }
        return o;
    }

    void UpdateStatus(const QuickPrompt& p) {
        if (statusError) return;
        std::vector<std::wstring> notes;
        if (auto u = UnknownPlaceholders(p.text); !u.empty()) notes.push_back(L"Not a placeholder (left as typed): {" + u[0] + L"}");
        if (const std::wstring owner = QuickPromptStore::Shared().TriggerOwner(p.trigger, p.id); !owner.empty() && p.enabled) {
            QuickPrompt o;
            QuickPromptStore::Shared().Find(owner, &o);
            const bool mineWins = p.source == L"user" || (p.source == L"team" && o.source == L"builtin");
            notes.push_back(L"“" + p.trigger + L"” is also the trigger of “" + (o.title.empty() ? o.trigger : o.title) + L"”" +
                            (mineWins ? L"; this one wins." : L"; that one wins."));
        }
        if (!p.hotkey.empty() && host.hotkeyStatus)
            if (std::wstring hs = host.hotkeyStatus(p.id); !hs.empty()) notes.push_back(hs);
        status.clear();
        for (const auto& n : notes) status += (status.empty() ? L"" : L"   ·   ") + n;
    }

    void SaveNow() {
        KillTimer(hwnd, kSaveTimer);
        QuickPrompt p;
        if (currentId.empty() || !QuickPromptStore::Shared().Find(currentId, &p) || p.ReadOnly()) return;
        p.trigger = GetText(trigger);
        p.title = GetText(title);
        std::wstring t = GetText(text), clean;
        for (wchar_t c : t)
            if (c != L'\r') clean += c;
        p.text = clean;
        p.tags = SplitTags(GetText(tags));
        std::wstring hk = GetText(hotkey);
        while (!hk.empty() && iswspace(hk.back())) hk.pop_back();
        while (!hk.empty() && iswspace(hk.front())) hk.erase(hk.begin());
        UINT mods = 0, vk = 0;
        std::wstring hkError;
        if (!hk.empty() && !ParseHotkey(hk, mods, vk)) hkError = L"“" + hk + L"” isn't a shortcut Windows accepts (try Ctrl+Alt+1).";
        else p.hotkey = hk.empty() ? L"" : HotkeyToText(mods, vk);
        const std::wstring err = QuickPromptStore::Shared().Update(p);
        statusError = !err.empty() || !hkError.empty();
        status = !err.empty() ? err : hkError;
        if (err.empty()) {
            if (host.changed) host.changed();
            QuickPrompt saved;
            if (QuickPromptStore::Shared().Find(p.id, &saved)) {
                for (auto& r : rows)
                    if (!r.header && r.p.id == saved.id) r.p = saved;
                if (!statusError) UpdateStatus(saved);
                if (status.empty()) status = L"Saved";
            }
            storeVersion = QuickPromptStore::Shared().Version();
            InvalidateRect(list, nullptr, FALSE);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    std::wstring UniqueTrigger(std::wstring base) {
        auto& st = QuickPromptStore::Shared();
        if (base.size() > 28) base.resize(28);
        if (st.TriggerOwner(base, L"").empty() && CheckTrigger(base).empty()) return base;
        for (int i = 2;; ++i) {
            std::wstring t = base + std::to_wstring(i);
            if (st.TriggerOwner(t, L"").empty()) return t;
        }
    }

    void New(const std::wstring& textFrom = L"") {
        QuickPrompt p;
        p.trigger = UniqueTrigger(L"newprompt");
        p.title = L"New prompt";
        p.text = textFrom;
        std::wstring id;
        if (!QuickPromptStore::Shared().Add(p, &id).empty()) return;
        if (host.changed) host.changed();
        SetWindowTextW(search, L"");
        Rebuild();
        Select(id);
        SetFocus(trigger);
        SendMessageW(trigger, EM_SETSEL, 0, -1);
    }

    void Duplicate() {
        QuickPrompt p;
        if (!QuickPromptStore::Shared().Find(currentId, &p)) return;
        p.id.clear();
        p.source = L"user";
        p.trigger = UniqueTrigger(p.trigger + L"2");
        p.title = (p.title.empty() ? p.trigger : p.title) + L" (copy)";
        p.uses = 0;
        p.lastUsed.clear();
        p.hotkey.clear();
        p.enabled = true;
        std::wstring id;
        if (!QuickPromptStore::Shared().Add(p, &id).empty()) return;
        if (host.changed) host.changed();
        Rebuild();
        Select(id);
        SetFocus(trigger);
        SendMessageW(trigger, EM_SETSEL, 0, -1);
    }

    void Delete() {
        QuickPrompt p;
        if (!QuickPromptStore::Shared().Find(currentId, &p) || p.ReadOnly()) return;
        const std::wstring q = L"Delete “" + (p.title.empty() ? p.trigger : p.title) + L"”? This can't be undone.";
        if (MessageBoxW(hwnd, q.c_str(), L"QuickPrompts", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        QuickPromptStore::Shared().Remove(p.id);
        if (host.changed) host.changed();
        Rebuild();
        Select(L"");
    }

    void Import() {
        auto files = PickFiles(hwnd,
                               Filter(L"Prompts (*.json;*.csv;*.plist)\0*.json;*.csv;*.plist\0All files\0*.*\0"),
                               false, L"Import prompts (QuickPrompts JSON, CSV, or macOS Text Replacements)");
        if (files.empty()) return;
        std::vector<std::wstring> skipped;
        std::wstring err;
        const int n = QuickPromptStore::Shared().ImportFile(files[0], &skipped, &err);
        if (host.changed) host.changed();
        Rebuild();
        statusError = !err.empty();
        status = !err.empty() ? err : L"Imported " + std::to_wstring(n) + (n == 1 ? L" prompt" : L" prompts");
        if (!skipped.empty()) {
            status += L"; skipped " + std::to_wstring(skipped.size()) + L" whose trigger is taken or too short: ";
            for (size_t i = 0; i < skipped.size() && i < 5; ++i) status += (i ? L", " : L"") + skipped[i];
            if (skipped.size() > 5) status += L"…";
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    std::wstring SavePath(const wchar_t* filter, const wchar_t* ext, const wchar_t* name) {
        wchar_t buf[MAX_PATH * 2];
        wcscpy_s(buf, name);
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = filter;
        ofn.lpstrFile = buf;
        ofn.nMaxFile = (DWORD)std::size(buf);
        ofn.lpstrDefExt = ext;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        return GetSaveFileNameW(&ofn) ? std::wstring(buf) : L"";
    }

    void Export() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"My prompts as a team file (JSON)…");
        AppendMenuW(m, MF_STRING, 2, L"My prompts as CSV…");
        AppendMenuW(m, MF_STRING, 3, L"Every prompt shown, as a team file (JSON)…");
        RECT r;
        GetWindowRect(buttons[4], &r);
        const int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, r.left, r.top, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (!pick) return;
        std::vector<std::wstring> ids;
        for (const auto& row : rows)
            if (!row.header && (pick == 3 || row.p.source == L"user")) ids.push_back(row.p.id);
        if (ids.empty()) {
            status = L"Nothing to export: there are no prompts of your own yet.";
            statusError = true;
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        bool ok;
        std::wstring path;
        if (pick == 2) {
            path = SavePath(L"CSV (*.csv)\0*.csv\0\0", L"csv", L"quickprompts.csv");
            if (path.empty()) return;
            ok = QuickPromptStore::Shared().ExportCsv(path, ids);
        } else {
            path = SavePath(L"QuickPrompts team file (*.json)\0*.json\0\0", L"json", L"quickprompts-team.json");
            if (path.empty()) return;
            ok = QuickPromptStore::Shared().ExportJson(path, ids);
        }
        statusError = !ok;
        status = ok ? L"Exported " + std::to_wstring(ids.size()) + L" to " + path : L"Couldn't write " + path;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Team() {
        const std::wstring cur = host.teamFile ? host.teamFile() : L"";
        HMENU m = CreatePopupMenu();
        if (!cur.empty()) {
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (L"Now: " + cur).c_str());
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        }
        AppendMenuW(m, MF_STRING, 1, L"Choose the team prompts file…");
        if (!cur.empty()) AppendMenuW(m, MF_STRING, 2, L"Stop using the team file");
        RECT r;
        GetWindowRect(buttons[5], &r);
        const int pick = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, r.left, r.top, 0, hwnd, nullptr);
        DestroyMenu(m);
        std::wstring next = cur;
        if (pick == 1) {
            auto files = PickFiles(hwnd, Filter(L"Team prompts (*.json)\0*.json\0"), false,
                                   L"Choose the shared quickprompts-team.json");
            if (files.empty()) return;
            next = files[0];
        } else if (pick == 2) {
            next.clear();
        } else {
            return;
        }
        if (host.setTeamFile) host.setTeamFile(next);
        if (host.changed) host.changed();
        Rebuild();
        const std::wstring e = QuickPromptStore::Shared().TeamError();
        statusError = !e.empty();
        status = !e.empty() ? e : next.empty() ? L"Team file removed" : L"Team prompts from " + next;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void InsertChip(int i) {
        if (i < 0 || i >= (int)std::size(kChips)) return;
        SetFocus(text);
        SendMessageW(text, EM_REPLACESEL, TRUE, (LPARAM)kChips[i]);
    }

    void OnTry() {
        if (tryGuard) return;
        const std::wstring t = GetText(tryBox);
        ExpandContext ctx;
        if (OpenClipboard(hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    ctx.clipboard = p;
                    GlobalUnlock(h);
                }
            CloseClipboard();
        }
        ctx.lastCapture = host.lastCapture ? host.lastCapture() : L"";
        const time_t now = time(nullptr);
        localtime_s(&ctx.now, &now);
        std::wstring out;
        size_t caret = 0;
        if (!TryExpand(t, QuickPromptStore::Shared().Active(), ctx, &out, &caret)) return;
        std::wstring flat;  // a single-line box: show line breaks as ⏎
        size_t flatCaret = 0;
        for (size_t i = 0; i < out.size(); ++i) {
            if (i == caret) flatCaret = flat.size();
            flat += out[i] == L'\n' ? std::wstring(L" ⏎ ") : std::wstring(1, out[i]);
        }
        if (caret >= out.size()) flatCaret = flat.size();
        tryGuard = true;
        SetWindowTextW(tryBox, flat.c_str());
        SendMessageW(tryBox, EM_SETSEL, flatCaret, flatCaret);
        tryGuard = false;
    }

    void OnCommand(int id, int code, HWND) {
        if (loading) return;
        switch (id) {
            case IdSearch:
                if (code == EN_CHANGE) SetTimer(hwnd, kFilterTimer, 120, nullptr);
                if (code == EN_KILLFOCUS || code == EN_SETFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
                return;
            case IdList:
                if (code == LBN_SELCHANGE) {
                    SaveNow();
                    const int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
                    if (i >= 0 && i < (int)rows.size()) {
                        if (rows[i].header) Select(currentId);  // headers aren't selectable
                        else Select(rows[i].p.id);
                    }
                }
                return;
            case IdTrigger: case IdTitle: case IdText: case IdHotkey: case IdTags:
                if (code == EN_CHANGE) SetTimer(hwnd, kSaveTimer, 450, nullptr);
                if (code == EN_KILLFOCUS || code == EN_SETFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
                if (code == EN_KILLFOCUS) SaveNow();
                return;
            case IdTry:
                if (code == EN_CHANGE) OnTry();
                if (code == EN_KILLFOCUS || code == EN_SETFOCUS) InvalidateRect(hwnd, nullptr, FALSE);
                return;
            case IdNew: SaveNow(); New(); return;
            case IdDuplicate: SaveNow(); Duplicate(); return;
            case IdDelete: Delete(); return;
            case IdImport: SaveNow(); Import(); return;
            case IdExport: SaveNow(); Export(); return;
            case IdTeam: SaveNow(); Team(); return;
        }
        if (id >= IdChipBase && id < IdChipBase + (int)std::size(kChips)) InsertChip(id - IdChipBase);
    }

    void OnClick(POINT p) {
        if (PtInRect(&switchRect, p) && host.setTyping) {
            host.setTyping(!(host.typing && host.typing()));
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
        QuickPrompt q;
        if (!currentId.empty() && PtInRect(&enabledRect, p) && QuickPromptStore::Shared().Find(currentId, &q)) {
            SaveNow();
            QuickPromptStore::Shared().SetEnabled(q.id, !q.enabled);
            if (host.changed) host.changed();
            Rebuild();
            Select(q.id);
        }
    }

    // Another window or the team file changed the store: show it (without losing what's being typed).
    void Refresh() {
        if (QuickPromptStore::Shared().Version() == storeVersion) return;
        const HWND f = GetFocus();
        if (f == trigger || f == title || f == text || f == hotkey || f == tags) return;
        Rebuild();
        QuickPrompt p;
        if (!QuickPromptStore::Shared().Find(currentId, &p)) Select(L"");
        else InvalidateRect(hwnd, nullptr, FALSE);
    }
};

Window* g_win = nullptr;

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* W = g_win && g_win->hwnd == h ? g_win : nullptr;
    if (!W) {
        if (m == WM_MEASUREITEM && g_win) {  // arrives during the list's creation
            g_win->MeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(l));
            return TRUE;
        }
        return DefWindowProcW(h, m, w, l);
    }
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            const RECT rc = W->Client();
            BitmapPtr back = Bitmap::Create(std::max(1, (int)rc.right), std::max(1, (int)rc.bottom));
            if (back) {
                MemDC mdc(back->Handle());
                W->Paint(mdc);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_PRINTCLIENT: W->Paint((HDC)w); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: W->Layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = {W->S(860), W->S(600)};
            return 0;
        }
        case WM_DPICHANGED: {
            W->s = HIWORD(w) / 96.0f;
            W->Fonts();
            for (HWND e : {W->search, W->title, W->hotkey, W->tags, W->tryBox}) SendMessageW(e, WM_SETFONT, (WPARAM)W->fBody, TRUE);
            for (HWND e : {W->trigger, W->text}) SendMessageW(e, WM_SETFONT, (WPARAM)W->fMono, TRUE);
            for (HWND b : W->buttons) SendMessageW(b, WM_SETFONT, (WPARAM)W->fSmall, TRUE);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, RectW(*r), RectH(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            W->Rebuild();
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)w;
            const bool ro = (GetWindowLongW((HWND)l, GWL_STYLE) & ES_READONLY) != 0;
            SetTextColor(dc, ro ? theme::kTextDim : theme::kText);
            SetBkColor(dc, theme::kBgRaised);
            return (LRESULT)W->raisedBrush;
        }
        case WM_CTLCOLORLISTBOX:
            SetBkColor((HDC)w, theme::kSurface);
            return (LRESULT)W->surfaceBrush;
        case WM_MEASUREITEM: W->MeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(l)); return TRUE;
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (d->CtlType == ODT_LISTBOX) W->DrawItem(d);
            else W->DrawButton(d);
            return TRUE;
        }
        case WM_COMMAND: W->OnCommand(LOWORD(w), HIWORD(w), (HWND)l); return 0;
        case WM_LBUTTONDOWN: W->OnClick({GET_X_LPARAM(l), GET_Y_LPARAM(l)}); return 0;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            if ((HWND)w == h && (PtInRect(&W->switchRect, p) || PtInRect(&W->enabledRect, p))) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_TIMER:
            if (w == kSaveTimer) W->SaveNow();
            else if (w == kFilterTimer) {
                KillTimer(h, kFilterTimer);
                W->Rebuild();
            } else if (w == 3) {
                W->Refresh();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) W->Refresh();
            break;
        case WM_CLOSE: W->SaveNow(); DestroyWindow(h); return 0;
        case WM_DESTROY:
            g_win = nullptr;
            delete W;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterClassOnce(HICON icon) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

void SelectInitial(Window* W, const std::wstring& selectId) {
    W->Rebuild();
    if (selectId == L"new") {
        W->New();
        return;
    }
    std::wstring id = selectId;
    if (id.empty())
        for (const auto& r : W->rows)
            if (!r.header) {
                id = r.p.id;
                break;
            }
    W->Select(id);
}

// ---- welcome card ----

struct Welcome {
    HWND hwnd = nullptr;
    float s = 1;
    std::wstring picker;
    std::function<void(bool)> done;
    RECT openBtn{}, gotBtn{};
    int S(int v) const { return Px(s, v); }

    void Paint(HDC dc, const RECT& rc) {
        FillSolid(dc, rc, theme::kSurface);
        FrameSolid(dc, rc, theme::kBorder);
        HFONT eyebrow = MakeEyebrowFont(S(12)), title = MakeDisplayFont(S(40)), body = MakeFont(S(15)), fineFont = MakeFont(S(13)),
              mono = MakeFont(S(15), FW_SEMIBOLD, L"Cascadia Mono"), btn = MakeFont(S(14), FW_SEMIBOLD);
        const int L = S(32), R = rc.right - S(32);
        int y = S(28);
        HGDIOBJ old = SelectObject(dc, eyebrow);
        SetTextColor(dc, theme::kAccent);
        SetBkMode(dc, TRANSPARENT);
        DrawSpacedText(dc, L"NEW IN ATHER SCREENSHOT", {L, y, R, y + S(18)}, DT_LEFT | DT_SINGLELINE, 2);
        SelectObject(dc, old);
        y += S(22);
        DrawTextIn(dc, L"QUICKPROMPTS", {L, y, R, y + S(48)}, title, theme::kText);
        y += S(54);
        RECT b{L, y, R, y + S(48)};
        DrawTextIn(dc, L"Type a short trigger and press Space, in any app, and it turns into a saved prompt or phrase.", b, body,
                   theme::kTextDim, DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += S(56);
        struct Ex {
            const wchar_t *t, *d;
        } ex[] = {{L"doitforme", L"Asks an AI agent to do the task end to end and report back"},
                  {L"reviewpls", L"Asks for a review of the current change"},
                  {L"bugpls", L"Turns your last screenshot into a bug report, picture attached"}};
        for (const auto& e : ex) {
            RECT row{L, y, R, y + S(34)};
            FillRounded(dc, row, S(8), theme::kBgRaised, theme::kBorder);
            DrawTextIn(dc, e.t, {L + S(14), y, L + S(150), y + S(34)}, mono, theme::kAccent);
            DrawTextIn(dc, e.d, {L + S(150), y, R - S(12), y + S(34)}, fineFont, theme::kTextDim);
            y += S(40);
        }
        y += S(8);
        RECT pv{L, y, R, y + S(64)};
        DrawTextIn(dc,
                   L"To spot triggers, Ather Screenshot now listens to what you type. It keeps only the last few characters, in "
                   L"memory: never saved, never logged, never shared with AI agents. It's on now; turn it off any time in "
                   L"QuickPrompts or Settings.",
                   pv, fineFont, theme::kMuted, DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += S(70);
        if (!picker.empty()) {
            DrawTextIn(dc, picker + L" lists every prompt, so you don't have to remember the triggers.", {L, y, R, y + S(20)}, fineFont,
                       theme::kMuted);
        }
        const int by = rc.bottom - S(28) - S(40);
        gotBtn = {R - S(140), by, R, by + S(40)};
        openBtn = {gotBtn.left - S(12) - S(190), by, gotBtn.left - S(12), by + S(40)};
        FillRounded(dc, gotBtn, S(8), theme::kAccent, theme::kAccent);
        DrawTextIn(dc, L"Got it", gotBtn, btn, theme::kOnAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FillRounded(dc, openBtn, S(8), theme::kBgRaised, theme::kBorder);
        DrawTextIn(dc, L"Open QuickPrompts", openBtn, btn, theme::kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        for (HFONT f : {eyebrow, title, body, fineFont, mono, btn}) DeleteObject(f);
    }
};

Welcome* g_welcome = nullptr;

LRESULT CALLBACK WelcomeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Welcome* W = g_welcome && g_welcome->hwnd == h ? g_welcome : nullptr;
    if (!W) return DefWindowProcW(h, m, w, l);
    auto finish = [&](bool open) {
        auto done = W->done;
        DestroyWindow(h);
        if (done) done(open);
    };
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            BitmapPtr back = Bitmap::Create(rc.right, rc.bottom);
            if (back) {
                MemDC mdc(back->Handle());
                W->Paint(mdc, rc);
                BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_LBUTTONUP: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&W->gotBtn, p)) finish(false);
            else if (PtInRect(&W->openBtn, p)) finish(true);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            if (PtInRect(&W->gotBtn, p) || PtInRect(&W->openBtn, p)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        case WM_KEYDOWN:
            if (w == VK_RETURN || w == VK_ESCAPE) finish(false);
            return 0;
        case WM_CLOSE: finish(false); return 0;
        case WM_DESTROY:
            g_welcome = nullptr;
            delete W;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

constexpr int kWelcomeW = 600, kWelcomeH = 480;

}  // namespace

bool TryExpand(const std::wstring& text, const std::vector<QuickPrompt>& active, const ExpandContext& ctx, std::wstring* out, size_t* caret) {
    if (text.empty() || text.back() != L' ') return false;
    TriggerMatcher m;
    std::vector<TriggerMatcher::Entry> e;
    for (int i = 0; i < (int)active.size(); ++i) e.push_back({active[i].trigger, active[i].caseSensitive, i});
    m.SetTriggers(e);
    for (size_t i = 0; i + 1 < text.size(); ++i) {
        if (text[i] == L' ') m.Space(nullptr);
        else m.Char(text[i]);
    }
    int erase = 0;
    const int tag = m.Space(&erase);
    if (tag < 0) return false;
    Expansion x = ExpandPlaceholders(active[tag].text, ctx);
    std::wstring body = x.text + (x.pasteImage ? L" [last screenshot]" : L"");
    const std::wstring before = text.substr(0, text.size() - erase);
    *out = before + body;
    *caret = before.size() + x.text.size() - x.cursorBack;
    return true;
}

bool QuickPromptsWindowOpen() { return g_win != nullptr; }

void ShowQuickPromptsWindow(const QuickPromptsUiHost& host, const std::wstring& selectId) {
    if (g_win) {
        g_win->host = host;
        g_win->Refresh();
        if (!selectId.empty()) SelectInitial(g_win, selectId);
        if (IsIconic(g_win->hwnd)) ShowWindow(g_win->hwnd, SW_RESTORE);
        ForceForeground(g_win->hwnd);
        return;
    }
    RegisterClassOnce(host.icon);
    auto* W = new Window();
    W->host = host;
    POINT pt;
    GetCursorPos(&pt);
    W->s = DpiScaleAt(pt);
    W->Fonts();
    g_win = W;
    const RECT work = MonitorRectAt(pt, true);
    const int w = std::min(W->S(kW), (int)(RectW(work) * 0.95)), hh = std::min(W->S(kH), (int)(RectH(work) * 0.95));
    W->hwnd = CreateWindowExW(0, kClass, L"QuickPrompts — Ather Screenshot", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              work.left + (RectW(work) - w) / 2, work.top + (RectH(work) - hh) / 2, w, hh, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!W->hwnd) {
        g_win = nullptr;
        delete W;
        return;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(W->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF cap = theme::kSurface;
    DwmSetWindowAttribute(W->hwnd, DWMWA_CAPTION_COLOR, &cap, sizeof(cap));
    W->Create();
    W->Layout();
    SelectInitial(W, selectId);
    SetTimer(W->hwnd, 3, 2000, nullptr);  // pick up the team file or a hand-edited file
    ShowWithoutFlash(W->hwnd, true);
}

void ShowQuickPromptsWelcome(HICON icon, const std::wstring& pickerHotkey, std::function<void(bool)> done) {
    if (g_welcome) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = WelcomeProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = icon;
        wc.lpszClassName = kWelcomeClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* W = new Welcome();
    POINT pt;
    GetCursorPos(&pt);
    W->s = DpiScaleAt(pt);
    W->picker = pickerHotkey;
    W->done = std::move(done);
    g_welcome = W;
    const RECT work = MonitorRectAt(pt, true);
    const int w = W->S(kWelcomeW), h = W->S(kWelcomeH);
    W->hwnd = CreateWindowExW(WS_EX_TOPMOST, kWelcomeClass, L"QuickPrompts", WS_POPUP, work.left + (RectW(work) - w) / 2,
                              work.top + (RectH(work) - h) / 2, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!W->hwnd) {
        g_welcome = nullptr;
        delete W;
        return;
    }
    PrepareChromeless(W->hwnd, true);
    ShowWithoutFlash(W->hwnd, true);
}

BitmapPtr QuickPromptsSnapshot(const QuickPromptsUiHost& host, const std::wstring& select, std::vector<std::wstring>* rows) {
    if (g_win) return nullptr;
    RegisterClassOnce(host.icon);
    auto* W = new Window();
    W->host = host;
    W->snapshot = true;
    W->Fonts();
    g_win = W;
    W->hwnd = CreateWindowExW(0, kClass, L"", WS_POPUP | WS_CLIPCHILDREN, 0, 0, W->S(kW), W->S(kH - 32), nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!W->hwnd) {
        g_win = nullptr;
        delete W;
        return nullptr;
    }
    W->Create();
    W->Layout();
    SelectInitial(W, select);
    const RECT rc = W->Client();
    BitmapPtr out = Bitmap::Create(rc.right, rc.bottom);
    if (out) {
        MemDC dc(out->Handle());
        W->Paint(dc);
        // The child controls, each into its place.
        for (HWND c = GetWindow(W->hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (!(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(nullptr, W->hwnd, (POINT*)&r, 2);
            SaveDC(dc);
            SetViewportOrgEx(dc, r.left, r.top, nullptr);
            IntersectClipRect(dc, 0, 0, RectW(r), RectH(r));
            SendMessageW(c, WM_PRINT, (WPARAM)(HDC)dc, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
            RestoreDC(dc, -1);
        }
        for (size_t i = 0, n = (size_t)out->Width() * out->Height(); i < n; ++i) out->Bits()[i] |= 0xFF000000u;
    }
    if (rows) {
        RECT lr;
        GetClientRect(W->list, &lr);
        const int top = (int)SendMessageW(W->list, LB_GETTOPINDEX, 0, 0);
        for (int i = top, y = 0; i < (int)W->rows.size() && y < lr.bottom; ++i) {
            rows->push_back(W->rows[i].header ? L"# " + W->rows[i].label : W->rows[i].p.trigger);
            y += (int)SendMessageW(W->list, LB_GETITEMHEIGHT, i, 0);
        }
    }
    DestroyWindow(W->hwnd);
    return out;
}

BitmapPtr QuickPromptsWelcomeSnapshot(const std::wstring& pickerHotkey) {
    Welcome w;
    w.picker = pickerHotkey;
    RECT rc{0, 0, w.S(kWelcomeW), w.S(kWelcomeH)};
    BitmapPtr out = Bitmap::Create(rc.right, rc.bottom);
    if (!out) return nullptr;
    {
        MemDC dc(out->Handle());
        w.Paint(dc, rc);
    }
    for (size_t i = 0, n = (size_t)out->Width() * out->Height(); i < n; ++i) out->Bits()[i] |= 0xFF000000u;
    return out;
}

// ---- tests ----

ATHER_TEST(quickprompts_try_it_box) {
    std::vector<QuickPrompt> active(2);
    active[0].trigger = L"sig";
    active[0].text = L"Best,\nTin{cursor}";
    active[1].trigger = L"bugpls";
    active[1].text = L"Bug:{lastcapture:image}";
    ExpandContext ctx;
    std::wstring out;
    size_t caret = 0;
    CHECK(TryExpand(L"hello sig ", active, ctx, &out, &caret));
    CHECK(out == L"hello Best,\nTin");
    CHECK_EQ(caret, out.size());
    CHECK(!TryExpand(L"hello sig", active, ctx, &out, &caret));
    CHECK(!TryExpand(L"design ", active, ctx, &out, &caret));
    CHECK(TryExpand(L"bugpls ", active, ctx, &out, &caret));
    CHECK(out == L"Bug: [last screenshot]");
}

}  // namespace ather
