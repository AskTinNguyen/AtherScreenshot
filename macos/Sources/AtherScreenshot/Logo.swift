import AppKit
import CoreText

// The A⁵ mark, traced from the brand artwork in its own coordinate space (y down).
// Content spans x 90..552, y 212..690.
enum Logo {
    private static let a: [CGPoint] = [CGPoint(x: 90, y: 690), CGPoint(x: 270, y: 296), CGPoint(x: 360, y: 296), CGPoint(x: 540, y: 690),
                                       CGPoint(x: 460, y: 690), CGPoint(x: 313, y: 368), CGPoint(x: 165, y: 690)]
    private static let bar = CGRect(x: 307, y: 483, width: 13, height: 127)
    private static let five = CGRect(x: 424, y: 212, width: 128, height: 170)
    private static let minX: CGFloat = 90, maxX: CGFloat = 552, minY: CGFloat = 212, maxY: CGFloat = 690

    private static func fivePath() -> CGPath? {
        let font = CTFontCreateWithName("Futura-CondensedExtraBold" as CFString, 100, nil)
        var ch: UniChar = 0x35
        var glyph: CGGlyph = 0
        CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1)
        guard let p = CTFontCreatePathForGlyph(font, glyph, nil) else { return nil }
        var flip = CGAffineTransform(scaleX: 1, y: -1)  // glyphs are y-up
        return p.copy(using: &flip)
    }

    // Draws into a flipped (y-down) context. `template`: a single-colour silhouette for the menu bar.
    static func draw(_ ctx: CGContext, in box0: CGRect, tile: Bool, template: Bool = false) {
        var box = box0
        if tile {
            let r = box.width * 0.22
            let path = CGPath(roundedRect: box, cornerWidth: r, cornerHeight: r, transform: nil)
            ctx.addPath(path)
            ctx.setFillColor(Theme.bg.cgColor)
            ctx.fillPath()
            if box.width >= 32 {
                ctx.addPath(path)
                ctx.setStrokeColor(Theme.rgb(44, 46, 40).cgColor)
                ctx.setLineWidth(max(1, box.width / 64))
                ctx.strokePath()
            }
            box = box.insetBy(dx: box.width * 0.14, dy: box.height * 0.14)
        }
        let w = maxX - minX, h = maxY - minY, k = min(box.width / w, box.height / h)
        let ox = box.minX + (box.width - w * k) / 2 - minX * k, oy = box.minY + (box.height - h * k) / 2 - minY * k
        func P(_ p: CGPoint) -> CGPoint { CGPoint(x: ox + p.x * k, y: oy + p.y * k) }
        let gray = template ? NSColor.black.cgColor : Theme.logoGray.cgColor
        let lime = template ? NSColor.black.cgColor : Theme.accent.cgColor
        ctx.addLines(between: a.map(P))
        ctx.closePath()
        ctx.setFillColor(gray)
        ctx.fillPath()
        if w * k >= 12 {
            let b = CGRect(x: ox + bar.minX * k, y: oy + bar.minY * k, width: max(1, bar.width * k), height: bar.height * k)
            if template {
                ctx.setBlendMode(.clear)
                ctx.fill(b)
                ctx.setBlendMode(.normal)
            } else {
                ctx.setFillColor(lime)
                ctx.fill(b)
            }
        }
        // The superscript 5, fitted to its box using the glyph's real outline.
        if let g = fivePath() {
            let gb = g.boundingBoxOfPath
            let fk = min(five.width * k / gb.width, five.height * k / gb.height)
            var t = CGAffineTransform(translationX: ox + five.minX * k + (five.width * k - gb.width * fk) / 2, y: oy + five.minY * k)
                .scaledBy(x: fk, y: fk).translatedBy(x: -gb.minX, y: -gb.minY)
            if let p = g.copy(using: &t) {
                ctx.addPath(p)
                ctx.setFillColor(lime)
                ctx.fillPath()
            }
        }
    }

    static func render(_ size: Int, tile: Bool = true, template: Bool = false) -> CGImage? {
        guard let ctx = makeContext(width: size, height: size) else { return nil }
        draw(ctx, in: CGRect(x: 0, y: 0, width: size, height: size), tile: tile, template: template)
        return ctx.makeImage()
    }

    static var appIcon: NSImage {
        render(512).map { NSImage(cgImage: $0, size: NSSize(width: 512, height: 512)) } ?? NSImage()
    }

    static var menuBarIcon: NSImage {
        let img = render(36, tile: false, template: true).map { NSImage(cgImage: $0, size: NSSize(width: 18, height: 18)) } ?? NSImage()
        img.isTemplate = true
        return img
    }

    // `AtherScreenshot --write-iconset <dir>`: used by build.sh to make AppIcon.icns.
    static func writeIconset(_ dir: URL) -> Bool {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        for base in [16, 32, 128, 256, 512] {
            for scale in [1, 2] {
                // macOS icons sit inside a margin of the canvas (Big Sur grid: 824/1024).
                let px = base * scale
                guard let ctx = makeContext(width: px, height: px) else { return false }
                let inset = CGFloat(px) * 100 / 1024
                ctx.setShadow(offset: CGSize(width: 0, height: CGFloat(px) * 0.01), blur: CGFloat(px) * 0.02, color: NSColor.black.withAlphaComponent(0.35).cgColor)
                draw(ctx, in: CGRect(x: inset, y: inset, width: CGFloat(px) - 2 * inset, height: CGFloat(px) - 2 * inset), tile: true)
                guard let img = ctx.makeImage(), let data = img.pngData() else { return false }
                let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
                guard (try? data.write(to: dir.appendingPathComponent(name))) != nil else { return false }
            }
        }
        return true
    }
}
