import AVFoundation
import AppKit
import Combine

// Meeting recordings: one folder per meeting in <captures>/Meetings/, holding
//   meeting.m4a          both sources mixed (what you share)
//   tracks/others.m4a    system audio: everyone else in the call
//   tracks/you.m4a       the microphone: you
//   meeting.json         title, times, sources, markers, transcript, summary, notes (this file is the truth)
//   transcript.md        the same as readable notes, rewritten whenever meeting.json is
// A meeting still being recorded lives in a hidden ".recording-<id>" folder, so the store never lists it.

enum Speaker: String, Codable, CaseIterable {
    case me, others
    var label: String { self == .me ? "You" : "Others" }
}

struct MeetingMarker: Codable, Hashable, Identifiable {
    var id = UUID()
    var time: Double
    var label: String
}

struct MeetingLine: Codable, Hashable, Identifiable {
    var id = UUID()
    var start: Double
    var end: Double
    var speaker: Speaker
    var text: String
    var words: [TimedWord] = []   // each word's time, for editing the audio by its text

    init(id: UUID = UUID(), start: Double, end: Double, speaker: Speaker, text: String, words: [TimedWord] = []) {
        self.id = id; self.start = start; self.end = end; self.speaker = speaker; self.text = text; self.words = words
    }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = c.value(.id, UUID()); start = c.value(.start, 0); end = c.value(.end, 0); speaker = c.value(.speaker, .others)
        text = c.value(.text, ""); words = c.value(.words, [])
    }
}

// A stretch of the recording left out of the edit. The files are never changed; playback and exports skip it.
struct MeetingCut: Codable, Hashable, Identifiable {
    enum Reason: String, Codable { case manual, filler, repeated, pause, trim }
    var id = UUID()
    var start: Double
    var end: Double
    var reason = Reason.manual
    var length: Double { max(0, end - start) }
}

// Something made from the transcript: a follow-up email, show notes, key quotes.
struct MeetingOutput: Codable, Hashable, Identifiable {
    var id = UUID()
    var kind: String
    var text: String
    var created = Date()
}

struct MeetingSummary: Codable, Hashable {
    var overview = ""
    var keyPoints: [String] = []
    var decisions: [String] = []
    var actionItems: [String] = []
    var created = Date()
    var isEmpty: Bool { overview.isEmpty && keyPoints.isEmpty && decisions.isEmpty && actionItems.isEmpty }
}

enum TranscriptState: String, Codable {
    case none, pending, running, done, failed
}

struct Meeting: Codable, Identifiable, Hashable {
    var format = 1
    var id = UUID()
    var title = "Meeting"
    var created = Date()          // when recording started
    var duration: Double = 0      // recording time, pauses left out
    var app = ""                  // the app whose sound was recorded; "" = all apps
    var hasOthers = false         // tracks/others.m4a exists
    var hasYou = false            // tracks/you.m4a exists
    var micName = ""
    var markers: [MeetingMarker] = []
    var lines: [MeetingLine] = []
    var notes = ""
    var summary: MeetingSummary?
    var transcript = TranscriptState.none
    var transcriptError: String?
    var transcriptEngine: String?
    var language: String?
    var summaryError: String?
    var cuts: [MeetingCut] = []
    var names: [String: String] = [:]   // speaker → name shown ("others" → "Guest")
    var outputs: [MeetingOutput] = []
    var imported = false                // opened from an audio file rather than recorded

    var folder = URL(fileURLWithPath: "/")  // not saved: where meeting.json was found

    enum CodingKeys: String, CodingKey {
        case format, id, title, created, duration, app, hasOthers, hasYou, micName, markers, lines, notes, summary, transcript,
             transcriptError, transcriptEngine, language, summaryError, cuts, names, outputs, imported
    }

    init() {}

    // Missing or unreadable keys fall back to their defaults, so a meeting.json from another version still opens.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        format = c.value(.format, 1); id = c.value(.id, UUID()); title = c.value(.title, "Meeting"); created = c.value(.created, Date())
        duration = c.value(.duration, 0); app = c.value(.app, ""); hasOthers = c.value(.hasOthers, false); hasYou = c.value(.hasYou, false)
        micName = c.value(.micName, ""); markers = c.value(.markers, []); lines = c.value(.lines, []); notes = c.value(.notes, "")
        summary = c.value(.summary, nil); transcript = c.value(.transcript, .none); transcriptError = c.value(.transcriptError, nil)
        transcriptEngine = c.value(.transcriptEngine, nil); language = c.value(.language, nil); summaryError = c.value(.summaryError, nil)
        cuts = c.value(.cuts, []); names = c.value(.names, [:]); outputs = c.value(.outputs, []); imported = c.value(.imported, false)
    }

    func name(_ s: Speaker) -> String {
        if let n = names[s.rawValue], !n.trimmingCharacters(in: .whitespaces).isEmpty { return n }
        return imported && s == .others ? "Speaker" : s.label
    }

    var audioURL: URL { folder.appendingPathComponent("meeting.m4a") }
    var jsonURL: URL { folder.appendingPathComponent("meeting.json") }
    var markdownURL: URL { folder.appendingPathComponent("transcript.md") }
    var agentURL: URL { folder.appendingPathComponent("transcript.json") }
    // meeting.m4a, or (before it's mixed) the first track there is.
    var playableURL: URL {
        if FileManager.default.fileExists(atPath: audioURL.path) { return audioURL }
        return tracks.map(trackURL).first { FileManager.default.fileExists(atPath: $0.path) } ?? audioURL
    }
    var editedAudioURL: URL { folder.appendingPathComponent("edited.m4a") }
    func trackURL(_ s: Speaker) -> URL { folder.appendingPathComponent("tracks/\(s == .me ? "you" : "others").m4a") }
    var tracks: [Speaker] { (hasOthers ? [Speaker.others] : []) + (hasYou ? [.me] : []) }

    var sourcesLabel: String {
        switch (hasOthers, hasYou) {
        case (true, true): return (app.isEmpty ? "Others" : app) + " + You"
        case (true, false): return app.isEmpty ? "Others (system audio)" : app
        case (false, true): return "You (microphone)"
        default: return "No sound"
        }
    }

    // Plain text for search: title, notes, markers, transcript, summary.
    var searchText: String {
        var s = [title, notes, app] + markers.map(\.label) + lines.map(\.text)
        if let m = summary { s += [m.overview] + m.keyPoints + m.decisions + m.actionItems }
        return s.joined(separator: "\n").lowercased()
    }
}

// MARK: - Formatting

enum MeetingFormat {
    // 1:02:03 or 2:03
    static func clock(_ t: Double) -> String {
        let s = max(0, Int(t.rounded(.down)))
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) : String(format: "%d:%02d", s / 60, s % 60)
    }
    // 00:01:23,450 (SRT)
    static func srtTime(_ t: Double) -> String {
        let ms = max(0, Int((t * 1000).rounded()))
        return String(format: "%02d:%02d:%02d,%03d", ms / 3_600_000, ms / 60_000 % 60, ms / 1000 % 60, ms % 1000)
    }
    // 1 h 2 min, 12 min, 45 s
    static func length(_ t: Double) -> String {
        let s = max(0, Int(t.rounded()))
        if s < 60 { return "\(s) s" }
        if s < 3600 { return "\(s / 60) min" }
        return "\(s / 3600) h \(s / 60 % 60) min"
    }
    static let dateTime: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }()
    static let folderStamp: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH-mm"
        return f
    }()
    static let defaultTitle: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "EEE d MMM, HH:mm"
        return f
    }()
}

// MARK: - Transcript: words → lines per speaker, echo removed, both speakers merged

struct TimedWord: Codable, Hashable {
    var start: Double
    var end: Double
    var text: String
}

enum MeetingTranscript {
    // A speaker's words become lines: a new line after a pause of 1.2 s, after a sentence end and a 0.5 s pause,
    // or once a line runs 30 s.
    static func lines(_ words: [TimedWord], speaker: Speaker) -> [MeetingLine] {
        var out: [MeetingLine] = []
        var cur: MeetingLine?
        for w in words.sorted(by: { $0.start < $1.start }) {
            let t = w.text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !t.isEmpty else { continue }
            if var c = cur {
                let gap = w.start - c.end
                let sentenceEnd = c.text.last.map { ".?!…".contains($0) } ?? false
                if gap < 1.2, !(sentenceEnd && gap >= 0.5), w.end - c.start < 30 {
                    c.text += (startsWithPunctuation(t) ? "" : " ") + t
                    c.end = max(c.end, w.end)
                    c.words.append(TimedWord(start: w.start, end: max(w.end, w.start), text: t))
                    cur = c
                    continue
                }
                out.append(c)
            }
            cur = MeetingLine(start: w.start, end: max(w.end, w.start), speaker: speaker, text: t,
                              words: [TimedWord(start: w.start, end: max(w.end, w.start), text: t)])
        }
        if let c = cur { out.append(c) }
        return out
    }

    private static func startsWithPunctuation(_ s: String) -> Bool { s.first.map { ",.;:!?…)'’".contains($0) } ?? false }

    static func normalizedWords(_ s: String) -> [String] {
        s.lowercased().split(whereSeparator: { !$0.isLetter && !$0.isNumber }).map(String.init)
    }

    // How much of `a`'s words are also in `b` (0…1).
    static func overlap(_ a: String, _ b: String) -> Double {
        let wa = normalizedWords(a), wb = Set(normalizedWords(b))
        guard !wa.isEmpty, !wb.isEmpty else { return 0 }
        return Double(wa.filter(wb.contains).count) / Double(wa.count)
    }

    // On speakers, the microphone also hears the others: a "You" line that mostly repeats what Others said at the same
    // time is that echo, not you, and is dropped.
    static func removeEcho(me: [MeetingLine], others: [MeetingLine]) -> [MeetingLine] {
        me.filter { line in
            let near = others.filter { $0.start < line.end + 1.5 && $0.end > line.start - 1.5 }
            guard !near.isEmpty else { return true }
            return overlap(line.text, near.map(\.text).joined(separator: " ")) < 0.6
        }
    }

    static func merge(me: [TimedWord], others: [TimedWord]) -> [MeetingLine] {
        let o = lines(others, speaker: .others)
        let m = removeEcho(me: lines(me, speaker: .me), others: o)
        return (o + m).sorted { $0.start == $1.start ? $0.speaker == .others : $0.start < $1.start }
    }
}

// MARK: - Exports

enum MeetingExport: String, CaseIterable {
    case markdown, text, srt, json
    var ext: String { ["markdown": "md", "text": "txt", "srt": "srt", "json": "json"][rawValue]! }
    var title: String {
        ["markdown": "Markdown notes (.md)", "text": "Plain text (.txt)", "srt": "Subtitles (.srt)", "json": "For AI agents (.json)"][rawValue]!
    }

    // `edited`: without the cut words, on the edited timeline.
    static func markdown(_ m: Meeting, edited: Bool = false) -> String {
        let lines = edited ? MeetingEdit.editedLines(m) : m.lines
        var s = "# \(m.title)\n\n"
        s += "- **When:** \(MeetingFormat.dateTime.string(from: m.created))\n"
        s += "- **Length:** \(MeetingFormat.clock(m.duration))"
        let saved = MeetingEdit.savedSeconds(m)
        if saved > 0.5 { s += " (edited: \(MeetingFormat.clock(MeetingEdit.editedDuration(m))))" }
        s += "\n- **Recorded:** \(m.sourcesLabel)\n"
        if let e = m.transcriptEngine { s += "- **Transcribed by:** \(e)\(m.language.map { " (\($0))" } ?? "")\n" }
        if let sum = m.summary, !sum.isEmpty {
            if !sum.overview.isEmpty { s += "\n## Summary\n\n\(sum.overview)\n" }
            func list(_ title: String, _ items: [String], check: Bool = false) {
                guard !items.isEmpty else { return }
                s += "\n## \(title)\n\n" + items.map { (check ? "- [ ] " : "- ") + $0 }.joined(separator: "\n") + "\n"
            }
            list("Key points", sum.keyPoints)
            list("Decisions", sum.decisions)
            list("Action items", sum.actionItems, check: true)
        }
        if !m.markers.isEmpty {
            let spans = MeetingEdit.merged(m.cuts)
            s += "\n## Marked moments\n\n" + m.markers.sorted { $0.time < $1.time }.map {
                "- **\(MeetingFormat.clock(edited ? MeetingEdit.editedTime($0.time, spans) : $0.time))** \($0.label)"
            }.joined(separator: "\n") + "\n"
        }
        let notes = m.notes.trimmingCharacters(in: .whitespacesAndNewlines)
        if !notes.isEmpty { s += "\n## Notes\n\n\(notes)\n" }
        for o in m.outputs { s += "\n## \(o.kind)\n\n\(o.text.trimmingCharacters(in: .whitespacesAndNewlines))\n" }
        s += "\n## Transcript\n\n"
        switch m.transcript {
        case .done where lines.isEmpty: s += "_No speech was recognized._\n"
        case .done: s += lines.map { "**\(MeetingFormat.clock($0.start)) \(m.name($0.speaker)):** \($0.text)" }.joined(separator: "\n\n") + "\n"
        case .failed: s += "_Not transcribed: \(m.transcriptError ?? "unknown error")_\n"
        case .pending, .running: s += "_Being transcribed…_\n"
        case .none: s += "_Not transcribed._\n"
        }
        return s
    }

    static func text(_ m: Meeting, edited: Bool = false) -> String {
        let lines = edited ? MeetingEdit.editedLines(m) : m.lines
        return "\(m.title)\n\(MeetingFormat.dateTime.string(from: m.created)) · \(MeetingFormat.clock(edited ? MeetingEdit.editedDuration(m) : m.duration))\n\n"
            + lines.map { "[\(MeetingFormat.clock($0.start))] \(m.name($0.speaker)): \($0.text)" }.joined(separator: "\n") + "\n"
    }

    static func srt(_ m: Meeting, edited: Bool = false) -> String {
        let lines = edited ? MeetingEdit.editedLines(m) : m.lines
        return lines.enumerated().map { i, l in
            "\(i + 1)\n\(MeetingFormat.srtTime(l.start)) --> \(MeetingFormat.srtTime(max(l.end, l.start + 0.5)))\n\(m.name(l.speaker)): \(l.text)\n"
        }.joined(separator: "\n")
    }

    func render(_ m: Meeting, edited: Bool = false) -> String {
        switch self {
        case .markdown: return MeetingExport.markdown(m, edited: edited)
        case .text: return MeetingExport.text(m, edited: edited)
        case .srt: return MeetingExport.srt(m, edited: edited)
        case .json: return MeetingAgentExport.data(m).flatMap { String(data: $0, encoding: .utf8) } ?? "{}"
        }
    }
}

// MARK: - Store

final class MeetingStore: ObservableObject {
    static let shared = MeetingStore()
    static let changed = Notification.Name("ather.meetings.changed")
    @Published private(set) var meetings: [Meeting] = []   // newest first
    @Published var busy: Set<UUID> = []                    // being transcribed or summarized
    private var loaded = false

    var root: URL { Settings.shared.capturesFolder.appendingPathComponent("Meetings", isDirectory: true) }

    func loadIfNeeded() { if !loaded { reload() } }

    func reload() {
        loaded = true
        let fm = FileManager.default
        var out: [Meeting] = []
        for dir in (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil, options: [.skipsHiddenFiles])) ?? [] {
            if let m = MeetingStore.read(dir) { out.append(m) }
        }
        meetings = out.sorted { $0.created > $1.created }
    }

    static func read(_ dir: URL) -> Meeting? {
        let u = dir.appendingPathComponent("meeting.json")
        guard let d = try? Data(contentsOf: u), var m = try? decoder.decode(Meeting.self, from: d) else { return nil }
        m.folder = dir
        return m
    }

    static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        e.dateEncodingStrategy = .iso8601
        return e
    }()
    static let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .iso8601
        return d
    }()

    // Writes meeting.json and transcript.md, and updates the list.
    @discardableResult
    func save(_ m: Meeting) -> Bool {
        guard MeetingStore.write(m) else { return false }
        if let i = meetings.firstIndex(where: { $0.id == m.id }) { meetings[i] = m } else {
            meetings.append(m)
            meetings.sort { $0.created > $1.created }
        }
        NotificationCenter.default.post(name: MeetingStore.changed, object: self, userInfo: ["id": m.id])
        return true
    }

    static func write(_ m: Meeting) -> Bool {
        guard let d = try? encoder.encode(m) else { return false }
        do {
            try FileManager.default.createDirectory(at: m.folder, withIntermediateDirectories: true)
            try d.write(to: m.jsonURL, options: .atomic)
            try Data(MeetingExport.markdown(m).utf8).write(to: m.markdownURL, options: .atomic)
            if let j = MeetingAgentExport.data(m) { try j.write(to: m.agentURL, options: .atomic) }
            return true
        } catch { return false }
    }

    func meeting(_ id: UUID) -> Meeting? { meetings.first { $0.id == id } }

    // Tells views a meeting's files changed (e.g. meeting.m4a was mixed after the fact).
    func reloadOne(_ id: UUID) {
        NotificationCenter.default.post(name: MeetingStore.changed, object: self, userInfo: ["id": id])
    }

    func update(_ id: UUID, _ change: (inout Meeting) -> Void) {
        guard var m = meeting(id) ?? meetings.first(where: { $0.id == id }) else { return }
        change(&m)
        save(m)
    }

    // A free folder name for a meeting: "<yyyy-MM-dd HH-mm> <title>".
    func folderName(created: Date, title: String) -> String {
        let t = Output.sanitize(title, maxLen: 60)
        return MeetingFormat.folderStamp.string(from: created) + (t.isEmpty ? "" : " " + t)
    }

    func uniqueFolder(_ name: String) -> URL {
        var u = root.appendingPathComponent(name, isDirectory: true)
        var i = 2
        while FileManager.default.fileExists(atPath: u.path), i < 1000 {
            u = root.appendingPathComponent("\(name) (\(i))", isDirectory: true)
            i += 1
        }
        return u
    }

    // Renaming also renames the folder (unless that fails, e.g. a file in it is open elsewhere).
    func rename(_ id: UUID, to title: String) {
        let t = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty, var m = meeting(id), m.title != t else { return }
        m.title = t
        let dst = uniqueFolder(folderName(created: m.created, title: t))
        if (try? FileManager.default.moveItem(at: m.folder, to: dst)) != nil { m.folder = dst }
        save(m)
    }

    // To the Trash, so it can be put back.
    func delete(_ id: UUID) {
        guard let m = meeting(id) else { return }
        try? FileManager.default.trashItem(at: m.folder, resultingItemURL: nil)
        meetings.removeAll { $0.id == id }
        NotificationCenter.default.post(name: MeetingStore.changed, object: self, userInfo: ["id": id])
    }

    func search(_ query: String) -> [Meeting] {
        let words = query.lowercased().split(separator: " ").map(String.init)
        guard !words.isEmpty else { return meetings }
        return meetings.filter { m in let hay = m.searchText; return words.allSatisfy { hay.contains($0) } }
    }

    func export(_ m: Meeting, as kind: MeetingExport, edited: Bool = false, to url: URL) throws {
        try Data(kind.render(m, edited: edited).utf8).write(to: url, options: .atomic)
    }
}

// MARK: - Waveform

enum Waveform {
    // The loudest sample in each of `buckets` slices of the file, 0…1. Cached next to the file (".peaks").
    static func peaks(_ url: URL, buckets: Int = 1200) -> [Float] {
        let cache = url.deletingLastPathComponent().appendingPathComponent(".peaks-\(buckets)")
        let mtime = (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        if let cm = (try? cache.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate), cm >= mtime,
           let d = try? Data(contentsOf: cache), d.count == buckets * 4 {
            return d.withUnsafeBytes { Array($0.bindMemory(to: Float.self)) }
        }
        let out = compute(url, buckets: buckets)
        if !out.isEmpty { try? out.withUnsafeBufferPointer { Data(buffer: $0) }.write(to: cache, options: .atomic) }
        return out
    }

    static func compute(_ url: URL, buckets: Int) -> [Float] {
        let asset = AVURLAsset(url: url)
        guard let track = asset.tracks(withMediaType: .audio).first, let reader = try? AVAssetReader(asset: asset) else { return [] }
        let rate = 8000.0
        let out = AVAssetReaderTrackOutput(track: track, outputSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: rate, AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 32, AVLinearPCMIsFloatKey: true, AVLinearPCMIsNonInterleaved: false, AVLinearPCMIsBigEndianKey: false,
        ])
        guard reader.canAdd(out) else { return [] }
        reader.add(out)
        guard reader.startReading() else { return [] }
        let total = max(1, CMTimeGetSeconds(asset.duration) * rate)
        var peaks = [Float](repeating: 0, count: buckets)
        var index = 0
        while let sb = out.copyNextSampleBuffer() {
            guard let block = CMSampleBufferGetDataBuffer(sb) else { continue }
            var len = 0
            var ptr: UnsafeMutablePointer<CChar>?
            guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr) == noErr, let ptr else { continue }
            ptr.withMemoryRebound(to: Float.self, capacity: len / 4) { f in
                for i in 0..<(len / 4) {
                    let b = min(buckets - 1, Int(Double(index + i) / total * Double(buckets)))
                    let v = abs(f[i])
                    if v > peaks[b] { peaks[b] = v }
                }
            }
            index += len / 4
        }
        // Loudness reads better on a square-root scale: quiet speech still shows.
        let top = max(0.05, peaks.max() ?? 1)
        return peaks.map { min(1, sqrt($0 / top)) }
    }
}
