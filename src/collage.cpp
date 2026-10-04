#include "collage.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "selftest.h"

namespace ather {

const wchar_t* const CollageSpec::kStepNames[4] = {L"None", L"Small", L"Medium", L"Large"};
const CollageSpec::Background CollageSpec::kBackgrounds[5] = {
    {L"White", RGB(255, 255, 255)}, {L"Light gray", RGB(237, 237, 240)}, {L"Dark", RGB(28, 28, 31)}, {L"Black", RGB(0, 0, 0)}, {L"Transparent", std::nullopt},
};

CollageSpec::CollageSpec(std::vector<SIZE> s) : sizes(std::move(s)) {
    order.resize(sizes.size());
    std::iota(order.begin(), order.end(), 0);
}

const wchar_t* CollageLayoutLabel(CollageLayout l) {
    static const wchar_t* const k[] = {L"Auto", L"Grid", L"Row", L"Column", L"Feature"};
    return k[(int)l];
}

const wchar_t* CollageSizeLabel(CollageSize s) {
    static const wchar_t* const k[] = {L"Fit screenshots", L"1920 px wide", L"Square 2048 px"};
    return k[(int)s];
}

namespace {

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Content size and one rect per image, origin at (0, 0).
void Place(const std::vector<SIZE>& sizes, CollageLayout layout, double g, double* outW, double* outH, std::vector<CollageRect>* out) {
    const size_t n = sizes.size();
    out->assign(n, {});
    if (!n) {
        *outW = *outH = 0;
        return;
    }
    std::vector<double> aspect, ws, hs;
    for (const auto& s : sizes) {
        aspect.push_back(std::max(0.05, (double)s.cx / std::max(1L, s.cy)));
        ws.push_back(s.cx);
        hs.push_back(s.cy);
    }
    const double medW = std::min(3000.0, Median(ws)), medH = std::min(2000.0, Median(hs));
    if (layout == CollageLayout::Feature && n < 2) layout = CollageLayout::Auto;
    switch (layout) {
        case CollageLayout::Row: {
            const double h = medH;
            double x = 0;
            for (size_t i = 0; i < n; ++i) {
                (*out)[i] = {x, 0, aspect[i] * h, h};
                x += aspect[i] * h + g;
            }
            *outW = x - g;
            *outH = h;
            return;
        }
        case CollageLayout::Column: {
            const double w = medW;
            double y = 0;
            for (size_t i = 0; i < n; ++i) {
                (*out)[i] = {0, y, w, w / aspect[i]};
                y += w / aspect[i] + g;
            }
            *outW = w;
            *outH = y - g;
            return;
        }
        case CollageLayout::Grid: {
            const int cols = (int)std::ceil(std::sqrt((double)n)), rows = ((int)n + cols - 1) / cols;
            const double cw = medW, ch = medH;
            for (size_t i = 0; i < n; ++i) {  // each fitted inside its cell
                const double cx = (i % cols) * (cw + g), cy = (double)(i / cols) * (ch + g);
                const double k = std::min(cw / sizes[i].cx, ch / sizes[i].cy);
                const double w = sizes[i].cx * k, h = sizes[i].cy * k;
                (*out)[i] = {cx + cw / 2 - w / 2, cy + ch / 2 - h / 2, w, h};
            }
            *outW = cols * (cw + g) - g;
            *outH = rows * (ch + g) - g;
            return;
        }
        case CollageLayout::Feature: {
            // First screenshot large on the left; the rest stacked on the right at the same total height.
            const double h = std::min(2400.0, std::max(600.0, (double)sizes[0].cy));
            const double w0 = aspect[0] * h;
            double inv = 0;
            for (size_t i = 1; i < n; ++i) inv += 1 / aspect[i];
            const double wr = (h - g * (double)(n - 2)) / inv;
            double y = 0;
            (*out)[0] = {0, 0, w0, h};
            for (size_t i = 1; i < n; ++i) {
                (*out)[i] = {w0 + g, y, wr, wr / aspect[i]};
                y += wr / aspect[i] + g;
            }
            *outW = w0 + g + wr;
            *outH = h;
            return;
        }
        case CollageLayout::Auto:
        default: {
            // Justified rows, all the same width: tries every row count and keeps the one closest to 4:3.
            double area = 0, widest = 0;
            for (size_t i = 0; i < n; ++i) {
                area += (double)sizes[i].cy * sizes[i].cy * aspect[i];
                widest = std::max(widest, (double)sizes[i].cx);
            }
            const double width = std::min(8000.0, std::max(std::sqrt(area) * 1.15, widest));
            const double total = std::accumulate(aspect.begin(), aspect.end(), 0.0);
            double bestScore = 1e300, bestH = 0;
            std::vector<CollageRect> best;
            for (size_t k = 1; k <= n; ++k) {
                std::vector<std::vector<size_t>> rows(1);
                double acc = 0;
                for (size_t i = 0; i < n; ++i) {
                    const double boundary = total * rows.size() / k;
                    if (!rows.back().empty() && rows.size() < k && acc + aspect[i] / 2 > boundary) rows.emplace_back();
                    rows.back().push_back(i);
                    acc += aspect[i];
                }
                std::vector<CollageRect> rects(n);
                double y = 0;
                for (const auto& row : rows) {
                    double sum = 0;
                    for (size_t i : row) sum += aspect[i];
                    const double h = (width - g * (double)(row.size() - 1)) / sum;
                    double x = 0;
                    for (size_t i : row) {
                        rects[i] = {x, y, aspect[i] * h, h};
                        x += aspect[i] * h + g;
                    }
                    y += h + g;
                }
                const double height = y - g;
                const double score = std::fabs(std::log((width / height) / (4.0 / 3.0)));
                if (score < bestScore) {
                    bestScore = score;
                    bestH = height;
                    best = rects;
                }
            }
            *outW = width;
            *outH = bestH;
            *out = best;
            return;
        }
    }
}

}  // namespace

CollageResult LayoutCollage(const CollageSpec& spec, double unit) {
    std::vector<SIZE> sizes;
    for (int slot : spec.order) sizes.push_back(spec.sizes[slot]);
    const double g = CollageSpec::kSteps[spec.gap] * unit;
    const double m = CollageSpec::kSteps[spec.margin] * unit * 2;
    CollageResult r;
    double cw, ch;
    Place(sizes, spec.layout, g, &cw, &ch, &r.rects);
    for (auto& rc : r.rects) {
        rc.x += m;
        rc.y += m;
    }
    double w = cw + 2 * m, h = ch + 2 * m, f = 1;
    switch (spec.size) {
        case CollageSize::Fit: break;
        case CollageSize::Wide:
            f = 1920 / w;
            for (auto& rc : r.rects) rc = {rc.x * f, rc.y * f, rc.w * f, rc.h * f};
            w = 1920;
            h = std::round(h * f);
            break;
        case CollageSize::Square: {
            const double side = 2048;
            f = side / std::max(w, h);
            const double dx = (side - w * f) / 2, dy = (side - h * f) / 2;
            for (auto& rc : r.rects) rc = {rc.x * f + dx, rc.y * f + dy, rc.w * f, rc.h * f};
            w = h = side;
            break;
        }
    }
    r.radius = spec.rounded ? 12 * unit * std::min(1.0, std::max(0.35, f)) : 0;
    r.scale = f;
    r.width = std::round(w);
    r.height = std::round(h);
    for (auto& rc : r.rects) {  // integral, like CGRect.integral
        const double x0 = std::floor(rc.x), y0 = std::floor(rc.y), x1 = std::ceil(rc.x + rc.w), y1 = std::ceil(rc.y + rc.h);
        rc = {x0, y0, x1 - x0, y1 - y0};
    }
    return r;
}

// ---- tests (CompositionTests.testCollageLayouts) ----

ATHER_TEST(collage_layouts) {
    const std::vector<SIZE> imgs = {{1600, 1000}, {800, 1200}, {1200, 700}, {1000, 1000}, {1400, 900}};
    for (int l = 0; l <= (int)CollageLayout::Feature; ++l) {
        CollageSpec spec(imgs);
        spec.layout = (CollageLayout)l;
        const CollageResult r = LayoutCollage(spec, 1);
        test::Note(std::string("layout ") + std::to_string(l));
        CHECK_EQ(r.rects.size(), imgs.size());
        for (size_t i = 0; i < r.rects.size(); ++i) {
            const CollageRect& a = r.rects[i];
            CHECK(a.x >= -1 && a.y >= -1 && a.x + a.w <= r.width + 1 && a.y + a.h <= r.height + 1);  // inside the canvas
            const double want = (double)imgs[spec.order[i]].cx / imgs[spec.order[i]].cy;
            CHECK_NEAR(a.w / a.h, want, want * 0.03);  // keeps the aspect ratio
            for (size_t j = i + 1; j < r.rects.size(); ++j) {  // no overlaps
                const CollageRect& b = r.rects[j];
                const double ox = std::min(a.x + a.w - 1, b.x + b.w - 1) - std::max(a.x + 1, b.x + 1);
                const double oy = std::min(a.y + a.h - 1, b.y + b.h - 1) - std::max(a.y + 1, b.y + 1);
                CHECK(ox < 1 || oy < 1);
            }
        }
    }
    CollageSpec spec(imgs);
    spec.size = CollageSize::Wide;
    CHECK_EQ(LayoutCollage(spec, 2).width, 1920.0);
    spec.size = CollageSize::Square;
    const CollageResult sq = LayoutCollage(spec, 2);
    CHECK(sq.width == 2048 && sq.height == 2048);
    // Auto aims for 4:3 overall.
    CollageSpec autoSpec(imgs);
    const CollageResult a = LayoutCollage(autoSpec, 1);
    CHECK(a.width / a.height > 1.0 && a.width / a.height < 1.8);
}

}  // namespace ather
