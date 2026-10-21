import AVFoundation
import AppKit
import Speech
import SwiftUI
import XCTest
@testable import AtherScreenshot

// Bug reports (BugReport.swift): the bug logic, the picture, the logs, the bundle, the clip when the recording stops, the
// Vietnamese server transcription's seam and the app's side. Never asks for the microphone or speech recognition.
final class BugReportTests: XCTestCase {
    private var out: URL?

    override func setUp() {
        out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"].map { URL(fileURLWithPath: $0) }
        _ = NSApplication.shared
    }

    private func temp() throws -> URL {
        let u = FileManager.default.temporaryDirectory.appendingPathComponent("ather-bug-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    // Words said one after another from `t`, `step` seconds apart.
    private func said(_ t: Double, _ text: String, step: Double = 0.3) -> [CaptionWord] {
        text.split(separator: " ").enumerated().map { i, w in CaptionWord(start: t + Double(i) * step, end: t + Double(i) * step + step * 0.8, text: String(w)) }
    }

    private func text(_ u: URL) -> String { (try? String(contentsOf: u, encoding: .utf8)) ?? "" }
    private func object(_ u: URL) -> [String: Any] { ((try? Data(contentsOf: u)).flatMap { try? JSONSerialization.jsonObject(with: $0) }) as? [String: Any] ?? [:] }

    // A stand-in for a game: sky, ground, a closed door and a health bar.
    private func scene(_ w: Int, _ h: Int) -> CGImage {
        let ctx = makeContext(width: w, height: h)!
        let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [Theme.rgb(40, 90, 170).cgColor, Theme.rgb(120, 170, 230).cgColor] as CFArray, locations: [0, 1])!
        ctx.drawLinearGradient(g, start: .zero, end: CGPoint(x: 0, y: CGFloat(h) * 2 / 3), options: [])
        ctx.setFillColor(Theme.rgb(70, 110, 50).cgColor)
        ctx.fill(CGRect(x: 0, y: CGFloat(h) * 2 / 3, width: CGFloat(w), height: CGFloat(h) / 3))
        let u = CGFloat(h) / 100
        ctx.setFillColor(Theme.rgb(110, 70, 40).cgColor)
        ctx.fill(CGRect(x: CGFloat(w) * 0.42, y: CGFloat(h) * 0.3, width: CGFloat(w) * 0.16, height: CGFloat(h) * 0.37))
        ctx.setFillColor(Theme.rgb(30, 30, 30, 0.8).cgColor)
        ctx.fill(CGRect(x: 3 * u, y: 3 * u, width: 30 * u, height: 4 * u))
        ctx.setFillColor(Theme.rgb(220, 60, 60).cgColor)
        ctx.fill(CGRect(x: 3.5 * u, y: 3.5 * u, width: 20 * u, height: 3 * u))
        return ctx.makeImage()!
    }

    private func pixel(_ img: CGImage, _ x: Int, _ y: Int) -> (r: Int, g: Int, b: Int) {
        let c = TestMedia.rgb(img, x, y)
        return (Int((c.0 * 255).rounded()), Int((c.1 * 255).rounded()), Int((c.2 * 255).rounded()))
    }

    // MARK: the bug logic

    func testCommandsAndWordsByVoice() {
        var tr = BugTracker()
        XCTAssertTrue(tr.tick(0, recording: 0, rec: 0).isEmpty)
        var ev = tr.heard(said(10, "bug start"))
        XCTAssertEqual(ev.map(\.kind), [.started])
        XCTAssertEqual(ev.first?.bug.start, 10)
        XCTAssertTrue(tr.isOpen)
        XCTAssertTrue(tr.heard(said(11, "the door does not open.")).isEmpty)
        XCTAssertEqual(tr.heard(said(13, "bug start")).map(\.kind), [.ignored])  // one is open
        ev = tr.heard(said(14, "after the boss Bug stop. thanks"))  // a command inside dictation
        XCTAssertEqual(ev.map(\.kind), [.stopped])
        XCTAssertEqual(ev.first?.bug.description, "the door does not open. after the boss")  // no command words, nothing after it
        XCTAssertEqual(ev.first?.bug.stop ?? 0, 14.9, accuracy: 1e-9)
        XCTAssertFalse(tr.busy)
        XCTAssertTrue(tr.heard(said(20, "bug stop")).isEmpty)  // nothing open
        ev = tr.heard(said(30, "hello bug start it crashes bug cancel later"))
        XCTAssertEqual(ev.map(\.kind), [.started, .cancelled])
        XCTAssertEqual(ev.last?.bug.description, "it crashes")
        XCTAssertFalse(tr.busy)
        ev = tr.heard([CaptionWord(start: 40, end: 40.2, text: "Bug,"), CaptionWord(start: 40.3, end: 40.5, text: "START!")])
        XCTAssertEqual(ev.map(\.kind), [.started])
        ev = tr.heard(said(41, "BUG Stop"))
        XCTAssertEqual(ev.map(\.kind), [.stopped])
        XCTAssertEqual(ev.first?.bug.words.count, 0)
        XCTAssertNil(BugWords.findCommand(said(0, "a bugs start bug")))
        XCTAssertEqual(BugWords.findCommand(said(0, "bug bug cancel"))?.at, 1)
    }

    func testShortcutKeepsLateWordsAndStopsItself() {
        var tr = BugTracker()
        _ = tr.tick(0, recording: 0, rec: 0)
        XCTAssertEqual(tr.toggle(1).map(\.kind), [.started])
        _ = tr.heard(said(1.5, "water is missing"))
        XCTAssertTrue(tr.toggle(4).isEmpty)  // stopped: waits for words still on their way
        XCTAssertTrue(!tr.isOpen && tr.busy)
        _ = tr.heard([CaptionWord(start: 3.7, end: 3.9, text: "again"), CaptionWord(start: 4.2, end: 4.4, text: "later")])
        XCTAssertTrue(tr.tick(5, recording: 0, rec: 0).isEmpty)
        var ev = tr.tick(4 + BugTracker.grace, recording: 0, rec: 0)
        XCTAssertEqual(ev.map(\.kind), [.stopped])
        XCTAssertEqual(ev.first?.bug.description, "water is missing again")
        XCTAssertEqual(ev.first?.bug.stop, 4)
        // Left open, a bug stops itself after 2 minutes.
        XCTAssertEqual(tr.toggle(100).count, 1)
        XCTAssertTrue(tr.tick(219, recording: 0, rec: 0).isEmpty)
        XCTAssertTrue(tr.isOpen)
        XCTAssertTrue(tr.tick(220, recording: 0, rec: 0).isEmpty)
        XCTAssertFalse(tr.isOpen)
        ev = tr.tick(221.6, recording: 0, rec: 0)
        XCTAssertEqual(ev.map(\.kind), [.stopped])
        XCTAssertEqual(ev.first?.bug.autoStopped, true)
        XCTAssertEqual(ev.first?.bug.stop, 220)
        // "Bug start" while the last one waits for its words: that one is saved first.
        _ = tr.toggle(400)
        _ = tr.toggle(402)
        ev = tr.heard(said(402.5, "bug start"))
        XCTAssertEqual(ev.map(\.kind), [.stopped, .started])
        XCTAssertEqual(ev.first?.bug.stop, 402)
    }

    func testTimesFollowTheRecordingAndItsPauses() {
        var tr = BugTracker()
        // Recording 7 starts at t = 100, is paused from 112 to 117 and ends at 126.
        func rec(_ t: Double) -> Double { t < 112 ? t - 100 : t < 117 ? 12 : t - 105 }
        var t = 99.0
        while t <= 145 {
            let on = t >= 100 && t <= 126
            _ = tr.tick(t, recording: on ? 7 : 0, rec: on ? rec(t) : 0)
            t += 0.25
        }
        XCTAssertEqual(tr.heard(said(110, "bug start")).first?.bug.recording, 7)
        _ = tr.heard(said(111, "jump"))
        _ = tr.heard(said(118, "falls through"))
        let b = tr.heard(said(120, "bug stop")).first?.bug
        XCTAssertEqual(b?.recStart ?? 0, 10, accuracy: 1e-9)
        XCTAssertEqual(b?.recStop ?? 0, 15, accuracy: 1e-9)  // the 5 paused seconds aren't in the recording
        let starts = b?.recWords.map(\.start) ?? []
        XCTAssertEqual(starts.count, 3)
        for (got, want) in zip(starts, [11, 13, 13.3]) { XCTAssertEqual(got, want, accuracy: 1e-9) }
        _ = tr.heard(said(124, "bug start"))
        let late = tr.heard(said(130, "bug stop")).first?.bug  // past the recording's end: it ends there
        XCTAssertEqual(late?.recStart ?? 0, 19, accuracy: 1e-9)
        XCTAssertTrue((21...21.25).contains(late?.recStop ?? 0))
        XCTAssertEqual(tr.heard(said(140, "bug start")).first?.bug.recording, 0)
    }

    func testTranscriptPassesWordsOnOnceSettled() {
        var f = BugTranscript()
        let w = said(10, "bug start the door")
        XCTAssertEqual(f.settled(w, now: 10.5, final: false).map(\.text), [])
        XCTAssertEqual(f.settled(w, now: 11.6, final: false).map(\.text), ["bug", "start"])  // a second behind the audio
        var revised = w
        revised[3].text = "doors"
        XCTAssertEqual(f.settled(revised, now: 11.7, final: false).map(\.text), [])
        XCTAssertEqual(f.settled(revised, now: 11.7, final: true).map(\.text), ["the", "doors"])
        XCTAssertTrue(f.settled(revised, now: 30, final: true).isEmpty)  // never twice
    }

    // MARK: the picture

    func testOverlayBarHeaderAndLines() throws {
        let ctx = makeContext(width: 1280, height: 720)!
        ctx.setFillColor(Theme.rgb(200, 200, 200).cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: 1280, height: 720))
        let shot = ctx.makeImage()!
        let header = "BUG · 2026-10-21 14:03:22 · Game"
        let long = String(repeating: "The door still does not open after the boss dies. ", count: 60)
        let (none, a) = try XCTUnwrap(BugOverlay.draw(shot, header: header, description: ""))
        let (some, b) = try XCTUnwrap(BugOverlay.draw(shot, header: header, description: "The door does not open after the boss dies."))
        let (_, c) = try XCTUnwrap(BugOverlay.draw(shot, header: header, description: long))
        XCTAssertEqual(a.bar.minX, 0)
        XCTAssertEqual(a.bar.width, 1280)
        XCTAssertEqual(a.bar.maxY, 720)
        XCTAssertGreaterThan(a.bar.minY, 600)
        XCTAssertEqual([a.lines, b.lines, c.lines], [0, 1, BugOverlay.maxLines])
        XCTAssertTrue(c.cut && !b.cut)
        XCTAssertTrue(a.bar.height < b.bar.height && b.bar.height < c.bar.height && c.bar.height < 360)
        XCTAssertEqual(pixel(some, 10, Int(b.bar.minY) - 2).r, 200)  // above the bar: as it was
        let edge = pixel(some, 2, 718)
        XCTAssertTrue((20..<80).contains((edge.r + edge.g + edge.b) / 3))  // dark, translucent
        func count(_ img: CGImage, _ r: CGRect, _ f: ((r: Int, g: Int, b: Int)) -> Bool) -> Int {
            var n = 0
            for y in stride(from: Int(r.minY), to: Int(r.maxY), by: 2) { for x in stride(from: Int(r.minX), to: Int(r.maxX), by: 2) where f(pixel(img, x, y)) { n += 1 } }
            return n
        }
        XCTAssertGreaterThan(count(some, b.bar) { $0.r > 235 && $0.g > 235 && $0.b > 235 }, 60)  // the description in white
        XCTAssertLessThan(count(none, a.bar) { $0.r > 235 && $0.g > 235 && $0.b > 235 }, 5)  // header only
        XCTAssertGreaterThan(count(none, a.bar) { $0.g > 200 && $0.r > 150 && $0.b < 90 }, 3)  // "BUG" in lime
        if let out {
            let img = scene(1600, 900)
            let snap = try XCTUnwrap(BugOverlay.draw(img, header: header, description: "The door does not open after the boss dies. I tried it twice, and the key is still in the inventory. It happens in level three only, right after the cut scene ends and the music changes.")).0
            try snap.pngData()!.write(to: out.appendingPathComponent("mac-bug-overlay.png"))
        }
    }

    // MARK: logs

    func testLogsRecentNewestTailAndOpenFiles() throws {
        let dir = try temp(), logs = dir.appendingPathComponent("game")
        let fm = FileManager.default
        for d in ["old", "recent", "other"] { try fm.createDirectory(at: logs.appendingPathComponent(d), withIntermediateDirectories: true) }
        func make(_ rel: String, _ bytes: Int, age: TimeInterval) throws {
            var s = ""
            var i = 1
            while s.utf8.count < bytes { s += String(format: "line %06d\n", i); i += 1 }
            let u = logs.appendingPathComponent(rel)
            try Data(s.utf8.prefix(bytes)).write(to: u)
            try fm.setAttributes([.modificationDate: Date().addingTimeInterval(-age)], ofItemAtPath: u.path)
        }
        try make("old/a.log", 100, age: 7200)
        try make("old/b.log", 100, age: 3600)  // the newest, though older than 30 minutes
        try make("recent/c.log", 100, age: 0)
        try make("recent/d.log", 100, age: 600)
        try make("recent/e.log", 100, age: 7200)
        try make("recent/notes.txt", 50, age: 0)
        try make("other/c.log", 40, age: 0)
        try make("big.log", 3 << 20, age: 0)
        // The game's own log, still open for writing.
        let gameURL = logs.appendingPathComponent("game.log")
        fm.createFile(atPath: gameURL.path, contents: nil)
        let game = try FileHandle(forWritingTo: gameURL)
        game.write(Data("started\nloaded level 3\n".utf8))
        setenv("ATHER_TEST_LOGS", logs.path, 1)
        let paths = "$ATHER_TEST_LOGS/game.log; \(logs.path)/old/ ;\"\(logs.path)/recent/*.log\";\(logs.path)/big.log;/nope/x.log;;\(logs.path)/none/*.log"
        var missing: [String] = []
        let found = BugLogs.find(paths, now: Date(), missing: &missing)
        XCTAssertEqual(found.map { String($0.path.dropFirst(logs.path.count + 1)) }, ["game.log", "old/b.log", "recent/c.log", "recent/d.log", "big.log"])
        XCTAssertEqual(missing, ["/nope/x.log", "\(logs.path)/none/*.log"])
        let copied = BugLogs.copy(found, to: dir.appendingPathComponent("logs"), maxBytes: 1 << 20)
        XCTAssertEqual(copied.count, 5)
        XCTAssertEqual(copied.first, BugLog(source: gameURL.path, file: "logs/game.log", size: 23, copied: 23))  // open for writing, still copied
        XCTAssertEqual(text(dir.appendingPathComponent("logs/game.log")), "started\nloaded level 3\n")
        let big = copied.last!
        XCTAssertTrue(big.truncated && big.size == 3 << 20 && big.copied <= 1 << 20 && big.copied > (1 << 20) - 64)
        let tail = text(dir.appendingPathComponent("logs/big.log"))
        XCTAssertEqual(UInt64(tail.utf8.count), big.copied)
        XCTAssertTrue(tail.hasPrefix("line "))  // from the start of a line
        try game.close()
        let twice = BugLogs.copy([logs.appendingPathComponent("recent/c.log"), logs.appendingPathComponent("other/c.log")], to: dir.appendingPathComponent("logs2"), maxBytes: 1 << 20)
        XCTAssertEqual(twice.map(\.file), ["logs/c.log", "logs/c (2).log"])
        XCTAssertEqual(BugLogs.expand("~/x"), NSHomeDirectory() + "/x")
        XCTAssertEqual(BugLogs.expand("%ATHER_TEST_LOGS%/y"), logs.path + "/y")
    }

    // MARK: the bundle

    private func bundle(_ dir: URL, words: String = "The door does not open after the boss dies. Tried twice.", recording: Int = 3) -> BugBundle {
        var bug = Bug()
        bug.id = 1
        bug.start = 50
        bug.words = said(50.5, words)
        bug.stop = 54
        bug.recording = recording
        if recording != 0 {
            bug.recStart = 12
            bug.recStop = 16
            bug.recWords = bug.words.map { CaptionWord(start: $0.start - 38, end: $0.end - 38, text: $0.text) }
        }
        let wall = Date(timeIntervalSince1970: 1_792_000_000)
        let b = BugBundle(bug: bug, folder: dir.appendingPathComponent("Bugs").appendingPathComponent(BugBundle.folderName(wall, bug.description)))
        b.id = "test-bug"
        b.wallStart = wall
        b.app = "Game"
        b.window = "Level 3"
        b.area = CGRect(x: 0, y: 0, width: 640, height: 360)
        b.screenshot = scene(640, 360)
        b.clip = recording != 0 ? .pending : .none
        return b
    }

    func testBundleFilesClipAndNote() async throws {
        let dir = try temp()
        try Data(String(repeating: "x", count: 300).utf8).write(to: dir.appendingPathComponent("game.log"))
        let b = bundle(dir)
        XCTAssertTrue(b.folder.lastPathComponent.hasSuffix(" The door does not open after the boss"))
        let tone = (0..<16000).map { Int16(8000 * sin(Double($0) * 0.15)) }
        try b.begin(voice: tone)
        b.finish(logPaths: dir.appendingPathComponent("game.log").path + ";" + dir.appendingPathComponent("missing.log").path, logMaxBytes: 1 << 20)
        for f in ["bug.png", "screenshot.png", "voice.wav", "logs/game.log", "bug.md", "bug.json"] {
            XCTAssertTrue(FileManager.default.fileExists(atPath: b.folder.appendingPathComponent(f).path), f)
        }
        // screenshot.png as it was; bug.png with the bar
        let clean = try XCTUnwrap(CGImage.load(b.folder.appendingPathComponent("screenshot.png")))
        XCTAssertEqual(clean.dataProvider?.data as Data?, try XCTUnwrap(CGImage.load(b.folder.appendingPathComponent("screenshot.png"))).dataProvider?.data as Data?)
        XCTAssertTrue(pixel(clean, 5, 300) == pixel(b.screenshot!, 5, 300))
        let over = try XCTUnwrap(CGImage.load(b.folder.appendingPathComponent("bug.png")))
        XCTAssertEqual(over.width, 640)
        XCTAssertTrue(pixel(over, 5, 5) == pixel(b.screenshot!, 5, 5))
        XCTAssertFalse(pixel(over, 2, 357) == pixel(b.screenshot!, 2, 357))
        let wav = try Data(contentsOf: b.folder.appendingPathComponent("voice.wav"))
        XCTAssertEqual(wav.count, 44 + 32000)
        XCTAssertEqual(String(decoding: wav.prefix(4), as: UTF8.self), "RIFF")
        XCTAssertEqual(wav.subdata(in: 24..<28).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }, 16000)
        var md = text(b.folder.appendingPathComponent("bug.md"))
        XCTAssertTrue(md.hasPrefix("# The door does not open after the boss dies\n"))
        XCTAssertTrue(md.contains("**App:** Game, “Level 3”"))
        XCTAssertTrue(md.contains("**Recording:** the screen recording, 0:12.0 to 0:16.0"))
        XCTAssertTrue(md.contains("**Clip:** when the recording stops"))
        XCTAssertTrue(md.contains("The door does not open after the boss dies. Tried twice."))
        XCTAssertTrue(md.contains("`logs/game.log`: from `\(dir.path)/game.log`, 300 bytes"))
        XCTAssertTrue(md.contains("Not found: `\(dir.path)/missing.log`"))
        var j = object(b.folder.appendingPathComponent("bug.json"))
        XCTAssertEqual(j["format"] as? Int, 1)
        XCTAssertEqual(j["id"] as? String, "test-bug")
        XCTAssertEqual(j["seconds"] as? Double, 4)
        let words = j["words"] as? [[String: Any]] ?? []
        XCTAssertEqual(words.count, 11)
        XCTAssertEqual(words.first?["start"] as? Double, 0.5)
        XCTAssertEqual(words.first?["recording_time"] as? Double, 12.5)
        XCTAssertEqual((j["foreground"] as? [String: Any])?["app"] as? String, "Game")
        XCTAssertEqual(((j["screenshot"] as? [String: Any])?["area"] as? [String: Any])?["width"] as? Double, 640)
        XCTAssertEqual((j["recording"] as? [String: Any])?["start"] as? Double, 12)
        XCTAssertEqual((j["clip"] as? [String: Any])?["state"] as? String, "pending")
        XCTAssertEqual(j["voice"] as? String, "voice.wav")
        let log = (j["logs"] as? [[String: Any]])?.first
        XCTAssertEqual(log?["size"] as? Int, 300)
        XCTAssertEqual(log?["truncated"] as? Bool, false)
        XCTAssertEqual((j["missing_logs"] as? [String])?.count, 1)

        // The recording stops: the clip with its captions, and the bug as a note on the recording.
        let rec = try await TestMedia.colors(w: 320, h: 180, seconds: 20, colors: Array(repeating: [NSColor.red, .green, .blue], count: 7).flatMap { $0 }, tone: true)
        await b.finishClip(Recorder.Outcome(url: rec, duration: 20, error: nil), preRoll: 10)
        XCTAssertEqual(b.clip, .saved, b.clipNote)
        XCTAssertEqual(b.clipFrom, 2)
        XCTAssertEqual(b.clipTo, 16)
        let clip = AVURLAsset(url: b.folder.appendingPathComponent("clip.mp4"))
        let video = try await clip.loadTracks(withMediaType: .video).first
        let codec = try await video?.load(.formatDescriptions).first.map { CMFormatDescriptionGetMediaSubType($0) }
        XCTAssertEqual(codec, kCMVideoCodecType_H264)
        let duration = try await video?.load(.timeRange).duration.seconds ?? 0
        XCTAssertEqual(duration, 14, accuracy: 1 / 30.0 + 1e-3)  // 10 s before "bug start" to "bug stop", within a frame
        func dark(_ img: CGImage) -> Int {
            var n = 0
            for y in stride(from: 120, to: 178, by: 2) { for x in 60..<260 { let p = pixel(img, x, y); n += max(p.r, p.g, p.b) < 153 ? 1 : 0 } }
            return n
        }
        let before = try await TestMedia.frame(clip, at: 5), during = try await TestMedia.frame(clip, at: 11.2)
        XCTAssertEqual(dark(before), 0)
        XCTAssertGreaterThan(dark(during), 20)  // "The door…" from 12.5 s in the recording, 10.5 s into the clip
        if let out { try during.pngData()!.write(to: out.appendingPathComponent("mac-bug-clip-frame.png")) }
        let notes = (object(URL(fileURLWithPath: rec.path + ".notes.json"))["notes"] as? [[String: Any]]) ?? []
        XCTAssertEqual(notes.count, 1)
        XCTAssertEqual(notes.first?["kind"] as? String, "issue")
        XCTAssertEqual(notes.first?["time"] as? Double, 12)
        XCTAssertEqual(notes.first?["endTime"] as? Double, 16)
        XCTAssertEqual(notes.first?["text"] as? String, "Bug: " + b.bug.description)
        md = text(b.folder.appendingPathComponent("bug.md"))
        XCTAssertTrue(md.contains("**Recording:** `\(rec.lastPathComponent)`, 0:12.0 to 0:16.0"))
        XCTAssertTrue(md.contains("**Clip:** `clip.mp4`, 0:02.0 to 0:16.0 of the recording, with captions"))
        j = object(b.folder.appendingPathComponent("bug.json"))
        XCTAssertEqual((j["clip"] as? [String: Any])?["state"] as? String, "saved")
        XCTAssertEqual((j["recording"] as? [String: Any])?["path"] as? String, rec.path)

        // A bug 4 s into the recording: the pre-roll stops at its start. A second note joins the first.
        let early = bundle(dir, words: "early")
        early.folder = dir.appendingPathComponent("Bugs/early")
        early.bug.recStart = 4
        early.bug.recStop = 7
        early.bug.recWords = []
        try early.begin(voice: [])
        early.finish(logPaths: "", logMaxBytes: 1 << 20)
        await early.finishClip(Recorder.Outcome(url: rec, duration: 20, error: nil), preRoll: 10)
        XCTAssertEqual(early.clip, .saved)
        XCTAssertEqual(early.clipFrom, 0)
        let short = try await AVURLAsset(url: early.folder.appendingPathComponent("clip.mp4")).loadTracks(withMediaType: .video).first?.load(.timeRange).duration.seconds ?? 0
        XCTAssertEqual(short, 7, accuracy: 1 / 30.0 + 1e-3)
        XCTAssertEqual((object(URL(fileURLWithPath: rec.path + ".notes.json"))["notes"] as? [[String: Any]])?.count, 2)
    }

    func testWithoutARecordingThereIsNoClip() async throws {
        let dir = try temp()
        let b = bundle(dir, words: "", recording: 0)
        XCTAssertEqual(b.folder.lastPathComponent, BugBundle.folderName(b.wallStart, "x").replacingOccurrences(of: " x", with: ""))  // just the date and time
        XCTAssertEqual(b.folder.lastPathComponent.count, 19)
        try b.begin(voice: [])
        b.finish(logPaths: "", logMaxBytes: 1 << 20)
        XCTAssertFalse(FileManager.default.fileExists(atPath: b.folder.appendingPathComponent("clip.mp4").path))
        let md = text(b.folder.appendingPathComponent("bug.md"))
        XCTAssertTrue(md.hasPrefix("# Bug at "))
        XCTAssertTrue(md.contains("**Clip:** none: nothing was being recorded"))
        XCTAssertFalse(md.contains("**Recording:**"))
        XCTAssertTrue(md.contains("Nothing: voice commands were off."))
        let j = object(b.folder.appendingPathComponent("bug.json"))
        XCTAssertTrue(j["recording"] is NSNull && j["voice"] is NSNull)
        XCTAssertEqual(j["clip"] as? [String: String], ["state": "none", "note": "nothing was being recorded"])
        // A bug in a recording that was then discarded: no clip, no note.
        let d = bundle(dir, words: "discarded")
        try d.begin(voice: [])
        await d.finishClip(Recorder.Outcome(url: nil, duration: 0, error: nil), preRoll: 10)
        XCTAssertEqual(d.clip, .none)
        XCTAssertTrue(text(d.folder.appendingPathComponent("bug.md")).contains("**Clip:** none: the recording was discarded"))
    }

    // MARK: Vietnamese with Apple's servers (D10)

    private final class FakeTranscriber: BugTranscriber {
        var calls: [(url: URL, locale: String, onDevice: Bool)] = []
        var fail = false
        func transcribe(_ wav: URL, locale: String, onDevice: Bool) async throws -> [CaptionWord] {
            calls.append((wav, locale, onDevice))
            if fail { throw VideoExport.Failure.failed("The server didn't answer.") }
            return [CaptionWord(start: 0.5, end: 0.9, text: "Cửa"), CaptionWord(start: 1.0, end: 1.4, text: "không"), CaptionWord(start: 1.5, end: 1.9, text: "mở.")]
        }
    }

    func testVietnameseGoesToTheServerOnlyWhenOnAndOnlyVoiceWav() async throws {
        let dir = try temp()
        let fake = FakeTranscriber()
        // Off: nothing is sent (the app only calls it with the setting on).
        let off = bundle(dir, words: "the door")
        try off.begin(voice: [1, 2, 3])
        off.finish(logPaths: "", logMaxBytes: 1 << 20)
        XCTAssertTrue(fake.calls.isEmpty)
        XCTAssertNil(object(off.folder.appendingPathComponent("bug.json"))["transcription"])
        // On: voice.wav as vi-VN, on the server; the answer becomes the description and the captions.
        let on = bundle(dir, words: "the door")
        on.voiceStart = 50.2
        try on.begin(voice: [Int16](repeating: 100, count: 32000))
        await on.transcribeVietnamese(fake) { $0 - 38 }
        XCTAssertEqual(fake.calls.count, 1)
        XCTAssertEqual(fake.calls.first?.locale, "vi-VN")
        XCTAssertEqual(fake.calls.first?.onDevice, false)
        XCTAssertEqual(fake.calls.first?.url, on.folder.appendingPathComponent("voice.wav"))
        XCTAssertEqual(on.bug.description, "Cửa không mở.")
        XCTAssertEqual(on.bug.words.first?.start ?? 0, 50.7, accuracy: 1e-9)
        XCTAssertEqual(on.bug.recWords.first?.start ?? 0, 12.7, accuracy: 1e-9)
        on.finish(logPaths: "", logMaxBytes: 1 << 20)
        let t = object(on.folder.appendingPathComponent("bug.json"))["transcription"] as? [String: Any]
        XCTAssertEqual(t?["locale"] as? String, "vi-VN")
        XCTAssertEqual(t?["on_device"] as? Bool, false)
        XCTAssertNil(t?["error"])
        XCTAssertTrue(text(on.folder.appendingPathComponent("bug.md")).contains("Cửa không mở."))
        // The server fails: the on-device words stay, and bug.json says why.
        fake.fail = true
        let failed = bundle(dir, words: "the door does not open")
        try failed.begin(voice: [1, 2, 3])
        await failed.transcribeVietnamese(fake) { $0 }
        failed.finish(logPaths: "", logMaxBytes: 1 << 20)
        XCTAssertEqual(failed.bug.description, "the door does not open")
        XCTAssertEqual((object(failed.folder.appendingPathComponent("bug.json"))["transcription"] as? [String: Any])?["error"] as? String, "The server didn't answer.")
        // No narration, nothing to send.
        let silent = bundle(dir, words: "quiet")
        try silent.begin(voice: [])
        await silent.transcribeVietnamese(fake) { $0 }
        XCTAssertEqual(fake.calls.count, 2)
    }

    // The voice commands' requests never leave the Mac.
    func testVoiceCommandsStayOnTheMac() throws {
        let live = BugSpeech.liveRequest()
        XCTAssertTrue(live.requiresOnDeviceRecognition)
        XCTAssertTrue(live.shouldReportPartialResults)
        XCTAssertEqual(live.contextualStrings, ["bug start", "bug stop", "bug cancel"])
        XCTAssertTrue(BugSpeech.fileRequest(URL(fileURLWithPath: "/tmp/x.wav")).requiresOnDeviceRecognition)
        XCTAssertEqual(BugSpeech.resample([Float](repeating: 0.5, count: 48000), from: 48000).count, 16000)
    }

    // "Bug start … the door does not open … bug stop" made by `say`, through on-device recognition and the bug logic.
    // Only when this process may already use speech recognition: tests never ask for it.
    func testRecognitionFromASaidFile() async throws {
        try XCTSkipIf(SFSpeechRecognizer.authorizationStatus() != .authorized,
                      "Speech recognition isn't allowed for the test runner (status \(SFSpeechRecognizer.authorizationStatus().rawValue)): owner checks it in the app.")
        let rec = try XCTUnwrap(SFSpeechRecognizer(locale: Locale(identifier: "en-US")))
        try XCTSkipIf(!rec.supportsOnDeviceRecognition, "No on-device recognition for en-US on this Mac.")
        let aiff = try temp().appendingPathComponent("said.aiff")
        let say = Process()
        say.executableURL = URL(fileURLWithPath: "/usr/bin/say")
        say.arguments = ["-o", aiff.path, "Bug start. [[slnc 900]] The door does not open. [[slnc 900]] Bug stop."]
        try say.run()
        say.waitUntilExit()
        let words = try await BugSpeech.words(rec, BugSpeech.fileRequest(aiff))
        var tr = BugTracker()
        var feed = BugTranscript()
        let events = tr.heard(feed.settled(words, now: 1e9, final: true))
        XCTAssertEqual(events.map(\.kind), [.started, .stopped], words.map(\.text).joined(separator: " "))
        let said = events.last?.bug.description.lowercased() ?? ""
        XCTAssertGreaterThanOrEqual(["door", "does", "not", "open"].filter { said.contains($0) }.count, 3, said)
        XCTAssertFalse(said.contains("bug"))
    }

    // MARK: the app's side

    // Waits on the main actor, so its tasks and the main run loop's timers go on meanwhile.
    @MainActor
    private func wait(_ seconds: Double = 20, until done: () -> Bool = { false }) async {
        let end = Date().addingTimeInterval(seconds)
        while !done() && Date() < end { try? await Task.sleep(nanoseconds: 20_000_000) }
    }

    @MainActor
    func testBugReportsInTheApp() async throws {
        let dir = try temp()
        try Data("game started\n".utf8).write(to: dir.appendingPathComponent("game.log"))
        let r = BugReports.shared
        var opened: URL?
        let start = bugClock()
        r.grab = { _, _ in self.scene(800, 450) }  // never ScreenCaptureKit in tests
        r.recordingNow = { BugReports.RecordingNow(serial: 42, seconds: 30 + bugClock() - start, area: CGRect(x: 0, y: 0, width: 800, height: 450)) }
        r.showFolder = { opened = $0 }
        r.configure(BugReports.Options(folder: dir, logPaths: dir.appendingPathComponent("game.log").path, hotkey: "Ctrl+Alt+B"))
        XCTAssertFalse(r.listening)  // listening is opt-in
        r.toggle()
        await wait(0.3)
        r.toggle()
        await wait { r.lastFolder?.path.hasPrefix(dir.path) == true && r.busy == 0 }
        let first = try XCTUnwrap(r.lastFolder)
        for f in ["bug.png", "screenshot.png", "logs/game.log", "bug.md", "bug.json"] { XCTAssertTrue(FileManager.default.fileExists(atPath: first.appendingPathComponent(f).path), f) }
        XCTAssertTrue(text(first.appendingPathComponent("bug.md")).contains("**Clip:** when the recording stops"))
        r.recordingNow = { BugReports.RecordingNow() }
        r.toggle()
        await wait(0.2)
        r.toggle()
        await wait { r.lastFolder != first && r.busy == 0 }
        let second = try XCTUnwrap(r.lastFolder)
        XCTAssertNotEqual(second, first)
        XCTAssertTrue(text(second.appendingPathComponent("bug.md")).contains("**Clip:** none: nothing was being recorded"))
        // The recording stops: the first bug gets its clip and note.
        let rec = try await TestMedia.colors(w: 160, h: 90, seconds: 34, colors: Array(repeating: [NSColor.red, .green, .blue], count: 12).flatMap { $0 })
        r.recordingFinished(serial: 42, Recorder.Outcome(url: rec, duration: 34, error: nil))
        await wait(60) { r.busy == 0 }
        XCTAssertTrue(FileManager.default.fileExists(atPath: first.appendingPathComponent("clip.mp4").path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: rec.path + ".notes.json"))
        XCTAssertTrue(text(first.appendingPathComponent("bug.md")).contains("**Clip:** `clip.mp4`, 0:20."))
        XCTAssertFalse(FileManager.default.fileExists(atPath: second.appendingPathComponent("clip.mp4").path))
        // Copy last bug report: onto its own pasteboard here, never the user's clipboard.
        let pb = NSPasteboard(name: NSPasteboard.Name("ather-test-\(UUID().uuidString)"))
        XCTAssertTrue(r.copyLast(to: pb))
        let urls = pb.readObjects(forClasses: [NSURL.self]) as? [URL] ?? []
        XCTAssertEqual(urls.map(\.lastPathComponent), ["bug.png", "bug.md", "game.log"])
        XCTAssertEqual(urls.first?.deletingLastPathComponent().path, second.path)
        pb.releaseGlobally()
        r.openFolder()
        XCTAssertEqual(opened?.path, dir.appendingPathComponent("Bugs").path)
        r.recordingNow = { BugReports.RecordingNow() }
        r.grab = { r, cursor in try? await Capture.rect(r, cursor: cursor) }
        r.showFolder = { Output.open($0) }
    }

    // MARK: settings and commands

    func testSettingsShortcutAndPalette() throws {
        XCTAssertEqual(Settings.defaults["VoiceCommands"] as? Bool, false)
        XCTAssertEqual(Settings.defaults["CloudVietnamese"] as? Bool, false)
        XCTAssertEqual(Settings.defaults["LogPaths"] as? String, "")
        XCTAssertEqual(Settings.defaults["PreRollSeconds"] as? Int, 10)
        XCTAssertEqual(Settings.defaults["LogMaxMB"] as? Int, 20)
        XCTAssertEqual(cmdDef(.bugReport).hotkey, "Ctrl+Alt+B")
        XCTAssertEqual(kCmds.filter { Hotkey.parse($0.hotkey) == Hotkey.parse("Ctrl+Alt+B") }.map(\.cmd), [.bugReport])  // a shortcut of its own
        XCTAssertEqual(cmdDef(.copyLastBug).title, "Copy last bug report")
        XCTAssertEqual(cmdDef(.openBugsFolder).title, "Open bug reports folder")
        guard let out else { return }
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 760), styleMask: [.titled], backing: .buffered, defer: false)
        w.isReleasedWhenClosed = false
        w.appearance = NSAppearance(named: .darkAqua)
        let h = NSHostingView(rootView: SettingsView(query: "bug"))
        w.contentView = h
        w.orderFront(nil)
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))
        h.layoutSubtreeIfNeeded()
        h.display()
        let rep = try XCTUnwrap(h.bitmapImageRepForCachingDisplay(in: h.bounds))
        h.cacheDisplay(in: h.bounds, to: rep)
        try rep.representation(using: .png, properties: [:])?.write(to: out.appendingPathComponent("mac-settings-bug-reports.png"))
        w.close()
    }

    // Markers and bug notes share one sidecar; one that can't be read is left alone.
    func testVideoNotesJoinTheOnesThere() throws {
        let dir = try temp()
        let video = dir.appendingPathComponent("a.mp4")
        try Data().write(to: video)
        let side = URL(fileURLWithPath: video.path + ".notes.json")
        XCTAssertNotNil(VideoNotes.add([VideoNotes.New(time: 2, kind: "note", text: "mine", author: "me")], to: video, fps: 30))
        XCTAssertNotNil(VideoNotes.add([VideoNotes.New(time: 1, end: 3, kind: "issue", text: "Bug: x", author: "Bug report")], to: video, fps: 30))
        let notes = object(side)["notes"] as? [[String: Any]] ?? []
        XCTAssertEqual(notes.map { $0["text"] as? String }, ["Bug: x", "mine"])
        XCTAssertEqual(notes.first?["endFrame"] as? Int, 90)
        XCTAssertNotEqual(notes.first?["id"] as? Int, notes.last?["id"] as? Int)
        try Data("{damaged".utf8).write(to: side)
        XCTAssertNil(VideoNotes.add([VideoNotes.New(time: 1, kind: "note", text: "y", author: "t")], to: video, fps: 30))
        XCTAssertEqual(text(side), "{damaged")
    }
}
