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
}
