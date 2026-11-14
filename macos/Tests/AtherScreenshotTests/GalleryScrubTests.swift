import AVFoundation
import AppKit
import XCTest
@testable import AtherScreenshot

// Scrubbing a video tile in the gallery, and video thumbnails of odd-sized recordings.
final class GalleryScrubTests: XCTestCase {
    func testOrderIsCoarseToFineAndCoversEveryFrame() {
        XCTAssertEqual(Scrub.order.count, Scrub.frames)
        XCTAssertEqual(Set(Scrub.order), Set(0..<Scrub.frames))
        XCTAssertEqual(Array(Scrub.order.prefix(5)), [32, 16, 8, 24, 40])
        // After the first 6 frames, no spot is more than 8 frames from a decoded one.
        let early = Set(Scrub.order.prefix(6))
        for k in 0..<Scrub.frames { XCTAssertLessThanOrEqual(early.map { abs($0 - k) }.min()!, 8, "frame \(k)") }
    }

    func testIndexTimeBadgeAndZone() {
        XCTAssertEqual(Scrub.index(0), 0)
        XCTAssertEqual(Scrub.index(0.5), 24)
        XCTAssertEqual(Scrub.index(1), 47)
        XCTAssertEqual(Scrub.index(-0.2), 0)
        XCTAssertEqual(Scrub.time(0, duration: 48), 0.5, accuracy: 1e-9)
        XCTAssertEqual(Scrub.time(47, duration: 48), 47.5, accuracy: 1e-9)
        XCTAssertEqual(Scrub.badge(12.5 / 34, length: 34), "0:12 / 0:34")
        XCTAssertEqual(Scrub.badge(0, length: 125), "0:00 / 2:05")
        XCTAssertEqual(Scrub.badge(1, length: 125), "2:05 / 2:05")
        XCTAssertTrue(Scrub.inZone(CGPoint(x: 50, y: 110), width: 200, height: 120))
        XCTAssertFalse(Scrub.inZone(CGPoint(x: 50, y: 100), width: 200, height: 120))  // above the bottom 18 pt
        XCTAssertFalse(Scrub.inZone(CGPoint(x: 30, y: 110), width: 50, height: 120))   // too narrow for a timeline
    }

    func testNearestDecodedFrame() {
        var pics = [Int?](repeating: nil, count: Scrub.frames)
        XCTAssertNil(Scrub.nearest(pics, 10))
        pics[32] = 32
        XCTAssertEqual(Scrub.nearest(pics, 0), 32)
        pics[16] = 16
        XCTAssertEqual(Scrub.nearest(pics, 20), 16)
        XCTAssertEqual(Scrub.nearest(pics, 30), 32)
        XCTAssertEqual(Scrub.nearest(pics, 16), 16)
    }

    // A clip whose colour changes every second: the frame under the mouse comes first and is the right second's; then
    // every frame comes, once, at the tile's thumbnail size; leaving brings the poster back; another video starts over.
    @MainActor func testScrubberDecodesTheFramesOfTheVideo() async throws {
        let u = try await TestMedia.colors(w: 640, h: 360, seconds: 3)
        defer { try? FileManager.default.removeItem(at: u) }
        let s = Scrubber()
        s.scrub(u, at: 0.9, side: 190)   // the third second: blue
        XCTAssertEqual(s.url, u)
        try await until { s.picture != nil }
        XCTAssertNotNil(s.pics[Scrub.index(0.9)], "the frame under the mouse first")
        let (r, g, b) = TestMedia.rgb(s.picture!, s.picture!.width / 2, s.picture!.height / 2)
        XCTAssertTrue(b > 0.8 && r < 0.2 && g < 0.2, "blue at 90%: \(r) \(g) \(b)")
        XCTAssertEqual(s.duration, 3, accuracy: 0.05)
        try await until { s.decoded == Scrub.frames }
        for (k, p) in s.pics.enumerated() {
            let p = try XCTUnwrap(p)
            XCTAssertLessThanOrEqual(max(p.width, p.height), 256)
            let (r, g, b) = TestMedia.rgb(p, p.width / 2, p.height / 2)
            let second = Int(Scrub.time(k, duration: 3))
            let want = [(r, "red"), (g, "green"), (b, "blue")][second]
            XCTAssertGreaterThan(want.0, 0.8, "frame \(k) (second \(second)) should be \(want.1)")
        }
        s.scrub(u, at: 0.1, side: 190)
        let (r2, _, _) = TestMedia.rgb(s.picture!, 10, 10)
        XCTAssertGreaterThan(r2, 0.8, "red at 10%, from the cache")
        s.end(URL(fileURLWithPath: "/elsewhere.mp4"))   // another tile's leave doesn't end this one
        XCTAssertEqual(s.url, u)
        s.end(u)
        XCTAssertNil(s.url)
        XCTAssertNil(s.picture)
        XCTAssertEqual(s.decoded, Scrub.frames, "its frames stay for coming back")

        let v = try await TestMedia.colors(w: 320, h: 240, seconds: 2, colors: [.green, .green])
        defer { try? FileManager.default.removeItem(at: v) }
        s.scrub(v, at: 0.5, side: 190)
        XCTAssertNil(s.picture, "another video's frames start over")
        try await until { s.picture != nil }
        XCTAssertGreaterThan(TestMedia.rgb(s.picture!, 5, 5).1, 0.8)
    }

    // On Windows, the thumbnails of recordings whose size isn't whole 16 × 16 blocks came out sheared into slanted
    // streaks (the decoder pads the frame; reading rows at width × 4 drifted). The Mac's gallery thumbnails
    // (QuickLook), index posters and scrub frames (AVAssetImageGenerator) of such clips must come out straight:
    // a red left half and a blue right half keep their edge in the same column from top to bottom.
    // The H.264 encoder evens odd sizes out (1281 × 721 is stored 1280 × 720), so the odd sizes come as HEVC and
    // ProRes 4444 too, which keep them; 1636 × 854 and 868 × 674 are the Windows recordings' (even, not whole blocks).
    @MainActor func testOddSizedVideoThumbnailsDoNotShear() async throws {
        let kinds: [(AVVideoCodecType, AVFileType, String)] = [(.h264, .mp4, "mp4"), (.hevc, .mp4, "mp4"), (.proRes4444, .mov, "mov")]
        var odd = 0
        let sizes = [(1281, 721), (853, 479), (1636, 854), (868, 674)]
        for (w, h, codec, type, ext) in sizes.flatMap({ s in kinds.map { (s.0, s.1, $0.0, $0.1, $0.2) } }) {
            let u = try await halves(w: w, h: h, codec: codec, type: type, ext: ext)
            defer { try? FileManager.default.removeItem(at: u) }
            let asset = AVURLAsset(url: u)
            let size = try await asset.loadTracks(withMediaType: .video).first!.load(.naturalSize)
            print("shear check: \(w)×\(h) as \(codec.rawValue) is stored \(Int(size.width))×\(Int(size.height))")
            if Int(size.width) % 2 == 1 || Int(size.height) % 2 == 1 { odd += 1 }
            let made = await Indexer.poster(asset, duration: 1)
            let poster = try XCTUnwrap(made, "\(w)×\(h) poster")
            straight(poster, "\(w)×\(h) index poster", aspect: Double(w) / Double(h))

            let s = Scrubber()
            s.scrub(u, at: 0.5, side: 190)
            try await until { s.picture != nil }
            straight(s.picture!, "\(w)×\(h) scrub frame", aspect: Double(w) / Double(h))

            var thumb: NSImage?, came = false
            ThumbCache.shared.get(u, side: 190) { thumb = $0; came = true }
            try await until(timeout: 20) { came }
            if let t = thumb?.cgImage(forProposedRect: nil, context: nil, hints: nil) {
                straight(t, "\(w)×\(h) gallery thumbnail", aspect: nil)
            } else {
                print("note: QuickLook made no thumbnail for \(w)×\(h) in the test process")
            }
        }
        XCTAssertGreaterThan(odd, 0, "some clip really has an odd size")
    }

    // A one-second clip, red on its left half and blue on its right.
    private func halves(w: Int, h: Int, codec: AVVideoCodecType, type: AVFileType, ext: String) async throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("ather-odd-\(UUID().uuidString).\(ext)")
        let writer = try AVAssetWriter(outputURL: url, fileType: type)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: codec, AVVideoWidthKey: w, AVVideoHeightKey: h])
        let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: w, kCVPixelBufferHeightKey as String: h])
        writer.add(input)
        writer.startWriting()
        writer.startSession(atSourceTime: .zero)
        for i in 0..<10 {
            while !input.isReadyForMoreMediaData { try await Task.sleep(nanoseconds: 2_000_000) }
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb)
            CVPixelBufferLockBaseAddress(pb!, [])
            let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb!), width: w, height: h, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pb!),
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
            ctx.setFillColor(NSColor.red.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w / 2, height: h))
            ctx.setFillColor(NSColor.blue.cgColor)
            ctx.fill(CGRect(x: w / 2, y: 0, width: w - w / 2, height: h))
            CVPixelBufferUnlockBaseAddress(pb!, [])
            ad.append(pb!, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: 10))
        }
        input.markAsFinished()
        await writer.finishWriting()
        if writer.status != .completed { throw writer.error ?? VideoSource.Failure.noVideo }
        return url
    }

    // The red-to-blue edge is in the middle column on rows near the top, the middle and the bottom.
    private func straight(_ img: CGImage, _ what: String, aspect: Double?) {
        if let aspect { XCTAssertEqual(Double(img.width) / Double(img.height), aspect, accuracy: 0.03, what) }
        var edges: [Int] = []
        for fy in [0.08, 0.5, 0.92] {
            let y = Int(Double(img.height) * fy)
            let edge = (0..<img.width).first { x in let (r, _, b) = TestMedia.rgb(img, x, y); return b > r } ?? -1
            edges.append(edge)
        }
        let mid = img.width / 2, slack = max(2, img.width / 50)
        print("shear check: \(what) \(img.width)×\(img.height), red/blue edge at x = \(edges) (middle \(mid))")
        for e in edges { XCTAssertLessThanOrEqual(abs(e - mid), slack, "\(what) (\(img.width)×\(img.height)): edges at \(edges), not \(mid)") }
        XCTAssertLessThanOrEqual(edges.max()! - edges.min()!, 2, "\(what): the edge drifts down the picture \(edges)")
    }

    private func until(timeout: Double = 10, _ ok: () -> Bool) async throws {
        let end = Date().addingTimeInterval(timeout)
        while !ok() {
            if Date() > end { XCTFail("timed out"); throw XCTSkip("timed out") }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
    }
}
