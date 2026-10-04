import AppKit
import XCTest
@testable import AtherScreenshot

// Canvas space, image layers and collages.
final class CompositionTests: XCTestCase {
    private func solid(_ w: Int, _ h: Int, _ c: NSColor) -> CGImage {
        let ctx = makeContext(width: w, height: h)!
        ctx.setFillColor(c.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage()!
    }

    func testAddedSpaceUsesFillAndKeepsImage() {
        let base = solid(200, 100, .red)
        var st = DocState()
        st.crop = CGRect(x: 0, y: 0, width: 200, height: 180)   // 80 px of space below
        st.fill = CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1)
        let out = Render.compose(base, st)
        XCTAssertEqual(out.width, 200)
        XCTAssertEqual(out.height, 180)
        XCTAssertEqual(out.color(atPixel: CGPoint(x: 100, y: 50))?.hex, "#FF0000")
        XCTAssertEqual(out.color(atPixel: CGPoint(x: 100, y: 150))?.hex, "#FFFFFF")

        st.crop = CGRect(x: -20, y: -20, width: 240, height: 140)  // even margin, transparent
        st.fill = nil
        let clear = Render.compose(base, st)
        XCTAssertEqual(clear.width, 240)
        XCTAssertEqual(alpha(clear, 5, 5), 0)
        XCTAssertEqual(clear.color(atPixel: CGPoint(x: 120, y: 70))?.hex, "#FF0000")
    }

    private func alpha(_ img: CGImage, _ x: Int, _ y: Int) -> UInt8 {
        var px = [UInt8](repeating: 0, count: 4)
        let ctx = CGContext(data: &px, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
        ctx.draw(img.cropping(to: CGRect(x: x, y: y, width: 1, height: 1))!, in: CGRect(x: 0, y: 0, width: 1, height: 1))
        return px[3]
    }

    func testEdgeColorMatchesBorder() {
        XCTAssertEqual(NSColor(cgColor: Render.edgeColor(solid(50, 50, .blue)))?.hex, NSColor.blue.usingColorSpace(.sRGB)?.hex)
    }

    func testImageLayerIsFlattenedUnderAnnotations() {
        let base = solid(300, 200, .white)
        var a = Annot(tool: .image, color: 6, level: 0, unit: 1, pts: [CGPoint(x: 100, y: 50), CGPoint(x: 200, y: 150)])
        a.layer = ImageLayer(image: solid(40, 40, .green), source: nil)
        var st = DocState()
        st.annots = [a]
        let out = Render.compose(base, st)
        XCTAssertEqual(out.color(atPixel: CGPoint(x: 150, y: 100))?.hex, "#00FF00")
        XCTAssertEqual(out.color(atPixel: CGPoint(x: 50, y: 100))?.hex, "#FFFFFF")

        // Blur applies to the layer too, since layers are part of the background.
        let px = Annot(tool: .blur, color: 0, level: 4, unit: 1, pts: [CGPoint(x: 60, y: 20), CGPoint(x: 240, y: 180)])
        st.annots.append(px)
        let mixed = Render.compose(base, st).color(atPixel: CGPoint(x: 100, y: 50))?.hex
        XCTAssertNotEqual(mixed, "#00FF00")
    }

    func testTextWrapsAtWidth() {
        var t = Annot(tool: .text, color: 0, level: 1, unit: 1, pts: [.zero], text: "A long remark that should wrap onto several lines here")
        let one = Render.textRect(t)
        t.wrap = 160
        let wrapped = Render.textRect(t)
        XCTAssertLessThanOrEqual(wrapped.width, 160.5)
        XCTAssertGreaterThan(wrapped.height, one.height * 1.5)
    }

    func testCollageLayouts() {
        let imgs = [solid(1600, 1000, .red), solid(800, 1200, .green), solid(1200, 700, .blue), solid(1000, 1000, .yellow), solid(1400, 900, .gray)]
        for layout in CollageLayout.allCases {
            var spec = CollageSpec(images: imgs, sources: imgs.map { _ in nil })
            spec.layout = layout
            let r = Collage.layout(spec, unit: 1)
            XCTAssertEqual(r.rects.count, imgs.count, "\(layout)")
            let canvas = CGRect(origin: .zero, size: r.size)
            for (i, rect) in r.rects.enumerated() {
                XCTAssertTrue(canvas.insetBy(dx: -1, dy: -1).contains(rect), "\(layout) rect \(i) \(rect) outside \(canvas)")
                let want = CGFloat(imgs[spec.order[i]].width) / CGFloat(imgs[spec.order[i]].height)
                XCTAssertEqual(rect.width / rect.height, want, accuracy: want * 0.03, "\(layout) keeps aspect")
                for (j, other) in r.rects.enumerated() where j > i {
                    XCTAssertLessThan(rect.insetBy(dx: 1, dy: 1).intersection(other.insetBy(dx: 1, dy: 1)).width, 1, "\(layout) \(i) overlaps \(j)")
                }
            }
        }
        var spec = CollageSpec(images: imgs, sources: imgs.map { _ in nil })
        spec.size = .wide
        XCTAssertEqual(Collage.layout(spec, unit: 2).size.width, 1920)
        spec.size = .square
        let sq = Collage.layout(spec, unit: 2).size
        XCTAssertEqual(sq.width, 2048)
        XCTAssertEqual(sq.height, 2048)
    }

    func testCollageMetadata() {
        let l = Library(persists: false)
        let a = URL(fileURLWithPath: "/tmp/ather-review/a.png"), b = URL(fileURLWithPath: "/tmp/ather-review/b.png"), c = URL(fileURLWithPath: "/tmp/ather-review/c.png")
        l.apply([(a, 1, 1), (b, 2, 1)])
        let col = l.createCollection("Sprint")
        l.add([a, b], toCollection: col.id)
        l.addTags(["bug", "ios"], to: [a])
        l.addTags(["bug"], to: [b])
        l.noteEdit(c, from: nil, info: NameInfo(), edited: false, includes: [a, b], collage: true)
        l.apply([(a, 1, 1), (b, 2, 1), (c, 3, 1)])
        XCTAssertEqual(l.meta(c).tags, ["bug", "collage"])
        XCTAssertEqual(l.meta(c).collections, [col.id])
        XCTAssertEqual(l.meta(c).includes, [a.path, b.path])
    }
}
