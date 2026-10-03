import AppKit
import Carbon.HIToolbox

// Always-on-top image. Drag to move, scroll to zoom, ⌥-scroll for opacity, double-click or Esc closes.
final class Pin: NSObject, NSWindowDelegate {
    static var all: [Pin] = []

    let image: CGImage
    let scale: CGFloat
    private let panel: KeyablePanel
    private let view: PinView
    private var zoom: CGFloat = 1

    @discardableResult
    static func show(_ img: CGImage, at frameCG: CGRect? = nil, scale: CGFloat? = nil) -> Pin {
        let p = Pin(img, frameCG: frameCG, scale: scale)
        all.append(p)
        return p
    }

    static func closeAll() { for p in all { p.close() } }

    private init(_ img: CGImage, frameCG: CGRect?, scale: CGFloat?) {
        image = img
        self.scale = scale ?? frameCG.map { CGFloat(img.width) / max(1, $0.width) } ?? Geo.mouseScreen.backingScaleFactor
        var size = NSSize(width: CGFloat(img.width) / self.scale, height: CGFloat(img.height) / self.scale)
        let vis = Geo.mouseScreen.visibleFrame
        let fit = min(1, vis.width * 0.8 / size.width, vis.height * 0.8 / size.height)
        size = NSSize(width: size.width * fit, height: size.height * fit)
        zoom = fit
        let frame = frameCG.map { Geo.toNS($0) } ?? NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height)
        panel = KeyablePanel(contentRect: NSRect(origin: frame.origin, size: size), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        view = PinView(frame: NSRect(origin: .zero, size: size))
        super.init()
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.level = .floating
        panel.isMovableByWindowBackground = true
        panel.isReleasedWhenClosed = false
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        panel.delegate = self
        view.image = img
        view.pin = self
        panel.contentView = view
        panel.onKey = { [weak self] e in self?.key(e) ?? false }
        panel.orderFrontRegardless()
    }

    func close() {
        panel.orderOut(nil)
        Pin.all.removeAll { $0 === self }
    }

    func setZoom(_ z: CGFloat, around p: NSPoint) {
        let z = min(8, max(0.1, z))
        let old = panel.frame
        let size = NSSize(width: CGFloat(image.width) / scale * z, height: CGFloat(image.height) / scale * z)
        let fx = (p.x - old.minX) / old.width, fy = (p.y - old.minY) / old.height
        panel.setFrame(NSRect(x: p.x - size.width * fx, y: p.y - size.height * fy, width: size.width, height: size.height), display: true)
        zoom = z
    }

    func scroll(_ e: NSEvent) {
        let dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 40 : e.scrollingDeltaY / 4
        if e.modifierFlags.contains(.option) {
            panel.alphaValue = min(1, max(0.15, panel.alphaValue + dy * 0.1))
        } else {
            setZoom(zoom * pow(1.1, dy), around: NSEvent.mouseLocation)
        }
    }

    private func key(_ e: NSEvent) -> Bool {
        let cmd = e.modifierFlags.contains(.command)
        switch Int(e.keyCode) {
        case kVK_Escape: close()
        case kVK_ANSI_C where cmd: copyImage(image); Toast.shared.show("Copied to clipboard")
        case kVK_ANSI_E where cmd: edit()
        case kVK_ANSI_0 where cmd: setZoom(1, around: NSEvent.mouseLocation)
        case kVK_ANSI_W where cmd: close()
        default: return false
        }
        return true
    }

    @objc func copyAction() { copyImage(image); Toast.shared.show("Copied to clipboard") }
    @objc func edit() { Editor.open(image, scale: scale); close() }
    @objc func save() {
        let url = Output.newCaptureURL(ext: "png", info: NameInfo(w: image.width, h: image.height))
        Output.savePNG(image, to: url) { ok in Toast.shared.show(ok ? "Saved" : "Save failed", url.lastPathComponent) }
    }
    @objc func actualSize() { setZoom(1, around: NSEvent.mouseLocation) }
    @objc func setOpacity(_ sender: NSMenuItem) { panel.alphaValue = CGFloat(sender.tag) / 100 }
    @objc func closeAction() { close() }
    @objc func closeAllAction() { Pin.closeAll() }

    func menu() -> NSMenu {
        let m = NSMenu()
        m.addItem(withTitle: "Copy", action: #selector(copyAction), keyEquivalent: "c").target = self
        m.addItem(withTitle: "Annotate", action: #selector(edit), keyEquivalent: "e").target = self
        m.addItem(withTitle: "Save to captures", action: #selector(save), keyEquivalent: "").target = self
        m.addItem(withTitle: "Actual size", action: #selector(actualSize), keyEquivalent: "0").target = self
        let op = NSMenuItem(title: "Opacity", action: nil, keyEquivalent: "")
        let sub = NSMenu()
        for v in [100, 80, 60, 40, 20] {
            let i = sub.addItem(withTitle: "\(v)%", action: #selector(setOpacity(_:)), keyEquivalent: "")
            i.tag = v
            i.target = self
            i.state = abs(panel.alphaValue * 100 - CGFloat(v)) < 1 ? .on : .off
        }
        op.submenu = sub
        m.addItem(op)
        m.addItem(.separator())
        m.addItem(withTitle: "Close", action: #selector(closeAction), keyEquivalent: "").target = self
        m.addItem(withTitle: "Close all pins", action: #selector(closeAllAction), keyEquivalent: "").target = self
        return m
    }
}

private final class PinView: NSView {
    var image: CGImage?
    weak var pin: Pin?
    private var hover = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        autoresizingMask = [.width, .height]
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var mouseDownCanMoveWindow: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        guard let image, let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.interpolationQuality = .high
        ctx.draw(image, in: bounds)
        let border = NSBezierPath(rect: bounds.insetBy(dx: 0.5, dy: 0.5))
        border.lineWidth = 1
        (hover ? Theme.accent : Theme.border).setStroke()
        border.stroke()
    }

    override func mouseEntered(with event: NSEvent) { hover = true; needsDisplay = true }
    override func mouseExited(with event: NSEvent) { hover = false; needsDisplay = true }
    override func mouseDown(with event: NSEvent) {
        window?.makeKey()
        if event.clickCount == 2 { pin?.close() } else { super.mouseDown(with: event) }
    }
    override func scrollWheel(with event: NSEvent) { pin?.scroll(event) }
    override func menu(for event: NSEvent) -> NSMenu? { pin?.menu() }
}
