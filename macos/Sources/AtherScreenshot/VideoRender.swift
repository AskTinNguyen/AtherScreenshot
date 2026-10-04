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
    var defaultSeconds: Double { self == .zoom ? 3 : self == .title ? 2.5 : 3 }
}

enum MarkAnimation: Int, CaseIterable {
    case none, fade, pop
    var label: String { ["No animation", "Fade", "Pop"][rawValue] }
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
    var animation = MarkAnimation.fade

    var rect: CGRect { Geo.norm(a, b) }
    func active(_ t: Double) -> Bool { t >= start && t < end }
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
            let (alpha, _) = envelope(m, t)
            var fx: CIImage
            if m.kind == .blur {
                fx = img.clampedToExtent().applyingGaussianBlur(sigma: Double(max(4, min(r.width, r.height) * [0.03, 0.05, 0.08, 0.12, 0.18][m.level]))).cropped(to: r)
            } else {
                let block = max(6, min(r.width, r.height) * [0.04, 0.07, 0.1, 0.14, 0.2][m.level])
                fx = img.clampedToExtent().applyingFilter("CIPixellate", parameters: [kCIInputScaleKey: block, kCIInputCenterKey: CIVector(x: r.minX, y: r.minY)]).cropped(to: r)
            }
            if alpha < 1 { fx = faded(fx, alpha) }
            img = fx.composited(over: img)
        }
        // 2. Markup that sits on the video (moves with zoom).
        for m in edit.marks where !m.kind.isRegion && m.kind != .title && m.active(t) {
            guard let (cg, r) = markImage(m) else { continue }
            img = place(cg, r, H, envelope(m, t)).composited(over: img)
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
            guard let (cg, pr) = captionImage(c, in: tsize) else { continue }
            framed = place(cg, pr.offsetBy(dx: origin.x, dy: origin.y), OH, (1, 1)).composited(over: framed)
        }
        for m in edit.marks where m.kind == .title && m.active(t) {
            guard let cg = titleImage(m, size: tsize) else { continue }
            framed = place(cg, CGRect(origin: origin, size: tsize), OH, envelope(m, t)).composited(over: framed)
        }
        if preview { return framed.composited(over: img) }
        return framed.composited(over: CIImage(color: .black).cropped(to: target))
    }

    func viewRect(at t: Double) -> CGRect {
        guard !preview || zoomInPreview, let z = edit.marks.first(where: { $0.kind == .zoom && $0.active(t) }) else { return view }
        let target = FrameRenderer.zoomTarget(z.rect, view: view)
        let ramp = min(0.45, (z.end - z.start) / 3)
        let p = smooth(min(1, min(t - z.start, z.end - t) / max(0.01, ramp)))
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

    private func smooth(_ x: Double) -> CGFloat { CGFloat(x * x * (3 - 2 * x)) }

    // Opacity and scale for a mark's entrance and exit.
    func envelope(_ m: Mark, _ t: Double) -> (CGFloat, CGFloat) {
        let a = t - m.start, b = m.end - t
        switch m.animation {
        case .none: return (1, 1)
        case .fade:
            let d = max(0.01, min(0.25, (m.end - m.start) / 3))
            return (CGFloat(min(1, a / d, b / d)), 1)
        case .pop:
            let x = min(1, a / 0.22)
            let back = 1 + 2.2 * pow(x - 1, 3) + 1.2 * pow(x - 1, 2)   // ease-out with a little overshoot
            return (CGFloat(min(1, a / 0.1, b / 0.15)), CGFloat(0.55 + 0.45 * back))
        }
    }

    // MARK: drawing helpers

    private func ci(_ r: CGRect, _ H: CGFloat) -> CGRect { CGRect(x: r.minX, y: H - r.maxY, width: r.width, height: r.height) }

    private func faded(_ i: CIImage, _ alpha: CGFloat) -> CIImage {
        i.applyingFilter("CIColorMatrix", parameters: ["inputAVector": CIVector(x: 0, y: 0, z: 0, w: alpha)])
    }

    private func place(_ cg: CGImage, _ r: CGRect, _ H: CGFloat, _ env: (CGFloat, CGFloat)) -> CIImage {
        let c = ci(r, H)
        var i = CIImage(cgImage: cg).transformed(by: CGAffineTransform(scaleX: c.width / CGFloat(cg.width), y: c.height / CGFloat(cg.height)))
            .transformed(by: CGAffineTransform(translationX: c.minX, y: c.minY))
        if env.1 != 1 {
            i = i.transformed(by: CGAffineTransform(translationX: -c.midX, y: -c.midY).concatenating(CGAffineTransform(scaleX: env.1, y: env.1))
                .concatenating(CGAffineTransform(translationX: c.midX, y: c.midY)))
        }
        return env.0 < 1 ? faded(i, max(0, env.0)) : i
    }

    private func cached(_ key: String, _ make: () -> CGImage?) -> CGImage? {
        FrameRenderer.lock.lock()
        if let c = FrameRenderer.cache[key] { FrameRenderer.lock.unlock(); return c }
        FrameRenderer.lock.unlock()
        guard let img = make() else { return nil }
        FrameRenderer.lock.lock()
        if FrameRenderer.cache.count > 300 { FrameRenderer.cache.removeAll() }
        FrameRenderer.cache[key] = img
        FrameRenderer.lock.unlock()
        return img
    }

    // A mark drawn on its own, with the rect it covers in video coordinates.
    func markImage(_ m: Mark) -> (CGImage, CGRect)? {
        let u = unit
        var annot: Annot?
        switch m.kind {
        case .arrow: annot = Annot(tool: .arrow, color: m.color, level: m.level, unit: u, pts: [m.a, m.b])
        case .box: annot = Annot(tool: .rect, color: m.color, level: m.level, unit: u, pts: [m.rect.origin, CGPoint(x: m.rect.maxX, y: m.rect.maxY)])
        case .ellipse: annot = Annot(tool: .ellipse, color: m.color, level: m.level, unit: u, pts: [m.rect.origin, CGPoint(x: m.rect.maxX, y: m.rect.maxY)])
        case .step: annot = Annot(tool: .step, color: m.color, level: m.level, unit: u, pts: [CGPoint(x: m.rect.midX, y: m.rect.midY)], step: m.step)
        case .text:
            let shown = m.text.isEmpty && preview ? "Text" : m.text
            guard !shown.isEmpty else { return nil }
            annot = Annot(tool: .text, color: m.color, level: m.level, unit: u, pts: [m.rect.origin], text: shown, wrap: max(40, m.rect.width))
        default: break
        }
        if let a = annot {
            let b = Render.bounds(a).insetBy(dx: -8 * u, dy: -8 * u).integral
            let key = "a|\(m.kind)|\(a.pts)|\(a.color)|\(a.level)|\(a.text)|\(a.step)|\(u)"
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
            let shown = m.text.isEmpty && preview ? "Say something" : m.text
            guard !shown.isEmpty else { return nil }
            let tail = r.height * 0.32
            let full = CGRect(x: r.minX, y: r.minY, width: r.width, height: r.height + tail)
            let key = "b|\(shown)|\(r.size)|\(m.color)|\(m.level)"
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
                var tb = NSAttributedString(string: shown, attributes: attr()).boundingRect(with: CGSize(width: inner.width, height: 10_000), options: [.usesLineFragmentOrigin])
                while tb.height > inner.height && fs > 8 { fs *= 0.9; tb = NSAttributedString(string: shown, attributes: attr()).boundingRect(with: CGSize(width: inner.width, height: 10_000), options: [.usesLineFragmentOrigin]) }
                withNSContext(ctx, flipped: true) {
                    NSAttributedString(string: shown, attributes: attr()).draw(with: CGRect(x: inner.minX, y: inner.midY - tb.height / 2, width: inner.width, height: tb.height + 2), options: [.usesLineFragmentOrigin])
                }
                return ctx.makeImage()
            }) else { return nil }
            return (img, full)
        }
        return nil
    }

    // A caption pill in output coordinates (top-left origin), sized like the export.
    func captionImage(_ c: Caption, in size: CGSize) -> (CGImage, CGRect)? {
        let text = c.text.isEmpty ? "Type a caption…" : c.text
        let fontSize = max(14, size.height * VideoEdit.captionScale[edit.captionSize])
        let attrs = VideoExport.captionAttributes(fontSize, dim: c.text.isEmpty)
        let maxW = size.width * 0.86
        let tb = NSAttributedString(string: text, attributes: attrs).boundingRect(with: CGSize(width: maxW, height: 10_000), options: [.usesLineFragmentOrigin])
        let pad = fontSize * 0.45
        let w = ceil(tb.width) + pad * 2, h = ceil(tb.height) + pad * 1.2
        let margin = size.height * 0.06
        let y: CGFloat = c.position == .top ? margin : c.position == .middle ? (size.height - h) / 2 : size.height - margin - h
        let r = CGRect(x: ((size.width - w) / 2).rounded(), y: y.rounded(), width: w.rounded(.up), height: h.rounded(.up))
        let key = "c|\(text)|\(r.size)|\(fontSize)"
        guard let img = cached(key, {
            guard let ctx = makeContext(width: Int(r.width), height: Int(r.height)) else { return nil }
            ctx.addPath(CGPath(roundedRect: CGRect(origin: .zero, size: r.size), cornerWidth: fontSize * 0.35, cornerHeight: fontSize * 0.35, transform: nil))
            ctx.setFillColor(NSColor.black.withAlphaComponent(0.62).cgColor)
            ctx.fillPath()
            withNSContext(ctx, flipped: true) {
                NSAttributedString(string: text, attributes: attrs).draw(with: CGRect(x: pad, y: pad * 0.6, width: r.width - pad * 2, height: ceil(tb.height) + 2), options: [.usesLineFragmentOrigin])
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
