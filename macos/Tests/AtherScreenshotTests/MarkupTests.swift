import AppKit
import CoreImage
import XCTest
@testable import AtherScreenshot

// Video markup through the frame renderer the preview and the export share.
final class MarkupTests: XCTestCase {
    private let size = CGSize(width: 640, height: 360)

    // Left half red, right half blue, a white bar at x 300–340.
    private var frame: CIImage {
        let ctx = makeContext(width: 640, height: 360)!
        ctx.setFillColor(NSColor.red.cgColor); ctx.fill(CGRect(x: 0, y: 0, width: 320, height: 360))
        ctx.setFillColor(NSColor.blue.cgColor); ctx.fill(CGRect(x: 320, y: 0, width: 320, height: 360))
        ctx.setFillColor(NSColor.white.cgColor); ctx.fill(CGRect(x: 300, y: 0, width: 40, height: 360))
        return CIImage(cgImage: ctx.makeImage()!)
    }

    private func render(_ e: VideoEdit, _ t: Double, preview: Bool = false, zoom: Bool = false) -> CGImage {
        let r = FrameRenderer(edit: e, full: size, preview: preview)
        r.zoomInPreview = zoom
        return sharedCIContext.createCGImage(r.render(frame, at: t), from: CGRect(origin: .zero, size: r.out))!
    }

    private func px(_ img: CGImage, _ x: Int, _ y: Int) -> NSColor { img.color(atPixel: CGPoint(x: x, y: y))!.usingColorSpace(.sRGB)! }

    private func edit(_ marks: [Mark]) -> VideoEdit {
        var e = VideoEdit(trimEnd: 10)
        e.marks = marks
        return e
    }

    private func mark(_ k: MarkKind, _ a: CGPoint, _ b: CGPoint, color: Int = 0, text: String = "", anim: MarkAnimation = .none) -> Mark {
        Mark(kind: k, start: 1, end: 3, a: a, b: b, text: text, color: color, animation: anim)
    }

    func testBlurOnlyWhileActive() {
        let e = edit([mark(.blur, CGPoint(x: 260, y: 0), CGPoint(x: 380, y: 360))])
        XCTAssertLessThan(px(render(e, 0.5), 296, 180).greenComponent, 0.05)     // before: pure red
        XCTAssertGreaterThan(px(render(e, 2), 296, 180).greenComponent, 0.15)    // during: red mixed with the white bar
        XCTAssertLessThan(px(render(e, 3.5), 296, 180).greenComponent, 0.05)     // after
    }

    func testArrowEmojiAndTitle() {
        let arrow = mark(.arrow, CGPoint(x: 60, y: 300), CGPoint(x: 220, y: 140), color: 6)   // white on red
        XCTAssertGreaterThan(px(render(edit([arrow]), 2), 140, 220).greenComponent, 0.8)

        let emoji = mark(.emoji, CGPoint(x: 480, y: 40), CGPoint(x: 600, y: 160), text: "✅")   // green box on blue
        let img = render(edit([emoji]), 2)
        let greens = stride(from: 490, to: 590, by: 5).flatMap { x in stride(from: 50, to: 150, by: 5).map { (x, $0) } }
            .filter { let c = px(img, $0.0, $0.1); return c.greenComponent > 0.5 && c.blueComponent < 0.6 }.count
        XCTAssertGreaterThan(greens, 20)

        let title = mark(.title, .zero, CGPoint(x: 640, y: 360), color: 7, text: "Release 1.2")
        let t = render(edit([title]), 2)
        XCTAssertLessThan(px(t, 10, 10).brightnessComponent, 0.15)                // the card covers the frame
        XCTAssertLessThan(px(t, 630, 350).brightnessComponent, 0.15)
    }

    func testZoomInExportAndOnlyWhilePlayingInPreview() {
        let z = mark(.zoom, CGPoint(x: 500, y: 150), CGPoint(x: 560, y: 210))
        let e = edit([z])
        XCTAssertGreaterThan(px(render(e, 0.5), 40, 180).redComponent, 0.9)       // not zoomed yet: red on the left
        let mid = render(e, 2)
        XCTAssertGreaterThan(px(mid, 40, 180).blueComponent, 0.9)                 // zoomed into the blue area
        XCTAssertLessThan(px(render(e, 2, preview: true), 40, 180).blueComponent, 0.1)        // paused preview: not zoomed
        XCTAssertGreaterThan(px(render(e, 2, preview: true, zoom: true), 40, 180).blueComponent, 0.9)
        XCTAssertEqual(FrameRenderer.zoomTarget(z.rect, view: CGRect(origin: .zero, size: size)).width / FrameRenderer.zoomTarget(z.rect, view: CGRect(origin: .zero, size: size)).height, 640.0 / 360, accuracy: 0.01)
    }

    func testAnimations() {
        let r = FrameRenderer(edit: edit([]), full: size, preview: false)
        let fade = mark(.box, .zero, CGPoint(x: 10, y: 10), anim: .fade)
        XCTAssertEqual(r.envelope(fade, 1).0, 0, accuracy: 0.01)
        XCTAssertEqual(r.envelope(fade, 2).0, 1, accuracy: 0.01)
        let pop = mark(.emoji, .zero, CGPoint(x: 10, y: 10), anim: .pop)
        XCTAssertLessThan(r.envelope(pop, 1.02).1, 0.8)
        XCTAssertEqual(r.envelope(pop, 1.5).1, 1, accuracy: 0.01)
    }

    func testCropAppliesToMarkupAndCaptions() {
        var e = edit([mark(.title, .zero, CGPoint(x: 640, y: 360), color: 6, text: "Hi")])
        e.crop = CGRect(x: 320, y: 0, width: 320, height: 360)
        let img = render(e, 2)
        XCTAssertEqual(img.width, 320)
        XCTAssertGreaterThan(px(img, 5, 5).brightnessComponent, 0.9)              // white card fills the cropped frame
    }
}
