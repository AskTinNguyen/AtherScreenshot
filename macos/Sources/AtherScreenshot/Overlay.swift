import AppKit
import Carbon.HIToolbox

enum OverlayMode { case region, window, color, ruler }

struct OverlayResult {
    var rect: CGRect          // CG global points
    var point: CGPoint        // where the user clicked (color picker)
    var window: WindowInfo?   // set when a click snapped to a window
}

// Full-screen selector over a frozen snapshot: one borderless panel per display, shared state here.
final class Overlay {
    static var active: Overlay?

    let mode: OverlayMode
    let snap: Snapshot
    let wins: [WindowInfo]
    var done: ((OverlayResult?) -> Void)?
    var cursor = Geo.mouse
    var anchor: CGPoint?
    var selection: CGRect?
    var rulerA: CGPoint?, rulerB: CGPoint?
    var rulerDragging = false
    let crosshair = Settings.shared.bool("Crosshair")
    let magnifier = Settings.shared.bool("Magnifier")
    private var panels: [KeyablePanel] = []
    private var views: [OverlayView] = []

    static func run(_ mode: OverlayMode, snapshot: Snapshot, done: @escaping (OverlayResult?) -> Void) {
        active?.finish(nil)
        let o = Overlay(mode: mode, snap: snapshot, wins: Capture.windows())
        o.done = done
        active = o
        o.begin()
    }

    private init(mode: OverlayMode, snap: Snapshot, wins: [WindowInfo]) {
        self.mode = mode
        self.snap = snap
        self.wins = wins
    }

    private func begin() {
        for screen in NSScreen.screens {
            let frameCG = Geo.toCG(screen.frame)
            guard let shot = snap.shots.first(where: { $0.displayID == Geo.displayID(screen) }) ?? snap.shot(at: frameCG.center) else { continue }
            let p = KeyablePanel(contentRect: screen.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            p.level = .screenSaver
            p.isOpaque = true
            p.hasShadow = false
            p.isReleasedWhenClosed = false
            p.acceptsMouseMovedEvents = true
            p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
            p.animationBehavior = .none
            excludeFromCapture(p)
            let v = OverlayView(overlay: self, frameCG: frameCG, shot: shot)
            p.contentView = v
            p.onKey = { [weak self] e in self?.key(e) ?? false }
            p.setFrame(screen.frame, display: false)
            p.orderFrontRegardless()
            panels.append(p)
            views.append(v)
        }
        activateApp()
        let under = panels.first { $0.frame.contains(NSEvent.mouseLocation) } ?? panels.first
        under?.makeKey()
        NSCursor.crosshair.set()
        refresh()
    }

    func finish(_ r: OverlayResult?) {
        guard Overlay.active === self else { return }
        Overlay.active = nil
        for p in panels { p.orderOut(nil) }
        panels.removeAll()
        views.removeAll()
        NSCursor.arrow.set()
        let d = done
        done = nil
        // Let the window server drop the overlay before anything captures the screen again.
        DispatchQueue.main.async { d?(r) }
    }

    func refresh() { for v in views { v.needsDisplay = true } }

    var hoverWindow: WindowInfo? {
        guard mode == .region || mode == .window else { return nil }
        return Capture.window(at: cursor, in: wins)
    }

    var screenRect: CGRect { snap.shot(at: cursor)?.frame ?? Geo.toCG(Geo.mouseScreen.frame) }

    // What a click would take right now.
    var hoverRect: CGRect? {
        switch mode {
        case .region: return (hoverWindow?.frame ?? screenRect).intersection(snap.bounds)
        case .window: return hoverWindow?.frame.intersection(snap.bounds)
        default: return nil
        }
    }

    var highlight: CGRect? { selection ?? (anchor == nil ? hoverRect : nil) }

    // MARK: input

    func mouseDown(_ p: CGPoint) {
        cursor = p
        switch mode {
        case .region, .window: anchor = p
        case .ruler:
            rulerA = p
            rulerB = p
            rulerDragging = true
        case .color: finish(OverlayResult(rect: CGRect(origin: p, size: CGSize(width: 1, height: 1)), point: p))
        }
        refresh()
    }

    func mouseDragged(_ p: CGPoint, shift: Bool) {
        cursor = p
        switch mode {
        case .region:
            if let a = anchor, hypot(p.x - a.x, p.y - a.y) > 3 { selection = Geo.norm(a, p).integral }
        case .ruler where rulerDragging:
            rulerB = shift ? snap45(rulerA ?? p, p) : p
        default: break
        }
        refresh()
    }

    func mouseUp(_ p: CGPoint) {
        cursor = p
        switch mode {
        case .region:
            if let s = selection, s.width >= 2, s.height >= 2 {
                finish(OverlayResult(rect: s, point: p, window: nil))
            } else if let r = hoverRect {
                finish(OverlayResult(rect: r, point: p, window: hoverWindow))
            }
            anchor = nil
            selection = nil
        case .window:
            if let w = hoverWindow { finish(OverlayResult(rect: w.frame, point: p, window: w)) }
            anchor = nil
        case .ruler: rulerDragging = false
        case .color: break
        }
        refresh()
    }

    func mouseMoved(_ p: CGPoint) {
        cursor = p
        refresh()
    }

    private func snap45(_ a: CGPoint, _ b: CGPoint) -> CGPoint {
        let dx = b.x - a.x, dy = b.y - a.y
        let ang = (atan2(dy, dx) / (.pi / 4)).rounded() * (.pi / 4)
        let len = hypot(dx, dy)
        return CGPoint(x: a.x + cos(ang) * len, y: a.y + sin(ang) * len)
    }

    var rulerText: String? {
        guard let a = rulerA, let b = rulerB else { return nil }
        let k = snap.scale(for: Geo.norm(a, b).insetBy(dx: -1, dy: -1))
        let w = abs(b.x - a.x) * k, h = abs(b.y - a.y) * k
        let ang = atan2(-(b.y - a.y), b.x - a.x) * 180 / .pi
        return String(format: "%.0f × %.0f px  ·  %.1f px  ·  %.1f°", w, h, hypot(w, h), ang)
    }

    private func key(_ e: NSEvent) -> Bool {
        let cmd = e.modifierFlags.contains(.command)
        let step: CGFloat = e.modifierFlags.contains(.shift) ? 10 : 1
        switch Int(e.keyCode) {
        case kVK_Escape: finish(nil)
        case kVK_Space where mode == .region: finish(OverlayResult(rect: screenRect, point: cursor))
        case kVK_ANSI_A where cmd && mode == .region: finish(OverlayResult(rect: snap.bounds, point: cursor))
        case kVK_Return, kVK_ANSI_KeypadEnter:
            if let s = selection { finish(OverlayResult(rect: s, point: cursor)) }
            else if mode == .color { finish(OverlayResult(rect: CGRect(origin: cursor, size: CGSize(width: 1, height: 1)), point: cursor)) }
        case kVK_ANSI_C where mode == .ruler:
            if let t = rulerText {
                copyText(t)
                Toast.shared.show("Measurement copied", t)
            }
        case kVK_LeftArrow: nudge(-step, 0)
        case kVK_RightArrow: nudge(step, 0)
        case kVK_UpArrow: nudge(0, -step)
        case kVK_DownArrow: nudge(0, step)
        default: return false
        }
        return true
    }

    private func nudge(_ dx: CGFloat, _ dy: CGFloat) {
        if var s = selection, anchor == nil {
            s = s.offsetBy(dx: dx, dy: dy)
            selection = s
        } else {
            cursor = CGPoint(x: cursor.x + dx, y: cursor.y + dy)
            CGWarpMouseCursorPosition(cursor)
        }
        refresh()
    }
}

final class OverlayView: NSView {
    unowned let overlay: Overlay
    let frameCG: CGRect
    let shot: DisplayShot

    init(overlay: Overlay, frameCG: CGRect, shot: DisplayShot) {
        self.overlay = overlay
        self.frameCG = frameCG
        self.shot = shot
        super.init(frame: NSRect(origin: .zero, size: frameCG.size))
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeAlways, .inVisibleRect, .cursorUpdate], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func cursorUpdate(with event: NSEvent) { NSCursor.crosshair.set() }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .crosshair) }

    private func global(_ e: NSEvent) -> CGPoint { Geo.toCG(NSEvent.mouseLocation) }
    private func local(_ p: CGPoint) -> CGPoint { CGPoint(x: p.x - frameCG.minX, y: p.y - frameCG.minY) }
    private func local(_ r: CGRect) -> CGRect { r.offsetBy(dx: -frameCG.minX, dy: -frameCG.minY) }

    override func mouseDown(with e: NSEvent) { overlay.mouseDown(global(e)) }
    override func mouseDragged(with e: NSEvent) { overlay.mouseDragged(global(e), shift: e.modifierFlags.contains(.shift)) }
    override func mouseUp(with e: NSEvent) { overlay.mouseUp(global(e)) }
    override func mouseMoved(with e: NSEvent) { overlay.mouseMoved(global(e)) }
    override func rightMouseDown(with e: NSEvent) { overlay.finish(nil) }

    override func draw(_ dirty: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.interpolationQuality = .none
        drawImageFlipped(ctx, shot.image, in: bounds)

        let hl = overlay.highlight.map { local($0) }.flatMap { $0.intersects(bounds) ? $0 : nil }
        let dim = CGMutablePath()
        dim.addRect(bounds)
        if let hl, overlay.mode != .ruler, overlay.mode != .color { dim.addRect(hl) }
        ctx.addPath(dim)
        ctx.setFillColor(NSColor.black.withAlphaComponent(overlay.mode == .ruler || overlay.mode == .color ? 0.15 : 0.42).cgColor)
        ctx.fillPath(using: .evenOdd)

        if let hl, overlay.mode == .region || overlay.mode == .window {
            ctx.setStrokeColor(Theme.accent.cgColor)
            ctx.setLineWidth(1.5)
            ctx.stroke(hl.insetBy(dx: -0.75, dy: -0.75))
            if let g = overlay.highlight {
                let k = overlay.snap.scale(for: g)
                label(String(format: "%.0f × %.0f", g.width * k, g.height * k), near: hl)
            }
        }

        if overlay.mode == .ruler { drawRuler(ctx) }

        let here = frameCG.contains(overlay.cursor)
        let c = local(overlay.cursor)
        if here, overlay.crosshair, overlay.mode != .ruler, overlay.selection == nil {
            ctx.setLineWidth(1)
            ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.45).cgColor)
            ctx.setShadow(offset: .zero, blur: 2, color: NSColor.black.withAlphaComponent(0.8).cgColor)
            ctx.strokeLineSegments(between: [CGPoint(x: 0, y: c.y + 0.5), CGPoint(x: bounds.width, y: c.y + 0.5),
                                             CGPoint(x: c.x + 0.5, y: 0), CGPoint(x: c.x + 0.5, y: bounds.height)])
            ctx.setShadow(offset: .zero, blur: 0, color: nil)
        }
        if here, overlay.magnifier || overlay.mode == .color { drawMagnifier(ctx, at: c) }
        if here { drawHint() }
    }

    private func pill(_ text: String, at origin: CGPoint, font: NSFont = Theme.mono(11), fg: NSColor = Theme.text,
                      bg: NSColor = Theme.surface.withAlphaComponent(0.92)) -> CGRect {
        let attr = NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: fg])
        let s = attr.size()
        var r = CGRect(x: origin.x, y: origin.y, width: s.width + 14, height: s.height + 8)
        r.origin.x = min(max(4, r.minX), bounds.width - r.width - 4)
        r.origin.y = min(max(4, r.minY), bounds.height - r.height - 4)
        let path = NSBezierPath(roundedRect: r, xRadius: 6, yRadius: 6)
        bg.setFill()
        path.fill()
        Theme.border.setStroke()
        path.stroke()
        attr.draw(at: CGPoint(x: r.minX + 7, y: r.minY + 4))
        return r
    }

    private func label(_ text: String, near r: CGRect) {
        let y = r.minY - 26 >= 4 ? r.minY - 26 : r.minY + 6
        _ = pill(text, at: CGPoint(x: r.minX, y: y), fg: Theme.onAccent, bg: Theme.accent)
    }

    private func drawHint() {
        let text: String
        switch overlay.mode {
        case .region: text = "Drag to select  ·  Click a window  ·  Space: this display  ·  ⌘A: everything  ·  Esc cancels"
        case .window: text = "Click the window to record  ·  Esc cancels"
        case .color: text = "Click to copy the color  ·  Arrows move 1 px  ·  Esc cancels"
        case .ruler: text = "Drag to measure  ·  ⇧ snaps to 45°  ·  C copies  ·  Esc closes"
        }
        let attr = NSAttributedString(string: text, attributes: [.font: Theme.font(12, .medium)])
        _ = pill(text, at: CGPoint(x: (bounds.width - attr.size().width - 14) / 2, y: 18), font: Theme.font(12, .medium), fg: Theme.textDim)
    }

    private func drawMagnifier(_ ctx: CGContext, at c: CGPoint) {
        let cells = 15, cell: CGFloat = 9
        let size = CGFloat(cells) * cell
        var o = CGPoint(x: c.x + 22, y: c.y + 22)
        if o.x + size > bounds.width - 4 { o.x = c.x - 22 - size }
        if o.y + size + 30 > bounds.height - 4 { o.y = c.y - 22 - size - 30 }
        let box = CGRect(x: o.x, y: o.y, width: size, height: size)
        let k = shot.scale
        let px = CGPoint(x: floor(c.x * k), y: floor(c.y * k))
        let half = CGFloat(cells / 2)
        ctx.saveGState()
        ctx.addPath(CGPath(roundedRect: box, cornerWidth: 8, cornerHeight: 8, transform: nil))
        ctx.clip()
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(box)
        if let crop = shot.image.cropped(CGRect(x: px.x - half, y: px.y - half, width: CGFloat(cells), height: CGFloat(cells))) {
            ctx.interpolationQuality = .none
            // Keep pixel alignment when the crop was clamped at a display edge.
            let dx = max(0, half - px.x) * cell, dy = max(0, half - px.y) * cell
            drawImageFlipped(ctx, crop, in: CGRect(x: box.minX + dx, y: box.minY + dy, width: CGFloat(crop.width) * cell, height: CGFloat(crop.height) * cell))
        }
        ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.08).cgColor)
        ctx.setLineWidth(1)
        for i in 1..<cells {
            let t = CGFloat(i) * cell
            ctx.strokeLineSegments(between: [CGPoint(x: box.minX + t, y: box.minY), CGPoint(x: box.minX + t, y: box.maxY),
                                             CGPoint(x: box.minX, y: box.minY + t), CGPoint(x: box.maxX, y: box.minY + t)])
        }
        ctx.setStrokeColor(Theme.accent.cgColor)
        ctx.setLineWidth(1.5)
        ctx.stroke(CGRect(x: box.minX + half * cell, y: box.minY + half * cell, width: cell, height: cell))
        ctx.restoreGState()
        ctx.addPath(CGPath(roundedRect: box, cornerWidth: 8, cornerHeight: 8, transform: nil))
        ctx.setStrokeColor(Theme.border.cgColor)
        ctx.setLineWidth(1)
        ctx.strokePath()

        let color = shot.image.color(atPixel: px) ?? .black
        let p = CGPoint(x: (frameCG.minX + c.x).rounded(.down), y: (frameCG.minY + c.y).rounded(.down))
        let r = pill(String(format: "%@  %.0f, %.0f", color.hex, p.x, p.y), at: CGPoint(x: box.minX, y: box.maxY + 6))
        color.setFill()
        NSBezierPath(roundedRect: CGRect(x: r.maxX + 4, y: r.minY + 3, width: r.height - 6, height: r.height - 6), xRadius: 3, yRadius: 3).fill()
    }

    private func drawRuler(_ ctx: CGContext) {
        guard let a0 = overlay.rulerA, let b0 = overlay.rulerB else { return }
        let a = local(a0), b = local(b0)
        ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.5).cgColor)
        ctx.setLineWidth(1)
        ctx.setLineDash(phase: 0, lengths: [4, 4])
        ctx.stroke(Geo.norm(a, b))
        ctx.setLineDash(phase: 0, lengths: [])
        ctx.setStrokeColor(Theme.accent.cgColor)
        ctx.setLineWidth(2)
        ctx.strokeLineSegments(between: [a, b])
        ctx.setFillColor(Theme.accent.cgColor)
        for p in [a, b] { ctx.fillEllipse(in: CGRect(x: p.x - 4, y: p.y - 4, width: 8, height: 8)) }
        if let t = overlay.rulerText { _ = pill(t, at: CGPoint(x: (a.x + b.x) / 2 + 10, y: (a.y + b.y) / 2 + 10), fg: Theme.onAccent, bg: Theme.accent) }
    }
}
