#include "quickprompts.h"

#include <bcrypt.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#include "json.h"
#include "selftest.h"

namespace ather {
namespace {

std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool IsWordChar(wchar_t c) { return iswalnum(c) || c == L'_'; }

bool EndsWith(const std::wstring& s, const std::wstring& tail, bool caseSensitive) {
    if (tail.size() > s.size()) return false;
    const size_t off = s.size() - tail.size();
    for (size_t i = 0; i < tail.size(); ++i) {
        wchar_t a = s[off + i], b = tail[i];
        if (!caseSensitive) a = (wchar_t)towlower(a), b = (wchar_t)towlower(b);
        if (a != b) return false;
    }
    return true;
}

FILETIME StampOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return {};
    return d.ftLastWriteTime;
}

bool SameStamp(const FILETIME& a, const FILETIME& b) { return CompareFileTime(&a, &b) == 0; }

bool ReadFileBytes(const std::wstring& path, std::string* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 16 * 1024 * 1024) {  // a prompts file is never this big
        CloseHandle(f);
        return false;
    }
    out->resize((size_t)size.QuadPart);
    DWORD rd = 0;
    const bool ok = out->empty() || (ReadFile(f, out->data(), (DWORD)out->size(), &rd, nullptr) && rd == out->size());
    CloseHandle(f);
    if (out->size() >= 3 && (unsigned char)(*out)[0] == 0xEF && (unsigned char)(*out)[1] == 0xBB && (unsigned char)(*out)[2] == 0xBF)
        out->erase(0, 3);
    return ok;
}

// Text files from other apps: UTF-8 (with or without BOM) or UTF-16 LE with BOM.
std::wstring ReadTextFile(const std::wstring& path, bool* ok) {
    std::string bytes;
    *ok = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(f);
        return L"";
    }
    bytes.resize((size_t)size.QuadPart);
    DWORD rd = 0;
    if (!bytes.empty()) ReadFile(f, bytes.data(), (DWORD)bytes.size(), &rd, nullptr);
    CloseHandle(f);
    *ok = true;
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE)
        return std::wstring(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
        bytes.erase(0, 3);
    return FromUtf8(bytes);
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = data.empty() || (WriteFile(f, data.data(), (DWORD)data.size(), &wr, nullptr) && wr == data.size());
    FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::wstring NowIso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t b[32];
    swprintf_s(b, L"%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

Json PromptToJson(const QuickPrompt& p, bool full) {
    Json o = Json::Object();
    o.Set("id", Json(p.id));
    o.Set("trigger", Json(p.trigger));
    o.Set("title", Json(p.title));
    o.Set("text", Json(p.text));
    if (full) {
        o.Set("enabled", p.enabled);
        o.Set("caseSensitive", p.caseSensitive);
        o.Set("hotkey", Json(p.hotkey));
    }
    Json tags = Json::Array();
    for (const auto& t : p.tags) tags.Push(Json(t));
    o.Set("tags", tags);
    if (full) {
        o.Set("uses", p.uses);
        o.Set("lastUsed", Json(p.lastUsed));
    }
    return o;
}

std::wstring CsvField(const std::wstring& s) {
    if (s.find_first_of(L",\"\r\n") == std::wstring::npos) return s;
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') o += L'"';
        o += c;
    }
    return o + L"\"";
}

std::wstring XmlUnescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != L'&') {
            o += s[i];
            continue;
        }
        const size_t semi = s.find(L';', i);
        if (semi == std::wstring::npos || semi - i > 10) {
            o += s[i];
            continue;
        }
        const std::wstring ent = s.substr(i + 1, semi - i - 1);
        if (ent == L"amp") o += L'&';
        else if (ent == L"lt") o += L'<';
        else if (ent == L"gt") o += L'>';
        else if (ent == L"quot") o += L'"';
        else if (ent == L"apos") o += L'\'';
        else if (!ent.empty() && ent[0] == L'#') {
            const unsigned long cp = ent.size() > 1 && (ent[1] == L'x' || ent[1] == L'X') ? wcstoul(ent.c_str() + 2, nullptr, 16)
                                                                                            : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (cp >= 0x10000 && cp <= 0x10FFFF) {
                o += (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
                o += (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
            } else if (cp) {
                o += (wchar_t)cp;
            }
        } else {
            o += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return o;
}

std::wstring Pad2(int v) {
    wchar_t b[8];
    swprintf_s(b, L"%02d", v);
    return b;
}

std::wstring FormatDate(const std::wstring& fmt, const std::tm& t) {
    std::wstring o;
    for (size_t i = 0; i < fmt.size();) {
        auto at = [&](const wchar_t* tok) { return fmt.compare(i, wcslen(tok), tok) == 0; };
        if (at(L"yyyy")) o += std::to_wstring(t.tm_year + 1900), i += 4;
        else if (at(L"MM")) o += Pad2(t.tm_mon + 1), i += 2;
        else if (at(L"dd")) o += Pad2(t.tm_mday), i += 2;
        else if (at(L"HH")) o += Pad2(t.tm_hour), i += 2;
        else if (at(L"mm")) o += Pad2(t.tm_min), i += 2;
        else if (at(L"ss")) o += Pad2(t.tm_sec), i += 2;
        else o += fmt[i++];
    }
    return o;
}

bool KnownPlaceholder(const std::wstring& name) {
    static const wchar_t* const kNames[] = {L"cursor", L"clipboard", L"date", L"time", L"datetime", L"lastcapture",
                                            L"lastcapture:image"};
    const std::wstring n = LowerW(name);
    for (auto k : kNames)
        if (n == k) return true;
    return n.rfind(L"date:", 0) == 0;
}

}  // namespace

std::wstring CheckTrigger(const std::wstring& trigger) {
    if (trigger.size() < 2) return L"A trigger needs at least 2 characters.";
    if (trigger.size() > 32) return L"A trigger can have at most 32 characters.";
    for (wchar_t c : trigger)
        if (iswspace(c) || iswcntrl(c)) return L"A trigger can't contain spaces.";
    return L"";
}

const std::vector<QuickPrompt>& BuiltinQuickPrompts() {
    static const std::vector<QuickPrompt> kList = [] {
        struct B {
            const wchar_t *id, *trigger, *title, *text, *tags;
        };
        static const B kB[] = {
            {L"builtin.doitforme", L"doitforme", L"Do it for me",
             L"Please do this end to end:\n"
             L"1. Read the relevant code and docs first, so the change fits how this project already works.\n"
             L"2. Make the change completely, including the tests it needs.\n"
             L"3. Build and run the tests.\n"
             L"4. Report what you changed, what you verified and how, and anything you couldn't verify.\n\n"
             L"Task: {cursor}",
             L"agent"},
            {L"builtin.planpls", L"planpls", L"Plan first",
             L"Before writing any code, propose a short plan: the files you'll touch, the approach, the risks and how "
             L"you'll test it. Then wait for my go-ahead.\n\nTask: {cursor}",
             L"agent"},
            {L"builtin.reviewpls", L"reviewpls", L"Review this change",
             L"Review the current changes for bugs, edge cases, missing tests and anything that breaks existing "
             L"behaviour. List each finding with file:line, why it matters and a suggested fix, most severe first. "
             L"Skip style nits.",
             L"agent review"},
            {L"builtin.explainpls", L"explainpls", L"Explain this",
             L"Explain what this does, step by step, for someone new to this codebase: the purpose, the main flow, and "
             L"anything surprising.\n\n{cursor}",
             L"learn"},
            {L"builtin.bugpls", L"bugpls", L"Describe this bug",
             L"Turn the attached screenshot into a bug report with: a one-line title, steps to reproduce, expected "
             L"result, actual result, and severity (blocker / major / minor). Ask me for anything you can't tell "
             L"from the picture.\n\n{cursor}{lastcapture:image}",
             L"bug screenshot"},
            {L"builtin.fixthispls", L"fixthispls", L"Fix from screenshot",
             L"The attached screenshot shows a problem. Find its cause in the code, fix it, and tell me how you "
             L"verified the fix.\n\n{cursor}{lastcapture:image}",
             L"agent bug screenshot"},
            {L"builtin.testpls", L"testpls", L"Write tests",
             L"Write tests for this change that cover the normal path, the edge cases and the failure cases. Run them "
             L"and report the results.\n\n{cursor}",
             L"agent test"},
            {L"builtin.shorterpls", L"shorterpls", L"Make it shorter",
             L"Rewrite the text below at about half the length. Keep every fact, number and name; drop filler.\n\n"
             L"{clipboard}",
             L"writing"},
            {L"builtin.vienpls", L"vienpls", L"Translate VI ↔ EN",
             L"Translate the text below between Vietnamese and English (whichever it isn't in). Keep technical terms, "
             L"names and the tone.\n\n{clipboard}",
             L"writing vietnamese"},
            {L"builtin.commitpls", L"commitpls", L"Commit message",
             L"Write a commit message for the staged changes: a summary line under 72 characters, a blank line, then "
             L"what changed and why in a few short lines.",
             L"git"},
            {L"builtin.playtestpls", L"playtestpls", L"Playtest notes",
             L"Summarise these playtest notes into a list of issues. For each: a title, steps to reproduce, how often "
             L"it happened, severity, and which team should own it.\n\n{clipboard}",
             L"playtest qa"},
            {L"builtin.standupme", L"standupme", L"Stand-up",
             L"Draft my stand-up update from these notes, in three short parts: Yesterday, Today, Blockers.\n\n"
             L"{clipboard}",
             L"team"},
        };
        std::vector<QuickPrompt> v;
        for (const auto& b : kB) {
            QuickPrompt p;
            p.id = b.id;
            p.trigger = b.trigger;
            p.title = b.title;
            p.text = b.text;
            p.source = L"builtin";
            std::wstringstream ss(b.tags);
            for (std::wstring t; ss >> t;) p.tags.push_back(t);
            v.push_back(std::move(p));
        }
        return v;
    }();
    return kList;
}

// ---- TriggerMatcher ----

void TriggerMatcher::SetTriggers(std::vector<Entry> entries) {
    // Longest first, so "quickfix" beats "fix".
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.trigger.size() > b.trigger.size(); });
    entries_ = std::move(entries);
}

void TriggerMatcher::Char(wchar_t c) {
    buf_ += c;
    if (buf_.size() > kMaxBuffer) buf_.erase(0, buf_.size() - kMaxBuffer);
}

void TriggerMatcher::Backspace() {
    if (!buf_.empty()) buf_.pop_back();
}

void TriggerMatcher::Reset() { buf_.clear(); }

int TriggerMatcher::Space(int* erase) {
    for (const auto& e : entries_) {
        if (e.trigger.empty() || !EndsWith(buf_, e.trigger, e.caseSensitive)) continue;
        const size_t start = buf_.size() - e.trigger.size();
        // A whole word only: "redoitforme" must not fire "doitforme". Triggers that start with punctuation
        // (";dfm") still need a boundary before them, which a space or the start of typing gives.
        if (start > 0 && (IsWordChar(buf_[start - 1]) || (!IsWordChar(e.trigger[0]) && !iswspace(buf_[start - 1])))) continue;
        if (erase) *erase = (int)e.trigger.size() + 1;
        buf_.clear();
        return e.tag;
    }
    Char(L' ');
    return -1;
}

std::vector<std::pair<int, int>> FeedMatcher(TriggerMatcher& m, const std::wstring& typed) {
    std::vector<std::pair<int, int>> hits;
    for (wchar_t c : typed) {
        if (c == L' ') {
            int erase = 0;
            int tag = m.Space(&erase);
            if (tag >= 0) hits.push_back({tag, erase});
        } else if (c == L'\b') {
            m.Backspace();
        } else if (c == L'\n') {
            m.Reset();
        } else {
            m.Char(c);
        }
    }
    return hits;
}

// ---- placeholders ----

Expansion ExpandPlaceholders(const std::wstring& text, const ExpandContext& ctx) {
    Expansion e;
    size_t cursorAt = std::wstring::npos;
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'{' && i + 1 < text.size() && text[i + 1] == L'{') {
            out += L'{';
            ++i;
            continue;
        }
        if (c == L'}' && i + 1 < text.size() && text[i + 1] == L'}') {
            out += L'}';
            ++i;
            continue;
        }
        if (c == L'\r') continue;  // \r\n -> \n
        if (c != L'{') {
            out += c;
            continue;
        }
        const size_t close = text.find(L'}', i + 1);
        if (close == std::wstring::npos || close - i > 64) {
            out += c;
            continue;
        }
        const std::wstring name = text.substr(i + 1, close - i - 1), lower = LowerW(name);
        if (lower == L"cursor") {
            if (cursorAt == std::wstring::npos) cursorAt = out.size();
        } else if (lower == L"clipboard") {
            for (wchar_t k : ctx.clipboard)
                if (k != L'\r') out += k;
        } else if (lower == L"date") {
            out += FormatDate(L"yyyy-MM-dd", ctx.now);
        } else if (lower == L"time") {
            out += FormatDate(L"HH:mm", ctx.now);
        } else if (lower == L"datetime") {
            out += FormatDate(L"yyyy-MM-dd HH:mm", ctx.now);
        } else if (lower.rfind(L"date:", 0) == 0) {
            out += FormatDate(name.substr(5), ctx.now);
        } else if (lower == L"lastcapture") {
            out += ctx.lastCapture;
        } else if (lower == L"lastcapture:image") {
            e.pasteImage = true;
        } else {
            out += text.substr(i, close - i + 1);  // not ours: leave it as typed
        }
        i = close;
    }
    if (cursorAt != std::wstring::npos) e.cursorBack = (int)(out.size() - cursorAt);
    e.text = std::move(out);
    return e;
}

std::vector<std::wstring> UnknownPlaceholders(const std::wstring& text) {
    std::vector<std::wstring> v;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'{') continue;
        if (i + 1 < text.size() && text[i + 1] == L'{') {
            ++i;
            continue;
        }
        const size_t close = text.find(L'}', i + 1);
        if (close == std::wstring::npos || close - i > 64) continue;
        const std::wstring name = text.substr(i + 1, close - i - 1);
        if (!name.empty() && !KnownPlaceholder(name) && std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
        i = close;
    }
    return v;
}

// ---- store ----

QuickPromptStore& QuickPromptStore::Shared() {
    static QuickPromptStore s;
    return s;
}

std::wstring QuickPromptStore::MakeId() {
    BYTE b[8];
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    wchar_t s[24];
    swprintf_s(s, L"p%02x%02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    return s;
}

std::vector<QuickPrompt> QuickPromptStore::ParsePrompts(const std::string& utf8, bool* ok) {
    std::vector<QuickPrompt> v;
    bool parsed = false;
    Json j = Json::Parse(utf8, &parsed);
    if (ok) *ok = parsed && j.IsObject();
    if (!parsed || !j.IsObject()) return v;
    const Json& arr = j["prompts"];
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& o = arr[i];
        if (!o.IsObject()) continue;
        QuickPrompt p;
        p.id = o["id"].WStr();
        p.trigger = o["trigger"].WStr();
        p.title = o["title"].WStr();
        p.text = o["text"].WStr();
        p.enabled = o["enabled"].Bool(true);
        p.caseSensitive = o["caseSensitive"].Bool(false);
        p.hotkey = o["hotkey"].WStr();
        for (size_t k = 0; k < o["tags"].size(); ++k) p.tags.push_back(o["tags"][k].WStr());
        p.uses = o["uses"].Int(0);
        p.lastUsed = o["lastUsed"].WStr();
        if (p.text.empty() && p.trigger.empty()) continue;
        v.push_back(std::move(p));
    }
    return v;
}

std::vector<QuickPrompt> QuickPromptStore::ParseCsv(const std::wstring& text) {
    // RFC 4180-ish: trigger,text[,title]. A first row "trigger,text..." is a header.
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (quoted) {
            if (c == L'"' && i + 1 < text.size() && text[i + 1] == L'"') field += L'"', ++i;
            else if (c == L'"') quoted = false;
            else field += c;
            continue;
        }
        if (c == L'"') {
            quoted = true;
            any = true;
        } else if (c == L',') {
            row.push_back(field), field.clear(), any = true;
        } else if (c == L'\n' || c == L'\r') {
            if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            if (any || !field.empty()) row.push_back(field);
            if (!row.empty()) rows.push_back(row);
            row.clear(), field.clear(), any = false;
        } else {
            field += c;
        }
    }
    if (any || !field.empty()) row.push_back(field);
    if (!row.empty()) rows.push_back(row);
    std::vector<QuickPrompt> v;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].size() < 2) continue;
        if (r == 0 && LowerW(rows[r][0]) == L"trigger") continue;
        QuickPrompt p;
        p.trigger = rows[r][0];
        p.text = rows[r][1];
        if (rows[r].size() > 2) p.title = rows[r][2];
        v.push_back(std::move(p));
    }
    return v;
}

std::vector<QuickPrompt> QuickPromptStore::ParseApplePlist(const std::wstring& text) {
    // macOS writes Text Replacements.plist as an <array> of <dict> with "phrase" and "shortcut" keys.
    std::vector<QuickPrompt> v;
    for (size_t pos = 0;;) {
        const size_t d = text.find(L"<dict>", pos);
        if (d == std::wstring::npos) break;
        const size_t e = text.find(L"</dict>", d);
        if (e == std::wstring::npos) break;
        const std::wstring dict = text.substr(d, e - d);
        std::wstring phrase, shortcut;
        for (size_t k = 0;;) {
            const size_t ks = dict.find(L"<key>", k);
            if (ks == std::wstring::npos) break;
            const size_t ke = dict.find(L"</key>", ks);
            if (ke == std::wstring::npos) break;
            const std::wstring key = dict.substr(ks + 5, ke - ks - 5);
            const size_t ss = dict.find(L"<string>", ke), se = dict.find(L"</string>", ke);
            const size_t nextKey = dict.find(L"<key>", ke);
            std::wstring val;
            if (ss != std::wstring::npos && se != std::wstring::npos && ss < se && (nextKey == std::wstring::npos || ss < nextKey))
                val = XmlUnescape(dict.substr(ss + 8, se - ss - 8));
            if (key == L"phrase") phrase = val;
            else if (key == L"shortcut") shortcut = val;
            k = ke + 6;
        }
        if (!shortcut.empty() && !phrase.empty()) {
            QuickPrompt p;
            p.trigger = shortcut;
            p.text = phrase;
            v.push_back(std::move(p));
        }
        pos = e + 7;
    }
    return v;
}

void QuickPromptStore::Load(const std::wstring& folder, const std::wstring& teamFile, std::wstring* error) {
    std::lock_guard lock(mu_);
    folder_ = folder;
    teamFile_ = teamFile;
    user_.clear();
    state_.clear();
    userBad_ = false;
    const std::wstring path = UserPath();
    userStamp_ = StampOf(path);
    std::string bytes;
    if (ReadFileBytes(path, &bytes)) {
        bool ok = false;
        user_ = ParsePrompts(bytes, &ok);
        if (!ok) {
            userBad_ = true;  // keep it as it is; a copy shows what went wrong
            CopyFileW(path.c_str(), (path + L".bad").c_str(), FALSE);
            if (error) *error = L"quickprompts.json couldn't be read, so it was left as it is (a copy is in quickprompts.json.bad).";
        } else {
            Json j = Json::Parse(bytes);
            for (const auto& [id, st] : j["state"].Members()) {
                State s;
                s.enabled = st["enabled"].Bool(true);
                s.uses = st["uses"].Int(0);
                s.lastUsed = st["lastUsed"].WStr();
                state_[FromUtf8(id)] = s;
            }
        }
        for (auto& p : user_) {
            p.source = L"user";
            if (p.id.empty()) p.id = MakeId();
        }
    }
    LoadTeam();
    ++version_;
}

void QuickPromptStore::LoadTeam() {
    team_.clear();
    teamError_.clear();
    teamStamp_ = StampOf(teamFile_);
    if (teamFile_.empty()) return;
    std::string bytes;
    if (!ReadFileBytes(teamFile_, &bytes)) {
        teamError_ = L"The team prompts file can't be opened: " + teamFile_;
        return;
    }
    bool ok = false;
    team_ = ParsePrompts(bytes, &ok);
    if (!ok) {
        teamError_ = L"The team prompts file isn't valid JSON: " + teamFile_;
        return;
    }
    int n = 0;
    for (auto& p : team_) {
        p.source = L"team";
        p.hotkey.clear();
        p.enabled = true;
        if (p.id.empty()) p.id = L"team." + LowerW(p.trigger.empty() ? std::to_wstring(n) : p.trigger);
        else if (p.id.rfind(L"team.", 0) != 0) p.id = L"team." + p.id;
        ++n;
    }
}

void QuickPromptStore::SetTeamFile(const std::wstring& teamFile) {
    std::lock_guard lock(mu_);
    if (teamFile == teamFile_) return;
    teamFile_ = teamFile;
    LoadTeam();
    ++version_;
}

bool QuickPromptStore::ReloadIfChanged() {
    std::lock_guard lock(mu_);
    bool changed = false;
    if (!folder_.empty() && !SameStamp(StampOf(UserPath()), userStamp_)) {
        Load(folder_, teamFile_);
        return true;
    }
    if (!SameStamp(StampOf(teamFile_), teamStamp_)) {
        LoadTeam();
        ++version_;
        changed = true;
    }
    return changed;
}

bool QuickPromptStore::Save() {
    std::lock_guard lock(mu_);
    ++version_;
    if (folder_.empty() || userBad_) return false;
    Json root = Json::Object();
    root.Set("version", 1);
    Json arr = Json::Array();
    for (const auto& p : user_) arr.Push(PromptToJson(p, true));
    root.Set("prompts", arr);
    Json st = Json::Object();
    for (const auto& [id, s] : state_) {
        if (s.enabled && s.uses == 0) continue;
        Json o = Json::Object();
        o.Set("enabled", s.enabled);
        o.Set("uses", s.uses);
        o.Set("lastUsed", Json(s.lastUsed));
        st.Set(ToUtf8(id), o);
    }
    root.Set("state", st);
    const bool ok = WriteFileAtomic(UserPath(), root.Dump());
    userStamp_ = StampOf(UserPath());
    return ok;
}

std::vector<QuickPrompt> QuickPromptStore::All() const {
    std::lock_guard lock(mu_);
    std::vector<QuickPrompt> v;
    auto withState = [&](QuickPrompt p) {
        auto it = state_.find(p.id);
        if (it != state_.end()) {
            p.enabled = it->second.enabled;
            p.uses = it->second.uses;
            p.lastUsed = it->second.lastUsed;
        }
        return p;
    };
    for (const auto& p : BuiltinQuickPrompts()) v.push_back(withState(p));
    for (const auto& p : team_) v.push_back(withState(p));
    for (const auto& p : user_) v.push_back(p);
    return v;
}

std::vector<QuickPrompt> QuickPromptStore::Active() const {
    std::vector<QuickPrompt> all = All(), out;
    auto rank = [](const QuickPrompt& p) { return p.source == L"user" ? 0 : p.source == L"team" ? 1 : 2; };
    std::stable_sort(all.begin(), all.end(), [&](const QuickPrompt& a, const QuickPrompt& b) { return rank(a) < rank(b); });
    for (auto& p : all) {
        if (!p.enabled || !CheckTrigger(p.trigger).empty()) continue;
        const std::wstring t = LowerW(p.trigger);
        if (std::any_of(out.begin(), out.end(), [&](const QuickPrompt& q) { return LowerW(q.trigger) == t; })) continue;
        out.push_back(std::move(p));
    }
    return out;
}

bool QuickPromptStore::Find(const std::wstring& id, QuickPrompt* out) const {
    for (auto& p : All())
        if (p.id == id) {
            if (out) *out = p;
            return true;
        }
    return false;
}

std::wstring QuickPromptStore::TeamError() const {
    std::lock_guard lock(mu_);
    return teamError_;
}

QuickPrompt* QuickPromptStore::FindUser(const std::wstring& id) {
    for (auto& p : user_)
        if (p.id == id) return &p;
    return nullptr;
}

std::wstring QuickPromptStore::TriggerOwner(const std::wstring& trigger, const std::wstring& exceptId) const {
    const std::wstring t = LowerW(trigger);
    for (const auto& p : All())
        if (p.id != exceptId && p.enabled && LowerW(p.trigger) == t) return p.id;
    return L"";
}

std::wstring QuickPromptStore::Validate(const QuickPrompt& p) const {
    if (std::wstring e = CheckTrigger(p.trigger); !e.empty()) return e;
    // Another of the person's own prompts with the same trigger would never be reached.
    for (const auto& q : user_)
        if (q.id != p.id && q.enabled && p.enabled && LowerW(q.trigger) == LowerW(p.trigger))
            return L"“" + p.trigger + L"” is already the trigger of “" + (q.title.empty() ? q.trigger : q.title) + L"”.";
    return L"";
}

std::wstring QuickPromptStore::Add(QuickPrompt p, std::wstring* newId) {
    std::lock_guard lock(mu_);
    p.source = L"user";
    if (p.id.empty() || FindUser(p.id)) p.id = MakeId();
    if (std::wstring e = Validate(p); !e.empty()) return e;
    if (newId) *newId = p.id;
    user_.push_back(std::move(p));
    Save();
    return L"";
}

std::wstring QuickPromptStore::Update(const QuickPrompt& p) {
    std::lock_guard lock(mu_);
    QuickPrompt* cur = FindUser(p.id);
    if (!cur) return L"Only your own prompts can be changed. Duplicate it to edit a copy.";
    if (std::wstring e = Validate(p); !e.empty()) return e;
    QuickPrompt next = p;
    next.source = L"user";
    *cur = std::move(next);
    Save();
    return L"";
}

void QuickPromptStore::Remove(const std::wstring& id) {
    std::lock_guard lock(mu_);
    user_.erase(std::remove_if(user_.begin(), user_.end(), [&](const QuickPrompt& p) { return p.id == id; }), user_.end());
    Save();
}

void QuickPromptStore::SetEnabled(const std::wstring& id, bool on) {
    std::lock_guard lock(mu_);
    if (QuickPrompt* p = FindUser(id)) p->enabled = on;
    else state_[id].enabled = on;
    Save();
}

void QuickPromptStore::NoteUsed(const std::wstring& id) {
    std::lock_guard lock(mu_);
    if (QuickPrompt* p = FindUser(id)) {
        ++p->uses;
        p->lastUsed = NowIso();
    } else {
        State& s = state_[id];
        ++s.uses;
        s.lastUsed = NowIso();
    }
    Save();
}

int QuickPromptStore::ImportFile(const std::wstring& path, std::vector<std::wstring>* skipped, std::wstring* error) {
    bool ok = false;
    const std::wstring text = ReadTextFile(path, &ok);
    if (!ok) {
        if (error) *error = L"Couldn't open " + path;
        return 0;
    }
    std::vector<QuickPrompt> in;
    const std::wstring lower = LowerW(path);
    if (lower.size() > 6 && lower.substr(lower.size() - 6) == L".plist") {
        in = ParseApplePlist(text);
    } else if (lower.size() > 4 && lower.substr(lower.size() - 4) == L".csv") {
        in = ParseCsv(text);
    } else {
        in = ParsePrompts(ToUtf8(text), &ok);
        if (!ok) {
            if (error) *error = L"That file isn't a QuickPrompts JSON file.";
            return 0;
        }
    }
    std::lock_guard lock(mu_);
    int added = 0;
    for (auto& p : in) {
        p.id.clear();
        p.source = L"user";
        p.uses = 0;
        p.lastUsed.clear();
        p.enabled = true;
        if (!CheckTrigger(p.trigger).empty() || !TriggerOwner(p.trigger, L"").empty()) {
            if (skipped) skipped->push_back(p.trigger);
            continue;
        }
        p.id = MakeId();
        user_.push_back(std::move(p));
        ++added;
    }
    if (added) Save();
    return added;
}

bool QuickPromptStore::ExportJson(const std::wstring& path, const std::vector<std::wstring>& ids) const {
    Json root = Json::Object();
    root.Set("version", 1);
    Json arr = Json::Array();
    for (const auto& p : All())
        if (std::find(ids.begin(), ids.end(), p.id) != ids.end()) {
            QuickPrompt q = p;
            if (q.id.rfind(L"team.", 0) == 0) q.id = q.id.substr(5);
            arr.Push(PromptToJson(q, false));
        }
    root.Set("prompts", arr);
    return WriteFileAtomic(path, root.Dump());
}

bool QuickPromptStore::ExportCsv(const std::wstring& path, const std::vector<std::wstring>& ids) const {
    std::wstring t = L"trigger,text,title\r\n";
    for (const auto& p : All())
        if (std::find(ids.begin(), ids.end(), p.id) != ids.end())
            t += CsvField(p.trigger) + L"," + CsvField(p.text) + L"," + CsvField(p.title) + L"\r\n";
    return WriteFileAtomic(path, "\xEF\xBB\xBF" + ToUtf8(t));
}

// ---- tests ----

namespace {
TriggerMatcher MatcherFor(std::initializer_list<const wchar_t*> triggers) {
    TriggerMatcher m;
    std::vector<TriggerMatcher::Entry> e;
    int i = 0;
    for (auto t : triggers) e.push_back({t, false, i++});
    m.SetTriggers(e);
    return m;
}
}  // namespace

ATHER_TEST(quickprompts_matcher_expands_after_space_only) {
    TriggerMatcher m = MatcherFor({L"doitforme"});
    auto h = FeedMatcher(m, L"doitforme ");
    CHECK_EQ(h.size(), 1u);
    CHECK(h.size() == 1 && h[0].first == 0 && h[0].second == 10);
    CHECK(m.Buffer().empty());
    // Enter, Tab and punctuation never expand (Enter and Tab reset; punctuation is just a character).
    m.Reset();
    CHECK(FeedMatcher(m, L"doitforme\n").empty());
    CHECK(FeedMatcher(m, L"doitforme. ").empty());
    // Typed with words before it, in the middle of a sentence.
    m.Reset();
    CHECK_EQ(FeedMatcher(m, L"hello there doitforme ").size(), 1u);
}

ATHER_TEST(quickprompts_matcher_whole_words_only) {
    TriggerMatcher m = MatcherFor({L"doitforme"});
    CHECK(FeedMatcher(m, L"redoitforme ").empty());
    m.Reset();
    CHECK(FeedMatcher(m, L"x_doitforme ").empty());
    m.Reset();
    CHECK_EQ(FeedMatcher(m, L"(doitforme ").size(), 1u);  // after punctuation is a new word
    // Prefixed triggers need a space or the start of typing before them.
    TriggerMatcher p = MatcherFor({L";dfm"});
    CHECK_EQ(FeedMatcher(p, L";dfm ").size(), 1u);
    CHECK_EQ(FeedMatcher(p, L"ok ;dfm ").size(), 1u);
    CHECK(FeedMatcher(p, L"a;dfm ").empty());
    CHECK(FeedMatcher(p, L"x.;dfm ").empty());
}

ATHER_TEST(quickprompts_matcher_backspace_reset_case_longest) {
    TriggerMatcher m = MatcherFor({L"doitforme"});
    CHECK_EQ(FeedMatcher(m, L"doitfoX\brme ").size(), 1u);
    CHECK(FeedMatcher(m, L"doitfo\nrme ").empty());  // the caret moved in the middle
    m.Reset();
    CHECK_EQ(FeedMatcher(m, L"DoItForMe ").size(), 1u);  // case-insensitive by default
    TriggerMatcher cs;
    cs.SetTriggers({{L"Sig", true, 7}});
    CHECK(FeedMatcher(cs, L"sig ").empty());
    cs.Reset();
    auto h = FeedMatcher(cs, L"Sig ");
    CHECK(h.size() == 1 && h[0].first == 7 && h[0].second == 4);
    TriggerMatcher l = MatcherFor({L"fix", L"quickfix"});
    h = FeedMatcher(l, L"quickfix ");
    CHECK(h.size() == 1 && h[0].first == 1 && h[0].second == 9);
    l.Reset();
    h = FeedMatcher(l, L"fix ");
    CHECK(h.size() == 1 && h[0].first == 0);
    // The buffer stays bounded however long people type without a reset.
    TriggerMatcher b = MatcherFor({L"zz"});
    FeedMatcher(b, std::wstring(500, L'a'));
    CHECK(b.Buffer().size() <= TriggerMatcher::kMaxBuffer);
    CHECK_EQ(FeedMatcher(b, L" zz ").size(), 1u);
}

ATHER_TEST(quickprompts_placeholders) {
    ExpandContext ctx;
    ctx.clipboard = L"copied\r\ntext";
    ctx.lastCapture = L"C:\\shots\\a.png";
    ctx.now.tm_year = 2026 - 1900;
    ctx.now.tm_mon = 10;
    ctx.now.tm_mday = 4;
    ctx.now.tm_hour = 9;
    ctx.now.tm_min = 5;
    ctx.now.tm_sec = 7;
    Expansion e = ExpandPlaceholders(L"Hi {cursor}!\r\nbye", ctx);
    CHECK(e.text == L"Hi !\nbye");
    CHECK_EQ(e.cursorBack, 5);
    CHECK(!e.pasteImage);
    e = ExpandPlaceholders(L"{clipboard}|{date}|{time}|{datetime}|{date:dd/MM/yyyy ss}|{lastcapture}", ctx);
    CHECK(e.text == L"copied\ntext|2026-11-04|09:05|2026-11-04 09:05|04/11/2026 07|C:\\shots\\a.png");
    e = ExpandPlaceholders(L"{{literal}} {unknown} {LastCapture:Image}", ctx);
    CHECK(e.text == L"{literal} {unknown} ");
    CHECK(e.pasteImage);
    CHECK_EQ(e.cursorBack, 0);
    auto u = UnknownPlaceholders(L"{cursor} {nope} {{x}} {date:yyyy} {nope}");
    CHECK(u.size() == 1 && u[0] == L"nope");
    // Every built-in prompt uses only known placeholders and has a valid, unique trigger.
    for (const auto& p : BuiltinQuickPrompts()) {
        CHECK(UnknownPlaceholders(p.text).empty());
        CHECK(CheckTrigger(p.trigger).empty());
        int same = 0;
        for (const auto& q : BuiltinQuickPrompts()) same += _wcsicmp(p.trigger.c_str(), q.trigger.c_str()) == 0;
        CHECK_EQ(same, 1);
    }
}

ATHER_TEST(quickprompts_store_round_trip_and_precedence) {
    const std::wstring dir = test::TempDir();
    QuickPromptStore s;
    std::wstring err;
    s.Load(dir, L"", &err);
    CHECK(err.empty());
    CHECK_EQ(s.All().size(), BuiltinQuickPrompts().size());
    QuickPrompt p;
    p.trigger = L"sigvn";
    p.title = L"Chữ ký";
    p.text = L"Trân trọng,\nNguyễn {{x}} 🎮";
    p.tags = {L"mail"};
    std::wstring id;
    CHECK(s.Add(p, &id).empty());
    CHECK(!id.empty());
    QuickPrompt dup = p;
    CHECK(!s.Add(dup).empty());  // the same trigger twice
    CHECK(!s.Add(QuickPrompt{L"", L"a b", L"", L"x"}).empty());
    s.SetEnabled(L"builtin.planpls", false);
    s.NoteUsed(L"builtin.doitforme");

    QuickPromptStore r;
    r.Load(dir, L"", &err);
    QuickPrompt back;
    CHECK(r.Find(id, &back));
    CHECK(back.text == p.text && back.title == p.title && back.tags.size() == 1);
    QuickPrompt plan, doit;
    CHECK(r.Find(L"builtin.planpls", &plan) && !plan.enabled);
    CHECK(r.Find(L"builtin.doitforme", &doit) && doit.uses == 1);
    auto active = r.Active();
    CHECK(std::none_of(active.begin(), active.end(), [](const QuickPrompt& q) { return q.id == L"builtin.planpls"; }));

    // The person's own prompt beats a built-in one with the same trigger; team beats built-in.
    const std::wstring team = dir + L"\\team.json";
    {
        std::ofstream f(team, std::ios::binary);
        f << R"({"prompts": [{"id": "t1", "trigger": "reviewpls", "title": "Team review", "text": "team"},
                             {"trigger": "doitforme", "text": "team doit"}]})";
    }
    QuickPrompt mine;
    mine.trigger = L"DoItForMe";
    mine.text = L"mine";
    CHECK(r.Add(mine).empty());
    r.SetTeamFile(team);
    CHECK(r.TeamError().empty());
    active = r.Active();
    auto textOf = [&](const wchar_t* trig) {
        for (const auto& q : active)
            if (_wcsicmp(q.trigger.c_str(), trig) == 0) return q.text;
        return std::wstring();
    };
    CHECK(textOf(L"doitforme") == L"mine");
    CHECK(textOf(L"reviewpls") == L"team");
    QuickPrompt t1;
    CHECK(r.Find(L"team.t1", &t1) && t1.ReadOnly());
    CHECK(!r.Update(t1).empty());  // read-only

    // A hand-edited file is picked up; a broken one is never overwritten.
    Sleep(20);
    {
        std::ofstream f(dir + L"\\quickprompts.json", std::ios::binary);
        f << R"({"version": 1, "prompts": [{"id": "h1", "trigger": "handmade", "text": "by hand"}]})";
    }
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    HANDLE h = CreateFileW((dir + L"\\quickprompts.json").c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    CHECK(r.ReloadIfChanged());
    CHECK(r.Find(L"h1", nullptr));
    {
        std::ofstream f(dir + L"\\quickprompts.json", std::ios::binary);
        f << "{ not json";
    }
    QuickPromptStore bad;
    bad.Load(dir, L"", &err);
    CHECK(!err.empty());
    CHECK(GetFileAttributesW((dir + L"\\quickprompts.json.bad").c_str()) != INVALID_FILE_ATTRIBUTES);
    bad.Add(QuickPrompt{L"", L"zzz", L"", L"x"});  // accepted in memory, but the broken file is kept
    std::string after;
    ReadFileBytes(dir + L"\\quickprompts.json", &after);
    CHECK(after == "{ not json");
}

ATHER_TEST(quickprompts_import_export) {
    auto csv = QuickPromptStore::ParseCsv(L"trigger,text,title\r\nsig,\"Best,\r\n\"\"Tin\"\"\",Signature\naddr,Hà Nội\n");
    CHECK_EQ(csv.size(), 2u);
    CHECK(csv.size() == 2 && csv[0].trigger == L"sig" && csv[0].text == L"Best,\r\n\"Tin\"" && csv[0].title == L"Signature");
    CHECK(csv.size() == 2 && csv[1].text == L"Hà Nội");
    auto pl = QuickPromptStore::ParseApplePlist(
        L"<?xml version=\"1.0\"?><plist><array><dict><key>phrase</key><string>On my way &amp; soon &#x1F600;</string>"
        L"<key>shortcut</key><string>omw</string></dict><dict><key>shortcut</key><string>x</string></dict></array></plist>");
    CHECK_EQ(pl.size(), 1u);
    CHECK(pl.size() == 1 && pl[0].trigger == L"omw" && pl[0].text == L"On my way & soon \U0001F600");

    const std::wstring dir = test::TempDir();
    QuickPromptStore s;
    s.Load(dir, L"");
    {
        std::ofstream f(dir + L"\\in.csv", std::ios::binary);
        f << "\xEF\xBB\xBFtrigger,text\nnewone,Hello\ndoitforme,taken\na,too short\n";
    }
    std::vector<std::wstring> skipped;
    std::wstring err;
    CHECK_EQ(s.ImportFile(dir + L"\\in.csv", &skipped, &err), 1);
    CHECK_EQ(skipped.size(), 2u);
    std::vector<std::wstring> ids;
    for (const auto& p : s.All())
        if (p.source == L"user" || p.id == L"builtin.bugpls") ids.push_back(p.id);
    CHECK(s.ExportJson(dir + L"\\team.json", ids));
    QuickPromptStore t;
    t.Load(test::TempDir(), dir + L"\\team.json");
    CHECK(t.TeamError().empty());
    int team = 0;
    for (const auto& p : t.All()) team += p.source == L"team";
    CHECK_EQ(team, 2);
    CHECK(s.ExportCsv(dir + L"\\out.csv", ids));
    bool ok = false;
    auto back = QuickPromptStore::ParseCsv(ReadTextFile(dir + L"\\out.csv", &ok));
    CHECK_EQ(back.size(), 2u);
}

}  // namespace ather
