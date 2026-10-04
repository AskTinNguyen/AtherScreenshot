#pragma once
#include <optional>

#include "common.h"

namespace ather {

// Collage layouts: place screenshots on one canvas at (close to) their own resolution.
// Port of macos/Sources/AtherScreenshot/Collage.swift.

enum class CollageLayout { Auto, Grid, Row, Column, Feature };
enum class CollageSize { Fit, Wide, Square };
const wchar_t* CollageLayoutLabel(CollageLayout l);
const wchar_t* CollageSizeLabel(CollageSize s);

struct CollageSpec {
    std::vector<SIZE> sizes;   // pixel size of each screenshot
    std::vector<int> order;    // slot shown at each position
    CollageLayout layout = CollageLayout::Auto;
    int gap = 2;               // index into kSteps
    int margin = 2;
    int background = 0;        // index into kBackgrounds
    bool rounded = true;
    bool shadow = false;
    CollageSize size = CollageSize::Fit;

    static constexpr double kSteps[] = {0, 8, 16, 32};  // points, scaled by the capture unit
    static const wchar_t* const kStepNames[4];
    struct Background {
        const wchar_t* name;
        std::optional<COLORREF> color;  // none: transparent
    };
    static const Background kBackgrounds[5];

    explicit CollageSpec(std::vector<SIZE> s = {});
};

struct CollageRect {
    double x = 0, y = 0, w = 0, h = 0;
};

struct CollageResult {
    double width = 0, height = 0;
    std::vector<CollageRect> rects;  // per position in `order`, integral
    double radius = 0;
    double scale = 1;  // applied by the size preset
};

CollageResult LayoutCollage(const CollageSpec& spec, double unit);

}  // namespace ather
