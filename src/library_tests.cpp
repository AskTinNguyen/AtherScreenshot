// Library, filters and indexer tests (LibraryTests.swift + Windows-specific ones).
#include "common.h"  // first: NOMINMAX

#include <objidl.h>

#include <algorithm>
#include <bit>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "library.h"
#include "media.h"
#include "output.h"
#include "selftest.h"
#include "smart.h"

namespace ather {
namespace {

namespace gp = Gdiplus;

std::wstring U(const std::wstring& n) { return L"C:\\tmp\\ather-lib-test\\" + n; }

BitmapPtr Solid(int w, int h, uint32_t argb, uint32_t stripe = 0) {
    auto b = Bitmap::Create(w, h);
    std::fill_n(b->Bits(), (size_t)w * h, argb);
    if (stripe)
        for (int y = 0; y < h; ++y) std::fill_n(b->Bits() + (size_t)y * w, w / 3, stripe);
    return b;
}

constexpr uint32_t kRed = 0xFFFF3B30, kBlack = 0xFF000000, kBlue = 0xFF0A84FF, kWhite = 0xFFFFFFFF;

std::unique_ptr<Library> Lib(const std::vector<FileStat>& stats) {
    auto l = std::make_unique<Library>(false);
    l->Apply(stats);
    return l;
}

bool Contains(const std::vector<std::wstring>& v, const std::wstring& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// A fake app screen: title bar, sidebar, text-like lines, a coloured button. `variant` moves things around.
BitmapPtr FakeScreen(int w, int h, int variant, uint32_t accent) {
    auto b = Bitmap::Create(w, h);
    {
        gp::Bitmap gb(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(b->Bits()));
        gp::Graphics g(&gb);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        const float s = w / 1000.f;
        gp::SolidBrush bg(variant % 2 ? gp::Color(255, 30, 30, 34) : gp::Color(255, 250, 250, 250));
        g.FillRectangle(&bg, 0, 0, w, h);
        gp::SolidBrush bar(gp::Color(255, 60, 60, 70));
        g.FillRectangle(&bar, 0.f, 0.f, (float)w, 40 * s);
        gp::SolidBrush side(gp::Color(255, 225, 228, 235));
        const float sideW = (180 + variant * 70) * s;
        g.FillRectangle(&side, 0.f, 40 * s, sideW, (float)h);
        gp::SolidBrush ink(variant % 2 ? gp::Color(255, 200, 200, 200) : gp::Color(255, 40, 40, 40));
        for (int i = 0; i < 14; ++i) {
            const float y = (70 + i * (30 + variant * 6)) * s;
            const float len = (300 + ((i * 37 + variant * 91) % 420)) * s;
            g.FillRectangle(&ink, sideW + 30 * s, y, len, 10 * s);
        }
        gp::SolidBrush acc(gp::Color(255, (BYTE)(accent >> 16), (BYTE)(accent >> 8), (BYTE)accent));
        g.FillRectangle(&acc, (600 + variant * 40) * s, (500 - variant * 60) * s, 220 * s, 60 * s);
    }
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) b->Bits()[i] |= 0xFF000000u;
    return b;
}

void WriteText(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr = 0;
    WriteFile(f, text.data(), (DWORD)text.size(), &wr, nullptr);
    CloseHandle(f);
}

std::string ReadText(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::string s(GetFileSize(f, nullptr), '\0');
    DWORD rd = 0;
    ReadFile(f, s.data(), (DWORD)s.size(), &rd, nullptr);
    CloseHandle(f);
    return s;
}

}  // namespace

// Developer tool: `AtherScreenshot.exe --feature-stats <folder>` prints how the feature distance behaves on real
// screenshots (rescaled copies vs. different captures), for calibrating the thresholds. Reads files only.
int FeatureStats(const std::wstring& folder) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct F {
        std::vector<float> f, copy;
        uint64_t h = 0, hc = 0;
        int w = 0, ht = 0;
    };
    std::vector<F> fs;
    for (const auto& st : Library::ListCaptures(folder)) {
        if (MediaTypeOf(st.path) != MediaType::Image || fs.size() >= 80) continue;
        int w = 0, h = 0;
        auto img = LoadImageScaled(st.path, 512, &w, &h);
        auto full = LoadImageScaled(st.path, 0);
        if (!img || !full) continue;
        // A copy saved at half size, with a little noise like a re-encode.
        auto half = Resample(*full, std::max(1, full->Width() / 2), std::max(1, full->Height() / 2));
        uint32_t seed = 12345;
        for (size_t i = 0, n = (size_t)half->Width() * half->Height(); i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int d = (int)(seed >> 29) - 4;
            uint32_t p = half->Bits()[i];
            auto ch = [&](int s) { return (uint32_t)std::clamp((int)((p >> s) & 255) + d, 0, 255) << s; };
            half->Bits()[i] = 0xFF000000u | ch(16) | ch(8) | ch(0);
        }
        const int m = std::max(half->Width(), half->Height());
        auto halfThumb = m > 512 ? Resample(*half, std::max(1, half->Width() * 512 / m), std::max(1, half->Height() * 512 / m)) : half;
        fs.push_back({indexer::Feature(*img), indexer::Feature(*halfThumb), indexer::DHash(*img), indexer::DHash(*halfThumb), w, h});
    }
    auto pct = [](std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
    };
    auto halves = [](const std::vector<float>& a, const std::vector<float>& b, double* hist, double* dct) {
        double x = 0, y = 0;
        for (size_t i = 0; i < 64; ++i) x += (double)a[i] * b[i];
        for (size_t i = 64; i < 64 + 255 && i < a.size(); ++i) y += (double)a[i] * b[i];
        *hist = x;
        *dct = y;
    };
    std::vector<double> copyD, pairD, pairH, pairDct, copyH, copyDct;
    std::vector<int> copyBits, closeBits;
    int dupPairs = 0;
    for (size_t i = 0; i < fs.size(); ++i) {
        copyD.push_back(indexer::FeatureDistance(fs[i].f, fs[i].copy));
        double hh, dd;
        halves(fs[i].f, fs[i].copy, &hh, &dd);
        copyH.push_back(hh);
        copyDct.push_back(dd);
        copyBits.push_back(std::popcount(fs[i].h ^ fs[i].hc));
        for (size_t j = i + 1; j < fs.size(); ++j) {
            const double d = indexer::FeatureDistance(fs[i].f, fs[j].f);
            pairD.push_back(d);
            halves(fs[i].f, fs[j].f, &hh, &dd);
            pairH.push_back(hh);
            pairDct.push_back(dd);
            const int bits = std::popcount(fs[i].h ^ fs[j].h);
            if (d <= 0.3) closeBits.push_back(bits);
            if (d <= 0.3 && bits <= 10) ++dupPairs;
        }
    }
    char line[512];
    auto row = [&](const char* name, const std::vector<double>& v) {
        sprintf_s(line, "%-22s n=%4zu  min %.3f  p5 %.3f  p25 %.3f  median %.3f  p75 %.3f  max %.3f\n", name, v.size(), pct(v, 0), pct(v, 0.05),
                  pct(v, 0.25), pct(v, 0.5), pct(v, 0.75), pct(v, 1));
        test::Out(line);
    };
    sprintf_s(line, "%zu screenshots from %s\n", fs.size(), ToUtf8(folder).c_str());
    test::Out(line);
    row("copy distance", copyD);
    row("copy hist dot", copyH);
    row("copy dct dot", copyDct);
    row("pair distance", pairD);
    row("pair hist dot", pairH);
    row("pair dct dot", pairDct);
    std::vector<double> cb(copyBits.begin(), copyBits.end()), xb(closeBits.begin(), closeBits.end());
    row("copy dhash bits", cb);
    row("close-pair dhash bits", xb);
    sprintf_s(line, "pairs flagged as duplicates (d<=0.3 and dhash<=10): %d\n", dupPairs);
    test::Out(line);
    test::FlushOut();
    CoUninitialize();
    return 0;
}

ATHER_TEST(library_auto_tag_setting_tags_everything_once) {
    Library l(false);
    const std::wstring a = L"C:\\t\\slack.png", b = L"C:\\t\\phone.png", c = L"C:\\t\\plain.png";
    l.Apply({{c, 3, 1}, {b, 2, 1}, {a, 1, 1}});
    l.TestSetApp(a, L"Slack");
    l.TestSetText(b, L"Sign in to continue", 1);
    l.DismissSuggestion(L"login", {b});  // turned down: never applied
    l.SetAutoTag(true);
    CHECK((l.Meta(a).tags == std::vector<std::wstring>{L"chat"}));
    CHECK(l.Meta(b).tags.empty());
    CHECK(l.Meta(c).tags.empty());
    l.RemoveTag(L"chat", {a});
    l.SetAutoTag(true);  // already on: not applied again
    CHECK(l.Meta(a).tags.empty());
}

ATHER_TEST(library_filter_matching) {
    ItemMeta m;
    m.w = 3000;
    m.h = 1000;
    m.tags = {L"Bug", L"ui"};
    m.rating = 4;
    m.app = L"Safari";
    m.text = L"Invoice total 42";
    m.mtime = NowEpoch();
    m.size = 500'000;
    m.colors = {{250, 10, 10, 0.5}, {20, 20, 20, 0.5}};
    const std::wstring url = U(L"a.png");
    auto f = [](auto set) {
        Filter x;
        set(x);
        return x;
    };
    CHECK(f([](Filter& x) { x.text = L"invoice 42"; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.text = L"safari bug"; }).Matches(url, m));  // app and tags are searchable too
    CHECK(f([](Filter& x) { x.text = L"receipt"; }).Matches(url, m));     // "invoice" suggests the receipt tag
    CHECK(!f([](Filter& x) { x.text = L"terminal"; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.tags = {L"bug", L"UI"}; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.tags = {L"bug", L"mobile"}; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.tags = {L"bug", L"mobile"}; x.anyTag = true; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.untagged = true; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.minRating = 4; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.minRating = 5; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.color = L"#FF0000"; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.color = L"#0A84FF"; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.shape = ShapeFilter::Wide; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.shape = ShapeFilter::Portrait; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.date = DateFilter::Today; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.size = SizeFilter::Medium; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.apps = {L"Safari"}; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.types = {MediaType::Video}; }).Matches(url, m));
    CHECK(f([](Filter& x) { x.minWidth = 1920; x.minHeight = 1000; }).Matches(url, m));
    CHECK(!f([](Filter& x) { x.minWidth = 1920; x.minHeight = 1080; }).Matches(url, m));
    CHECK_EQ(f([](Filter& x) { x.text = L"x"; x.minRating = 2; }).ActiveCount(), 2);
}

ATHER_TEST(library_smart_folder_round_trip) {
    Filter f;
    f.text = L"error";
    f.types = {MediaType::Image};
    f.tags = {L"bug"};
    f.minRating = 3;
    f.color = L"#FF3B30";
    f.shape = ShapeFilter::Tall;
    f.date = DateFilter::Week;
    bool ok = false;
    Json j = Json::Parse(f.ToJson().Dump(), &ok);
    CHECK(ok);
    CHECK(Filter::FromJson(j) == f);
}

ATHER_TEST(library_tags_collections_and_renames) {
    auto l = Lib({{U(L"c.gif"), 3, 30}, {U(L"b.png"), 2, 20}, {U(L"a.png"), 1, 10}});
    const std::wstring a = U(L"a.png"), b = U(L"b.png");
    l->AddTags({L"Bug", L"bug", L" ui "}, {a, b});
    CHECK((l->Meta(a).tags == std::vector<std::wstring>{L"Bug", L"ui"}));  // de-duplicated case-insensitively, trimmed
    CHECK(l->AllTags().front().first == L"Bug" && l->AllTags().front().second == 2);
    l->RenameTag(L"bug", L"Defect");
    CHECK((l->Meta(b).tags == std::vector<std::wstring>{L"Defect", L"ui"}));
    auto c = l->CreateCollection(L"Release notes");
    l->SetAutoTags(c.id, {L"release"});
    l->AddToCollection({a}, c.id);
    CHECK_EQ(l->CountIn(c.id), 1);
    CHECK(Contains(l->Meta(a).tags, L"release"));  // collection auto tags
    l->SetRating(9, {a});
    CHECK_EQ(l->Meta(a).rating, 5);
    const std::wstring moved = U(L"renamed.png");
    l->Moved(a, moved);
    CHECK_EQ(l->Meta(moved).rating, 5);
    CHECK(Contains(l->Meta(moved).collections, c.id));
    CHECK(l->Meta(a).tags.empty());
    l->DeleteCollection(c.id);
    CHECK(l->Meta(moved).collections.empty());
}

ATHER_TEST(library_indexer_and_duplicates) {
    auto red = Solid(400, 300, kRed, kBlack), redCopy = Solid(800, 600, kRed, kBlack), blue = Solid(400, 300, kBlue, kWhite);
    auto pal = indexer::Palette(*red);
    CHECK_EQ(pal.size(), 2u);
    CHECK_NEAR(pal[0].ratio, 2.0 / 3, 0.05);
    CHECK(Filter::Distance(pal[0].r, pal[0].g, pal[0].b, 255, 59, 48) < 60);
    const uint64_t h1 = indexer::DHash(*red), h2 = indexer::DHash(*redCopy), h3 = indexer::DHash(*blue);
    CHECK(std::popcount(h1 ^ h2) <= 4);
    CHECK(std::popcount(h1 ^ h3) > 4);

    auto l = Lib({{U(L"b.png"), 3, 1}, {U(L"r2.png"), 2, 1}, {U(L"r1.png"), 1, 1}});
    l->TestSetHash(U(L"r1.png"), h1);
    l->TestSetHash(U(L"r2.png"), h2);
    l->TestSetHash(U(L"b.png"), h3);
    l->TestSetFeature(U(L"r1.png"), indexer::Feature(*red));
    l->TestSetFeature(U(L"r2.png"), indexer::Feature(*redCopy));
    l->TestSetFeature(U(L"b.png"), indexer::Feature(*blue));
    auto groups = l->DuplicateGroups();
    CHECK_EQ(groups.size(), 1u);
    if (!groups.empty()) {
        CHECK_EQ(groups[0].size(), 2u);
        CHECK(groups[0][0] == U(L"r2.png"));  // newest first
    }
    auto sim = l->Similar(U(L"r1.png"));
    CHECK(sim.size() == 2 && sim[0].first == U(L"r2.png"));
    CHECK(Library::SimilarityPercent(sim[0].second) > 90);
}

ATHER_TEST(library_feature_calibration) {
    // A rescaled copy is close; other screens of the same layout are apart; a photo is far. (Real screenshots are
    // calibrated with --feature-stats; see indexer::Feature.)
    auto a = FakeScreen(1600, 1000, 0, 0x0A84FF), aSmall = FakeScreen(800, 500, 0, 0x0A84FF);
    auto aShifted = FakeScreen(1600, 1000, 0, 0xFF3B30);  // same screen, different button colour
    const float dCopy = indexer::FeatureDistance(indexer::Feature(*a), indexer::Feature(*aSmall));
    const float dTweak = indexer::FeatureDistance(indexer::Feature(*a), indexer::Feature(*aShifted));
    float dFar = 9, dFarMin = 9;
    for (int v = 1; v < 4; ++v) {
        auto o = FakeScreen(1600, 1000, v, v == 2 ? 0x34C759 : 0xFF9500);
        dFarMin = std::min(dFarMin, indexer::FeatureDistance(indexer::Feature(*a), indexer::Feature(*o)));
    }
    auto photo = Solid(1600, 1000, 0xFF3A5F2B, 0xFF9DC3E6);  // nothing like a screen
    dFar = indexer::FeatureDistance(indexer::Feature(*a), indexer::Feature(*photo));
    test::Note("copy " + std::to_string(dCopy) + "  tweak " + std::to_string(dTweak) + "  other screens >= " +
               std::to_string(dFarMin) + "  photo " + std::to_string(dFar));
    CHECK(dCopy < 0.1f);
    CHECK(dTweak < 0.3f);    // still a duplicate candidate: same layout
    CHECK(dFarMin > 0.15f);  // other screens of the same fake app layout: clearly apart from copies
    CHECK(dFar > 0.9f);
    CHECK(Library::SimilarityPercent(dCopy) > 90);
}

ATHER_TEST(library_path_containment_is_component_wise) {
    CHECK(Library::IsInside(L"C:\\Users\\a\\Pictures\\Ather\\2026-10\\x.png", L"C:\\Users\\a\\Pictures\\Ather"));
    CHECK(!Library::IsInside(L"C:\\Users\\a\\Pictures\\Ather Old\\x.png", L"C:\\Users\\a\\Pictures\\Ather"));
    CHECK(!Library::IsInside(L"C:\\Caps2\\x.png", L"C:\\Caps"));
    CHECK(Library::IsInside(L"C:\\Users\\a\\Pictures\\Ather\\x.png", L"C:\\Users\\a\\Pictures\\Ather\\"));
    CHECK(Library::IsInside(L"c:\\users\\A\\pictures\\ather\\x.png", L"C:\\Users\\a\\Pictures\\Ather"));  // case-insensitive
}

ATHER_TEST(library_decoding_tolerates_missing_keys) {
    ItemMeta m = ItemMeta::FromJson(Json::Parse(R"({"rating": 4, "tags": ["bug"]})"));
    CHECK_EQ(m.rating, 4);
    CHECK((m.tags == std::vector<std::wstring>{L"bug"}));
    CHECK_EQ(m.indexed, 0);
    Filter f = Filter::FromJson(Json::Parse(R"({"text": "x", "futureField": 1})"));
    CHECK(f.text == L"x");
    ItemMeta w = ItemMeta::FromJson(Json::Parse(R"({"rating": "five", "w": [1], "text": 3, "colors": {}})"));  // wrong types
    CHECK(w.rating == 0 && w.w == 0 && !w.text && w.colors.empty());
}

ATHER_TEST(library_changed_file_is_re_ocrd) {
    Library l(false);
    const std::wstring u = L"C:\\tmp\\ather-review\\a.png";
    l.Apply({{u, 100, 10}});
    l.TestSetText(u, L"old text", 1);
    l.Apply({{u, 100, 10}});
    CHECK(l.Meta(u).text == std::optional<std::wstring>(L"old text"));  // unchanged file keeps its OCR
    l.Apply({{u, 200, 10}});
    CHECK(!l.Meta(u).text);  // modified file is OCR'd again
    CHECK_EQ(l.Meta(u).indexed, 0);
}

ATHER_TEST(library_prunes_only_inside_the_captures_folder) {
    Library l(false);
    l.SetFolder(L"C:\\Caps");
    l.Apply({{L"C:\\Caps\\a.png", 1, 1}, {L"C:\\Caps2\\b.png", 1, 1}});
    l.Apply({});  // the folder is empty now
    CHECK(!l.Has(L"C:\\Caps\\a.png"));
    CHECK(l.Has(L"C:\\Caps2\\b.png"));  // not inside C:\Caps: left alone
}

ATHER_TEST(library_duplicate_cache_invalidates_on_change) {
    Library l(false);
    const std::wstring a = L"C:\\tmp\\r\\a.png", b = L"C:\\tmp\\r\\b.png";
    l.Apply({{b, 2, 1}, {a, 1, 1}});
    l.TestSetHash(a, 0xFF);
    l.TestSetHash(b, 0x0F0000000000ull);
    CHECK(l.DuplicateGroups().empty());
    l.TestSetHash(b, 0xFF);
    CHECK_EQ(l.DuplicateGroups().size(), 1u);
}

ATHER_TEST(library_edited_copy_inherits_metadata) {
    Library l(false);
    const std::wstring a = L"C:\\tmp\\r\\a.png", b = L"C:\\tmp\\r\\b.png", d = L"C:\\tmp\\r\\d.png";
    l.Apply({{a, 1, 1}});
    l.AddTags({L"bug"}, {a});
    l.SetRating(4, {a});
    l.SetComment(L"check", a);
    l.TestSetApp(a, L"Safari");
    auto c = l.CreateCollection(L"Sprint");
    l.AddToCollection({a}, c.id);
    l.NoteEdit(b, a, L"", L"", true);
    l.Apply({{b, 2, 1}, {a, 1, 1}});
    const ItemMeta& m = l.Meta(b);
    CHECK((m.tags == std::vector<std::wstring>{L"bug", L"edited"}));
    CHECK_EQ(m.rating, 4);
    CHECK(m.comment == L"check");
    CHECK(m.app == L"Safari");
    CHECK((m.collections == std::vector<std::wstring>{c.id}));
    CHECK(m.editedFrom == std::optional<std::wstring>(a));
    // Saving straight from a fresh capture without changes is not an edit.
    l.NoteEdit(d, L"", L"Notepad", L"", false);
    l.Apply({{d, 3, 1}, {b, 2, 1}, {a, 1, 1}});
    CHECK(l.Meta(d).tags.empty());
    CHECK(l.Meta(d).app == L"Notepad");
}

ATHER_TEST(library_collage_keeps_shared_tags) {
    Library l(false);
    const std::wstring a = L"C:\\t\\a.png", b = L"C:\\t\\b.png", out = L"C:\\t\\collage.png";
    l.Apply({{b, 2, 1}, {a, 1, 1}});
    l.AddTags({L"bug", L"ui"}, {a});
    l.AddTags({L"Bug", L"web"}, {b});
    l.NoteEdit(out, L"", L"", L"", true, {a, b}, true);
    l.Apply({{out, 3, 1}, {b, 2, 1}, {a, 1, 1}});
    CHECK((l.Meta(out).tags == std::vector<std::wstring>{L"bug", L"collage"}));
    CHECK((l.Meta(out).includes == std::vector<std::wstring>{a, b}));
}

ATHER_TEST(library_reads_a_mac_library_and_round_trips) {
    // Written by Swift's JSONEncoder: escaped slashes, a 64-bit hash, keys this version doesn't know.
    const std::string mac = R"({"version":1,"items":{"\/Users\/tin\/Pictures\/AtherScreenshot\/2026-10\/a.png":{"mtime":1759561200.25,)"
                            R"("size":751234,"w":1800,"h":1000,"tags":["bug"],"rating":3,"comment":"","collections":["8F0E3C0A-1B2C-4D5E-8F90-ABCDEF012345"],)"
                            R"("app":"Safari","window":"Checkout","text":"Pay now \u00e9","colors":[{"r":250,"g":10,"b":10,"ratio":0.42}],)"
                            R"("dhash":18446744073709551615,"indexed":1,"includes":[],"dismissed":["chat"],"futureKey":{"x":1}}},)"
                            R"("collections":[{"id":"8F0E3C0A-1B2C-4D5E-8F90-ABCDEF012345","name":"Sprint 42","autoTags":["sprint"]}],)"
                            R"("smartFolders":[{"id":"11111111-2222-3333-4444-555555555555","name":"Red things","filter":{"color":"#FF0000","types":["image"],"date":"week"}}]})";
    WriteText(SupportFolder() + L"\\library.json", mac);
    {
        Library l(true);
        l.LoadIfNeeded();
        const std::wstring p = L"/Users/tin/Pictures/AtherScreenshot/2026-10/a.png";
        CHECK(l.Has(p));
        const ItemMeta& m = l.Meta(p);
        CHECK(m.app == L"Safari" && m.rating == 3 && m.w == 1800);
        CHECK(m.dhash == std::optional<uint64_t>(18446744073709551615ull));
        CHECK(m.text == std::optional<std::wstring>(L"Pay now \u00e9"));
        CHECK(m.colors.size() == 1 && m.colors[0].r == 250);
        CHECK_EQ(l.Collections().size(), 1u);
        CHECK(l.SmartFolders().size() == 1 && l.SmartFolders()[0].filter.date == DateFilter::Week);
        // A change, then a save: the Mac app must still be able to read the result.
        l.SetComment(L"seen on Windows", p);
        l.Flush();
    }
    bool ok = false;
    Json back = Json::Parse(ReadText(SupportFolder() + L"\\library.json"), &ok);
    CHECK(ok);
    const Json& item = back["items"]["/Users/tin/Pictures/AtherScreenshot/2026-10/a.png"];
    CHECK(item.IsObject());
    CHECK(item["dhash"].UInt() == 18446744073709551615ull);
    CHECK(item["comment"].Str() == "seen on Windows");
    CHECK(item["mtime"].Num() == 1759561200.25);
    CHECK(back["collections"][0]["name"].Str() == "Sprint 42");
    CHECK(back["smartFolders"][0]["filter"]["color"].Str() == "#FF0000");
    CHECK(back["version"].Int() == 1);
    DeleteFileW((SupportFolder() + L"\\library.json").c_str());
}

ATHER_TEST(library_unreadable_file_is_kept_aside) {
    const std::wstring file = SupportFolder() + L"\\library.json";
    WriteText(file, "{ this is not json");
    {
        Library l(true);
        l.LoadIfNeeded();
        CHECK(l.Paths().empty());
        CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);  // moved, not overwritten
        l.Flush();  // nothing pending: writes nothing
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((SupportFolder() + L"\\library.unreadable-*.json").c_str(), &fd);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadText(SupportFolder() + L"\\" + fd.cFileName) == "{ this is not json");
        DeleteFileW((SupportFolder() + L"\\" + fd.cFileName).c_str());
        FindClose(h);
    }
}

ATHER_TEST(library_imports_legacy_ocr_index) {
    const std::wstring idx = SupportFolder() + L"\\ocr-index.txt";
    const std::wstring line = L"c:\\caps\\2026-01\\shot.png\tHello invoice\n";
    HANDLE f = CreateFileW(idx.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD wr;
    WriteFile(f, line.data(), (DWORD)(line.size() * 2), &wr, nullptr);
    CloseHandle(f);
    DeleteFileW((SupportFolder() + L"\\library.json").c_str());
    Library l(true);
    l.SetFolder(L"C:\\Caps");
    l.Apply({{L"C:\\Caps\\2026-01\\Shot.png", 5, 5}});
    CHECK(l.Meta(L"C:\\Caps\\2026-01\\Shot.png").text == std::optional<std::wstring>(L"Hello invoice"));
    DeleteFileW(idx.c_str());
}

ATHER_TEST(library_lists_and_indexes_real_files) {
    const std::wstring dir = test::TempDir();
    CreateDirectoryW((dir + L"\\2026-10").c_str(), nullptr);
    CreateDirectoryW((dir + L"\\.hidden").c_str(), nullptr);
    auto img = FakeScreen(640, 400, 0, 0x0A84FF);
    CHECK(SavePng(*img, dir + L"\\2026-10\\one.png"));
    Sleep(20);
    CHECK(SavePng(*img, dir + L"\\two.png"));
    CHECK(SavePng(*img, dir + L"\\.hidden\\skip.png"));
    WriteText(dir + L"\\notes.txt", "not media");
    auto list = Library::ListCaptures(dir);
    CHECK_EQ(list.size(), 2u);
    if (list.size() == 2) CHECK(FileNameOf(list[0].path) == L"two.png");  // newest first
    IndexResult r = indexer::Index(dir + L"\\two.png", false);
    CHECK(r.w == 640 && r.h == 400);
    CHECK(r.dhash.has_value() && !r.feature.empty() && !r.colors.empty());
    CHECK(!r.text.has_value());
}

}  // namespace ather
