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
