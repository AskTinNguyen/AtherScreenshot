import AVFoundation
import AppKit
import XCTest
@testable import AtherScreenshot

final class VideoTests: XCTestCase {
    // A 3-second, 30 fps test clip whose color changes every second.
    private func makeClip() async throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("ather-clip-\(UUID().uuidString).mp4")
        let w = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 640, AVVideoHeightKey: 360])
        let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                                                                                                               kCVPixelBufferWidthKey as String: 640, kCVPixelBufferHeightKey as String: 360])
        w.add(input)
        w.startWriting()
        w.startSession(atSourceTime: .zero)
        let colors: [NSColor] = [.red, .green, .blue]
        for i in 0..<90 {
            while !input.isReadyForMoreMediaData { try await Task.sleep(nanoseconds: 2_000_000) }
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb)
            CVPixelBufferLockBaseAddress(pb!, [])
            let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb!), width: 640, height: 360, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pb!),
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
            ctx.setFillColor(colors[i / 30].cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: 640, height: 360))
            CVPixelBufferUnlockBaseAddress(pb!, [])
            ad.append(pb!, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: 30))
        }
        input.markAsFinished()
        await w.finishWriting()
        return url
    }

    func testExportTrimSpeedCropCaptions() async throws {
        let clip = try await makeClip()
        let asset = AVURLAsset(url: clip)
        var e = VideoEdit(trimEnd: 3)
        e.trimStart = 1
        e.trimEnd = 3
        e.speed = 2
        e.crop = CGRect(x: 100, y: 50, width: 321, height: 201)
        e.captions = [Caption(start: 1.2, end: 2.5, text: "Click Deploy")]
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-out-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(asset, e, to: out)
        let res = AVURLAsset(url: out)
        let dur = try await res.load(.duration).seconds
        XCTAssertEqual(dur, 1, accuracy: 0.1)                       // 2 s trimmed, played at 2×
        let size = try await VideoExport.displaySize(res)
        XCTAssertEqual(size, CGSize(width: 320, height: 200))       // crop, rounded down to even
        // The first frame comes from second 1 of the source (green), not 0 (red).
        let gen = AVAssetImageGenerator(asset: res)
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = .zero
        let first = try await gen.image(at: CMTime(seconds: 0.05, preferredTimescale: 600)).image
        let c = first.color(atPixel: CGPoint(x: 10, y: 10))!.usingColorSpace(.sRGB)!
        XCTAssertGreaterThan(c.greenComponent, 0.6)
        XCTAssertLessThan(c.redComponent, 0.4)

        // The caption (1.2–2.5 s in the source → 0.1–0.75 s out) is burned in: a dark pill near the bottom.
        let mid = try await gen.image(at: CMTime(seconds: 0.4, preferredTimescale: 600)).image
        if let o = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] { try mid.pngData()!.write(to: URL(fileURLWithPath: o).appendingPathComponent("video-frame.png")) }
        func darkPixels(_ img: CGImage) -> Int {
            stride(from: 150, to: 198, by: 4).reduce(0) { n, y in
                n + (100..<220).filter { x in (img.color(atPixel: CGPoint(x: x, y: y))?.usingColorSpace(.sRGB)?.brightnessComponent ?? 1) < 0.6 }.count
            }
        }
        XCTAssertGreaterThan(darkPixels(mid), 10, "caption background")
        let after = try await gen.image(at: CMTime(seconds: 0.9, preferredTimescale: 600)).image
        XCTAssertEqual(darkPixels(after), 0, "caption gone")

        let gif = FileManager.default.temporaryDirectory.appendingPathComponent("ather-out-\(UUID().uuidString).gif")
        try await VideoExport.gif(asset, e, to: gif, fps: 10)
        let src = CGImageSourceCreateWithURL(gif as CFURL, nil)!
        XCTAssertEqual(CGImageSourceGetCount(src), 10)
    }

    func testEditorWindowSnapshot() async throws {
        guard let out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] else { throw XCTSkip("set ATHER_TEST_OUT") }
        let clip = try await makeClip()
        await MainActor.run { _ = NSApplication.shared; VideoEditor.open(clip) }
        try await Task.sleep(nanoseconds: 1_500_000_000)
        await MainActor.run {
            let e = VideoEditor.instances.last!
            e.window.setContentSize(NSSize(width: 1180, height: 760))
            e.edit.trimStart = 0.4
            e.edit.trimEnd = 2.6
            e.edit.crop = CGRect(x: 60, y: 30, width: 480, height: 300)
            let words = ["Open", "Settings,", "then", "click", "Deploy"].enumerated().map { CaptionWord(start: 0.5 + Double($0.offset) * 0.22, end: 0.72 + Double($0.offset) * 0.22, text: $0.element) }
            e.edit.captions = [Caption(start: 0.5, end: 1.6, text: "Open Settings, then click Deploy", words: words), Caption(start: 1.8, end: 2.4, text: "Done")]
            e.edit.captionLook = .outline
            e.edit.marks = [
                Mark(kind: .arrow, start: 0.6, end: 2.0, a: CGPoint(x: 140, y: 250), b: CGPoint(x: 250, y: 160), color: 6, level: 2),
                Mark(kind: .emoji, start: 0.6, end: 2.0, a: CGPoint(x: 420, y: 70), b: CGPoint(x: 490, y: 140), text: "🎉"),
                Mark(kind: .bubble, start: 0.8, end: 2.2, a: CGPoint(x: 260, y: 60), b: CGPoint(x: 410, y: 110), text: "Click here", color: 6, level: 1),
                Mark(kind: .step, start: 0.5, end: 2.5, a: CGPoint(x: 100, y: 80), b: CGPoint(x: 130, y: 110), color: 0, level: 2, step: 1),
                Mark(kind: .blur, start: 0.5, end: 2.5, a: CGPoint(x: 300, y: 200), b: CGPoint(x: 420, y: 250)),
                Mark(kind: .zoom, start: 1.8, end: 2.6, a: CGPoint(x: 200, y: 100), b: CGPoint(x: 360, y: 200)),
            ]
            e.seek(1.0)
        }
        try await Task.sleep(nanoseconds: 1_200_000_000)
        await MainActor.run {
            let e = VideoEditor.instances.last!
            e.selected = e.edit.marks[2].id
            let v = e.window.contentView!
            v.layoutSubtreeIfNeeded()
            let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)!
            v.cacheDisplay(in: v.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out).appendingPathComponent("ui-video-editor.png"))
            // A real window capture (includes the video layer), when this process may record the screen.
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
            p.arguments = ["-x", "-o", "-l\(e.window.windowNumber)", URL(fileURLWithPath: out).appendingPathComponent("ui-video-editor-live.png").path]
            try? p.run()
            p.waitUntilExit()
            e.dirty = false
            e.window.close()
        }
    }

    func testCaptionChunking() {
        let words: [(Double, Double, String)] = [(0, 0.3, "Open"), (0.35, 0.6, "the"), (0.65, 1.0, "settings"), (2.5, 2.8, "Then"), (2.85, 3.2, "click"), (3.25, 3.6, "save.")]
        let caps = VideoExport.chunk(words)
        XCTAssertEqual(caps.map(\.text), ["Open the settings", "Then click save."])
        XCTAssertEqual(caps[0].start, 0)
        XCTAssertLessThanOrEqual(caps[0].end, caps[1].start)
    }
}
