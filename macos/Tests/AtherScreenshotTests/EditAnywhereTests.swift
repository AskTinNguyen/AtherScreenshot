import AVFoundation
import AppKit
import UniformTypeIdentifiers
import XCTest
@testable import AtherScreenshot

final class EditAnywhereTests: XCTestCase {
    // A landscape 640 × 360 frame stored with a 90° turn, like a phone video: red top-left quarter, blue elsewhere.
    private func phoneClip() async throws -> URL {
        try await TestMedia.clip(w: 640, h: 360, seconds: 1, rotation: 90) { _, ctx in
            ctx.setFillColor(NSColor.blue.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: 640, height: 360))
            ctx.setFillColor(NSColor.red.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: 320, height: 180))
        }
    }

    private func isRed(_ c: (CGFloat, CGFloat, CGFloat)) -> Bool { c.0 > 0.6 && c.2 < 0.4 }
    private func isBlue(_ c: (CGFloat, CGFloat, CGFloat)) -> Bool { c.2 > 0.6 && c.0 < 0.4 }

    // Port of video_rotated_phone_clip_reads_upright_like_the_preview: size, preview, export and frame grabs
    // all show it the way AVPlayer does (the track's preferredTransform applied).
    func testRotatedPhoneClipReadsUprightEverywhere() async throws {
        let url = try await phoneClip()
        let clip = try await VideoSource.probe(url)
        XCTAssertEqual(clip.rotation, 90)
        XCTAssertEqual([clip.w, clip.h], [360, 640], "upright size")
        let r = await Indexer.index(url, ocr: false)
        XCTAssertEqual([r.w, r.h], [360, 640], "the gallery stores the upright size")

        // The reference: how the system shows it. A 90° turn puts the stored top-left on the top right.
        let ref = try await TestMedia.frame(AVURLAsset(url: url), at: 0.5)
        XCTAssertEqual([ref.width, ref.height], [360, 640])
        let probes = [(300, 100), (60, 100), (60, 540), (300, 540)]
        XCTAssertTrue(isRed(TestMedia.rgb(ref, 300, 100)) && isBlue(TestMedia.rgb(ref, 60, 100)))

        // The preview: the sequence through the compositor.
        let e = TestMedia.edit([clip])
        let seq = try await VideoSequence.build(e.clips, frame: e.frame, box: RendererBox(FrameRenderer(edit: e, full: e.frame, preview: true)))
        let pv = try await TestMedia.frame(seq.composition, at: 0.5, video: seq.video)
        XCTAssertEqual([pv.width, pv.height], [360, 640])
        for (x, y) in probes { XCTAssertEqual(isRed(TestMedia.rgb(pv, x, y)), isRed(TestMedia.rgb(ref, x, y)), "preview at \(x),\(y)") }

        // The export, and what's under markup placed on the upright frame.
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-out-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(e, to: out)
        let ex = try await TestMedia.frame(AVURLAsset(url: out), at: 0.5)
        XCTAssertEqual([ex.width, ex.height], [360, 640], "export comes out upright")
        for (x, y) in probes { XCTAssertEqual(isRed(TestMedia.rgb(ex, x, y)), isRed(TestMedia.rgb(ref, x, y)), "export at \(x),\(y)") }
    }

    // Only a video whose frames decode counts; its sound only if the audio decodes.
    func testProbeAcceptsOnlyDecodableVideoAndSound() async throws {
        let junk = FileManager.default.temporaryDirectory.appendingPathComponent("ather-junk-\(UUID().uuidString).mp4")
        try Data((0..<4096).map { UInt8(truncatingIfNeeded: $0 &* 31) }).write(to: junk)
        do {
            _ = try await VideoSource.probe(junk)
            XCTFail("junk must not open")
        } catch {}
        let silent = try await VideoSource.probe(try await TestMedia.colors(seconds: 1))
        XCTAssertFalse(silent.hasAudio)
        let loud = try await VideoSource.probe(try await TestMedia.colors(seconds: 1, tone: true))
        XCTAssertTrue(loud.hasAudio)
        XCTAssertEqual(loud.length, 1, accuracy: 0.05)
    }

    // One list of types: the open panel, the gallery's import and the Finder document types agree.
    func testOneListOfPictureAndVideoTypes() throws {
        for e in ["mp4", "mov", "m4v"] { XCTAssertTrue(MediaFiles.isVideo(URL(fileURLWithPath: "/x/a.\(e.uppercased())")), e) }
        for e in ["png", "jpg", "heic", "gif"] { XCTAssertFalse(MediaFiles.isVideo(URL(fileURLWithPath: "/x/a.\(e)")), e) }
        XCTAssertEqual(MediaType.of(URL(fileURLWithPath: "/x/a.m4v")), .video)
        XCTAssertEqual(MediaType.of(URL(fileURLWithPath: "/x/a.gif")), .gif)
        for e in MediaFiles.pictureExtensions {
            XCTAssertTrue(UTType(filenameExtension: e)?.conforms(to: .image) ?? false, "\(e) is covered by public.image")
        }
        for e in MediaFiles.videoExtensions {
            XCTAssertTrue(UTType(filenameExtension: e)?.conforms(to: .movie) ?? false, "\(e) is covered by public.movie")
        }
        XCTAssertEqual(MediaFiles.contentTypes(pictures: true, videos: true).count, MediaFiles.all.count)
        // The bundle declares both, as an alternate handler (in Open With, never the default).
        let plist = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Resources/Info.plist")
        let d = try XCTUnwrap(NSDictionary(contentsOf: plist) as? [String: Any])
        let types = try XCTUnwrap(d["CFBundleDocumentTypes"] as? [[String: Any]])
        let declared = Set(types.flatMap { $0["LSItemContentTypes"] as? [String] ?? [] })
        XCTAssertEqual(declared, ["public.image", "public.movie"])
        XCTAssertTrue(types.allSatisfy { $0["LSHandlerRank"] as? String == "Alternate" })
    }

    // Videos indexed before 0.0.2 get indexed again (their size may be sideways); pictures don't.
    func testOnlyVideosAreReindexed() {
        XCTAssertEqual(Library.indexVersion(for: URL(fileURLWithPath: "/x/a.mp4")), 2)
        XCTAssertEqual(Library.indexVersion(for: URL(fileURLWithPath: "/x/a.png")), 1)
        XCTAssertEqual(Library.indexVersion(for: URL(fileURLWithPath: "/x/a.gif")), 1)
    }

    // The editor's own player shows the phone clip upright too (the preview reads the same composition).
    func testEditorPreviewPlayerShowsItUpright() async throws {
        let url = try await phoneClip()
        await MainActor.run { _ = NSApplication.shared; VideoEditor.open(url) }
        try await Task.sleep(nanoseconds: 1_200_000_000)
        let e = try await MainActor.run { try XCTUnwrap(VideoEditor.instances.last) }
        let output = AVPlayerItemVideoOutput(pixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
        let item = try await MainActor.run { try XCTUnwrap(e.player.currentItem) }
        await MainActor.run { item.add(output) }
        await e.player.seek(to: CMTime(seconds: 0.5, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
        var px: CVPixelBuffer?
        for _ in 0..<40 where px == nil {
            px = output.copyPixelBuffer(forItemTime: CMTime(seconds: 0.5, preferredTimescale: 600), itemTimeForDisplay: nil)
            if px == nil { try await Task.sleep(nanoseconds: 50_000_000) }
        }
        let buf = try XCTUnwrap(px, "the preview produced a frame")
        let img = try XCTUnwrap(sharedCIContext.createCGImage(CIImage(cvPixelBuffer: buf), from: CGRect(x: 0, y: 0, width: CVPixelBufferGetWidth(buf), height: CVPixelBufferGetHeight(buf))))
        XCTAssertEqual([img.width, img.height], [360, 640])
        XCTAssertTrue(isRed(TestMedia.rgb(img, 300, 100)) && isBlue(TestMedia.rgb(img, 60, 100)), "stored top-left shows top right")
        await MainActor.run {
            XCTAssertEqual(e.videoSize, CGSize(width: 360, height: 640))
            e.dirty = false
            e.window.close()
        }
    }
}
