import AppKit
import CoreImage

enum Tool: Int, CaseIterable, Codable {
    case select, arrow, line, rect, ellipse, pen, highlight, text, step, blur, pixelate, spotlight, magnify, crop, canvas, image

    var info: (name: String, key: Character, symbol: String, hint: String) {
        switch self {
        case .select: return ("Select", "v", "cursorarrow", "Click to select, drag to move, ⌫ removes, ⌘D duplicates")
        case .arrow: return ("Arrow", "a", "arrow.up.right", "Drag to draw  ·  ⇧ snaps to 45°")
        case .line: return ("Line", "l", "line.diagonal", "Drag to draw  ·  ⇧ snaps to 45°")
        case .rect: return ("Rectangle", "r", "rectangle", "Drag to draw  ·  ⇧ for a square")
        case .ellipse: return ("Ellipse", "e", "circle", "Drag to draw  ·  ⇧ for a circle")
        case .pen: return ("Pen", "p", "scribble", "Draw freehand")
        case .highlight: return ("Highlighter", "h", "highlighter", "Drag over text to highlight it")
        case .text: return ("Text", "t", "textformat", "Click to type  ·  ↩ finishes  ·  ⇧↩ for a new line")
        case .step: return ("Step number", "n", "1.circle", "Click to drop numbered markers 1, 2, 3…")
        case .blur: return ("Blur", "b", "drop", "Drag over an area to blur it  ·  size sets the strength")
        case .pixelate: return ("Pixelate", "x", "square.grid.3x3", "Drag over sensitive information  ·  ⌘R auto-redacts")
        case .spotlight: return ("Spotlight", "s", "flashlight.on.fill", "Drag to keep an area bright and dim everything else")
        case .magnify: return ("Magnifier", "m", "plus.magnifyingglass", "Drag from the detail to where the zoomed bubble should go")
        case .crop: return ("Crop", "c", "crop", "Drag the area to keep")
        case .canvas: return ("Canvas", "k", "arrow.up.left.and.arrow.down.right.square", "Drag an edge to add space  ·  ⌥ drags both sides")
        case .image: return ("Insert image", "i", "photo.badge.plus", "Drop, paste or pick a screenshot  ·  drag a corner to resize, ⇧ for free resize")
        }
    }
    var isPixel: Bool { self == .blur || self == .pixelate || self == .magnify }
    // Changes to these redraw the background raster (image layers sit under blur/pixelate).
    var isRaster: Bool { isPixel || self == .image }
    var isRect: Bool { [.rect, .ellipse, .highlight, .blur, .pixelate, .spotlight, .crop].contains(self) }
}

let kColors: [NSColor] = [Theme.rgb(255, 59, 48), Theme.rgb(255, 149, 0), Theme.rgb(255, 204, 0), Theme.rgb(52, 199, 89),
                          Theme.rgb(10, 132, 255), Theme.accent, Theme.rgb(255, 255, 255), Theme.rgb(24, 24, 24)]
let kColorNames = ["Red", "Orange", "Yellow", "Green", "Blue", "Ather lime", "White", "Black"]
let kLevels = 5
let kStroke: [CGFloat] = [2, 3, 5, 8, 12]
let kTextPx: [CGFloat] = [16, 22, 30, 42, 58]
let kStepR: [CGFloat] = [11, 14, 18, 23, 30]
let kBlurR: [CGFloat] = [3, 6, 10, 15, 22]
let kMagR: [CGFloat] = [36, 48, 64, 84, 110]  // magnifier bubble radius (zoom is 2×)

// A screenshot placed on the canvas. Always flattened on export.
struct ImageLayer {
    var image: CGImage
    var source: URL?
    var radius: CGFloat = 0
    var shadow = false
    var border = false
    var opacity: CGFloat = 1
    var slot: Int?             // index into the collage's images, when part of a collage layout
}

struct Annot: Codable {
    var tool: Tool
    var color: Int
    var level: Int
    var unit: CGFloat      // capture scale when created, so strokes look the same on Retina captures
    var pts: [CGPoint]
    var text = ""
    var step = 0
    var wrap: CGFloat?     // text: wrap width, so notes stay on the canvas
    var layer: ImageLayer?

    private enum CodingKeys: String, CodingKey { case tool, color, level, unit, pts, text, step, wrap }

    var nsColor: NSColor { kColors[min(max(0, color), kColors.count - 1)] }
    var strokeW: CGFloat { kStroke[level] * unit }
    var rect: CGRect { Geo.norm(pts[0], pts.last!) }
    var font: NSFont { .systemFont(ofSize: kTextPx[level] * unit, weight: .bold) }

    func moved(_ dx: CGFloat, _ dy: CGFloat) -> Annot {
        var a = self
        a.pts = pts.map { CGPoint(x: $0.x + dx, y: $0.y + dy) }
        return a
    }
}

struct DocState {
    var annots: [Annot] = []
    var crop: CGRect?      // the document frame in image coordinates; inside the image it crops, outside it adds space
    var fill: CGColor?     // background for space outside the image; nil is transparent
}

enum Render {
    static func textAttrs(_ a: Annot) -> [NSAttributedString.Key: Any] {
        let sh = NSShadow()
        sh.shadowBlurRadius = 3 * a.unit
        sh.shadowOffset = .zero
        sh.shadowColor = (a.nsColor.isDark ? NSColor.white : NSColor.black).withAlphaComponent(0.55)
        return [.font: a.font, .foregroundColor: a.nsColor, .shadow: sh]
    }

    static func textRect(_ a: Annot) -> CGRect {
        let s = NSAttributedString(string: a.text.isEmpty ? "Ag" : a.text, attributes: textAttrs(a))
        var r = s.boundingRect(with: CGSize(width: a.wrap ?? 100_000, height: 100_000), options: [.usesLineFragmentOrigin])
        r.origin = a.pts[0]
        if a.text.isEmpty { r.size.width = 0 }
        return r
    }

    static func bounds(_ a: Annot) -> CGRect {
        switch a.tool {
        case .text: return textRect(a)
        case .step:
            let r = kStepR[a.level] * a.unit
            return CGRect(x: a.pts[0].x - r, y: a.pts[0].y - r, width: 2 * r, height: 2 * r)
        case .pen:
            let xs = a.pts.map(\.x), ys = a.pts.map(\.y)
            return CGRect(x: xs.min()!, y: ys.min()!, width: xs.max()! - xs.min()!, height: ys.max()! - ys.min()!).insetBy(dx: -a.strokeW, dy: -a.strokeW)
        case .magnify:
            let R = kMagR[a.level] * a.unit, c = a.pts.last!, s = a.pts[0]
            return CGRect(x: c.x - R, y: c.y - R, width: 2 * R, height: 2 * R).union(CGRect(x: s.x - R / 2, y: s.y - R / 2, width: R, height: R))
        case .arrow, .line: return a.rect.insetBy(dx: -a.strokeW * 2, dy: -a.strokeW * 2)
        default: return a.rect
        }
    }

    private static func distToSegment(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
        let dx = b.x - a.x, dy = b.y - a.y
        let l2 = dx * dx + dy * dy
        let t = l2 == 0 ? 0 : max(0, min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2))
        return hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy))
    }

    static func hit(_ a: Annot, _ p: CGPoint, slop: CGFloat) -> Bool {
        let tol = a.strokeW / 2 + slop
        switch a.tool {
        case .arrow, .line: return distToSegment(p, a.pts[0], a.pts.last!) <= tol
        case .pen: return zip(a.pts, a.pts.dropFirst()).contains { distToSegment(p, $0, $1) <= tol } || (a.pts.count == 1 && hypot(p.x - a.pts[0].x, p.y - a.pts[0].y) <= tol)
        case .rect, .ellipse:
            let r = a.rect
            return r.insetBy(dx: -tol, dy: -tol).contains(p) && !r.insetBy(dx: tol, dy: tol).contains(p)
        default: return bounds(a).insetBy(dx: -slop, dy: -slop).contains(p)
        }
    }

    // MARK: pixel tools (blur, pixelate, magnify), applied in order on top of the base image

    static func frame(_ base: CGImage, _ st: DocState) -> CGRect {
        let extent = CGRect(x: 0, y: 0, width: base.width, height: base.height)
        guard var f = st.crop?.integral, f.width >= 1, f.height >= 1 else { return extent }
        if !f.intersects(extent) && !st.annots.contains(where: { $0.tool == .image }) { f = extent }
        return f
    }

    // Everything under the annotations, covering the document frame: fill, the screenshot, image layers.
    static func raster(_ base: CGImage, _ st: DocState) -> (CGImage, CGRect) {
        let extent = CGRect(x: 0, y: 0, width: base.width, height: base.height)
        let f = frame(base, st)
        let layers = st.annots.filter { $0.tool == .image && $0.layer != nil }
        if f == extent && layers.isEmpty { return (base, f) }
        guard let ctx = makeContext(width: Int(f.width), height: Int(f.height)) else { return (base, extent) }
        ctx.translateBy(x: -f.minX, y: -f.minY)
        if let fill = st.fill {
            ctx.setFillColor(fill)
            ctx.fill(f)
        }
        drawImageFlipped(ctx, base, in: extent)
        for a in layers { drawLayer(a, ctx) }
        return (ctx.makeImage() ?? base, f)
    }

    static func drawLayer(_ a: Annot, _ ctx: CGContext) {
        guard let l = a.layer else { return }
        let r = a.rect
        guard r.width >= 1, r.height >= 1 else { return }
        let path = CGPath(roundedRect: r, cornerWidth: min(l.radius, r.width / 2), cornerHeight: min(l.radius, r.height / 2), transform: nil)
        ctx.saveGState()
        ctx.setAlpha(l.opacity)
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)
        if l.shadow {
            ctx.saveGState()
            let d = max(r.width, r.height)
            ctx.setShadow(offset: CGSize(width: 0, height: min(10 * a.unit, d * 0.012)), blur: min(30 * a.unit, d * 0.04), color: NSColor.black.withAlphaComponent(0.45).cgColor)
            ctx.addPath(path)
            ctx.setFillColor(NSColor.black.cgColor)
            ctx.fillPath()
            ctx.restoreGState()
        }
        ctx.saveGState()
        ctx.addPath(path)
        ctx.clip()
        ctx.interpolationQuality = .high
        drawImageFlipped(ctx, l.image, in: r)
        ctx.restoreGState()
        if l.border {
            ctx.addPath(path)
            ctx.setStrokeColor(a.nsColor.cgColor)
            ctx.setLineWidth(a.strokeW)
            ctx.strokePath()
        }
        ctx.endTransparencyLayer()
        ctx.restoreGState()
    }

    // The raster with blur, pixelate and magnify applied in order. Returned with the rect it covers.
    static func pixelBase(_ base: CGImage, _ st: DocState) -> (CGImage, CGRect) {
        let (bg, f) = raster(base, st)
        let pix = st.annots.filter { $0.tool.isPixel }
        guard !pix.isEmpty else { return (bg, f) }
        let H = f.height
        func ci(_ r: CGRect) -> CGRect { CGRect(x: r.minX - f.minX, y: H - (r.maxY - f.minY), width: r.width, height: r.height) }
        func ci(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x - f.minX, y: H - (p.y - f.minY)) }
        let extent = CGRect(x: 0, y: 0, width: bg.width, height: bg.height)
        var cur = CIImage(cgImage: bg)
        for a in pix {
            switch a.tool {
            case .blur:
                let r = ci(a.rect).intersection(extent)
                guard !r.isEmpty else { continue }
                let b = cur.clampedToExtent().applyingGaussianBlur(sigma: Double(max(1, kBlurR[a.level] * a.unit))).cropped(to: r)
                cur = b.composited(over: cur)
            case .pixelate:
                let r = ci(a.rect).intersection(extent)
                guard !r.isEmpty else { continue }
                let block = max(4, (10 * a.unit).rounded())
                let p = cur.clampedToExtent().applyingFilter("CIPixellate", parameters: [kCIInputScaleKey: block, kCIInputCenterKey: CIVector(x: r.minX, y: r.minY)]).cropped(to: r)
                cur = p.composited(over: cur)
            case .magnify:
                let R = kMagR[a.level] * a.unit
                let s = ci(a.pts[0]), c = ci(a.pts.last!)
                let zoomed = cur.clampedToExtent()
                    .transformed(by: CGAffineTransform(translationX: -s.x, y: -s.y).concatenating(CGAffineTransform(scaleX: 2, y: 2))
                        .concatenating(CGAffineTransform(translationX: c.x, y: c.y)))
                let mask = CIFilter(name: "CIRadialGradient", parameters: [
                    "inputCenter": CIVector(x: c.x, y: c.y), "inputRadius0": R - 0.75, "inputRadius1": R + 0.75,
                    "inputColor0": CIColor.white, "inputColor1": CIColor.clear,
                ])!.outputImage!
                cur = zoomed.applyingFilter("CIBlendWithAlphaMask", parameters: [kCIInputBackgroundImageKey: cur, kCIInputMaskImageKey: mask]).cropped(to: extent)
            default: break
            }
        }
        return (sharedCIContext.createCGImage(cur, from: extent) ?? bg, f)
    }

    // MARK: vector drawing in image coordinates on a flipped (y-down) context

    static func spotlight(_ ctx: CGContext, _ annots: [Annot], extent: CGRect, extra: CGRect? = nil) {
        var spots = annots.filter { $0.tool == .spotlight }.map(\.rect)
        if let extra { spots.append(extra) }
        guard !spots.isEmpty else { return }
        let path = CGMutablePath()
        path.addRect(extent)
        for r in spots { path.addPath(CGPath(roundedRect: r, cornerWidth: min(8, r.width / 4), cornerHeight: min(8, r.height / 4), transform: nil)) }
        ctx.saveGState()
        ctx.addPath(path)
        ctx.setFillColor(NSColor.black.withAlphaComponent(0.55).cgColor)
        ctx.fillPath(using: .evenOdd)
        ctx.restoreGState()
    }

    static func arrowHead(_ a: Annot) -> (tip: CGPoint, left: CGPoint, right: CGPoint, base: CGPoint) {
        let p0 = a.pts[0], p1 = a.pts.last!
        let ang = atan2(p1.y - p0.y, p1.x - p0.x)
        let len = min(max(a.strokeW * 3.2, 12 * a.unit), hypot(p1.x - p0.x, p1.y - p0.y) * 0.6 + a.strokeW)
        let half = len * 0.55
        let base = CGPoint(x: p1.x - cos(ang) * len, y: p1.y - sin(ang) * len)
        return (p1, CGPoint(x: base.x + sin(ang) * half, y: base.y - cos(ang) * half),
                CGPoint(x: base.x - sin(ang) * half, y: base.y + cos(ang) * half), base)
    }

    static func draw(_ a: Annot, _ ctx: CGContext, preview: Bool = false) {
        let col = a.nsColor.cgColor
        ctx.saveGState()
        defer { ctx.restoreGState() }
        ctx.setStrokeColor(col)
        ctx.setFillColor(col)
        ctx.setLineWidth(a.strokeW)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        let shadow = { ctx.setShadow(offset: CGSize(width: 0, height: 1 * a.unit), blur: 3 * a.unit, color: NSColor.black.withAlphaComponent(0.35).cgColor) }
        switch a.tool {
        case .arrow:
            shadow()
            let h = arrowHead(a)
            ctx.beginTransparencyLayer(auxiliaryInfo: nil)
            ctx.strokeLineSegments(between: [a.pts[0], h.base])
            ctx.move(to: h.tip)
            ctx.addLine(to: h.left)
            ctx.addLine(to: h.right)
            ctx.closePath()
            ctx.setLineWidth(a.strokeW * 0.5)
            ctx.drawPath(using: .fillStroke)
            ctx.endTransparencyLayer()
        case .line:
            shadow()
            ctx.strokeLineSegments(between: [a.pts[0], a.pts.last!])
        case .rect:
            shadow()
            ctx.stroke(a.rect)
        case .ellipse:
            shadow()
            ctx.strokeEllipse(in: a.rect)
        case .pen:
            shadow()
            if a.pts.count == 1 {
                ctx.fillEllipse(in: CGRect(x: a.pts[0].x - a.strokeW / 2, y: a.pts[0].y - a.strokeW / 2, width: a.strokeW, height: a.strokeW))
            } else {
                ctx.addLines(between: a.pts)
                ctx.strokePath()
            }
        case .highlight:
            ctx.setBlendMode(.multiply)
            ctx.setFillColor(a.nsColor.withAlphaComponent(0.55).cgColor)
            ctx.fill(a.rect)
        case .text:
            withNSContext(ctx, flipped: true) {
                NSAttributedString(string: a.text, attributes: textAttrs(a)).draw(with: textRect(a), options: [.usesLineFragmentOrigin])
            }
        case .step:
            let r = kStepR[a.level] * a.unit, c = a.pts[0]
            let box = CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)
            shadow()
            ctx.fillEllipse(in: box)
            ctx.setShadow(offset: .zero, blur: 0, color: nil)
            ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.9).cgColor)
            ctx.setLineWidth(max(1.5, 2 * a.unit))
            ctx.strokeEllipse(in: box.insetBy(dx: a.unit, dy: a.unit))
            let s = NSAttributedString(string: "\(a.step)", attributes: [
                .font: NSFont.systemFont(ofSize: r * 1.05, weight: .bold),
                .foregroundColor: a.nsColor.isDark ? NSColor.white : Theme.bg,
            ])
            let sz = s.size()
            withNSContext(ctx, flipped: true) { s.draw(at: CGPoint(x: c.x - sz.width / 2, y: c.y - sz.height / 2)) }
        case .magnify:
            let R = kMagR[a.level] * a.unit, s = a.pts[0], c = a.pts.last!
            ctx.setStrokeColor(NSColor.white.cgColor)
            ctx.setLineWidth(max(1, 1.5 * a.unit))
            ctx.strokeEllipse(in: CGRect(x: s.x - R / 2, y: s.y - R / 2, width: R, height: R))
            let d = hypot(c.x - s.x, c.y - s.y)
            if d > R * 1.5 {
                let ux = (c.x - s.x) / d, uy = (c.y - s.y) / d
                ctx.strokeLineSegments(between: [CGPoint(x: s.x + ux * R / 2, y: s.y + uy * R / 2), CGPoint(x: c.x - ux * R, y: c.y - uy * R)])
            }
            shadow()
            ctx.setLineWidth(3 * a.unit)
            ctx.strokeEllipse(in: CGRect(x: c.x - R, y: c.y - R, width: 2 * R, height: 2 * R))
        case .blur, .pixelate, .spotlight, .crop:
            if preview {
                ctx.setStrokeColor(NSColor.white.cgColor)
                ctx.setLineWidth(max(1, a.unit))
                ctx.setLineDash(phase: 0, lengths: [5 * a.unit, 4 * a.unit])
                ctx.stroke(a.rect)
            }
        case .select, .canvas, .image: break
        }
    }

    // Full composition in image pixels, with the crop applied.
    // Full composition in document pixels: the frame (crop or extra space), image layers and annotations, flattened.
    static func compose(_ base: CGImage, _ st: DocState, pixel: (CGImage, CGRect)? = nil) -> CGImage {
        let f = frame(base, st)
        let (px, pr) = pixel.flatMap { $0.1 == f ? $0 : nil } ?? pixelBase(base, st)  // never fall back to the unannotated base
        guard let ctx = makeContext(width: Int(f.width), height: Int(f.height)) else { return px }
        ctx.translateBy(x: -f.minX, y: -f.minY)
        drawImageFlipped(ctx, px, in: pr)
        spotlight(ctx, st.annots, extent: f)
        for a in st.annots where !a.tool.isRaster || a.tool == .magnify { draw(a, ctx) }
        return ctx.makeImage() ?? base
    }

    // Average color along the image border: a fill that makes added space look like part of the screenshot.
    static func edgeColor(_ img: CGImage) -> CGColor {
        let n = 32
        guard let ctx = makeContext(width: n, height: n, flipped: false) else { return NSColor.white.cgColor }
        ctx.interpolationQuality = .medium
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: n, height: n))
        guard let data = ctx.data else { return NSColor.white.cgColor }
        let p = data.bindMemory(to: UInt8.self, capacity: ctx.bytesPerRow * n)
        var r = 0, g = 0, b = 0, a = 0, count = 0
        for y in 0..<n { for x in 0..<n where x == 0 || y == 0 || x == n - 1 || y == n - 1 {
            let o = y * ctx.bytesPerRow + x * 4   // BGRA, premultiplied
            b += Int(p[o]); g += Int(p[o + 1]); r += Int(p[o + 2]); a += Int(p[o + 3]); count += 1
        } }
        guard a > 0 else { return NSColor.white.cgColor }
        return CGColor(srgbRed: CGFloat(r) / CGFloat(a), green: CGFloat(g) / CGFloat(a), blue: CGFloat(b) / CGFloat(a), alpha: 1)
    }

    // Presentation export: gradient backdrop, padding, soft shadow and rounded corners.
    static func styled(_ img: CGImage, unit u: CGFloat) -> CGImage {
        let w = CGFloat(img.width), h = CGFloat(img.height)
        let pad = (min(max(min(w, h) * 0.08, 24 * u), 72 * u)).rounded()
        let W = Int(w + 2 * pad), H = Int(h + 2 * pad)
        guard let ctx = makeContext(width: W, height: H) else { return img }
        let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [Theme.rgb(124, 108, 255).cgColor, Theme.rgb(64, 170, 255).cgColor] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(grad, start: .zero, end: CGPoint(x: W, y: H), options: [])
        let r = 12 * u
        let box = CGRect(x: pad, y: pad, width: w, height: h)
        let path = CGPath(roundedRect: box, cornerWidth: r, cornerHeight: r, transform: nil)
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: 8 * u), blur: 36 * u, color: NSColor.black.withAlphaComponent(0.45).cgColor)
        ctx.addPath(path)
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fillPath()
        ctx.restoreGState()
        ctx.addPath(path)
        ctx.clip()
        drawImageFlipped(ctx, img, in: box)
        return ctx.makeImage() ?? img
    }
}
