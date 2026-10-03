import AppKit
import XCTest
@testable import AtherScreenshot

final class LibraryTests: XCTestCase {
    private func solid(_ w: Int, _ h: Int, _ c: NSColor, stripe: NSColor? = nil) -> CGImage {
        let ctx = makeContext(width: w, height: h)!
        ctx.setFillColor(c.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        if let stripe {
            ctx.setFillColor(stripe.cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w / 3, height: h))
        }
        return ctx.makeImage()!
    }

    private func u(_ n: String) -> URL { URL(fileURLWithPath: "/tmp/ather-lib-test/\(n)") }

    private func lib(_ names: [(String, Double, Int)]) -> Library {
        let l = Library(empty: true)
        l.apply(names.map { (u($0.0), $0.1, $0.2) })
        return l
    }

    func testFilterMatching() {
        var m = ItemMeta()
        m.w = 3000; m.h = 1000; m.tags = ["Bug", "ui"]; m.rating = 4; m.app = "Safari"; m.text = "Invoice total 42"
        m.mtime = Date().timeIntervalSince1970; m.size = 500_000
        m.colors = [PaletteColor(r: 250, g: 10, b: 10, ratio: 0.5), PaletteColor(r: 20, g: 20, b: 20, ratio: 0.5)]
        let url = u("a.png")
        XCTAssertTrue(Filter(text: "invoice 42").matches(url, m))
        XCTAssertTrue(Filter(text: "safari bug").matches(url, m))   // app and tags are searchable too
        XCTAssertFalse(Filter(text: "receipt").matches(url, m))
        XCTAssertTrue(Filter(tags: ["bug", "UI"]).matches(url, m))
        XCTAssertFalse(Filter(tags: ["bug", "mobile"]).matches(url, m))
        XCTAssertTrue(Filter(tags: ["bug", "mobile"], anyTag: true).matches(url, m))
        XCTAssertFalse(Filter(untagged: true).matches(url, m))
        XCTAssertTrue(Filter(minRating: 4).matches(url, m))
        XCTAssertFalse(Filter(minRating: 5).matches(url, m))
        XCTAssertTrue(Filter(color: "#FF0000").matches(url, m))
        XCTAssertFalse(Filter(color: "#0A84FF").matches(url, m))
        XCTAssertTrue(Filter(shape: .wide).matches(url, m))
        XCTAssertFalse(Filter(shape: .portrait).matches(url, m))
        XCTAssertTrue(Filter(date: .today).matches(url, m))
        XCTAssertTrue(Filter(size: .medium).matches(url, m))
        XCTAssertTrue(Filter(apps: ["Safari"]).matches(url, m))
        XCTAssertFalse(Filter(types: [.video]).matches(url, m))
        XCTAssertTrue(Filter(minWidth: 1920, minHeight: 1000).matches(url, m))
        XCTAssertFalse(Filter(minWidth: 1920, minHeight: 1080).matches(url, m))
        XCTAssertEqual(Filter(text: "x", minRating: 2).activeCount, 2)
    }

    func testSmartFolderRoundTrip() throws {
        let f = Filter(text: "error", types: [.image], tags: ["bug"], minRating: 3, color: "#FF3B30", shape: .tall, date: .week)
        let s = SmartFolder(name: "Bugs", filter: f)
        let back = try JSONDecoder().decode(SmartFolder.self, from: JSONEncoder().encode(s))
        XCTAssertEqual(back.filter, f)
    }

    func testTagsCollectionsAndRenames() {
        let l = lib([("a.png", 1, 10), ("b.png", 2, 20), ("c.gif", 3, 30)])
        let a = u("a.png"), b = u("b.png")
        l.addTags(["Bug", "bug", " ui "], to: [a, b])
        XCTAssertEqual(l.meta(a).tags, ["Bug", "ui"])   // de-duplicated case-insensitively, trimmed
        XCTAssertEqual(l.allTags.first?.0, "Bug")
        XCTAssertEqual(l.allTags.first?.1, 2)
        l.renameTag("bug", to: "Defect")
        XCTAssertEqual(l.meta(b).tags, ["Defect", "ui"])
        let c = l.createCollection("Release notes")
        l.setAutoTags(c.id, ["release"])
        l.add([a], toCollection: c.id)
        XCTAssertEqual(l.count(in: c.id), 1)
        XCTAssertTrue(l.meta(a).tags.contains("release"))  // collection auto tags
        l.setRating(9, [a])
        XCTAssertEqual(l.meta(a).rating, 5)
        let moved = u("renamed.png")
        l.moved(from: a, to: moved)
        XCTAssertEqual(l.meta(moved).rating, 5)
        XCTAssertTrue(l.meta(moved).collections.contains(c.id))
        XCTAssertEqual(l.meta(a).tags, [])
        l.deleteCollection(c.id)
        XCTAssertTrue(l.meta(moved).collections.isEmpty)
    }

    func testIndexerAndDuplicates() async {
        let red = solid(400, 300, .systemRed, stripe: .black)
        let redCopy = solid(800, 600, .systemRed, stripe: .black)   // same picture, different size
        let blue = solid(400, 300, .systemBlue, stripe: .white)
        let pal = Indexer.palette(red)
        XCTAssertEqual(pal.count, 2)
        XCTAssertEqual(pal[0].ratio, 2.0 / 3, accuracy: 0.05)
        XCTAssertTrue(Filter.distance((pal[0].r, pal[0].g, pal[0].b), (255, 59, 48)) < 60)
        let h1 = Indexer.dhash(red), h2 = Indexer.dhash(redCopy), h3 = Indexer.dhash(blue)
        XCTAssertLessThanOrEqual((h1 ^ h2).nonzeroBitCount, 4)
        XCTAssertGreaterThan((h1 ^ h3).nonzeroBitCount, 4)

        let l = lib([("r1.png", 1, 1), ("r2.png", 2, 1), ("b.png", 3, 1)])
        l.testSetHash(u("r1.png").path, h1)
        l.testSetHash(u("r2.png").path, h2)
        l.testSetHash(u("b.png").path, h3)
        for (n, img) in [("r1.png", red), ("r2.png", redCopy), ("b.png", blue)] { l.testSetFeature(u(n).path, Indexer.featurePrint(img)!) }
        let groups = l.duplicateGroups()
        XCTAssertEqual(groups.count, 1)
        XCTAssertEqual(Set(groups[0].map(\.lastPathComponent)), ["r1.png", "r2.png"])
        XCTAssertEqual(groups[0].first?.lastPathComponent, "r2.png")  // newest first
        XCTAssertNotNil(Indexer.featurePrint(red))
    }

    func testJustifiedRows() {
        let items = (0..<7).map { u("\($0).png") }
        let aspects: [URL: CGFloat] = Dictionary(uniqueKeysWithValues: items.enumerated().map { ($1, [1.5, 1, 2, 0.5, 1.5, 1, 1][$0]) })
        let rows = justifiedRows(items, aspect: { aspects[$0]! }, width: 1000, target: 200, spacing: 8)
        XCTAssertEqual(rows.flatMap(\.items), items)
        for r in rows.dropLast() {
            let w = r.items.reduce(0) { $0 + aspects[$1]! * r.height } + 8 * CGFloat(r.items.count - 1)
            XCTAssertEqual(w, 1000, accuracy: 0.5)     // full rows fill the width exactly
            XCTAssertLessThanOrEqual(r.height, 200.5)
        }
        XCTAssertEqual(rows.last!.height, 200)
    }

    func testSeededShuffleIsStable() {
        var a = SeededRandom(seed: 42), b = SeededRandom(seed: 42)
        XCTAssertEqual(Array(0..<20).shuffled(using: &a), Array(0..<20).shuffled(using: &b))
    }
}
