import AppKit

// Collage layouts: places screenshots on one canvas at (close to) their own resolution.

enum CollageLayout: Int, CaseIterable {
    case auto, grid, row, column, feature
    var label: String { ["Auto", "Grid", "Row", "Column", "Feature"][rawValue] }
}

enum CollageSize: Int, CaseIterable {
    case fit, wide, square
    var label: String { ["Fit screenshots", "1920 px wide", "Square 2048 px"][rawValue] }
}

struct CollageSpec {
    var images: [CGImage]
    var sources: [URL?]
    var order: [Int]                      // slot shown at each position
    var layout = CollageLayout.auto
    var gap = 2                           // index into steps
    var margin = 2
    var background = 0                    // index into backgrounds
    var rounded = true
    var shadow = false
    var size = CollageSize.fit

    static let steps: [CGFloat] = [0, 8, 16, 32]        // points; scaled by the capture unit
    static let stepNames = ["None", "Small", "Medium", "Large"]
    static let backgrounds: [(String, CGColor?)] = [
        ("White", CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1)),
        ("Light gray", CGColor(srgbRed: 0.93, green: 0.93, blue: 0.94, alpha: 1)),
        ("Dark", CGColor(srgbRed: 0.11, green: 0.11, blue: 0.12, alpha: 1)),
        ("Black", CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 1)),
        ("Transparent", nil),
    ]

    init(images: [CGImage], sources: [URL?]) {
        self.images = images
        self.sources = sources
        order = Array(images.indices)
    }
}

enum Collage {
    struct Result {
        var size: CGSize
        var rects: [CGRect]               // per position in `order`
        var radius: CGFloat
        var scale: CGFloat                // applied by the size preset
    }

    static func layout(_ spec: CollageSpec, unit: CGFloat) -> Result {
        let sizes = spec.order.map { CGSize(width: spec.images[$0].width, height: spec.images[$0].height) }
        let g = CollageSpec.steps[spec.gap] * unit
        let m = CollageSpec.steps[spec.margin] * unit * 2
        var (content, rects) = place(sizes, spec.layout, gap: g)
        rects = rects.map { $0.offsetBy(dx: m, dy: m) }
        var size = CGSize(width: content.width + 2 * m, height: content.height + 2 * m)
        var f: CGFloat = 1
        switch spec.size {
        case .fit: break
        case .wide:
            f = 1920 / size.width
            size = CGSize(width: 1920, height: (size.height * f).rounded())
            rects = rects.map { scaled($0, f) }
        case .square:
            let side: CGFloat = 2048
            f = side / max(size.width, size.height)
            let dx = (side - size.width * f) / 2, dy = (side - size.height * f) / 2
            rects = rects.map { scaled($0, f).offsetBy(dx: dx, dy: dy) }
            size = CGSize(width: side, height: side)
        }
        let radius = spec.rounded ? 12 * unit * min(1, max(0.35, f)) : 0
        return Result(size: CGSize(width: size.width.rounded(), height: size.height.rounded()), rects: rects.map { $0.integral }, radius: radius, scale: f)
    }

    private static func scaled(_ r: CGRect, _ f: CGFloat) -> CGRect { CGRect(x: r.minX * f, y: r.minY * f, width: r.width * f, height: r.height * f) }

    private static func median(_ v: [CGFloat]) -> CGFloat {
        let s = v.sorted()
        return s.isEmpty ? 0 : s[s.count / 2]
    }

    // Content size and one rect per image, origin at (0, 0).
    static func place(_ sizes: [CGSize], _ layout: CollageLayout, gap g: CGFloat) -> (CGSize, [CGRect]) {
        let n = sizes.count
        guard n > 0 else { return (.zero, []) }
        let aspect = sizes.map { max(0.05, $0.width / max(1, $0.height)) }
        let medW = min(3000, median(sizes.map(\.width))), medH = min(2000, median(sizes.map(\.height)))
        switch layout {
        case .row:
            let h = medH
            var x: CGFloat = 0
            let rects = aspect.map { a -> CGRect in defer { x += a * h + g }; return CGRect(x: x, y: 0, width: a * h, height: h) }
            return (CGSize(width: x - g, height: h), rects)
        case .column:
            let w = medW
            var y: CGFloat = 0
            let rects = aspect.map { a -> CGRect in defer { y += w / a + g }; return CGRect(x: 0, y: y, width: w, height: w / a) }
            return (CGSize(width: w, height: y - g), rects)
        case .grid:
            let cols = Int(ceil(sqrt(Double(n)))), rows = (n + cols - 1) / cols
            let cw = medW, ch = medH
            let rects = (0..<n).map { i -> CGRect in
                let cell = CGRect(x: CGFloat(i % cols) * (cw + g), y: CGFloat(i / cols) * (ch + g), width: cw, height: ch)
                let s = min(cw / sizes[i].width, ch / sizes[i].height)   // fit inside the cell
                let w = sizes[i].width * s, h = sizes[i].height * s
                return CGRect(x: cell.midX - w / 2, y: cell.midY - h / 2, width: w, height: h)
            }
            return (CGSize(width: CGFloat(cols) * (cw + g) - g, height: CGFloat(rows) * (ch + g) - g), rects)
        case .feature where n >= 2:
            // First screenshot large on the left; the rest stacked on the right at the same total height.
            let h = min(2400, max(600, sizes[0].height))
            let w0 = aspect[0] * h
            let rest = aspect.dropFirst()
            let wr = (h - g * CGFloat(rest.count - 1)) / rest.reduce(0) { $0 + 1 / $1 }
            var y: CGFloat = 0
            var rects = [CGRect(x: 0, y: 0, width: w0, height: h)]
            for a in rest {
                rects.append(CGRect(x: w0 + g, y: y, width: wr, height: wr / a))
                y += wr / a + g
            }
            return (CGSize(width: w0 + g + wr, height: h), rects)
        case .auto, .feature:
            // Justified rows, all the same width: tries every row count and keeps the one closest to 4:3.
            let area = zip(sizes, aspect).reduce(0) { $0 + $1.0.height * $1.0.height * $1.1 }
            let width = min(8000, max(sqrt(area) * 1.15, sizes.map(\.width).max() ?? 0))
            let total = aspect.reduce(0, +)
            var best: (score: CGFloat, height: CGFloat, rects: [CGRect])?
            for k in 1...n {
                var rows: [[Int]] = [[]]
                var acc: CGFloat = 0
                for i in 0..<n {
                    let boundary = total * CGFloat(rows.count) / CGFloat(k)
                    if !rows[rows.count - 1].isEmpty && rows.count < k && acc + aspect[i] / 2 > boundary { rows.append([]) }
                    rows[rows.count - 1].append(i)
                    acc += aspect[i]
                }
                var rects = [CGRect](repeating: .zero, count: n)
                var y: CGFloat = 0
                for row in rows {
                    let sum = row.reduce(0) { $0 + aspect[$1] }
                    let h = (width - g * CGFloat(row.count - 1)) / sum
                    var x: CGFloat = 0
                    for i in row { rects[i] = CGRect(x: x, y: y, width: aspect[i] * h, height: h); x += aspect[i] * h + g }
                    y += h + g
                }
                let height = y - g
                let score = abs(log((width / height) / (4.0 / 3.0)))
                if best == nil || score < best!.score { best = (score, height, rects) }
            }
            return (CGSize(width: width, height: best!.height), best!.rects)
        }
    }
}
