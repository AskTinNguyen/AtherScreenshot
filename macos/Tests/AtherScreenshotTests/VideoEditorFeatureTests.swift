import AVFoundation
import AppKit
import Carbon.HIToolbox
import XCTest
@testable import AtherScreenshot

// The video editor's 0.0.2.6 / 0.0.2.7 features (Windows: video_editor_clip_bin, video_editor_flip, video_editor_loop,
// video_editor_guides, video_editor_overlay_videos, video_editor_speed_per_clip_and_reverse).
final class VideoEditorFeatureTests: XCTestCase {
    private func until(_ what: String, timeout: Double = 8, _ ok: @MainActor () -> Bool) async throws {
        let end = Date().addingTimeInterval(timeout)
        while Date() < end {
            if await MainActor.run(body: ok) { return }
            try await Task.sleep(nanoseconds: 30_000_000)
        }
        XCTFail("timed out: \(what)")
    }

    func open(_ url: URL) async throws -> VideoEditor {
        await MainActor.run { _ = NSApplication.shared; VideoEditor.open(url) }
        try await until("editor loads") { VideoEditor.instances.last?.edit.clips.count == 1 && VideoEditor.instances.last?.duration ?? 0 > 0 }
        let e = await MainActor.run { VideoEditor.instances.last! }
        await MainActor.run {
            e.window.setContentSize(NSSize(width: 1180, height: 760))
            e.window.contentView?.layoutSubtreeIfNeeded()
        }
        return e
    }

    @MainActor func close(_ e: VideoEditor) {
        e.dirty = false
        e.window.close()
    }

    @MainActor func mouse(_ e: VideoEditor, _ type: NSEvent.EventType, _ p: CGPoint, clicks: Int = 1, in view: NSView? = nil) {
        let v = view ?? e.timeline
        let w = v.convert(p, to: nil)
        let ev = NSEvent.mouseEvent(with: type, location: w, modifierFlags: [], timestamp: 0, windowNumber: e.window.windowNumber,
                                    context: nil, eventNumber: 0, clickCount: clicks, pressure: 1)!
        switch type {
        case .leftMouseDown: v.mouseDown(with: ev)
        case .leftMouseDragged: v.mouseDragged(with: ev)
        default: v.mouseUp(with: ev)
        }
    }

    @MainActor @discardableResult func key(_ e: VideoEditor, _ code: Int, cmd: Bool = false) -> Bool {
        let ev = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: cmd ? [.command] : [], timestamp: 0, windowNumber: e.window.windowNumber,
                                  context: nil, characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: UInt16(code))!
        return e.key(ev)
    }

    static func snapshot(_ e: VideoEditor, _ name: String) {
        guard let out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] else { return }
        let v = e.window.contentView!
        v.layoutSubtreeIfNeeded()
        let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)!
        v.cacheDisplay(in: v.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out).appendingPathComponent(name))
    }

    // MARK: bin

    // Port of video_editor_clip_bin: Del and dragging up take a clip out, dragging a piece back down puts it where it's
    // dropped, a double-click adds it at the end, Del in the bin deletes; undo covers every step; never saved.
    func testClipBin() async throws {
        let a = try await TestMedia.colors(w: 640, h: 360, seconds: 3)
        let b = try await TestMedia.colors(w: 640, h: 360, seconds: 2, colors: [.yellow])
        let e = try await open(a)
        await MainActor.run { XCTAssertEqual(e.timeline.binH, 0, "one clip: no bin") }
        await MainActor.run { e.addClips([b]) }
        try await until("b joins") { e.edit.clips.count == 2 && e.duration > 4.9 }
        await MainActor.run { e.seek(1.5) }
        try await until("seeked") { abs(e.now - 1.5) < 0.05 }
        let a2 = await MainActor.run { () -> UUID in
            e.splitAtPlayhead()   // [a1, a2, b], a2 selected
            XCTAssertEqual(e.edit.clips.count, 3)
            XCTAssertGreaterThan(e.timeline.binH, 0)
            let a2 = e.edit.clips[1].id
            XCTAssertEqual(e.selected, a2)
            // ⌫: a2 goes to the bin; the video is shorter. Undo puts it back, ⌫ again takes it out.
            key(e, kVK_Delete)
            XCTAssertEqual(e.edit.clips.count, 2)
            XCTAssertEqual(e.edit.bin.map(\.id), [a2])
            XCTAssertEqual(e.duration, 3.5, accuracy: 0.1)
            e.undo()
            XCTAssertEqual(e.edit.clips.count, 3)
            XCTAssertTrue(e.edit.bin.isEmpty)
            e.selected = a2
            key(e, kVK_Delete)
            XCTAssertEqual(e.edit.bin.count, 1)
            return a2
        }
        try await until("rebuilt") { abs(e.duration - 3.5) < 0.1 }
        await MainActor.run {
            e.window.contentView?.layoutSubtreeIfNeeded()
            let tl = e.timeline, laneY = tl.laneY + tl.laneH / 2
            let chip = tl.binChips()[0]
            let c = CGPoint(x: chip.midX, y: chip.midY)
            // Dragged from the bin to the start of the timeline.
            mouse(e, .leftMouseDown, c)
            XCTAssertEqual(e.selectedBinIndex, 0)
            mouse(e, .leftMouseDragged, CGPoint(x: c.x + 30, y: c.y + 20))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(0.1), y: laneY))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(0.1), y: laneY))
            XCTAssertEqual(e.edit.clips.count, 3)
            XCTAssertEqual(e.edit.clips[0].id, a2)
            XCTAssertTrue(e.edit.bin.isEmpty)
            XCTAssertEqual(e.selectedClipIndex, 0)
            XCTAssertEqual(e.duration, 5, accuracy: 0.1)
            e.undo()
            XCTAssertEqual(e.edit.clips.count, 2)
            XCTAssertEqual(e.edit.bin.count, 1)
        }
        try await until("rebuilt after undo") { abs(e.duration - 3.5) < 0.1 }
        await MainActor.run {
            // b (the last clip) dragged up onto the bin.
            let tl = e.timeline, laneY = tl.laneY + tl.laneH / 2
            let bId = e.edit.clips[1].id
            let bMid = tl.x(VideoSequence.starts(e.edit.clips)[1] + e.edit.clips[1].duration / 2)
            mouse(e, .leftMouseDown, CGPoint(x: bMid, y: laneY))
            mouse(e, .leftMouseDragged, CGPoint(x: bMid, y: laneY - 20))
            mouse(e, .leftMouseDragged, CGPoint(x: bMid, y: tl.binH / 2))
            mouse(e, .leftMouseUp, CGPoint(x: bMid, y: tl.binH / 2))
            XCTAssertEqual(e.edit.clips.count, 1)
            XCTAssertEqual(e.edit.bin.count, 2)
            XCTAssertEqual(e.edit.bin[1].id, bId)
            // The last clip stays.
            e.selected = e.edit.clips[0].id
            key(e, kVK_Delete)
            XCTAssertEqual(e.edit.clips.count, 1, "the last clip stays")
            // A click on a piece selects it without moving anything; a double-click puts it back at the end.
            let chip = tl.binChips()[1]
            let c = CGPoint(x: chip.midX, y: chip.midY)
            mouse(e, .leftMouseDown, c)
            mouse(e, .leftMouseUp, c)
            XCTAssertEqual(e.selectedBinIndex, 1)
            XCTAssertEqual(e.edit.bin.count, 2)
            mouse(e, .leftMouseDown, c, clicks: 2)
            mouse(e, .leftMouseUp, c, clicks: 2)
            XCTAssertEqual(e.edit.clips.count, 2)
            XCTAssertEqual(e.edit.clips[1].id, bId)
            XCTAssertEqual(e.edit.bin.count, 1)
            // ⌫ in the bin deletes for good; undo brings it back.
            e.selected = e.edit.bin[0].id
            key(e, kVK_Delete)
            XCTAssertTrue(e.edit.bin.isEmpty)
            XCTAssertEqual(e.edit.clips.count, 2)
            e.undo()
            XCTAssertEqual(e.edit.bin.map(\.id), [a2])
        }
        // The bin is never saved: the export is the two clips on the timeline.
        let edit = await MainActor.run { e.edit }
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-bin-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(edit, to: out)
        let probed = try await VideoSource.probe(out)
        XCTAssertEqual(probed.length, VideoSequence.total(edit.clips), accuracy: 0.1)
        await MainActor.run { close(e) }
    }

    // MARK: flip

    // Left half red, right half blue, a green band along the top (for the vertical flip).
    static func halves(seconds: Double = 2) async throws -> URL {
        try await TestMedia.clip(w: 640, h: 360, seconds: seconds) { _, ctx in
            ctx.setFillColor(NSColor.red.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: 320, height: 360))
            ctx.setFillColor(NSColor.blue.cgColor); ctx.fill(CGRect(x: 320, y: 0, width: 320, height: 360))
            ctx.setFillColor(NSColor.green.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: 640, height: 60))
        }
    }

    // Port of video_flip_mirrors_the_picture: only the flipped clip's frames mirror, in the saved video too.
    func testFlipMirrorsTheSavedPicture() async throws {
        let src = try await Self.halves()
        let c = try await VideoSource.probe(src)
        var a = c, b = c
        a.outPoint = 1
        b.id = UUID(); b.inPoint = 1
        b.flipH = true
        var e = TestMedia.edit([a, b])
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-flip-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(e, to: out)
        let asset = AVURLAsset(url: out)
        let first = try await TestMedia.frame(asset, at: 0.5), second = try await TestMedia.frame(asset, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(first, 20, 180).0, 0.8, "the first clip as it is: red on the left")
        XCTAssertGreaterThan(TestMedia.rgb(second, 20, 180).2, 0.8, "the flipped clip: blue on the left")
        XCTAssertGreaterThan(TestMedia.rgb(second, 620, 180).0, 0.8)
        // Upside down: the green band at the bottom; the crop is taken from the flipped picture.
        e.clips[1].flipH = false
        e.clips[1].flipV = true
        e.crop = CGRect(x: 0, y: 200, width: 320, height: 160)
        let img = try await VideoExport.frame(e, at: 1.5)
        XCTAssertEqual(img.width, 320)
        XCTAssertGreaterThan(TestMedia.rgb(img, 160, img.height - 10).1, 0.8, "green at the bottom of the upside-down picture")
        XCTAssertGreaterThan(TestMedia.rgb(img, 160, 20).0, 0.8)
    }

    // Port of video_editor_flip: Flip acts on the selected clip (else the one at the playhead), one undo step, markup
    // wholly on that clip mirrored with it, the crop untouched; the clip lane shows ↔ / ↕.
    func testEditorFlip() async throws {
        let e = try await open(try await Self.halves(seconds: 3))
        await MainActor.run { e.seek(1.5) }
        try await until("seeked") { abs(e.now - 1.5) < 0.05 }
        let b0 = await MainActor.run { e.builds }
        await MainActor.run {
            e.splitAtPlayhead()   // [0–1.5, 1.5–3], the second selected
            e.selected = nil
            e.edit.marks = [Mark(kind: .box, start: 1.6, end: 2.5, a: CGPoint(x: 40, y: 50), b: CGPoint(x: 140, y: 100)),
                            Mark(kind: .box, start: 0.2, end: 1.0, a: CGPoint(x: 40, y: 50), b: CGPoint(x: 140, y: 100))]
            e.edit.crop = CGRect(x: 10, y: 10, width: 300, height: 200)
        }
        try await until("rebuilt") { e.builds > b0 }
        await MainActor.run { e.seek(2) }
        try await until("at 2 s") { abs(e.now - 2) < 0.05 }
        await MainActor.run {
            XCTAssertEqual(e.flipTarget, 1, "the clip at the playhead")
            e.toggleFlip(horizontal: true)
            XCTAssertTrue(e.edit.clips[1].flipH)
            XCTAssertFalse(e.edit.clips[0].flipH)
            XCTAssertEqual(e.edit.marks[0].rect, CGRect(x: 500, y: 50, width: 100, height: 50), "markup on it mirrored")
            XCTAssertEqual(e.edit.marks[1].rect, CGRect(x: 40, y: 50, width: 100, height: 50), "markup on the other clip stays")
            XCTAssertEqual(e.edit.crop, CGRect(x: 10, y: 10, width: 300, height: 200), "the crop stays")
            XCTAssertEqual(e.flipMenu.itemTitle(at: 0), "Flipped ↔")
            XCTAssertTrue(e.edit.clips[1].badges.contains("↔"))
            e.selected = e.edit.clips[0].id
            e.toggleFlip(horizontal: false)
            XCTAssertTrue(e.edit.clips[0].flipV)
            XCTAssertEqual(e.edit.marks[1].rect, CGRect(x: 40, y: 260, width: 100, height: 50))
            e.undo()
            XCTAssertFalse(e.edit.clips[0].flipV)
            e.undo()
            XCTAssertFalse(e.edit.clips[1].flipH)
            XCTAssertEqual(e.edit.marks[0].rect, CGRect(x: 40, y: 50, width: 100, height: 50))
            close(e)
        }
    }

    // MARK: loop

    // Port of video_editor_loop: plays round the trim and round a selected clip; with Loop off playback stops at the end.
    func testLoop() async throws {
        let e = try await open(try await TestMedia.colors(seconds: 3))
        await MainActor.run {
            XCTAssertTrue(key(e, kVK_ANSI_L, cmd: true))
            XCTAssertTrue(e.loop)
            e.edit.trimStart = 0.5
            e.edit.trimEnd = 1.2
            e.togglePlay()
        }
        try await until("round the trim") { e.loops >= 1 }
        await MainActor.run {
            XCTAssertNotEqual(e.player.rate, 0, "still playing")
            XCTAssertTrue(e.now >= 0.45 && e.now < 1.25)
            e.togglePlay()
            e.edit.trimStart = 0
            e.edit.trimEnd = 3
            e.seek(2)
        }
        let b0 = await MainActor.run { e.builds }
        await MainActor.run { e.splitAtPlayhead() }   // [0–2, 2–3], the second selected
        try await until("rebuilt") { e.builds > b0 }
        await MainActor.run {
            XCTAssertEqual(e.loopRange.0, 2, accuracy: 1e-6)
            XCTAssertEqual(e.loopRange.1, 3, accuracy: 1e-6)
            e.seek(2.6)
            e.togglePlay()
        }
        let l0 = await MainActor.run { e.loops }
        try await until("round the clip") { e.loops > l0 }
        await MainActor.run { XCTAssertGreaterThanOrEqual(e.now, 1.95) }
        await MainActor.run {
            e.togglePlay()
            e.loop = false
            e.selected = nil
            e.seek(2.7)
            e.togglePlay()
        }
        let l1 = await MainActor.run { e.loops }
        try await until("stops at the end") { e.player.rate == 0 }
        await MainActor.run {
            XCTAssertEqual(e.loops, l1, "didn't go round")
            close(e)
        }
    }

    // MARK: guides

    // Port of video_composition_guide_lines: each guide's lines at its fractions, ending on the picture's edges, and the
    // diagonals' corner lines square to the other diagonal (wide and tall).
    func testGuideLines() {
        let r = CGRect(x: 10, y: 20, width: 1600, height: 900)
        for (g, f) in [(GuideKind.golden, 0.317), (.thirds, 1.0 / 3), (.phi, 0.24)] {
            let l = g.lines(r)
            XCTAssertEqual(l.count, 4)
            XCTAssertEqual(l[0].0.x, r.minX + r.width * f, accuracy: 1e-6)
            XCTAssertEqual(l[0].0.y, r.minY); XCTAssertEqual(l[0].1.y, r.maxY)
            XCTAssertEqual(l[1].0.y, r.minY + r.height * f, accuracy: 1e-6)
            XCTAssertEqual(l[2].0.x, r.minX + r.width * (1 - f), accuracy: 1e-6)
        }
        for rect in [r, CGRect(x: 0, y: 0, width: 600, height: 1000)] {
            let d = GuideKind.diagonal.lines(rect)
            XCTAssertEqual(d.count, 6)
            func dir(_ l: (CGPoint, CGPoint)) -> CGVector { CGVector(dx: l.1.x - l.0.x, dy: l.1.y - l.0.y) }
            XCTAssertEqual(dir(d[2]).dx * dir(d[1]).dx + dir(d[2]).dy * dir(d[1]).dy, 0, accuracy: 1e-3, "square to the other diagonal")
            XCTAssertEqual(dir(d[4]).dx * dir(d[0]).dx + dir(d[4]).dy * dir(d[0]).dy, 0, accuracy: 1e-3)
            for l in d {   // every line ends on an edge
                let p = l.1
                let onEdge = abs(p.x - rect.minX) < 1e-6 || abs(p.x - rect.maxX) < 1e-6 || abs(p.y - rect.minY) < 1e-6 || abs(p.y - rect.maxY) < 1e-6
                XCTAssertTrue(onEdge)
            }
        }
        // On 16:9 the golden section passes where the diagonal's corner lines cross the other diagonal ((9/16)² ≈ 0.316).
        XCTAssertEqual(pow(9.0 / 16, 2), 0.317, accuracy: 0.002)
    }

    // Port of video_editor_guides: the menu, G and Show all; the monochrome preview; the edit (and so the export) untouched.
    func testEditorGuidesAndMonochrome() async throws {
        let e = try await open(try await TestMedia.colors(seconds: 2))
        await MainActor.run {
            e.guides = []; e.guidesHidden = false; e.monochrome = false
            let before = e.edit
            XCTAssertTrue(key(e, kVK_ANSI_G))
            XCTAssertEqual(e.shownGuides, [.thirds], "G with none picked: rule of thirds")
            key(e, kVK_ANSI_G)
            XCTAssertTrue(e.shownGuides.isEmpty, "G hides them")
            key(e, kVK_ANSI_G)
            XCTAssertEqual(e.shownGuides, [.thirds], "and brings the same back")
            let menu = NSMenu()
            e.fillGuidesMenu(menu, title: false)
            XCTAssertEqual(menu.items.prefix(4).map(\.title), GuideKind.allCases.map(\.label))
            (menu.items.first { $0.title == "Show all" }!.target as! MenuAction).fire()
            XCTAssertEqual(e.shownGuides, GuideKind.allCases)
            e.fillGuidesMenu(menu, title: false)
            (menu.items.first { $0.title == "Monochrome view" }!.target as! MenuAction).fire()
            XCTAssertTrue(e.monochrome)
            XCTAssertEqual(e.edit, before, "guides and the monochrome view aren't part of the edit")
            XCTAssertTrue(e.previewRenderer().monochrome)
        }
        // The preview renderer turns the picture gray; the export's renderer doesn't.
        let edit = await MainActor.run { e.edit }
        let red = CIImage(color: .red).cropped(to: CGRect(x: 0, y: 0, width: 640, height: 360))
        let ctx = CIContext()
        func px(_ img: CIImage) -> [UInt8] {
            var b = [UInt8](repeating: 0, count: 4)
            ctx.render(img, toBitmap: &b, rowBytes: 4, bounds: CGRect(x: 100, y: 100, width: 1, height: 1), format: .RGBA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
            return b
        }
        let pr = FrameRenderer(edit: edit, full: CGSize(width: 640, height: 360), preview: true)
        pr.monochrome = true
        let gray = px(pr.render(red, at: 0.5))
        XCTAssertLessThan(abs(Int(gray[0]) - Int(gray[1])), 6, "black and white")
        let saved = try await VideoExport.frame(edit, at: 0.5)
        XCTAssertGreaterThan(TestMedia.rgb(saved, 320, 180).0, 0.8, "the saved frame keeps its color and has no guide lines")
        XCTAssertGreaterThan(TestMedia.rgb(saved, 213, 120).0, 0.8)
        await MainActor.run {
            e.guides = []; e.monochrome = false
            close(e)
        }
    }

    // MARK: speed per clip

    // Port of video_clip_speeds_set_output_time.
    func testClipSpeedsSetOutputTime() {
        func c(_ d: Double, _ v: Double) -> Clip { var x = Clip(path: "/a.mp4", inPoint: 0, outPoint: d, length: 10, w: 640, h: 360, fps: 30, hasAudio: false); x.speed = v; return x }
        let clips = [c(2, 2), c(1, 1), c(2, 0.5)]
        XCTAssertEqual(VideoSequence.outputTime(clips, 5), 6, accuracy: 1e-9)
        XCTAssertEqual(VideoSequence.outputTime(clips, 1), 0.5, accuracy: 1e-9)
        XCTAssertEqual(VideoSequence.outputTime(clips, 6), 8, accuracy: 1e-9, "past the end at the last clip's speed")
        XCTAssertEqual(VideoSequence.timelineTime(clips, 1.5), 2.5, accuracy: 1e-9)
        XCTAssertEqual(VideoSequence.timelineTime(clips, 4), 4, accuracy: 1e-9)
        for t in stride(from: 0.0, through: 5, by: 0.25) { XCTAssertEqual(VideoSequence.timelineTime(clips, VideoSequence.outputTime(clips, t)), t, accuracy: 1e-9) }
        var e = TestMedia.edit(clips)
        XCTAssertEqual(e.outputDuration, 6, accuracy: 1e-9)
        e.trimStart = 1; e.trimEnd = 3
        XCTAssertEqual(e.outputDuration, 1.5, accuracy: 1e-9)
        XCTAssertEqual(e.speed, 1, "mixed speeds read as 1×")
        e.speed = 2   // the whole-video setter: every clip
        XCTAssertEqual(e.clips.map(\.speed), [2, 2, 2])
        XCTAssertEqual(e.clips[2].badges, "  2×")
    }

    // Port of video_export_speed_per_clip: the saved video, its sound, the GIF and Copy clip follow each clip's speed.
    func testExportSpeedPerClip() async throws {
        let src = try await TestMedia.colors(seconds: 3, tone: true)   // red, green, blue a second each
        let c = try await VideoSource.probe(src)
        var a = c, b = c
        a.outPoint = 1; a.speed = 2
        b.id = UUID(); b.inPoint = 1; b.speed = 0.5
        let e = TestMedia.edit([a, b])
        XCTAssertEqual(e.outputDuration, 4.5, accuracy: 1e-6)
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-speed-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(e, to: out)
        let asset = AVURLAsset(url: out)
        let v = try await asset.loadTracks(withMediaType: .video).first!
        let au = try await asset.loadTracks(withMediaType: .audio).first
        let vd = try await v.load(.timeRange).duration.seconds
        XCTAssertEqual(vd, 4.5, accuracy: 0.1)
        XCTAssertNotNil(au, "sound kept")
        if let au {
            let ad = try await au.load(.timeRange).duration.seconds
            XCTAssertEqual(ad, 4.5, accuracy: 0.15, "sound stretched with each clip")
        }
        let f0 = try await TestMedia.frame(asset, at: 0.25), f1 = try await TestMedia.frame(asset, at: 1.0), f2 = try await TestMedia.frame(asset, at: 4.0)
        XCTAssertGreaterThan(TestMedia.rgb(f0, 10, 10).0, 0.7, "0.25 s out is 0.5 s in: red")
        XCTAssertGreaterThan(TestMedia.rgb(f1, 10, 10).1, 0.7, "1.0 s out is 1.25 s in: green")
        XCTAssertGreaterThan(TestMedia.rgb(f2, 10, 10).2, 0.7, "4.0 s out is 2.75 s in: blue")
        // The GIF: 4.5 s at 10 fps.
        let gif = FileManager.default.temporaryDirectory.appendingPathComponent("ather-speed-\(UUID().uuidString).gif")
        try await VideoExport.gif(e, to: gif, fps: 10)
        let n = CGImageSourceGetCount(CGImageSourceCreateWithURL(gif as CFURL, nil)!)
        XCTAssertEqual(Double(n), 45, accuracy: 2)
        // Copy clip over timeline 0.5–2: 0.25 s + 2 s.
        var r = e
        r.trimStart = 0.5; r.trimEnd = 2
        let clip = try await Compress.clip(r)
        let cd = try await AVURLAsset(url: clip).load(.duration).seconds
        XCTAssertEqual(cd, 2.25, accuracy: 0.1)
        try? FileManager.default.removeItem(at: clip)
    }

    // The editor: Speed sets the selected clip (else the one at the playhead), Every clip at sets all, one undo step each;
    // the timeline stays at 1× while the preview plays each clip at its speed.
    func testEditorSpeedPerClip() async throws {
        let e = try await open(try await TestMedia.colors(seconds: 3))
        await MainActor.run { e.seek(1) }
        try await until("at 1 s") { abs(e.now - 1) < 0.05 }
        let b0 = await MainActor.run { e.builds }
        await MainActor.run { e.splitAtPlayhead(); e.selected = nil }
        try await until("split") { e.builds > b0 }
        await MainActor.run { e.seek(0.5) }
        try await until("at 0.5 s") { abs(e.now - 0.5) < 0.05 }
        let b1 = await MainActor.run { () -> Int in
            XCTAssertEqual(e.speedTarget, 0, "the clip at the playhead")
            e.syncSpeedMenu()
            XCTAssertEqual(e.speedMenu.itemTitle(at: 0), "Speed 1×")
            XCTAssertNotNil(e.speedMenu.menu?.item(withTitle: "Every clip at"))
            let b = e.builds
            e.setSpeed(2, every: false)
            XCTAssertEqual(e.edit.clips.map(\.speed), [2, 1])
            XCTAssertEqual(e.duration, 3, accuracy: 1e-6, "the timeline shows the footage at 1×")
            XCTAssertEqual(e.edit.outputDuration, 2.5, accuracy: 1e-6)
            XCTAssertEqual(e.speedMenu.itemTitle(at: 0), "Speed 2×")
            return b
        }
        try await until("rebuilt at speed") { e.builds > b1 }
        await MainActor.run {
            XCTAssertEqual(e.now, 0.5, accuracy: 0.05, "the playhead stays on its footage")
            XCTAssertEqual(e.player.currentTime().seconds, 0.25, accuracy: 0.05, "the player plays the output")
            e.seek(2)
        }
        try await until("at 2 s") { abs(e.now - 2) < 0.05 }
        await MainActor.run {
            e.selected = e.edit.clips[1].id
            e.setSpeed(0.5, every: false)
            XCTAssertEqual(e.edit.clips.map(\.speed), [2, 0.5])
            e.setSpeed(4, every: true)
            XCTAssertEqual(e.edit.clips.map(\.speed), [4, 4])
            e.undo()
            XCTAssertEqual(e.edit.clips.map(\.speed), [2, 0.5])
            e.undo(); e.undo()
            XCTAssertEqual(e.edit.clips.map(\.speed), [1, 1])
            close(e)
        }
    }

    // MARK: videos on top

    // Port of video_overlay_box_and_split_screen / video_export_with_overlay_video: the box and both split modes land
    // where they should in the saved video (and in Copy frame and the GIF), only while the overlay shows.
    func testOverlayRenderPositions() async throws {
        let main = try await TestMedia.colors(seconds: 3, colors: [.red])
        let top = try await TestMedia.colors(w: 320, h: 180, seconds: 2, colors: [.green])
        let m = try await VideoSource.probe(main), o = try await VideoSource.probe(top)
        var e = TestMedia.edit([m])
        var ov = VideoOverlay(clip: o, start: 1)   // 1–3 s
        ov.pos = CGPoint(x: 0.5, y: 0.5)
        ov.width = 0.25   // 160 × 90 at (320, 180)
        e.overlays = [ov]
        XCTAssertEqual(ov.rect(CGSize(width: 640, height: 360)), CGRect(x: 320, y: 180, width: 160, height: 90))
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-pip-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(e, to: out)
        let asset = AVURLAsset(url: out)
        let tracks = try await asset.loadTracks(withMediaType: .video)
        XCTAssertEqual(tracks.count, 1)
        let before = try await TestMedia.frame(asset, at: 0.5), during = try await TestMedia.frame(asset, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(before, 400, 225).0, 0.8, "not on yet")
        XCTAssertGreaterThan(TestMedia.rgb(during, 400, 225).1, 0.8, "the box")
        XCTAssertGreaterThan(TestMedia.rgb(during, 300, 225).0, 0.8, "left of the box: the video")
        XCTAssertGreaterThan(TestMedia.rgb(during, 400, 300).0, 0.8, "under the box: the video")
        // Copy frame draws it too.
        let copied = try await VideoExport.frame(e, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(copied, 400, 225).1, 0.8)
        // Split, right half, fit: each picture shown whole, 320 × 180 in the middle of its half, black bars above and below.
        e.overlays[0].layout = .splitRight
        let fit = try await VideoExport.frame(e, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(fit, 160, 180).0, 0.8, "the video in the left half")
        XCTAssertGreaterThan(TestMedia.rgb(fit, 480, 180).1, 0.8, "the overlay in the right half")
        XCTAssertLessThan(TestMedia.rgb(fit, 160, 20).0, 0.1, "bars")
        XCTAssertLessThan(TestMedia.rgb(fit, 480, 340).1, 0.1)
        // Fill: each half filled.
        e.overlays[0].fill = true
        let fill = try await VideoExport.frame(e, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(fill, 160, 20).0, 0.8)
        XCTAssertGreaterThan(TestMedia.rgb(fill, 480, 340).1, 0.8)
        // The top half.
        e.overlays[0].layout = .splitTop
        let topHalf = try await VideoExport.frame(e, at: 1.5)
        XCTAssertGreaterThan(TestMedia.rgb(topHalf, 320, 60).1, 0.8)
        XCTAssertGreaterThan(TestMedia.rgb(topHalf, 320, 300).0, 0.8)
        // A GIF shows it the same way.
        let gif = FileManager.default.temporaryDirectory.appendingPathComponent("ather-pip-\(UUID().uuidString).gif")
        try await VideoExport.gif(e, to: gif, fps: 4)
        let src = CGImageSourceCreateWithURL(gif as CFURL, nil)!
        let g = CGImageSourceCreateImageAtIndex(src, 7, nil)!   // 1.75 s
        XCTAssertGreaterThan(TestMedia.rgb(g, g.width / 2, g.height / 6).1, 0.7)
        XCTAssertGreaterThan(TestMedia.rgb(g, g.width / 2, g.height * 5 / 6).0, 0.7)
    }

    // A flipped video on top is mirrored in its box.
    func testFlippedOverlay() async throws {
        let main = try await TestMedia.colors(seconds: 2, colors: [.black])
        let top = try await TestMedia.clip(w: 320, h: 180, seconds: 2) { _, ctx in   // green left, black right
            ctx.setFillColor(NSColor.black.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: 320, height: 180))
            ctx.setFillColor(NSColor.green.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: 160, height: 180))
        }
        var e = TestMedia.edit([try await VideoSource.probe(main)])
        var ov = VideoOverlay(clip: try await VideoSource.probe(top))
        ov.pos = CGPoint(x: 0.5, y: 0.5); ov.width = 0.25   // 160 × 90 at (320, 180)
        ov.clip.flipH = true
        e.overlays = [ov]
        let img = try await VideoExport.frame(e, at: 1)
        XCTAssertLessThan(TestMedia.rgb(img, 330, 220).1, 0.1, "its left: the black right half")
        XCTAssertGreaterThan(TestMedia.rgb(img, 470, 220).1, 0.8)
    }

    // Port of video_editor_overlay_videos and video_editor_short_overlay_trims_stay_in_its_footage: the box moves and
    // sizes on the video, its bar moves and trims on the lane, a bin piece dragged to the lane goes on top, ⌫ takes it
    // to the bin; undo covers each step.
    func testEditorOverlayVideos() async throws {
        let top = try await TestMedia.colors(w: 320, h: 180, seconds: 2, colors: [.green])
        let topClip = try await VideoSource.probe(top)
        let e = try await open(try await TestMedia.colors(seconds: 3))
        let b0 = await MainActor.run { e.builds }
        await MainActor.run {
            e.addOverlay(topClip, at: 0.5, layout: .pip)
            XCTAssertEqual(e.edit.overlays.count, 1)
            XCTAssertEqual(e.selectedOverlayIndex, 0)
            XCTAssertGreaterThan(e.timeline.wantedHeight, e.timeline.ovY, "the lane at the bottom")
        }
        try await until("rebuilt with the overlay") { e.builds > b0 }
        await MainActor.run { e.seek(1) }
        try await until("at 1 s") { abs(e.now - 1) < 0.05 }
        await MainActor.run {
            e.window.contentView?.layoutSubtreeIfNeeded()
            let st = e.stageView
            let o = e.edit.overlays[0]
            let r = st.toView(e.overlayOnVideo(o))
            // Drag the box 64 video pixels left.
            let k = r.width / o.rect(e.viewRect.size).width
            mouse(e, .leftMouseDown, CGPoint(x: r.midX, y: r.midY), in: st)
            mouse(e, .leftMouseDragged, CGPoint(x: r.midX - 64 * k, y: r.midY), in: st)
            mouse(e, .leftMouseUp, CGPoint(x: r.midX - 64 * k, y: r.midY), in: st)
            XCTAssertEqual(e.edit.overlays[0].pos.x, o.pos.x - 0.1, accuracy: 0.01)
            // Its bottom-right corner, dragged out: bigger, the top-left stays.
            let r2 = st.toView(e.overlayOnVideo(e.edit.overlays[0]))
            mouse(e, .leftMouseDown, CGPoint(x: r2.maxX, y: r2.maxY), in: st)
            mouse(e, .leftMouseDragged, CGPoint(x: r2.maxX + 40, y: r2.maxY + 40), in: st)
            mouse(e, .leftMouseUp, CGPoint(x: r2.maxX + 40, y: r2.maxY + 40), in: st)
            XCTAssertGreaterThan(e.edit.overlays[0].width, o.width + 0.03)
            XCTAssertEqual(e.edit.overlays[0].pos.x, o.pos.x - 0.1, accuracy: 0.01)
            // The wheel: smaller; the presets.
            let w = e.edit.overlays[0].width
            e.scaleOverlay(0, by: 1 / 1.08)
            XCTAssertLessThan(e.edit.overlays[0].width, w)
            e.updateOverlay { $0.width = VideoOverlay.sizes[2].1 }
            XCTAssertEqual(e.edit.overlays[0].width, 0.5)
            // Its bar: dragged half a second later; its end, trimmed.
            let tl = e.timeline, y = tl.ovY + tl.ovH / 2
            let ob = e.edit.overlays[0]
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(ob.start + 0.8), y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(ob.start + 1.3), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(ob.start + 1.3), y: y))
            XCTAssertEqual(e.edit.overlays[0].start, 1, accuracy: 0.05)
            let o3 = e.edit.overlays[0]
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(o3.end), y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(o3.end - 1), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(o3.end - 1), y: y))
            XCTAssertEqual(e.edit.overlays[0].clip.duration, 1, accuracy: 0.05)
            // A short overlay's start dragged far left stays in its footage.
            e.edit.overlays[0].clip.inPoint = 0.05; e.edit.overlays[0].clip.outPoint = 0.15
            let o4 = e.edit.overlays[0]
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(o4.start), y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(o4.start - 0.8), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(o4.start - 0.8), y: y))
            XCTAssertGreaterThanOrEqual(e.edit.overlays[0].clip.inPoint, 0)
            XCTAssertLessThanOrEqual(e.edit.overlays[0].clip.outPoint, topClip.length + 1e-6)
            // ⌫: its footage goes to the bin; undo brings it back on top.
            e.selected = e.edit.overlays[0].id
            key(e, kVK_Delete)
            XCTAssertTrue(e.edit.overlays.isEmpty)
            XCTAssertEqual(e.edit.bin.count, 1)
            e.undo()
            XCTAssertEqual(e.edit.overlays.count, 1)
            XCTAssertTrue(e.edit.bin.isEmpty)
            e.selected = e.edit.overlays[0].id
            key(e, kVK_Delete)
        }
        await MainActor.run {
            // A bin piece dragged down to the lane goes on top there.
            e.window.contentView?.layoutSubtreeIfNeeded()
            let tl = e.timeline
            guard let chip = tl.binChips().first else { XCTFail("no bin piece \(e.edit.bin.count) \(tl.binH) \(tl.bounds) \(e.duration)"); return close(e) }
            mouse(e, .leftMouseDown, CGPoint(x: chip.midX, y: chip.midY))
            mouse(e, .leftMouseDragged, CGPoint(x: chip.midX + 20, y: chip.midY + 30))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(2), y: tl.ovY + 10))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(2), y: tl.ovY + 10))
            XCTAssertEqual(e.edit.overlays.count, 1)
            XCTAssertTrue(e.edit.bin.isEmpty)
            XCTAssertEqual(e.edit.overlays[0].start, 2, accuracy: 0.05)
            // The split layouts from the inspector's menu, and flipping the selected one.
            e.updateOverlay { $0.layout = .splitLeft }
            XCTAssertTrue(e.edit.overlays[0].isSplit)
            e.toggleFlip(horizontal: true)
            XCTAssertTrue(e.edit.overlays[0].clip.flipH)
            XCTAssertFalse(e.edit.clips[0].flipH, "the clip stays")
            close(e)
        }
    }
}
