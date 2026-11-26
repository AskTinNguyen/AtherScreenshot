import AVFoundation
import XCTest
@testable import AtherScreenshot

// Meetings: transcript lines and echo, exports, the store, mixing, text-based editing, clean-up, edited-audio export,
// the agent tools, and (live) real speech through the real transcriber.
final class MeetingTests: XCTestCase {

    // MARK: helpers

    // An AAC file of tone (amplitude > 0) and silence (0) pieces: [(seconds, amplitude)].
    static func audio(_ pieces: [(Double, Float)], channels: Int = 1, freq: Double = 300) throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("ather-meet-test-\(UUID().uuidString).m4a")
        let rate = 48000.0
        let f = try AVAudioFile(forWriting: url, settings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: rate, AVNumberOfChannelsKey: channels, AVEncoderBitRateKey: 96000],
                                commonFormat: .pcmFormatFloat32, interleaved: false)
        var n = 0
        for (secs, amp) in pieces {
            let frames = AVAudioFrameCount(secs * rate)
            let buf = AVAudioPCMBuffer(pcmFormat: f.processingFormat, frameCapacity: frames)!
            buf.frameLength = frames
            for ch in 0..<channels {
                let d = buf.floatChannelData![ch]
                for i in 0..<Int(frames) { d[i] = amp * Float(sin(2 * .pi * freq * Double(n + i) / rate)) }
            }
            n += Int(frames)
            try f.write(from: buf)
        }
        return url
    }

    static func duration(_ u: URL) async throws -> Double { CMTimeGetSeconds(try await AVURLAsset(url: u).load(.duration)) }

    static func words(_ text: String, from start: Double, step: Double = 0.4) -> [TimedWord] {
        text.split(separator: " ").enumerated().map { i, w in TimedWord(start: start + Double(i) * step, end: start + Double(i) * step + step * 0.8, text: String(w)) }
    }

    static func sampleMeeting(folder: URL? = nil) -> Meeting {
        var m = Meeting()
        m.title = "Sprint planning"
        m.duration = 20
        m.hasOthers = true
        m.hasYou = true
        m.transcript = .done
        m.lines = MeetingTranscript.merge(
            me: words("So um I think we we should ship on Friday.", from: 6),
            others: words("Morning everyone. Let's look at the release.", from: 0.5) + words("Friday works, uh, for the design team.", from: 12))
        m.markers = [MeetingMarker(time: 7, label: "Ship date")]
        m.folder = folder ?? FileManager.default.temporaryDirectory.appendingPathComponent("ather-meet-\(UUID().uuidString)")
        return m
    }

    // MARK: transcript

    func testLinesSplitOnPausesAndSentences() {
        var w = MeetingTests.words("Hello there. How are you", from: 0)  // a sentence end, then a short pause
        w[2].start += 0.6; w[3].start += 0.6; w[4].start += 0.6
        w[2].end += 0.6; w[3].end += 0.6; w[4].end += 0.6
        let lines = MeetingTranscript.lines(w + MeetingTests.words("Later words", from: 10), speaker: .others)
        XCTAssertEqual(lines.map(\.text), ["Hello there.", "How are you", "Later words"])
        XCTAssertEqual(lines[0].words.count, 2)
        XCTAssertEqual(lines[2].start, 10, accuracy: 0.001)
        XCTAssertTrue(lines.allSatisfy { $0.speaker == .others })
    }

    func testEchoOfOthersOnTheMicrophoneIsDropped() {
        let others = MeetingTests.words("we need the budget numbers by Monday", from: 2)
        let echo = MeetingTests.words("need the budget numbers by Monday", from: 2.3)  // the mic heard the speakers
        let mine = MeetingTests.words("I can send them tonight", from: 8)
        let lines = MeetingTranscript.merge(me: echo + mine, others: others)
        XCTAssertEqual(lines.count, 2)
        XCTAssertEqual(lines[0].speaker, .others)
        XCTAssertEqual(lines[1].speaker, .me)
        XCTAssertEqual(lines[1].text, "I can send them tonight")
        // Talking over each other with different words keeps both.
        let both = MeetingTranscript.merge(me: MeetingTests.words("wait one second please", from: 2.2), others: others)
        XCTAssertEqual(both.count, 2)
    }

    func testExports() {
        var m = MeetingTests.sampleMeeting()
        m.names = ["others": "Team"]
        m.summary = MeetingSummary(overview: "Ship Friday.", keyPoints: ["Release review"], decisions: ["Ship on Friday"], actionItems: ["You: tell design"])
        let md = MeetingExport.markdown(m)
        XCTAssertTrue(md.contains("# Sprint planning"))
        XCTAssertTrue(md.contains("## Action items\n\n- [ ] You: tell design"))
        XCTAssertTrue(md.contains("**0:00 Team:** Morning everyone."))
        XCTAssertTrue(md.contains("**0:06 You:** So um I think we we should ship on Friday."))
        XCTAssertTrue(md.contains("- **0:07** Ship date"))
        let srt = MeetingExport.srt(m)
        XCTAssertTrue(srt.hasPrefix("1\n00:00:00,500 --> "), srt)
        XCTAssertTrue(srt.contains("You: So um I think"))
        let txt = MeetingExport.text(m)
        XCTAssertTrue(txt.contains("[0:12] Team: Friday works, uh, for the design team."))
        XCTAssertEqual(MeetingFormat.clock(3725), "1:02:05")
        XCTAssertEqual(MeetingFormat.srtTime(83.45), "00:01:23,450")
    }

    func testStoreRoundTripWritesMarkdownAndAgentJSON() throws {
        let store = MeetingStore()
        var m = MeetingTests.sampleMeeting(folder: store.root.appendingPathComponent("test-\(UUID().uuidString)"))
        MeetingEdit.cut(&m, 6.4, 6.7, reason: .filler)
        XCTAssertTrue(store.save(m))
        let back = try XCTUnwrap(MeetingStore.read(m.folder))
        XCTAssertEqual(back.id, m.id)
        XCTAssertEqual(back.lines.count, m.lines.count)
        XCTAssertEqual(back.lines.first?.words.count, m.lines.first?.words.count)
        XCTAssertEqual(back.cuts.count, 1)
        XCTAssertTrue(FileManager.default.fileExists(atPath: m.markdownURL.path))
        let j = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: m.agentURL)) as? [String: Any])
        XCTAssertEqual(j["title"] as? String, "Sprint planning")
        let lines = try XCTUnwrap(j["lines"] as? [[String: Any]])
        let mine = try XCTUnwrap(lines.first { $0["speaker"] as? String == "You" })
        XCTAssertEqual(mine["edited_text"] as? String, "So I think we we should ship on Friday.")
        XCTAssertNotNil((mine["words"] as? [[String: Any]])?.first?["t"])
        // An older or newer meeting.json with missing keys still loads.
        try Data(#"{"title": "Old one", "duration": 5}"#.utf8).write(to: m.jsonURL)
        XCTAssertEqual(MeetingStore.read(m.folder)?.title, "Old one")
        try? FileManager.default.removeItem(at: m.folder)
    }

    func testSummaryParsing() {
        let s = MeetingAI.parse("""
        **SUMMARY:** The team agreed to ship.
        It went well.
        KEY POINTS:
        - Release is ready
        - QA passed
        DECISIONS:
        - None
        ACTION ITEMS:
        - [ ] Tín: write the notes
        """)
        XCTAssertEqual(s.overview, "The team agreed to ship. It went well.")
        XCTAssertEqual(s.keyPoints, ["Release is ready", "QA passed"])
        XCTAssertEqual(s.decisions, [])
        XCTAssertEqual(s.actionItems, ["Tín: write the notes"])
    }

    func testSpeechRunsBecomeWords() {
        var prior: [TimedWord] = []
        let w = MeetingSpeech.split(" Hello world", start: 1, end: 2, after: &prior)
        XCTAssertEqual(w.map(\.text), ["Hello", "world"])
        XCTAssertEqual(w[0].start, 1, accuracy: 0.001)
        XCTAssertEqual(w[1].end, 2, accuracy: 0.001)
        prior = w
        XCTAssertTrue(MeetingSpeech.split(".", start: nil, end: nil, after: &prior).isEmpty)
        XCTAssertEqual(prior.last?.text, "world.")
    }

    func testCallDetectionNames() {
        XCTAssertEqual(MeetingDetector.callApp(["com.apple.something", "us.zoom.xos"]), "Zoom")
        XCTAssertEqual(MeetingDetector.callApp(["com.google.Chrome.helper"]), "Google Chrome")
        XCTAssertEqual(MeetingDetector.callApp(["com.apple.WebKit.GPU"]), "Safari")
        XCTAssertNil(MeetingDetector.callApp(["com.apple.dictation"]))
    }

    // MARK: mixing (A1)

    func testMixTwoTracksIntoOneFile() async throws {
        let a = try MeetingTests.audio([(3, 0.3)], channels: 2), b = try MeetingTests.audio([(1, 0), (2, 0.3)])
        let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-mix-\(UUID().uuidString).m4a")
        try await MeetingRecorder.mix([a, b], to: out)
        let asset = AVURLAsset(url: out)
        let tracks = try await asset.loadTracks(withMediaType: .audio).count
        XCTAssertEqual(tracks, 1, "one mixed track to share")
        let d = try await MeetingTests.duration(out)
        XCTAssertEqual(d, 3, accuracy: 0.1)
        let peaks = Waveform.compute(out, buckets: 30)
        XCTAssertEqual(peaks.count, 30)
        XCTAssertGreaterThan(peaks.max() ?? 0, 0.5)
    }
}

// MARK: - Editing (A9, A10)

final class MeetingEditTests: XCTestCase {
    func testCutArithmetic() {
        var m = Meeting()
        m.duration = 10
        MeetingEdit.cut(&m, 2, 3)
        MeetingEdit.cut(&m, 2.5, 4)      // overlaps: merged
        MeetingEdit.cut(&m, 8, 20)       // clamped to the end
        let spans = MeetingEdit.merged(m.cuts)
        XCTAssertEqual(spans.count, 2)
        XCTAssertEqual(spans[0].start, 2); XCTAssertEqual(spans[0].end, 4)
        XCTAssertEqual(spans[1].end, 10)
        XCTAssertEqual(MeetingEdit.editedDuration(m), 6, accuracy: 0.001)
        XCTAssertEqual(MeetingEdit.editedTime(1, spans), 1, accuracy: 0.001)
        XCTAssertEqual(MeetingEdit.editedTime(3, spans), 2, accuracy: 0.001)   // inside a cut: where the cut was
        XCTAssertEqual(MeetingEdit.editedTime(5, spans), 3, accuracy: 0.001)
        XCTAssertEqual(MeetingEdit.kept(duration: 10, spans).map { [$0.start, $0.end] }, [[0, 2], [4, 8]])
        XCTAssertEqual(MeetingEdit.cutEnd(at: 2.5, spans), 4)
        XCTAssertNil(MeetingEdit.cutEnd(at: 5, spans))
        // Restoring the middle of a cut splits it.
        MeetingEdit.restore(&m, 2.5, 3)
        XCTAssertEqual(MeetingEdit.merged(m.cuts).map { [$0.start, $0.end] }, [[2, 2.5], [3, 4], [8, 10]])
        XCTAssertEqual(MeetingEdit.editedDuration(m), 6.5, accuracy: 0.001)
        XCTAssertFalse(MeetingEdit.cut(&m, 5, 5.001), "an empty cut is ignored")
    }

    func testEditedTranscriptDropsCutWordsAndRetimes() {
        var m = MeetingTests.sampleMeeting()
        let mine = m.lines.first { $0.speaker == .me }!
        let um = mine.words.first { $0.text == "um" }!
        MeetingEdit.cut(&m, um.start, um.end)
        let first = m.lines[0]
        MeetingEdit.cut(&m, first.start, first.end)   // the whole first line
        let edited = MeetingEdit.editedLines(m)
        XCTAssertEqual(edited.count, m.lines.count - 1)
        XCTAssertFalse(edited.contains { $0.text.contains("Morning") })
        let e = edited.first { $0.speaker == .me }!
        XCTAssertEqual(e.text, "So I think we we should ship on Friday.")
        XCTAssertLessThan(e.start, mine.start, "times move up by what was cut before")
        XCTAssertEqual(e.start, mine.start - (first.end - first.start), accuracy: 0.001)
        XCTAssertTrue(MeetingExport.markdown(m, edited: true).contains("So I think we we should"))
    }

    func testFillersAndRepeats() {
        var m = MeetingTests.sampleMeeting()
        let r = MeetingEdit.cleanUp(&m, fillers: true, repeats: true, pauses: false)
        XCTAssertEqual(r.fillers, 2, "um and uh,")
        XCTAssertEqual(r.repeats, 1, "we we")
        XCTAssertGreaterThan(r.saved, 0.5)
        let mine = MeetingEdit.editedLines(m).first { $0.speaker == .me }!
        XCTAssertEqual(mine.text, "So I think we should ship on Friday.")
        let theirs = MeetingEdit.editedLines(m).last { $0.speaker == .others }!
        XCTAssertEqual(theirs.text, "Friday works, for the design team.")
        // Running it again replaces, not stacks.
        let again = MeetingEdit.cleanUp(&m, fillers: true, repeats: true, pauses: false)
        XCTAssertEqual(again.saved, 0, accuracy: 0.001)
        XCTAssertEqual(m.cuts.count, 3)
    }

    func testPauseDetectionOnLevels() {
        // 1 s loud, 2.5 s quiet, 1 s loud, 0.6 s quiet, 1 s loud at 20 ms windows.
        var rms: [Float] = []
        for (secs, v) in [(1.0, Float(0.2)), (2.5, 0.001), (1.0, 0.2), (0.6, 0.001), (1.0, 0.2)] { rms += Array(repeating: v, count: Int(secs / 0.02)) }
        let cuts = MeetingEdit.pauseCuts(rms: rms, window: 0.02, minPause: 1.0, keep: 0.4)
        XCTAssertEqual(cuts.count, 1, "only the pause over 1 s")
        XCTAssertEqual(cuts[0].start, 1.2, accuracy: 0.03)
        XCTAssertEqual(cuts[0].end, 3.3, accuracy: 0.03)
    }

    func testExportEditedAudioIsShorter() async throws {
        // 2 s tone, 3 s silence, 2 s tone: shortening pauses and cutting 0.5 s of the first tone.
        let src = try MeetingTests.audio([(2, 0.4), (3, 0), (2, 0.4)])
        var m = Meeting()
        m.duration = 7
        m.folder = src.deletingLastPathComponent()
        let levels = MeetingEdit.rms(src)
        XCTAssertGreaterThan(levels.count, 300)
        let r = MeetingEdit.cleanUp(&m, fillers: false, repeats: false, pauses: true, rms: levels)
        XCTAssertEqual(r.pauses, 1)
        XCTAssertEqual(r.saved, 2.6, accuracy: 0.1)
        MeetingEdit.cut(&m, 0.5, 1.0)
        let expected = MeetingEdit.editedDuration(m)
        XCTAssertEqual(expected, 3.9, accuracy: 0.1)
        for enhance in [false, true] {
            let out = FileManager.default.temporaryDirectory.appendingPathComponent("ather-edited-\(UUID().uuidString).m4a")
            try await MeetingEdit.exportAudio(source: src, duration: m.duration, cuts: m.cuts, enhance: enhance, to: out)
            let d = try await MeetingTests.duration(out)
            XCTAssertEqual(d, expected, accuracy: 0.12, "enhance \(enhance)")
            let after = MeetingEdit.rms(out)
            XCTAssertLessThan(MeetingEdit.pauseCuts(rms: after, window: 0.02).count, 1, "no long pause is left")
        }
    }

    func testSliversBetweenCutsAreMerged() {
        var m = Meeting()
        m.duration = 10
        MeetingEdit.cut(&m, 1, 2)
        MeetingEdit.cut(&m, 2.01, 3)   // a 10 ms sliver would overlap the fades at both joins
        XCTAssertEqual(MeetingEdit.merged(m.cuts).count, 1)
        XCTAssertEqual(MeetingEdit.kept(duration: 10, MeetingEdit.merged(m.cuts)).count, 2)
    }

    @MainActor
    func testNotesGoToTheMeetingTheyWereTypedIn() throws {
        let store = MeetingStore()
        store.loadIfNeeded()
        var a = MeetingTests.sampleMeeting(folder: store.root.appendingPathComponent("notes-a-\(UUID().uuidString)")); a.title = "A"
        var b = MeetingTests.sampleMeeting(folder: store.root.appendingPathComponent("notes-b-\(UUID().uuidString)")); b.title = "B"
        b.notes = "B's own notes"
        store.save(a); store.save(b)
        defer { try? FileManager.default.removeItem(at: a.folder); try? FileManager.default.removeItem(at: b.folder) }
        let model = MeetingsModel(store: store)
        model.selectedID = a.id
        model.setNotes("typed in A")
        model.selectedID = b.id        // switched before the save timer fired
        RunLoop.main.run(until: Date().addingTimeInterval(0.8))
        XCTAssertEqual(store.meeting(a.id)?.notes, "typed in A")
        XCTAssertEqual(store.meeting(b.id)?.notes, "B's own notes")
        XCTAssertEqual(MeetingStore.read(a.folder)?.notes, "typed in A")
    }

    @MainActor
    func testUnmixedMeetingPlaysItsTrackAndIsMixedAtLaunch() async throws {
        let store = MeetingStore.shared
        store.loadIfNeeded()
        var m = MeetingTests.sampleMeeting(folder: store.root.appendingPathComponent("unmixed-\(UUID().uuidString)"))
        m.hasYou = false
        m.transcript = .done
        try FileManager.default.createDirectory(at: m.folder.appendingPathComponent("tracks"), withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: try MeetingTests.audio([(2, 0.3)], channels: 2), to: m.trackURL(.others))
        store.save(m)
        defer { store.delete(m.id) }
        XCTAssertEqual(m.playableURL, m.trackURL(.others), "before the mix, the track plays")
        MeetingPipeline.shared.resumePending()
        for _ in 0..<40 where !FileManager.default.fileExists(atPath: m.audioURL.path) { try await Task.sleep(nanoseconds: 100_000_000) }
        XCTAssertTrue(FileManager.default.fileExists(atPath: m.audioURL.path))
        XCTAssertEqual(m.playableURL, m.audioURL)
        XCTAssertFalse(FileManager.default.fileExists(atPath: m.folder.appendingPathComponent(".mixing.m4a").path))
    }

    @MainActor
    func testAgentToolsEditAndExport() async throws {
        let store = MeetingStore.shared
        let src = try MeetingTests.audio([(2, 0.4), (3, 0), (2, 0.4)])
        var m = MeetingTests.sampleMeeting(folder: store.root.appendingPathComponent("agent-\(UUID().uuidString)"))
        m.title = "Agent edit test \(UUID().uuidString.prefix(6))"
        m.duration = 7
        m.hasYou = false
        try FileManager.default.createDirectory(at: m.folder, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: src, to: m.audioURL)
        store.save(m)
        defer { store.delete(m.id) }
        let list = MeetingTools.list(["query": String(m.title.suffix(6))])
        XCTAssertEqual(list["total"] as? Int, 1)
        let got = try await MeetingTools.get(["id": m.id.uuidString, "wait_seconds": 0])
        XCTAssertEqual((got["lines"] as? [[String: Any]])?.count, m.lines.count)
        XCTAssertNil((got["lines"] as? [[String: Any]])?.first?["words"], "words only when asked")
        let lineID = m.lines[0].id.uuidString
        let e = try await MeetingTools.edit(["id": m.id.uuidString, "remove_fillers": true, "remove_repeats": true, "shorten_pauses": true, "cut_lines": [lineID]])
        XCTAssertEqual(e["fillers_cut"] as? Int, 2)
        XCTAssertEqual(e["repeats_cut"] as? Int, 1)
        XCTAssertEqual(e["pauses_shortened"] as? Int, 1)
        XCTAssertGreaterThan(e["shorter_by"] as? Double ?? 0, 3)
        let x = try await MeetingTools.export(["id": m.id.uuidString, "enhance": false, "formats": ["markdown", "srt", "json"]])
        let files = try XCTUnwrap(x["files"] as? [String: Any])
        let audio = try XCTUnwrap(files["audio"] as? String)
        let d = try await MeetingTests.duration(URL(fileURLWithPath: audio))
        XCTAssertEqual(d, x["edited_duration"] as? Double ?? 0, accuracy: 0.15)
        let md = try String(contentsOfFile: try XCTUnwrap(files["markdown"] as? String), encoding: .utf8)
        XCTAssertFalse(md.contains("Morning everyone"), "the cut line is gone")
        XCTAssertTrue(md.contains("So I think we should ship on Friday."))
        XCTAssertTrue(FileManager.default.fileExists(atPath: try XCTUnwrap(files["json"] as? String)))
        XCTAssertNoThrow(try MeetingTools.find(["id": "latest"]))
        XCTAssertThrowsError(try MeetingTools.find(["id": "no such meeting zzz"]))
        XCTAssertTrue(Agent.tools().contains { $0.name == "edit_meeting" })
    }
}

// MARK: - Live: real speech through the real transcriber (A3). Skipped where macOS can't transcribe on the device.

final class MeetingSpeechLiveTests: XCTestCase {
    func testTranscribesSynthesizedSpeech() async throws {
        try XCTSkipIf(ProcessInfo.processInfo.environment["ATHER_SKIP_LIVE"] != nil)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-say-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let aiff = dir.appendingPathComponent("speech.aiff")
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/say")
        p.arguments = ["-v", "Samantha", "-o", aiff.path, "Good morning everyone. Um, let's review the budget for the summer release. The the deadline is Friday."]
        try p.run()
        p.waitUntilExit()
        try XCTSkipIf(p.terminationStatus != 0, "say isn't available")
        let m4a = dir.appendingPathComponent("speech.m4a")
        try await MeetingRecorder.mix([aiff], to: m4a)
        let r: MeetingSpeech.Result
        do { r = try await MeetingSpeech.transcribe(m4a, language: "en-US") } catch {
            throw XCTSkip("No on-device transcription here: \(error.localizedDescription)")
        }
        let text = r.words.map(\.text).joined(separator: " ").lowercased()
        print("Transcribed with \(r.engine) (\(r.locale)): \(text)")
        for w in ["morning", "budget", "summer", "release", "friday"] { XCTAssertTrue(text.contains(w), "\(w) in “\(text)”") }
        XCTAssertTrue(zip(r.words, r.words.dropFirst()).allSatisfy { $0.start <= $1.start + 0.001 }, "words in time order")
        XCTAssertGreaterThan(r.words.last?.end ?? 0, 3)
        let lines = MeetingTranscript.lines(r.words, speaker: .others)
        XCTAssertFalse(lines.isEmpty)
    }
}
