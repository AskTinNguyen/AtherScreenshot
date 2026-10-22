import AppKit
import Carbon.HIToolbox
import XCTest
@testable import AtherScreenshot

// The capture tray (docs/intent/capture-tray, A6): captures collect in one editor, each picture keeps its own
// document, Save selected and Done save the ticked ones, closing asks first. Editors stay offscreen, copies go to a
// pasteboard of the test's own and notes to a library of its own; the prompts are answered by the test.
final class TrayTests: XCTestCase {
    private var pb: NSPasteboard!
    private var lib: Library!

    override func setUp() {
        _ = NSApplication.shared
        Editor.keepOffscreen = true
        pb = NSPasteboard(name: NSPasteboard.Name("ather-tray-test-\(UUID().uuidString)"))
        lib = Library(persists: false)
    }

    override func tearDown() {
        for e in Editor.instances { e.window.close() }   // close() asks nothing
        Editor.instances.removeAll()
        Editor.keepOffscreen = false
        pb.releaseGlobally()
    }

    // MARK: helpers

    private func solid(_ w: Int, _ h: Int, _ c: NSColor) -> CGImage {
        let ctx = makeContext(width: w, height: h)!
        ctx.setFillColor(c.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage()!
    }

    // A window opened for a capture, laid out like a window on a 1300 × 860 screen.
    private func capture(_ img: CGImage, app: String = "Notes", tray: Bool = true) -> Editor {
        let e = Editor.capture(img, scale: 1, info: NameInfo(app: app, window: app + " window"), tray: tray)
        e.pasteboard = pb
        e.library = lib
        e.window.setContentSize(NSSize(width: 1300, height: 860))
        e.window.contentView?.layoutSubtreeIfNeeded()
        return e
    }

    // Image point → window point, through the canvas's view transform.
    private func windowPoint(_ e: Editor, _ p: CGPoint) -> CGPoint {
        let c = e.canvas, z = c.zoom, o = c.origin, v = c.visible
        return c.convert(CGPoint(x: o.x + (p.x - v.minX) * z, y: o.y + (p.y - v.minY) * z), to: nil)
    }

    private func mouse(_ e: Editor, _ type: NSEvent.EventType, _ p: CGPoint) -> NSEvent {
        NSEvent.mouseEvent(with: type, location: windowPoint(e, p), modifierFlags: [], timestamp: 0, windowNumber: e.window.windowNumber,
                           context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
    }

    // A drag with the mouse from a to b (image coordinates) with a tool.
    private func drag(_ e: Editor, _ t: Tool, _ a: CGPoint, _ b: CGPoint) {
        e.canvas.setTool(t)
        e.canvas.mouseDown(with: mouse(e, .leftMouseDown, a))
        e.canvas.mouseDragged(with: mouse(e, .leftMouseDragged, b))
        e.canvas.mouseUp(with: mouse(e, .leftMouseUp, b))
    }

    private func key(_ e: Editor, _ code: Int, _ flags: NSEvent.ModifierFlags = [], _ chars: String = "") {
        let ev = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0, windowNumber: e.window.windowNumber,
                                  context: nil, characters: chars, charactersIgnoringModifiers: chars, isARepeat: false, keyCode: UInt16(code))!
        e.canvas.keyDown(with: ev)
    }

    // Pixels in one fixed format, to compare pictures exactly.
    private func pixels(_ img: CGImage) -> Data {
        let ctx = makeContext(width: img.width, height: img.height, flipped: false)!
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        return Data(bytes: ctx.data!, count: ctx.bytesPerRow * img.height)
    }

    private func savedFiles() -> Set<URL> { Set(Output.listCaptures()) }

    private func waitFor(_ what: String, _ cond: () -> Bool) {
        let end = Date().addingTimeInterval(15)
        while !cond() && Date() < end { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
        XCTAssertTrue(cond(), what)
    }

    private func apps(_ urls: Set<URL>) -> [String] {
        lib.apply(urls.map { ($0, 1, 1) })
        return urls.map { lib.meta($0).app }.sorted()
    }

    // MARK: A1: routing

    func testCapturesJoinTheOpenEditor() throws {
        let a = capture(solid(400, 300, .white))
        let windows = Editor.instances.count
        XCTAssertTrue(a.takesCaptures)
        XCTAssertFalse(a.isTray)
        XCTAssertTrue(a.strip.isHidden)
        let b = Editor.capture(solid(320, 200, .systemBlue), scale: 1, info: NameInfo(app: "Paint"), tray: true)
        XCTAssertTrue(a === b, "joins the open editor")
        XCTAssertEqual(Editor.instances.count, windows)
        XCTAssertEqual(a.tray.count, 2)
        XCTAssertEqual(a.current, 1)
        XCTAssertEqual(a.base.width, 320)
        XCTAssertTrue(a.isTray)
        XCTAssertFalse(a.strip.isHidden)

        // A newer collage and a newer window on a saved file: captures still go to the capture window.
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-tray-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let u1 = dir.appendingPathComponent("one.png"), u2 = dir.appendingPathComponent("two.png")
        try solid(200, 100, .systemRed).pngData()!.write(to: u1)
        try solid(100, 200, .systemGreen).pngData()!.write(to: u2)
        let c = try XCTUnwrap(Editor.openCollage([u1, u2]))
        Editor.open(url: u1)
        let f = try XCTUnwrap(Editor.instances.last)
        XCTAssertFalse(f === a || f === c)
        XCTAssertFalse(f.takesCaptures)
        XCTAssertTrue(Editor.capture(solid(64, 48, .systemRed), scale: 1, info: NameInfo(), tray: true) === a)
        XCTAssertEqual(a.tray.count, 3)
        XCTAssertEqual(c.tray.count, 1)
        XCTAssertEqual(f.tray.count, 1)
        XCTAssertEqual(Editor.instances.count, windows + 2)

        // Trays off: one window per capture, as before.
        let own = Editor.capture(solid(64, 48, .systemGreen), scale: 1, info: NameInfo(), tray: false)
        XCTAssertFalse(own === a)
        XCTAssertEqual(a.tray.count, 3)
        XCTAssertEqual(Editor.instances.count, windows + 3)
    }

    // MARK: A2: each picture keeps its document

    func testEachPictureKeepsItsDocument() {
        let e = capture(solid(400, 300, .white))
        drag(e, .rect, CGPoint(x: 40, y: 40), CGPoint(x: 200, y: 160))
        drag(e, .arrow, CGPoint(x: 300, y: 250), CGPoint(x: 220, y: 120))
        XCTAssertEqual(e.canvas.state.annots.count, 2)
        XCTAssertTrue(e.dirty)
        let pixels1 = pixels(e.export()), undo1 = e.canvas.undoCount

        XCTAssertTrue(e.add(solid(500, 260, .systemBlue), info: NameInfo(app: "Paint")))
        XCTAssertEqual(e.current, 1)
        XCTAssertTrue(e.canvas.state.annots.isEmpty)
        XCTAssertEqual(e.canvas.undoCount, 0)
        XCTAssertFalse(e.dirty)
        drag(e, .ellipse, CGPoint(x: 50, y: 50), CGPoint(x: 250, y: 200))
        drag(e, .crop, CGPoint(x: 20, y: 10), CGPoint(x: 420, y: 230))
        XCTAssertEqual(Render.frame(e.base, e.canvas.state).width, 400, accuracy: 2)
        e.canvas.extend(.below)
        let frame2 = Render.frame(e.base, e.canvas.state)
        XCTAssertGreaterThan(frame2.height, 230)
        let pixels2 = pixels(e.export()), undo2 = e.canvas.undoCount
        XCTAssertEqual(undo2, 3)
        e.canvas.setColor(3)
        e.canvas.setLevel(4)

        key(e, kVK_Tab, [.control, .shift], "\t")   // ⌃⇧⇥: the previous picture
        XCTAssertEqual(e.current, 0)
        XCTAssertEqual(pixels(e.export()), pixels1)
        XCTAssertEqual(e.canvas.undoCount, undo1)
        XCTAssertEqual(Render.frame(e.base, e.canvas.state).size, CGSize(width: 400, height: 300))
        XCTAssertEqual(e.canvas.color, 3)   // tool, color and size stay with the window
        XCTAssertEqual(e.canvas.level, 4)
        key(e, kVK_ANSI_Z, [.command], "z")  // undo: this picture's last step, the arrow
        XCTAssertEqual(e.canvas.state.annots.count, 1)
        XCTAssertEqual(e.canvas.state.annots.first?.tool, .rect)

        // A text box being typed when switching away goes into its own picture.
        e.canvas.setTool(.text)
        e.canvas.mouseDown(with: mouse(e, .leftMouseDown, CGPoint(x: 60, y: 220)))
        key(e, kVK_ANSI_H, [], "H")
        key(e, kVK_ANSI_I, [], "i")
        key(e, kVK_Tab, [.control], "\t")   // ⌃⇥: the next picture
        XCTAssertEqual(e.current, 1)
        XCTAssertEqual(pixels(e.export()), pixels2)
        XCTAssertEqual(e.canvas.undoCount, undo2)
        XCTAssertEqual(Render.frame(e.base, e.canvas.state), frame2)
        key(e, kVK_ANSI_Z, [.command], "z")  // undoes the space below, nothing of the first picture
        XCTAssertLessThan(Render.frame(e.base, e.canvas.state).height, frame2.height)
        key(e, kVK_ANSI_LeftBracket, [.command, .shift], "{")   // ⌘{
        XCTAssertEqual(e.current, 0)
        XCTAssertEqual(e.canvas.state.annots.count, 2)
        XCTAssertEqual(e.canvas.state.annots.last?.text, "Hi")
        key(e, kVK_ANSI_RightBracket, [.command, .shift], "}")   // ⌘}
        XCTAssertEqual(e.current, 1)
    }

    // MARK: A3: Save selected, Done and the close prompt

    func testSaveSelectedAndDone() throws {
        let e = capture(solid(400, 300, .white), app: "Notes")
        XCTAssertTrue(e.add(solid(320, 240, .systemBlue), info: NameInfo(app: "Paint", window: "Untitled")))
        XCTAssertTrue(e.add(solid(360, 200, .systemOrange), info: NameInfo(app: "Numbers", window: "Book1")))
        drag(e, .arrow, CGPoint(x: 300, y: 30), CGPoint(x: 80, y: 150))
        e.select(0)
        drag(e, .rect, CGPoint(x: 40, y: 40), CGPoint(x: 200, y: 160))
        XCTAssertTrue(e.tray.allSatisfy(\.keep), "new captures come ticked")
        e.strip.click(CGPoint(x: e.strip.tickRect(1).midX, y: e.strip.tickRect(1).midY))   // untick the second
        XCTAssertFalse(e.tray[1].keep)
        XCTAssertEqual(e.toSaveCount, 2)

        e.select(2)   // the first isn't current while Save selected writes it
        let before = savedFiles()
        e.strip.click(CGPoint(x: e.strip.saveRect.midX, y: e.strip.saveRect.midY))
        waitFor("two files and their badges") { savedFiles().count == before.count + 2 && e.tray[0].saved && e.tray[2].saved }
        let first = savedFiles().subtracting(before)
        XCTAssertEqual(apps(first), ["Notes", "Numbers"])
        let notes = try XCTUnwrap(first.first { lib.meta($0).app == "Notes" })
        XCTAssertEqual(lib.meta(notes).window, "Notes window")
        XCTAssertFalse(e.isDirty(0))
        XCTAssertFalse(e.isDirty(2))
        XCTAssertFalse(e.tray[1].saved, "unticked: untouched")
        XCTAssertEqual(e.toSaveCount, 0)

        // ⌘S on the first writes exactly what Save selected wrote for it.
        e.select(0)
        let mid = savedFiles()
        key(e, kVK_ANSI_S, [.command], "s")
        waitFor("one more file") { savedFiles().count == mid.count + 1 }
        let again = try XCTUnwrap(savedFiles().subtracting(mid).first)
        let a = try XCTUnwrap(CGImage.load(notes)), b = try XCTUnwrap(CGImage.load(again))
        XCTAssertEqual(pixels(a), pixels(b))

        // Edited again: dirty again.
        drag(e, .ellipse, CGPoint(x: 250, y: 60), CGPoint(x: 380, y: 200))
        XCTAssertTrue(e.isDirty(0))
        XCTAssertEqual(e.toSaveCount, 1)

        // Done: the second isn't ticked and was never saved, so it asks first; Cancel changes nothing.
        var asked: (Int, Bool)?
        e.answerClose = { n, done in asked = (n, done); return .cancel }
        key(e, kVK_Return, [], "\r")
        XCTAssertEqual(asked?.0, 1)
        XCTAssertEqual(asked?.1, true)
        XCTAssertTrue(Editor.instances.contains { $0 === e })
        XCTAssertNil(pb.data(forType: .png))
        let current = pixels(e.export())
        e.answerClose = { _, _ in .save }
        let last = savedFiles()
        key(e, kVK_Return, [], "\r")
        XCTAssertFalse(Editor.instances.contains { $0 === e }, "closed")
        waitFor("the first saved again") { savedFiles().count == last.count + 1 }
        let copied = try XCTUnwrap(pb.data(forType: .png).flatMap { NSBitmapImageRep(data: $0)?.cgImage })
        XCTAssertEqual(pixels(copied), current, "the current picture is on the clipboard")
    }

    func testClosePromptAnswers() {
        func make() -> Editor {
            let e = capture(solid(400, 300, .white), app: "Notes")
            e.add(solid(320, 240, .systemBlue), info: NameInfo(app: "Paint"))
            e.add(solid(360, 200, .systemOrange), info: NameInfo(app: "Numbers"))
            e.toggleKeep(1)
            return e
        }
        var asked = -1
        var answer = CloseAnswer.cancel
        let start = savedFiles()

        var e = make()
        e.answerClose = { n, _ in asked = n; return answer }
        XCTAssertFalse(e.windowShouldClose(e.window))   // Cancel
        XCTAssertEqual(asked, 3)
        XCTAssertTrue(Editor.instances.contains { $0 === e })
        answer = .discard
        _ = e.windowShouldClose(e.window)   // Close without saving
        XCTAssertFalse(Editor.instances.contains { $0 === e })
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(savedFiles(), start)

        e = make()
        e.answerClose = { n, _ in asked = n; return answer }
        answer = .save
        asked = -1
        _ = e.windowShouldClose(e.window)   // Save selected and close
        XCTAssertEqual(asked, 3)
        XCTAssertFalse(Editor.instances.contains { $0 === e })
        waitFor("the ticked two") { savedFiles().count == start.count + 2 }
        XCTAssertEqual(apps(savedFiles().subtracting(start)), ["Notes", "Numbers"])

        // Nothing to lose: no question.
        e = make()
        e.answerClose = { n, _ in asked = n; return answer }
        asked = -1
        e.toggleKeep(1)
        e.saveSelected(quiet: true)
        waitFor("all saved") { e.tray.allSatisfy(\.saved) }
        XCTAssertTrue(e.windowShouldClose(e.window))
        XCTAssertEqual(asked, -1)
    }

    // MARK: A4: the strip

    func testStripClicksKeysAndWheel() {
        let e = capture(solid(400, 300, .white))
        let canvasOne = e.canvas.frame.height
        XCTAssertTrue(e.strip.isHidden)
        XCTAssertEqual(e.strip.frame.height, 0)   // one picture: exactly as before
        key(e, kVK_Tab, [.control], "\t")
        XCTAssertEqual(e.current, 0)

        XCTAssertTrue(e.add(solid(320, 240, .systemBlue)))
        e.window.contentView?.layoutSubtreeIfNeeded()
        XCTAssertFalse(e.strip.isHidden)
        XCTAssertEqual(e.strip.frame.height, TrayStrip.height)
        XCTAssertEqual(e.canvas.frame.height, canvasOne - TrayStrip.height, accuracy: 0.5)
        let mid = { (r: CGRect) in CGPoint(x: r.midX, y: r.midY) }
        e.strip.click(mid(e.strip.thumbRect(0)))
        XCTAssertEqual(e.current, 0)
        e.strip.click(mid(e.strip.tickRect(1)))
        XCTAssertFalse(e.tray[1].keep)
        e.strip.click(mid(e.strip.tickRect(1)))
        XCTAssertTrue(e.tray[1].keep)

        XCTAssertTrue(e.add(solid(200, 400, .systemOrange)))
        XCTAssertEqual(e.current, 2)
        key(e, kVK_Tab, [.control], "\t")
        XCTAssertEqual(e.current, 0)
        key(e, kVK_Tab, [.control, .shift], "\t")
        XCTAssertEqual(e.current, 2)

        // ×: a picture without changes goes at once; one with changes asks first.
        XCTAssertEqual(e.strip.hit(mid(e.strip.removeRect(0)))?.part, .remove)
        e.strip.click(mid(e.strip.removeRect(0)))
        XCTAssertEqual(e.tray.count, 2)
        XCTAssertEqual(e.current, 1)
        drag(e, .rect, CGPoint(x: 20, y: 20), CGPoint(x: 150, y: 150))
        var asks = 0, discard = false
        e.answerDiscard = { asks += 1; return discard }
        e.strip.click(mid(e.strip.removeRect(1)))
        XCTAssertEqual(asks, 1)
        XCTAssertEqual(e.tray.count, 2)
        discard = true
        e.strip.click(mid(e.strip.removeRect(1)))
        XCTAssertEqual(asks, 2)
        XCTAssertEqual(e.tray.count, 1)
        XCTAssertEqual(e.base.width, 320)
        XCTAssertTrue(e.strip.isHidden)   // one picture left: no strip

        // Twenty pictures overflow the strip: the newest is scrolled into view, and the wheel scrolls it sideways.
        for i in 0..<19 { e.add(solid(160, 90, NSColor(hue: CGFloat(i) / 19, saturation: 0.7, brightness: 0.9, alpha: 1))) }
        e.window.contentView?.layoutSubtreeIfNeeded()
        XCTAssertEqual(e.tray.count, 20)
        XCTAssertGreaterThan(e.strip.scroll, 0)
        XCTAssertLessThanOrEqual(e.strip.boxRect(19).maxX, e.strip.bounds.width - TrayStrip.panel)
        e.strip.scrollBy(-10_000)
        XCTAssertEqual(e.strip.scroll, 0)
        let level = e.canvas.level
        let wheel = { (lines: Int32) in NSEvent(cgEvent: CGEvent(scrollWheelEvent2Source: nil, units: .line, wheelCount: 1, wheel1: lines, wheel2: 0, wheel3: 0)!)! }
        e.strip.scrollWheel(with: wheel(-1))
        XCTAssertGreaterThan(e.strip.scroll, 0)
        e.strip.scrollWheel(with: wheel(5))
        XCTAssertEqual(e.strip.scroll, 0)
        XCTAssertEqual(e.canvas.level, level, "the strip's wheel doesn't change the size")
    }

    func testTrayHoldsAtMost100Pictures() {
        let e = capture(solid(64, 48, .white))
        for i in 1..<Editor.trayMax { e.add(solid(64, 48, NSColor(white: CGFloat(i) / 100, alpha: 1))) }
        XCTAssertEqual(e.tray.count, Editor.trayMax)
        XCTAssertFalse(e.add(solid(64, 48, .red)))
        let windows = Editor.instances.count
        let other = Editor.capture(solid(64, 48, .red), scale: 1, info: NameInfo(), tray: true)
        XCTAssertFalse(other === e, "the 101st opens a window of its own")
        XCTAssertEqual(Editor.instances.count, windows + 1)
        XCTAssertEqual(e.tray.count, Editor.trayMax)
    }

    // 40 captures of 2560 × 1440 in one tray stay under about 1.5 GB: pictures that aren't current keep their
    // screenshot and a thumbnail, nothing composed. Random pixels, so the memory compressor can't hide them.
    func testTrayMemoryFor40Captures() {
        func noise() -> CGImage {
            let ctx = makeContext(width: 2560, height: 1440)!
            arc4random_buf(ctx.data!, ctx.bytesPerRow * 1440)
            return ctx.makeImage()!
        }
        func footprint() -> Double {
            var info = task_vm_info_data_t()
            var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
            let kr = withUnsafeMutablePointer(to: &info) {
                $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
            }
            return kr == KERN_SUCCESS ? Double(info.phys_footprint) : 0
        }
        let before = footprint()
        let e = capture(noise())
        for _ in 1..<40 { XCTAssertTrue(e.add(noise())) }
        let after = footprint(), raw = 40.0 * 2560 * 1440 * 4
        print(String(format: "TrayTests: 40 pictures of 2560 x 1440 in one tray: footprint %.0f MB, %.0f MB more than before", after / 1_048_576, (after - before) / 1_048_576))
        XCTAssertEqual(e.tray.count, 40)
        XCTAssertLessThan(after, 1.5e9)
        // The measure sees the screenshots. (Not the growth: memory earlier tests freed is reused, so it can be smaller.)
        XCTAssertGreaterThan(after, raw * 0.9, "the measure sees the screenshots")
        XCTAssertEqual(e.tray.filter { $0.thumb != nil }.count, 39)
    }
}
