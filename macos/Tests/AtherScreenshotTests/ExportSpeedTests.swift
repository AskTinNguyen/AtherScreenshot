import AVFoundation
import AppKit
import ImageIO
import XCTest
@testable import AtherScreenshot

// The faster export: GIF chunks joined, MP4 pieces joined, frames without edits passed through.
final class ExportSpeedTests: XCTestCase {
    private func tmp(_ ext: String) -> URL { FileManager.default.temporaryDirectory.appendingPathComponent("ather-speed-\(UUID().uuidString).\(ext)") }

    private func gifFrames(_ url: URL) -> [(CGImage, Double)] {
        guard let s = CGImageSourceCreateWithURL(url as CFURL, nil) else { return [] }
        return (0..<CGImageSourceGetCount(s)).compactMap { i in
            guard let img = CGImageSourceCreateImageAtIndex(s, i, nil), let p = CGImageSourceCopyPropertiesAtIndex(s, i, nil) as? [CFString: Any] else { return nil }
            return (img, ((p[kCGImagePropertyGIFDictionary] as? [CFString: Any])?[kCGImagePropertyGIFDelayTime] as? Double) ?? 0)
        }
    }

    // A GIF written in parts at once has the same frames, timing and colors as one written in one go.
    func testGifInChunksMatchesOnePass() async throws {
        var e = TestMedia.edit([try await VideoSource.probe(try await TestMedia.colors())])
        e.captions = [Caption(start: 0.5, end: 2.5, text: "Chunks")]
        let one = tmp("gif"), three = tmp("gif")
        defer { try? FileManager.default.removeItem(at: one); try? FileManager.default.removeItem(at: three) }
        try await VideoExport.gif(e, to: one, fps: 10, chunks: 1)
        try await VideoExport.gif(e, to: three, fps: 10, chunks: 3)
        let a = gifFrames(one), b = gifFrames(three)
        XCTAssertEqual(a.count, 30)
        XCTAssertEqual(b.count, a.count)
        XCTAssertEqual(b.map(\.1).reduce(0, +), a.map(\.1).reduce(0, +), accuracy: 0.001)
        for (i, (x, y)) in zip(a, b).enumerated() where i % 5 == 0 {
            for p in [CGPoint(x: 10, y: 10), CGPoint(x: 160, y: 120)] {
                let c1 = x.0.color(atPixel: p)!.usingColorSpace(.sRGB)!, c2 = y.0.color(atPixel: p)!.usingColorSpace(.sRGB)!
                XCTAssertEqual(c1.redComponent, c2.redComponent, accuracy: 0.06, "frame \(i)")
                XCTAssertEqual(c1.greenComponent, c2.greenComponent, accuracy: 0.06, "frame \(i)")
                XCTAssertEqual(c1.blueComponent, c2.blueComponent, accuracy: 0.06, "frame \(i)")
            }
        }
    }

    func testJoinGIFsRefusesWhatItDoesntKnow() {
        XCTAssertNil(VideoExport.joinGIFs([Data([1, 2, 3])]))
        XCTAssertNil(VideoExport.joinGIFs([Data("GIF87a".utf8) + Data([0, 0, 0, 0, 0, 0, 0, 0x3B]), Data("nope".utf8)]))
    }

    // Presentation times of a file's samples, and how many.
    private func times(_ url: URL, _ type: AVMediaType) async throws -> [Double] {
        let asset = AVURLAsset(url: url)
        guard let t = try await asset.loadTracks(withMediaType: type).first else { return [] }
        let r = try AVAssetReader(asset: asset)
        let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
        r.add(o)
        r.startReading()
        var out: [Double] = []
        while let s = o.copyNextSampleBuffer() {
            if CMSampleBufferGetNumSamples(s) > 0 { out.append(CMSampleBufferGetPresentationTimeStamp(s).seconds) }   // not the markers
        }
        return out
    }

    // A clip whose picture moves every frame (a bar sweeping across) with a tone, so misplaced frames show.
    private func movingClip(seconds: Double = 6, w: Int = 640, h: Int = 360) async throws -> URL {
        try await TestMedia.clip(w: w, h: h, seconds: seconds, tone: true) { i, ctx in
            ctx.setFillColor(NSColor.darkGray.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
            ctx.setFillColor(NSColor.yellow.cgColor)
            ctx.fill(CGRect(x: (i * 7) % w, y: 0, width: 12, height: h))
        }
    }

    // An MP4 encoded in pieces at once and joined has the same frames at the same times, and the same sound, as
    // one encoded in one go; and it says it went that way.
    func testPiecesMatchOnePass() async throws {
        var e = TestMedia.edit([try await VideoSource.probe(try await movingClip())])
        e.trimStart = 0.5
        e.trimEnd = 5.7
        e.captions = [Caption(start: 1, end: 4, text: "Pieces")]
        let one = tmp("mp4"), three = tmp("mp4")
        defer { try? FileManager.default.removeItem(at: one); try? FileManager.default.removeItem(at: three) }
        let probe = VideoExport.Probe()
        VideoExport.probe = probe
        defer { VideoExport.probe = nil }
        try await VideoExport.mp4(e, to: one, encoders: 1)
        try await VideoExport.mp4(e, to: three, encoders: 3)
        XCTAssertEqual(probe.notes, ["writer 1 piece", "writer 3 pieces"])
        XCTAssertEqual(probe.frames, 2 * 156)
        let v1 = try await times(one, .video), v3 = try await times(three, .video)
        XCTAssertEqual(v1.count, 156)
        XCTAssertEqual(v3.count, v1.count)
        for (a, b) in zip(v1, v3) { XCTAssertEqual(a, b, accuracy: 1e-4) }
        let a1 = try await times(one, .audio), a3 = try await times(three, .audio)
        XCTAssertEqual(a3.count, a1.count)
        XCTAssertEqual(a3.first ?? -1, a1.first ?? 1, accuracy: 1e-4)   // the AAC priming trim survives
        // The pictures: the bar is where it should be in both, at every second.
        let g1 = AVAssetImageGenerator(asset: AVURLAsset(url: one)), g3 = AVAssetImageGenerator(asset: AVURLAsset(url: three))
        for g in [g1, g3] { g.requestedTimeToleranceBefore = .zero; g.requestedTimeToleranceAfter = .zero }
        for t in stride(from: 0.2, to: 5.0, by: 0.6) {
            let x = try await g1.image(at: CMTime(seconds: t, preferredTimescale: 600)).image
            let y = try await g3.image(at: CMTime(seconds: t, preferredTimescale: 600)).image
            for px in stride(from: 4, to: 640, by: 9) {
                let c1 = x.color(atPixel: CGPoint(x: px, y: 20))!.usingColorSpace(.sRGB)!, c2 = y.color(atPixel: CGPoint(x: px, y: 20))!.usingColorSpace(.sRGB)!
                XCTAssertEqual(c1.brightnessComponent, c2.brightnessComponent, accuracy: 0.12, "t \(t) x \(px)")
            }
        }
    }

    // On this Mac (it has the encoders), a video of a few seconds goes in pieces by default, not one pass.
    func testLongerExportUsesPieces() async throws {
        let e = TestMedia.edit([try await VideoSource.probe(try await movingClip(seconds: 6))])
        let out = tmp("mp4")
        defer { try? FileManager.default.removeItem(at: out) }
        let probe = VideoExport.Probe()
        VideoExport.probe = probe
        defer { VideoExport.probe = nil }
        try await VideoExport.mp4(e, to: out)
        XCTAssertEqual(probe.notes, ["writer 2 pieces"])
        let frames = try await times(out, .video).count
        XCTAssertEqual(frames, 180)
        XCTAssertEqual(probe.frames, 180)
    }

    // Cancelling a save stops it and leaves no pieces behind.
    func testCancelLeavesNoPieces() async throws {
        let e = TestMedia.edit([try await VideoSource.probe(try await movingClip(seconds: 12, w: 1920, h: 1080))])
        let out = tmp("mp4")
        defer { try? FileManager.default.removeItem(at: out) }
        func pieceDirs() -> Set<String> {
            Set(((try? FileManager.default.contentsOfDirectory(atPath: FileManager.default.temporaryDirectory.path)) ?? []).filter { $0.hasPrefix("ather-pieces-") })
        }
        let before = pieceDirs()
        let probe = VideoExport.Probe()
        VideoExport.probe = probe
        defer { VideoExport.probe = nil }
        let task = Task { try await VideoExport.mp4(e, to: out, encoders: 2) }
        while probe.frames < 20 { try await Task.sleep(nanoseconds: 5_000_000) }
        task.cancel()
        do {
            try await task.value
            XCTFail("finished despite the cancel")
        } catch {}
        XCTAssertLessThan(probe.frames, 360, "stopped early")
        XCTAssertEqual(pieceDirs(), before)
    }
}
