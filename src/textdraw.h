#pragma once
#include <optional>

#include "common.h"

namespace ather::textdraw {

// DirectWrite text drawn onto premultiplied BGRA bitmaps, for video captions, bubbles, titles and emoji:
// color emoji, a color per character range, and an optional edge. Safe on any thread that has COM.

struct Range {
    size_t start = 0, length = 0;
    COLORREF color = 0;
};

struct Style {
    std::wstring family = L"Segoe UI";
    int weight = 600;  // DWRITE_FONT_WEIGHT
    float size = 16;   // pixels
    COLORREF color = RGB(255, 255, 255);
    float alpha = 1;
    bool center = true;
    std::vector<Range> ranges;      // colors that override `color`
    std::optional<COLORREF> edge;   // an outline around the glyphs
    float edgeWidth = 1;
};

struct Extent {
    float w = 0, h = 0;
};

// Laid out at most `maxWidth` wide (wrapping between words).
Extent Measure(const std::wstring& text, const Style& st, float maxWidth);
// Draws the text laid out in a box `width` wide with its top-left at (x, y).
void Draw(Bitmap& dst, const std::wstring& text, const Style& st, float x, float y, float width);

// A rounded-rectangle or bubble fill (anti-aliased), drawn with Direct2D too.
void FillRounded(Bitmap& dst, float x, float y, float w, float h, float radius, COLORREF c, float alpha);
// Speech bubble: rounded body with a tail at the bottom left.
void FillBubble(Bitmap& dst, float x, float y, float w, float h, float radius, float tail, COLORREF c);
// A ring (stroked ellipse).
void StrokeEllipse(Bitmap& dst, float x, float y, float w, float h, float width, COLORREF c);

// Soft shadow under what is already drawn (uses the alpha channel), black at `alpha`.
void DropShadow(Bitmap& img, float blur, float dx, float dy, float alpha);

}  // namespace ather::textdraw
