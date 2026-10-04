#pragma once
#include <optional>
#include <set>
#include <unordered_map>

#include "library.h"

namespace ather {

// What the gallery is showing: a library view, a type, a collection, a smart folder, or "similar to".
enum class ScopeKind { All, Recent, Rated, Uncategorized, Duplicates, Type, Collection, Smart, Similar };
struct Scope {
    ScopeKind kind = ScopeKind::All;
    MediaType type = MediaType::Image;  // Type
    std::wstring id;                    // Collection / Smart: id; Similar: path
    bool operator==(const Scope&) const = default;
    static Scope Of(ScopeKind k) { return {k, MediaType::Image, L""}; }
    static Scope OfType(MediaType t) { return {ScopeKind::Type, t, L""}; }
    static Scope OfCollection(const std::wstring& id) { return {ScopeKind::Collection, MediaType::Image, id}; }
    static Scope OfSmart(const std::wstring& id) { return {ScopeKind::Smart, MediaType::Image, id}; }
    static Scope SimilarTo(const std::wstring& path) { return {ScopeKind::Similar, MediaType::Image, path}; }
};

enum class GalleryLayout { Justified, Grid, List };
enum class GallerySort { Newest, Oldest, Name, Size, Dimensions, Rating, Random };
const wchar_t* SortLabel(GallerySort s);

// Rows of equal height that fill the width exactly (the last row keeps the target height). `items` index
// into the list that was laid out.
struct JustifiedRow {
    std::vector<int> items;
    double height = 0;
};
std::vector<JustifiedRow> JustifiedRows(const std::vector<double>& aspects, double width, double target, double spacing);
double ClampedAspect(double a);  // panoramas and long pages can't swallow a row

// Deterministic shuffle for the "Random" sort (the same seed gives the same order).
struct SeededRandom {
    uint64_t state;
    explicit SeededRandom(uint64_t seed) : state(seed) {}
    uint64_t Next();
};

// The gallery's state and logic, without any drawing. Port of GalleryModel in Gallery.swift.
class GalleryModel {
public:
    explicit GalleryModel(Library& lib);
    Library& lib;

    // view options (remembered in gallery.ini)
    GalleryLayout layout = GalleryLayout::Justified;
    double thumbSize = 190;
    bool showSidebar = false, showInspector = false, inspectorDiscovered = false, showNames = false;
    bool stackEdits = true;
    void LoadViewOptions();
    void SaveViewOptions() const;
    void SetShowInspector(bool on);

    const Scope& scope() const { return scope_; }
    void SetScope(const Scope& s);
    const Filter& filter() const { return filter_; }
    void SetFilter(const Filter& f);
    GallerySort sort() const { return sort_; }
    void SetSort(GallerySort s);
    void SetStackEdits(bool on);
    void Shuffle();
    std::wstring Title() const;
    bool InScope(const std::wstring& path, const ItemMeta& m, double now) const;

    // Results of the last Recompute.
    std::vector<std::wstring> visible;
    std::vector<std::vector<std::wstring>> groups;     // duplicates scope
    std::unordered_map<std::wstring, float> distances;  // similar scope
    std::unordered_map<std::wstring, int> stacks;       // shown capture -> versions in its stack
    int relatedCount = 0;                               // search results that only matched related words
    void Recompute();

    // Version stacks: each original with its edits ("edited from" links), newest first.
    std::vector<std::wstring> Versions(const std::wstring& path) const;
    bool IsExpanded(const std::wstring& path) const;
    void ToggleStack(const std::wstring& path);
    std::set<std::wstring> expanded;  // stack roots shown open

    // Selection
    std::set<std::wstring> selection;
    std::wstring focus, anchor;
    std::vector<std::wstring> Selected() const;  // in visible order
    std::vector<std::wstring> Targets() const;   // the selection, or the focused capture
    std::wstring Single() const { return selection.size() == 1 ? *selection.begin() : L""; }
    void Click(const std::wstring& path, bool shift, bool ctrl);
    void Move(int delta, bool extend);
    void MoveRow(int dir, bool extend);  // by visual row in the justified layout, by columns elsewhere
    void SelectAll();
    void Zoom(double factor);
    bool RevealInGallery(const std::wstring& path);  // widens the view if it's filtered out; false if gone

    // Preview and compare
    std::wstring preview;
    bool slideshow = false;
    void Step(int delta);
    std::optional<std::pair<std::wstring, std::wstring>> compare;  // before, after
    // Two selected images, or one edit (with its original, or else its previous version). False: nothing to compare.
    bool CompareSelected();

    // Layout feedback from the view, for keyboard navigation.
    int columns = 4;
    std::vector<JustifiedRow> rows;  // indices into `visible`

    static bool IsImage(const std::wstring& path) { return MediaTypeOf(path) == MediaType::Image; }

private:
    std::vector<std::wstring> CollapseStacks(const std::vector<std::wstring>& list);
    void Go(const std::wstring& path, bool extend);
    int IndexOf(const std::wstring& path) const;

    Scope scope_;
    Filter filter_;
    GallerySort sort_ = GallerySort::Newest;
    uint64_t seed_ = 0x9E3779B97F4A7C15ull;
    std::unordered_map<std::wstring, std::vector<std::wstring>> versionsByRoot_;  // stacks with 2+ versions
    std::unordered_map<std::wstring, std::wstring> rootOf_;
};

}  // namespace ather
