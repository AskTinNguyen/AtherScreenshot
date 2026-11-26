import AppKit
import SwiftUI
import XCTest
@testable import AtherScreenshot

// The Meetings window, the recording bar and Settings › Meetings rendered to PNGs for a visual check:
// ATHER_TEST_OUT=<dir> xcrun swift test --filter MeetingSnapshot
final class MeetingSnapshotTests: XCTestCase {
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

    private func wait(_ s: Double) { RunLoop.main.run(until: Date().addingTimeInterval(s)) }

    static func demoMeeting(in store: MeetingStore) -> Meeting {
        func w(_ text: String, _ start: Double) -> [TimedWord] {
            var t = start
            return text.split(separator: " ").map { (s: Substring) -> TimedWord in
                let d = 0.16 + Double(s.count) * 0.045
                defer { t += d + 0.05 }
                return TimedWord(start: t, end: t + d, text: String(s))
            }
        }
        var m = Meeting()
        m.title = "Q1 launch sync"
        m.created = Date(timeIntervalSince1970: 1_795_000_000)
        m.duration = 168
        m.hasOthers = true
        m.hasYou = true
        m.app = "Zoom"
        m.micName = "MacBook Pro Microphone"
        m.transcript = .done
        m.transcriptEngine = "Apple speech recognition on this Mac"
        m.language = "en_US"
        let others = w("Okay, so, um, let's get started. The goal today is to lock the launch date for the Q1 release.", 2)
            + w("Design is basically done. We still have, uh, two screens to polish on the onboarding flow.", 31)
            + w("Marketing needs the final build by the twentieth for the press kit.", 72)
            + w("I'd rather not slip. Can we cut the export presets from this release?", 118)
        let me = w("Thanks everyone. I think, um, we we can make February third if QA starts Monday.", 16)
            + w("Yes, the presets can wait for the point release. I'll update the roadmap today.", 126)
            + w("Let's also book a dry run of the demo for next Thursday.", 141)
        m.lines = MeetingTranscript.merge(me: me, others: others)
        _ = MeetingEdit.cleanUp(&m, fillers: true, repeats: true, pauses: false)
        MeetingEdit.cut(&m, 52, 66, reason: .pause)
        m.markers = [MeetingMarker(time: 22, label: "Launch date: Feb 3"), MeetingMarker(time: 126, label: "Presets move to 1.1")]
        m.summary = MeetingSummary(
            overview: "The team agreed to target February 3 for the Q1 launch, provided QA starts on Monday. Export presets move to the point release so design can finish onboarding.",
            keyPoints: ["Design has two onboarding screens left to polish", "Marketing needs the final build by the 20th for the press kit", "Export presets are cut from the Q1 release"],
            decisions: ["Launch on February 3", "Export presets ship in the point release"],
            actionItems: ["You: update the roadmap today", "Design: finish the two onboarding screens", "You: book a demo dry run for next Thursday"])
        m.outputs = [MeetingOutput(kind: "Follow-up email", text: "Subject: Q1 launch — Feb 3\n\nHi all, thanks for the sync. We're launching on February 3 if QA starts Monday. Export presets move to the point release.\n\n• Roadmap update — me, today\n• Onboarding polish — design\n• Demo dry run — next Thursday")]
        m.folder = store.root.appendingPathComponent("snap-\(UUID().uuidString)")
        return m
    }

    static func fakePeaks(_ n: Int = 1200) -> [Float] {
        (0..<n).map { i in
            let x = Double(i)
            let speech = 0.55 + 0.35 * sin(x / 7) * sin(x / 31)
            let gap = (i % 97 < 9) || (i > 360 && i < 390) ? 0.06 : 1.0
            return Float(max(0.04, min(1, speech * gap + 0.08 * sin(x * 1.7))))
        }
    }

    func testMeetingsWindow() throws {
        try XCTSkipIf(out == nil)
        let store = MeetingStore()
        let m = MeetingSnapshotTests.demoMeeting(in: store)
        store.save(m)
        defer { try? FileManager.default.removeItem(at: m.folder) }
        let model = MeetingsModel(store: store)
        model.peaksProvider = { _ in MeetingSnapshotTests.fakePeaks() }
        model.selectedID = nil
        model.selectedID = m.id
        let w = MeetingsWindow(model: model, register: false)
        w.window.setContentSize(NSSize(width: 1180, height: 780))
        w.window.orderFront(nil)
        wait(0.8)
        XCTAssertFalse(model.peaks.isEmpty, "waveform loaded")
        model.seek(33.1)
        if let i = model.words.firstIndex(where: { $0.word.text == "two" }) { model.anchor = i; model.focus = i + 2 }
        model.status = "Removed 3 filler words, 1 repeat · 1.9 s shorter"
        wait(0.6)
        snap(w.window.contentView!, "ui-meetings-transcript")

        model.tab = .summary
        wait(0.5)
        snap(w.window.contentView!, "ui-meetings-summary")

        model.tab = .notes
        model.setNotes("Ask Linh about the press kit timeline.\nDry run: Thursday 10:00.")
        wait(0.4)
        snap(w.window.contentView!, "ui-meetings-notes")
        w.window.close()
    }

    func testMeetingsEmpty() throws {
        try XCTSkipIf(out == nil)
        let store = MeetingStore()
        store.reload()
        let model = MeetingsModel(store: store)
        model.selectedID = nil
        let w = MeetingsWindow(model: model, register: false)
        w.window.setContentSize(NSSize(width: 1000, height: 620))
        w.window.orderFront(nil)
        wait(0.6)
        snap(w.window.contentView!, "ui-meetings-empty")
        w.window.close()
    }

    func testRecordingBar() throws {
        try XCTSkipIf(out == nil)
        let hud = MeetingHUD(recorder: nil, others: true, you: true)
        hud.preview(elapsed: 1342, others: 0.72, you: 0.38, markers: 2)
        wait(0.3)
        snap(try XCTUnwrap(hud.view), "ui-meeting-bar")
        hud.close()
    }

    func testMeetingSettings() throws {
        try XCTSkipIf(out == nil)
        let h = NSHostingViewWrapper.make(SettingsView(query: "meeting"))
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 760), styleMask: [.titled], backing: .buffered, defer: false)
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        w.contentView = h
        w.orderFront(nil)
        wait(0.6)
        snap(h, "ui-settings-meetings")
        w.close()
    }
}
