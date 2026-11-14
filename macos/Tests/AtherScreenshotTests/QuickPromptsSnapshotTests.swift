import AppKit
import SwiftUI
import XCTest
@testable import AtherScreenshot

// QuickPrompts UI rendered to PNGs for a visual check: ATHER_TEST_OUT=<dir> swift test --filter QuickPromptsSnapshot
final class QuickPromptsSnapshotTests: XCTestCase {
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

    private func host(_ view: NSView, size: NSSize) -> NSWindow {
        let w = NSWindow(contentRect: NSRect(origin: .zero, size: size), styleMask: [.titled], backing: .buffered, defer: false)
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        w.backgroundColor = Theme.bg
        w.contentView = view
        w.orderFront(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))
        return w
    }

    private func sampleStore() throws -> QuickPromptStore {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-qp-snap-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let team = dir.appendingPathComponent("quickprompts-team.json")
        try Data(#"{"prompts": [{"id": "release", "trigger": "releasepls", "title": "Release checklist", "text": "Walk through the release checklist for {date}: build, tests, notes, staging.", "tags": ["release"]}, {"id": "triage", "trigger": "triagepls", "title": "Triage this crash", "text": "Triage the crash in the attached log: likely cause, owner, severity.\n\n{clipboard}"}]}"#.utf8).write(to: team)
        let s = QuickPromptStore()
        s.load(folder: dir, teamFile: team.path)
        s.add(QuickPrompt(trigger: "sigvn", title: "Chữ ký", text: "Trân trọng,\nNguyễn Văn Tín\nAther Studio", tags: ["mail"]))
        s.add(QuickPrompt(trigger: "standupfix", title: "Stand-up for the bug bash", text: "Yesterday: fixed {cursor}\nToday: the bug bash\nBlockers: none", hotkey: "Ctrl+Alt+1", tags: ["team"]))
        s.setEnabled("builtin.archpls", false)
        s.noteUsed("builtin.doitforme")
        s.noteUsed("builtin.doitforme")
        return s
    }

    func testQuickPromptsWindow() throws {
        try XCTSkipIf(out == nil)
        let s = try sampleStore()
        let m = QuickPromptsModel(store: s)
        m.hotkeyStatus = { _ in nil }
        let w = QuickPromptsWindow(model: m, register: false)
        w.window.setContentSize(NSSize(width: 1000, height: 680))
        m.select(s.all.first { $0.trigger == "standupfix" }?.id)
        m.tryText = m.tryChanged("Ping the team: standupfix ")?.0 ?? ""
        w.window.orderFront(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        snap(w.window.contentView!, "ui-quickprompts-window")
        // A team starter selected (read-only) and a search.
        m.query = "pls"
        m.select("builtin.bugpls")
        RunLoop.main.run(until: Date().addingTimeInterval(0.4))
        snap(w.window.contentView!, "ui-quickprompts-window-starter")
        w.window.close()
    }

    func testQuickPromptsPicker() throws {
        try XCTSkipIf(out == nil)
        let items = QuickPromptsMac.shared.paletteItems(target: nil)
        XCTAssertEqual(items.count, QuickPromptStore.shared.active.count)
        Palette.shared.show(items, placeholder: "Search QuickPrompts…  (↩ pastes it)", mruKey: "QuickPromptsSnapshotMRU")
        if let v = NSApp.windows.first(where: { $0.isVisible && $0 is KeyablePanel })?.contentView { snap(v, "ui-quickprompts-picker") }
        Palette.shared.close()
    }

    func testQuickPromptsWelcome() throws {
        try XCTSkipIf(out == nil)
        let v = QuickPromptsWelcome.makeView(picker: Hotkey.display("Ctrl+Alt+P")) { _ in }
        let w = host(v, size: v.frame.size)
        snap(v, "ui-quickprompts-welcome")
        w.close()
    }

    func testQuickPromptsSettings() throws {
        try XCTSkipIf(out == nil)
        let h = NSHostingViewWrapper.make(SettingsView(query: "quickprompt"))
        let w = host(h, size: NSSize(width: 680, height: 720))
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        snap(h, "ui-settings-quickprompts")
        w.close()
    }
}
