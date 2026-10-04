import AVFoundation
import AppKit
import CoreImage

// Video markup items and the frame renderer shared by the editor preview and the export,
// so what you see while editing is what gets saved.

enum MarkKind: String, CaseIterable {
    case text, bubble, emoji, arrow, box, ellipse, step, blur, pixelate, zoom, title

    var label: String {
        switch self {
        case .text: return "Text"
        case .bubble: return "Speech bubble"
        case .emoji: return "Emoji"
        case .arrow: return "Arrow"
        case .box: return "Box"
        case .ellipse: return "Ellipse"
        case .step: return "Step number"
        case .blur: return "Blur"
        case .pixelate: return "Pixelate"
        case .zoom: return "Zoom"
        case .title: return "Title card"
        }
    }
    var plural: String { self == .box ? "boxes" : self == .emoji ? "emoji" : label.lowercased() + "s" }
    var symbol: String {
        switch self {
        case .text: return "textformat"
        case .bubble: return "text.bubble"
        case .emoji: return "face.smiling"
        case .arrow: return "arrow.up.right"
        case .box: return "rectangle"
        case .ellipse: return "circle"
        case .step: return "1.circle"
        case .blur: return "drop"
        case .pixelate: return "square.grid.3x3"
        case .zoom: return "plus.magnifyingglass"
        case .title: return "rectangle.inset.filled"
        }
    }
    var key: String {
        switch self {
        case .arrow: return "A"
        case .box: return "R"
        case .emoji: return "E"
        case .step: return "N"
        case .blur: return "X"
        case .zoom: return "Z"
        default: return ""
        }
    }
    var isLine: Bool { self == .arrow }
    var hasText: Bool { [.text, .bubble, .emoji, .title].contains(self) }
    var isRegion: Bool { [.blur, .pixelate, .zoom].contains(self) }
    var isStroke: Bool { [.arrow, .box, .ellipse].contains(self) }
    var defaultSeconds: Double { self == .title ? 2.5 : 3 }

    // What "Auto" means for each kind.
    var defaultStyle: AnimStyle {
        switch self {
        case .arrow, .box, .ellipse: return .drawOn
        case .step, .emoji, .bubble: return .pop
        case .text: return .slide
        case .title: return .fade
        case .blur, .pixelate, .zoom: return .none
        }
    }

    // The styles that make sense for this kind, so menus stay short.
    var styles: [AnimStyle] {
        switch self {
        case .arrow, .box, .ellipse: return [.none, .fade, .drawOn, .pop, .scale]
        case .step, .emoji: return [.none, .fade, .pop, .scale, .slide]
        case .text, .bubble: return [.none, .fade, .pop, .slide, .wipe, .blurIn, .typewriter]
        case .title: return [.none, .fade, .slide, .wipe, .blurIn]
        case .blur, .pixelate: return [.none, .fade, .blurIn]
        case .zoom: return []
        }
    }
}

// One animation style plays the item in and (mirrored) out.
enum AnimStyle: String, CaseIterable {
    case auto, none, fade, pop, scale, slide, wipe, blurIn, drawOn, typewriter
    var label: String {
        switch self {
        case .auto: return "Auto"
        case .none: return "None"
        case .fade: return "Fade"
        case .pop: return "Pop"
        case .scale: return "Scale"
        case .slide: return "Slide up"
        case .wipe: return "Wipe"
        case .blurIn: return "Blur in"
        case .drawOn: return "Draw on"
        case .typewriter: return "Typewriter"
        }
    }
    static let captionStyles: [AnimStyle] = [.auto, .none, .fade, .pop, .slide, .typewriter]
}

enum Emphasis: String, CaseIterable {
    case none, pulse, bounce, shake, ping
    var label: String { ["none": "None", "pulse": "Pulse", "bounce": "Bounce", "shake": "Shake", "ping": "Ping"][rawValue]! }
}

enum CaptionLook: Int, CaseIterable {
    case pill, outline, bar
    var label: String { ["Pill", "Outline", "Bar"][rawValue] }
}

struct Mark: Equatable {
    var id = UUID()
    var kind: MarkKind
    var start: Double            // seconds in the source video
    var end: Double
    var a: CGPoint               // rect corners, or arrow tail → head; video pixels, top-left origin
    var b: CGPoint
    var text = ""
    var subtitle = ""
    var color = 0                // index into kColors
    var level = 2
    var step = 1
    var style = AnimStyle.auto
    var exit: AnimStyle?         // advanced: a different exit; nil mirrors `style`
    var emphasis = Emphasis.none
    var snappy = false           // zoom: quick instead of smooth

    var rect: CGRect { Geo.norm(a, b) }
    func active(_ t: Double) -> Bool { t >= start && t < end }
    var inStyle: AnimStyle { style == .auto ? kind.defaultStyle : style }
    var outStyle: AnimStyle {
        let s = exit.map { $0 == .auto ? kind.defaultStyle : $0 } ?? inStyle
        return s == .drawOn || s == .typewriter ? .fade : s   // these don't play backwards well
    }
}

// How an item looks at one moment of its entrance, exit or emphasis.
struct Motion: Equatable {
    var alpha: CGFloat = 1
    var scale: CGFloat = 1
    var dx: CGFloat = 0          // video pixels; positive is right / down
    var dy: CGFloat = 0
    var reveal: CGFloat = 1      // 0…1: wipe, draw on, typewriter
    var wipe = false
    var blur: CGFloat = 0
    var ring: CGFloat?           // ping: 0…1 phase of the ring
}

// MARK: - Frame renderer

final class FrameRenderer {
    let edit: VideoEdit
    let full: CGSize             // source frame, pixels
    let view: CGRect             // the crop (or the whole frame), top-left origin
    let out: CGSize              // output pixels
    let preview: Bool            // preview: output stays in place inside the full frame, placeholders show
    var zoomInPreview = false    // preview applies zoom only while playing, so editing stays put
    private let unit: CGFloat

    private static var cache: [String: CGImage] = [:]
    private static let lock = NSLock()

    init(edit: VideoEdit, full: CGSize, preview: Bool) {
        self.edit = edit
        self.full = full
        self.preview = preview
        let f = CGRect(origin: .zero, size: full)
        var v = (edit.crop ?? f).intersection(f).integral
        if v.isNull || v.width < 16 || v.height < 16 { v = f }
        view = v
        out = preview ? full : CGSize(width: floor(v.width / 2) * 2, height: floor(v.height / 2) * 2)   // H.264 wants even sizes
        unit = max(1, full.height / 720)
    }

    // `src`: source frame. `t`: source time in seconds.
    func render(_ src: CIImage, at t: Double) -> CIImage {
        let H = full.height
        var img = src
        // 1. Blur and pixelate, in the order they were added.
        for m in edit.marks where (m.kind == .blur || m.kind == .pixelate) && m.active(t) {
            let r = ci(m.rect, H).intersection(img.extent)
            guard !r.isEmpty else { continue }
            let mo = motion(m, t)
            var fx: CIImage
            let k = mo.blur > 0 ? max(0.15, 1 - mo.blur / (12 * unit)) : 1   // blur in: the effect strengthens
            if m.kind == .blur {
                fx = img.clampedToExtent().applyingGaussianBlur(sigma: Double(k * max(4, min(r.width, r.height) * [0.03, 0.05, 0.08, 0.12, 0.18][m.level]))).cropped(to: r)
            } else {
                let block = max(6, k * min(r.width, r.height) * [0.04, 0.07, 0.1, 0.14, 0.2][m.level])
                fx = img.clampedToExtent().applyingFilter("CIPixellate", parameters: [kCIInputScaleKey: block, kCIInputCenterKey: CIVector(x: r.minX, y: r.minY)]).cropped(to: r)
            }
            if mo.alpha < 1 { fx = faded(fx, mo.alpha) }
            img = fx.composited(over: img)
        }
        // 2. Markup that sits on the video (moves with zoom).
        for m in edit.marks where !m.kind.isRegion && m.kind != .title && m.active(t) {
            let mo = motion(m, t)
            guard mo.alpha > 0.001, let (cg, r) = markImage(m, reveal: mo.wipe ? 1 : mo.reveal) else { continue }
            if let phase = mo.ring, let ring = ringImage(r) { img = place(ring.0, ring.1, H, Motion(alpha: (1 - phase) * 0.8, scale: 1 + phase * 0.5)).composited(over: img) }
            img = place(cg, r, H, mo).composited(over: img)
        }
        // 3. The visible area: the crop, or a zoom into it.
        let r = viewRect(at: t)
        let target = preview ? ci(view, H) : CGRect(origin: .zero, size: out)
        let rc = ci(r, H)
        let tf = CGAffineTransform(translationX: -rc.minX, y: -rc.minY)
            .concatenating(CGAffineTransform(scaleX: target.width / rc.width, y: target.height / rc.height))
            .concatenating(CGAffineTransform(translationX: target.minX, y: target.minY))
        var framed = img.transformed(by: tf).cropped(to: target)
        // 4. Captions and title cards stay put on screen.
        let tsize = target.size
        let origin = preview ? view.origin : .zero
        let OH = preview ? H : out.height
        for c in edit.captions where c.active(t) && (preview || !c.text.trimmingCharacters(in: .whitespaces).isEmpty) {
            let mo = captionMotion(c, t)
            guard mo.alpha > 0.001, let (cg, pr) = captionImage(c, in: tsize, at: t, reveal: mo.reveal) else { continue }
            framed = place(cg, pr.offsetBy(dx: origin.x, dy: origin.y), OH, mo).composited(over: framed)
        }
        for m in edit.marks where m.kind == .title && m.active(t) {
            let mo = motion(m, t)
            guard let cg = titleImage(m, size: tsize) else { continue }
            framed = place(cg, CGRect(origin: origin, size: tsize), OH, mo).composited(over: framed)
        }
        if preview { return framed.composited(over: img) }
        return framed.composited(over: CIImage(color: .black).cropped(to: target))
    }

    func viewRect(at t: Double) -> CGRect {
        guard !preview || zoomInPreview, let z = edit.marks.first(where: { $0.kind == .zoom && $0.active(t) }) else { return view }
        let target = FrameRenderer.zoomTarget(z.rect, view: view)
        let ramp = min(z.snappy ? 0.18 : 0.45, (z.end - z.start) / 3)
        let p = ease(min(1, min(t - z.start, z.end - t) / max(0.01, ramp)))
        return CGRect(x: view.minX + (target.minX - view.minX) * p, y: view.minY + (target.minY - view.minY) * p,
                      width: view.width + (target.width - view.width) * p, height: view.height + (target.height - view.height) * p)
    }

    // The zoom rect grown to the output's shape and kept inside the visible area.
    static func zoomTarget(_ r: CGRect, view v: CGRect) -> CGRect {
        let k = v.width / v.height
        var w = max(r.width, r.height * k, v.width / 6), h = w / k
        if h > v.height { h = v.height; w = h * k }
        var x = r.midX - w / 2, y = r.midY - h / 2
        x = min(max(v.minX, x), v.maxX - w)
        y = min(max(v.minY, y), v.maxY - h)
        return CGRect(x: x, y: y, width: w, height: h)
    }

    private func ease(_ x: Double) -> CGFloat { CGFloat(x * x * (3 - 2 * x)) }
    private func easeOut(_ x: CGFloat) -> CGFloat { 1 - pow(1 - x, 3) }

    // MARK: motion

    func motion(_ m: Mark, _ t: Double) -> Motion {
        var mo = Motion.between(m.inStyle, m.outStyle, start: m.start, end: m.end, t: t, chars: m.text.count, height: full.height)
        if m.emphasis != .none { emphasize(&mo, m.emphasis, since: t - m.start) }
        return mo
    }

    func captionMotion(_ c: Caption, _ t: Double) -> Motion {
        let s = edit.captionStyle == .auto ? AnimStyle.fade : edit.captionStyle
        return Motion.between(s, s == .typewriter ? .fade : s, start: c.start, end: c.end, t: t, chars: c.text.count, height: full.height)
    }

    private func emphasize(_ mo: inout Motion, _ e: Emphasis, since a: Double) {
        let u = Double(full.height)
        switch e {
        case .none: break
        case .pulse: mo.scale *= CGFloat(1 + 0.045 * sin(a * 2 * .pi / 1.2))
        case .bounce: mo.dy -= CGFloat(abs(sin(a * .pi / 0.55)) * u * 0.018)
        case .shake:
            let c = a.truncatingRemainder(dividingBy: 2)   // a short shake every two seconds
            if c < 0.45 { mo.dx += CGFloat(sin(c * 2 * .pi * 9) * (1 - c / 0.45) * u * 0.008) }
        case .ping: mo.ring = CGFloat(a.truncatingRemainder(dividingBy: 1.4) / 1.4)
        }
    }

    // MARK: drawing helpers

    private func ci(_ r: CGRect, _ H: CGFloat) -> CGRect { CGRect(x: r.minX, y: H - r.maxY, width: r.width, height: r.height) }

    private func faded(_ i: CIImage, _ alpha: CGFloat) -> CIImage {
        i.applyingFilter("CIColorMatrix", parameters: ["inputAVector": CIVector(x: 0, y: 0, z: 0, w: alpha)])
    }

    private func place(_ cg: CGImage, _ r: CGRect, _ H: CGFloat, _ mo: Motion) -> CIImage {
        let c = ci(r, H)
        var i = CIImage(cgImage: cg).transformed(by: CGAffineTransform(scaleX: c.width / CGFloat(cg.width), y: c.height / CGFloat(cg.height)))
            .transformed(by: CGAffineTransform(translationX: c.minX, y: c.minY))
        if mo.wipe && mo.reveal < 1 { i = i.cropped(to: CGRect(x: c.minX, y: c.minY - c.height, width: c.width * max(0, mo.reveal), height: c.height * 3)) }
        if mo.blur > 0.3 { i = i.clampedToExtent().applyingGaussianBlur(sigma: Double(mo.blur)).cropped(to: i.extent.insetBy(dx: -mo.blur * 3, dy: -mo.blur * 3)) }
        if mo.scale != 1 {
            i = i.transformed(by: CGAffineTransform(translationX: -c.midX, y: -c.midY).concatenating(CGAffineTransform(scaleX: mo.scale, y: mo.scale))
                .concatenating(CGAffineTransform(translationX: c.midX, y: c.midY)))
        }
        if mo.dx != 0 || mo.dy != 0 { i = i.transformed(by: CGAffineTransform(translationX: mo.dx, y: -mo.dy)) }
        return mo.alpha < 1 ? faded(i, max(0, mo.alpha)) : i
    }

    private func cached(_ key: String, _ make: () -> CGImage?) -> CGImage? {
        FrameRenderer.lock.lock()
        if let c = FrameRenderer.cache[key] { FrameRenderer.lock.unlock(); return c }
        FrameRenderer.lock.unlock()
        guard let img = make() else { return nil }
        FrameRenderer.lock.lock()
        if FrameRenderer.cache.count > 400 { FrameRenderer.cache.removeAll() }
        FrameRenderer.cache[key] = img
        FrameRenderer.lock.unlock()
        return img
    }

    private func typed(_ s: String, _ reveal: CGFloat) -> String {
        reveal >= 1 ? s : String(s.prefix(Int((CGFloat(s.count) * reveal).rounded(.up))))
    }

    // A mark drawn on its own, with the rect it covers in video coordinates.
    // `reveal` < 1 draws an arrow, box or ellipse partway (draw on), or text partway (typewriter).
    func markImage(_ m: Mark, reveal: CGFloat = 1) -> (CGImage, CGRect)? {
        let u = unit
        let rv = (reveal * 30).rounded(.up) / 30   // a step per frame is plenty, and keeps the cache small
        if m.kind.isStroke && rv < 1 { return strokeOn(m, rv) }
        var annot: Annot?
        switch m.kind {
        case .arrow: annot = Annot(tool: .arrow, color: m.color, level: m.level, unit: u, pts: [m.a, m.b])
        case .box: annot = Annot(tool: .rect, color: m.color, level: m.level, unit: u, pts: [m.rect.origin, CGPoint(x: m.rect.maxX, y: m.rect.maxY)])
        case .ellipse: annot = Annot(tool: .ellipse, color: m.color, level: m.level, unit: u, pts: [m.rect.origin, CGPoint(x: m.rect.maxX, y: m.rect.maxY)])
        case .step: annot = Annot(tool: .step, color: m.color, level: m.level, unit: u, pts: [CGPoint(x: m.rect.midX, y: m.rect.midY)], step: m.step)
        case .text:
            let shown = m.text.isEmpty && preview ? "Text" : typed(m.text, rv)
            guard !shown.isEmpty else { return nil }
            annot = Annot(tool: .text, color: m.color, level: m.level, unit: u, pts: [m.rect.origin], text: shown, wrap: max(40, m.rect.width))
        default: break
        }
        if let a = annot {
            var b = Render.bounds(a).insetBy(dx: -8 * u, dy: -8 * u).integral
            if m.kind == .text {   // typewriter: keep the box the size of the whole text, so it doesn't drift
                var whole = a
                whole.text = m.text.isEmpty ? a.text : m.text
                b = Render.bounds(whole).insetBy(dx: -8 * u, dy: -8 * u).integral
            }
            let key = "a|\(m.kind)|\(a.pts)|\(a.color)|\(a.level)|\(a.text)|\(a.step)|\(u)|\(b)"
            guard let img = cached(key, {
                guard let ctx = makeContext(width: max(1, Int(b.width)), height: max(1, Int(b.height))) else { return nil }
                ctx.translateBy(x: -b.minX, y: -b.minY)
                Render.draw(a, ctx)
                return ctx.makeImage()
            }) else { return nil }
            return (img, b)
        }
        let r = m.rect.integral
        guard r.width >= 4, r.height >= 4 else { return nil }
        if m.kind == .emoji {
            let shown = m.text.isEmpty ? (preview ? "🙂" : "") : m.text
            guard !shown.isEmpty else { return nil }
            let key = "e|\(shown)|\(r.size)"
            guard let img = cached(key, {
                guard let ctx = makeContext(width: Int(r.width), height: Int(r.height)) else { return nil }
                var fs = r.height * 0.82
                var s = NSAttributedString(string: shown, attributes: [.font: NSFont.systemFont(ofSize: fs)])
                if s.size().width > r.width { fs *= r.width / s.size().width; s = NSAttributedString(string: shown, attributes: [.font: NSFont.systemFont(ofSize: fs)]) }
                let sz = s.size()
                withNSContext(ctx, flipped: true) { s.draw(at: CGPoint(x: (r.width - sz.width) / 2, y: (r.height - sz.height) / 2)) }
                return ctx.makeImage()
            }) else { return nil }
            return (img, r)
        }
        if m.kind == .bubble {
            let whole = m.text.isEmpty && preview ? "Say something" : m.text
            guard !whole.isEmpty else { return nil }
            let shown = m.text.isEmpty ? whole : typed(whole, rv)
            let tail = r.height * 0.32
            let full = CGRect(x: r.minX, y: r.minY, width: r.width, height: r.height + tail)
            let key = "b|\(shown)|\(whole)|\(r.size)|\(m.color)|\(m.level)"
            guard let img = cached(key, {
                guard let ctx = makeContext(width: Int(full.width), height: Int(full.height)) else { return nil }
                let body = CGRect(x: 2, y: 2, width: r.width - 4, height: r.height - 4)
                let path = CGMutablePath()
                path.addRoundedRect(in: body, cornerWidth: min(body.height / 2.5, 28 * u), cornerHeight: min(body.height / 2.5, 28 * u))
                path.move(to: CGPoint(x: body.minX + body.width * 0.18, y: body.maxY - 1))
                path.addLine(to: CGPoint(x: body.minX + body.width * 0.1, y: body.maxY + tail - 2))
                path.addLine(to: CGPoint(x: body.minX + body.width * 0.32, y: body.maxY - 1))
                let fill = kColors[min(max(0, m.color), kColors.count - 1)]
                ctx.setShadow(offset: CGSize(width: 0, height: 2 * u), blur: 6 * u, color: NSColor.black.withAlphaComponent(0.35).cgColor)
                ctx.addPath(path)
                ctx.setFillColor(fill.cgColor)
                ctx.fillPath()
                ctx.setShadow(offset: .zero, blur: 0, color: nil)
                let p = NSMutableParagraphStyle()
                p.alignment = .center
                var fs = kTextPx[m.level] * u * 0.8
                func attr() -> [NSAttributedString.Key: Any] { [.font: NSFont.systemFont(ofSize: fs, weight: .semibold), .foregroundColor: fill.isDark ? NSColor.white : NSColor.black, .paragraphStyle: p] }
                let inner = body.insetBy(dx: body.height * 0.28, dy: body.height * 0.14)
                // Sized by the whole text, so typing it out doesn't change the layout.
                var tb = NSAttributedString(string: whole, attributes: attr()).boundingRect(with: CGSize(width: inner.width, height: 10_000), options: [.usesLineFragmentOrigin])
                while tb.height > inner.height && fs > 8 { fs *= 0.9; tb = NSAttributedString(string: whole, attributes: attr()).boundingRect(with: CGSize(width: inner.width, height: 10_000), options: [.usesLineFragmentOrigin]) }
                withNSContext(ctx, flipped: true) {
                    NSAttributedString(string: shown, attributes: attr()).draw(with: CGRect(x: inner.minX, y: inner.midY - tb.height / 2, width: inner.width, height: tb.height + 2), options: [.usesLineFragmentOrigin])
                }
                return ctx.makeImage()
            }) else { return nil }
            return (img, full)
        }
        return nil
    }

    // Arrow, box or ellipse drawn partway along its outline.
    private func strokeOn(_ m: Mark, _ p: CGFloat) -> (CGImage, CGRect)? {
        let u = unit
        if m.kind == .arrow {   // the arrow grows from its tail, head first
            var g = m
            g.b = CGPoint(x: m.a.x + (m.b.x - m.a.x) * max(0.08, p), y: m.a.y + (m.b.y - m.a.y) * max(0.08, p))
            return markImage(g, reveal: 1)
        }
        let a = Annot(tool: m.kind == .box ? .rect : .ellipse, color: m.color, level: m.level, unit: u, pts: [m.rect.origin, CGPoint(x: m.rect.maxX, y: m.rect.maxY)])
        let b = Render.bounds(a).insetBy(dx: -8 * u, dy: -8 * u).integral
        let key = "s|\(m.kind)|\(m.rect)|\(m.color)|\(m.level)|\(p)|\(u)"
        guard let img = cached(key, {
            guard let ctx = makeContext(width: max(1, Int(b.width)), height: max(1, Int(b.height))) else { return nil }
            ctx.translateBy(x: -b.minX, y: -b.minY)
            let r = a.rect
            let path = m.kind == .box ? CGPath(rect: r, transform: nil) : CGPath(ellipseIn: r, transform: nil)
            let len = m.kind == .box ? 2 * (r.width + r.height) : .pi * (3 * (r.width + r.height) / 2 - sqrt((3 * r.width + r.height) * (r.width + 3 * r.height)) / 2)
            ctx.setShadow(offset: CGSize(width: 0, height: 1 * u), blur: 3 * u, color: NSColor.black.withAlphaComponent(0.35).cgColor)
            ctx.setStrokeColor(a.nsColor.cgColor)
            ctx.setLineWidth(a.strokeW)
            ctx.setLineCap(.round)
            ctx.setLineDash(phase: 0, lengths: [len * p, len * 2])
            ctx.addPath(path)
            ctx.strokePath()
            return ctx.makeImage()
        }) else { return nil }
        return (img, b)
    }

    // Ping: a ring around the item that spreads and fades.
    private func ringImage(_ r: CGRect) -> (CGImage, CGRect)? {
        let pad = max(6, min(r.width, r.height) * 0.12)
        let box = r.insetBy(dx: -pad, dy: -pad).integral
        let key = "r|\(box.size)"
        guard let img = cached(key, {
            guard let ctx = makeContext(width: Int(box.width), height: Int(box.height)) else { return nil }
            ctx.setStrokeColor(NSColor.white.cgColor)
            ctx.setLineWidth(max(2, 2.5 * unit))
            ctx.strokeEllipse(in: CGRect(origin: .zero, size: box.size).insetBy(dx: 3 * unit, dy: 3 * unit))
            return ctx.makeImage()
        }) else { return nil }
        return (img, box)
    }

    // A caption in output coordinates (top-left origin), in the video's caption look.
    func captionImage(_ c: Caption, in size: CGSize, at t: Double? = nil, reveal: CGFloat = 1) -> (CGImage, CGRect)? {
        let whole = c.text.isEmpty ? "Type a caption…" : c.text
        let look = edit.captionLook
        let textColor = kColors[min(max(0, edit.captionColor), kColors.count - 1)]
        let edgeColor = kColors[min(max(0, edit.captionEdge), kColors.count - 1)]
        let yellow = Theme.rgb(255, 214, 10)
        let highlight = edit.captionColor == 2 ? (edgeColor.isDark ? NSColor.white : NSColor.black) : yellow   // stands out from yellow text too
        let fontSize = max(14, size.height * VideoEdit.captionScale[edit.captionSize]) * (look == .outline ? 1.25 : 1)
        let maxW = size.width * (look == .bar ? 0.92 : 0.86)
        // The spoken word lights up, when the caption still matches its transcription.
        var hot: Range<String.Index>?
        if edit.highlightWords, let t, !c.words.isEmpty, c.words.map(\.text).joined(separator: " ") == c.text, let i = c.words.firstIndex(where: { t >= $0.start && t < $0.end }) {
            var lo = c.text.startIndex
            for w in c.words.prefix(i) { lo = c.text.index(lo, offsetBy: w.text.count + 1) }
            hot = lo..<c.text.index(lo, offsetBy: c.words[i].text.count)
        }
        let shown = typed(whole, (reveal * 30).rounded(.up) / 30)
        func attributed(_ s: String) -> NSAttributedString {
            let a = NSMutableAttributedString(string: s, attributes: VideoExport.captionAttributes(fontSize, dim: c.text.isEmpty, look: look, color: textColor, edge: edgeColor))
            if let hot, hot.upperBound <= s.endIndex { a.addAttribute(.foregroundColor, value: highlight, range: NSRange(hot, in: s)) }
            return a
        }
        let tb = attributed(whole).boundingRect(with: CGSize(width: maxW, height: 10_000), options: [.usesLineFragmentOrigin])
        let pad = fontSize * 0.45
        var w = ceil(tb.width) + pad * 2, h = ceil(tb.height) + pad * 1.2
        if look == .bar { w = size.width }
        let margin = look == .bar ? 0 : size.height * 0.06
        var y: CGFloat = c.position == .top ? margin : c.position == .middle ? (size.height - h) / 2 : size.height - margin - h
        var x = (size.width - w) / 2
        if let p = c.center {   // dragged: kept fully on screen
            x = look == .bar ? 0 : min(max(0, p.x * size.width - w / 2), size.width - w)
            y = min(max(0, p.y * size.height - h / 2), size.height - h)
        }
        let r = CGRect(x: x.rounded(), y: y.rounded(), width: w.rounded(.up), height: h.rounded(.up))
        let key = "c|\(shown)|\(whole)|\(r.size)|\(fontSize)|\(look)|\(hot.map { "\($0)" } ?? "")|\(edit.captionColor)|\(edit.captionEdge)"
        guard let img = cached(key, {
            guard let ctx = makeContext(width: Int(r.width), height: Int(r.height)) else { return nil }
            if look != .outline {
                let radius = look == .bar ? 0 : fontSize * 0.35
                ctx.addPath(CGPath(roundedRect: CGRect(origin: .zero, size: r.size), cornerWidth: radius, cornerHeight: radius, transform: nil))
                ctx.setFillColor(edgeColor.withAlphaComponent(look == .bar ? 0.7 : 0.62).cgColor)
                ctx.fillPath()
            }
            withNSContext(ctx, flipped: true) {
                attributed(shown).draw(with: CGRect(x: (r.width - ceil(tb.width)) / 2, y: pad * 0.6, width: ceil(tb.width) + 1, height: ceil(tb.height) + 2), options: [.usesLineFragmentOrigin])
            }
            return ctx.makeImage()
        }) else { return nil }
        return (img, r)
    }

    // A full-screen card: solid background, title and optional subtitle.
    func titleImage(_ m: Mark, size: CGSize) -> CGImage? {
        let title = m.text.isEmpty && preview ? "Title" : m.text
        let key = "t|\(title)|\(m.subtitle)|\(m.color)|\(size)"
        return cached(key) {
            guard let ctx = makeContext(width: Int(size.width), height: Int(size.height)) else { return nil }
            let bg = kColors[min(max(0, m.color), kColors.count - 1)]
            ctx.setFillColor(bg.cgColor)
            ctx.fill(CGRect(origin: .zero, size: size))
            let fg = bg.isDark ? NSColor.white : NSColor.black
            let p = NSMutableParagraphStyle()
            p.alignment = .center
            let t = NSAttributedString(string: title, attributes: [.font: NSFont.systemFont(ofSize: size.height * 0.085, weight: .bold), .foregroundColor: fg, .paragraphStyle: p])
            let s = NSAttributedString(string: m.subtitle, attributes: [.font: NSFont.systemFont(ofSize: size.height * 0.04, weight: .medium), .foregroundColor: fg.withAlphaComponent(0.7), .paragraphStyle: p])
            let w = size.width * 0.8
            let tb = t.boundingRect(with: CGSize(width: w, height: 10_000), options: [.usesLineFragmentOrigin])
            let sb = m.subtitle.isEmpty ? .zero : s.boundingRect(with: CGSize(width: w, height: 10_000), options: [.usesLineFragmentOrigin])
            let gap = m.subtitle.isEmpty ? 0 : size.height * 0.03
            let top = (size.height - tb.height - gap - sb.height) / 2
            withNSContext(ctx, flipped: true) {
                t.draw(with: CGRect(x: (size.width - w) / 2, y: top, width: w, height: tb.height + 2), options: [.usesLineFragmentOrigin])
                if !m.subtitle.isEmpty { s.draw(with: CGRect(x: (size.width - w) / 2, y: top + tb.height + gap, width: w, height: sb.height + 2), options: [.usesLineFragmentOrigin]) }
            }
            return ctx.makeImage()
        }
    }
}

extension Motion {
    // Entrance and exit of one item at time `t`. Each style has one tuned duration.
    static func between(_ inS: AnimStyle, _ outS: AnimStyle, start: Double, end: Double, t: Double, chars: Int, height: CGFloat) -> Motion {
        let len = max(0.05, end - start)
        func dur(_ s: AnimStyle, entering: Bool) -> Double {
            switch s {
            case .none, .auto: return 0
            case .pop: return entering ? 0.24 : 0.16
            case .drawOn: return min(0.6, len / 2)
            case .typewriter: return min(len * 0.6, max(0.3, Double(chars) * 0.035))
            case .wipe: return 0.4
            default: return entering ? 0.3 : 0.22
            }
        }
        var m = Motion()
        let di = min(dur(inS, entering: true), len / 2), dO = min(dur(outS, entering: false), len / 2)
        if di > 0, t - start < di { m.apply(inS, CGFloat((t - start) / di), height: height) }
        if dO > 0, end - t < dO {
            var o = Motion()
            o.apply(outS, CGFloat(max(0, end - t) / dO), height: height)
            m.alpha *= o.alpha; m.scale *= o.scale; m.dx += o.dx; m.dy += o.dy
            m.blur = max(m.blur, o.blur)
            if o.wipe { m.wipe = true; m.reveal = min(m.reveal, o.reveal) }
        }
        return m
    }

    // `p`: 0 (hidden) … 1 (fully shown).
    mutating func apply(_ s: AnimStyle, _ p: CGFloat, height: CGFloat) {
        let p = min(1, max(0, p))
        let out = 1 - pow(1 - p, 3)
        switch s {
        case .none, .auto: break
        case .fade: alpha = p
        case .pop:
            let back = 1 + 2.2 * pow(p - 1, 3) + 1.2 * pow(p - 1, 2)   // ease-out with a little overshoot
            alpha = min(1, p * 2.5)
            scale = 0.55 + 0.45 * back
        case .scale:
            alpha = p
            scale = 0.85 + 0.15 * out
        case .slide:
            alpha = p
            dy = (1 - out) * height * 0.035
        case .wipe:
            wipe = true
            reveal = out
        case .blurIn:
            alpha = min(1, p * 1.6)
            blur = (1 - out) * max(1, height / 720) * 12
        case .drawOn, .typewriter:
            reveal = p
        }
    }
}

extension Caption {
    func active(_ t: Double) -> Bool { t >= start && t < end }
}

// Lets the preview's video composition pick up edits without being rebuilt.
final class RendererBox {
    private let lock = NSLock()
    private var r: FrameRenderer
    init(_ r: FrameRenderer) { self.r = r }
    var renderer: FrameRenderer {
        get { lock.lock(); defer { lock.unlock() }; return r }
        set { lock.lock(); r = newValue; lock.unlock() }
    }
}
