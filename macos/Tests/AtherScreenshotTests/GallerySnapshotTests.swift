import AppKit
import SwiftUI
import XCTest
@testable import AtherScreenshot

// ATHER_TEST_OUT=<dir> ATHER_SUPPORT_DIR=<tmp> swift test --filter GallerySnapshotTests
final class GallerySnapshotTests: XCTestCase {
    private func card(_ w: Int, _ h: Int, _ a: NSColor, _ b: NSColor, _ title: String, _ body: String) -> CGImage {
        let ctx = makeContext(width: w, height: h)!
        let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [a.cgColor, b.cgColor] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: w, y: h), options: [])
        withNSContext(ctx, flipped: true) {
            NSColor.white.withAlphaComponent(0.92).setFill()
            NSBezierPath(roundedRect: NSRect(x: w / 12, y: h / 6, width: w * 5 / 6, height: h * 2 / 3), xRadius: 18, yRadius: 18).fill()
            NSAttributedString(string: title, attributes: [.font: NSFont.systemFont(ofSize: CGFloat(h) / 12, weight: .bold), .foregroundColor: NSColor.black])
                .draw(at: CGPoint(x: w / 8, y: h / 4))
            NSAttributedString(string: body, attributes: [.font: NSFont.systemFont(ofSize: CGFloat(h) / 22), .foregroundColor: NSColor.darkGray])
                .draw(at: CGPoint(x: w / 8, y: h / 4 + h / 7))
        }
        return ctx.makeImage()!
    }

    private func snap(_ view: NSView, _ name: String, out: URL) {
        view.layoutSubtreeIfNeeded()
        view.display()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: out.appendingPathComponent(name + ".png"))
    }

    private func wait(_ s: Double) { RunLoop.main.run(until: Date().addingTimeInterval(s)) }

    func testGallery() throws {
        guard let outPath = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"], ProcessInfo.processInfo.environment["ATHER_SUPPORT_DIR"] != nil else {
            throw XCTSkip("set ATHER_TEST_OUT and ATHER_SUPPORT_DIR")
        }
        let out = URL(fileURLWithPath: outPath)
        _ = NSApplication.shared
        let caps = FileManager.default.temporaryDirectory.appendingPathComponent("ather-gallery-\(UUID().uuidString)/2026-10")
        try FileManager.default.createDirectory(at: caps, withIntermediateDirectories: true)
        Settings.shared.set("SaveFolder", caps.deletingLastPathComponent().path)
        defer { Settings.shared.reset("SaveFolder") }

        let specs: [(String, Int, Int, NSColor, NSColor, String, String)] = [
            ("Checkout error", 1600, 1000, .systemRed, .systemOrange, "Payment failed", "Error 402 · card declined"),
            ("Dashboard", 1800, 1000, .systemTeal, .systemBlue, "Weekly revenue", "Up 12% over last week"),
            ("Login screen", 900, 1400, .systemIndigo, .systemPurple, "Sign in", "Use your Ather account"),
            ("Release notes", 1400, 900, .systemGreen, .systemTeal, "Version 1.1", "Gallery, tags and smart folders"),
            ("Long page", 800, 2600, .systemGray, .darkGray, "Docs", "Scrolling capture of a long page"),
            ("Bug report", 1500, 950, .systemPink, .systemRed, "Crash on launch", "Stack trace attached"),
            ("Design review", 2000, 900, .systemYellow, .systemOrange, "Hero section", "New illustration and copy"),
            ("Settings", 1200, 1200, .systemBlue, .systemIndigo, "Preferences", "Shortcuts and uploads"),
            ("Chat", 1000, 1500, .systemMint, .systemGreen, "Team chat", "Ship it on Friday?"),
            ("Analytics", 1700, 950, .systemPurple, .systemPink, "Funnel", "Drop-off at step 3"),
            ("Dashboard copy", 1800, 1000, .systemTeal, .systemBlue, "Weekly revenue", "Up 12% over last week"),
            ("Terminal", 1500, 900, .black, .darkGray, "build succeeded", "12 tests passed"),
        ]
        for (i, s) in specs.enumerated() {
            let url = caps.appendingPathComponent("\(s.0).png")
            try card(s.1, s.2, s.3, s.4, s.5, s.6).pngData()!.write(to: url)
            try FileManager.default.setAttributes([.modificationDate: Date().addingTimeInterval(-Double(i) * 3600)], ofItemAtPath: url.path)
        }
        let lib = Library.shared
        let done = expectation(description: "scan")
        lib.refresh { done.fulfill() }
        wait(for: [done], timeout: 10)
        for _ in 0..<120 where lib.urls.contains(where: { lib.meta($0).indexed == 0 }) { wait(0.25) }
        func url(_ n: String) -> URL { caps.appendingPathComponent("\(n).png") }
        lib.addTags(["bug", "checkout"], to: [url("Checkout error"), url("Bug report")])
        lib.addTags(["dashboard"], to: [url("Dashboard"), url("Analytics"), url("Dashboard copy")])
        lib.addTags(["design"], to: [url("Design review"), url("Login screen")])
        lib.setRating(5, [url("Dashboard")]); lib.setRating(3, [url("Design review")])
        let col = lib.createCollection("Sprint 42")
        lib.add([url("Checkout error"), url("Bug report"), url("Release notes")], toCollection: col.id)
        _ = lib.saveSmartFolder("Red things", Filter(color: "#FF3B30"))
        lib.testSetApp(url("Dashboard").path, "Safari")
        lib.testSetApp(url("Terminal").path, "Terminal")
        lib.setComment("Numbers look off for Tuesday — check with data team.", url("Dashboard"))

        // New files appear without a manual refresh (the captures folder is watched).
        let fresh = caps.appendingPathComponent("Edited later.png")
        try card(800, 500, .systemTeal, .systemBlue, "Edited later", "Saved from the editor").pngData()!.write(to: fresh)
        for _ in 0..<20 where !lib.urls.contains(fresh) { wait(0.25) }
        XCTAssertTrue(lib.urls.contains(fresh))
        try FileManager.default.removeItem(at: fresh)
        for _ in 0..<20 where lib.urls.contains(fresh) { wait(0.25) }
        XCTAssertFalse(lib.urls.contains(fresh))

        let model = GalleryModel()
        model.showSidebar = false
        model.showInspector = false
        UserDefaults.standard.removeObject(forKey: "GalleryInspectorDiscovered")
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1320, height: 820), styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        let host = NSHostingView(rootView: GalleryView(model: model, lib: lib))
        w.contentView = host
        w.orderFront(nil)
        wait(2.5)
        snap(host, "gallery-canvas", out: out)
        model.selection = [url("Dashboard")]
        model.focus = url("Dashboard")
        wait(1)
        snap(host, "gallery-selected", out: out)
        model.showInspector = true
        wait(1)
        snap(host, "gallery-inspector", out: out)
        XCTAssertTrue(UserDefaults.standard.bool(forKey: "GalleryInspectorDiscovered"))
        model.showInspector = false
        model.showSidebar = true
        wait(1)
        snap(host, "gallery-sidebar", out: out)
        model.showSidebar = false
        model.showShortcuts = true
        wait(0.6)
        snap(host, "gallery-shortcuts", out: out)
        model.showShortcuts = false

        model.filter = Filter(text: "revenue")
        wait(1)
        snap(host, "gallery-search", out: out)

        model.filter = Filter(tags: ["bug"], color: "#FF3B30")
        wait(1)
        snap(host, "gallery-filtered", out: out)
        model.filter = Filter(color: "#FF3B30")
        wait(1)
        XCTAssertTrue(model.visible.contains(url("Checkout error")))
        XCTAssertFalse(model.visible.contains(url("Terminal")))
        model.filter = Filter()

        model.scope = .duplicates
        wait(1.5)
        XCTAssertEqual(model.groups.first.map { Set($0.map(\.lastPathComponent)) }, ["Dashboard.png", "Dashboard copy.png"])
        snap(host, "gallery-duplicates", out: out)

        model.findSimilar(url("Checkout error"))
        wait(1.5)
        XCTAssertEqual(model.visible.first, url("Checkout error"))
        snap(host, "gallery-similar", out: out)

        model.scope = .all
        model.layout = .grid
        model.selection = Set([url("Bug report"), url("Checkout error"), url("Chat")])
        wait(1.5)
        snap(host, "gallery-grid-multi", out: out)

        model.layout = .list
        wait(1)
        snap(host, "gallery-list", out: out)
        model.layout = .justified

        model.preview = url("Login screen")
        wait(1)
        snap(host, "gallery-preview", out: out)
        model.preview = nil
        w.setContentSize(NSSize(width: 700, height: 600))
        wait(1)
        snap(host, "gallery-narrow", out: out)
        w.close()
    }
}
