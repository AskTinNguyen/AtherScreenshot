import AVFoundation
import AppKit
import ScreenCaptureKit

// Records a meeting's sound: everyone else (system audio, through ScreenCaptureKit, optionally from one app only) and
// you (the microphone, through AVCaptureSession, so any input works on macOS 14). Each source goes to its own AAC
// track in a hidden ".recording-<id>" folder; both start at the same host time, so they line up. At Stop the two are
// mixed into meeting.m4a, the folder gets its real name, meeting.json is written and the transcript starts.
final class MeetingRecorder: NSObject, SCStreamOutput, SCStreamDelegate, AVCaptureAudioDataOutputSampleBufferDelegate {
    static var current: MeetingRecorder?
    static var isActive: Bool { current != nil }
    static var onFinished: [() -> Void] = []   // once it's fully over (used to quit cleanly)

    struct Options {
        var others = true          // system audio
        var you = true             // microphone
        var appBundleID = ""       // "" = every app's sound
        var micID = ""             // "" = the default input
        var title = ""
        var transcribe = true
        static func fromSettings() -> Options {
            let s = Settings.shared
            return Options(others: s.bool("MeetingSystemAudio"), you: s.bool("MeetingMicrophone"), appBundleID: s.string("MeetingApp"),
                           micID: s.string("MeetingMicDevice"), title: "", transcribe: s.bool("MeetingTranscribe"))
        }
    }

    struct Outcome {
        var meeting: Meeting?
        var error: String?
    }

    let options: Options
    private(set) var meeting = Meeting()
    var onFinish: ((Outcome) -> Void)?
    var whenFinished: [(Outcome) -> Void] = []
    private(set) var phase = Phase.starting
    enum Phase { case starting, recording, finishing }
    var hasAudio: Bool { phase == .recording }
    private(set) var notices: [String] = []   // e.g. "No microphone access": shown on the bar and in the toast

    private let queue = DispatchQueue(label: "ather.meeting")
    private var stream: SCStream?
    private var session: AVCaptureSession?
    private var hud: MeetingHUD?
    private let tmpFolder: URL
    private var cancelWhileStarting = false

    // Touched only on `queue`.
    private var others: TrackWriter?
    private var you: TrackWriter?
    private var origin = CMTime.invalid       // host time of recording time 0
    private var paused = false
    private var pauseStart = CMTime.invalid
    private var pauseOffset = CMTime.zero
    private var resumeAt = CMTime.invalid
    private var finished = false

    // Levels for the bar's meters (0…1), written on `queue`, read on main.
    private let levelLock = NSLock()
    private var _levels: (others: Float, you: Float) = (0, 0)
    var levels: (others: Float, you: Float) { levelLock.lock(); defer { levelLock.unlock() }; return _levels }

    private var startDate = Date()
    private var pausedTotal: TimeInterval = 0
    private var pausedAt: Date?
    var isPaused: Bool { pausedAt != nil }
    var elapsed: TimeInterval {
        guard phase != .starting else { return 0 }
        return Date().timeIntervalSince(startDate) - pausedTotal - (pausedAt.map { Date().timeIntervalSince($0) } ?? 0)
    }

    @discardableResult
    static func start(_ o: Options = .fromSettings(), configure: ((MeetingRecorder) -> Void)? = nil) -> MeetingRecorder? {
        guard current == nil else { return nil }
        guard o.others || o.you else {
            Toast.shared.show("Nothing to record", "Turn on system audio or the microphone in Settings › Meetings.", ms: 6000) { SettingsWindow.show() }
            return nil
        }
        let r = MeetingRecorder(o)
        configure?(r)
        current = r
        r.begin()
        AppDelegate.shared?.recordingChanged()
        return r
    }

    private init(_ o: Options) {
        options = o
        let id = UUID()
        tmpFolder = MeetingStore.shared.root.appendingPathComponent(".recording-\(id.uuidString)", isDirectory: true)
        super.init()
        meeting.id = id
        meeting.title = o.title.isEmpty ? "Meeting " + MeetingFormat.defaultTitle.string(from: Date()) : o.title
        meeting.folder = tmpFolder
    }

    // MARK: start

    private func begin() {
        hud = MeetingHUD(recorder: self)
        Task { @MainActor in
            do {
                try FileManager.default.createDirectory(at: tmpFolder.appendingPathComponent("tracks"), withIntermediateDirectories: true)
                var wantOthers = options.others, wantYou = options.you
                if wantYou, !(await AVCaptureDevice.requestAccess(for: .audio)) {
                    wantYou = false
                    notices.append("No microphone access: allow Ather Screenshot in System Settings › Privacy & Security › Microphone.")
                }
                if wantOthers, !(await Capture.resolvePermission()) {
                    wantOthers = false
                    notices.append("No system audio: allow Ather Screenshot in System Settings › Privacy & Security › Screen & System Audio Recording.")
                }
                guard wantOthers || wantYou else { throw MeetingEdit.Failure.failed(notices.joined(separator: "\n")) }
                if wantOthers {
                    do { try await startSystemAudio() } catch {
                        if !wantYou { throw error }
                        notices.append("Others' sound isn't recorded: \(error.localizedDescription)")
                    }
                }
                if wantYou {
                    do { try startMicrophone() } catch {
                        if others == nil { throw error }
                        notices.append("Your microphone isn't recorded: \(error.localizedDescription)")
                    }
                }
                let t0 = CMClockGetTime(CMClockGetHostTimeClock())
                queue.sync {
                    origin = t0
                    others?.begin(at: t0)
                    you?.begin(at: t0)
                }
                if cancelWhileStarting { return abort(nil, notice: "Meeting recording cancelled") }
                phase = .recording
                startDate = Date()
                meeting.created = startDate
                hud?.started()
                if !notices.isEmpty { Toast.shared.show("Recording the meeting", notices.joined(separator: "\n"), ms: 7000) }
                AppDelegate.shared?.recordingChanged()
                MeetingRecorder.remindConsentOnce()
            } catch {
                abort(cancelWhileStarting ? nil : error.localizedDescription, notice: cancelWhileStarting ? "Meeting recording cancelled" : nil)
            }
        }
    }

    // Recording people is regulated in many places: the first time, say so once.
    static func remindConsentOnce() {
        guard !UserDefaults.standard.bool(forKey: "MeetingConsentShown") else { return }
        UserDefaults.standard.set(true, forKey: "MeetingConsentShown")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) {
            Toast.shared.show("Let everyone know you're recording", "Recording a call may need everyone's consent where you or they are.", ms: 8000)
        }
    }

    private func startSystemAudio() async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let display = content.displays.first else { throw MeetingEdit.Failure.failed("No display.") }
        let filter: SCContentFilter
        if !options.appBundleID.isEmpty {
            guard let app = content.applications.first(where: { $0.bundleIdentifier == options.appBundleID }) else {
                throw MeetingEdit.Failure.failed("\(MeetingApps.name(options.appBundleID)) isn't running.")
            }
            filter = SCContentFilter(display: display, including: [app], exceptingWindows: [])
            meeting.app = app.applicationName
        } else {
            filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        }
        let cfg = SCStreamConfiguration()
        cfg.capturesAudio = true
        cfg.excludesCurrentProcessAudio = true   // never our own sounds (toasts, playback)
        cfg.sampleRate = 48000
        cfg.channelCount = 2
        cfg.width = 2                            // video is required by the stream; keep it tiny and rare
        cfg.height = 2
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: 1)
        cfg.queueDepth = 3
        cfg.showsCursor = false
        let w = try TrackWriter(url: tmpFolder.appendingPathComponent("tracks/others.m4a"), channels: 2)
        queue.sync { others = w }
        let st = SCStream(filter: filter, configuration: cfg, delegate: self)
        try st.addStreamOutput(self, type: .audio, sampleHandlerQueue: queue)
        try st.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)  // frames are dropped; avoids log noise
        try await st.startCapture()
        stream = st
        meeting.hasOthers = true
    }

    private func startMicrophone() throws {
        let device = options.micID.isEmpty ? AVCaptureDevice.default(for: .audio) : (AVCaptureDevice(uniqueID: options.micID) ?? AVCaptureDevice.default(for: .audio))
        guard let device else { throw MeetingEdit.Failure.failed("No microphone found.") }
        let s = AVCaptureSession()
        let input = try AVCaptureDeviceInput(device: device)
        guard s.canAddInput(input) else { throw MeetingEdit.Failure.failed("Can't use \(device.localizedName).") }
        s.addInput(input)
        let out = AVCaptureAudioDataOutput()
        out.audioSettings = [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 1,
                             AVLinearPCMBitDepthKey: 32, AVLinearPCMIsFloatKey: true, AVLinearPCMIsNonInterleaved: false]
        out.setSampleBufferDelegate(self, queue: queue)
        guard s.canAddOutput(out) else { throw MeetingEdit.Failure.failed("Can't read \(device.localizedName).") }
        s.addOutput(out)
        let w = try TrackWriter(url: tmpFolder.appendingPathComponent("tracks/you.m4a"), channels: 1)
        queue.sync { you = w }
        s.startRunning()
        guard s.isRunning else { throw MeetingEdit.Failure.failed("\(device.localizedName) didn't start.") }
        session = s
        meeting.hasYou = true
        meeting.micName = device.localizedName
    }

    // MARK: samples (on `queue`)

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, sb.isValid else { return }
        take(sb, into: others, meter: 0)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard sb.isValid else { return }
        // The capture session stamps samples on its own clock: move them to host time, where the system audio is.
        var out = sb
        if let clock = session?.synchronizationClock {
            let host = CMClockGetHostTimeClock()
            let pts = CMSyncConvertTime(sb.presentationTimeStamp, from: clock, to: host)
            if let copy = MeetingRecorder.retime(sb, by: CMTimeSubtract(sb.presentationTimeStamp, pts)) { out = copy }
        }
        take(out, into: you, meter: 1)
    }

    private func take(_ sb: CMSampleBuffer, into w: TrackWriter?, meter: Int) {
        let level = MeetingRecorder.level(sb)
        levelLock.lock()
        if meter == 0 { _levels.others = max(level, _levels.others * 0.8) } else { _levels.you = max(level, _levels.you * 0.8) }
        levelLock.unlock()
        guard let w, origin.isValid, !paused, !finished else { return }
        let pts = sb.presentationTimeStamp
        guard pts >= origin else { return }
        // Captured during a pause but delivered after it: retiming would put it before what came earlier.
        if resumeAt.isValid, pts < resumeAt { return }
        if pauseOffset != .zero {
            if let r = MeetingRecorder.retime(sb, by: pauseOffset) { w.append(r) }
        } else {
            w.append(sb)
        }
    }

    static func retime(_ sb: CMSampleBuffer, by offset: CMTime) -> CMSampleBuffer? {
        guard offset != .zero else { return sb }
        var count: CMItemCount = 0
        CMSampleBufferGetSampleTimingInfoArray(sb, entryCount: 0, arrayToFill: nil, entriesNeededOut: &count)
        var infos = [CMSampleTimingInfo](repeating: CMSampleTimingInfo(), count: count)
        CMSampleBufferGetSampleTimingInfoArray(sb, entryCount: count, arrayToFill: &infos, entriesNeededOut: &count)
        for i in infos.indices {
            infos[i].presentationTimeStamp = CMTimeSubtract(infos[i].presentationTimeStamp, offset)
            if infos[i].decodeTimeStamp.isValid { infos[i].decodeTimeStamp = CMTimeSubtract(infos[i].decodeTimeStamp, offset) }
        }
        var out: CMSampleBuffer?
        CMSampleBufferCreateCopyWithNewTiming(allocator: nil, sampleBuffer: sb, sampleTimingEntryCount: count, sampleTimingArray: &infos, sampleBufferOut: &out)
        return out
    }

    // Loudness of a buffer for the meter, 0…1 on a dB scale from -60 dBFS.
    static func level(_ sb: CMSampleBuffer) -> Float {
        guard let fmt = sb.formatDescription, let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fmt)?.pointee else { return 0 }
        var block: CMBlockBuffer?
        var size = 0
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: &size, bufferListOut: nil, bufferListSize: 0,
                                                                blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil)
        guard size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: MemoryLayout<AudioBufferList>.alignment)
        defer { raw.deallocate() }
        let abl = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: nil, bufferListOut: abl, bufferListSize: size,
                                                                      blockBufferAllocator: nil, blockBufferMemoryAllocator: nil,
                                                                      flags: kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
                                                                      blockBufferOut: &block) == noErr else { return 0 }
        let isFloat = asbd.mFormatFlags & kAudioFormatFlagIsFloat != 0
        var sum: Double = 0
        var n = 0
        for b in UnsafeMutableAudioBufferListPointer(abl) {
            guard let d = b.mData else { continue }
            if isFloat && asbd.mBitsPerChannel == 32 {
                let p = d.bindMemory(to: Float.self, capacity: Int(b.mDataByteSize) / 4)
                for i in stride(from: 0, to: Int(b.mDataByteSize) / 4, by: 4) { sum += Double(p[i] * p[i]); n += 1 }
            } else if asbd.mBitsPerChannel == 16 {
                let p = d.bindMemory(to: Int16.self, capacity: Int(b.mDataByteSize) / 2)
                for i in stride(from: 0, to: Int(b.mDataByteSize) / 2, by: 4) { let v = Double(p[i]) / 32768; sum += v * v; n += 1 }
            }
        }
        guard n > 0 else { return 0 }
        let db = 20 * log10(max(1e-6, sqrt(sum / Double(n))))
        return Float(max(0, min(1, (db + 60) / 60)))
    }

    // System audio stopped by itself (the app quit, the display went away): keep the microphone going if there is one.
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async {
            guard self.phase == .recording else { return }
            if self.session != nil {
                self.notices.append("Others' sound stopped: \(error.localizedDescription)")
                Toast.shared.show("Others' sound stopped", "The microphone is still recorded. \(error.localizedDescription)", ms: 6000)
                self.stream = nil
            } else {
                self.stop(reason: error.localizedDescription)
            }
        }
    }

    // MARK: control

    func togglePause() {
        guard phase == .recording else { return }
        if let at = pausedAt {
            pausedTotal += Date().timeIntervalSince(at)
            pausedAt = nil
            queue.async {
                let now = CMClockGetTime(CMClockGetHostTimeClock())
                self.pauseOffset = CMTimeAdd(self.pauseOffset, CMTimeSubtract(now, self.pauseStart))
                self.resumeAt = now
                self.paused = false
            }
        } else {
            pausedAt = Date()
            queue.async {
                self.pauseStart = CMClockGetTime(CMClockGetHostTimeClock())
                self.paused = true
            }
        }
        hud?.update()
    }

    // Marks this moment (recording time). Returns the marker.
    @discardableResult
    func mark(_ label: String = "", at seconds: Double? = nil) -> MeetingMarker? {
        guard phase == .recording else { return nil }
        let n = meeting.markers.count + 1
        let m = MeetingMarker(time: max(0, seconds ?? elapsed), label: label.isEmpty ? "Marked moment \(n)" : label)
        meeting.markers.append(m)
        hud?.flashMark()
        return m
    }

    func discard() { stop(discard: true) }

    func stop(discard: Bool = false, reason: String? = nil) {
        switch phase {
        case .finishing: return
        case .starting:
            cancelWhileStarting = true
            return
        case .recording: break
        }
        if pausedAt != nil { togglePause() }
        phase = .finishing
        let duration = elapsed
        hud?.finishing(discard: discard)
        let st = stream, se = session
        Task { @MainActor in
            try? await st?.stopCapture()
            se?.stopRunning()
            let end = CMTimeAdd(self.origin, CMTime(seconds: duration, preferredTimescale: 48000))
            let writers: [TrackWriter] = self.queue.sync {
                self.finished = true
                return [self.others, self.you].compactMap { $0 }
            }
            if discard {
                writers.forEach { $0.cancel() }
                return self.abort(nil, notice: "Meeting recording discarded")
            }
            for w in writers { await w.finish(end: end) }
            await self.save(duration: duration, note: reason)
        }
    }

    private func save(duration: TimeInterval, note: String?) async {
        let fm = FileManager.default
        meeting.duration = duration
        meeting.hasOthers = meeting.hasOthers && fm.fileExists(atPath: tmpFolder.appendingPathComponent("tracks/others.m4a").path)
        meeting.hasYou = meeting.hasYou && fm.fileExists(atPath: tmpFolder.appendingPathComponent("tracks/you.m4a").path)
        guard meeting.hasOthers || meeting.hasYou else { return abort("Nothing was recorded.") }
        do {
            try await MeetingRecorder.mix(meeting.tracks.map(meeting.trackURL), to: meeting.audioURL)
        } catch {
            return abort("Couldn't save the meeting: \(error.localizedDescription)")
        }
        let store = MeetingStore.shared
        let dst = store.uniqueFolder(store.folderName(created: meeting.created, title: meeting.title))
        do {
            try fm.moveItem(at: tmpFolder, to: dst)
            meeting.folder = dst
        } catch {
            meeting.folder = tmpFolder  // still saved, just under the hidden name
        }
        meeting.transcript = options.transcribe ? .pending : .none
        store.save(meeting)
        let saved = meeting
        finishedCleanup()
        let outcome = Outcome(meeting: saved, error: nil)
        onFinish?(outcome)
        whenFinished.forEach { $0(outcome) }
        Toast.shared.show(note == nil ? "Meeting saved" : "Meeting recording stopped: \(note!)",
                          "\(saved.title)  ·  \(MeetingFormat.clock(saved.duration))" + (options.transcribe ? "  ·  transcribing on this Mac…" : ""),
                          ms: Settings.shared.int("ToastMs") + 3500) { MeetingsWindow.show(select: saved.id) }
        if options.transcribe { MeetingPipeline.shared.process(saved.id) }
    }

    private func abort(_ error: String?, notice: String? = nil) {
        let st = stream, se = session
        queue.sync {
            finished = true
            others?.cancel()
            you?.cancel()
        }
        Task { try? await st?.stopCapture() }
        se?.stopRunning()
        try? FileManager.default.removeItem(at: tmpFolder)
        finishedCleanup()
        let outcome = Outcome(meeting: nil, error: error)
        onFinish?(outcome)
        whenFinished.forEach { $0(outcome) }
        if let error { Toast.shared.show("Meeting recording failed", error, ms: 7000) } else if let notice { Toast.shared.show(notice) }
    }

    private func finishedCleanup() {
        hud?.close()
        hud = nil
        stream = nil
        session = nil
        if MeetingRecorder.current === self { MeetingRecorder.current = nil }
        AppDelegate.shared?.recordingChanged()
        let f = MeetingRecorder.onFinished
        MeetingRecorder.onFinished = []
        f.forEach { $0() }
    }

    // MARK: mixing

    // All tracks played together into one AAC file (or the one track, re-encoded the same way).
    static func mix(_ tracks: [URL], to out: URL) async throws {
        let comp = AVMutableComposition()
        var params: [AVMutableAudioMixInputParameters] = []
        var longest = CMTime.zero
        for u in tracks {
            let a = AVURLAsset(url: u)
            guard let src = try await a.loadTracks(withMediaType: .audio).first else { continue }
            let dur = try await a.load(.duration)
            guard dur > .zero, let t = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) else { continue }
            try t.insertTimeRange(CMTimeRange(start: .zero, duration: dur), of: src, at: .zero)
            let p = AVMutableAudioMixInputParameters(track: t)
            p.setVolume(tracks.count > 1 ? 0.9 : 1, at: .zero)  // a little headroom when both talk at once
            params.append(p)
            longest = max(longest, dur)
        }
        guard !params.isEmpty, longest > .zero else { throw MeetingEdit.Failure.failed("The recording is empty.") }
        try? FileManager.default.removeItem(at: out)
        guard let s = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetAppleM4A) else { throw MeetingEdit.Failure.failed("Can't mix the audio.") }
        let mix = AVMutableAudioMix()
        mix.inputParameters = params
        s.audioMix = mix
        s.outputURL = out
        s.outputFileType = .m4a
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in s.exportAsynchronously { k.resume() } }
        guard s.status == .completed else { throw MeetingEdit.Failure.failed(s.error?.localizedDescription ?? "Mixing failed.") }
    }
}

// One source's AAC file. Samples come in on the recorder's queue.
final class TrackWriter {
    private let writer: AVAssetWriter
    private let input: AVAssetWriterInput
    private var started = false
    private var lastPTS = CMTime.invalid
    private(set) var samples = 0

    init(url: URL, channels: Int) throws {
        try? FileManager.default.removeItem(at: url)
        writer = try AVAssetWriter(outputURL: url, fileType: .m4a)
        input = AVAssetWriterInput(mediaType: .audio, outputSettings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48000, AVNumberOfChannelsKey: channels,
            AVEncoderBitRateKey: channels > 1 ? 128_000 : 96_000,
        ])
        input.expectsMediaDataInRealTime = true
        guard writer.canAdd(input) else { throw MeetingEdit.Failure.failed("Can't write the audio.") }
        writer.add(input)
        guard writer.startWriting() else { throw writer.error ?? MeetingEdit.Failure.failed("Can't write the audio.") }
    }

    // Time 0 of the file is `origin` (host time), for every track.
    func begin(at origin: CMTime) {
        writer.startSession(atSourceTime: origin)
        started = true
    }

    func append(_ sb: CMSampleBuffer) {
        guard started, writer.status == .writing, input.isReadyForMoreMediaData else { return }
        let pts = sb.presentationTimeStamp
        if lastPTS.isValid && pts <= lastPTS { return }
        if input.append(sb) { lastPTS = pts; samples += 1 }
    }

    func cancel() { if writer.status == .writing { writer.cancelWriting() } }

    func finish(end: CMTime) async {
        guard writer.status == .writing else { return }
        input.markAsFinished()
        if samples > 0 { writer.endSession(atSourceTime: end) }
        await writer.finishWriting()
        if samples == 0 || writer.status != .completed { try? FileManager.default.removeItem(at: writer.outputURL) }
    }
}

// Apps people meet in, for the "Record sound from" choice and for spotting a call.
enum MeetingApps {
    static let known: [(id: String, name: String)] = [
        ("us.zoom.xos", "Zoom"), ("com.microsoft.teams2", "Microsoft Teams"), ("com.microsoft.teams", "Microsoft Teams (classic)"),
        ("com.tinyspeck.slackmacgap", "Slack"), ("com.apple.FaceTime", "FaceTime"), ("com.cisco.webexmeetingsapp", "Webex"),
        ("Cisco-Systems.Spark", "Webex"), ("com.hnc.Discord", "Discord"), ("net.whatsapp.WhatsApp", "WhatsApp"),
        ("com.google.Chrome", "Google Chrome"), ("com.apple.Safari", "Safari"), ("company.thebrowser.Browser", "Arc"),
        ("com.microsoft.edgemac", "Microsoft Edge"), ("org.mozilla.firefox", "Firefox"), ("com.brave.Browser", "Brave"),
        ("com.skype.skype", "Skype"), ("com.ringcentral.glip", "RingCentral"), ("com.gotomeeting.GoToMeeting", "GoTo Meeting"),
    ]
    static let browsers: Set<String> = ["com.google.Chrome", "com.apple.Safari", "company.thebrowser.Browser", "com.microsoft.edgemac", "org.mozilla.firefox", "com.brave.Browser"]

    static func name(_ id: String) -> String {
        known.first { $0.id == id }?.name ?? NSRunningApplication.runningApplications(withBundleIdentifier: id).first?.localizedName ?? id
    }

    // Choices for Settings: every app's sound, the meeting apps that are running, then the others that are running.
    static func choices() -> [(String, String)] {
        var out: [(String, String)] = [("", "All apps (everyone in any call)")]
        let running = NSWorkspace.shared.runningApplications.filter { $0.activationPolicy == .regular && $0.bundleIdentifier != nil && $0.bundleIdentifier != kBundleID }
        var seen = Set<String>()
        for k in known where running.contains(where: { $0.bundleIdentifier == k.id }) && seen.insert(k.id).inserted { out.append((k.id, k.name)) }
        for a in running.sorted(by: { ($0.localizedName ?? "") < ($1.localizedName ?? "") }) {
            guard let id = a.bundleIdentifier, seen.insert(id).inserted else { continue }
            out.append((id, a.localizedName ?? id))
        }
        let cur = Settings.shared.string("MeetingApp")
        if !cur.isEmpty, !seen.contains(cur) { out.append((cur, name(cur) + " (not running)")) }
        return out
    }
}
