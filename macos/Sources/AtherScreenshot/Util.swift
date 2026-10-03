import AppKit
import CoreImage
import ImageIO
import UniformTypeIdentifiers

let kAppName = "AtherScreenshot"
let kProductName = "Ather Screenshot"
let kBundleID = "com.ather.screenshot"

// Ather brand: warm near-black, off-white type, one electric-lime accent.
enum Theme {
    static func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a)
    }
    static let bg = rgb(14, 14, 13)            // #0E0E0D page
    static let surface = rgb(26, 27, 25)       // #1A1B19 cards, bars
    static let raised = rgb(37, 38, 35)        // #252623 inputs, pills
    static let selected = rgb(42, 45, 33)      // lime-tinted selection
    static let border = rgb(50, 52, 46)        // #32342E hairlines
    static let text = rgb(242, 239, 232)       // #F2EFE8 warm off-white
    static let textDim = rgb(208, 206, 199)
    static let muted = rgb(150, 150, 142)      // #96968E captions
    static let accent = rgb(212, 255, 0)       // #D4FF00 Ather lime
    static let onAccent = rgb(14, 14, 13)
    static let logoGray = rgb(201, 204, 205)

    static func font(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        .systemFont(ofSize: size, weight: weight)
    }
    static func mono(_ size: CGFloat, _ weight: NSFont.Weight = .medium) -> NSFont {
        .monospacedSystemFont(ofSize: size, weight: weight)
    }
}

// MARK: - Geometry
// Capture rectangles are kept in CoreGraphics global coordinates: points, origin at the top-left of
// the primary display, y down. AppKit window frames use the bottom-left origin; convert at the edges.
enum Geo {
    static var primaryHeight: CGFloat { NSScreen.screens.first?.frame.height ?? 0 }

    static func toCG(_ r: NSRect) -> CGRect {
        CGRect(x: r.minX, y: primaryHeight - r.maxY, width: r.width, height: r.height)
    }
    static func toNS(_ r: CGRect) -> NSRect {
        NSRect(x: r.minX, y: primaryHeight - r.maxY, width: r.width, height: r.height)
    }
    static func toCG(_ p: NSPoint) -> CGPoint { CGPoint(x: p.x, y: primaryHeight - p.y) }
    static func toNS(_ p: CGPoint) -> NSPoint { NSPoint(x: p.x, y: primaryHeight - p.y) }

    static var mouse: CGPoint { toCG(NSEvent.mouseLocation) }

    static func displayID(_ s: NSScreen) -> CGDirectDisplayID {
        (s.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? CGMainDisplayID()
    }
    static func screen(at p: CGPoint) -> NSScreen {
        NSScreen.screens.first { toCG($0.frame).contains(p) } ?? NSScreen.main ?? NSScreen.screens[0]
    }
    static var mouseScreen: NSScreen { screen(at: mouse) }
    static var virtualRect: CGRect { NSScreen.screens.map { toCG($0.frame) }.reduce(CGRect.null) { $0.union($1) } }

    static func norm(_ a: CGPoint, _ b: CGPoint) -> CGRect {
        CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y))
    }
}

extension CGRect {
    var center: CGPoint { CGPoint(x: midX, y: midY) }
}

// MARK: - Images

extension CGImage {
    var size: CGSize { CGSize(width: width, height: height) }

    static func load(_ url: URL) -> CGImage? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    func pngData() -> Data? {
        let data = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dst, self, nil)
        return CGImageDestinationFinalize(dst) ? data as Data : nil
    }

    func nsImage(scale: CGFloat = 1) -> NSImage {
        NSImage(cgImage: self, size: NSSize(width: CGFloat(width) / scale, height: CGFloat(height) / scale))
    }

    // Top-left pixel coordinates.
    func cropped(_ r: CGRect) -> CGImage? {
        let r = r.integral.intersection(CGRect(origin: .zero, size: size))
        guard !r.isEmpty else { return nil }
        return cropping(to: r)
    }

    func color(atPixel p: CGPoint) -> NSColor? {
        guard let one = cropped(CGRect(x: floor(p.x), y: floor(p.y), width: 1, height: 1)) else { return nil }
        var px = [UInt8](repeating: 0, count: 4)
        guard let ctx = CGContext(data: &px, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.draw(one, in: CGRect(x: 0, y: 0, width: 1, height: 1))
        return Theme.rgb(Int(px[0]), Int(px[1]), Int(px[2]))
    }
}

extension NSColor {
    var hex: String {
        let c = usingColorSpace(.sRGB) ?? self
        return String(format: "#%02X%02X%02X", Int(round(c.redComponent * 255)), Int(round(c.greenComponent * 255)),
                      Int(round(c.blueComponent * 255)))
    }
    var isDark: Bool {
        let c = usingColorSpace(.sRGB) ?? self
        return c.redComponent * 299 + c.greenComponent * 587 + c.blueComponent * 114 < 588
    }
}

// A bitmap context in image (top-left, y down) coordinates.
func makeContext(width: Int, height: Int, flipped: Bool = true) -> CGContext? {
    guard let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                              space: CGColorSpace(name: CGColorSpace.sRGB)!,
                              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    else { return nil }
    if flipped {
        ctx.translateBy(x: 0, y: CGFloat(height))
        ctx.scaleBy(x: 1, y: -1)
    }
    ctx.interpolationQuality = .high
    return ctx
}

// Draws a CGImage upright into a flipped (y-down) context.
func drawImageFlipped(_ ctx: CGContext, _ img: CGImage, in r: CGRect) {
    ctx.saveGState()
    ctx.translateBy(x: r.minX, y: r.maxY)
    ctx.scaleBy(x: 1, y: -1)
    ctx.draw(img, in: CGRect(origin: .zero, size: r.size))
    ctx.restoreGState()
}

func withNSContext(_ ctx: CGContext, flipped: Bool, _ body: () -> Void) {
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: flipped)
    body()
    NSGraphicsContext.restoreGraphicsState()
}

let sharedCIContext = CIContext(options: [.cacheIntermediates: false])

// MARK: - Misc

func fileName(_ url: URL) -> String { url.lastPathComponent }

func copyText(_ s: String) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.setString(s, forType: .string)
}

@discardableResult
func copyImage(_ img: CGImage) -> Bool {
    guard let png = img.pngData() else { return false }
    let pb = NSPasteboard.general
    pb.clearContents()
    let item = NSPasteboardItem()
    item.setData(png, forType: .png)
    if let tiff = img.nsImage().tiffRepresentation { item.setData(tiff, forType: .tiff) }
    return pb.writeObjects([item])
}

func copyFile(_ url: URL) {
    NSPasteboard.general.clearContents()
    NSPasteboard.general.writeObjects([url as NSURL])
}

final class KeyablePanel: NSPanel {
    var onKey: ((NSEvent) -> Bool)?
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
    override func keyDown(with event: NSEvent) {
        if onKey?(event) != true { super.keyDown(with: event) }
    }
}

// Transient chrome (palette, toasts, recording bar) never shows up in our own captures.
func excludeFromCapture(_ w: NSWindow) { w.sharingType = .none }

func roundedPanel(_ rect: NSRect, level: NSWindow.Level = .floating, key: Bool = false) -> KeyablePanel {
    let p = KeyablePanel(contentRect: rect, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
    p.isOpaque = false
    p.backgroundColor = .clear
    p.hasShadow = true
    p.level = level
    p.isReleasedWhenClosed = false
    p.hidesOnDeactivate = false
    p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
    p.becomesKeyOnlyIfNeeded = !key
    p.animationBehavior = .none
    return p
}

final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

// Brings our accessory app forward, e.g. for an editor or settings window.
func activateApp() {
    if #available(macOS 14, *) { NSApp.activate() } else { NSApp.activate(ignoringOtherApps: true) }
}
