import AVFoundation
import ImageIO
import UniformTypeIdentifiers
import XCTest
@testable import AtherScreenshot

// "Compress to under N MB" (Compress.swift): the same checks as the Windows self-tests in src/compress.cpp.
final class CompressTests: XCTestCase {
    func testPlanFollowsTheHandoffMath() {
        // 30 MB over 80.2 s with sound, from 2560 wide: ~2.9 Mbps in all, 96 kbps of it sound, 1920 wide.
        var p = Compress.planVideo(cap: Compress.bytes(mb: 30), seconds: 80.2, w: 2560, h: 1440, audio: true)
        XCTAssertTrue(p.fits)
        XCTAssertEqual(p.videoBps + p.audioBps, 30 * 1024 * 1024 * 0.9 * 8 / 80.2, accuracy: 1)
        XCTAssertEqual(p.audioBps, 96_000)
        XCTAssertEqual([p.w, p.h], [1920, 1080])
        // No sound: all of it video. Smaller than 1920: never scaled up.
        p = Compress.planVideo(cap: Compress.bytes(mb: 30), seconds: 80.2, w: 1280, h: 720, audio: false)
        XCTAssertEqual(p.audioBps, 0)
        XCTAssertEqual([p.w, p.h], [1280, 720])
        // Under 500 kbps: 1280 wide; under 250 kbps: 854; under 100 kbps it doesn't fit.
        p = Compress.planVideo(cap: Compress.bytes(mb: 8), seconds: 120, w: 2560, h: 1440, audio: true)
        XCTAssertTrue(p.videoBps < 500_000 && p.videoBps >= 250_000)
        XCTAssertEqual([p.w, p.h], [1280, 720])
        p = Compress.planVideo(cap: Compress.bytes(mb: 8), seconds: 240, w: 2560, h: 1440, audio: true)
        XCTAssertLessThan(p.videoBps, 250_000)
        XCTAssertEqual([p.w, p.h], [854, 480])
        XCTAssertFalse(Compress.planVideo(cap: Compress.bytes(mb: 1), seconds: 300, w: 1920, h: 1080, audio: true).fits)
        // Portrait phone video: the long side counts, and the sides stay even.
        p = Compress.planVideo(cap: Compress.bytes(mb: 25), seconds: 30, w: 2160, h: 3840, audio: true)
        XCTAssertEqual([p.w, p.h], [1080, 1920])
        p = Compress.planVideo(cap: Compress.bytes(mb: 25), seconds: 30, w: 1001, h: 777, audio: true)
        XCTAssertTrue(p.w % 2 == 0 && p.h % 2 == 0)
        // A retry lowers the rate by cap / actual × 0.95.
        XCTAssertEqual(Compress.retryScale(cap: 30, actual: 33), 30.0 / 33 * 0.95, accuracy: 1e-12)
        let first = Compress.planVideo(cap: Compress.bytes(mb: 30), seconds: 80.2, w: 2560, h: 1440, audio: true)
        let again = Compress.planVideo(cap: Compress.bytes(mb: 30), seconds: 80.2, w: 2560, h: 1440, audio: true, videoScale: Compress.retryScale(cap: 30, actual: 33))
        XCTAssertEqual(again.videoBps, first.videoBps * 30 / 33 * 0.95, accuracy: 1)
    }

    func testSizesAndNames() throws {
        XCTAssertEqual(Compress.parseMegabytes("12"), 12)
        XCTAssertEqual(Compress.parseMegabytes("2,5 MB"), 2.5)
        XCTAssertEqual(Compress.parseMegabytes("8mb"), 8)
        XCTAssertEqual(Compress.parseMegabytes("ten"), 0)
        XCTAssertEqual(Compress.parseMegabytes("12 GB"), 0)
        XCTAssertEqual(Compress.parseMegabytes("0"), 0)
        XCTAssertEqual(Compress.label(8), "8 MB")
        XCTAssertEqual(Compress.label(2.5), "2.5 MB")
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let src = dir.appendingPathComponent("clip.mp4")
        XCTAssertEqual(Compress.outputURL(for: src, ext: "mp4").lastPathComponent, "clip_small.mp4")
        XCTAssertEqual(Compress.outputURL(for: src, ext: "mp4").lastPathComponent, "clip_small-2.mp4")  // the first is reserved meanwhile
        XCTAssertEqual(Compress.pictureSteps(w: 4000, h: 3000).first, Compress.PictureStep(quality: 0.85, scale: 1))
        XCTAssertEqual(Compress.pictureSteps(w: 400, h: 300).count, 4)  // never below 320 px on the long side
    }

    func testVideoLandsUnderTheCap() async throws {
        let src = try await Self.noise(w: 640, h: 360, seconds: 4, tone: true)
        let before = Compress.fileBytes(src)
        let cap = min(400 * 1024, before * 0.6)
        var calls = 0
        var r = try await Compress.video(src, cap: cap) { _, _ in calls += 1 }
        XCTAssertFalse(r.skipped)
        XCTAssertGreaterThan(calls, 0)
        try Compress.finish(&r)
        let out = try XCTUnwrap(r.out)
        XCTAssertEqual(out.lastPathComponent, src.deletingPathExtension().lastPathComponent + "_small.mp4")
        let after = Compress.fileBytes(out)
        XCTAssertTrue(after > 0 && after <= cap, "\(after) bytes for a cap of \(cap), \(r.attempts) tries")
        XCTAssertEqual(Compress.fileBytes(src), before)  // the original stays as it was
        let asset = AVURLAsset(url: out)
        let size = try await asset.loadTracks(withMediaType: .video).first?.load(.naturalSize)
        let sounds = try await asset.loadTracks(withMediaType: .audio).count
        let seconds = try await asset.load(.duration).seconds
        XCTAssertEqual(size, CGSize(width: 640, height: 360))
        XCTAssertEqual(sounds, 1)
        XCTAssertEqual(seconds, 4, accuracy: 0.2)
        // Already small enough: nothing made.
        let again = try await Compress.video(out, cap: cap)
        XCTAssertTrue(again.skipped && again.tmp == nil)
        // Too little room for the length: refused.
        do { _ = try await Compress.video(src, cap: 30 * 1024); XCTFail("should not fit") } catch {}
        // 1920 × 1080 at a cap that leaves it under 500 kbps of video: 1280 wide.
        let hd = try await Self.noise(w: 1920, h: 1080, seconds: 2, tone: false)
        var s = try await Compress.video(hd, cap: 100 * 1024)
        try Compress.finish(&s)
        XCTAssertEqual([s.w, s.h], [1280, 720])
        let hdSize = try await AVURLAsset(url: try XCTUnwrap(s.out)).loadTracks(withMediaType: .video).first?.load(.naturalSize)
        XCTAssertEqual(hdSize, CGSize(width: 1280, height: 720))
    }

    func testRotatedPhoneVideoComesOutUpright() async throws {
        let src = try await TestMedia.clip(w: 640, h: 360, seconds: 2, rotation: 90) { i, ctx in Self.paintNoise(ctx, seed: UInt32(i)) }
        var r = try await Compress.video(src, cap: min(200 * 1024, Compress.fileBytes(src) * 0.6))
        try Compress.finish(&r)
        XCTAssertEqual([r.w, r.h], [360, 640])
        let size = try await AVURLAsset(url: try XCTUnwrap(r.out)).loadTracks(withMediaType: .video).first?.load(.naturalSize)
        XCTAssertEqual(size, CGSize(width: 360, height: 640))
    }

    func testPictureMakesASmallerJpeg() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let src = dir.appendingPathComponent("shot.png")
        // Noise compresses badly as PNG, so the JPEG has to work for it.
        let ctx = CGContext(data: nil, width: 1600, height: 1000, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
        Self.paintNoise(ctx, seed: 7)
        let dest = try XCTUnwrap(CGImageDestinationCreateWithURL(src as CFURL, UTType.png.identifier as CFString, 1, nil))
        CGImageDestinationAddImage(dest, ctx.makeImage()!, nil)
        XCTAssertTrue(CGImageDestinationFinalize(dest))
        let before = Compress.fileBytes(src), cap = 400.0 * 1024
        XCTAssertGreaterThan(before, cap)
        var r = try Compress.picture(src, cap: cap)
        try Compress.finish(&r)
        let out = try XCTUnwrap(r.out)
        XCTAssertEqual(out.lastPathComponent, "shot_small.jpg")
        XCTAssertLessThanOrEqual(Compress.fileBytes(out), cap)
        XCTAssertEqual(Compress.fileBytes(src), before)
        let img = try XCTUnwrap(CGImageSourceCreateWithURL(out as CFURL, nil).flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) })
        XCTAssertEqual([img.width, img.height], [r.w, r.h])
        XCTAssertLessThanOrEqual(img.width, 1600)
    }

    // Noise, which no encoder can shrink much, so the rate has to hold.
    private static func noise(w: Int, h: Int, seconds: Double, tone: Bool) async throws -> URL {
        try await TestMedia.clip(w: w, h: h, seconds: seconds, tone: tone) { i, ctx in paintNoise(ctx, seed: UInt32(i)) }
    }

    private static func paintNoise(_ ctx: CGContext, seed: UInt32) {
        guard let data = ctx.data else { return }
        let px = data.assumingMemoryBound(to: UInt32.self)
        var x = seed &* 2_654_435_761 &+ 99
        for i in 0..<(ctx.bytesPerRow / 4 * ctx.height) {
            x = x &* 1_664_525 &+ 1_013_904_223
            px[i] = 0xFF00_0000 | (x >> 8)
        }
    }
}
