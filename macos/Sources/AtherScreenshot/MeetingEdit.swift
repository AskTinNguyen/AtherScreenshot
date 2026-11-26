import AVFoundation
import AudioToolbox

// Editing a meeting by its text (Descript-style, simpler). Every edit is a cut: a stretch of the recording left out.
// Cuts never change the files; playback skips them, and "Export edited audio" joins what's left with short fades.
// Times are seconds of the recording ("original"); the edit has its own, shorter timeline ("edited").

enum MeetingEdit {
    typealias Span = (start: Double, end: Double)

    // MARK: cut arithmetic

    // The cuts as sorted, non-overlapping spans.
    static func merged(_ cuts: [MeetingCut]) -> [Span] {
        var out: [Span] = []
        for c in cuts.filter({ $0.end > $0.start }).sorted(by: { $0.start < $1.start }) {
            // A kept sliver under 30 ms between two cuts would overlap its fades: the cuts become one.
            if let last = out.last, c.start <= last.end + 0.03 { out[out.count - 1].end = max(last.end, c.end) } else { out.append((c.start, c.end)) }
        }
        return out
    }

    static func isCut(_ t: Double, _ spans: [Span]) -> Bool { spans.contains { t >= $0.start && t < $0.end } }

    // A word is cut when its middle is.
    static func isCut(_ w: TimedWord, _ spans: [Span]) -> Bool { isCut((w.start + w.end) / 2, spans) }

    // The end of the cut containing `t`, or nil (playback jumps there).
    static func cutEnd(at t: Double, _ spans: [Span]) -> Double? { spans.first { t >= $0.start && t < $0.end - 0.005 }?.end }

    static func kept(duration: Double, _ spans: [Span]) -> [Span] {
        var out: [Span] = []
        var t = 0.0
        for s in spans where s.end > 0 && s.start < duration {
            if s.start > t { out.append((t, min(s.start, duration))) }
            t = max(t, s.end)
        }
        if t < duration { out.append((t, duration)) }
        return out.filter { $0.end - $0.start > 0.001 }
    }

    // Where an original time lands in the edit (a time inside a cut lands where the cut was).
    static func editedTime(_ t: Double, _ spans: [Span]) -> Double {
        var removed = 0.0
        for s in spans {
            if t >= s.end { removed += s.end - s.start } else if t > s.start { removed += t - s.start } else { break }
        }
        return t - removed
    }

    static func editedDuration(_ m: Meeting) -> Double { editedTime(m.duration, merged(m.cuts)) }
    static func savedSeconds(_ m: Meeting) -> Double { max(0, m.duration - editedDuration(m)) }

    // Adds a cut (clamped to the recording). Returns false for an empty one.
    @discardableResult
    static func cut(_ m: inout Meeting, _ start: Double, _ end: Double, reason: MeetingCut.Reason = .manual) -> Bool {
        let s = max(0, start), e = min(m.duration > 0 ? m.duration : end, end)
        guard e - s > 0.01 else { return false }
        m.cuts.append(MeetingCut(start: s, end: e, reason: reason))
        return true
    }

    // Takes a span out of every cut (splitting cuts that cross it), so that audio plays again.
    static func restore(_ m: inout Meeting, _ start: Double, _ end: Double) {
        var out: [MeetingCut] = []
        for c in m.cuts {
            if c.end <= start || c.start >= end { out.append(c); continue }
            if c.start < start { var a = c; a.id = UUID(); a.end = start; out.append(a) }
            if c.end > end { var b = c; b.id = UUID(); b.start = end; out.append(b) }
        }
        m.cuts = out
    }

    static func removeCuts(_ m: inout Meeting, reason: MeetingCut.Reason) { m.cuts.removeAll { $0.reason == reason } }

    // MARK: the edited transcript

    // Lines without their cut words, on the edited timeline. Lines with every word cut are gone.
    static func editedLines(_ m: Meeting) -> [MeetingLine] {
        let spans = merged(m.cuts)
        guard !spans.isEmpty else { return m.lines }
        return m.lines.compactMap { l in
            let words = l.words.isEmpty ? [TimedWord(start: l.start, end: l.end, text: l.text)] : l.words
            let left = words.filter { !isCut($0, spans) }
            guard let first = left.first, let last = left.last else { return nil }
            let mapped = left.map { TimedWord(start: editedTime($0.start, spans), end: editedTime($0.end, spans), text: $0.text) }
            return MeetingLine(id: l.id, start: editedTime(first.start, spans), end: editedTime(last.end, spans), speaker: l.speaker,
                               text: join(left.map(\.text)), words: mapped)
        }
    }

    static func join(_ words: [String]) -> String {
        var s = ""
        for w in words {
            if !s.isEmpty, !(w.first.map { ",.;:!?…)'’".contains($0) } ?? false) { s += " " }
            s += w
        }
        return s
    }

    // MARK: clean-up

    static let fillers: Set<String> = ["um", "umm", "ummm", "uh", "uhh", "uhm", "erm", "er", "err", "ah", "ahh", "hmm", "hm", "mm", "mmm", "mhm", "eh"]

    static func bare(_ w: String) -> String { w.lowercased().filter { $0.isLetter || $0.isNumber || $0 == "'" } }

    // Cuts for each filler word: from its start to its end, plus the comma or pause that comes with it, up to the next word.
    static func fillerCuts(_ m: Meeting) -> [MeetingCut] {
        var out: [MeetingCut] = []
        for l in m.lines {
            for (i, w) in l.words.enumerated() where fillers.contains(bare(w.text)) {
                let next = i + 1 < l.words.count ? l.words[i + 1].start : w.end + 0.15
                out.append(MeetingCut(start: w.start, end: max(w.end, min(next, w.end + 0.25)), reason: .filler))
            }
        }
        return out
    }

    // "the the", "I I think": the first of two same words in a row (a stammer) is cut, from its start to the second's.
    static func repeatCuts(_ m: Meeting) -> [MeetingCut] {
        var out: [MeetingCut] = []
        for l in m.lines where l.words.count > 1 {
            for i in 0..<(l.words.count - 1) {
                let a = l.words[i], b = l.words[i + 1]
                let w = bare(a.text)
                guard !w.isEmpty, w == bare(b.text), b.start - a.end < 0.8, !fillers.contains(w) else { continue }
                out.append(MeetingCut(start: a.start, end: b.start, reason: .repeated))
            }
        }
        return out
    }

    // Silences longer than `minPause`, found in the loudness of the audio (`rms`, one value per `window` seconds),
    // become `keep` seconds long: the middle is cut, half of `keep` stays on each side.
    static func pauseCuts(rms: [Float], window: Double, minPause: Double = 1.0, keep: Double = 0.4, threshold: Float? = nil) -> [MeetingCut] {
        let th = threshold ?? silenceThreshold(rms)
        var out: [MeetingCut] = []
        var runStart: Int?
        func close(_ end: Int) {
            guard let s = runStart else { return }
            let a = Double(s) * window, b = Double(end) * window
            if b - a >= minPause { out.append(MeetingCut(start: a + keep / 2, end: b - keep / 2, reason: .pause)) }
            runStart = nil
        }
        for (i, v) in rms.enumerated() {
            if v < th { if runStart == nil { runStart = i } } else { close(i) }
        }
        close(rms.count)
        return out
    }

    // Quiet is relative: the room's noise floor (the quietest tenth) times 3, kept between -50 and -30 dBFS.
    static func silenceThreshold(_ rms: [Float]) -> Float {
        guard !rms.isEmpty else { return 0.01 }
        let sorted = rms.sorted()
        let floor = sorted[min(sorted.count - 1, sorted.count / 10)]
        return min(0.0316, max(0.00316, floor * 3))
    }

    // Loudness per `window` seconds of a file (mono, 8 kHz), for finding pauses.
    static func rms(_ url: URL, window: Double = 0.02) -> [Float] {
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
        let per = max(1, Int(rate * window))
        var result: [Float] = []
        var acc: Float = 0
        var n = 0
        while let sb = out.copyNextSampleBuffer() {
            guard let block = CMSampleBufferGetDataBuffer(sb) else { continue }
            var len = 0
            var ptr: UnsafeMutablePointer<CChar>?
            guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr) == noErr, let ptr else { continue }
            ptr.withMemoryRebound(to: Float.self, capacity: len / 4) { f in
                for i in 0..<(len / 4) {
                    acc += f[i] * f[i]
                    n += 1
                    if n == per { result.append(sqrt(acc / Float(n))); acc = 0; n = 0 }
                }
            }
        }
        if n > per / 2 { result.append(sqrt(acc / Float(n))) }
        return result
    }

    struct CleanupResult { var fillers = 0, repeats = 0, pauses = 0; var saved = 0.0 }

    // Adds the clean-up cuts asked for (replacing earlier ones of the same kind) and says what they took out.
    static func cleanUp(_ m: inout Meeting, fillers doFillers: Bool, repeats doRepeats: Bool, pauses doPauses: Bool,
                        minPause: Double = 1.0, keep: Double = 0.4, rms: [Float]? = nil) -> CleanupResult {
        let before = editedDuration(m)
        var r = CleanupResult()
        if doFillers {
            removeCuts(&m, reason: .filler)
            let c = fillerCuts(m); r.fillers = c.count; m.cuts += c
        }
        if doRepeats {
            removeCuts(&m, reason: .repeated)
            let c = repeatCuts(m); r.repeats = c.count; m.cuts += c
        }
        if doPauses {
            removeCuts(&m, reason: .pause)
            let levels = rms ?? MeetingEdit.rms(m.playableURL)
            let c = pauseCuts(rms: levels, window: 0.02, minPause: minPause, keep: keep).filter { $0.end <= m.duration + 0.05 }
            r.pauses = c.count; m.cuts += c
        }
        r.saved = max(0, before - editedDuration(m))
        return r
    }

    // MARK: export the edited audio

    enum Failure: LocalizedError {
        case failed(String)
        var errorDescription: String? { if case .failed(let s) = self { return s }; return nil }
    }

    // The kept spans of `source`, joined, with 10 ms fades at each join so cuts don't click. `enhance` adds a rumble
    // filter, gentle compression and evens out the loudness. Writes AAC (.m4a).
    static func exportAudio(source: URL, duration: Double, cuts: [MeetingCut], enhance: Bool, to out: URL) async throws {
        let asset = AVURLAsset(url: source)
        guard let src = try await asset.loadTracks(withMediaType: .audio).first else { throw Failure.failed("The recording has no sound.") }
        let fileDuration = CMTimeGetSeconds(try await asset.load(.duration))
        let total = min(duration > 0 ? duration : fileDuration, fileDuration)
        let comp = AVMutableComposition()
        guard let track = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) else { throw Failure.failed("Can't edit the audio.") }
        let ts: CMTimeScale = 48000
        var at = CMTime.zero
        var joins: [CMTime] = []
        for k in kept(duration: total, merged(cuts)) {
            let range = CMTimeRange(start: CMTime(seconds: k.start, preferredTimescale: ts), end: CMTime(seconds: k.end, preferredTimescale: ts))
            try track.insertTimeRange(range, of: src, at: at)
            at = CMTimeAdd(at, range.duration)
            joins.append(at)
        }
        guard at > .zero else { throw Failure.failed("Everything is cut: nothing is left to export.") }
        joins.removeLast()
        let params = AVMutableAudioMixInputParameters(track: track)
        let fade = CMTime(value: 480, timescale: ts)  // 10 ms
        for j in joins where j > fade {
            params.setVolumeRamp(fromStartVolume: 1, toEndVolume: 0, timeRange: CMTimeRange(start: CMTimeSubtract(j, fade), duration: fade))
            params.setVolumeRamp(fromStartVolume: 0, toEndVolume: 1, timeRange: CMTimeRange(start: j, duration: fade))
        }
        let mix = AVMutableAudioMix()
        mix.inputParameters = [params]

        let fm = FileManager.default
        let joined = enhance ? fm.temporaryDirectory.appendingPathComponent("ather-edit-\(UUID().uuidString).m4a") : out
        try? fm.removeItem(at: joined)
        defer { if enhance { try? fm.removeItem(at: joined) } }
        guard let s = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetAppleM4A) else { throw Failure.failed("Can't export the audio.") }
        s.outputURL = joined
        s.outputFileType = .m4a
        s.audioMix = mix
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in s.exportAsynchronously { k.resume() } }
        guard s.status == .completed else { throw Failure.failed(s.error?.localizedDescription ?? "The export failed.") }
        if enhance { try Enhance.run(joined, to: out) }
    }
}

// MARK: - Enhance: rumble filter → compressor → even loudness (offline, on the Mac)

enum Enhance {
    static func run(_ input: URL, to output: URL) throws {
        let file = try AVAudioFile(forReading: input)
        let format = file.processingFormat
        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        let eq = AVAudioUnitEQ(numberOfBands: 2)
        eq.bands[0].filterType = .highPass       // rumble, desk knocks, hum below speech
        eq.bands[0].frequency = 80
        eq.bands[0].bypass = false
        eq.bands[1].filterType = .parametric     // a little presence, so voices read clearly
        eq.bands[1].frequency = 3000
        eq.bands[1].bandwidth = 1.5
        eq.bands[1].gain = 2
        eq.bands[1].bypass = false
        let comp = AVAudioUnitEffect(audioComponentDescription: AudioComponentDescription(
            componentType: kAudioUnitType_Effect, componentSubType: kAudioUnitSubType_DynamicsProcessor,
            componentManufacturer: kAudioUnitManufacturer_Apple, componentFlags: 0, componentFlagsMask: 0))
        let au = comp.audioUnit
        AudioUnitSetParameter(au, kDynamicsProcessorParam_Threshold, kAudioUnitScope_Global, 0, -26, 0)
        AudioUnitSetParameter(au, kDynamicsProcessorParam_HeadRoom, kAudioUnitScope_Global, 0, 8, 0)
        AudioUnitSetParameter(au, kDynamicsProcessorParam_AttackTime, kAudioUnitScope_Global, 0, 0.005, 0)
        AudioUnitSetParameter(au, kDynamicsProcessorParam_ReleaseTime, kAudioUnitScope_Global, 0, 0.12, 0)
        engine.attach(player)
        engine.attach(eq)
        engine.attach(comp)
        engine.connect(player, to: eq, format: format)
        engine.connect(eq, to: comp, format: format)
        engine.connect(comp, to: engine.mainMixerNode, format: format)
        try engine.enableManualRenderingMode(.offline, format: format, maximumFrameCount: 4096)
        try engine.start()
        player.scheduleFile(file, at: nil)
        player.play()

        // Pass 1: render into memory-backed temp file (float), measuring loudness and peak.
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("ather-enh-\(UUID().uuidString).caf")
        defer { try? FileManager.default.removeItem(at: tmp) }
        let mid = try AVAudioFile(forWriting: tmp, settings: format.settings, commonFormat: .pcmFormatFloat32, interleaved: false)
        guard let buf = AVAudioPCMBuffer(pcmFormat: engine.manualRenderingFormat, frameCapacity: engine.manualRenderingMaximumFrameCount) else {
            throw MeetingEdit.Failure.failed("Can't enhance the audio.")
        }
        var peak: Float = 0
        var sumSq: Double = 0
        var count = 0
        let total = file.length
        while engine.manualRenderingSampleTime < total {
            let frames = AVAudioFrameCount(min(Int64(buf.frameCapacity), total - engine.manualRenderingSampleTime))
            let status = try engine.renderOffline(frames, to: buf)
            guard status == .success else { if status == .error { throw MeetingEdit.Failure.failed("Enhancing the audio failed.") }; continue }
            for ch in 0..<Int(buf.format.channelCount) {
                guard let d = buf.floatChannelData?[ch] else { continue }
                for i in 0..<Int(buf.frameLength) {
                    let v = d[i]
                    peak = max(peak, abs(v))
                    if abs(v) > 0.003 { sumSq += Double(v * v); count += 1 }  // speech, not the silence between
                }
            }
            try mid.write(from: buf)
        }
        player.stop()
        engine.stop()

        // Pass 2: one gain for the whole file: speech at about -18 dBFS RMS, peaks never above -1 dBFS.
        let rms = count > 0 ? Float(sqrt(sumSq / Double(count))) : 0
        let gain = rms > 0 ? min(0.126 / rms, 0.89 / max(peak, 0.0001), 8) : 1
        let reader = try AVAudioFile(forReading: tmp)
        try? FileManager.default.removeItem(at: output)
        let writer = try AVAudioFile(forWriting: output, settings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: format.sampleRate, AVNumberOfChannelsKey: format.channelCount,
            AVEncoderBitRateKey: format.channelCount > 1 ? 160_000 : 96_000,
        ], commonFormat: .pcmFormatFloat32, interleaved: false)
        guard let b2 = AVAudioPCMBuffer(pcmFormat: reader.processingFormat, frameCapacity: 8192) else { throw MeetingEdit.Failure.failed("Can't enhance the audio.") }
        while reader.framePosition < reader.length {
            try reader.read(into: b2)
            for ch in 0..<Int(b2.format.channelCount) {
                guard let d = b2.floatChannelData?[ch] else { continue }
                for i in 0..<Int(b2.frameLength) { d[i] *= gain }
            }
            try writer.write(from: b2)
        }
    }
}

// MARK: - For agents: transcript.json

enum MeetingAgentExport {
    static func json(_ m: Meeting) -> [String: Any] {
        let spans = MeetingEdit.merged(m.cuts)
        func r(_ v: Double) -> Double { (v * 1000).rounded() / 1000 }
        let lines: [[String: Any]] = m.lines.map { l in
            let words = l.words.isEmpty ? [TimedWord(start: l.start, end: l.end, text: l.text)] : l.words
            let kept = words.filter { !MeetingEdit.isCut($0, spans) }
            return [
                "id": l.id.uuidString, "speaker": m.name(l.speaker), "source": l.speaker == .me ? "microphone" : "system_audio",
                "start": r(l.start), "end": r(l.end), "text": l.text,
                "edited_text": MeetingEdit.join(kept.map(\.text)), "cut": kept.isEmpty,
                "edited_start": r(MeetingEdit.editedTime(l.start, spans)),
                "words": words.map { ["t": r($0.start), "e": r($0.end), "w": $0.text, "cut": MeetingEdit.isCut($0, spans)] as [String: Any] },
            ]
        }
        var d: [String: Any] = [
            "app": "Ather Screenshot", "format": 1, "title": m.title, "created": ISO8601DateFormatter().string(from: m.created),
            "duration": r(m.duration), "edited_duration": r(MeetingEdit.editedDuration(m)), "audio": "meeting.m4a",
            "speakers": m.tracks.map { ["name": m.name($0), "source": $0 == .me ? "microphone" : "system_audio"] },
            "markers": m.markers.sorted { $0.time < $1.time }.map { ["time": r($0.time), "label": $0.label] as [String: Any] },
            "cuts": spans.map { ["start": r($0.start), "end": r($0.end)] },
            "lines": lines, "notes": m.notes, "transcript": m.transcript.rawValue,
            "outputs": m.outputs.map { ["kind": $0.kind, "text": $0.text] },
        ]
        if let lang = m.language { d["language"] = lang }
        if let s = m.summary, !s.isEmpty {
            d["summary"] = ["overview": s.overview, "key_points": s.keyPoints, "decisions": s.decisions, "action_items": s.actionItems]
        }
        return d
    }

    static func data(_ m: Meeting) -> Data? {
        try? JSONSerialization.data(withJSONObject: json(m), options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
    }
}
