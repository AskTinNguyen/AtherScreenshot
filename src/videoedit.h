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

struct VideoEdit {
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
    std::optional<Placed> StrokeOn(const Mark& m, double p) const;
    std::optional<Placed> RingImage(VRect r) const;

    VideoEdit edit_;
    SIZE full_;
    VRect view_;
    SIZE out_;
    bool preview_;
    double unit_;
};

// ---- pixel helpers shared with the video window ----

// Draws `img` stretched into `r` (destination pixels, fractional) on premultiplied `dst` with motion applied:
// scale about the center, offset, wipe, blur and alpha.
void PlaceImage(Bitmap& dst, const Bitmap& img, VRect r, const Motion& mo);
// Frees the rendered marks, captions and title cards kept between frames.
void ClearRenderCache();

}  // namespace ather
