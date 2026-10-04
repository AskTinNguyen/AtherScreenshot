#pragma once
#include "common.h"

namespace ather::annot {

// The screenshot editor's drawing code for the shapes video markup shares with it: same colors, sizes,
// arrowheads, step badges and text, so a box in a video looks like a box in a screenshot.

int ColorCount();
COLORREF Color(int index);  // clamped
const wchar_t* ColorName(int index);
int Levels();
float TextPx(int level);
bool IsDark(COLORREF c);

enum class Kind { Arrow, Rect, Ellipse, Step, Text };

struct Shape {
    Kind kind = Kind::Arrow;
    int color = 0;  // index into the palette
    int level = 2;
    float unit = 1;  // stroke and text scale
    float ax = 0, ay = 0, bx = 0, by = 0;  // arrow tail → head, or rect corners; text: top-left; step: center
    std::wstring text;
    int step = 1;
    float wrap = 0;  // text: wrap width (0 = none)
};

struct BoxF {
    float x = 0, y = 0, w = 0, h = 0;
};

// What the shape covers, including its stroke.
BoxF Bounds(const Shape& s);
float StrokeWidth(const Shape& s);
// Draws onto premultiplied BGRA pixels, with (ox, oy) of the shape's space at the bitmap's top-left.
void Draw(Bitmap& dst, const Shape& s, float ox, float oy);
// A box or ellipse outline traced from its top-left, `part` (0…1) of the way round (video "draw on").
void DrawPartialOutline(Bitmap& dst, const Shape& s, float part, float ox, float oy);

}  // namespace ather::annot
