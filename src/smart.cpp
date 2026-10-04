#include "smart.h"

#include <algorithm>
#include <mutex>

#include "selftest.h"

namespace ather {

double NowEpoch() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (double)(t - 116444736000000000ull) / 1e7;
}

namespace {

std::wstring Lower(const std::wstring& s) { return LowerText(s); }

bool Contains(const std::wstring& hay, const std::wstring& w) { return hay.find(w) != std::wstring::npos; }

template <class T>
bool In(const std::vector<T>& v, const T& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// ---- calendar helpers (local time, Gregorian) ----

uint64_t EpochToFt(double e) { return (uint64_t)(e * 1e7) + 116444736000000000ull; }
double FtToEpoch(uint64_t t) { return (double)((int64_t)t - 116444736000000000ll) / 1e7; }

SYSTEMTIME LocalOf(double epoch) {
    const uint64_t t = EpochToFt(epoch);
    FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc, local;
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

double EpochOfLocal(SYSTEMTIME local) {
    SYSTEMTIME utc;
    TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc);
    FILETIME ft;
    SystemTimeToFileTime(&utc, &ft);
    return FtToEpoch((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
}

SYSTEMTIME Midnight(SYSTEMTIME t) {
    t.wHour = t.wMinute = t.wSecond = t.wMilliseconds = 0;
    return t;
}

// Moves a local date by whole days (calendar arithmetic, no time zone involved).
SYSTEMTIME AddDays(SYSTEMTIME t, int days) {
    FILETIME ft;
    SystemTimeToFileTime(&t, &ft);
    uint64_t v = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    v += (int64_t)days * 864000000000ll;
    ft = {(DWORD)v, (DWORD)(v >> 32)};
    FileTimeToSystemTime(&ft, &t);
    return t;
}

int FirstDayOfWeek() {  // 0 = Sunday
    wchar_t buf[4] = L"0";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, buf, 4);  // 0 = Monday … 6 = Sunday
    return (_wtoi(buf) + 1) % 7;
}

SYSTEMTIME WeekStart(SYSTEMTIME today) {
    const int back = (today.wDayOfWeek - FirstDayOfWeek() + 7) % 7;
    return AddDays(today, -back);
}

// ---- suggested tags (lists copied from Smart.swift) ----

struct TagList {
    const wchar_t* tag;
    std::vector<std::wstring> names;
};

const std::vector<TagList>& AppTags() {
    // Tag → apps that imply it, matched case-insensitively against the capture's source app. Windows apps
    // report their process name (Code, chrome, msedge…), so those spellings are included too.
    static const std::vector<TagList> k = {
        {L"code", {L"xcode", L"visual studio code", L"code", L"cursor", L"intellij idea", L"pycharm", L"webstorm", L"android studio",
                   L"sublime text", L"nova", L"zed", L"bbedit", L"goland", L"rider", L"devenv", L"idea64", L"pycharm64",
                   L"webstorm64", L"sublime_text", L"rider64", L"goland64", L"notepad++", L"windsurf"}},
        {L"terminal", {L"terminal", L"iterm2", L"iterm", L"warp", L"ghostty", L"alacritty", L"kitty", L"hyper", L"wezterm",
                       L"windowsterminal", L"cmd", L"powershell", L"pwsh", L"conhost", L"wezterm-gui", L"mintty"}},
        {L"chat", {L"slack", L"messages", L"discord", L"whatsapp", L"telegram", L"signal", L"microsoft teams", L"teams", L"messenger",
                   L"wechat", L"line", L"zoom", L"ms-teams"}},
        {L"email", {L"mail", L"microsoft outlook", L"outlook", L"spark", L"airmail", L"mimestream", L"superhuman", L"olk",
                    L"thunderbird", L"hxoutlook"}},
        {L"design", {L"figma", L"sketch", L"adobe xd", L"framer", L"affinity designer", L"affinity designer 2", L"adobe photoshop",
                     L"adobe illustrator", L"pixelmator pro", L"photoshop", L"illustrator", L"designer"}},
        {L"web", {L"safari", L"google chrome", L"chrome", L"arc", L"firefox", L"microsoft edge", L"brave browser", L"orion", L"dia",
                  L"vivaldi", L"opera", L"msedge", L"brave"}},
        {L"document", {L"pages", L"microsoft word", L"notion", L"obsidian", L"bear", L"notes", L"craft", L"preview", L"ulysses",
                       L"winword", L"notepad", L"acrobat", L"acrord32"}},
        {L"spreadsheet", {L"numbers", L"microsoft excel", L"excel"}},
        {L"calendar", {L"calendar", L"fantastical", L"busycal", L"notion calendar"}},
        {L"settings", {L"system settings", L"system preferences", L"systemsettings"}},
        {L"mobile", {L"simulator", L"iphone mirroring", L"phoneexperiencehost", L"scrcpy"}},
    };
    return k;
}

const std::vector<TagList>& WordTags() {
    // Tag → words or phrases in the text in the image.
    static const std::vector<TagList> k = {
        {L"error", {L"error", L"exception", L"failed", L"failure", L"fatal", L"denied", L"crash", L"crashed", L"not found", L"unable to",
                    L"couldn't", L"could not", L"cannot", L"traceback", L"panic", L"timed out", L"timeout", L"invalid", L"unexpected",
                    L"something went wrong"}},
        {L"receipt", {L"subtotal", L"invoice", L"receipt", L"amount due", L"order total", L"order #", L"order number", L"vat",
                      L"total paid"}},
        {L"login", {L"sign in", L"log in", L"login", L"forgot password", L"password", L"verification code", L"two-factor", L"sign up"}},
        {L"dashboard", {L"dashboard", L"analytics", L"revenue", L"metrics", L"conversion", L"active users", L"kpi"}},
        {L"settings", {L"preferences", L"settings"}},
    };
    return k;
}

const std::vector<std::wstring> kCodeSignals = {L"func ", L"def ", L"class ", L"import ", L"const ", L"let ", L"var ",
                                                L"return ", L"=>", L"};", L"</", L"#include", L"public ", L"private "};
const std::vector<std::wstring> kTerminalSignals = {L"$ ", L"% ", L"~/", L"sudo ", L"npm ", L"git ", L"brew ",
                                                    L"cd ", L"ls ", L"zsh", L"bash"};

struct CacheEntry {
    std::wstring key;
    std::vector<std::wstring> tags;
};
std::mutex g_tagMu;
std::unordered_map<std::wstring, CacheEntry> g_tagCache;

// ---- search ----

const std::vector<std::wstring> kStop = {L"the", L"a", L"an", L"and", L"or", L"of", L"to", L"in", L"on", L"with", L"for",
                                         L"from", L"that", L"this", L"my", L"me", L"screenshot", L"screenshots", L"capture",
                                         L"captures", L"image", L"showing", L"show", L"about", L"where", L"which", L"some", L"any"};

// Screenshot vocabulary, copied from Smart.swift.
const std::vector<std::vector<std::wstring>> kGroups = {
    {L"chart", L"graph", L"plot", L"dashboard", L"analytics", L"metrics", L"stats", L"statistics"},
    {L"login", L"log in", L"signin", L"sign in", L"password", L"auth", L"authentication", L"account"},
    {L"error", L"errors", L"failed", L"failure", L"exception", L"crash", L"crashed", L"crashes", L"bug", L"issue", L"problem",
     L"warning", L"fatal"},
    {L"chat", L"message", L"messages", L"conversation", L"dm", L"slack", L"discord", L"thread", L"reply"},
    {L"code", L"source", L"function", L"snippet", L"programming", L"swift", L"python", L"javascript", L"typescript"},
    {L"terminal", L"shell", L"console", L"command", L"cli", L"bash", L"zsh"},
    {L"build", L"compile", L"compiler", L"compiled", L"tests", L"passed"},
    {L"payment", L"checkout", L"card", L"billing", L"pay", L"purchase", L"order", L"declined"},
    {L"receipt", L"invoice", L"bill", L"total", L"subtotal"},
    {L"website", L"web", L"site", L"page", L"browser", L"landing", L"url"},
    {L"design", L"mockup", L"figma", L"ui", L"layout", L"prototype", L"illustration"},
    {L"email", L"mail", L"inbox", L"subject"},
    {L"settings", L"preferences", L"options", L"config", L"configuration"},
    {L"phone", L"mobile", L"iphone", L"ios", L"android", L"app"},
    {L"team", L"colleagues", L"coworkers", L"people", L"teammates"},
    {L"sales", L"revenue", L"income", L"profit", L"earnings"},
    {L"meeting", L"call", L"zoom", L"calendar", L"event", L"schedule"},
    {L"release", L"version", L"changelog", L"notes", L"update"},
    {L"docs", L"documentation", L"document", L"guide", L"manual"},
    {L"dark", L"night", L"black"},
};

std::mutex g_queryMu;
std::unordered_map<std::wstring, std::vector<std::wstring>> g_relatedCache;

std::wstring Stemmed(const std::wstring& w) {
    for (const wchar_t* suf : {L"ing", L"ed", L"es", L"s"}) {
        const size_t n = wcslen(suf);
        if (w.size() > n + 3 && w.compare(w.size() - n, n, suf) == 0) return w.substr(0, w.size() - n);
    }
    return w;
}

bool IsWordChar(wchar_t c) { return IsCharAlphaNumericW(c) != 0; }

std::wstring Replace(std::wstring s, const std::wstring& a, const std::wstring& b) {
    for (size_t p = 0; (p = s.find(a, p)) != std::wstring::npos; p += b.size()) s.replace(p, a.size(), b);
    return s;
}

}  // namespace

namespace autotag {

bool Has(const std::wstring& hay, const std::wstring& w) {
    if (w.empty()) return false;
    for (size_t p = hay.find(w); p != std::wstring::npos; p = hay.find(w, p + 1)) {
        const wchar_t before = p == 0 ? L' ' : hay[p - 1];
        const wchar_t after = p + w.size() >= hay.size() ? L' ' : hay[p + w.size()];
        if (!IsWordChar(before) && !IsWordChar(after)) return true;
    }
    return false;
}

std::vector<std::wstring> Compute(const std::wstring&, const ItemMeta& m) {
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& t) {
        if (!In(out, t)) out.push_back(t);
    };
    const std::wstring app = Lower(m.app);
    for (const auto& [t, names] : AppTags())
        if (In(names, app)) add(t);
    const std::wstring text = Lower(m.text.value_or(L""));
    const std::wstring hay = L" " + Replace(text, L"\n", L" ") + L" ";
    if (!text.empty()) {
        for (const auto& [t, ws] : WordTags())
            if (std::any_of(ws.begin(), ws.end(), [&](const std::wstring& w) { return Has(hay, w); })) add(t);
        if (std::count_if(kCodeSignals.begin(), kCodeSignals.end(), [&](const std::wstring& s) { return Contains(text, s); }) >= 3)
            add(L"code");
        if (std::count_if(kTerminalSignals.begin(), kTerminalSignals.end(), [&](const std::wstring& s) { return Contains(text, s); }) >= 3)
            add(L"terminal");
        if (Contains(text, L"from:") && Contains(text, L"subject:")) add(L"email");
        if (Contains(text, L"http://") || Contains(text, L"https://") || Contains(text, L"www.")) add(L"web");
    }
    if (m.w > 0 && m.h > 0 && (double)m.h / m.w >= 1.8 && m.w <= 1400) add(L"mobile");
    if (!m.colors.empty()) {
        const PaletteColor& c = *std::max_element(m.colors.begin(), m.colors.end(),
                                                  [](const PaletteColor& a, const PaletteColor& b) { return a.ratio < b.ratio; });
        if (c.ratio > 0.35) {
            const double lum = (0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / 255;
            if (lum < 0.18) add(L"dark");
        }
    }
    return out;
}

std::vector<std::wstring> Suggest(const std::wstring& path, const ItemMeta& m) {
    wchar_t key[96];
    swprintf_s(key, L"%.6f|%lld|%dx%d|%zu|", m.mtime, m.text ? (long long)m.text->size() : -1ll, m.w, m.h, m.colors.size());
    const std::wstring k = key + m.app;
    {
        std::lock_guard lock(g_tagMu);
        auto it = g_tagCache.find(path);
        if (it != g_tagCache.end() && it->second.key == k) return it->second.tags;
    }
    auto tags = Compute(path, m);
    std::lock_guard lock(g_tagMu);
    if (g_tagCache.size() > 20000) g_tagCache.clear();
    g_tagCache[path] = {k, tags};
    return tags;
}

std::vector<std::wstring> Pending(const std::wstring& path, const ItemMeta& m) {
    std::vector<std::wstring> have;
    for (const auto& t : m.tags) have.push_back(Lower(t));
    for (const auto& t : m.dismissed) have.push_back(Lower(t));
    std::vector<std::wstring> out;
    for (const auto& t : Suggest(path, m))
        if (!In(have, t)) out.push_back(t);
    return out;
}

}  // namespace autotag

std::vector<std::wstring> SearchQuery::Related(const std::wstring& w) {
    {
        std::lock_guard lock(g_queryMu);
        auto it = g_relatedCache.find(w);
        if (it != g_relatedCache.end()) return it->second;
    }
    std::vector<std::wstring> out;
    auto add = [&](const std::wstring& x) {
        if (x != w && !In(out, x)) out.push_back(x);
    };
    const std::wstring stem = Stemmed(w);
    for (const auto& g : kGroups)
        if (In(g, w) || In(g, stem))
            for (const auto& x : g) add(x);
    if (stem != w) add(stem);
    std::lock_guard lock(g_queryMu);
    g_relatedCache[w] = out;
    return out;
}

SearchQuery SearchQuery::Parse(const std::wstring& text, double now) {
    if (now == 0) now = NowEpoch();
    SearchQuery q;
    std::wstring s = L" " + Lower(text) + L" ";
    const SYSTEMTIME today = Midnight(LocalOf(now));
    const double todayE = EpochOfLocal(today);
    const SYSTEMTIME week = WeekStart(today);
    const double weekE = EpochOfLocal(week);
    SYSTEMTIME month = today;
    month.wDay = 1;
    SYSTEMTIME lastMonth = month;
    if (lastMonth.wMonth == 1) {
        lastMonth.wMonth = 12;
        --lastMonth.wYear;
    } else {
        --lastMonth.wMonth;
    }
    SYSTEMTIME year = month;
    year.wMonth = 1;
    SYSTEMTIME lastYear = year;
    --lastYear.wYear;
    struct Phrase {
        const wchar_t* p;
        double since;
        std::optional<double> until;
    };
    const Phrase phrases[] = {
        {L"today", todayE, std::nullopt},
        {L"yesterday", todayE - 86400, todayE},
        {L"this week", weekE, std::nullopt},
        {L"last week", weekE - 7 * 86400, weekE},
        {L"past week", now - 7 * 86400, std::nullopt},
        {L"this month", EpochOfLocal(month), std::nullopt},
        {L"last month", EpochOfLocal(lastMonth), EpochOfLocal(month)},
        {L"this year", EpochOfLocal(year), std::nullopt},
        {L"last year", EpochOfLocal(lastYear), EpochOfLocal(year)},
    };
    for (const auto& ph : phrases) {
        const std::wstring needle = std::wstring(L" ") + ph.p + L" ";
        if (!Contains(s, needle)) continue;
        q.since = ph.since;
        q.until = ph.until;
        s = Replace(s, needle, L" ");
    }
    for (size_t start = 0; start < s.size();) {
        size_t sp = s.find(L' ', start);
        std::wstring word = s.substr(start, sp == std::wstring::npos ? std::wstring::npos : sp - start);
        if (!word.empty() && !In(kStop, word)) q.terms.push_back({word, Related(word)});
        if (sp == std::wstring::npos) break;
        start = sp + 1;
    }
    return q;
}

MatchKind SearchQuery::Match(const std::wstring& hay, double mtime) const {
    if (since && mtime < *since) return MatchKind::None;
    if (until && mtime >= *until) return MatchKind::None;
    bool exact = true;
    const std::wstring padded = L" " + hay + L" ";
    for (const auto& t : terms) {
        if (Contains(hay, t.word)) continue;
        if (!std::any_of(t.related.begin(), t.related.end(), [&](const std::wstring& r) { return autotag::Has(padded, r); }))
            return MatchKind::None;
        exact = false;
    }
    return exact ? MatchKind::Exact : MatchKind::Related;
}

// ---- tests (SmartTests.swift) ----

namespace {
ItemMeta TestMeta(const std::wstring& app = L"", const std::wstring& text = L"", int w = 1600, int h = 1000, double mtime = 1000) {
    ItemMeta m;
    m.app = app;
    m.text = text;
    m.w = w;
    m.h = h;
    m.mtime = mtime;
    return m;
}
const std::wstring kU = L"C:\\tmp\\ather-smart\\a.png";
}  // namespace

ATHER_TEST(smart_suggested_tags) {
    using autotag::Compute;
    CHECK(Compute(kU, TestMeta(L"Slack", L"Ship it on Friday?")) == std::vector<std::wstring>{L"chat"});
    CHECK(In(Compute(kU, TestMeta(L"Safari", L"Payment failed. Error 402: card declined")), std::wstring(L"error")));
    CHECK(In(Compute(kU, TestMeta(L"Safari", L"Payment failed")), std::wstring(L"web")));
    CHECK(In(Compute(kU, TestMeta(L"", L"import Foundation\nfunc main() {\n  let x = 1\n  return x\n}")), std::wstring(L"code")));
    CHECK(In(Compute(kU, TestMeta(L"", L"Sign in to continue · Forgot password?")), std::wstring(L"login")));
    CHECK(In(Compute(kU, TestMeta(L"", L"", 1179, 2556)), std::wstring(L"mobile")));
    CHECK(!In(Compute(kU, TestMeta(L"", L"display settings for the paystub")), std::wstring(L"receipt")));
    CHECK(In(Compute(kU, TestMeta(L"msedge")), std::wstring(L"web")));  // Windows process names
    ItemMeta m = TestMeta(L"Slack");
    m.dismissed = {L"chat"};
    CHECK(autotag::Pending(kU, m).empty());
    ItemMeta dark = TestMeta();
    dark.colors = {{20, 20, 24, 0.6}, {240, 240, 240, 0.4}};
    CHECK(In(Compute(kU, dark), std::wstring(L"dark")));
}

ATHER_TEST(smart_related_words_and_ranking) {
    Filter f;
    f.text = L"sales graph";
    CHECK(f.Match(kU, TestMeta(L"", L"Weekly revenue chart")) == MatchKind::Related);
    CHECK(f.Match(kU, TestMeta(L"", L"sales graph for Q3")) == MatchKind::Exact);
    CHECK(f.Match(kU, TestMeta(L"", L"Team chat")) == MatchKind::None);
    Filter conv;
    conv.text = L"conversation";
    CHECK(conv.Match(kU, TestMeta(L"Slack", L"Ship it")) == MatchKind::Related);  // via the suggested "chat" tag
    Filter login;
    login.text = L"login";
    CHECK(login.Match(kU, TestMeta(L"", L"Sign in to your account")) == MatchKind::Exact);  // via the suggested tag
    Filter auth;
    auth.text = L"authentication";
    CHECK(auth.Match(kU, TestMeta(L"", L"Sign in to your account")) == MatchKind::Related);
    Filter design;
    design.text = L"design";  // short related words only match whole words: "ui" must not match "build"
    CHECK(design.Match(kU, TestMeta(L"", L"build succeeded")) == MatchKind::None);
}

ATHER_TEST(smart_date_phrases) {
    const double now = NowEpoch();
    SearchQuery q = SearchQuery::Parse(L"the payment error from last week", now);
    CHECK(q.terms.size() == 2 && q.terms[0].word == L"payment" && q.terms[1].word == L"error");
    CHECK(q.since.has_value());
    const double weekStart = EpochOfLocal(WeekStart(Midnight(LocalOf(now))));
    CHECK(q.Match(L"payment error", weekStart - 86400) != MatchKind::None);
    CHECK(q.Match(L"payment error", now) == MatchKind::None);  // this week: excluded
    CHECK(q.Match(L"payment error", weekStart - 20 * 86400) == MatchKind::None);
    CHECK(SearchQuery::Parse(L"today", now).Match(L"anything", now) != MatchKind::None);
    SearchQuery y = SearchQuery::Parse(L"yesterday", now);
    CHECK(y.Match(L"x", now) == MatchKind::None);
    CHECK(y.Match(L"x", EpochOfLocal(Midnight(LocalOf(now))) - 3600) != MatchKind::None);
    SearchQuery lm = SearchQuery::Parse(L"bug last month", now);
    CHECK(lm.since && lm.until && *lm.since < *lm.until && *lm.until <= now);
}

}  // namespace ather
