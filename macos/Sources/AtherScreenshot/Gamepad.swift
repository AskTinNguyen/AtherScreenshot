import AppKit
import CoreText
import GameController

// A game controller drawn into a corner of recorded frames (like OBS's Input Overlay): sticks, triggers,
// bumpers, D-pad, A/B/X/Y and View/Menu, lit as they are used. Xbox, PlayStation and MFi pads through the
// GameController framework. Nothing is drawn while no controller is connected. Windows: src/gamepad.cpp.

struct PadButtons: OptionSet, Hashable {
    let rawValue: UInt16
    // The XInput bit values, so both apps mean the same thing.
    static let up = PadButtons(rawValue: 0x0001), down = PadButtons(rawValue: 0x0002)
    static let left = PadButtons(rawValue: 0x0004), right = PadButtons(rawValue: 0x0008)
    static let menu = PadButtons(rawValue: 0x0010), view = PadButtons(rawValue: 0x0020)
    static let leftThumb = PadButtons(rawValue: 0x0040), rightThumb = PadButtons(rawValue: 0x0080)
    static let leftShoulder = PadButtons(rawValue: 0x0100), rightShoulder = PadButtons(rawValue: 0x0200)
    static let a = PadButtons(rawValue: 0x1000), b = PadButtons(rawValue: 0x2000)
    static let x = PadButtons(rawValue: 0x4000), y = PadButtons(rawValue: 0x8000)
}

struct PadState: Equatable {
    var connected = false
    var buttons: PadButtons = []
    var lx: Float = 0, ly: Float = 0, rx: Float = 0, ry: Float = 0  // sticks, -1…1, y up, dead zone removed
    var lt: Float = 0, rt: Float = 0                                // triggers, 0…1
    var playStation = false                                         // ✕○□△ labels

    // Round dead zone, rescaled so its edge is 0 and full tilt is 1 (XInput's dead zones).
    static func stick(_ x: Float, _ y: Float, dead: Float) -> (Float, Float) {
        let mag = min(1, (x * x + y * y).squareRoot())
        guard mag > dead else { return (0, 0) }
        let k = (mag - dead) / (1 - dead) / mag
        return (max(-1, min(1, x * k)), max(-1, min(1, y * k)))
    }
    static func trigger(_ v: Float) -> Float {
        let t: Float = 30 / 255
        return v <= t ? 0 : min(1, (v - t) / (1 - t))
    }
}

enum PadCorner: String, CaseIterable {
    case topLeft = "topleft", topRight = "topright", bottomLeft = "bottomleft", bottomRight = "bottomright"
    static func parse(_ s: String) -> PadCorner { PadCorner(rawValue: s.lowercased()) ?? .bottomRight }
    var title: String {
        switch self {
        case .topLeft: return "Top left"
        case .topRight: return "Top right"
        case .bottomLeft: return "Bottom left"
        case .bottomRight: return "Bottom right"
        }
    }
}

// Holds every press seen between two frames, so a tap shorter than a frame still shows in one.
struct PadLatch {
    private var cur = PadState(), held = PadState()

    mutating func feed(_ s: PadState) {
        cur = s
        held.connected = s.connected
        held.playStation = s.playStation
        held.buttons.formUnion(s.buttons)
        held.lt = max(held.lt, s.lt)
        held.rt = max(held.rt, s.rt)
        // A stick flick between two frames shows at its widest.
        if s.lx * s.lx + s.ly * s.ly >= held.lx * held.lx + held.ly * held.ly { held.lx = s.lx; held.ly = s.ly }
        if s.rx * s.rx + s.ry * s.ry >= held.rx * held.rx + held.ry * held.ry { held.rx = s.rx; held.ry = s.ry }
    }

    // What to draw now; then starts over from the current state.
    mutating func take() -> PadState {
        var out = held
        out.connected = cur.connected
        held = cur
        return out
    }
}

// Follows the first connected controller. Every value change feeds the latch, so nothing between frames is lost.
final class GamepadMonitor {
    private let lock = NSLock()
    private var latch = PadLatch()
    private let queue = DispatchQueue(label: "ather.gamepad")
    private var observers: [NSObjectProtocol] = []
    private weak var pad: GCController?
    private(set) var changes = 0  // bumped on every input, so a still screen can still show presses

    init() {
        GCController.shouldMonitorBackgroundEvents = true  // the app records others' windows; it's never frontmost
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] _ in self?.attach() })
        observers.append(nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { [weak self] _ in self?.attach() })
        attach()
    }

    deinit {
        observers.forEach(NotificationCenter.default.removeObserver)
        pad?.extendedGamepad?.valueChangedHandler = nil
    }

    func take() -> PadState {
        lock.lock()
        defer { lock.unlock() }
        return latch.take()
    }

    var changeCount: Int {
        lock.lock()
        defer { lock.unlock() }
        return changes
    }

    private func attach() {
        let first = GCController.controllers().first { $0.extendedGamepad != nil }
        if first !== pad {
            pad?.extendedGamepad?.valueChangedHandler = nil
            pad = first
            if let c = first, let g = c.extendedGamepad {
                c.handlerQueue = queue
                let ps = GamepadMonitor.isPlayStation(c)
                g.valueChangedHandler = { [weak self] g, _ in self?.feed(GamepadMonitor.state(g, playStation: ps)) }
            }
        }
        let s = first?.extendedGamepad.map { GamepadMonitor.state($0, playStation: GamepadMonitor.isPlayStation(first!)) } ?? PadState()
        queue.async { self.feed(s) }
    }

    private func feed(_ s: PadState) {
        lock.lock()
        latch.feed(s)
        changes += 1
        lock.unlock()
    }

    static func isPlayStation(_ c: GCController) -> Bool {
        let n = (c.productCategory + " " + (c.vendorName ?? "")).lowercased()
        return n.contains("dualshock") || n.contains("dualsense") || n.contains("playstation")
    }

    static func state(_ g: GCExtendedGamepad, playStation: Bool) -> PadState {
        var s = PadState(connected: true)
        s.playStation = playStation
        let pairs: [(GCControllerButtonInput?, PadButtons)] = [
            (g.dpad.up, .up), (g.dpad.down, .down), (g.dpad.left, .left), (g.dpad.right, .right),
            (g.buttonMenu, .menu), (g.buttonOptions, .view), (g.leftThumbstickButton, .leftThumb), (g.rightThumbstickButton, .rightThumb),
            (g.leftShoulder, .leftShoulder), (g.rightShoulder, .rightShoulder),
            (g.buttonA, .a), (g.buttonB, .b), (g.buttonX, .x), (g.buttonY, .y),
        ]
        for (b, bit) in pairs where b?.isPressed == true { s.buttons.insert(bit) }
        (s.lx, s.ly) = PadState.stick(g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value, dead: 7849 / 32767)
        (s.rx, s.ry) = PadState.stick(g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value, dead: 8689 / 32767)
        s.lt = PadState.trigger(g.leftTrigger.value)
        s.rt = PadState.trigger(g.rightTrigger.value)
        return s
    }
}

// MARK: - Layout and drawing

// Where the overlay goes: (x, y) is the top-left of its 240 × 172 design box (y down), `u` pixels per design unit.
struct PadLayout: Equatable {
    var x: CGFloat = 0, y: CGFloat = 0, u: CGFloat = 1
    func at(_ dx: CGFloat, _ dy: CGFloat) -> CGPoint { CGPoint(x: x + dx * u, y: y + dy * u) }
}

enum Gamepad {
    static let boxW: CGFloat = 240, boxH: CGFloat = 172
    static let aButton = CGPoint(x: 196.7, y: 67.1), leftStick = CGPoint(x: 43, y: 49), leftTrigger = CGPoint(x: 46, y: -2)

    // About 220 pt wide, but never more than a third of the frame across or half of it down. `scale` is pixels per point.
    static func layout(frameW: Int, frameH: Int, corner: PadCorner, scale: CGFloat) -> PadLayout {
        let fw = CGFloat(frameW), fh = CGFloat(frameH)
        let w = min(220 * scale, fw * 0.33, fh * 0.5 * boxW / boxH)
        let u = w / boxW, h = boxH * u
        let m = min(16 * scale, min(fw, fh) * 0.03)
        let left = corner == .topLeft || corner == .bottomLeft, top = corner == .topLeft || corner == .topRight
        return PadLayout(x: left ? m : fw - w - m, y: top ? m : fh - h - m, u: u)
    }

    // The pixels the overlay can touch (y down), for keeping a clean copy of what's under it.
    static func footprint(frameW: Int, frameH: Int, corner: PadCorner, scale: CGFloat) -> CGRect {
        let L = layout(frameW: frameW, frameH: frameH, corner: corner, scale: scale)
        let pad = 10 * L.u
        let r = CGRect(x: floor(L.x - pad), y: floor(L.y - pad), width: ceil(boxW * L.u + 2 * pad) + 1, height: ceil(boxH * L.u + 2 * pad) + 1)
        return r.intersection(CGRect(x: 0, y: 0, width: frameW, height: frameH))
    }

    // Drawing takes a few milliseconds and the pad mostly sits still, so the last layer is kept and reused.
    private struct Cached { let s: PadState; let w: Int, h: Int; let u: CGFloat, fx: CGFloat, fy: CGFloat; let image: CGImage }
    private static var cache: Cached?
    private static let cacheLock = NSLock()

    // Draws the controller onto a frame (BGRA, top-left origin rows) at `opacity` (0…1). Nothing for a disconnected pad.
    static func draw(into ctx: CGContext, frameW: Int, frameH: Int, state s: PadState, corner: PadCorner, scale: CGFloat, opacity: CGFloat) {
        let opacity = max(0, min(1, opacity))
        guard s.connected, opacity >= 0.01, frameW >= 32, frameH >= 32 else { return }
        let L = layout(frameW: frameW, frameH: frameH, corner: corner, scale: scale)
        let pad = 10 * L.u
        let ox = floor(L.x - pad), oy = floor(L.y - pad)
        let w = Int(ceil(boxW * L.u + 2 * pad)) + 1, h = Int(ceil(boxH * L.u + 2 * pad)) + 1
        let fx = L.x - ox, fy = L.y - oy
        cacheLock.lock()
        var image = cache.flatMap { $0.s == s && $0.w == w && $0.h == h && $0.u == L.u && $0.fx == fx && $0.fy == fy ? $0.image : nil }
        if image == nil, let img = layer(s, w: w, h: h, u: L.u, fx: fx, fy: fy) {
            cache = Cached(s: s, w: w, h: h, u: L.u, fx: fx, fy: fy, image: img)
            image = img
        }
        cacheLock.unlock()
        guard let image else { return }
        // Drawn on its own layer, then blended at the chosen opacity, so overlapping parts fade together.
        ctx.saveGState()
        ctx.setAlpha(opacity)
        ctx.interpolationQuality = .none
        ctx.draw(image, in: CGRect(x: ox, y: CGFloat(frameH) - oy - CGFloat(h), width: CGFloat(w), height: CGFloat(h)))
        ctx.restoreGState()
    }

    // Draws into a BGRA pixel buffer in place.
    static func draw(into px: CVPixelBuffer, state: PadState, corner: PadCorner, scale: CGFloat, opacity: CGFloat) {
        guard state.connected else { return }
        CVPixelBufferLockBaseAddress(px, [])
        defer { CVPixelBufferUnlockBaseAddress(px, []) }
        let w = CVPixelBufferGetWidth(px), h = CVPixelBufferGetHeight(px)
        guard let base = CVPixelBufferGetBaseAddress(px),
              let ctx = CGContext(data: base, width: w, height: h, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(px),
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return }
        draw(into: ctx, frameW: w, frameH: h, state: state, corner: corner, scale: scale, opacity: opacity)
    }

    static func layer(_ s: PadState, w: Int, h: Int, u: CGFloat, fx: CGFloat, fy: CGFloat) -> CGImage? {
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(h))
        ctx.scaleBy(x: 1, y: -1)  // y down, like the design measurements
        ctx.translateBy(x: fx, y: fy)
        ctx.scaleBy(x: u, y: u)
        ctx.setShouldAntialias(true)
        PadPainter(g: ctx, s: s).paint()
        return ctx.makeImage()
    }
}

// The whole controller in design units (240 × 172, y down). Everything is measured from the reference photo
// (a white modern controller, 485 px wide there): 1 px ≈ 0.495 units. Same numbers as PaintPad on Windows.
private struct PadPainter {
    let g: CGContext
    let s: PadState

    static func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: Int = 255) -> CGColor {
        CGColor(srgbRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: CGFloat(a) / 255)
    }
    let orange = rgb(255, 160, 46), orangeHot = rgb(255, 214, 150), seam = rgb(0, 0, 0, 40)

    func on(_ b: PadButtons) -> Bool { s.buttons.contains(b) }

    func roundRect(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat, _ r: CGFloat) -> CGPath {
        CGPath(roundedRect: CGRect(x: x, y: y, width: w, height: h), cornerWidth: min(r, w / 2), cornerHeight: min(r, h / 2), transform: nil)
    }
    func circle(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat) -> CGRect { CGRect(x: cx - r, y: cy - r, width: 2 * r, height: 2 * r) }

    func fill(_ p: CGPath, _ c: CGColor) {
        g.addPath(p)
        g.setFillColor(c)
        g.fillPath()
    }
    func stroke(_ p: CGPath, _ c: CGColor, _ w: CGFloat, round: Bool = false) {
        g.addPath(p)
        g.setStrokeColor(c)
        g.setLineWidth(w)
        g.setLineCap(round ? .round : .butt)
        g.setLineJoin(.round)
        g.strokePath()
    }
    func fillEllipse(_ r: CGRect, _ c: CGColor) {
        g.setFillColor(c)
        g.fillEllipse(in: r)
    }
    func ring(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, _ w: CGFloat, _ c: CGColor) {
        g.setStrokeColor(c)
        g.setLineWidth(w)
        g.strokeEllipse(in: circle(cx, cy, r))
    }
    func line(_ a: CGPoint, _ b: CGPoint, _ c: CGColor, _ w: CGFloat) {
        let p = CGMutablePath()
        p.move(to: a)
        p.addLine(to: b)
        stroke(p, c, w, round: true)
    }
    func linear(_ p: CGPath, _ y0: CGFloat, _ y1: CGFloat, _ c0: CGColor, _ c1: CGColor) {
        g.saveGState()
        g.addPath(p)
        g.clip()
        let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [c0, c1] as CFArray, locations: [0, 1])!
        g.drawLinearGradient(grad, start: CGPoint(x: 0, y: y0), end: CGPoint(x: 0, y: y1), options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
        g.restoreGState()
    }

    func glow(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, _ c: CGColor, _ strength: CGFloat = 1) {
        for i in stride(from: 5, through: 1, by: -1) {
            let a = min(1, c.alpha * 0.09 * strength)
            fillEllipse(circle(cx, cy, r + CGFloat(i) * 1.5), c.copy(alpha: a)!)
        }
    }

    // A disc lit from the top left: `mid` where the light hits, `edge` toward the rim. Swapped, it reads as a dip.
    func dome(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, _ mid: CGColor, _ edge: CGColor) {
        g.saveGState()
        g.addEllipse(in: circle(cx, cy, r))
        g.clip()
        let grad = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [mid, edge] as CFArray, locations: [0, 1])!
        let hot = CGPoint(x: cx - r * 0.3, y: cy - r * 0.35)
        g.drawRadialGradient(grad, startCenter: hot, startRadius: 0, endCenter: CGPoint(x: cx, y: cy), endRadius: r, options: [.drawsAfterEndLocation])
        g.restoreGState()
    }

    // A small white button with a gray rim, orange when pressed.
    func whiteButton(_ cx: CGFloat, _ cy: CGFloat, _ r: CGFloat, _ down: Bool) {
        fillEllipse(CGRect(x: cx - r + 0.4, y: cy - r + 0.9, width: 2 * r, height: 2 * r), PadPainter.rgb(0, 0, 0, 22))
        if down { glow(cx, cy, r, orange) }
        dome(cx, cy, r, down ? PadPainter.rgb(255, 205, 140) : PadPainter.rgb(255, 255, 255), down ? orange : PadPainter.rgb(222, 223, 227))
        ring(cx, cy, r, 0.7, seam)
    }

    // The shell: flat top, rounded shoulders, near-straight sides and round grips with a dip between them.
    func body() -> CGPath {
        let left: [CGPoint] = [
            CGPoint(x: 120, y: 6), CGPoint(x: 100, y: 6), CGPoint(x: 80, y: 5), CGPoint(x: 62, y: 7),
            CGPoint(x: 36, y: 9), CGPoint(x: 14, y: 18), CGPoint(x: 6, y: 42),
            CGPoint(x: 0, y: 62), CGPoint(x: 0, y: 100), CGPoint(x: 3, y: 132),
            CGPoint(x: 6, y: 160), CGPoint(x: 28, y: 170), CGPoint(x: 46, y: 164),
            CGPoint(x: 60, y: 158), CGPoint(x: 68, y: 144), CGPoint(x: 80, y: 134),
            CGPoint(x: 94, y: 126), CGPoint(x: 106, y: 124), CGPoint(x: 120, y: 124),
        ]
        var pts = left
        for i in stride(from: left.count - 2, through: 0, by: -1) { pts.append(CGPoint(x: 240 - left[i].x, y: left[i].y)) }
        let p = CGMutablePath()
        p.move(to: pts[0])
        var i = 1
        while i + 2 < pts.count {
            p.addCurve(to: pts[i + 2], control1: pts[i], control2: pts[i + 1])
            i += 3
        }
        p.closeSubpath()
        return p
    }

    func paint() {
        // Triggers sit behind the controller, out of sight; pulled, an orange tab rises behind the shoulder.
        for side in 0..<2 {
            let v = CGFloat(side == 1 ? s.rt : s.lt), x: CGFloat = side == 1 ? 172 : 32
            guard v > 0 else { continue }
            let p = roundRect(x, 6 - 10 * v, 36, 12, 5)
            linear(p, -4, 8, PadPainter.rgb(255, 196, 120), orange)
            stroke(p, PadPainter.rgb(0, 0, 0, 50), 0.8)
        }
        // Bumpers: a thin white band along each shoulder, just above the shell.
        for side in 0..<2 {
            let X: (CGFloat) -> CGFloat = { side == 1 ? 240 - $0 : $0 }
            let down = on(side == 1 ? .rightShoulder : .leftShoulder)
            let band = CGMutablePath()
            band.move(to: CGPoint(x: X(22), y: 13))
            band.addCurve(to: CGPoint(x: X(54), y: 4.4), control1: CGPoint(x: X(30), y: 8), control2: CGPoint(x: X(42), y: 5))
            band.addCurve(to: CGPoint(x: X(78), y: 4.6), control1: CGPoint(x: X(62), y: 4), control2: CGPoint(x: X(70), y: 4))
            stroke(band, PadPainter.rgb(0, 0, 0, 46), 8.4, round: true)
            stroke(band, down ? orange : PadPainter.rgb(246, 246, 248), 7, round: true)
        }
        // Shell: a light shadow under it (so it separates from a white video), white shading to light gray.
        let shell = body()
        for i in stride(from: 3, through: 1, by: -1) {
            var t = CGAffineTransform(translationX: 0, y: 1.2 * CGFloat(i))
            if let sh = shell.copy(using: &t) { stroke(sh, PadPainter.rgb(0, 0, 0, 12), 2.2 * CGFloat(i)) }
        }
        linear(shell, 6, 170, PadPainter.rgb(255, 255, 255), PadPainter.rgb(238, 238, 241))
        // Soft shading just inside the edge, like the curve of the shell turning away.
        g.saveGState()
        g.addPath(shell)
        g.clip()
        for (w, a) in [(14.0, 9), (9.0, 10), (5.0, 12), (2.4, 16)] { stroke(shell, PadPainter.rgb(150, 152, 160, a), w) }
        g.restoreGState()
        stroke(shell, PadPainter.rgb(0, 0, 0, 48), 0.9)

        // Raised mounds around the sticks, and the D-pad's round plate.
        for c in [Gamepad.leftStick, CGPoint(x: 157.8, y: 89.6)] {
            fillEllipse(CGRect(x: c.x - 26, y: c.y - 25, width: 53, height: 53), PadPainter.rgb(0, 0, 0, 8))
            dome(c.x, c.y, 26.5, PadPainter.rgb(255, 255, 255), PadPainter.rgb(240, 240, 243))
        }
        let dpx: CGFloat = 82.6, dpy: CGFloat = 90.6
        dome(dpx, dpy, 28.5, PadPainter.rgb(222, 223, 227), PadPainter.rgb(243, 243, 245))  // darker where the light comes from: a dip
        ring(dpx, dpy, 28.5, 0.8, PadPainter.rgb(0, 0, 0, 30))

        middle()
        sticks()
        dpad(dpx, dpy)
        faceButtons()
    }

    // View, home, Menu; two small buttons with a dot; a pill; three status lights.
    func middle() {
        whiteButton(84.6, 26.8, 6.4, on(.view))
        whiteButton(154.4, 26.8, 6.4, on(.menu))
        let iconV = on(.view) ? PadPainter.rgb(255, 255, 255) : PadPainter.rgb(120, 121, 128)
        let iconM = on(.menu) ? PadPainter.rgb(255, 255, 255) : PadPainter.rgb(120, 121, 128)
        line(CGPoint(x: 82.4, y: 26.8), CGPoint(x: 86.8, y: 26.8), iconV, 0.9)  // ‹ with a stem
        line(CGPoint(x: 82.4, y: 26.8), CGPoint(x: 84.2, y: 25), iconV, 0.9)
        line(CGPoint(x: 82.4, y: 26.8), CGPoint(x: 84.2, y: 28.6), iconV, 0.9)
        line(CGPoint(x: 152.2, y: 26.8), CGPoint(x: 156.6, y: 26.8), iconM, 0.9)  // stem with a ›
        line(CGPoint(x: 156.6, y: 26.8), CGPoint(x: 154.8, y: 25), iconM, 0.9)
        line(CGPoint(x: 156.6, y: 26.8), CGPoint(x: 154.8, y: 28.6), iconM, 0.9)
        whiteButton(119.7, 26.8, 7.9, false)  // home
        let gray = PadPainter.rgb(140, 141, 148)
        stroke(roundRect(117.2, 24.3, 5, 5, 1.2), gray, 0.8)
        line(CGPoint(x: 119.7, y: 24.3), CGPoint(x: 119.7, y: 29.3), gray, 0.8)
        line(CGPoint(x: 117.2, y: 26.8), CGPoint(x: 122.2, y: 26.8), gray, 0.8)
        ring(119.7, 26.8, 5.8, 0.6, PadPainter.rgb(0, 0, 0, 30))
        whiteButton(101.4, 47.5, 6.0, false)
        whiteButton(138.5, 47.5, 6.0, false)
        let arc = CGMutablePath()  // a small circular arrow (GDI+ DrawArc 200° + 250°)
        arc.addArc(center: CGPoint(x: 101.4, y: 47.5), radius: 2.4, startAngle: 200 * .pi / 180, endAngle: 450 * .pi / 180, clockwise: false)
        stroke(arc, gray, 0.8)
        line(CGPoint(x: 136.5, y: 49.5), CGPoint(x: 140.5, y: 45.5), gray, 0.8)  // a small slash
        fillEllipse(CGRect(x: 118.5, y: 46.3, width: 2.4, height: 2.4), PadPainter.rgb(70, 71, 78))
        let pill = roundRect(112.8, 65.4, 13.9, 6.9, 3.45)
        fillEllipse(CGRect(x: 112.8, y: 66.4, width: 13.9, height: 6.9), PadPainter.rgb(0, 0, 0, 20))
        linear(pill, 65.4, 72.3, PadPainter.rgb(255, 255, 255), PadPainter.rgb(224, 225, 229))
        stroke(pill, seam, 0.7)
        for y: CGFloat in [84.7, 90.6, 97] { fillEllipse(CGRect(x: 118.7, y: y - 1, width: 2, height: 2), PadPainter.rgb(96, 97, 104)) }
    }

    // Sticks: an orange ring fixed in the shell (glowing), a dark gap, and a black cap that tilts with the stick.
    func sticks() {
        for side in 0..<2 {
            let cx: CGFloat = side == 1 ? 157.8 : Gamepad.leftStick.x, cy: CGFloat = side == 1 ? 89.6 : Gamepad.leftStick.y
            let click = on(side == 1 ? .rightThumb : .leftThumb)
            glow(cx, cy, 18.6, orange, click ? 2.2 : 0.8)
            ring(cx, cy, 16.6, 4.2, click ? PadPainter.rgb(255, 238, 205) : orange)
            ring(cx, cy, 15.2, 1.0, click ? PadPainter.rgb(255, 255, 255) : orangeHot)  // the bright inner edge of the light
            fillEllipse(circle(cx, cy, 14.4), PadPainter.rgb(38, 38, 42))
            let x = cx + CGFloat(side == 1 ? s.rx : s.lx) * 4.5, y = cy - CGFloat(side == 1 ? s.ry : s.ly) * 4.5
            fillEllipse(CGRect(x: x - 12.9, y: y - 12.2, width: 25.8, height: 25.8), PadPainter.rgb(0, 0, 0, 70))
            dome(x, y, 12.9, PadPainter.rgb(76, 76, 80), PadPainter.rgb(12, 12, 14))
            ring(x, y, 9.4, 1.0, PadPainter.rgb(0, 0, 0, 110))  // the dished top
            let shine = CGMutablePath()
            shine.addArc(center: CGPoint(x: x, y: y), radius: 11, startAngle: 200 * .pi / 180, endAngle: 270 * .pi / 180, clockwise: false)
            stroke(shine, PadPainter.rgb(255, 255, 255, 40), 1.0)
        }
    }

    // D-pad: black and glossy, with arrow notches and a raised middle; the pressed arm lights orange.
    func dpad(_ cx: CGFloat, _ cy: CGFloat) {
        let a: CGFloat = 8, len: CGFloat = 22.5
        let cross = roundRect(cx - a, cy - len, 2 * a, 2 * len, 3.2).union(roundRect(cx - len, cy - a, 2 * len, 2 * a, 3.2))
        var down = CGAffineTransform(translationX: 0.6, y: 1.4)
        if let sh = cross.copy(using: &down) { fill(sh, PadPainter.rgb(0, 0, 0, 50)) }
        linear(cross, cy - len, cy + len, PadPainter.rgb(56, 56, 60), PadPainter.rgb(16, 16, 18))
        let arms: [(PadButtons, CGRect, Int)] = [  // 0 up, 1 down, 2 left, 3 right
            (.up, CGRect(x: cx - a, y: cy - len, width: 2 * a, height: len - a), 0),
            (.down, CGRect(x: cx - a, y: cy + a, width: 2 * a, height: len - a), 1),
            (.left, CGRect(x: cx - len, y: cy - a, width: len - a, height: 2 * a), 2),
            (.right, CGRect(x: cx + a, y: cy - a, width: len - a, height: 2 * a), 3),
        ]
        for (b, r, dir) in arms {
            let pressed = on(b)
            if pressed { fill(roundRect(r.minX + 0.6, r.minY + 0.6, r.width - 1.2, r.height - 1.2, 2.6), orange) }
            let mx = r.midX, my = r.midY, t: CGFloat = 3.6
            let tri: [CGPoint]
            switch dir {
            case 0: tri = [CGPoint(x: mx, y: my - t), CGPoint(x: mx - t, y: my + t * 0.55), CGPoint(x: mx + t, y: my + t * 0.55)]
            case 1: tri = [CGPoint(x: mx, y: my + t), CGPoint(x: mx - t, y: my - t * 0.55), CGPoint(x: mx + t, y: my - t * 0.55)]
            case 2: tri = [CGPoint(x: mx - t, y: my), CGPoint(x: mx + t * 0.55, y: my - t), CGPoint(x: mx + t * 0.55, y: my + t)]
            default: tri = [CGPoint(x: mx + t, y: my), CGPoint(x: mx - t * 0.55, y: my - t), CGPoint(x: mx - t * 0.55, y: my + t)]
            }
            let p = CGMutablePath()
            p.addLines(between: tri)
            p.closeSubpath()
            fill(p, pressed ? PadPainter.rgb(255, 255, 255) : PadPainter.rgb(8, 8, 10))
            if !pressed { line(tri[1], tri[2], PadPainter.rgb(255, 255, 255, 34), 0.6) }
        }
        stroke(cross, PadPainter.rgb(255, 255, 255, 30), 0.7)
        dome(cx, cy, 5.2, PadPainter.rgb(60, 60, 64), PadPainter.rgb(14, 14, 16))
        ring(cx, cy, 5.2, 0.7, PadPainter.rgb(0, 0, 0, 160))
    }

    // A, B, X and Y: glossy black with letters in their colors; pressed, they fill with their color and glow.
    func faceButtons() {
        let cx = Gamepad.aButton.x, cy = Gamepad.aButton.y - 18.6, d: CGFloat = 18.6, r: CGFloat = 8.9
        let ps = s.playStation
        let face: [(PadButtons, CGFloat, CGFloat, String, (Int, Int, Int))] = [
            (.a, cx, cy + d, ps ? "✕" : "A", (46, 184, 74)),
            (.b, cx + d, cy, ps ? "○" : "B", (232, 44, 56)),
            (.x, cx - d, cy, ps ? "□" : "X", (42, 132, 242)),
            (.y, cx, cy - d, ps ? "△" : "Y", (244, 204, 20)),
        ]
        let font = CTFontCreateWithName("SF Pro Text Semibold" as CFString, 10.5, nil)
        for (b, x, y, t, c) in face {
            let pressed = on(b)
            let color = PadPainter.rgb(c.0, c.1, c.2)
            fillEllipse(CGRect(x: x - r + 0.4, y: y - r + 1.1, width: 2 * r, height: 2 * r), PadPainter.rgb(0, 0, 0, 45))
            if pressed { glow(x, y, r, color, 2) }
            let hi = pressed ? PadPainter.rgb(min(255, c.0 + 70), min(255, c.1 + 70), min(255, c.2 + 70)) : PadPainter.rgb(74, 74, 80)
            dome(x, y, r, hi, pressed ? color : PadPainter.rgb(6, 6, 8))
            fillEllipse(CGRect(x: x - r * 0.55, y: y - r * 0.85, width: r * 1.1, height: r * 0.55), PadPainter.rgb(255, 255, 255, 46))
            let str = NSAttributedString(string: t, attributes: [.font: font, .foregroundColor: pressed ? PadPainter.rgb(255, 255, 255) : color])
            let ln = CTLineCreateWithAttributedString(str)
            let bounds = CTLineGetBoundsWithOptions(ln, .useGlyphPathBounds)
            g.saveGState()
            g.textMatrix = .identity
            g.translateBy(x: x - bounds.midX, y: y + bounds.midY + 0.4)
            g.scaleBy(x: 1, y: -1)  // text draws y up
            g.textPosition = .zero
            CTLineDraw(ln, g)
            g.restoreGState()
        }
    }
}
