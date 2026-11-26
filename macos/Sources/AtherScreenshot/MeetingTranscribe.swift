import AVFoundation
import Foundation
import Speech
#if canImport(FoundationModels)
import FoundationModels
#endif

// Speech to words, on this Mac. macOS 26 and later: SpeechTranscriber (long-form, made for this). Earlier: on-device
// SFSpeechRecognizer, in one-minute pieces. Nothing is ever sent off the Mac.
enum MeetingSpeech {
    struct Result {
        var words: [TimedWord]
        var engine: String
        var locale: String
    }

    static func locale(_ setting: String) -> Locale { setting.isEmpty ? Locale.current : Locale(identifier: setting) }

    static func transcribe(_ url: URL, language: String) async throws -> Result {
        let loc = locale(language)
        if #available(macOS 26, *), SpeechTranscriber.isAvailable {
            return try await analyzer(url, locale: loc)
        }
        return try await legacy(url, locale: loc)
    }

    // MARK: macOS 26+: SpeechAnalyzer

    @available(macOS 26, *)
    static func analyzer(_ url: URL, locale requested: Locale) async throws -> Result {
        guard let loc = await SpeechTranscriber.supportedLocale(equivalentTo: requested) else {
            throw MeetingEdit.Failure.failed("This Mac can't transcribe \(requested.localizedString(forIdentifier: requested.identifier) ?? requested.identifier). Pick another language in Settings › Meetings.")
        }
        let t = SpeechTranscriber(locale: loc, transcriptionOptions: [], reportingOptions: [], attributeOptions: [.audioTimeRange])
        // The language model is downloaded by macOS the first time (once per language).
        if let req = try await AssetInventory.assetInstallationRequest(supporting: [t]) { try await req.downloadAndInstall() }
        let analyzer = SpeechAnalyzer(modules: [t])
        let file = try AVAudioFile(forReading: url)
        let collect = Task { () throws -> [TimedWord] in
            var out: [TimedWord] = []
            for try await r in t.results { out += words(r) }
            return out
        }
        do {
            if let last = try await analyzer.analyzeSequence(from: file) {
                try await analyzer.finalizeAndFinish(through: last)
            } else {
                await analyzer.cancelAndFinishNow()
            }
        } catch {
            collect.cancel()
            throw error
        }
        return Result(words: try await collect.value, engine: "Apple speech recognition on this Mac", locale: loc.identifier)
    }

    @available(macOS 26, *)
    static func words(_ r: SpeechTranscriber.Result) -> [TimedWord] {
        var out: [TimedWord] = []
        for run in r.text.runs {
            let s = String(r.text[run.range].characters)
            let range = run.audioTimeRange
            out += split(s, start: range.map { CMTimeGetSeconds($0.start) }, end: range.map { CMTimeGetSeconds($0.end) }, after: &out)
        }
        // A result without word times: one piece over its whole range.
        if out.isEmpty {
            let text = String(r.text.characters).trimmingCharacters(in: .whitespacesAndNewlines)
            if !text.isEmpty { out.append(TimedWord(start: CMTimeGetSeconds(r.range.start), end: CMTimeGetSeconds(r.range.end), text: text)) }
        }
        return out
    }

    // A run of text with a time span becomes words, the span shared by length; punctuation on its own joins the word
    // before. Text without a time joins the last word.
    static func split(_ s: String, start: Double?, end: Double?, after prior: inout [TimedWord]) -> [TimedWord] {
        let tokens = s.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        guard !tokens.isEmpty else { return [] }
        var out: [TimedWord] = []
        guard let start, let end, end >= start else {
            if !prior.isEmpty { prior[prior.count - 1].text += (tokens.first.map(isPunctuation) == true ? "" : " ") + tokens.joined(separator: " ") }
            return []
        }
        let total = Double(tokens.reduce(0) { $0 + max(1, $1.count) })
        var t = start
        for tok in tokens {
            let d = (end - start) * Double(max(1, tok.count)) / total
            if isPunctuation(tok) {
                if !out.isEmpty { out[out.count - 1].text += tok } else if !prior.isEmpty { prior[prior.count - 1].text += tok }
            } else {
                out.append(TimedWord(start: t, end: t + d, text: tok))
            }
            t += d
        }
        return out
    }

    static func isPunctuation(_ s: String) -> Bool { !s.isEmpty && s.allSatisfy { $0.isPunctuation } }

    // MARK: macOS 14–15: SFSpeechRecognizer on the device, one minute at a time

    static func legacy(_ url: URL, locale: Locale) async throws -> Result {
        let status = await withCheckedContinuation { k in SFSpeechRecognizer.requestAuthorization { k.resume(returning: $0) } }
        guard status == .authorized else {
            throw MeetingEdit.Failure.failed("Allow Ather Screenshot in System Settings › Privacy & Security › Speech Recognition.")
        }
        guard let rec = SFSpeechRecognizer(locale: locale), rec.isAvailable else {
            throw MeetingEdit.Failure.failed("Speech recognition isn't available for \(locale.identifier).")
        }
        guard rec.supportsOnDeviceRecognition else {
            throw MeetingEdit.Failure.failed("This Mac can't transcribe \(locale.identifier) on the device, and meetings are never sent to a server.")
        }
        let asset = AVURLAsset(url: url)
        let duration = CMTimeGetSeconds(try await asset.load(.duration))
        var words: [TimedWord] = []
        var failures = 0, pieces = 0
        var t = 0.0
        while t < duration {
            let len = min(55, duration - t)
            defer { t += len }
            guard len > 0.5 else { continue }
            pieces += 1
            let piece = FileManager.default.temporaryDirectory.appendingPathComponent("ather-meet-\(UUID().uuidString).m4a")
            defer { try? FileManager.default.removeItem(at: piece) }
            guard let s = AVAssetExportSession(asset: asset, presetName: AVAssetExportPresetAppleM4A) else { continue }
            s.outputURL = piece
            s.outputFileType = .m4a
            s.timeRange = CMTimeRange(start: CMTime(seconds: t, preferredTimescale: 600), duration: CMTime(seconds: len, preferredTimescale: 600))
            await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in s.exportAsynchronously { k.resume() } }
            guard s.status == .completed else { failures += 1; continue }
            let req = SFSpeechURLRecognitionRequest(url: piece)
            req.requiresOnDeviceRecognition = true
            req.shouldReportPartialResults = false
            req.addsPunctuation = true
            do {
                let got = try await BugSpeech.words(rec, req)
                words += got.map { TimedWord(start: $0.start + t, end: $0.end + t, text: $0.text) }
            } catch {
                failures += 1   // a silent minute is an error too ("no speech detected")
            }
        }
        if words.isEmpty && failures == pieces && pieces > 3 { throw MeetingEdit.Failure.failed("Speech recognition failed on every part of the recording.") }
        return Result(words: words, engine: "Apple speech recognition on this Mac", locale: locale.identifier)
    }
}

// MARK: - Notes and outputs from the on-device Apple Intelligence model (macOS 26+)

enum MeetingAI {
    static let outputKinds = ["Follow-up email", "Show notes", "Key quotes", "Highlights"]

    // nil when the model can be used, else why not.
    static var unavailableReason: String? {
        #if canImport(FoundationModels)
        if #available(macOS 26, *) {
            switch SystemLanguageModel.default.availability {
            case .available: return nil
            case .unavailable(let r):
                switch r {
                case .deviceNotEligible: return "This Mac doesn't support Apple Intelligence."
                case .appleIntelligenceNotEnabled: return "Turn on Apple Intelligence in System Settings to summarize meetings."
                case .modelNotReady: return "Apple Intelligence is still getting ready. Try again in a while."
                @unknown default: return "Apple Intelligence isn't available."
                }
            }
        }
        #endif
        return "Summaries need macOS 26 or later with Apple Intelligence."
    }
    static var available: Bool { unavailableReason == nil }

    // The edited transcript as plain lines for the model, in pieces of about `limit` characters.
    static func transcriptPieces(_ m: Meeting, limit: Int = 6000) -> [String] {
        var pieces: [String] = []
        var cur = ""
        for l in MeetingEdit.editedLines(m) {
            let line = "[\(MeetingFormat.clock(l.start))] \(m.name(l.speaker)): \(l.text)\n"
            if cur.count + line.count > limit, !cur.isEmpty { pieces.append(cur); cur = "" }
            cur += line
        }
        if !cur.isEmpty { pieces.append(cur) }
        return pieces
    }

    static let format = """
    Answer in exactly this format, in the language of the transcript, with nothing before or after:
    SUMMARY:
    <two to four sentences>
    KEY POINTS:
    - <point>
    DECISIONS:
    - <decision, or "- None">
    ACTION ITEMS:
    - <who: what, or "- None">
    """

    static func parse(_ text: String) -> MeetingSummary {
        var s = MeetingSummary()
        var section = ""
        var overview: [String] = []
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            let upper = line.uppercased().replacingOccurrences(of: "*", with: "").replacingOccurrences(of: "#", with: "").trimmingCharacters(in: .whitespaces)
            if upper.hasPrefix("SUMMARY") { section = "s"; let rest = line.split(separator: ":", maxSplits: 1).dropFirst().joined(); if !rest.trimmingCharacters(in: .whitespaces).isEmpty { overview.append(rest.trimmingCharacters(in: .whitespaces)) }; continue }
            if upper.hasPrefix("KEY POINTS") { section = "k"; continue }
            if upper.hasPrefix("DECISIONS") { section = "d"; continue }
            if upper.hasPrefix("ACTION ITEMS") { section = "a"; continue }
            guard !line.isEmpty else { continue }
            var item = line
            for p in ["- [ ] ", "- ", "• ", "* ", "– "] where item.hasPrefix(p) { item = String(item.dropFirst(p.count)); break }
            item = item.trimmingCharacters(in: .whitespaces)
            let none = ["none", "none.", "n/a", "không có", "không"].contains(item.lowercased())
            switch section {
            case "s": overview.append(line)
            case "k": if !none { s.keyPoints.append(item) }
            case "d": if !none { s.decisions.append(item) }
            case "a": if !none { s.actionItems.append(item) }
            default: overview.append(line)
            }
        }
        s.overview = overview.joined(separator: " ")
        return s
    }

    static func summarize(_ m: Meeting) async throws -> MeetingSummary {
        let pieces = transcriptPieces(m)
        guard !pieces.isEmpty else { throw MeetingEdit.Failure.failed("There's no transcript to summarize.") }
        if pieces.count == 1 {
            return parse(try await ask("Here is the transcript of a meeting titled “\(m.title)”. \"You\" is the person who recorded it.\n\n\(pieces[0])\n\n\(format)"))
        }
        // Long meetings: notes for each part, then one summary of the notes.
        var notes: [String] = []
        for (i, p) in pieces.enumerated() {
            notes.append(try await ask("This is part \(i + 1) of \(pieces.count) of a meeting transcript. Write short notes of what was said: the topics, any decisions and any tasks someone took on (with who).\n\n\(p)"))
        }
        var joined = notes.joined(separator: "\n\n")
        if joined.count > 7000 { joined = String(joined.prefix(7000)) }
        return parse(try await ask("These are notes from the parts of one meeting titled “\(m.title)”, in order.\n\n\(joined)\n\n\(format)"))
    }

    static func create(_ kind: String, from m: Meeting) async throws -> String {
        var context = ""
        if let s = m.summary, !s.isEmpty {
            context = "Summary: \(s.overview)\nKey points:\n" + s.keyPoints.map { "- \($0)" }.joined(separator: "\n")
                + "\nDecisions:\n" + s.decisions.map { "- \($0)" }.joined(separator: "\n") + "\nAction items:\n" + s.actionItems.map { "- \($0)" }.joined(separator: "\n")
        }
        let pieces = transcriptPieces(m, limit: context.isEmpty ? 7000 : 4500)
        let transcript = pieces.first ?? ""
        let more = pieces.count > 1 ? "\n(The transcript continues; use the summary for the rest.)" : ""
        let task: String
        switch kind {
        case "Follow-up email": task = "Write a short, friendly follow-up email to the meeting's participants: a thank-you line, what was decided, the action items with owners, and next steps. Plain text, with a subject line first."
        case "Show notes": task = "Write podcast-style show notes: a one-paragraph description, then the topics covered as a list with their [m:ss] times from the transcript."
        case "Key quotes": task = "Pick the 3 to 6 most important or memorable things said, quoted word for word from the transcript, each with its speaker and [m:ss] time."
        default: task = "List the highlights: the 3 to 6 moments most worth listening to again, each with its [m:ss] time from the transcript and one line on why."
        }
        return try await ask("Meeting: “\(m.title)”.\n\(context)\n\nTranscript:\n\(transcript)\(more)\n\n\(task) Use the language of the transcript.")
    }

    static func ask(_ prompt: String) async throws -> String {
        #if canImport(FoundationModels)
        if #available(macOS 26, *) {
            if let why = unavailableReason { throw MeetingEdit.Failure.failed(why) }
            let session = LanguageModelSession(instructions: "You write accurate, concise meeting notes. Only use what is in the transcript; never invent names, numbers or tasks.")
            return try await session.respond(to: prompt).content
        }
        #endif
        throw MeetingEdit.Failure.failed(unavailableReason ?? "Apple Intelligence isn't available.")
    }
}

// MARK: - After a recording: transcribe each track, merge, then summarize

final class MeetingPipeline {
    static let shared = MeetingPipeline()
    private var running: Set<UUID> = []

    func process(_ id: UUID, summarize: Bool? = nil) {
        guard !running.contains(id), let m = MeetingStore.shared.meeting(id) else { return }
        running.insert(id)
        MeetingStore.shared.busy.insert(id)
        MeetingStore.shared.update(id) { $0.transcript = .running; $0.transcriptError = nil }
        let language = Settings.shared.string("MeetingLanguage")
        let wantSummary = summarize ?? Settings.shared.bool("MeetingSummarize")
        Task {
            var me: [TimedWord] = [], others: [TimedWord] = []
            var engine = "", locale = "", failure: String?
            do {
                for s in m.tracks {
                    let url = FileManager.default.fileExists(atPath: m.trackURL(s).path) ? m.trackURL(s) : m.audioURL
                    let r = try await MeetingSpeech.transcribe(url, language: language)
                    if s == .me { me = r.words } else { others = r.words }
                    engine = r.engine
                    locale = r.locale
                }
            } catch {
                failure = error.localizedDescription
            }
            let lines = MeetingTranscript.merge(me: me, others: others)
            await MainActor.run {
                MeetingStore.shared.update(id) {
                    if let failure {
                        $0.transcript = .failed
                        $0.transcriptError = failure
                    } else {
                        $0.transcript = .done
                        $0.lines = lines
                        $0.transcriptEngine = engine
                        $0.language = locale
                        $0.cuts.removeAll { $0.reason != .manual && $0.reason != .trim }  // clean-up is redone on the new words
                    }
                }
            }
            if failure == nil, wantSummary, !lines.isEmpty, MeetingAI.available { await summarizeNow(id) }
            await MainActor.run {
                self.running.remove(id)
                MeetingStore.shared.busy.remove(id)
                if let failure { Toast.shared.show("Couldn't transcribe the meeting", failure, ms: 7000) { MeetingsWindow.show(select: id) } }
            }
        }
    }

    @MainActor
    func summarizeNow(_ id: UUID) async {
        guard let m = MeetingStore.shared.meeting(id) else { return }
        MeetingStore.shared.busy.insert(id)
        defer { if !running.contains(id) { MeetingStore.shared.busy.remove(id) } }
        do {
            let s = try await MeetingAI.summarize(m)
            MeetingStore.shared.update(id) { $0.summary = s; $0.summaryError = nil }
        } catch {
            MeetingStore.shared.update(id) { $0.summaryError = error.localizedDescription }
        }
    }

    @MainActor
    func createOutput(_ kind: String, for id: UUID) async -> String? {
        guard let m = MeetingStore.shared.meeting(id) else { return "That meeting is gone." }
        MeetingStore.shared.busy.insert(id)
        defer { MeetingStore.shared.busy.remove(id) }
        do {
            let text = try await MeetingAI.create(kind, from: m)
            MeetingStore.shared.update(id) { mm in
                mm.outputs.removeAll { $0.kind == kind }
                mm.outputs.append(MeetingOutput(kind: kind, text: text))
            }
            return nil
        } catch {
            return error.localizedDescription
        }
    }
}
