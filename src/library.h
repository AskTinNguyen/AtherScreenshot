#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "common.h"
#include "json.h"

namespace ather {

// The gallery's metadata layer: every file in the captures folder plus what the user and the indexer know
// about it (tags, rating, comment, collections, source app, OCR text, palette, perceptual hash, feature
// vector). Stored in %APPDATA%\AtherScreenshot\library.json, in the same format as the Mac app's, so either
// app reads the other's file (paths simply don't match across machines). Image files are never modified.

enum class MediaType { Image, Gif, Video };
MediaType MediaTypeOf(const std::wstring& path);
const wchar_t* MediaTypeName(MediaType t);   // "image" | "gif" | "video" (the JSON spelling)
const wchar_t* MediaTypeLabel(MediaType t);  // Screenshots | GIFs | Videos
const wchar_t* MediaTypeWords(MediaType t);  // searchable words
bool IsMediaFile(const std::wstring& path);
// The one list of types this app opens: "png", "jpg"… (pictures) and "mp4", "mov"… (videos).
std::vector<std::wstring> MediaExtensions(bool pictures, bool videos);
// An open-dialog filter for them ("Pictures and videos", then each kind on its own, then all files).
std::wstring MediaFilter(bool pictures, bool videos);

struct PaletteColor {
    uint8_t r = 0, g = 0, b = 0;
    double ratio = 0;
    std::wstring Hex() const;
};

struct ItemMeta {
    double mtime = 0;  // seconds since 1970
    int64_t size = 0;
    int w = 0, h = 0;
    std::optional<double> duration;
    std::vector<std::wstring> tags;
    int rating = 0;
    std::wstring comment;
    std::vector<std::wstring> collections;  // collection ids (UUID strings)
    std::wstring app, window;
    std::optional<std::wstring> text;  // OCR; unset until indexed
    std::vector<PaletteColor> colors;
    std::optional<uint64_t> dhash;
    int indexed = 0;                        // indexer version that produced the technical fields
    std::optional<std::wstring> editedFrom;  // path of the capture this one was edited from
    std::vector<std::wstring> includes;     // screenshots placed into this one (image layers, collages)
    std::vector<std::wstring> dismissed;    // suggested tags turned down

    double Aspect() const { return w > 0 && h > 0 ? (double)w / h : 16.0 / 10; }
    Json ToJson() const;
    static ItemMeta FromJson(const Json& j);  // tolerates missing keys and wrong types
};

struct LibCollection {
    std::wstring id, name;
    std::vector<std::wstring> autoTags;  // added to everything dropped into the collection
};

enum class ShapeFilter { Any, Landscape, Portrait, Square, Wide, Tall };
enum class DateFilter { Any, Today, Week, Month, Year };
enum class SizeFilter { Any, Small, Medium, Large, Huge };
const wchar_t* ShapeLabel(ShapeFilter s);
const wchar_t* DateLabel(DateFilter d);
const wchar_t* SizeLabel(SizeFilter s);
bool ShapeMatches(ShapeFilter s, double aspect);
bool SizeMatches(SizeFilter s, int64_t bytes);
double DateSince(DateFilter d, double now);  // epoch seconds

enum class MatchKind { None, Exact, Related };

// The search bar and what a smart folder saves.
struct Filter {
    std::wstring text;
    std::vector<MediaType> types;
    std::vector<std::wstring> tags;
    bool anyTag = false;
    bool untagged = false;
    int minRating = 0;
    std::wstring color;  // "#RRGGBB" or empty
    ShapeFilter shape = ShapeFilter::Any;
    DateFilter date = DateFilter::Any;
    SizeFilter size = SizeFilter::Any;
    std::vector<std::wstring> apps;
    int minWidth = 0, minHeight = 0;

    bool operator==(const Filter&) const = default;
    bool IsEmpty() const { return *this == Filter(); }
    int ActiveCount() const;
    bool Matches(const std::wstring& path, const ItemMeta& m) const { return Match(path, m) != MatchKind::None; }
    // Related: the search text only matched through related words.
    MatchKind Match(const std::wstring& path, const ItemMeta& m, double now = 0) const;

    Json ToJson() const;
    static Filter FromJson(const Json& j);
    static bool Rgb(const std::wstring& hex, uint8_t* r, uint8_t* g, uint8_t* b);
    // "Redmean" color distance: cheap and close to perceptual. 0 … ~765.
    static double Distance(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2);
};

struct SmartFolder {
    std::wstring id, name;
    Filter filter;
};

struct FileStat {
    std::wstring path;
    double mtime = 0;
    int64_t size = 0;
};

// What the indexer learns from one file (computed off the UI thread).
struct IndexResult {
    int w = 0, h = 0;
    std::optional<double> duration;
    std::vector<PaletteColor> colors;
    std::optional<uint64_t> dhash;
    std::optional<std::wstring> text;
    std::vector<float> feature;  // empty: none
};

namespace indexer {
IndexResult Index(const std::wstring& path, bool ocr);
uint64_t DHash(const Bitmap& img);                            // 64-bit difference hash of a 9×8 grayscale thumbnail
std::vector<PaletteColor> Palette(const Bitmap& img, int k = 6);  // k-means on 48×48, biggest share first
std::vector<float> Feature(const Bitmap& img);               // see the calibration note in library.cpp
float FeatureDistance(const std::vector<float>& a, const std::vector<float>& b);
}  // namespace indexer

class Library {
public:
    // 2: videos are measured upright (phone videos stored sideways with a rotation), so they are indexed again.
    static constexpr int kIndexVersion = 2;
    static Library& Shared();

    // `persists: false` gives tests an in-memory store that never touches disk.
    explicit Library(bool persists = true);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    void SetFolder(const std::wstring& capturesFolder);
    const std::wstring& Folder() const { return folder_; }

    // Loads library.json once. Every mutation goes through here first, so nothing can save an empty
    // store over the real one.
    void LoadIfNeeded();
    // Rescans the captures folder on a worker thread, then indexes what's new. `done` runs on the UI thread.
    void Refresh(std::function<void()> done = nullptr);
    // Reconciles metadata with a listing: new files get entries, changed files get re-indexed, files gone
    // from the captures folder lose theirs. `stats` is newest first.
    // `complete` false: part of the folder couldn't be listed (drive missing, folder renamed, no access), so
    // nothing is pruned — an unreadable folder must never look like an empty one.
    void Apply(const std::vector<FileStat>& stats, bool complete = true);
    // Recursive, newest first. `complete` is false when the folder or one of its subfolders couldn't be read.
    static std::vector<FileStat> ListCaptures(const std::wstring& folder, bool* complete = nullptr);
    static bool IsInside(const std::wstring& path, const std::wstring& dir);  // component-wise, case-insensitive
    bool IsInLibrary(const std::wstring& path) const { return IsInside(path, folder_); }

    // Pending metadata for a file we're about to write, applied when the scan finds it.
    void NoteCapture(const std::wstring& path, const std::wstring& app, const std::wstring& window);
    // An edit joins the gallery inheriting the original's tags, collections, rating, comment and source app,
    // tagged "edited" if something changed. A collage is tagged "collage" and keeps what all sources share.
    void NoteEdit(const std::wstring& path, const std::wstring& source, const std::wstring& app, const std::wstring& window,
                  bool edited, const std::vector<std::wstring>& includes = {}, bool collage = false);

    // ---- mutations (UI thread) ----
    void AddTags(const std::vector<std::wstring>& tags, const std::vector<std::wstring>& paths);
    void RemoveTag(const std::wstring& tag, const std::vector<std::wstring>& paths);
    void DismissSuggestion(const std::wstring& tag, const std::vector<std::wstring>& paths);
    void ApplySuggestions(const std::vector<std::wstring>* paths = nullptr);  // all paths when null
    void SetRating(int rating, const std::vector<std::wstring>& paths);
    void SetComment(const std::wstring& comment, const std::wstring& path);
    void RenameTag(const std::wstring& from, const std::wstring& to);
    void DeleteTag(const std::wstring& tag);
    LibCollection CreateCollection(const std::wstring& name);
    void RenameCollection(const std::wstring& id, const std::wstring& name);
    void SetAutoTags(const std::wstring& id, const std::vector<std::wstring>& tags);
    void DeleteCollection(const std::wstring& id);
    void AddToCollection(const std::vector<std::wstring>& paths, const std::wstring& id);
    void RemoveFromCollection(const std::vector<std::wstring>& paths, const std::wstring& id);
    SmartFolder SaveSmartFolder(const std::wstring& name, const Filter& f);
    void UpdateSmartFolder(const std::wstring& id, const Filter* f, const std::wstring* name);
    void DeleteSmartFolder(const std::wstring& id);
    void Moved(const std::wstring& from, const std::wstring& to);  // keeps metadata across renames
    void Removed(const std::vector<std::wstring>& paths);
    // Copies files from elsewhere into <captures>\Imported (files already in the library are used as is).
    std::vector<std::wstring> ImportFiles(const std::vector<std::wstring>& files, const std::wstring& collection = L"");

    // ---- queries ----
    const std::vector<std::wstring>& Paths() const { return paths_; }
    const ItemMeta& Meta(const std::wstring& path) const;
    bool Has(const std::wstring& path) const { return meta_.count(path) != 0; }
    const std::vector<LibCollection>& Collections() const { return collections_; }
    const std::vector<SmartFolder>& SmartFolders() const { return smartFolders_; }
    const LibCollection* FindCollection(const std::wstring& id) const;
    std::vector<std::pair<std::wstring, int>> AllTags() const;  // most used first
    std::vector<std::pair<std::wstring, int>> AllApps() const;
    int CountIn(const std::wstring& collection) const;
    // Groups of identical or near-identical captures, largest first, newest first within a group.
    std::vector<std::vector<std::wstring>> DuplicateGroups(int maxDistance = 10, float maxFeatureDistance = 0.3f);
    // Everything ordered by visual similarity to `path` (feature distance).
    std::vector<std::pair<std::wstring, float>> Similar(const std::wstring& path, size_t limit = 120);
    static int SimilarityPercent(float distance);
    bool HasFeatures() const { return !features_.empty(); }
    std::pair<int, int> Progress() const { return progress_; }  // indexed so far, total queued (0,0 when idle)
    uint64_t Version() const { return version_; }               // bumped on every change

    // Called after every change (UI thread).
    int Subscribe(std::function<void()> fn);
    void Unsubscribe(int id);

    // Renames and deletes made through the library (UI thread), so other parts of the app can follow.
    std::function<void(const std::wstring& from, const std::wstring& to)> onMoved;
    std::function<void(const std::vector<std::wstring>& paths)> onRemoved;

    void Save();   // debounced 0.8 s, written on a worker thread
    void Flush();  // writes any pending save now (quit)
    void StopBackgroundWork();
    // "Tag captures automatically" (settings.ini [Gallery] AutoTag): turning it on tags the whole library once.
    void SetAutoTag(bool on);

    // tests
    void TestSetHash(const std::wstring& path, uint64_t h);
    void TestSetFeature(const std::wstring& path, std::vector<float> f);
    void TestSetText(const std::wstring& path, const std::wstring& text, int indexed);
    void TestSetApp(const std::wstring& path, const std::wstring& app);
    std::wstring FilePath() const;

private:
    struct Snapshot;
    void Changed();
    void Edit(const std::vector<std::wstring>& paths, const std::function<void(ItemMeta&)>& body);
    void LoadFile();
    void ImportLegacyOcr();
    void WriteSnapshot(const std::shared_ptr<Snapshot>& s);
    void Watch();
    void ChangedOnDisk();
    void IndexInBackground();
    void ApplyIndexResult(const std::wstring& path, double mtime, IndexResult r);

    bool persists_ = true, loaded_ = false;
    std::wstring folder_, watchedFolder_;
    std::vector<std::wstring> paths_;
    std::unordered_map<std::wstring, ItemMeta> meta_;
    std::vector<LibCollection> collections_;
    std::vector<SmartFolder> smartFolders_;
    std::unordered_map<std::wstring, std::vector<float>> features_;
    std::unordered_map<std::wstring, ItemMeta> pending_;      // metadata for files about to be written
    std::unordered_map<std::wstring, std::wstring> legacyOcr_;  // lower-case path -> text (old ocr-index.txt)
    bool featuresDirty_ = false;
    bool readOnly_ = false;        // library.json or features.bin exists but couldn't be read: never overwrite it
    uint64_t featuresSnapSeq_ = 0;  // newest snapshot that carried features
    uint64_t version_ = 0, techVersion_ = 0;
    int generation_ = 0;  // bumped by renames and deletes so an in-flight scan can't undo them
    std::pair<int, int> progress_{0, 0};
    std::map<int, std::function<void()>> listeners_;
    int nextListener_ = 1;
    UINT_PTR saveTimer_ = 0, diskTimer_ = 0;
    uint64_t saveSeq_ = 0;
    bool indexing_ = false, autoTag_ = false;

    struct DupCache {
        uint64_t version = ~0ull;
        size_t count = 0;
        std::vector<std::vector<std::wstring>> groups;
    } dupCache_;
    struct SimCache {
        std::wstring path;
        uint64_t version = ~0ull;
        size_t count = 0;
        std::vector<std::pair<std::wstring, float>> result;
    } simCache_;

    struct Workers;
    std::shared_ptr<Workers> shared_;  // state the worker threads share with us
};

std::wstring NewUuid();
std::wstring LowerText(const std::wstring& s);
std::vector<std::wstring> NormalizeTags(const std::vector<std::wstring>& tags);  // trimmed, de-duplicated (case-insensitive)
std::wstring FormatLibraryDate(double epoch);  // "yyyy-MM-dd HH:mm", local time, Gregorian

}  // namespace ather
