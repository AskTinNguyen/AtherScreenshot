import AVFoundation
import AppKit
import Carbon.HIToolbox
import XCTest
@testable import AtherScreenshot

// ⇧-drag marks a range on the timeline; ⌘C copies it (or the selected clip) as an H.264 + AAC clip, or the frame on
// screen as a picture; ⌘⇧C always copies the frame. Every test copies to a pasteboard of its own, never the general one.
final class VideoCopyTests: XCTestCase {
    private var board: NSPasteboard!
    private var caps: URL!
    private var oldFolder = ""

    override func setUp() {
        board = NSPasteboard(name: NSPasteboard.Name("ather-test-\(UUID().uuidString)"))
        caps = FileManager.default.temporaryDirectory.appendingPathComponent("ather-caps-\(UUID().uuidString)")
        try? FileManager.default.createDirectory(at: caps, withIntermediateDirectories: true)
        oldFolder = Settings.shared.string("SaveFolder")
        Settings.shared.set("SaveFolder", caps.path)
    }

    override func tearDown() {
        board.releaseGlobally()
        Settings.shared.set("SaveFolder", oldFolder)
    }

    // Frame i shows its number in binary: eight stripes 40 px wide across the top, white for a 1 and black for a 0, which
    // survive H.264 and any color handling. Below them, blue or gray: which clip it is.
    private static func numbered(seconds: Double, blue: Bool = false) async throws -> URL {
        try await TestMedia.clip(w: 320, h: 180, fps: 30, seconds: seconds, tone: true) { i, ctx in
            ctx.setFillColor(blue ? CGColor(srgbRed: 0, green: 0, blue: 1, alpha: 1) : CGColor(gray: 0.4, alpha: 1))
            ctx.fill(CGRect(x: 0, y: 0, width: 320, height: 180))
            for k in 0..<8 {
                ctx.setFillColor(CGColor(gray: (i >> k) & 1 == 1 ? 1 : 0, alpha: 1))
                ctx.fill(CGRect(x: 40 * k, y: 0, width: 40, height: 80))
            }
        }
    }

    // The number on a frame of such a clip, and whether it's the blue one. `at`: where the source frame's top-left is in
    // the picture (a crop moves it).
    private static func number(_ img: CGImage, at o: CGPoint = .zero) -> (Int, Bool) {
        var n = 0
        for k in 0..<8 {
            let (r, g, b) = TestMedia.rgb(img, Int(o.x) + 40 * k + 20, Int(o.y) + 40)
            if (r + g + b) / 3 > 0.5 { n |= 1 << k }
        }
        let (r, _, b) = TestMedia.rgb(img, Int(o.x) + 160, Int(o.y) + 130)
        return (n, b > 0.5 && r < 0.3)
    }

    // Every frame of a video in order, by number.
    private static func frames(_ url: URL) async throws -> [(Int, Bool)] {
        let asset = AVURLAsset(url: url)
        let tracks = try await asset.loadTracks(withMediaType: .video)
        let track = try XCTUnwrap(tracks.first)
        let r = try AVAssetReader(asset: asset)
        let o = AVAssetReaderTrackOutput(track: track, outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
        r.add(o)
        r.startReading()
        var out: [(Int, Bool)] = []
        while let s = o.copyNextSampleBuffer() {
            if let pb = CMSampleBufferGetImageBuffer(s), let img = VideoExport.cgImage(pb) { out.append(number(img)) }
        }
        return out
    }

    private func until(_ what: String, timeout: Double = 10, _ ok: @MainActor () -> Bool) async throws {
        let end = Date().addingTimeInterval(timeout)
        while Date() < end {
            if await MainActor.run(body: ok) { return }
            try await Task.sleep(nanoseconds: 30_000_000)
        }
        XCTFail("timed out: \(what)")
    }

    private func open(_ url: URL) async throws -> VideoEditor {
        await MainActor.run { _ = NSApplication.shared; VideoEditor.open(url) }
        try await until("editor loads") { VideoEditor.instances.last?.url == url && VideoEditor.instances.last?.edit.clips.count == 1 }
        let e = await MainActor.run { VideoEditor.instances.last! }
        await MainActor.run { e.pasteboard = self.board }
        return e
    }

    private func close(_ e: VideoEditor) async {
        await MainActor.run {
            e.dirty = false
            e.window.close()
        }
    }

    private func seek(_ e: VideoEditor, _ t: Double) async throws {
        await MainActor.run { e.seek(t) }
        try await until("at \(t) s") { abs(e.now - t) < 1e-3 }
    }

    @MainActor private func mouse(_ e: VideoEditor, _ type: NSEvent.EventType, _ p: CGPoint, shift: Bool = false) {
        let tl = e.timeline
        let ev = NSEvent.mouseEvent(with: type, location: tl.convert(p, to: nil), modifierFlags: shift ? [.shift] : [], timestamp: 0,
                                    windowNumber: e.window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
        switch type {
        case .leftMouseDown: tl.mouseDown(with: ev)
        case .leftMouseDragged: tl.mouseDragged(with: ev)
        default: tl.mouseUp(with: ev)
        }
    }

    @MainActor @discardableResult private func key(_ e: VideoEditor, _ code: Int, _ flags: NSEvent.ModifierFlags = []) -> Bool {
        let ev = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0, windowNumber: e.window.windowNumber,
                                  context: nil, characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: UInt16(code))!
        return e.key(ev)
    }

    // ⇧-drag marks a range on frame boundaries (a third of a frame off still lands on the frame's edge), inside the trim;
    // a grip drags without ⇧; ⇧I and ⇧O set its ends at the playhead; Esc clears it. None of it is an edit. ⌘C then saves
    // that part as an H.264 + AAC MP4 in the captures folder, as long as the range, its frames the range's, and puts the
    // file on the pasteboard.
    func testShiftDragMarksARangeAndCommandCCopiesItAsAClip() async throws {
        let general = NSPasteboard.general.changeCount
        let e = try await open(try await Self.numbered(seconds: 3))
        let before = await MainActor.run { e.edit }
        await MainActor.run {
            let tl = e.timeline, y: CGFloat = 26, px = tl.bounds.width / CGFloat(e.duration * 30)   // the thumbnails; pixels a frame
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(20 / 30) + px / 3, y: y), shift: true)
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(40 / 30), y: y), shift: true)
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(50 / 30) - px / 3, y: y), shift: true)
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(50 / 30) - px / 3, y: y), shift: true)
            XCTAssertEqual(e.marked?.lowerBound ?? -1, 20 / 30, accuracy: 1e-9)
            XCTAssertEqual(e.marked?.upperBound ?? -1, 50 / 30, accuracy: 1e-9)
            XCTAssertEqual(e.markedLabel, "1.0 s · ⌘C copies")
            // A grip, without ⇧: the end to frame 60.
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(50 / 30) + 1, y: y))
            XCTAssertTrue(tl.isDragging)
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(60 / 30), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(60 / 30), y: y))
            XCTAssertEqual(e.marked?.upperBound ?? -1, 60 / 30, accuracy: 1e-9)
            // Inside the trim: a drag over everything stops at its ends.
            e.edit.trimStart = 10 / 30
            e.edit.trimEnd = 80 / 30
            mouse(e, .leftMouseDown, CGPoint(x: 1, y: y), shift: true)
            mouse(e, .leftMouseDragged, CGPoint(x: tl.bounds.width - 1, y: y), shift: true)
            mouse(e, .leftMouseUp, CGPoint(x: tl.bounds.width - 1, y: y), shift: true)
            XCTAssertEqual(e.marked, 10 / 30 ..< 80 / 30)
            e.edit.trimStart = before.trimStart
            e.edit.trimEnd = before.trimEnd
            // Esc clears it; a plain click outside a range does too.
            XCTAssertTrue(key(e, kVK_Escape))
            XCTAssertNil(e.marked)
            XCTAssertTrue(e.window.isVisible, "Esc only cleared the range")
            e.setMarked(20 / 30, 50 / 30)
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(70 / 30), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(70 / 30), y: y))
            XCTAssertNil(e.marked)
        }
        // ⇧I / ⇧O at the playhead; the trim stays.
        try await seek(e, 20 / 30)
        await MainActor.run { key(e, kVK_ANSI_I, [.shift]) }
        try await seek(e, 50 / 30)
        await MainActor.run {
            key(e, kVK_ANSI_O, [.shift])
            XCTAssertEqual(e.marked?.lowerBound ?? -1, 20 / 30, accuracy: 1e-9)
            XCTAssertEqual(e.marked?.upperBound ?? -1, 50 / 30, accuracy: 1e-9)
            XCTAssertEqual(e.edit, before, "a selection, not an edit")
            XCTAssertFalse(e.dirty)
            e.menuNeedsUpdate(e.copyMenu.menu!)
            XCTAssertEqual(e.copyMenu.item(at: 1)?.isEnabled, true, "Copy clip, with a range")
            XCTAssertTrue(key(e, kVK_ANSI_C, [.command]))
        }
        try await until("copied", timeout: 30) { e.lastCopiedClip != nil }
        let copied = await MainActor.run { e.lastCopiedClip }
        let out = try XCTUnwrap(copied)
        XCTAssertTrue(out.path.hasPrefix(caps.path), "in the captures folder")
        let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]
        XCTAssertEqual(urls?.map(\.standardizedFileURL), [out.standardizedFileURL], "the file, on the pasteboard")
        let asset = AVURLAsset(url: out)
        let videos = try await asset.loadTracks(withMediaType: .video), sounds = try await asset.loadTracks(withMediaType: .audio)
        let video = try XCTUnwrap(videos.first), audio = try XCTUnwrap(sounds.first)
        let vfs = try await video.load(.formatDescriptions), afs = try await audio.load(.formatDescriptions)
        let vf = try XCTUnwrap(vfs.first), af = try XCTUnwrap(afs.first)
        XCTAssertEqual(CMFormatDescriptionGetMediaSubType(vf), kCMVideoCodecType_H264)
        XCTAssertEqual(CMFormatDescriptionGetMediaSubType(af), kAudioFormatMPEG4AAC)
        let length = try await video.load(.timeRange).duration.seconds
        XCTAssertEqual(length, 1, accuracy: 1 / 30.0 + 1e-6, "as long as the range")
        let got = try await Self.frames(out)
        XCTAssertEqual(got.count, 30)
        XCTAssertTrue(got.first! == (20, false) && got.last! == (49, false), "frames \(got.first.map { "\($0)" } ?? "")…\(got.last.map { "\($0)" } ?? "")")
        // The same frames the export draws at the range's ends.
        let edit = await MainActor.run { e.edit }
        let a = Self.number(try await VideoExport.frame(edit, at: 20 / 30)), b = Self.number(try await VideoExport.frame(edit, at: 49 / 30))
        XCTAssertTrue(got.first! == a && got.last! == b)
        await MainActor.run { XCTAssertNotNil(e.marked, "still marked after copying") }
        XCTAssertEqual(NSPasteboard.general.changeCount, general, "the general pasteboard was never touched")
        await close(e)
    }

    // ⌘C with no range copies the clip selected in the clip lane, as edited: here the second clip, all of it.
    func testCommandCCopiesTheSelectedClip() async throws {
        let general = NSPasteboard.general.changeCount
        let e = try await open(try await Self.numbered(seconds: 2))
        let b = try await Self.numbered(seconds: 2, blue: true)
        await MainActor.run { e.addClips([b]) }
        try await until("b joins") { e.edit.clips.count == 2 && e.duration > 3.9 }
        await MainActor.run {
            e.selected = e.edit.clips[1].id
            XCTAssertNil(e.marked)
            XCTAssertTrue(e.canCopyClip)
            XCTAssertTrue(key(e, kVK_ANSI_C, [.command]))
        }
        try await until("copied", timeout: 30) { e.lastCopiedClip != nil }
        let copied = await MainActor.run { e.lastCopiedClip }
        let out = try XCTUnwrap(copied)
        let got = try await Self.frames(out)
        XCTAssertEqual(got.count, 60)
        XCTAssertTrue(got.allSatisfy { $0.1 }, "only the second clip's frames")
        XCTAssertTrue(got.first! == (0, true) && got.last! == (59, true))
        let urls = board.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]
        XCTAssertEqual(urls?.map(\.standardizedFileURL), [out.standardizedFileURL])
        XCTAssertEqual(NSPasteboard.general.changeCount, general)
        await close(e)
    }

    // With nothing picked ⌘C, and ⌘⇧C also with a range marked, put the frame on screen on the pasteboard (PNG and TIFF):
    // the frame renderer's picture for the export (here cropped, with a caption and a box) at the export's size. No file.
    func testCommandCCopiesTheFrameOnScreen() async throws {
        let general = NSPasteboard.general.changeCount
        let clip = try await Self.numbered(seconds: 3)
        let e = try await open(clip)
        await MainActor.run {
            e.edit.crop = CGRect(x: 0, y: 20, width: 320, height: 140)
            e.edit.captions = [Caption(start: 0.5, end: 2.5, text: "Copied frame")]
            e.edit.marks = [Mark(kind: .box, start: 0.5, end: 2.5, a: CGPoint(x: 100, y: 60), b: CGPoint(x: 220, y: 120), color: 0, level: 2)]
        }
        // The picture expected at `t`: the first frame an export starting there renders (what its encoder gets).
        func expected(_ t: Double) async throws -> CGImage {
            var one = await MainActor.run { e.edit }
            one.trimStart = t
            one.trimEnd = t + 0.1
            let p = try await VideoExport.prepare(one)
            let r = try AVAssetReader(asset: p.composition)
            let o = AVAssetReaderVideoCompositionOutput(videoTracks: p.composition.tracks(withMediaType: .video), videoSettings: nil)
            o.videoComposition = p.video
            r.add(o)
            r.startReading()
            defer { r.cancelReading() }
            let s = try XCTUnwrap(o.copyNextSampleBuffer())
            return try XCTUnwrap(CMSampleBufferGetImageBuffer(s).flatMap { VideoExport.cgImage($0) })
        }
        // The largest difference of any color channel between two pictures (0…255), or nil when their sizes differ.
        func difference(_ a: CGImage, _ b: CGImage) -> Int? {
            guard a.width == b.width, a.height == b.height else { return nil }
            func bytes(_ i: CGImage) -> [UInt8] {
                let ctx = makeContext(width: i.width, height: i.height, flipped: false)!
                ctx.draw(i, in: CGRect(x: 0, y: 0, width: i.width, height: i.height))
                return Array(UnsafeBufferPointer(start: ctx.data!.assumingMemoryBound(to: UInt8.self), count: ctx.bytesPerRow * i.height))
            }
            return zip(bytes(a), bytes(b)).reduce(0) { max($0, abs(Int($1.0) - Int($1.1))) }
        }
        func pasted() throws -> CGImage {
            let png = try XCTUnwrap(board.data(forType: .png))
            XCTAssertNotNil(board.data(forType: .tiff))
            return try XCTUnwrap(CGImageSourceCreateWithData(png as CFData, nil).flatMap { CGImageSourceCreateImageAtIndex($0, 0, nil) })
        }
        for (t, shift) in [(45.0 / 30, false), (60.0 / 30, true)] {
            try await seek(e, t)
            let previous = await MainActor.run { e.lastCopiedFrame }
            await MainActor.run {
                if shift { e.setMarked(10 / 30, 30 / 30) }   // ⌘⇧C: the frame, even with a range
                XCTAssertTrue(key(e, kVK_ANSI_C, shift ? [.command, .shift] : [.command]))
            }
            try await until("frame copied") { e.lastCopiedFrame != nil && e.lastCopiedFrame !== previous }
            let img = try pasted()
            XCTAssertEqual([img.width, img.height], [320, 140], "the export's size: the crop")
            let want = try await expected(t)
            let d = difference(img, want)
            print("frame at \(t) s: the copy differs from the renderer's frame by at most \(d.map(String.init) ?? "-") of 255")
            XCTAssertNotNil(d)
            XCTAssertEqual(d, 0)
            XCTAssertEqual(Self.number(img, at: CGPoint(x: 0, y: -20)).0, Int((t * 30).rounded()), "the frame on screen")
        }
        await MainActor.run {
            XCTAssertNil(e.lastCopiedClip, "no clip made")
            e.menuNeedsUpdate(e.copyMenu.menu!)
            XCTAssertEqual(e.copyMenu.item(at: 2)?.isEnabled, true, "Copy frame")
            XCTAssertTrue(key(e, kVK_Escape))
            e.menuNeedsUpdate(e.copyMenu.menu!)
            XCTAssertEqual(e.copyMenu.item(at: 1)?.isEnabled, false, "Copy clip: nothing to copy")
        }
        let files = FileManager.default.enumerator(atPath: caps.path)?.allObjects as? [String] ?? []
        XCTAssertFalse(files.contains { $0.hasSuffix(".mp4") || $0.hasSuffix(".png") }, "nothing saved")
        XCTAssertEqual(NSPasteboard.general.changeCount, general)
        await close(e)
    }

    // The editor with a marked range, for a look: ATHER_TEST_OUT=<dir> swift test --filter VideoCopyTests
    func testMarkedRangeSnapshot() async throws {
        guard let dir = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] else { throw XCTSkip("set ATHER_TEST_OUT") }
        let e = try await open(try await Self.numbered(seconds: 8))
        await MainActor.run {
            e.window.setContentSize(NSSize(width: 1180, height: 760))
            e.edit.captions = [Caption(start: 1, end: 3, text: "Boss fight starts")]
            e.setMarked(1.5, 1.5 + 6.2)
        }
        try await seek(e, 1.5)
        try await Task.sleep(nanoseconds: 1_200_000_000)   // thumbnails
        await MainActor.run {
            let v = e.window.contentView!
            v.layoutSubtreeIfNeeded()
            let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)!
            v.cacheDisplay(in: v.bounds, to: rep)
            // On the window's own background, which the view's drawing leaves out (the video layer stays black).
            let img = rep.cgImage!
            let ctx = makeContext(width: img.width, height: img.height, flipped: false)!
            ctx.setFillColor(Theme.bg.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: img.width, height: img.height))
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
            try? ctx.makeImage()?.pngData()?.write(to: URL(fileURLWithPath: dir).appendingPathComponent("ui-video-range.png"))
        }
        await close(e)
    }
}
