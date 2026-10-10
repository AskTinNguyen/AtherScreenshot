#pragma once
#include <optional>
#include <utility>

#include "common.h"

namespace ather {

// The video editor's model and the frame renderer shared by the preview and the export, so what you see
// while editing is what gets saved. Port of macos/Sources/AtherScreenshot/VideoRender.swift and the model in
// VideoEditor.swift. Coordinates are video pixels with a top-left origin; times are seconds in the source.

struct VPoint {
    double x = 0, y = 0;
    bool operator==(const VPoint&) const = default;
};

struct VRect {
    double x = 0, y = 0, w = 0, h = 0;
    double MaxX() const { return x + w; }
    double MaxY() const { return y + h; }
    double MidX() const { return x + w / 2; }
    double MidY() const { return y + h / 2; }
    bool Contains(VPoint p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    VRect Inset(double dx, double dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    VRect Offset(double dx, double dy) const { return {x + dx, y + dy, w, h}; }
    VRect Integral() const;  // like CGRect.integral
    bool operator==(const VRect&) const = default;
};
VRect NormRect(VPoint a, VPoint b);

enum class MarkKind { Text, Bubble, Emoji, Arrow, Box, Ellipse, Step, Blur, Pixelate, Zoom, Title };
constexpr int kMarkKinds = 11;
enum class AnimStyle { Auto, None, Fade, Pop, Scale, Slide, Wipe, BlurIn, DrawOn, Typewriter };
enum class Emphasis { None, Pulse, Bounce, Shake, Ping };
enum class CaptionLook { Pill, Outline, Bar };
enum class CaptionPosition { Bottom, Middle, Top };

const wchar_t* KindLabel(MarkKind k);
std::wstring KindPlural(MarkKind k);
wchar_t KindKey(MarkKind k);         // shortcut letter, or 0
wchar_t KindGlyph(MarkKind k);       // Segoe Fluent Icons
bool KindIsLine(MarkKind k);
bool KindHasText(MarkKind k);
bool KindIsRegion(MarkKind k);
bool KindIsStroke(MarkKind k);
double KindDefaultSeconds(MarkKind k);
AnimStyle KindDefaultStyle(MarkKind k);  // what "Auto" means
std::vector<AnimStyle> KindStyles(MarkKind k);  // the styles that suit the kind, so menus stay short
const wchar_t* StyleLabel(AnimStyle s);
const std::vector<AnimStyle>& CaptionStyles();
const wchar_t* EmphasisLabel(Emphasis e);
const wchar_t* LookLabel(CaptionLook l);
const wchar_t* PositionLabel(CaptionPosition p);

uint64_t NewItemId();

struct Mark {
    uint64_t id = NewItemId();
    MarkKind kind = MarkKind::Box;
    double start = 0, end = 3;
    VPoint a, b;  // rect corners, or arrow tail → head
    std::wstring text, subtitle;
    int color = 0;  // index into the editor palette
    int level = 2;
    int step = 1;
    AnimStyle style = AnimStyle::Auto;
    std::optional<AnimStyle> exit;  // advanced: a different exit; none mirrors `style`
    Emphasis emphasis = Emphasis::None;
    bool snappy = false;  // zoom: quick instead of smooth

    VRect Rect() const { return NormRect(a, b); }
    bool Active(double t) const { return t >= start && t < end; }
    AnimStyle InStyle() const;
    AnimStyle OutStyle() const;
    bool operator==(const Mark&) const = default;
};

struct CaptionWord {
    double start = 0, end = 0;
    std::wstring text;
    bool operator==(const CaptionWord&) const = default;
};

struct Caption {
    uint64_t id = NewItemId();
    double start = 0, end = 0;
    std::wstring text;
    CaptionPosition position = CaptionPosition::Bottom;
    std::optional<VPoint> center;    // dragged: center as a fraction of the frame; overrides `position`
    std::vector<CaptionWord> words;  // from auto captions: when each word is spoken
    bool Active(double t) const { return t >= start && t < end; }
    bool operator==(const Caption&) const = default;
};

// A reviewer's note on one frame of a file (or a range of frames), kept next to that file (videoeditor.cpp's sidecar,
// "<video>.notes.json"). It's tied to the footage, not the edit: it stays put when clips are cut or moved, and shows
// wherever its frame is on the timeline. Never part of a plain Save.
enum class NoteKind { Note, Issue, Question, Good };
constexpr int kNoteKinds = 4;
const wchar_t* NoteKindLabel(NoteKind k);  // "Note", "Issue", "Question", "Looks good"
const char* NoteKindKey(NoteKind k);       // as saved: "note", "issue", "question", "good"
NoteKind NoteKindOf(const std::string& key);  // anything unknown is a Note
int NoteKindColor(NoteKind k);             // the editor palette (annot): lime, red, blue, green

struct Note {
    uint64_t id = NewItemId();
    std::wstring path;              // the file it's on
    double src = 0;                 // the noted frame's own time in that file
    std::optional<double> srcEnd;   // a range: its last frame's time
    std::wstring text, author;
    NoteKind kind = NoteKind::Note;
    bool resolved = false;
    std::optional<VPoint> pin;      // a spot on the picture: fractions of the file's upright frame
    int64_t created = 0;            // seconds since 1970 (UTC)
    bool operator==(const Note&) const = default;
};

// The notes kept with a video: "<video file name>.notes.json" next to it (UTF-8 JSON, one entry per note with its
// frame number and time in the file, kind, status, author, text, pin and when it was made), for the editor and for
// other tools. `times`: the file's frame times (FrameTimes), for frame numbers; when empty, a grid at `fps`.
std::wstring NotesPath(const std::wstring& video);
std::string NotesJson(const std::wstring& video, std::vector<Note> notes, const std::vector<double>& times, double fps);
// A sidecar's notes, on `video`; false when the text isn't one (damaged). A note without a time is placed by its frame.
bool ParseNotes(const std::string& json, const std::wstring& video, const std::vector<double>& times, double fps, std::vector<Note>* out);
// The order notes are kept and compared in: by frame, then as made.
void SortNotes(std::vector<Note>& notes);

// One video of the sequence: the stretch [in, out) of a file, in that file's seconds. Clips play back to back;
// each is fitted into the sequence frame (black bars when the shape differs).
struct Clip {
    uint64_t id = NewItemId();
    uint64_t source = 0;  // shared by the pieces split from one added video (not by a second copy of the file)
    std::wstring path;
    double in = 0, out = 0;
    double length = 0;  // the whole file
    int w = 0, h = 0;   // upright size
    double fps = 0;
    bool hasAudio = false;
    double Duration() const { return std::max(0.0, out - in); }
    bool operator==(const Clip&) const = default;
};

double ClipsDuration(const std::vector<Clip>& clips);
double ClipStart(const std::vector<Clip>& clips, size_t i);  // where clip i begins on the timeline
// The clip playing at timeline time `t` and the source time in it (the last clip's end past the end).
std::optional<std::pair<size_t, double>> LocateClip(const std::vector<Clip>& clips, double t);

struct VideoEdit {
    std::vector<Clip> clips;  // empty = the whole source file
    int frameW = 0, frameH = 0;  // the sequence frame; 0 = the first clip's size
    double trimStart = 0, trimEnd = 0;
    std::optional<VRect> crop;
    double speed = 1;
    bool muted = false;
    std::vector<Caption> captions;
    int captionSize = 2;   // 0…4
    int captionColor = 6;  // text (white)
    int captionEdge = 7;   // box, bar or outline (black)
    std::vector<Mark> marks;
    CaptionLook captionLook = CaptionLook::Pill;
    AnimStyle captionStyle = AnimStyle::Auto;
    bool highlightWords = true;
    std::vector<Note> notes;  // of every file the editor has open, also ones whose footage isn't in the clips now

    static constexpr double kSpeeds[] = {0.5, 1, 1.5, 2, 4};
    static constexpr double kCaptionScale[] = {0.03, 0.037, 0.045, 0.055, 0.068};
    static const wchar_t* const kCaptionSizes[5];

    double OutputDuration() const { return std::max(0.0, trimEnd - trimStart) / speed; }
    bool operator==(const VideoEdit&) const = default;
};

// How an item looks at one moment of its entrance, exit or emphasis.
struct Motion {
    double alpha = 1, scale = 1;
    double dx = 0, dy = 0;  // video pixels; positive is right / down
    double reveal = 1;      // 0…1: wipe, draw on, typewriter
    bool wipe = false;
    double blur = 0;
    std::optional<double> ring;  // ping: 0…1 phase of the ring

    void Apply(AnimStyle s, double p, double height);  // p: 0 (hidden) … 1 (fully shown)
    // Entrance and exit of one item at time `t`. Each style has one tuned duration.
    static Motion Between(AnimStyle in, AnimStyle out, double start, double end, double t, int chars, double height);
};

// ---- frames ----

// Every frame of the timeline in order: where it shows (timeline seconds), its clip, and its time in that clip's file.
// Made from each file's frame times (FrameTimes in videoio.h), or a grid at the clip's rate for a file whose times
// aren't known (yet). The frames are the ones SequenceReader gives: for each clip, the frame on screen at its in point
// (from the clip's start), then the ones before its out point. Frame n counts from the timeline's start.
struct TimelineFrames {
    struct Frame {
        double t = 0, src = 0;
        uint32_t clip = 0;
    };
    std::vector<Frame> frames;
    std::vector<double> fps;         // each clip's rate (30 when the file doesn't say)
    std::vector<size_t> clipFirst;   // clip i's frames are [clipFirst[i], clipFirst[i + 1])

    // `times`: a file's frame times, or null when they aren't known.
    static TimelineFrames Of(const std::vector<Clip>& clips, const std::function<const std::vector<double>*(const Clip&)>& times);
    bool empty() const { return frames.empty(); }
    size_t size() const { return frames.size(); }
    size_t Nearest(double t) const;  // the frame closest to timeline time `t` (what the paused editor shows there)
    size_t At(double t) const;       // the frame on screen at `t`: the last one starting at or before it
    double Fps(size_t n) const;      // the rate of frame n's clip
    std::wstring Timecode(size_t n) const;  // m:ss:ff, frames within the second at the clip's rate
    std::wstring Readout(size_t n) const;   // "0:12:37 · frame 757 · 60 fps"
    // Go to: a frame number ("757"), a timecode ("0:12:37", "1:02:03:04" with hours) or a time ("12.6", "0:12.6").
    std::optional<size_t> Find(const std::wstring& text) const;
};
// The timeline frame showing a file's frame at source time `src`: in the first clip of that file whose frames
// include it (within half a frame). None when that footage isn't on the timeline.
std::optional<size_t> FrameOfSource(const TimelineFrames& tf, const std::vector<Clip>& clips, const std::wstring& path, double src);
std::wstring Timecode(double t, double fps);  // m:ss:ff
std::wstring FpsLabel(double fps);            // "60 fps", "29.97 fps"
std::vector<double> FrameGrid(double length, double fps);  // k / fps for every frame starting before `length`

// Replaces the clips and moves everything on the timeline with the clip it sits in: marks and captions keep
// their place in the footage, items in a removed clip (or a cut-off part of one) go, and the trim follows.
void ApplyClips(VideoEdit& e, std::vector<Clip> clips);

// Groups transcribed words into short captions: a new one after a pause > 0.7 s, ~42 characters or 3.5 s;
// each holds until the next (at most 0.8 s more). Keeps each word's times.
std::vector<Caption> ChunkCaptions(const std::vector<CaptionWord>& words);

struct Placed {
    BitmapPtr image;
    VRect rect;  // where it goes: video pixels for marks, output pixels for captions
};

class FrameRenderer {
public:
    FrameRenderer(const VideoEdit& edit, SIZE full, bool preview);

    // `src`: the source frame (full size, opaque). `t`: source time in seconds. Returns a frame of `Out()` size.
    BitmapPtr Render(const Bitmap& src, double t) const;
    // The same, but when nothing changes the frame (an export without edits at `t`) it can be `src` itself.
    // `owned`: nothing else will look at `src` again, so it can be drawn on instead of a copy of it.
    BitmapPtr Render(const BitmapPtr& src, double t, bool owned = false) const;

    // An export frame at `t` is the source frame as it is: no edit shows, and nothing is cropped or zoomed.
    bool Untouched(double t) const;
    // An export frame at `t` whose edits change only parts of the source frame (nothing zooming, no title card, and
    // a crop that starts on even pixels): those parts, apart from each other, in source pixels on even coordinates
    // (none when nothing shows), else nothing. DrawEdits draws what falls on one (`area`, whose top-left is `at`)
    // exactly as Render does on the whole frame.
    std::optional<std::vector<RECT>> EditAreas(double t) const;
    void DrawEdits(Bitmap& area, POINT at, double t) const;

    VRect ViewRect(double t) const;  // the crop, or a zoom into it
    static VRect ZoomTarget(VRect r, VRect view);
    Motion MotionOf(const Mark& m, double t) const;
    Motion CaptionMotion(const Caption& c, double t) const;
    std::optional<Placed> MarkImage(const Mark& m, double reveal = 1) const;
    // A caption in output coordinates (`size` is the output frame), in the video's caption look.
    std::optional<Placed> CaptionImage(const Caption& c, SIZE size, std::optional<double> t = std::nullopt, double reveal = 1) const;
    BitmapPtr TitleImage(const Mark& m, SIZE size) const;

    const VideoEdit& Edit() const { return edit_; }
    SIZE Full() const { return full_; }
    VRect View() const { return view_; }
    SIZE Out() const { return out_; }
    bool zoomInPreview = false;  // preview applies zoom only while playing, so editing stays put
    uint64_t settled = 0;        // preview while paused: the selected item shows fully, not mid-animation

private:
    BitmapPtr Draw(const Bitmap& src, const BitmapPtr* shared, double t, bool owned) const;
    // Draw's steps, on `img` holding the part of the frame from `at` on (source pixels for 1–2, output for 4).
    struct Effect {
        RECT rc{};  // the pixels a blur or pixelate changes
        Motion mo;
        double sigma = 0;  // blur
        int block = 0;     // pixelate
    };
    std::optional<Effect> EffectOf(const Mark& m, double t) const;
    void DrawRegions(Bitmap& img, POINT at, double t) const;  // 1. blur and pixelate
    void DrawMarks(Bitmap& img, POINT at, double t) const;    // 2. markup on the video
    void DrawOverlays(Bitmap& target, double ox, double oy, POINT at, SIZE tsize, double t) const;  // 4. captions, title cards
    std::optional<Placed> StrokeOn(const Mark& m, double p) const;
    std::optional<Placed> RingImage(VRect r) const;

    VideoEdit edit_;
    SIZE full_;
    VRect view_;
    SIZE out_;
    bool preview_;
    double unit_;
};

// ---- the review video ----

// A note as the review video shows it.
struct ReviewNote {
    double t = 0;       // timeline time of the noted frame (that frame's own time)
    size_t frame = 0;   // its timeline frame number
    std::wstring timecode;  // m:ss:ff
    NoteKind kind = NoteKind::Note;
    bool resolved = false;
    std::wstring author, text;
    std::optional<VPoint> pin;  // in output pixels
};

// What "Save review video" adds to an edit: a summary card first, a burn-in of the timecode and frame number on every
// frame, and at each note the noted frame held under its note card (silent), then on.
struct ReviewPlan {
    std::vector<ReviewNote> notes;  // in timeline order, within the trim
    double intro = 3;               // seconds of the summary card
    double hold = 3;                // seconds each note holds
    TimelineFrames frames;          // of the edit's clips: for the burn-in's numbers
    std::wstring title, date;       // the summary card's
    std::vector<std::wstring> reviewers;
};
std::wstring BurnInText(const TimelineFrames& tf, size_t n);  // "0:12:37 · frame 757"
// Draw onto an opaque output frame (all scaled to its height); each returns where it drew.
RECT DrawBurnIn(Bitmap& frame, const std::wstring& text);  // top left, on an opaque box
RECT DrawNoteCard(Bitmap& frame, const ReviewNote& n);      // the card, and the pin
void DrawNotePin(Bitmap& frame, const ReviewNote& n);       // just the pin
BitmapPtr SummaryCard(SIZE size, const ReviewPlan& plan);
// The kind's line on cards and lists: "Issue", "Issue · Resolved".
std::wstring NoteKindLine(NoteKind k, bool resolved);

// ---- pixel helpers shared with the video window ----

// Draws `img` stretched into `r` (destination pixels, fractional) on premultiplied `dst` with motion applied:
// scale about the center, offset, wipe, blur and alpha.
// `at`: where `dst`'s top-left is, when it holds just a part of the frame (`r` stays in frame pixels).
void PlaceImage(Bitmap& dst, const Bitmap& img, VRect r, const Motion& mo, POINT at = {});
// Frees the rendered marks, captions and title cards kept between frames.
void ClearRenderCache();

}  // namespace ather
