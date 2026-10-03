import AppKit
import Carbon.HIToolbox

// Scrolls the area under the region with synthetic wheel events and stitches the frames,
// keeping sticky headers and footers only once. Needs Accessibility access to post events.
final class ScrollCapture {
    static var active: ScrollCapture?

    private struct Frame {
        let w: Int, h: Int
        let px: [UInt32]
        var hashes: [UInt64]
    }

    private let region: CGRect
    private let delayMs: Int
    private let maxFrames: Int
    private let done: (CGImage?, Int, String?) -> Void
    private var frames: [Frame] = []
    private var offsets: [Int] = []
    private var top = -1, bottom = -1
    private var lines: Int32 = 3
    private var stalls = 0
    private var savedCursor = Geo.mouse
    private var cancelled = false

    static func start(region: CGRect, delayMs: Int, maxFrames: Int, done: @escaping (CGImage?, Int, String?) -> Void) {
        guard active == nil else { return }
        guard AXIsProcessTrustedWithOptions([kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary) else {
            done(nil, 0, "Scrolling capture sends scroll events, so it needs Accessibility access: System Settings › Privacy & Security › Accessibility.")
            return
        }
        let s = ScrollCapture(region: region, delayMs: delayMs, maxFrames: max(2, maxFrames), done: done)
        active = s
        Task { @MainActor in await s.run() }
    }

    static func cancel() { active?.cancelled = true }

    private init(region: CGRect, delayMs: Int, maxFrames: Int, done: @escaping (CGImage?, Int, String?) -> Void) {
        self.region = region
        self.delayMs = min(5000, max(50, delayMs))  // also reachable through `defaults write`
        self.maxFrames = min(1000, maxFrames)
        self.done = done
    }

    private static func pixels(_ img: CGImage) -> Frame? {
        let w = img.width, h = img.height
        var px = [UInt32](repeating: 0, count: w * h)
        let ok = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return false }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        guard ok else { return nil }
        var hashes = [UInt64](repeating: 0, count: h)
        for y in 0..<h {
            var v: UInt64 = 1_469_598_103_934_665_603  // FNV-1a over the row
            for x in 0..<w { v = (v ^ UInt64(px[y * w + x] & 0x00FF_FFFF)) &* 1_099_511_628_211 }
            hashes[y] = v
        }
        return Frame(w: w, h: h, px: px, hashes: hashes)
    }

    // How far content moved up between `a` (before) and `b` (after), ignoring sticky rows.
    // Rows of constant colour match everything, so they don't count as evidence.
    private static func findOffset(_ a: [UInt64], _ b: [UInt64], top: Int, bottom: Int) -> Int {
        let h = a.count, lo = top, hi = h - bottom
        var best = 0, bestHits = 0
        guard hi - lo - 24 > 1 else { return 0 }
        for d in 1..<(hi - lo - 24) {
            var hits = 0, n = 0
            var y = lo
            while y + d < hi {
                if a[y + d] == a[min(y + d + 1, hi - 1)] && b[y] == b[min(y + 1, hi - 1)] { y += 1; continue }  // flat area
                n += 1
                if a[y + d] == b[y] { hits += 1 }
                y += 1
            }
            if n >= 12, hits > bestHits, Double(hits) / Double(n) >= 0.6 {
                bestHits = hits
                best = d
            }
        }
        return best
    }

    private func wheel(_ lines: Int32) {
        let e = CGEvent(scrollWheelEvent2Source: nil, units: .line, wheelCount: 1, wheel1: -lines, wheel2: 0, wheel3: 0)
        e?.location = region.center
        e?.post(tap: .cghidEventTap)
    }

    private func escPressed() -> Bool { CGEventSource.keyState(.combinedSessionState, key: CGKeyCode(kVK_Escape)) }

    private func grab() async -> Frame? {
        guard let img = try? await Capture.rect(region, cursor: false) else { return nil }
        return ScrollCapture.pixels(img)
    }

    private func run() async {
        guard let first = await grab() else { return finish("Capture failed.") }
        frames.append(first)
        // The wheel goes to whatever is under the cursor: park it in the middle of the region.
        CGWarpMouseCursorPosition(region.center)
        var last = first.hashes
        while !cancelled && !escPressed() && frames.count < maxFrames {
            wheel(lines)
            try? await Task.sleep(nanoseconds: UInt64(delayMs) * 1_000_000)
            guard let f = await grab() else { return finish("Capture failed.") }
            let h = f.h
            if top < 0 {  // sticky rows: identical at the same position in both frames, from the edges inward
                var t = 0, b = 0
                while t < h / 3 && f.hashes[t] == last[t] { t += 1 }
                while b < h / 3 && f.hashes[h - 1 - b] == last[h - 1 - b] { b += 1 }
                if t + b >= h - 24 {  // nothing scrolled
                    stalls += 1
                    if stalls >= 2 { break }
                    continue
                }
                top = t
                bottom = b
            }
            let d = ScrollCapture.findOffset(last, f.hashes, top: top, bottom: bottom)
            if d == 0 {
                stalls += 1
                if stalls >= 2 { break }  // reached the end (or the page stopped moving)
                continue
            }
            stalls = 0
            // Aim for ~60% of the band per step: enough overlap for a reliable match, few frames.
            let band = h - top - bottom
            let perLine = Double(d) / Double(lines)
            if perLine > 0 { lines = Int32(min(40, max(1, (Double(band) * 0.6 / perLine).rounded()))) }
            frames.append(f)
            offsets.append(d)
            last = f.hashes
        }
        finish(nil)
    }

    private func stitch() -> CGImage? {
        guard let f0 = frames.first else { return nil }
        let w = f0.w, h = f0.h, bottom = max(0, self.bottom)
        let total = h + offsets.reduce(0, +)
        var out = [UInt32](repeating: 0, count: w * total)
        func copy(_ src: Frame, _ srcY: Int, _ dstY: Int, _ rows: Int) {
            guard rows > 0 else { return }
            out.withUnsafeMutableBufferPointer { o in
                src.px.withUnsafeBufferPointer { s in
                    (o.baseAddress! + dstY * w).update(from: s.baseAddress! + srcY * w, count: rows * w)
                }
            }
        }
        // First frame without its footer, then the newly revealed rows of each frame, then the footer once.
        copy(f0, 0, 0, h - bottom)
        var y = h - bottom
        for (i, d) in offsets.enumerated() {
            copy(frames[i + 1], h - bottom - d, y, d)
            y += d
        }
        copy(frames.last!, h - bottom, y, bottom)
        let data = out.withUnsafeBufferPointer { Data(buffer: $0) }
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: w, height: total, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    private func finish(_ err: String?) {
        ScrollCapture.active = nil
        CGWarpMouseCursorPosition(savedCursor)
        let img = err == nil ? stitch() : nil
        done(img, frames.count, err)
    }
}
