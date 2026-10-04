#include "gallery_model.h"

#include <shlwapi.h>

#include <algorithm>
#include <unordered_set>

#include "selftest.h"
#include "smart.h"

#pragma comment(lib, "shlwapi")

namespace ather {

const wchar_t* SortLabel(GallerySort s) {
    static const wchar_t* const k[] = {L"Newest first", L"Oldest first", L"Name", L"File size", L"Dimensions", L"Rating", L"Random"};
    return k[(int)s];
}

double ClampedAspect(double a) { return std::clamp(a, 0.25, 4.0); }

std::vector<JustifiedRow> JustifiedRows(const std::vector<double>& aspects, double width, double target, double spacing) {
    std::vector<JustifiedRow> rows;
    if (width <= 0) return rows;
    JustifiedRow cur;
    double sum = 0;
    for (int i = 0; i < (int)aspects.size(); ++i) {
        const double a = ClampedAspect(aspects[i]);
        cur.items.push_back(i);
        sum += a;
        const double w = sum * target + spacing * (cur.items.size() - 1);
        if (w >= width) {
            cur.height = (width - spacing * (cur.items.size() - 1)) / sum;
            rows.push_back(std::move(cur));
            cur = {};
            sum = 0;
        }
    }
    if (!cur.items.empty()) {
        cur.height = target;
        rows.push_back(std::move(cur));
    }
    return rows;
}

uint64_t SeededRandom::Next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    uint64_t z = state;
    z = (z ^ (z >> 33)) * 0xff51afd7ed558ccdull;
    return z ^ (z >> 33);
}

GalleryModel::GalleryModel(Library& l) : lib(l) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    seed_ = (uint64_t)t.QuadPart | 1;
}

static std::wstring ViewIni() { return SupportFolder() + L"\\gallery.ini"; }

void GalleryModel::LoadViewOptions() {
    const std::wstring ini = ViewIni();
    wchar_t buf[32];
    GetPrivateProfileStringW(L"View", L"Layout", L"rows", buf, 32, ini.c_str());
    layout = _wcsicmp(buf, L"grid") == 0 ? GalleryLayout::Grid : _wcsicmp(buf, L"list") == 0 ? GalleryLayout::List : GalleryLayout::Justified;
    thumbSize = std::clamp((double)GetPrivateProfileIntW(L"View", L"ThumbSize", 190, ini.c_str()), 110.0, 360.0);
    // Canvas-first: sidebar and inspector start hidden.
    showSidebar = GetPrivateProfileIntW(L"View", L"Sidebar", 0, ini.c_str()) != 0;
    showInspector = GetPrivateProfileIntW(L"View", L"Inspector", 0, ini.c_str()) != 0;
    inspectorDiscovered = GetPrivateProfileIntW(L"View", L"InspectorDiscovered", 0, ini.c_str()) != 0;
    stackEdits = GetPrivateProfileIntW(L"View", L"StackEdits", 1, ini.c_str()) != 0;
    showNames = GetPrivateProfileIntW(L"View", L"ShowNames", 0, ini.c_str()) != 0;
}

void GalleryModel::SaveViewOptions() const {
    const std::wstring ini = ViewIni();
    auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"View", k, v.c_str(), ini.c_str()); };
    put(L"Layout", layout == GalleryLayout::Grid ? L"grid" : layout == GalleryLayout::List ? L"list" : L"rows");
    put(L"ThumbSize", std::to_wstring((int)thumbSize));
    put(L"Sidebar", showSidebar ? L"1" : L"0");
    put(L"Inspector", showInspector ? L"1" : L"0");
    put(L"InspectorDiscovered", inspectorDiscovered ? L"1" : L"0");
    put(L"StackEdits", stackEdits ? L"1" : L"0");
    put(L"ShowNames", showNames ? L"1" : L"0");
}

void GalleryModel::SetShowInspector(bool on) {
    showInspector = on;
    if (on) inspectorDiscovered = true;  // the action bar stops spelling out "Details Ctrl+I"
    SaveViewOptions();
}

std::wstring GalleryModel::Title() const {
    switch (scope_.kind) {
        case ScopeKind::All: return L"All captures";
        case ScopeKind::Uncategorized: return L"Uncategorized";
        case ScopeKind::Recent: return L"Last 7 days";
        case ScopeKind::Rated: return L"Rated";
        case ScopeKind::Duplicates: return L"Duplicates";
        case ScopeKind::Type: return MediaTypeLabel(scope_.type);
        case ScopeKind::Collection:
            if (const LibCollection* c = lib.FindCollection(scope_.id)) return c->name;
            return L"Collection";
        case ScopeKind::Smart:
            for (const auto& s : lib.SmartFolders())
                if (s.id == scope_.id) return s.name;
            return L"Smart folder";
        case ScopeKind::Similar: return L"Similar to " + FileNameOf(scope_.id);
    }
    return L"";
}

void GalleryModel::SetScope(const Scope& s) {
    if (s == scope_) return;
    scope_ = s;
    if (s.kind == ScopeKind::Smart)
        for (const auto& f : lib.SmartFolders())
            if (f.id == s.id) filter_ = f.filter;
    selection.clear();
    focus.clear();
    Recompute();
}

void GalleryModel::SetFilter(const Filter& f) {
    if (f == filter_) return;
    filter_ = f;
    Recompute();
}

void GalleryModel::SetSort(GallerySort s) {
    sort_ = s;
    Recompute();
}

void GalleryModel::SetStackEdits(bool on) {
    stackEdits = on;
    SaveViewOptions();
    Recompute();
}

void GalleryModel::Shuffle() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    seed_ = (uint64_t)t.QuadPart * 2654435761ull | 1;
    Recompute();
}

bool GalleryModel::InScope(const std::wstring& path, const ItemMeta& m, double now) const {
    switch (scope_.kind) {
        case ScopeKind::Uncategorized: return m.tags.empty() && m.collections.empty();
        case ScopeKind::Recent: return m.mtime >= now - 7 * 86400;
        case ScopeKind::Rated: return m.rating > 0;
        case ScopeKind::Type: return MediaTypeOf(path) == scope_.type;
        case ScopeKind::Collection: return std::find(m.collections.begin(), m.collections.end(), scope_.id) != m.collections.end();
        default: return true;
    }
}

void GalleryModel::Recompute() {
    const double now = NowEpoch();
    if (scope_.kind == ScopeKind::Duplicates) {
        distances.clear();
        groups.clear();
        visible.clear();
        stacks.clear();
        relatedCount = 0;
        for (const auto& g : lib.DuplicateGroups()) {
            std::vector<std::wstring> kept;
            for (const auto& p : g)
                if (filter_.Matches(p, lib.Meta(p))) kept.push_back(p);
            if (kept.size() > 1) {
                visible.insert(visible.end(), kept.begin(), kept.end());
                groups.push_back(std::move(kept));
            }
        }
    } else if (scope_.kind == ScopeKind::Similar) {
        groups.clear();
        distances.clear();
        stacks.clear();
        relatedCount = 0;
        visible = {scope_.id};
        for (const auto& [p, d] : lib.Similar(scope_.id)) {
            if (!filter_.Matches(p, lib.Meta(p))) continue;
            distances[p] = d;
            visible.push_back(p);
        }
    } else {
        groups.clear();
        distances.clear();
        std::vector<std::wstring> list;
        std::unordered_set<std::wstring> related;
        for (const auto& p : lib.Paths()) {
            const ItemMeta& m = lib.Meta(p);
            if (!InScope(p, m, now)) continue;
            const MatchKind k = filter_.Match(p, m, now);
            if (k == MatchKind::None) continue;
            if (k == MatchKind::Related) related.insert(p);
            list.push_back(p);
        }
        auto meta = [&](const std::wstring& p) -> const ItemMeta& { return lib.Meta(p); };
        switch (sort_) {
            case GallerySort::Newest:
                std::stable_sort(list.begin(), list.end(), [&](auto& a, auto& b) { return meta(a).mtime > meta(b).mtime; });
                break;
            case GallerySort::Oldest:
                std::stable_sort(list.begin(), list.end(), [&](auto& a, auto& b) { return meta(a).mtime < meta(b).mtime; });
                break;
            case GallerySort::Name:
                std::stable_sort(list.begin(), list.end(),
                                 [&](auto& a, auto& b) { return StrCmpLogicalW(FileNameOf(a).c_str(), FileNameOf(b).c_str()) < 0; });
                break;
            case GallerySort::Size:
                std::stable_sort(list.begin(), list.end(), [&](auto& a, auto& b) { return meta(a).size > meta(b).size; });
                break;
            case GallerySort::Dimensions:
                std::stable_sort(list.begin(), list.end(),
                                 [&](auto& a, auto& b) { return (int64_t)meta(a).w * meta(a).h > (int64_t)meta(b).w * meta(b).h; });
                break;
            case GallerySort::Rating:
                std::stable_sort(list.begin(), list.end(), [&](auto& a, auto& b) {
                    return meta(a).rating != meta(b).rating ? meta(a).rating > meta(b).rating : meta(a).mtime > meta(b).mtime;
                });
                break;
            case GallerySort::Random: {
                SeededRandom g(seed_);
                for (size_t i = list.size(); i > 1; --i) std::swap(list[i - 1], list[g.Next() % i]);
                break;
            }
        }
        // Exact matches first; captures that only matched related words follow.
        relatedCount = (int)related.size();
        if (!related.empty())
            std::stable_partition(list.begin(), list.end(), [&](const std::wstring& p) { return !related.count(p); });
        visible = CollapseStacks(list);
    }
    std::unordered_set<std::wstring> vis(visible.begin(), visible.end());
    for (auto it = selection.begin(); it != selection.end();) it = vis.count(*it) ? std::next(it) : selection.erase(it);
    if (!focus.empty() && !vis.count(focus)) focus.clear();
}

// Groups each original with its edits (following "edited from" links) and, while browsing, shows only the
// newest version of each until the stack is expanded.
std::vector<std::wstring> GalleryModel::CollapseStacks(const std::vector<std::wstring>& list) {
    std::unordered_map<std::wstring, std::wstring> roots;
    auto root = [&](const std::wstring& u) {
        if (auto it = roots.find(u); it != roots.end()) return it->second;
        std::wstring cur = u;
        std::unordered_set<std::wstring> seen{cur};
        for (;;) {
            const auto& from = lib.Meta(cur).editedFrom;
            if (!from || !lib.Has(*from) || !seen.insert(*from).second) break;
            cur = *from;
        }
        roots[u] = cur;
        return cur;
    };
    std::unordered_map<std::wstring, std::vector<std::wstring>> byRoot;
    for (const auto& u : lib.Paths()) byRoot[root(u)].push_back(u);
    versionsByRoot_.clear();
    rootOf_.clear();
    for (auto& [r, vs] : byRoot) {
        if (vs.size() < 2) continue;
        std::stable_sort(vs.begin(), vs.end(), [&](auto& a, auto& b) { return lib.Meta(a).mtime > lib.Meta(b).mtime; });
        for (const auto& v : vs) rootOf_[v] = r;
        versionsByRoot_[r] = vs;
    }
    stacks.clear();
    if (!stackEdits || !filter_.text.empty()) return list;
    std::unordered_set<std::wstring> inList(list.begin(), list.end()), done;
    std::vector<std::wstring> out;
    for (const auto& u : list) {
        auto r = rootOf_.find(u);
        if (r == rootOf_.end()) {
            out.push_back(u);
            continue;
        }
        if (!done.insert(r->second).second) continue;
        const auto& vs = versionsByRoot_[r->second];
        std::vector<std::wstring> shown;
        for (const auto& v : vs)
            if (inList.count(v)) shown.push_back(v);
        if (expanded.count(r->second)) {
            for (const auto& v : shown) {
                out.push_back(v);
                stacks[v] = (int)vs.size();
            }
        } else if (!shown.empty()) {
            out.push_back(shown[0]);
            stacks[shown[0]] = (int)vs.size();
        }
    }
    return out;
}

std::vector<std::wstring> GalleryModel::Versions(const std::wstring& path) const {
    auto r = rootOf_.find(path);
    if (r == rootOf_.end()) return {};
    auto v = versionsByRoot_.find(r->second);
    return v == versionsByRoot_.end() ? std::vector<std::wstring>{} : v->second;
}

bool GalleryModel::IsExpanded(const std::wstring& path) const {
    auto r = rootOf_.find(path);
    return r != rootOf_.end() && expanded.count(r->second);
}

void GalleryModel::ToggleStack(const std::wstring& path) {
    auto r = rootOf_.find(path);
    if (r == rootOf_.end()) return;
    if (!expanded.erase(r->second)) expanded.insert(r->second);
    Recompute();
}

bool GalleryModel::CompareSelected() {
    std::vector<std::wstring> ts;
    for (const auto& t : Targets())
        if (IsImage(t)) ts.push_back(t);
    if (ts.size() == 2) {
        compare = std::make_pair(ts[1], ts[0]);
        return true;
    }
    if (ts.size() == 1) {
        if (const auto& o = lib.Meta(ts[0]).editedFrom; o && lib.Has(*o)) {
            compare = std::make_pair(*o, ts[0]);
            return true;
        }
        for (const auto& v : Versions(ts[0]))
            if (v != ts[0]) {
                compare = std::make_pair(v, ts[0]);
                return true;
            }
    }
    return false;
}

// ---- selection ----

int GalleryModel::IndexOf(const std::wstring& p) const {
    auto it = std::find(visible.begin(), visible.end(), p);
    return it == visible.end() ? -1 : (int)(it - visible.begin());
}

std::vector<std::wstring> GalleryModel::Selected() const {
    std::vector<std::wstring> out;
    for (const auto& v : visible)
        if (selection.count(v)) out.push_back(v);
    return out;
}

std::vector<std::wstring> GalleryModel::Targets() const {
    auto s = Selected();
    if (!s.empty()) return s;
    if (!focus.empty()) return {focus};
    return {};
}

void GalleryModel::Click(const std::wstring& u, bool shift, bool ctrl) {
    const int i = IndexOf(anchor), j = IndexOf(u);
    if (shift && i >= 0 && j >= 0) {
        if (!ctrl) selection.clear();
        for (int k = std::min(i, j); k <= std::max(i, j); ++k) selection.insert(visible[k]);
    } else if (ctrl) {
        if (!selection.erase(u)) selection.insert(u);
        anchor = u;
    } else {
        selection = {u};
        anchor = u;
    }
    focus = u;
}

void GalleryModel::Go(const std::wstring& u, bool extend) {
    const int i = IndexOf(anchor), j = IndexOf(u);
    if (extend && i >= 0 && j >= 0) {
        selection.clear();
        for (int k = std::min(i, j); k <= std::max(i, j); ++k) selection.insert(visible[k]);
    } else {
        selection = {u};
        anchor = u;
    }
    focus = u;
    if (!preview.empty()) preview = u;
}

void GalleryModel::Move(int d, bool extend) {
    if (visible.empty()) return;
    const std::wstring cur = !focus.empty() ? focus : selection.empty() ? L"" : *selection.begin();
    int i = cur.empty() ? -1 : IndexOf(cur);
    if (i < 0) i = d > 0 ? -1 : (int)visible.size();
    Go(visible[std::clamp(i + d, 0, (int)visible.size() - 1)], extend);
}

void GalleryModel::MoveRow(int dir, bool extend) {
    const std::wstring cur = !focus.empty() ? focus : selection.empty() ? L"" : *selection.begin();
    const int ci = IndexOf(cur);
    int ri = -1;
    if (layout == GalleryLayout::Justified && ci >= 0)
        for (int r = 0; r < (int)rows.size() && ri < 0; ++r)
            if (std::find(rows[r].items.begin(), rows[r].items.end(), ci) != rows[r].items.end()) ri = r;
    if (ri < 0) return Move(dir * (layout == GalleryLayout::List ? 1 : columns), extend);
    const int ni = ri + dir;
    if (ni < 0 || ni >= (int)rows.size()) return;
    const JustifiedRow& row = rows[ri];
    double x = 0;
    for (int k : row.items) {
        const double w = ClampedAspect(lib.Meta(visible[k]).Aspect()) * row.height;
        if (k == ci) {
            x += w / 2;
            break;
        }
        x += w;
    }
    double acc = 0;
    int target = rows[ni].items.back();
    for (int k : rows[ni].items) {
        acc += ClampedAspect(lib.Meta(visible[k]).Aspect()) * rows[ni].height;
        if (acc >= x) {
            target = k;
            break;
        }
    }
    Go(visible[target], extend);
}

void GalleryModel::SelectAll() { selection = std::set<std::wstring>(visible.begin(), visible.end()); }

void GalleryModel::Zoom(double f) {
    thumbSize = std::clamp(thumbSize * f, 110.0, 360.0);
    SaveViewOptions();
}

bool GalleryModel::RevealInGallery(const std::wstring& u) {
    if (!lib.Has(u) || std::find(lib.Paths().begin(), lib.Paths().end(), u) == lib.Paths().end()) return false;
    if (IndexOf(u) < 0) {
        if (auto r = rootOf_.find(u); r != rootOf_.end()) {  // hidden inside a collapsed stack
            expanded.insert(r->second);
            Recompute();
        }
    }
    if (IndexOf(u) < 0) {
        scope_ = Scope();
        filter_ = Filter();
        Recompute();
    }
    selection = {u};
    focus = anchor = u;
    return true;
}

void GalleryModel::Step(int d) {
    const int i = IndexOf(preview);
    if (i < 0 || visible.empty()) return;
    const int n = (int)visible.size();
    preview = visible[((i + d) % n + n) % n];
    selection = {preview};
    focus = preview;
}

// ---- tests ----

ATHER_TEST(gallery_justified_rows) {
    const std::vector<double> aspects = {1.5, 1, 2, 0.5, 1.5, 1, 1};
    auto rows = JustifiedRows(aspects, 1000, 200, 8);
    std::vector<int> all;
    for (const auto& r : rows) all.insert(all.end(), r.items.begin(), r.items.end());
    CHECK((all == std::vector<int>{0, 1, 2, 3, 4, 5, 6}));
    for (size_t i = 0; i + 1 < rows.size(); ++i) {
        double w = 8.0 * (rows[i].items.size() - 1);
        for (int k : rows[i].items) w += aspects[k] * rows[i].height;
        CHECK_NEAR(w, 1000, 0.5);  // full rows fill the width exactly
        CHECK(rows[i].height <= 200.5);
    }
    CHECK_NEAR(rows.back().height, 200, 1e-9);
}

ATHER_TEST(gallery_seeded_shuffle_is_stable) {
    SeededRandom a(42), b(42);
    for (int i = 0; i < 20; ++i) CHECK_EQ(a.Next(), b.Next());
}

ATHER_TEST(gallery_version_stacks_and_compare) {
    Library l(false);
    const std::wstring a = L"C:\\s\\orig.png", b = L"C:\\s\\edit1.png", c = L"C:\\s\\edit2.png", d = L"C:\\s\\other.png";
    l.Apply({{d, 2, 1}, {a, 1, 1}});
    l.NoteEdit(b, a, L"", L"", true);
    l.Apply({{b, 3, 1}, {d, 2, 1}, {a, 1, 1}});
    l.NoteEdit(c, b, L"", L"", true);
    l.Apply({{c, 4, 1}, {b, 3, 1}, {d, 2, 1}, {a, 1, 1}});
    GalleryModel m(l);
    m.Recompute();
    CHECK((std::set<std::wstring>(m.visible.begin(), m.visible.end()) == std::set<std::wstring>{c, d}));  // newest stands for the stack
    CHECK_EQ(m.stacks[c], 3);
    CHECK((m.Versions(a) == std::vector<std::wstring>{c, b, a}));
    m.ToggleStack(c);
    CHECK((std::set<std::wstring>(m.visible.begin(), m.visible.end()) == std::set<std::wstring>{a, b, c, d}));
    m.SetStackEdits(false);
    CHECK_EQ(m.visible.size(), 4u);
    CHECK(m.stacks.empty());
    m.stackEdits = true;
    m.selection = {c};
    m.focus = c;
    CHECK(m.CompareSelected());
    CHECK(m.compare && m.compare->first == b && m.compare->second == c);
    // A search shows every version (stacks only collapse while browsing).
    Filter f;
    f.text = L"orig";
    m.SetFilter(f);
    CHECK((m.visible == std::vector<std::wstring>{a}));
}

ATHER_TEST(gallery_related_matches_come_after_exact_ones) {
    Library l(false);
    const std::wstring chart = L"C:\\r\\chart.png", sales = L"C:\\r\\sales.png", other = L"C:\\r\\other.png";
    l.Apply({{chart, 3, 1}, {sales, 2, 1}, {other, 1, 1}});
    l.TestSetText(chart, L"Weekly revenue chart", 1);  // related to "sales graph" only
    l.TestSetText(sales, L"sales graph for Q3", 1);     // exact
    l.TestSetText(other, L"Team chat", 1);
    GalleryModel m(l);
    Filter f;
    f.text = L"sales graph";
    m.SetFilter(f);
    CHECK((m.visible == std::vector<std::wstring>{sales, chart}));  // exact first, although older
    CHECK_EQ(m.relatedCount, 1);
    f.text = L"statistics";
    m.SetFilter(f);
    CHECK(m.visible.size() == 2 && m.relatedCount == 2);  // no exact match ("chart" and "graph" are related)
}

ATHER_TEST(gallery_scopes_selection_and_navigation) {
    Library l(false);
    std::vector<FileStat> st;
    for (int i = 9; i >= 0; --i) st.push_back({L"C:\\g\\" + std::to_wstring(i) + (i == 3 ? L".mp4" : L".png"), 100.0 + i, 1000});
    l.Apply(st);
    l.AddTags({L"bug"}, {L"C:\\g\\1.png"});
    l.SetRating(4, {L"C:\\g\\2.png"});
    GalleryModel m(l);
    m.Recompute();
    CHECK_EQ(m.visible.size(), 10u);
    CHECK(m.visible.front() == L"C:\\g\\9.png");  // newest first
    m.SetScope(Scope::Of(ScopeKind::Rated));
    CHECK((m.visible == std::vector<std::wstring>{L"C:\\g\\2.png"}));
    m.SetScope(Scope::OfType(MediaType::Video));
    CHECK((m.visible == std::vector<std::wstring>{L"C:\\g\\3.mp4"}));
    m.SetScope(Scope::Of(ScopeKind::Uncategorized));
    CHECK_EQ(m.visible.size(), 9u);
    m.SetScope(Scope());
    m.Click(m.visible[2], false, false);
    m.Click(m.visible[5], true, false);  // shift: a range
    CHECK_EQ(m.selection.size(), 4u);
    m.Click(m.visible[7], false, true);  // ctrl: add one
    CHECK_EQ(m.selection.size(), 5u);
    m.Move(1, false);
    CHECK(m.focus == m.visible[8] && m.selection.size() == 1);
    m.columns = 4;
    m.layout = GalleryLayout::Grid;
    m.MoveRow(-1, false);
    CHECK(m.focus == m.visible[4]);
    m.SetSort(GallerySort::Oldest);
    CHECK(m.visible.front() == L"C:\\g\\0.png");
    m.SetSort(GallerySort::Name);
    CHECK(m.visible.front() == L"C:\\g\\0.png" && m.visible.back() == L"C:\\g\\9.png");
    Filter f;
    f.tags = {L"bug"};
    m.SetFilter(f);
    CHECK((m.visible == std::vector<std::wstring>{L"C:\\g\\1.png"}));
    CHECK(m.RevealInGallery(L"C:\\g\\5.png"));  // filtered out: the view widens
    CHECK(m.filter().IsEmpty() && m.focus == L"C:\\g\\5.png");
}

}  // namespace ather
