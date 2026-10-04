import AppKit
import XCTest
@testable import AtherScreenshot

// Renders real windows offscreen to PNGs for a visual check: ATHER_TEST_OUT=<dir> swift test
final class UISnapshotTests: XCTestCase {
    var out: URL?

    override func setUp() {
        out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"].map { URL(fileURLWithPath: $0) }
        _ = NSApplication.shared
    }

    private func snap(_ view: NSView, _ name: String) {
        guard let out else { return }
        view.layoutSubtreeIfNeeded()
        view.display()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return XCTFail("no rep") }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: out.appendingPathComponent(name + ".png"))
    }

    private func sample() -> CGImage {
        let ctx = makeContext(width: 1400, height: 820)!
        let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [NSColor.systemTeal.cgColor, NSColor.systemIndigo.cgColor] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: 1400, y: 820), options: [])
        withNSContext(ctx, flipped: true) {
            NSAttributedString(string: "Deploy finished  ·  admin@ather.dev  ·  10.0.4.12", attributes: [.font: NSFont.systemFont(ofSize: 44, weight: .semibold), .foregroundColor: NSColor.white])
                .draw(at: CGPoint(x: 80, y: 120))
        }
        return ctx.makeImage()!
    }

    func testEditorWindow() throws {
        try XCTSkipIf(out == nil)
        let e = Editor.open(sample(), scale: 2)
        e.window.setContentSize(NSSize(width: 1080, height: 700))
        e.canvas.state.annots = [
            Annot(tool: .arrow, color: 0, level: 2, unit: 2, pts: [CGPoint(x: 300, y: 600), CGPoint(x: 600, y: 260)]),
            Annot(tool: .rect, color: 5, level: 1, unit: 2, pts: [CGPoint(x: 60, y: 90), CGPoint(x: 1340, y: 200)]),
            Annot(tool: .step, color: 4, level: 2, unit: 2, pts: [CGPoint(x: 900, y: 500)], step: 1),
            Annot(tool: .pixelate, color: 0, level: 2, unit: 2, pts: [CGPoint(x: 520, y: 110), CGPoint(x: 950, y: 180)]),
            Annot(tool: .text, color: 6, level: 2, unit: 2, pts: [CGPoint(x: 700, y: 620)], text: "Looks good"),
        ]
        e.canvas.selected = 1
        e.canvas.changed(pixels: true)
        snap(e.window.contentView!, "ui-editor")
        e.dirty = false
        e.window.close()
    }

    func testEditorSpaceAndOverlay() throws {
        try XCTSkipIf(out == nil)
        let e = Editor.open(sample(), scale: 2)
        e.window.setContentSize(NSSize(width: 1150, height: 760))
        e.canvas.extend(.below)
        let inset = makeContext(width: 600, height: 360)!
        inset.setFillColor(NSColor.systemOrange.cgColor)
        inset.fill(CGRect(x: 0, y: 0, width: 600, height: 360))
        withNSContext(inset, flipped: true) {
            NSAttributedString(string: "Inserted screenshot", attributes: [.font: NSFont.systemFont(ofSize: 40, weight: .bold), .foregroundColor: NSColor.white]).draw(at: CGPoint(x: 40, y: 40))
        }
        e.canvas.insert(inset.makeImage()!, source: nil, at: CGPoint(x: 1050, y: 560))
        var note = Annot(tool: .text, color: 7, level: 1, unit: 2, pts: [CGPoint(x: 40, y: 860)],
                         text: "Remarks: the deploy banner should mention the region, and the IP needs to be redacted before sharing.")
        note.wrap = 1300
        e.canvas.state.annots.append(note)
        e.canvas.changed(pixels: true)
        snap(e.window.contentView!, "ui-editor-space-overlay")
        e.canvas.setTool(.canvas)
        snap(e.window.contentView!, "ui-editor-canvas-tool")
        e.dirty = false
        e.window.close()
    }

    func testCollageEditor() throws {
        try XCTSkipIf(out == nil)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-collage-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let colors: [NSColor] = [.systemTeal, .systemPink, .systemIndigo, .systemOrange]
        let sizes = [(1600, 1000), (900, 1300), (1300, 800), (1100, 1100)]
        var urls: [URL] = []
        for (i, (w, h)) in sizes.enumerated() {
            let ctx = makeContext(width: w, height: h)!
            ctx.setFillColor(colors[i].cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
            withNSContext(ctx, flipped: true) {
                NSAttributedString(string: "Screen \(i + 1)", attributes: [.font: NSFont.systemFont(ofSize: 64, weight: .bold), .foregroundColor: NSColor.white]).draw(at: CGPoint(x: 50, y: 50))
            }
            let u = dir.appendingPathComponent("s\(i).png")
            try ctx.makeImage()!.pngData()!.write(to: u)
            urls.append(u)
        }
        let e = try XCTUnwrap(Editor.openCollage(urls))
        e.window.setContentSize(NSSize(width: 1150, height: 780))
        snap(e.window.contentView!, "ui-collage-auto")
        e.collage?.layout = .feature
        e.collage?.background = 2
        e.collage?.shadow = true
        e.applyCollage()
        snap(e.window.contentView!, "ui-collage-feature")
        let img = e.export()
        XCTAssertGreaterThan(img.width, 1000)
        e.dirty = false
        e.window.close()
    }

    func testPaletteAndToast() throws {
        try XCTSkipIf(out == nil)
        let items = kCmds.prefix(12).map { d in
            PaletteItem(id: d.cmd.rawValue, title: d.title, keywords: d.keywords, icon: d.icon, hint: Hotkey.display(d.hotkey),
                        toggled: d.cmd == .toggleSystemAudio ? true : nil) {}
        }
        Palette.shared.show(Array(items))
        if let v = NSApp.windows.first(where: { $0.isVisible && $0 is KeyablePanel })?.contentView { snap(v, "ui-palette") }
        Palette.shared.close()
        Toast.shared.show("Copied to clipboard", "1400 × 820  ·  Ather_20261003_230112_481.png", image: sample())
        if let v = NSApp.windows.first(where: { $0.isVisible && $0.level == .statusBar })?.contentView { snap(v, "ui-toast") }
        Toast.shared.hide()
    }

    func testSettingsView() throws {
        try XCTSkipIf(out == nil)
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 900), styleMask: [.titled], backing: .buffered, defer: false)
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        let h = NSHostingViewWrapper.make(SettingsView())
        w.contentView = h
        w.orderFront(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))
        snap(h, "ui-settings")
        w.close()
    }
}

import SwiftUI
enum NSHostingViewWrapper {
    static func make<V: View>(_ v: V) -> NSView { NSHostingView(rootView: v) }
}
