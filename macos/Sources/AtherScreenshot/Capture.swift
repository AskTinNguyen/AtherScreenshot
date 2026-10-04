import AppKit
import ScreenCaptureKit

struct DisplayShot {
    let displayID: CGDirectDisplayID
    let frame: CGRect      // CG global points
    let image: CGImage
    var scale: CGFloat { CGFloat(image.width) / frame.width }
}

// Frozen picture of every display, taken before the region overlay appears.
final class Snapshot {
    let shots: [DisplayShot]
    init(_ shots: [DisplayShot]) { self.shots = shots }

    func shot(at p: CGPoint) -> DisplayShot? { shots.first { $0.frame.contains(p) } }
    var bounds: CGRect { shots.reduce(CGRect.null) { $0.union($1.frame) } }

    // Pixel scale of the result: the highest scale among the displays it touches.
    func scale(for r: CGRect) -> CGFloat {
        shots.filter { $0.frame.intersects(r) }.map(\.scale).max() ?? 1
    }

    func crop(_ r: CGRect) -> CGImage? {
        let r = r.intersection(bounds)
        guard !r.isEmpty else { return nil }
        let hits = shots.filter { $0.frame.intersects(r) }
        if hits.count == 1, let s = hits.first {
            let px = CGRect(x: (r.minX - s.frame.minX) * s.scale, y: (r.minY - s.frame.minY) * s.scale,
                            width: r.width * s.scale, height: r.height * s.scale)
            return s.image.cropped(px.integral)
        }
        let k = scale(for: r)
        guard let ctx = makeContext(width: Int((r.width * k).rounded()), height: Int((r.height * k).rounded())) else { return nil }
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: r.width * k, height: r.height * k))
        for s in hits {
            let dst = CGRect(x: (s.frame.minX - r.minX) * k, y: (s.frame.minY - r.minY) * k,
                             width: s.frame.width * k, height: s.frame.height * k)
            drawImageFlipped(ctx, s.image, in: dst)
        }
        return ctx.makeImage()
    }

    func color(at p: CGPoint) -> NSColor? {
        guard let s = shot(at: p) else { return nil }
        return s.image.color(atPixel: CGPoint(x: (p.x - s.frame.minX) * s.scale, y: (p.y - s.frame.minY) * s.scale))
    }
}

struct WindowInfo {
    let id: CGWindowID
    let pid: pid_t
    let frame: CGRect    // CG global points
    let app: String
    let title: String
}

enum CaptureError: LocalizedError {
    case permission, failed(String)
    var errorDescription: String? {
        switch self {
        case .permission: return "Allow Ather Screenshot in System Settings › Privacy & Security › Screen & System Audio Recording, then try again."
        case .failed(let s): return s
        }
    }
}

enum Capture {
    // CGPreflightScreenCaptureAccess answers once per process: a grant made while we run still reads
    // as denied until relaunch. ScreenCaptureKit checks live, so resolvePermission() asks it too.
    private static var grantedLive = false
    static func hasPermission() -> Bool { grantedLive || CGPreflightScreenCaptureAccess() }

    static func resolvePermission() async -> Bool {
        if hasPermission() { return true }
        guard (try? await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)) != nil else { return false }
        grantedLive = true
        return true
    }

    // Asks once; macOS shows its own prompt the first time.
    static func ensurePermission() -> Bool {
        if CGPreflightScreenCaptureAccess() { return true }
        CGRequestScreenCaptureAccess()
        return false
    }

    static func content() async throws -> SCShareableContent {
        try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
    }

    // Our own transient chrome (marked with sharingType = .none) is left out explicitly too:
    // newer macOS versions no longer honour sharingType for ScreenCaptureKit.
    static func excludedWindows(_ content: SCShareableContent) -> [SCWindow] {
        let ours = Set(NSApp.windows.filter { $0.isVisible && $0.sharingType == .none }.map { CGWindowID($0.windowNumber) })
        return content.windows.filter { ours.contains($0.windowID) }
    }

    static func scale(of id: CGDirectDisplayID) -> CGFloat {
        NSScreen.screens.first { Geo.displayID($0) == id }?.backingScaleFactor ?? 2
    }

    static func capture(display: SCDisplay, content: SCShareableContent, rect: CGRect? = nil, cursor: Bool) async throws -> CGImage {
        let filter = SCContentFilter(display: display, excludingWindows: excludedWindows(content))
        let cfg = SCStreamConfiguration()
        let frame = CGDisplayBounds(display.displayID)
        let local = rect.map { CGRect(x: $0.minX - frame.minX, y: $0.minY - frame.minY, width: $0.width, height: $0.height) }
        let size = local?.size ?? frame.size
        let k = scale(of: display.displayID)
        if let local { cfg.sourceRect = local }
        cfg.width = max(1, Int((size.width * k).rounded()))
        cfg.height = max(1, Int((size.height * k).rounded()))
        cfg.showsCursor = cursor
        cfg.captureResolution = .best
        cfg.colorSpaceName = CGColorSpace.sRGB
        return try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg)
    }

    static func snapshot(cursor: Bool, only: CGRect? = nil) async throws -> Snapshot {
        guard hasPermission() else { throw CaptureError.permission }
        let content = try await content()
        var shots: [DisplayShot] = []
        for d in content.displays {
            let frame = CGDisplayBounds(d.displayID)
            if let only, !frame.intersects(only) { continue }
            let img = try await capture(display: d, content: content, cursor: cursor)
            shots.append(DisplayShot(displayID: d.displayID, frame: frame, image: img))
        }
        if shots.isEmpty { throw CaptureError.failed("No display to capture.") }
        return Snapshot(shots)
    }

    // A live capture of one rectangle (scrolling capture, repeat last region).
    static func rect(_ r: CGRect, cursor: Bool) async throws -> CGImage {
        guard hasPermission() else { throw CaptureError.permission }
        let content = try await content()
        let hits = content.displays.filter { CGDisplayBounds($0.displayID).intersects(r) }
        if hits.count == 1, let d = hits.first {
            let clipped = r.intersection(CGDisplayBounds(d.displayID))
            return try await capture(display: d, content: content, rect: clipped, cursor: cursor)
        }
        guard let img = try await snapshot(cursor: cursor, only: r).crop(r) else { throw CaptureError.failed("Nothing to capture there.") }
        return img
    }

    static func window(_ id: CGWindowID, cursor: Bool) async throws -> CGImage {
        guard hasPermission() else { throw CaptureError.permission }
        let content = try await content()
        guard let w = content.windows.first(where: { $0.windowID == id }) else { throw CaptureError.failed("That window is gone.") }
        let filter = SCContentFilter(desktopIndependentWindow: w)
        let cfg = SCStreamConfiguration()
        let k = CGFloat(filter.pointPixelScale)
        cfg.width = max(1, Int(filter.contentRect.width * k))
        cfg.height = max(1, Int(filter.contentRect.height * k))
        cfg.showsCursor = cursor
        cfg.captureResolution = .best
        cfg.colorSpaceName = CGColorSpace.sRGB
        return try await SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg)
    }

    // On-screen app windows, front to back, without our own.
    static func windows() -> [WindowInfo] {
        guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return [] }
        let me = ProcessInfo.processInfo.processIdentifier
        return list.compactMap { d in
            guard (d[kCGWindowLayer as String] as? Int) == 0,
                  let pid = d[kCGWindowOwnerPID as String] as? pid_t, pid != me,
                  let b = d[kCGWindowBounds as String] as? NSDictionary,
                  let frame = CGRect(dictionaryRepresentation: b), frame.width > 40, frame.height > 30,
                  (d[kCGWindowAlpha as String] as? Double ?? 1) > 0.01,
                  let id = d[kCGWindowNumber as String] as? CGWindowID else { return nil }
            return WindowInfo(id: id, pid: pid, frame: frame, app: d[kCGWindowOwnerName as String] as? String ?? "",
                              title: d[kCGWindowName as String] as? String ?? "")
        }
    }

    static func window(at p: CGPoint, in list: [WindowInfo]) -> WindowInfo? { list.first { $0.frame.contains(p) } }

    // The front window of the frontmost app (our app is a background agent, so it never is).
    static func activeWindow() -> WindowInfo? {
        let list = windows()
        if let pid = NSWorkspace.shared.frontmostApplication?.processIdentifier, let w = list.first(where: { $0.pid == pid }) { return w }
        return list.first
    }
}
