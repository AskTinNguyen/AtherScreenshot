import AVFoundation
import AppKit
import Carbon.HIToolbox
import XCTest
@testable import AtherScreenshot

final class ClipTests: XCTestCase {
    private func clip(_ path: String, _ inP: Double, _ outP: Double, source: UUID = UUID()) -> Clip {
        Clip(source: source, path: "/tmp/\(path).mp4", inPoint: inP, outPoint: outP, length: max(outP, 10), w: 640, h: 360, fps: 30, hasAudio: false)
    }

    // Port of video_clips_locate_and_total.
    func testClipsLocateAndTotal() {
        let v = [clip("a", 1, 3), clip("b", 0, 4)]
        XCTAssertEqual(VideoSequence.total(v), 6, accuracy: 1e-9)
        XCTAssertEqual(VideoSequence.starts(v)[1], 2, accuracy: 1e-9)
        var s = VideoSequence.locate(v, 2.5)
        XCTAssertTrue(s?.index == 1 && abs(s!.fileTime - 0.5) < 1e-9)
        s = VideoSequence.locate(v, 0.5)
        XCTAssertTrue(s?.index == 0 && abs(s!.fileTime - 1.5) < 1e-9)
        s = VideoSequence.locate(v, 99)  // past the end: the end of the last clip
        XCTAssertTrue(s?.index == 1 && abs(s!.fileTime - 4) < 1e-9)
    }

    // Port of video_clip_changes_move_items_with_their_footage.
    func testClipChangesMoveItemsWithTheirFootage() {
        var e = VideoEdit(trimEnd: 6)
        let a = clip("a", 0, 4), b = clip("b", 0, 2)
        e.clips = [a, b]
        let inA = Mark(kind: .box, start: 1, end: 2, a: .zero, b: CGPoint(x: 10, y: 10))
        let inB = Mark(kind: .box, start: 4.5, end: 5.5, a: .zero, b: CGPoint(x: 10, y: 10))
        e.marks = [inA, inB]
        e.captions = [Caption(start: 4.2, end: 4.8, text: "hi", words: [CaptionWord(start: 4.2, end: 4.5, text: "hi")])]

        var swapped = e
        swapped.applyClips([b, a])  // b first now
        XCTAssertEqual(swapped.marks[0].start, 3, accuracy: 1e-9)    // a's mark moved 2 s later
        XCTAssertEqual(swapped.marks[1].start, 0.5, accuracy: 1e-9)  // b's mark moved to the front
        XCTAssertEqual(swapped.captions[0].start, 0.2, accuracy: 1e-9)
        XCTAssertEqual(swapped.captions[0].words[0].start, 0.2, accuracy: 1e-9)  // word times follow
        XCTAssertEqual(swapped.trimEnd, 6, accuracy: 1e-9)

        var removed = e
        removed.applyClips([a])
        XCTAssertEqual(removed.marks.count, 1)  // b's mark went with b
        XCTAssertTrue(removed.captions.isEmpty)
        XCTAssertEqual(removed.trimEnd, 4, accuracy: 1e-9)

        var trimmed = e
        var a2 = a
        a2.inPoint = 2.5  // cut a's first 2.5 s: the mark at 1–2 s is gone, b moves 2.5 s earlier
        trimmed.applyClips([a2, b])
        XCTAssertEqual(trimmed.marks.count, 1)
        XCTAssertEqual(trimmed.marks[0].start, 2, accuracy: 1e-9)
        XCTAssertEqual(trimmed.trimEnd, 3.5, accuracy: 1e-9)

        var split = e  // splitting keeps everything where it was; dropping one half drops its items
        var left = a, right = a
        left.outPoint = 1.5
        right.id = UUID()
        right.inPoint = 1.5
        split.applyClips([left, right, b])
        XCTAssertEqual(split.marks.count, 2)
        XCTAssertEqual(split.marks[0].start, 1, accuracy: 1e-9)
        XCTAssertEqual(split.marks[1].start, 4.5, accuracy: 1e-9)
        split.applyClips([right, b])
        XCTAssertEqual(split.marks.count, 1)
        XCTAssertEqual(split.marks[0].start, 3, accuracy: 1e-9)

        // The same file added twice: removing one copy drops its items; they don't jump to the other copy.
        let x1 = clip("x", 0, 10), x2 = clip("x", 0, 10)
        var twice = VideoEdit(trimEnd: 18)  // (a trimmed end, so it follows its footage)
        twice.clips = [x1, x2]
        twice.captions = [Caption(start: 15, end: 17, text: "late")]
        twice.applyClips([x1])
        XCTAssertTrue(twice.captions.isEmpty)
        XCTAssertEqual(twice.trimEnd, 10, accuracy: 1e-9)

        var kept = e  // the trim follows its footage
        kept.trimStart = 1  // 1 s into a
        kept.trimEnd = 5    // 1 s into b
        var cut = kept
        var b2 = b
        b2.outPoint = 1.5
        cut.applyClips([a, b2])
        XCTAssertEqual(cut.trimStart, 1, accuracy: 1e-9)
        XCTAssertEqual(cut.trimEnd, 5, accuracy: 1e-9)
        kept.applyClips([b, a])  // the ends would cross: back to the whole sequence
        XCTAssertEqual(kept.trimStart, 0, accuracy: 1e-9)
        XCTAssertEqual(kept.trimEnd, 6, accuracy: 1e-9)
    }

    // Port of video_sequence_joins_clips_into_one_video: mixed sizes and sound.
    func testSequenceJoinsClipsIntoOneVideo() async throws {
        let a = try await VideoSource.probe(try await TestMedia.colors(w: 640, h: 360, seconds: 1, colors: [.red], tone: true))
        let b = try await VideoSource.probe(try await TestMedia.colors(w: 300, h: 300, seconds: 1, colors: [.green]))  // square, silent
        let c = try await VideoSource.probe(try await TestMedia.colors(w: 640, h: 360, seconds: 1, colors: [.blue], tone: true))
        let e = TestMedia.edit([a, b, c])
        XCTAssertEqual(e.frame, CGSize(width: 640, height: 360), "the first video's size")
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-join-\(UUID().uuidString).mp4")
        try await VideoExport.mp4(e, to: out)
        let res = AVURLAsset(url: out)
        let joined = try await VideoSource.probe(out)
        XCTAssertEqual(joined.length, 3, accuracy: 0.1)
        XCTAssertEqual([joined.w, joined.h], [640, 360])
        XCTAssertTrue(joined.hasAudio)
        let f0 = try await TestMedia.frame(res, at: 0.5), f1 = try await TestMedia.frame(res, at: 1.5), f2 = try await TestMedia.frame(res, at: 2.5)
        XCTAssertGreaterThan(TestMedia.rgb(f0, 320, 180).0, 0.6)
        XCTAssertGreaterThan(TestMedia.rgb(f1, 320, 180).1, 0.6, "the square clip in the middle")
        XCTAssertLessThan(TestMedia.rgb(f1, 40, 180).1, 0.15, "fitted with black bars")
        XCTAssertGreaterThan(TestMedia.rgb(f2, 320, 180).2, 0.6)
        // The sound holds its place across the silent clip: loud, quiet, loud.
        let levels = try await loudness(res, buckets: 3)
        XCTAssertGreaterThan(levels[0], 0.05)
        XCTAssertLessThan(levels[1], 0.01)
        XCTAssertGreaterThan(levels[2], 0.05)
    }

    // Average level per stretch of the audio.
    private func loudness(_ asset: AVAsset, buckets: Int) async throws -> [Double] {
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        let track = try XCTUnwrap(tracks.first)
        let dur = try await asset.load(.duration).seconds
        let r = try AVAssetReader(asset: asset)
        let o = AVAssetReaderTrackOutput(track: track, outputSettings: [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMBitDepthKey: 16,
                                                                      AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false, AVNumberOfChannelsKey: 1])
        r.add(o)
        r.startReading()
        var sums = [Double](repeating: 0, count: buckets), counts = [Double](repeating: 0, count: buckets)
        while let sb = o.copyNextSampleBuffer() {
            let t = sb.presentationTimeStamp.seconds
            guard let block = CMSampleBufferGetDataBuffer(sb) else { continue }
            var len = 0
            var ptr: UnsafeMutablePointer<CChar>?
            CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr)
            let n = len / 2, rate = Double(n) / max(1e-6, sb.duration.seconds)
            ptr!.withMemoryRebound(to: Int16.self, capacity: n) { p in
                for i in 0..<n {
                    let k = min(buckets - 1, max(0, Int((t + Double(i) / rate) / dur * Double(buckets))))
                    sums[k] += abs(Double(p[i]) / 32768)
                    counts[k] += 1
                }
            }
        }
        return zip(sums, counts).map { $1 > 0 ? $0 / $1 : 0 }
    }
}

// The editor itself: joining, splitting, reordering, the clip lane by mouse.
final class ClipEditorTests: XCTestCase {
    private func until(_ what: String, timeout: Double = 5, _ ok: @MainActor () -> Bool) async throws {
        let end = Date().addingTimeInterval(timeout)
        while Date() < end {
            if await MainActor.run(body: ok) { return }
            try await Task.sleep(nanoseconds: 30_000_000)
        }
        XCTFail("timed out: \(what)")
    }

    private func open(_ url: URL) async throws -> VideoEditor {
        await MainActor.run { _ = NSApplication.shared; VideoEditor.open(url) }
        try await until("editor loads") { VideoEditor.instances.last?.edit.clips.count == 1 }
        return await MainActor.run { VideoEditor.instances.last! }
    }

    @MainActor private func mouse(_ e: VideoEditor, _ type: NSEvent.EventType, _ p: CGPoint) {
        let tl = e.timeline
        let w = tl.convert(p, to: nil)
        let ev = NSEvent.mouseEvent(with: type, location: w, modifierFlags: [], timestamp: 0, windowNumber: e.window.windowNumber,
                                    context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
        switch type {
        case .leftMouseDown: tl.mouseDown(with: ev)
        case .leftMouseDragged: tl.mouseDragged(with: ev)
        default: tl.mouseUp(with: ev)
        }
    }

    @MainActor private func key(_ e: VideoEditor, _ code: Int, cmd: Bool = false) -> Bool {
        let ev = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: cmd ? [.command] : [], timestamp: 0, windowNumber: e.window.windowNumber,
                                  context: nil, characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: UInt16(code))!
        return e.key(ev)
    }

    // Port of video_editor_joins_splits_and_reorders_clips.
    func testEditorJoinsSplitsAndReordersClips() async throws {
        let caps = FileManager.default.temporaryDirectory.appendingPathComponent("ather-caps-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: caps, withIntermediateDirectories: true)
        let old = Settings.shared.string("SaveFolder")
        Settings.shared.set("SaveFolder", caps.path)
        defer { Settings.shared.set("SaveFolder", old) }
        let a = try await TestMedia.colors(w: 640, h: 360, seconds: 3, tone: true)
        let b = try await TestMedia.colors(w: 360, h: 360, seconds: 2, colors: [.yellow])
        let e = try await open(a)
        await MainActor.run { XCTAssertEqual(e.timeline.laneH, 0, "one video: no clip lane") }
        await MainActor.run { e.addClips([b]) }
        try await until("b joins") { e.edit.clips.count == 2 }
        await MainActor.run {
            XCTAssertGreaterThan(e.timeline.laneH, 0)
            XCTAssertEqual(e.duration, 5, accuracy: 0.1)
            XCTAssertEqual(e.edit.trimEnd, 5, accuracy: 0.1, "an untrimmed end follows the new total")
            XCTAssertEqual(e.edit.frame, CGSize(width: 640, height: 360))
            // A caption in b's footage, then split a: everything stays put.
            e.edit.captions = [Caption(start: 3.5, end: 4.0, text: "in b")]
        }
        try await until("rebuilt") { e.duration > 4.9 }
        await MainActor.run { e.seek(1.2) }
        try await until("seeked") { abs(e.now - 1.2) < 0.05 }
        await MainActor.run {
            e.splitAtPlayhead()
            XCTAssertEqual(e.edit.clips.count, 3)
            XCTAssertEqual(e.edit.clips[0].outPoint, e.edit.clips[1].inPoint, accuracy: 1e-9)
            XCTAssertEqual(e.edit.clips[0].source, e.edit.clips[1].source, "pieces of one video share their source")
            XCTAssertEqual(e.edit.captions[0].start, 3.5, accuracy: 1e-6)
            XCTAssertEqual(e.selectedClipIndex, 1, "the second half is selected")
            e.rebuildInspector()
        }
        // b goes first: its caption moves with it.
        await MainActor.run {
            e.moveClip(2, to: 0)
            XCTAssertEqual(e.edit.clips[0].path, b.path)
            XCTAssertEqual(e.edit.captions[0].start, 0.5, accuracy: 1e-6)
        }
        // Removing b takes its caption; undo brings both back; the last clip can't be removed.
        await MainActor.run {
            e.removeClip(0)
            XCTAssertEqual(e.edit.clips.count, 2)
            XCTAssertTrue(e.edit.captions.isEmpty)
            e.undo()
            XCTAssertEqual(e.edit.clips.count, 3)
            XCTAssertEqual(e.edit.captions.count, 1)
            e.removeClip(0); e.removeClip(0)
            XCTAssertEqual(e.edit.clips.count, 1)
            e.removeClip(0)
            XCTAssertEqual(e.edit.clips.count, 1, "the last clip stays")
            e.undo(); e.undo()
            XCTAssertEqual(e.edit.clips.count, 3)
        }
        try await until("rebuilt after undo") { abs(e.duration - 5) < 0.1 }
        // Saving joins them into one video, the original untouched.
        await MainActor.run { e.save(gif: false) }
        try await until("saved", timeout: 20) { Output.listCaptures().contains { $0.pathExtension == "mp4" } }
        try await Task.sleep(nanoseconds: 300_000_000)
        let saved = try XCTUnwrap(Output.listCaptures().first { $0.pathExtension == "mp4" })
        let v = try await VideoSource.probe(saved)
        XCTAssertEqual(v.length, 5, accuracy: 0.2)
        XCTAssertEqual([v.w, v.h], [640, 360])
        XCTAssertTrue(v.hasAudio)
        await MainActor.run {
            e.dirty = false
            e.window.close()
        }
    }

    private func twoClips() async throws -> VideoEditor {
        let a = try await TestMedia.colors(w: 640, h: 360, seconds: 3)
        let b = try await TestMedia.colors(w: 640, h: 360, seconds: 2, colors: [.yellow])
        let e = try await open(a)
        await MainActor.run {
            e.window.setContentSize(NSSize(width: 1100, height: 760))
            e.window.contentView?.layoutSubtreeIfNeeded()
            e.addClips([b])
        }
        try await until("b joins") { e.edit.clips.count == 2 && e.duration > 4.9 }
        await MainActor.run { e.window.contentView?.layoutSubtreeIfNeeded() }
        return e
    }

    // Port of video_editor_clip_lane_mouse: click selects, dragging a clip reorders, dragging a selected clip's
    // edge trims it (applied on mouse-up).
    func testClipLaneMouse() async throws {
        let e = try await twoClips()
        await MainActor.run {
            let tl = e.timeline, y = tl.laneH / 2
            let starts = VideoSequence.starts(e.edit.clips)
            let mid1 = tl.x(starts[1] + e.edit.clips[1].duration / 2)
            mouse(e, .leftMouseDown, CGPoint(x: mid1, y: y))
            XCTAssertEqual(e.selectedClipIndex, 1, "click selects")
            mouse(e, .leftMouseDragged, CGPoint(x: mid1 - 40, y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(0.2), y: y))
            XCTAssertEqual(e.edit.clips[0].duration, 3, accuracy: 1e-6, "nothing moves until release")
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(0.2), y: y))
            XCTAssertEqual(e.edit.clips[0].duration, 2, accuracy: 1e-6, "dragged in front")
            XCTAssertEqual(e.selectedClipIndex, 0)
            // Its right edge, dragged left, trims it.
            let edge = tl.x(e.edit.clips[0].duration)
            mouse(e, .leftMouseDown, CGPoint(x: edge, y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(e.edit.clips[0].duration - 0.5), y: y))
            XCTAssertEqual(e.edit.clips[0].duration, 2, accuracy: 1e-6)
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(1.5), y: y))
            XCTAssertEqual(e.edit.clips[0].duration, 1.5, accuracy: 0.05, "trimmed on release")
            e.dirty = false
            e.window.close()
        }
    }

    // Port of video_editor_clip_lane_edges_knob_and_keys: a selected clip's left edge trims it (not its
    // neighbour), the knob scrubs over the lane, keys can't change clips mid-drag, and a click on a very short
    // clip's edge leaves it alone.
    func testClipLaneEdgesKnobAndKeys() async throws {
        let e = try await twoClips()
        await MainActor.run {
            let tl = e.timeline, y = tl.laneH / 2
            e.selected = e.edit.clips[1].id
            let edge = tl.x(VideoSequence.starts(e.edit.clips)[1])
            mouse(e, .leftMouseDown, CGPoint(x: edge + 2, y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(VideoSequence.starts(e.edit.clips)[1] + 0.5), y: y))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(VideoSequence.starts(e.edit.clips)[1] + 0.5), y: y))
            XCTAssertEqual(e.edit.clips[0].duration, 3, accuracy: 1e-6, "the neighbour is untouched")
            XCTAssertEqual(e.edit.clips[1].inPoint, 0.5, accuracy: 0.05, "the selected clip's left edge trimmed it")
        }
        // The knob scrubs, even over the lane.
        await MainActor.run { e.seek(1) }
        try await until("at 1 s") { abs(e.now - 1) < 0.05 }
        await MainActor.run {
            let tl = e.timeline
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(1), y: 2))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(2), y: 2))
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(2), y: 2))
            XCTAssertEqual(e.edit.clips.count, 2, "no clip drag")
        }
        try await until("scrubbed to 2 s") { abs(e.now - 2) < 0.1 }
        // Mid-drag, Delete and ⌘Z do nothing; Esc cancels the drag.
        await MainActor.run {
            let tl = e.timeline, y = tl.laneH / 2
            let order = e.edit.clips.map(\.id)
            mouse(e, .leftMouseDown, CGPoint(x: tl.x(0.5), y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(0.5) + 40, y: y))
            mouse(e, .leftMouseDragged, CGPoint(x: tl.x(4.4), y: y))
            XCTAssertTrue(key(e, kVK_Delete))
            XCTAssertTrue(key(e, kVK_ANSI_Z, cmd: true))
            XCTAssertEqual(e.edit.clips.count, 2)
            XCTAssertTrue(key(e, kVK_Escape))
            XCTAssertFalse(tl.isDragging)
            mouse(e, .leftMouseUp, CGPoint(x: tl.x(4.4), y: y))
            XCTAssertEqual(e.edit.clips.map(\.id), order, "the cancelled drag didn't reorder")
            XCTAssertTrue(e.window.isVisible, "Esc only cancelled the drag")
        }
        // A very short clip: a click on its edge leaves it alone.
        await MainActor.run {
            var clips = e.edit.clips
            clips[1].outPoint = clips[1].inPoint + 0.1
            e.setClips(clips)
        }
        try await until("short clip") { e.edit.clips[1].duration < 0.11 && e.duration < 3.2 }
        await MainActor.run {
            let tl = e.timeline, y = tl.laneH / 2
            e.selected = e.edit.clips[1].id
            let right = tl.x(e.duration)
            let before = e.edit.clips[1]
            mouse(e, .leftMouseDown, CGPoint(x: right - 1, y: y))
            mouse(e, .leftMouseUp, CGPoint(x: right - 1, y: y))
            XCTAssertEqual(e.edit.clips[1].inPoint, before.inPoint, accuracy: 1e-9)
            XCTAssertEqual(e.edit.clips[1].outPoint, before.outPoint, accuracy: 1e-9, "not pushed past its end")
            e.dirty = false
            e.window.close()
        }
    }

    func testClipLaneSnapshot() async throws {
        guard let out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] else { throw XCTSkip("set ATHER_TEST_OUT") }
        let e = try await twoClips()
        try await Task.sleep(nanoseconds: 800_000_000)  // thumbnails
        await MainActor.run {
            e.selected = e.edit.clips[1].id
            e.edit.captions = [Caption(start: 3.4, end: 4.4, text: "In the second clip")]
            e.window.contentView?.layoutSubtreeIfNeeded()
            let v = e.window.contentView!
            let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)!
            v.cacheDisplay(in: v.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out).appendingPathComponent("ui-video-clips.png"))
            e.dirty = false
            e.window.close()
        }
    }
}
