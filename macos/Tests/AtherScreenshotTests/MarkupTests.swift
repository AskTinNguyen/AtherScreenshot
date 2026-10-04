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

    private func mark(_ k: MarkKind, _ a: CGPoint, _ b: CGPoint, color: Int = 0, text: String = "", anim: AnimStyle = .none) -> Mark {
        Mark(kind: k, start: 1, end: 3, a: a, b: b, text: text, color: color, style: anim)
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

    func testAnimationStyles() {
        let r = FrameRenderer(edit: edit([]), full: size, preview: false)
        func mo(_ st: AnimStyle, _ t: Double, kind: MarkKind = .box, exit: AnimStyle? = nil, emphasis: Emphasis = .none) -> Motion {
            var m = mark(kind, .zero, CGPoint(x: 10, y: 10), text: "Hello world", anim: st)
            m.exit = exit
            m.emphasis = emphasis
            return r.motion(m, t)
        }
        XCTAssertEqual(mo(.fade, 1).alpha, 0, accuracy: 0.01)
        XCTAssertEqual(mo(.fade, 2).alpha, 1, accuracy: 0.01)
        XCTAssertLessThan(mo(.fade, 2.95).alpha, 0.5)                   // the same style plays it out
        XCTAssertLessThan(mo(.pop, 1.02).scale, 0.8)
        XCTAssertEqual(mo(.pop, 1.5).scale, 1, accuracy: 0.01)
        XCTAssertGreaterThan(mo(.slide, 1.05).dy, 5)                    // comes up from below
        XCTAssertLessThan(mo(.drawOn, 1.1).reveal, 0.5)
        XCTAssertEqual(mo(.drawOn, 2.95).reveal, 1)                     // draw on leaves with a fade instead
        XCTAssertLessThan(mo(.drawOn, 2.95).alpha, 1)
        XCTAssertTrue(mo(.wipe, 1.1).wipe)
        XCTAssertGreaterThan(mo(.blurIn, 1.05).blur, 1)
        XCTAssertEqual(mo(.auto, 1.05, kind: .arrow).reveal, mo(.drawOn, 1.05, kind: .arrow).reveal)   // auto: the kind's default
        // Advanced: a different exit.
        XCTAssertEqual(mo(.pop, 2.95, exit: AnimStyle.none).alpha, 1)
        XCTAssertEqual(mo(.none, 2.95, exit: .fade).alpha, mo(.fade, 2.95).alpha, accuracy: 0.01)
        // While on screen.
        XCTAssertNotEqual(mo(.none, 2.3, emphasis: .pulse).scale, 1)
        XCTAssertNotNil(mo(.none, 2, emphasis: .ping).ring)
        XCTAssertNotEqual(mo(.none, 2.1, emphasis: .bounce).dy, 0)
    }

    func testDrawOnAndTypewriterRenderPartway() {
        let r = FrameRenderer(edit: edit([]), full: size, preview: false)
        let box = mark(.box, CGPoint(x: 100, y: 100), CGPoint(x: 300, y: 250), color: 6)
        let full = r.markImage(box)!.0, half = r.markImage(box, reveal: 0.3)!.0
        func lit(_ i: CGImage) -> Int {   // opaque pixels, read straight from an RGBA copy
            var px = [UInt8](repeating: 0, count: i.width * i.height * 4)
            let ctx = CGContext(data: &px, width: i.width, height: i.height, bitsPerComponent: 8, bytesPerRow: i.width * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            ctx.draw(i, in: CGRect(x: 0, y: 0, width: i.width, height: i.height))
            return stride(from: 3, to: px.count, by: 4).filter { px[$0] > 128 }.count
        }
        XCTAssertLessThan(lit(half), lit(full) / 2)
        let text = mark(.text, CGPoint(x: 50, y: 50), CGPoint(x: 400, y: 100), color: 6, text: "Typewriter text here")
        XCTAssertLessThan(lit(r.markImage(text, reveal: 0.25)!.0), lit(r.markImage(text)!.0))
        XCTAssertEqual(r.markImage(text, reveal: 0.25)!.1, r.markImage(text)!.1)   // same box while typing, so it doesn't drift
    }

    func testCaptionLooksAndWordHighlight() {
        var e = edit([])
        let words = [CaptionWord(start: 1, end: 1.5, text: "Click"), CaptionWord(start: 1.5, end: 3, text: "Deploy")]
        e.captions = [Caption(start: 1, end: 3, text: "Click Deploy", words: words)]
        let r = FrameRenderer(edit: e, full: size, preview: false)
        let (_, pill) = r.captionImage(e.captions[0], in: size, at: 1.2)!
        XCTAssertLessThan(pill.width, size.width * 0.6)
        e.captionLook = .bar
        XCTAssertEqual(FrameRenderer(edit: e, full: size, preview: false).captionImage(e.captions[0], in: size, at: 1.2)!.1.width, size.width)
        // The spoken word turns yellow: count yellow pixels in the first and second half of the caption.
        e.captionLook = .pill
        let rr = FrameRenderer(edit: e, full: size, preview: false)
        func yellowSide(_ t: Double) -> (Int, Int) {
            let img = rr.captionImage(e.captions[0], in: size, at: t)!.0
            var l = 0, rt = 0
            for x in stride(from: 0, to: img.width, by: 2) { for y in stride(from: 0, to: img.height, by: 2) {
                let c = img.color(atPixel: CGPoint(x: x, y: y))!.usingColorSpace(.sRGB)!
                if c.redComponent > 0.8 && c.greenComponent > 0.6 && c.blueComponent < 0.3 { if x < img.width / 2 { l += 1 } else { rt += 1 } }
            } }
            return (l, rt)
        }
        let early = yellowSide(1.2), late = yellowSide(2)
        XCTAssertGreaterThan(early.0, early.1)    // "Click" lit
        XCTAssertGreaterThan(late.1, late.0)      // then "Deploy"
        // Edited text no longer matches the words: no highlight.
        e.captions[0].text = "Press Deploy"
        let edited = FrameRenderer(edit: e, full: size, preview: false).captionImage(e.captions[0], in: size, at: 1.2)!.0
        XCTAssertNotNil(edited)
    }

    func testCropAppliesToMarkupAndCaptions() {
        var e = edit([mark(.title, .zero, CGPoint(x: 640, y: 360), color: 6, text: "Hi")])
        e.crop = CGRect(x: 320, y: 0, width: 320, height: 360)
        let img = render(e, 2)
        XCTAssertEqual(img.width, 320)
        XCTAssertGreaterThan(px(img, 5, 5).brightnessComponent, 0.9)              // white card fills the cropped frame
    }
}
