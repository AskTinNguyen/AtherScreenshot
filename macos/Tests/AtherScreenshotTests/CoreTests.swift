import AppKit
import XCTest
@testable import AtherScreenshot

final class CoreTests: XCTestCase {
    func testHotkeyRoundTrip() {
        let hk = Hotkey.parse("ctrl+alt+shift+K")!
        XCTAssertEqual(hk.text, "Ctrl+Alt+Shift+K")
        XCTAssertEqual(hk.display, "⌃⌥⇧K")
        XCTAssertEqual(Hotkey.parse("Cmd+F5")?.isFunctionKey, true)
        XCTAssertNil(Hotkey.parse("Ctrl+Alt"))
        XCTAssertNil(Hotkey.parse("Ctrl+Nope"))
        for c in kCmds where !c.hotkey.isEmpty { XCTAssertNotNil(Hotkey.parse(c.hotkey), c.hotkey) }
    }

    func testDefaultHotkeysAreUnique() {
        let keys = kCmds.map(\.hotkey).filter { !$0.isEmpty }
        XCTAssertEqual(Set(keys).count, keys.count)
    }

    func testSanitizeAndUniqueNames() throws {
        XCTAssertEqual(Output.sanitize(" a/b:c. "), "a_b_c")
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try Data().write(to: dir.appendingPathComponent("x.png"))
        XCTAssertEqual(Output.uniqueURL(dir, "x", "png").lastPathComponent, "x (2).png")
        let u = Output.makeCaptureURL(base: dir, ext: "png", info: NameInfo(app: "Safari", window: "Hi", w: 10, h: 20))
        XCTAssertTrue(u.lastPathComponent.hasPrefix("Ather_"))
        XCTAssertTrue(u.deletingLastPathComponent().lastPathComponent.contains("-"))  // yyyy-MM subfolder
    }

    func testJsonPath() {
        let d = #"{"data":{"link":"https://i.imgur.com/x.png"},"files":[{"url":"a"},{"url":"b"}],"n":3}"#.data(using: .utf8)!
        XCTAssertEqual(Upload.jsonPath(d, "data.link"), "https://i.imgur.com/x.png")
        XCTAssertEqual(Upload.jsonPath(d, "files.1.url"), "b")
        XCTAssertEqual(Upload.jsonPath(d, "n"), "3")
        XCTAssertNil(Upload.jsonPath(d, "files.5.url"))
        XCTAssertEqual(Upload.uriEncode("2026-10/a b.png", keepSlash: true), "2026-10/a%20b.png")
    }

    private func words(_ line: [String], _ n: Int = 0) -> [OcrWord] {
        line.enumerated().map { i, t in OcrWord(text: t, rect: CGRect(x: i * 100, y: n * 30, width: 90, height: 20), line: n) }
    }

    func testFindSensitive() {
        XCTAssertEqual(OCR.findSensitive(words(["mail", "me:", "jane.doe@example.com"])).count, 1)
        XCTAssertEqual(OCR.findSensitive(words(["server", "10.0.12.255", "ok"])).count, 1)
        XCTAssertEqual(OCR.findSensitive(words(["card", "4111", "1111", "1111", "1111"])).count, 1)  // Luhn-valid, merged into one box
        XCTAssertEqual(OCR.findSensitive(words(["order", "1234", "5678"])).count, 0)  // too short for a phone or card number
        XCTAssertEqual(OCR.findSensitive(words(["key", "sk-ant-api03-abcdefghij"])).count, 1)
        XCTAssertEqual(OCR.findSensitive(words(["just", "some", "ordinary", "words", "2026"])).count, 0)
        XCTAssertTrue(OCR.looksLikeSecret("Ab3dEf9hIjKl2nOpQrStUv"))
        XCTAssertFalse(OCR.looksLikeSecret("internationalization"))
    }

    private func textImage(_ text: String, size: CGSize = CGSize(width: 900, height: 160)) -> CGImage {
        let ctx = makeContext(width: Int(size.width), height: Int(size.height))!
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fill(CGRect(origin: .zero, size: size))
        withNSContext(ctx, flipped: true) {
            NSAttributedString(string: text, attributes: [.font: NSFont.systemFont(ofSize: 36), .foregroundColor: NSColor.black])
                .draw(at: CGPoint(x: 20, y: 50))
        }
        return ctx.makeImage()!
    }

    func testVisionOcrAndRedaction() throws {
        let img = textImage("Contact jane.doe@example.com today")
        let text = try OCR.text(img)
        XCTAssertTrue(text.contains("example.com"), text)
        let rects = OCR.findSensitive(try OCR.words(img))
        XCTAssertEqual(rects.count, 1)
        let r = rects[0]
        XCTAssertGreaterThan(r.minX, 120)  // the box covers the email, not "Contact"
        XCTAssertTrue(r.minY < 90 && r.maxY > 60)
        let red = OCR.pixelate(img, rects: rects)
        XCTAssertEqual(red.width, img.width)
        XCTAssertFalse(try OCR.text(red).contains("example.com"))
    }

    func testComposeCropAndStyled() {
        let base = textImage("Hello")
        var st = DocState()
        st.annots = [
            Annot(tool: .arrow, color: 0, level: 2, unit: 1, pts: [CGPoint(x: 10, y: 10), CGPoint(x: 200, y: 100)]),
            Annot(tool: .blur, color: 0, level: 2, unit: 1, pts: [CGPoint(x: 0, y: 0), CGPoint(x: 300, y: 160)]),
            Annot(tool: .magnify, color: 0, level: 1, unit: 1, pts: [CGPoint(x: 60, y: 70), CGPoint(x: 600, y: 80)]),
            Annot(tool: .text, color: 3, level: 1, unit: 1, pts: [CGPoint(x: 400, y: 20)], text: "Note"),
            Annot(tool: .step, color: 4, level: 1, unit: 1, pts: [CGPoint(x: 700, y: 40)], step: 1),
            Annot(tool: .spotlight, color: 0, level: 1, unit: 1, pts: [CGPoint(x: 380, y: 0), CGPoint(x: 900, y: 160)]),
        ]
        let full = Render.compose(base, st)
        XCTAssertEqual(full.width, 900)
        st.crop = CGRect(x: 100, y: 20, width: 400, height: 100)
        let cropped = Render.compose(base, st)
        XCTAssertEqual(cropped.width, 400)
        XCTAssertEqual(cropped.height, 100)
        let styled = Render.styled(cropped, unit: 1)
        XCTAssertGreaterThan(styled.width, 400)
        // Spotlight dims outside its area: a pixel at the far left is darker than a white one.
        let left = full.color(atPixel: CGPoint(x: 350, y: 150))!.usingColorSpace(.sRGB)!
        XCTAssertLessThan(left.redComponent, 0.6)
        if let dir = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] {
            try? full.pngData()?.write(to: URL(fileURLWithPath: dir).appendingPathComponent("compose.png"))
            try? styled.pngData()?.write(to: URL(fileURLWithPath: dir).appendingPathComponent("styled.png"))
        }
    }

    func testHitTesting() {
        let line = Annot(tool: .line, color: 0, level: 1, unit: 1, pts: [CGPoint(x: 0, y: 0), CGPoint(x: 100, y: 100)])
        XCTAssertTrue(Render.hit(line, CGPoint(x: 50, y: 52), slop: 4))
        XCTAssertFalse(Render.hit(line, CGPoint(x: 80, y: 20), slop: 4))
        let rect = Annot(tool: .rect, color: 0, level: 1, unit: 1, pts: [CGPoint(x: 0, y: 0), CGPoint(x: 100, y: 100)])
        XCTAssertTrue(Render.hit(rect, CGPoint(x: 0, y: 50), slop: 4))
        XCTAssertFalse(Render.hit(rect, CGPoint(x: 50, y: 50), slop: 4))  // hollow
    }

    func testSnapshotCropAcrossDisplays() {
        func solid(_ w: Int, _ h: Int, _ c: NSColor) -> CGImage {
            let ctx = makeContext(width: w, height: h)!
            ctx.setFillColor(c.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
            return ctx.makeImage()!
        }
        // A Retina display at the origin and a 1× display to its right.
        let snap = Snapshot([
            DisplayShot(displayID: 1, frame: CGRect(x: 0, y: 0, width: 100, height: 100), image: solid(200, 200, .red)),
            DisplayShot(displayID: 2, frame: CGRect(x: 100, y: 0, width: 100, height: 100), image: solid(100, 100, .blue)),
        ])
        let one = snap.crop(CGRect(x: 10, y: 10, width: 20, height: 20))!
        XCTAssertEqual(one.width, 40)
        let both = snap.crop(CGRect(x: 90, y: 0, width: 20, height: 10))!
        XCTAssertEqual(both.width, 40)  // composed at the highest scale
        XCTAssertEqual(both.color(atPixel: CGPoint(x: 2, y: 2))?.hex, "#FF0000")
        XCTAssertEqual(both.color(atPixel: CGPoint(x: 38, y: 2))?.hex, "#0000FF")
    }
}
