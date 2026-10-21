import AVFoundation
import AppKit
import Speech

// Bug reports for playtests (README "Bug reports"; Windows: src/bugreport.cpp). "Bug start" … "bug stop", said
// (Settings › Bug reports, opt-in, on-device recognition) or with the BugReport shortcut, make one folder,
// <captures>/Bugs/<yyyy-MM-dd HH-mm-ss> <first words>/, holding:
//   bug.png         the screenshot from "bug start" with what was said burnt in as a bar along the bottom
//   screenshot.png  the same screenshot as it was
//   voice.wav       what the microphone heard in between (with voice commands on)
//   logs/…          the game's logs (LogPaths)
//   bug.md, bug.json
//   clip.mp4        with a recording running: from 10 s before "bug start" to "bug stop" (H.264), the words as captions.
//                   Cut once the recording stops; the bug is also an Issue note on the recording.

// MARK: - The bug logic: timed events in, what happened out (no microphone, screen or clock of its own)

enum BugCommand: Equatable { case start, stop, cancel }

enum BugWords {
    // A word as the commands compare it: lower case, letters and digits only.
    static func bare(_ w: String) -> String { String(w.lowercased().unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) }) }

    // The first "bug start", "bug stop" or "bug cancel" in words[from…] (case and punctuation ignored), and the index of its "bug".
    static func findCommand(_ words: [CaptionWord], from: Int = 0) -> (command: BugCommand, at: Int)? {
        var i = from
        while i + 1 < words.count {
            if bare(words[i].text) == "bug" {
                switch bare(words[i + 1].text) {
                case "start": return (.start, i)
                case "stop": return (.stop, i)
                case "cancel": return (.cancel, i)
                default: break
                }
            }
            i += 1
        }
        return nil
    }
}

func bugClock() -> Double { ProcessInfo.processInfo.systemUptime }

struct Bug {
    var id = 0
    var start = 0.0, stop = 0.0           // bugClock()
    var autoStopped = false               // left open for BugTracker.maxSeconds
    var words: [CaptionWord] = []         // what was said between start and stop
    // The MP4 recording that ran at "bug start" (its Recorder.serial; 0: none), in its own time (pauses left out).
    var recording = 0
    var recStart = 0.0, recStop = 0.0
    var recWords: [CaptionWord] = []
    var description: String { words.map(\.text).joined(separator: " ") }
}

struct BugEvent {
    enum Kind { case started, stopped, cancelled, ignored }
    var kind: Kind
    var bug: Bug
}

struct BugTracker {
    static let maxSeconds = 120.0  // a bug left open stops itself
    static let grace = 1.5         // after a stop by shortcut: words still on their way are kept

    private struct Sample {
        var t: Double
        var recording: Int
        var rec: Double
    }
    private var samples: [Sample] = []
    private var open: Bug?
    private var closing: Bug?
    private var nextId = 1

    var isOpen: Bool { open != nil }
    var busy: Bool { open != nil || closing != nil }  // open, or stopped and waiting for its last words

    // Something recognized. Commands inside the words count too; the command words never become part of the description.
    mutating func heard(_ words: [CaptionWord]) -> [BugEvent] {
        var out: [BugEvent] = []
        var i = 0
        while true {
            let found = BugWords.findCommand(words, from: i)
            let at = found?.at ?? words.count
            for w in words[min(i, at)..<at] {
                if open != nil, w.start >= open!.start - 1e-6 { open!.words.append(w) }
                else if open == nil, closing != nil, w.start < closing!.stop { closing!.words.append(w) }  // said before the shortcut
            }
            guard let c = found?.command else { break }
            let t = words[at].start
            switch c {
            case .start:
                if let o = open { out.append(BugEvent(kind: .ignored, bug: o)) }
                else {
                    if closing != nil { out.append(finish()) }
                    out.append(begin(t))
                }
            case .stop:
                if open != nil {
                    if closing != nil { out.append(finish()) }
                    close(t)
                    out.append(finish())  // the words before it were heard first: nothing is on its way
                }
            case .cancel:
                if let o = open {
                    out.append(BugEvent(kind: .cancelled, bug: o))
                    open = nil
                }
            }
            i = at + 2
        }
        return out
    }

    // The shortcut or the palette: starts a bug, or stops the open one (kept `grace` for the words still coming).
    mutating func toggle(_ t: Double) -> [BugEvent] {
        guard open != nil else { return [begin(t)] }
        var out: [BugEvent] = []
        if closing != nil { out.append(finish()) }
        close(t)
        return out
    }

    // The recording's clock at `t` (recording 0: none runs), several times a second while listening or a bug is open.
    // Also finishes stops and stops a bug left open too long.
    mutating func tick(_ t: Double, recording: Int, rec: Double) -> [BugEvent] {
        samples.append(Sample(t: t, recording: recording, rec: rec))
        if let first = samples.firstIndex(where: { $0.t >= t - 2 * BugTracker.maxSeconds }), first > 1 { samples.removeFirst(first - 1) }
        var out: [BugEvent] = []
        if let o = open, t - o.start >= BugTracker.maxSeconds {
            if closing != nil { out.append(finish()) }
            open!.autoStopped = true
            close(o.start + BugTracker.maxSeconds)
        }
        if let c = closing, t >= c.stop + BugTracker.grace { out.append(finish()) }
        return out
    }

    // Recording time at `t` for `recording`, from the samples tick kept (flat while it was paused).
    func recordingTime(_ recording: Int, _ t: Double) -> Double {
        let mine = samples.filter { $0.recording == recording }
        let before = mine.last { $0.t <= t }, after = mine.first { $0.t > t }
        if let b = before, let a = after { return b.rec + min(max(t - b.t, 0), a.rec - b.rec) }
        if let b = before {  // on from the last sample, but not past the recording's end (the first sample without it)
            let end = samples.first { $0.t > b.t && $0.recording != recording }?.t ?? t
            return b.rec + (min(t, end) - b.t)
        }
        if let a = after { return max(0, a.rec - (a.t - t)) }
        return 0
    }

    private mutating func begin(_ t: Double) -> BugEvent {
        var b = Bug()
        b.id = nextId
        nextId += 1
        b.start = t
        // The recording running then: the newest sample up to `t` (a voice command is heard a moment after it was said).
        b.recording = (samples.last { $0.t <= t + 1e-9 } ?? samples.first)?.recording ?? 0
        open = b
        return BugEvent(kind: .started, bug: b)
    }

    private mutating func close(_ t: Double) {
        guard var o = open else { return }
        o.stop = max(t, o.start)
        closing = o
        open = nil
    }

    private mutating func finish() -> BugEvent {
        var b = closing!
        closing = nil
        if b.recording != 0 {
            b.recStart = recordingTime(b.recording, b.start)
            b.recStop = max(b.recStart, recordingTime(b.recording, b.stop))
            b.recWords = b.words.map { w in
                let s = recordingTime(b.recording, w.start)
                return CaptionWord(start: s, end: max(s + 0.05, recordingTime(b.recording, w.end)), text: w.text)
            }
        }
        return BugEvent(kind: .stopped, bug: b)
    }
}

// What a recognition task has said so far, passed on word by word once it has settled: a word is final at the end of a
// task, or once the audio is a second past it (partial results still revise the last words).
struct BugTranscript {
    private var passed = -Double.infinity  // words starting at or before this were passed on

    mutating func settled(_ words: [CaptionWord], now: Double, final: Bool) -> [CaptionWord] {
        let ready = words.filter { $0.start > passed && (final || $0.end < now - 1) }
        if let last = ready.last { passed = last.start }
        return ready
    }
}

// MARK: - Logs

struct BugLog: Equatable {
    var source: String        // the game's file
    var file = ""             // the copy, in the bundle: logs/<name>
    var size: UInt64 = 0      // the source's size
    var copied: UInt64 = 0    // bytes in the copy
    var truncated = false     // only its end was kept
    var error = ""            // couldn't be copied
}

enum BugLogs {
    // ~, $NAME, ${NAME} and %NAME% (as on Windows) from the environment.
    static func expand(_ path: String) -> String {
        var s = (path as NSString).expandingTildeInPath
        let env = ProcessInfo.processInfo.environment
        for pattern in [#"\$\{([A-Za-z_][A-Za-z0-9_]*)\}"#, #"\$([A-Za-z_][A-Za-z0-9_]*)"#, #"%([A-Za-z_][A-Za-z0-9_]*)%"#] {
            guard let re = try? NSRegularExpression(pattern: pattern) else { continue }
            for m in re.matches(in: s, range: NSRange(s.startIndex..., in: s)).reversed() {
                guard let whole = Range(m.range, in: s), let name = Range(m.range(at: 1), in: s), let v = env[String(s[name])] else { continue }
                s.replaceSubrange(whole, with: v)
            }
        }
        return s
    }

    // The files LogPaths names now: each file named; from a folder or wildcard the files changed in the last 30 minutes
    // before `now`, or the newest if none was, at most 20. `missing`: entries matching nothing.
    static func find(_ paths: String, now: Date, missing: inout [String]) -> [URL] {
        var out: [URL] = []
        var seen = Set<String>()
        func add(_ u: URL) { if seen.insert(u.path.lowercased()).inserted { out.append(u) } }
        let fm = FileManager.default
        for raw in paths.split(separator: ";") {
            let entry = raw.trimmingCharacters(in: CharacterSet.whitespaces.union(CharacterSet(charactersIn: "\"")))
            guard !entry.isEmpty else { continue }
            var path = expand(entry)
            while path.count > 1, path.hasSuffix("/") { path.removeLast() }
            var isDir: ObjCBool = false
            let exists = fm.fileExists(atPath: path, isDirectory: &isDir)
            if exists && !isDir.boolValue {
                add(URL(fileURLWithPath: path))
                continue
            }
            let dir: String, pattern: String
            if exists {
                dir = path
                pattern = "*"
            } else if path.contains("*") || path.contains("?") {
                dir = (path as NSString).deletingLastPathComponent
                pattern = (path as NSString).lastPathComponent
            } else {
                missing.append(entry)
                continue
            }
            var files: [(URL, Date)] = []
            for name in (try? fm.contentsOfDirectory(atPath: dir.isEmpty ? "." : dir)) ?? [] where fnmatch(pattern, name, 0) == 0 {
                let u = URL(fileURLWithPath: dir).appendingPathComponent(name)
                guard let v = try? u.resourceValues(forKeys: [.isRegularFileKey, .contentModificationDateKey]), v.isRegularFile == true else { continue }
                files.append((u, v.contentModificationDate ?? .distantPast))
            }
            guard !files.isEmpty else {
                missing.append(entry)
                continue
            }
            files.sort { $0.1 > $1.1 }
            let recent = files.filter { $0.1 >= now.addingTimeInterval(-30 * 60) }.count
            for f in files.prefix(min(recent > 0 ? recent : 1, 20)) { add(f.0) }
        }
        return out
    }

    // Copies the logs into `dir`. A file over `maxBytes` keeps its last `maxBytes`, from the start of a line. A game can
    // keep writing its log meanwhile: the copy goes up to the size seen first.
    static func copy(_ files: [URL], to dir: URL, maxBytes: UInt64) -> [BugLog] {
        guard !files.isEmpty else { return [] }
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var names = Set<String>()
        var out: [BugLog] = []
        for src in files {
            var l = BugLog(source: src.path)
            var name = src.lastPathComponent
            let stem = (name as NSString).deletingPathExtension, ext = (name as NSString).pathExtension
            var k = 2
            while names.contains(name.lowercased()) {
                name = stem + " (\(k))" + (ext.isEmpty ? "" : "." + ext)
                k += 1
            }
            names.insert(name.lowercased())
            guard let h = try? FileHandle(forReadingFrom: src) else {
                l.error = "couldn't be read"
                out.append(l)
                continue
            }
            defer { try? h.close() }
            l.size = (try? h.seekToEnd()) ?? 0
            var from: UInt64 = 0
            if l.size > maxBytes {
                l.truncated = true
                from = l.size - maxBytes
                try? h.seek(toOffset: from)
                if let head = try? h.read(upToCount: Int(min(64 << 10, maxBytes))), let nl = head.firstIndex(of: 10) { from += UInt64(nl - head.startIndex) + 1 }
            }
            try? h.seek(toOffset: from)
            let dst = dir.appendingPathComponent(name)
            guard FileManager.default.createFile(atPath: dst.path, contents: nil), let o = try? FileHandle(forWritingTo: dst) else {
                l.error = "couldn't be copied"
                out.append(l)
                continue
            }
            let want = l.size - from
            while l.copied < want, let chunk = try? h.read(upToCount: Int(min(1 << 20, want - l.copied))), !chunk.isEmpty {
                o.write(chunk)
                l.copied += UInt64(chunk.count)
            }
            try? o.close()
            l.file = "logs/" + name
            out.append(l)
        }
        return out
    }
}

// MARK: - The picture

enum BugOverlay {
    static let maxLines = 6

    struct Layout {
        var bar = CGRect.zero  // where the bar went (top-left origin)
        var lines = 0          // description lines drawn
        var cut = false        // the description was longer than maxLines
    }

    // bug.png: the screenshot with a bar along the bottom, a header line ("BUG · 2026-10-21 14:03:22 · Game") and the
    // description wrapped below it (at most maxLines, then "…"), white on dark translucent, sized to the picture.
    static func draw(_ shot: CGImage, header: String, description: String) -> (CGImage, Layout)? {
        let W = CGFloat(shot.width), H = CGFloat(shot.height)
        guard let ctx = makeContext(width: shot.width, height: shot.height) else { return nil }
        drawImageFlipped(ctx, shot, in: CGRect(x: 0, y: 0, width: W, height: H))
        // Sized to the picture like captions are to a video, and still readable when a chat shows it small.
        let fs = max(14, min(H * 0.034, W / 40)), pad = fs * 0.75, width = max(1, W - 2 * pad)
        let head = NSMutableAttributedString(string: header, attributes: [.font: NSFont.systemFont(ofSize: fs * 0.62, weight: .bold),
                                                                          .foregroundColor: Theme.rgb(208, 206, 199)])
        if header.hasPrefix("BUG") { head.addAttribute(.foregroundColor, value: Theme.accent, range: NSRange(location: 0, length: 3)) }
        let textAttrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: fs, weight: .semibold), .foregroundColor: NSColor.white]
        func height(_ s: NSAttributedString) -> CGFloat {
            ceil(s.boundingRect(with: CGSize(width: width, height: .greatestFiniteMagnitude), options: [.usesLineFragmentOrigin, .usesFontLeading]).height)
        }
        var lay = Layout()
        var text = description.split(whereSeparator: \.isWhitespace).joined(separator: " ")  // one paragraph
        let lineH = height(NSAttributedString(string: "Ag", attributes: textAttrs))
        var textH: CGFloat = 0
        if !text.isEmpty {
            let maxH = lineH * (CGFloat(maxLines) + 0.5)
            if height(NSAttributedString(string: text, attributes: textAttrs)) > maxH {  // the most whole words that fit with "…"
                lay.cut = true
                let words = text.split(separator: " ")
                var lo = 0, hi = words.count
                while lo < hi {
                    let mid = (lo + hi + 1) / 2
                    if height(NSAttributedString(string: words.prefix(mid).joined(separator: " ") + "…", attributes: textAttrs)) <= maxH { lo = mid } else { hi = mid - 1 }
                }
                text = words.prefix(lo).joined(separator: " ") + "…"
            }
            textH = height(NSAttributedString(string: text, attributes: textAttrs))
            lay.lines = max(1, Int((textH / max(1, lineH)).rounded()))
        }
        let headH = height(head), gap = text.isEmpty ? 0 : fs * 0.3
        let top = min(H, max(0, floor(H - (pad + headH + gap + textH + pad))))
        ctx.setFillColor(NSColor.black.withAlphaComponent(0.72).cgColor)
        ctx.fill(CGRect(x: 0, y: top, width: W, height: H - top))
        withNSContext(ctx, flipped: true) {
            head.draw(with: CGRect(x: pad, y: top + pad, width: width, height: headH + 2), options: [.usesLineFragmentOrigin, .usesFontLeading])
            if !text.isEmpty {
                NSAttributedString(string: text, attributes: textAttrs)
                    .draw(with: CGRect(x: pad, y: top + pad + headH + gap, width: width, height: textH + 2), options: [.usesLineFragmentOrigin, .usesFontLeading])
            }
        }
        lay.bar = CGRect(x: 0, y: top, width: W, height: H - top)
        guard let img = ctx.makeImage() else { return nil }
        return (img, lay)
    }
}

// MARK: - The bundle

// The server transcription of voice.wav (Transcribe Vietnamese with Apple's servers): a seam, so tests see the request.
protocol BugTranscriber {
    func transcribe(_ wav: URL, locale: String, onDevice: Bool) async throws -> [CaptionWord]
}

struct AppleSpeech: BugTranscriber {
    func transcribe(_ wav: URL, locale: String, onDevice: Bool) async throws -> [CaptionWord] {
        guard SFSpeechRecognizer.authorizationStatus() == .authorized else {
            throw VideoExport.Failure.failed("Allow Ather Screenshot in System Settings › Privacy & Security › Speech Recognition.")
        }
        guard let rec = SFSpeechRecognizer(locale: Locale(identifier: locale)), rec.isAvailable else {
            throw VideoExport.Failure.failed("Speech recognition for \(locale) isn't available right now.")
        }
        let req = SFSpeechURLRecognitionRequest(url: wav)
        req.requiresOnDeviceRecognition = onDevice
        req.shouldReportPartialResults = false
        req.addsPunctuation = true
        return try await BugSpeech.words(rec, req)
    }
}

final class BugBundle {
    var bug: Bug
    var id = UUID().uuidString
    var wallStart = Date()
    var app = "", window = ""
    var area = CGRect.zero               // the screenshot's area on screen (CG global points)
    var screenshot: CGImage?
    var folder: URL
    var logs: [BugLog] = []
    var missingLogs: [String] = []
    var voice = false                     // voice.wav was written
    var voiceStart = 0.0                  // bugClock() of voice.wav's first sample
    var transcription: (locale: String, error: String?)?  // the server transcription, when asked for
    enum Clip: String { case none, pending, saved, failed }
    var clip = Clip.none
    var recordingPath = ""
    var clipFrom = 0.0, clipTo = 0.0      // recording seconds
    var clipNote = ""                     // why there's no clip, or why it failed

    init(bug: Bug, folder: URL) {
        self.bug = bug
        self.folder = folder
    }

    private static func local(_ d: Date, _ format: String) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = format
        return f.string(from: d)
    }

    // "2026-10-21 14-03-22 the door does not open" (file-name safe, the words cut to about 40 characters).
    static func folderName(_ wallStart: Date, _ description: String) -> String {
        let stamp = local(wallStart, "yyyy-MM-dd HH-mm-ss")
        let cleaned = String(description.map { c -> Character in ("<>:\"/\\|?*".contains(c) || c.isNewline || c.asciiValue.map { $0 < 32 } == true) ? " " : c })
        var words = cleaned.split(separator: " ").joined(separator: " ")
        if words.count > 40 {
            let head = String(words.prefix(41))
            if let sp = head.lastIndex(of: " "), head.distance(from: head.startIndex, to: sp) >= 20 { words = String(head[..<sp]) } else { words = String(words.prefix(40)) }
        }
        while words.hasSuffix(" ") || words.hasSuffix(".") { words.removeLast() }
        return words.isEmpty ? stamp : stamp + " " + words
    }

    var title: String {
        let d = bug.description
        var end = d.endIndex
        var i = d.startIndex
        while i < d.endIndex {
            let c = d[i], next = d.index(after: i)
            if (c == "." || c == "!" || c == "?") && (next == d.endIndex || d[next] == " ") {
                end = c == "." ? i : next
                break
            }
            i = next
        }
        var t = d[..<end].trimmingCharacters(in: .whitespaces)
        if t.count > 80 {
            let head = String(t.prefix(80))
            t = (head.lastIndex(of: " ").map { String(head[..<$0]) } ?? head) + "…"
        }
        guard let first = t.first else { return "Bug at " + BugBundle.local(wallStart, "HH:mm:ss") }
        return first.uppercased() + String(t.dropFirst())
    }

    var header: String { "BUG · " + BugBundle.local(wallStart, "yyyy-MM-dd HH:mm:ss") + (app.isEmpty ? "" : " · " + app) }

    private func exists(_ name: String) -> Bool { FileManager.default.fileExists(atPath: folder.appendingPathComponent(name).path) }

    private static func timecode(_ s: Double) -> String {
        let tenths = Int((max(0, s) * 10).rounded())
        let h = tenths / 36000, m = tenths / 600 % 60, sec = tenths / 10 % 60, t = tenths % 10
        return h > 0 ? String(format: "%d:%02d:%02d.%d", h, m, sec, t) : String(format: "%d:%02d.%d", m, sec, t)
    }

    private static func size(_ b: UInt64) -> String {
        b >= 1 << 20 ? String(format: "%.1f MB", Double(b) / 1048576) : b >= 1024 ? "\((b + 512) / 1024) KB" : "\(b) bytes"
    }

    private var clipLine: String {
        switch clip {
        case .saved: return "`clip.mp4`, \(BugBundle.timecode(clipFrom)) to \(BugBundle.timecode(clipTo)) of the recording, with captions"
        case .pending: return "when the recording stops"
        case .failed: return "couldn't be made: " + clipNote
        case .none: return "none: " + (clipNote.isEmpty ? "nothing was being recorded" : clipNote)
        }
    }

    var files: [String] {
        ["bug.png", "screenshot.png", "clip.mp4", "voice.wav"].filter(exists) + logs.filter { !$0.file.isEmpty }.map(\.file) + ["bug.md", "bug.json"]
    }

    var markdown: String {
        var s = "# \(title)\n\n"
        s += "- **When:** \(BugBundle.local(wallStart, "yyyy-MM-dd HH:mm:ss")), for \(Int((bug.stop - bug.start).rounded())) s"
            + (bug.autoStopped ? " (it stopped itself after 2 minutes)" : "") + "\n"
        if !app.isEmpty || !window.isEmpty { s += "- **App:** \(app.isEmpty ? "?" : app)" + (window.isEmpty ? "" : ", “\(window)”") + "\n" }
        if bug.recording != 0 {
            s += "- **Recording:** " + (recordingPath.isEmpty ? "the screen recording" : "`\((recordingPath as NSString).lastPathComponent)`")
                + ", \(BugBundle.timecode(bug.recStart)) to \(BugBundle.timecode(bug.recStop))\n"
        }
        s += "- **Clip:** \(clipLine)\n"
        if let t = transcription {
            s += "- **Transcribed by:** Apple's servers (\(t.locale))" + (t.error.map { ", which failed: \($0)" } ?? "") + "\n"
        }
        s += "\n## What was said\n\n"
        let d = bug.description
        s += d.isEmpty ? (voice ? "Nothing was recognized: listen to `voice.wav`." : "Nothing: voice commands were off.") : d
        s += "\n\n## Files\n\n"
        if exists("bug.png") { s += "- `bug.png`: the screenshot with what was said\n" }
        if exists("screenshot.png") { s += "- `screenshot.png`: the screenshot as it was\n" }
        if clip == .saved { s += "- `clip.mp4`: the recording around the bug\n" }
        if voice { s += "- `voice.wav`: what the microphone heard\n" }
        for l in logs {
            if !l.error.isEmpty { s += "- Couldn't copy `\(l.source)`: \(l.error)\n" }
            else { s += "- `\(l.file)`: from `\(l.source)`, \(BugBundle.size(l.size))" + (l.truncated ? " (only its last \(BugBundle.size(l.copied)))" : "") + "\n" }
        }
        for m in missingLogs { s += "- Not found: `\(m)`\n" }
        return s
    }

    var json: [String: Any] {
        func r(_ v: Double) -> Double { (v * 100).rounded() / 100 }
        let iso = ISO8601DateFormatter()
        var j: [String: Any] = [
            "app": "Ather Screenshot", "format": 1, "kind": "bug", "id": id, "title": title, "description": bug.description,
            "started": iso.string(from: wallStart), "started_local": BugBundle.local(wallStart, "yyyy-MM-dd HH:mm:ss"),
            "stopped": iso.string(from: wallStart.addingTimeInterval(bug.stop - bug.start)), "seconds": r(bug.stop - bug.start),
            "auto_stopped": bug.autoStopped, "foreground": ["app": app, "window": window],
        ]
        j["words"] = bug.words.enumerated().map { i, w -> [String: Any] in
            var e: [String: Any] = ["text": w.text, "start": r(w.start - bug.start), "end": r(w.end - bug.start)]  // seconds after "bug start"
            if i < bug.recWords.count { e["recording_time"] = r(bug.recWords[i].start) }
            return e
        }
        if exists("screenshot.png") {
            let a: [String: Any] = ["x": r(area.minX), "y": r(area.minY), "width": r(area.width), "height": r(area.height)]
            j["screenshot"] = ["file": "screenshot.png", "overlay": "bug.png", "area": a] as [String: Any]
        } else {
            j["screenshot"] = NSNull()
        }
        if bug.recording != 0 {
            j["recording"] = ["path": recordingPath.isEmpty ? NSNull() as Any : recordingPath as Any, "start": r(bug.recStart), "stop": r(bug.recStop)] as [String: Any]
        } else {
            j["recording"] = NSNull()
        }
        var c: [String: Any] = ["state": clip.rawValue]
        if clip == .saved {
            c["file"] = "clip.mp4"
            c["from"] = r(clipFrom)
            c["to"] = r(clipTo)
            c["captions"] = !bug.recWords.isEmpty
        }
        if !clipNote.isEmpty { c["note"] = clipNote } else if clip == .none { c["note"] = "nothing was being recorded" }
        j["clip"] = c
        j["voice"] = voice ? "voice.wav" : NSNull()
        if let t = transcription {
            var e: [String: Any] = ["locale": t.locale, "on_device": false, "source": "voice.wav"]
            if let err = t.error { e["error"] = err }
            j["transcription"] = e
        }
        j["logs"] = logs.map { l -> [String: Any] in
            var e: [String: Any] = ["source": l.source, "size": l.size, "copied": l.copied, "truncated": l.truncated]
            if !l.file.isEmpty { e["file"] = l.file }
            if l.truncated { e["note"] = "Only the end of the log was kept: it was over the size limit (LogMaxMB)." }
            if !l.error.isEmpty { e["error"] = l.error }
            return e
        }
        j["missing_logs"] = missingLogs
        j["files"] = files
        return j
    }

    func writeText() {
        try? Data(markdown.utf8).write(to: folder.appendingPathComponent("bug.md"), options: .atomic)
        if let d = try? JSONSerialization.data(withJSONObject: json, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]) {
            try? d.write(to: folder.appendingPathComponent("bug.json"), options: .atomic)
        }
    }

    // Starts the folder (it must not exist yet) with voice.wav: 16 kHz mono, may be empty.
    func begin(voice samples: [Int16]) throws {
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        if !samples.isEmpty { voice = (try? BugBundle.wav(samples, rate: 16000).write(to: folder.appendingPathComponent("voice.wav"))) != nil }
    }

    // With "Transcribe Vietnamese with Apple's servers" on: voice.wav, and only it, goes to the server as vi-VN; what comes
    // back becomes the description and the captions. `recordingTime` maps bug-clock times onto the recording's.
    func transcribeVietnamese(_ t: BugTranscriber, recordingTime: (Double) -> Double) async {
        guard voice else { return }
        do {
            let words = try await t.transcribe(folder.appendingPathComponent("voice.wav"), locale: "vi-VN", onDevice: false)
            transcription = ("vi-VN", nil)
            guard !words.isEmpty else { return }
            bug.words = words.map { CaptionWord(start: voiceStart + $0.start, end: voiceStart + $0.end, text: $0.text) }
            if bug.recording != 0 {
                bug.recWords = bug.words.map { w in
                    let s = recordingTime(w.start)
                    return CaptionWord(start: s, end: max(s + 0.05, recordingTime(w.end)), text: w.text)
                }
            }
        } catch {
            transcription = ("vi-VN", error.localizedDescription)  // the on-device words (if any) and voice.wav stay
        }
    }

    // The pictures, the logs, bug.md and bug.json.
    func finish(logPaths: String, logMaxBytes: UInt64) {
        if let shot = screenshot {
            try? shot.pngData()?.write(to: folder.appendingPathComponent("screenshot.png"))
            if let (over, _) = BugOverlay.draw(shot, header: header, description: bug.description) { try? over.pngData()?.write(to: folder.appendingPathComponent("bug.png")) }
        }
        missingLogs = []
        logs = BugLogs.copy(BugLogs.find(logPaths, now: Date(), missing: &missingLogs), to: folder.appendingPathComponent("logs"), maxBytes: logMaxBytes)
        writeText()
    }

    // Once the recording has stopped: the clip from `preRoll` seconds before the bug to its end (H.264, the words as
    // captions), and the bug as an Issue note on the recording; then bug.md and bug.json again.
    func finishClip(_ o: Recorder.Outcome, preRoll: Double) async {
        defer { writeText() }
        guard let rec = o.url, rec.pathExtension.lowercased() == "mp4" else {
            clip = .none
            clipNote = o.discarded ? "the recording was discarded" : "the recording wasn't saved"
            return
        }
        recordingPath = rec.path
        do {
            let c = try await VideoSource.probe(rec)
            clipFrom = min(max(0, bug.recStart - preRoll), c.length)
            clipTo = min(max(bug.recStop, clipFrom), c.length)
            guard clipTo - clipFrom >= 0.1 else { throw VideoExport.Failure.failed("the bug is past the end of the recording") }
            var e = VideoEdit(trimEnd: clipTo, clips: [c], frame: CGSize(width: c.w, height: c.h))
            e.trimStart = clipFrom
            e.captions = VideoExport.chunk(bug.recWords.map { ($0.start, $0.end, $0.text) })
            // H.264 for chats and Windows, at a rate that keeps a bug's clip small.
            let r = try await Compress.edit(e, cap: Compress.bytes(mb: min(50, max(4, (clipTo - clipFrom) * 0.75))))
            guard let tmp = r.tmp else { throw VideoExport.Failure.failed("The clip wasn't written.") }
            let dst = folder.appendingPathComponent("clip.mp4")
            try? FileManager.default.removeItem(at: dst)
            try FileManager.default.moveItem(at: tmp, to: dst)
            clip = .saved
            clipNote = ""
            VideoNotes.add([VideoNotes.New(time: bug.recStart, end: bug.recStop > bug.recStart ? bug.recStop : nil, kind: "issue",
                                           text: "Bug: " + (bug.words.isEmpty ? folder.lastPathComponent : bug.description), author: "Bug report")],
                           to: rec, fps: c.fps > 0 ? c.fps : 30)
        } catch {
            clip = .failed
            clipNote = error.localizedDescription
        }
    }

    static func wav(_ pcm: [Int16], rate: Int) -> Data {
        var d = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        let bytes = UInt32(pcm.count * 2)
        d.append(contentsOf: Array("RIFF".utf8)); u32(36 + bytes); d.append(contentsOf: Array("WAVE".utf8))
        d.append(contentsOf: Array("fmt ".utf8)); u32(16); u16(1); u16(1); u32(UInt32(rate)); u32(UInt32(rate * 2)); u16(2); u16(16)
        d.append(contentsOf: Array("data".utf8)); u32(bytes)
        pcm.withUnsafeBytes { d.append(contentsOf: $0) }
        return d
    }
}

// MARK: - Voice: on-device speech recognition on the microphone

// The voice commands' listener: AVAudioEngine's input into SFSpeechRecognizer (en-US) with on-device recognition only;
// a Mac without it gets no voice commands. Words pass on once settled (BugTranscript); the task is renewed every minute
// or so between bugs. The microphone's audio also goes to voice.wav while a bug is open.
final class BugSpeech {
    static let commands = ["bug start", "bug stop", "bug cancel"]

    // Every request leaves the speech on this Mac.
    static func liveRequest() -> SFSpeechAudioBufferRecognitionRequest {
        let r = SFSpeechAudioBufferRecognitionRequest()
        r.requiresOnDeviceRecognition = true
        r.shouldReportPartialResults = true
        r.addsPunctuation = true
        r.contextualStrings = commands
        return r
    }
    static func fileRequest(_ url: URL) -> SFSpeechURLRecognitionRequest {
        let r = SFSpeechURLRecognitionRequest(url: url)
        r.requiresOnDeviceRecognition = true
        r.shouldReportPartialResults = false
        r.addsPunctuation = true
        r.contextualStrings = commands
        return r
    }

    static func words(_ t: SFTranscription, offset: Double) -> [CaptionWord] {
        t.segments.flatMap { s -> [CaptionWord] in
            let parts = s.substring.split(separator: " ")
            return parts.map { CaptionWord(start: offset + s.timestamp, end: offset + s.timestamp + max(0.05, s.duration), text: String($0)) }
        }
    }

    // A whole file's words (no partial results).
    static func words(_ rec: SFSpeechRecognizer, _ req: SFSpeechURLRecognitionRequest) async throws -> [CaptionWord] {
        try await withCheckedThrowingContinuation { k in
            var done = false
            rec.recognitionTask(with: req) { result, error in
                guard !done else { return }
                if let result, result.isFinal { done = true; k.resume(returning: words(result.bestTranscription, offset: 0)) }
                else if let error { done = true; k.resume(throwing: VideoExport.Failure.failed("Couldn't transcribe: \(error.localizedDescription)")) }
            }
        }
    }

    var onHeard: (([CaptionWord]) -> Void)?   // main thread
    private let engine = AVAudioEngine()
    private var recognizer: SFSpeechRecognizer?
    private let lock = NSLock()
    private var request: SFSpeechAudioBufferRecognitionRequest?  // under `lock`: the tap feeds it
    private var task: SFSpeechRecognitionTask?
    private var taskStart = 0.0
    private final class Feed {  // one task's transcript: its final words still come in after a newer task started
        var transcript = BugTranscript()
        var latest: [CaptionWord] = []
    }
    private var feed = Feed()
    private var timer: Timer?
    private var tape: [Float]?                    // under `lock`: voice.wav while a bug is open
    private var tapeRate = 48000.0
    private(set) var tapeStart = 0.0
    var keepTask = false                          // a bug is open: don't renew the task in the middle of it

    func start() throws {
        guard let rec = SFSpeechRecognizer(locale: Locale(identifier: "en-US")), rec.supportsOnDeviceRecognition else {
            throw VideoExport.Failure.failed("This Mac can't recognize speech on the device, so voice commands are off.")
        }
        recognizer = rec
        let input = engine.inputNode
        let fmt = input.outputFormat(forBus: 0)
        guard fmt.sampleRate > 0, fmt.channelCount > 0 else { throw VideoExport.Failure.failed("There's no microphone to listen to.") }
        tapeRate = fmt.sampleRate
        input.installTap(onBus: 0, bufferSize: 4096, format: fmt) { [weak self] buf, _ in
            guard let self else { return }
            self.lock.lock()
            self.request?.append(buf)
            if self.tape != nil, let ch = buf.floatChannelData, self.tape!.count < Int(self.tapeRate * 135) {
                self.tape!.append(contentsOf: UnsafeBufferPointer(start: ch[0], count: Int(buf.frameLength)))
            }
            self.lock.unlock()
        }
        engine.prepare()
        try engine.start()
        renew()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.poll() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        lock.lock()
        request?.endAudio()
        request = nil
        tape = nil
        lock.unlock()
        task?.cancel()
        task = nil
    }

    func startTape() {
        lock.lock()
        tape = []
        tapeStart = bugClock()
        lock.unlock()
    }

    // What the microphone heard since startTape, as 16 kHz mono.
    func stopTape() -> [Int16] {
        lock.lock()
        let t = tape ?? []
        tape = nil
        lock.unlock()
        return BugSpeech.resample(t, from: tapeRate)
    }

    static func resample(_ s: [Float], from rate: Double) -> [Int16] {
        guard rate > 0, !s.isEmpty else { return [] }
        let step = rate / 16000, n = Int(Double(s.count) / step)
        return (0..<n).map { i in
            let p = Double(i) * step, k = Int(p), f = Float(p - Double(k))
            let v = s[k] * (1 - f) + (k + 1 < s.count ? s[k + 1] : s[k]) * f
            return Int16(max(-1, min(1, v)) * 32767)
        }
    }

    // A new recognition task (the last one's final words still come in).
    private func renew() {
        guard let rec = recognizer else { return }
        let req = BugSpeech.liveRequest()
        lock.lock()
        request?.endAudio()
        request = req
        lock.unlock()
        let start = bugClock(), f = Feed()
        taskStart = start
        feed = f
        task = rec.recognitionTask(with: req) { [weak self] result, error in
            DispatchQueue.main.async {
                guard let self else { return }
                if let result {
                    f.latest = BugSpeech.words(result.bestTranscription, offset: start)
                    let ready = f.transcript.settled(f.latest, now: bugClock(), final: result.isFinal)
                    if !ready.isEmpty { self.onHeard?(ready) }
                }
                if self.feed === f, error != nil || result?.isFinal == true { self.renew() }
            }
        }
    }

    private func poll() {
        let ready = feed.transcript.settled(feed.latest, now: bugClock(), final: false)
        if !ready.isEmpty { onHeard?(ready) }
        if !keepTask, bugClock() - taskStart > 50 { renew() }
    }
}

// MARK: - In the app

final class BugReports {
    static let shared = BugReports()

    struct Options {
        var folder: URL
        var voice = false            // VoiceCommands
        var logPaths = ""            // LogPaths
        var preRoll = 10.0           // PreRollSeconds
        var logMaxMB = 20            // LogMaxMB
        var cloudVietnamese = false  // CloudVietnamese
        var cursor = false
        var hotkey = ""              // the BugReport shortcut, for the notifications

        static func fromSettings() -> Options {
            let s = Settings.shared
            return Options(folder: s.capturesFolder, voice: s.bool("VoiceCommands"), logPaths: s.string("LogPaths"),
                           preRoll: Double(min(120, max(0, s.int("PreRollSeconds")))), logMaxMB: min(1024, max(1, s.int("LogMaxMB"))),
                           cloudVietnamese: s.bool("CloudVietnamese"), cursor: s.bool("CaptureCursor"), hotkey: s.hotkey(.bugReport))
        }
    }

    // A recording now: its serial, its time (pauses left out), its area and whether it's a GIF.
    struct RecordingNow {
        var serial = 0
        var seconds = 0.0
        var area: CGRect?
        var gif = false
    }

    var options = Options(folder: Settings.shared.capturesFolder)
    // Seams (tests): the screenshot, the recording clock, the server transcription, showing a folder.
    var grab: (CGRect, Bool) async -> CGImage? = { r, cursor in try? await Capture.rect(r, cursor: cursor) }
    var recordingNow: () -> RecordingNow = {
        guard let r = Recorder.current, r.hasFootage else { return RecordingNow() }
        return RecordingNow(serial: r.serial, seconds: max(0, r.elapsed), area: r.area, gif: r.gif)
    }
    var transcriber: BugTranscriber = AppleSpeech()
    var showFolder: (URL) -> Void = { Output.open($0) }
    var onListeningChanged: (() -> Void)?

    private(set) var listening = false
    private(set) var last: BugBundle?
    private(set) var busy = 0
    private var tracker = BugTracker()
    private struct StartInfo {
        var wall = Date()
        var app = "", window = ""
        var area = CGRect.zero
        var shot: Task<CGImage?, Never>?
        var taped = false
    }
    private var starts: [Int: StartInfo] = [:]
    private var timer: Timer?
    private var speech: BugSpeech?
    private var voiceTried = false
    private struct RecordingBugs {
        var written: [BugBundle] = []
        var finished: Recorder.Outcome?
    }
    private var recordings: [Int: RecordingBugs] = [:]
    private var watched: Set<Int> = []

    var isOpen: Bool { tracker.isOpen }

    // Applies the settings: starts or stops listening to match (a recognizer or microphone that's missing is said once).
    func configure(_ o: Options) {
        options = o
        if !o.voice {
            voiceTried = false
            if speech != nil || listening {
                speech?.stop()
                speech = nil
                listening = false
                onListeningChanged?()
            }
        } else if speech == nil, !voiceTried {
            voiceTried = true
            startListening()
        }
        sync()
    }

    private func startListening() {
        // Only here, in the app, after the user turned it on: never from tests.
        SFSpeechRecognizer.requestAuthorization { [weak self] status in
            AVCaptureDevice.requestAccess(for: .audio) { mic in
                DispatchQueue.main.async {
                    guard let self, self.options.voice, self.speech == nil else { return }
                    let hk = self.hotkeyText
                    guard status == .authorized, mic else {
                        return Toast.shared.show("Voice commands are off",
                                                 "Allow Ather Screenshot to use Speech Recognition and the Microphone in System Settings › Privacy & Security. \(hk) still makes bug reports.",
                                                 ms: 9000)
                    }
                    let sp = BugSpeech()
                    sp.onHeard = { [weak self] words in self?.heard(words) }
                    do {
                        try sp.start()
                    } catch {
                        return Toast.shared.show("Voice commands are off", "\(error.localizedDescription) \(hk) still makes bug reports.", ms: 9000)
                    }
                    self.speech = sp
                    self.listening = true
                    self.sync()
                    self.onListeningChanged?()
                }
            }
        }
    }

    private var hotkeyText: String { options.hotkey.isEmpty ? "“Bug report: start / stop”" : Hotkey.display(options.hotkey) }

    private func now() -> RecordingNow {
        let r = recordingNow()
        return r.gif ? RecordingNow(area: r.area, gif: true) : r
    }

    private func tick() {
        let r = now()
        handle(tracker.tick(bugClock(), recording: r.serial, rec: r.seconds))
    }

    func heard(_ words: [CaptionWord]) {
        tick()
        handle(tracker.heard(words))
    }

    // The shortcut and the palette.
    func toggle() {
        tick()
        let wasOpen = tracker.isOpen
        handle(tracker.toggle(bugClock()))
        if wasOpen { Toast.shared.show("Saving the bug…", ms: 3000) }
    }

    private func sync() {
        speech?.keepTask = tracker.busy
        let need = listening || tracker.busy
        if need && timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.tick() }
        } else if !need, let t = timer {
            t.invalidate()
            timer = nil
        }
    }

    private func handle(_ events: [BugEvent]) {
        for e in events {
            switch e.kind {
            case .started: started(e.bug)
            case .stopped: stopped(e.bug)
            case .cancelled:
                if starts.removeValue(forKey: e.bug.id)?.taped == true { _ = speech?.stopTape() }
                Toast.shared.show("Bug cancelled", "Nothing was saved.")
            case .ignored: break  // "bug start" while one is open
            }
        }
        sync()
    }

    private func started(_ bug: Bug) {
        var s = StartInfo()
        s.wall = Date().addingTimeInterval(bug.start - bugClock())
        let front = Capture.activeWindow().flatMap { $0.pid == getpid() ? nil : $0 }
        s.app = front?.app ?? ""
        s.window = front?.title ?? ""
        // What's recorded, or else the display with the app in front.
        s.area = recordingNow().area ?? Geo.toCG(Geo.screen(at: front.map { CGPoint(x: $0.frame.midX, y: $0.frame.midY) } ?? Geo.mouse).frame)
        let grab = self.grab, area = s.area, cursor = options.cursor
        s.shot = Task { await grab(area, cursor) }
        if let sp = speech {
            sp.startTape()
            s.taped = true
        }
        starts[bug.id] = s
        if bug.recording != 0, !watched.contains(bug.recording), let r = Recorder.current, r.serial == bug.recording {
            watched.insert(bug.recording)
            let serial = bug.recording
            r.whenFinished.append { [weak self] o in self?.recordingFinished(serial: serial, o) }
        }
        Toast.shared.show("Bug started", listening ? "Describe it, then say “bug stop” (or press \(hotkeyText))." : "Press \(hotkeyText) again to save it.", ms: 3000)
    }

    private func stopped(_ bug: Bug) {
        let s = starts.removeValue(forKey: bug.id) ?? StartInfo(wall: Date().addingTimeInterval(bug.start - bugClock()))
        let base = options.folder.appendingPathComponent("Bugs", isDirectory: true)
        var folder = base.appendingPathComponent(BugBundle.folderName(s.wall, bug.description), isDirectory: true)
        var k = 2
        while FileManager.default.fileExists(atPath: folder.path) {
            folder = base.appendingPathComponent(BugBundle.folderName(s.wall, bug.description) + " (\(k))", isDirectory: true)
            k += 1
        }
        let b = BugBundle(bug: bug, folder: folder)
        b.wallStart = s.wall
        b.app = s.app
        b.window = s.window
        b.area = s.area
        b.clip = bug.recording != 0 ? .pending : .none
        let voice = s.taped ? (speech?.stopTape() ?? []) : []
        b.voiceStart = speech?.tapeStart ?? bug.start
        let mapper = tracker, o = options, transcriber = self.transcriber
        busy += 1
        Task { @MainActor in
            defer { self.busy -= 1 }
            b.screenshot = await s.shot?.value
            do {
                try b.begin(voice: voice)
            } catch {
                return Toast.shared.show("The bug couldn't be saved", error.localizedDescription, ms: 6000)
            }
            if o.cloudVietnamese { await b.transcribeVietnamese(transcriber) { mapper.recordingTime(bug.recording, $0) } }
            self.noteInGallery(b, ["bug.png", "screenshot.png"])  // before the files appear, so the gallery tags them
            await Task.detached { b.finish(logPaths: o.logPaths, logMaxBytes: UInt64(o.logMaxMB) << 20) }.value
            self.saved(b)
        }
    }

    private func noteInGallery(_ b: BugBundle, _ names: [String]) {
        var info = NameInfo()
        info.app = b.app
        info.window = b.window
        for n in names {
            let u = b.folder.appendingPathComponent(n)
            Library.shared.noteCapture(u, info: info)
            Library.shared.noteCaptureDetails(u, tags: ["bug"], comment: b.bug.description)
        }
    }

    private func saved(_ b: BugBundle) {
        last = b
        var body = b.title + "\n"
        if b.bug.autoStopped { body += "It stopped itself after 2 minutes. " }
        if b.clip == .pending { body += "The clip is cut when the recording stops. " }
        Toast.shared.show("Bug saved", body + "Click to open the folder.", image: b.screenshot, ms: 4500) { [weak self] in self?.showFolder(b.folder) }
        guard b.bug.recording != 0 else { return }
        if let o = recordings[b.bug.recording]?.finished { cutClips([b], o) } else { recordings[b.bug.recording, default: RecordingBugs()].written.append(b) }
    }

    // The recording `serial` ended: clips for its bugs. The recorder calls it; tests too.
    func recordingFinished(serial: Int, _ o: Recorder.Outcome) {
        var rb = recordings[serial] ?? RecordingBugs()
        rb.finished = o
        let waiting = rb.written
        rb.written = []
        recordings[serial] = rb
        cutClips(waiting, o)
    }

    private func cutClips(_ bundles: [BugBundle], _ o: Recorder.Outcome) {
        guard !bundles.isEmpty else { return }
        if o.url != nil { for b in bundles { noteInGallery(b, ["clip.mp4"]) } }
        let preRoll = options.preRoll
        busy += 1
        Task { @MainActor in
            defer { self.busy -= 1 }
            for b in bundles { await b.finishClip(o, preRoll: preRoll) }  // one by one: their notes go into one sidecar
        }
    }

    // The folder of the newest bug saved: this session's, or the newest in Bugs with a bug.md.
    var lastFolder: URL? {
        if let l = last { return l.folder }
        let dir = options.folder.appendingPathComponent("Bugs", isDirectory: true)
        let items = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.creationDateKey])) ?? []
        return items.filter { FileManager.default.fileExists(atPath: $0.appendingPathComponent("bug.md").path) }
            .max { ((try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast) < ((try? $1.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast) }
    }

    // bug.png, clip.mp4, bug.md and up to 10 logs onto `pasteboard` as files.
    @discardableResult
    func copyLast(to pasteboard: NSPasteboard) -> Bool {
        guard let dir = lastFolder else {
            Toast.shared.show("No bug report yet", "Start one with \(hotkeyText).")
            return false
        }
        let fm = FileManager.default
        var files = ["bug.png", "clip.mp4", "bug.md"].map { dir.appendingPathComponent($0) }.filter { fm.fileExists(atPath: $0.path) }
        let logs = dir.appendingPathComponent("logs")
        files += ((try? fm.contentsOfDirectory(atPath: logs.path)) ?? []).sorted().prefix(10).map { logs.appendingPathComponent($0) }
        pasteboard.clearContents()
        guard pasteboard.writeObjects(files as [NSURL]) else {
            Toast.shared.show("Couldn't copy the bug report")
            return false
        }
        Toast.shared.show("Bug report copied", "\(files.count) files: paste them into Slack or your tracker.")
        return true
    }

    func openFolder() {
        let dir = options.folder.appendingPathComponent("Bugs", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        showFolder(dir)
    }

    // Quitting: stops listening (the microphone).
    func stop() {
        speech?.stop()
        speech = nil
        listening = false
        timer?.invalidate()
        timer = nil
    }
}
