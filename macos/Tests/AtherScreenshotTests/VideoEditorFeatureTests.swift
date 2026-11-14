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
}
