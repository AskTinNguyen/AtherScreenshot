import AppKit
import SwiftUI
import XCTest
@testable import AtherScreenshot

// ATHER_TEST_OUT=<dir> ATHER_SUPPORT_DIR=<tmp> swift test --filter GalleryScrubSnapshotTests
// Video tiles in the gallery: at rest (poster, duration badge) and one scrubbed 40% along its timeline.
final class GalleryScrubSnapshotTests: XCTestCase {
    private func wait(_ s: Double) { RunLoop.main.run(until: Date().addingTimeInterval(s)) }

    private func snap(_ view: NSView, _ name: String, out: URL) {
        view.layoutSubtreeIfNeeded()
        view.display()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: out.appendingPathComponent(name + ".png"))
    }

    // A clip whose hue turns and whose big label counts the seconds, so each spot along it looks different.
    private func clip(_ to: URL, w: Int, h: Int, seconds: Double, hue: CGFloat) throws {
        final class Box: @unchecked Sendable { var made: Result<URL, Error>? }
        let box = Box()
        Task.detached {
            do {
                box.made = .success(try await TestMedia.clip(w: w, h: h, fps: 10, seconds: seconds) { i, ctx in
                    let t = Double(i) / 10
                    let a = NSColor(hue: (hue + CGFloat(t / seconds) * 0.6).truncatingRemainder(dividingBy: 1), saturation: 0.75, brightness: 0.9, alpha: 1)
                    let b = NSColor(hue: (hue + 0.15 + CGFloat(t / seconds) * 0.6).truncatingRemainder(dividingBy: 1), saturation: 0.9, brightness: 0.55, alpha: 1)
                    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [a.cgColor, b.cgColor] as CFArray, locations: [0, 1])!
                    ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: w, y: h), options: [])
                    withNSContext(ctx, flipped: true) {
                        NSAttributedString(string: String(format: "%.1f s", t), attributes: [.font: NSFont.monospacedDigitSystemFont(ofSize: CGFloat(h) / 4, weight: .heavy),
                                                                                            .foregroundColor: NSColor.white])
                            .draw(at: CGPoint(x: w / 10, y: h / 3))
                    }
                })
            } catch { box.made = .failure(error) }
        }
        for _ in 0..<600 where box.made == nil { wait(0.05) }
        let u = try XCTUnwrap(box.made).get()
        try FileManager.default.moveItem(at: u, to: to)
    }

    func testGalleryScrub() throws {
        guard let outPath = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"], ProcessInfo.processInfo.environment["ATHER_SUPPORT_DIR"] != nil else {
            throw XCTSkip("set ATHER_TEST_OUT and ATHER_SUPPORT_DIR")
        }
        let out = URL(fileURLWithPath: outPath)
        _ = NSApplication.shared
        let caps = FileManager.default.temporaryDirectory.appendingPathComponent("ather-gallery-scrub-\(UUID().uuidString)/2026-11")
        try FileManager.default.createDirectory(at: caps, withIntermediateDirectories: true)
        Settings.shared.set("SaveFolder", caps.deletingLastPathComponent().path)
        defer { Settings.shared.reset("SaveFolder") }

        let walk = caps.appendingPathComponent("Walkthrough.mp4")
        try clip(walk, w: 1280, h: 720, seconds: 34, hue: 0.55)
        try clip(caps.appendingPathComponent("Bug repro.mp4"), w: 1000, h: 700, seconds: 8, hue: 0.0)
        try clip(caps.appendingPathComponent("Odd size.mp4"), w: 1636, h: 854, seconds: 5, hue: 0.3)
        for (i, f) in ["Walkthrough.mp4", "Bug repro.mp4", "Odd size.mp4"].enumerated() {
            try FileManager.default.setAttributes([.modificationDate: Date().addingTimeInterval(-Double(i) * 3600)], ofItemAtPath: caps.appendingPathComponent(f).path)
        }
        let lib = Library.shared
        let done = expectation(description: "scan")
        lib.refresh { done.fulfill() }
        wait(for: [done], timeout: 10)
        for _ in 0..<120 where lib.urls.contains(where: { lib.meta($0).indexed == 0 }) { wait(0.25) }
        XCTAssertEqual(lib.meta(walk).duration ?? 0, 34, accuracy: 0.2)

        let model = GalleryModel()
        model.showSidebar = false
        model.showInspector = false
        model.layout = .justified
        model.filter = Filter()
        model.scope = .all
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 520), styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
        w.titlebarAppearsTransparent = true
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        let host = NSHostingView(rootView: GalleryView(model: model, lib: lib))
        w.contentView = host
        w.orderFront(nil)
        wait(2.5)
        snap(host, "gallery-video", out: out)

        // 40% along the walkthrough's timeline: 0:13 of 0:34, its frame there in the tile.
        model.scrubber.scrub(walk, at: 0.4, side: 300)
        for _ in 0..<100 where model.scrubber.picture == nil { wait(0.05) }
        XCTAssertNotNil(model.scrubber.picture)
        wait(0.5)
        snap(host, "gallery-video-scrub", out: out)
        XCTAssertEqual(Scrub.badge(model.scrubber.at, length: 34), "0:13 / 0:34")
        model.scrubber.end(walk)
        wait(0.3)
        XCTAssertNil(model.scrubber.picture)
        w.close()
    }
}
