import AVFoundation
import AppKit
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

enum RecordTarget {
    case region(CGRect)       // CG global points
    case window(WindowInfo)   // follows the window, even when it's covered
}

// One recording at a time: countdown → ScreenCaptureKit stream → MP4 (H.264 + AAC) or GIF.
// The countdown, frame and control bar are excluded from the stream; click ripples and the key pill are not.
final class Recorder: NSObject, SCStreamOutput, SCStreamDelegate {
    static var current: Recorder?
    static var isActive: Bool { current != nil }

    let gif: Bool
    let target: RecordTarget
    private let s = Settings.shared
    private let queue = DispatchQueue(label: "ather.recorder")
    private var stream: SCStream?
    private var countdown: Countdown?
    private let chrome = RecordingChrome()
    private var viz: InputViz?
    private var finished = false
    // countdown → starting (async stream setup) → recording; stop() during the first two cancels cleanly.
    private enum Phase { case countdown, starting, recording }
    private var phase = Phase.countdown
    // Recording or still saving the file: quitting must wait for it.
    var hasFootage: Bool { phase == .recording }
    private var cancelWhileStarting = false

    // Touched only on `queue`.
    private var writer: AVAssetWriter?
    private var videoIn: AVAssetWriterInput?
    private var audioIn: AVAssetWriterInput?
    private var micIn: AVAssetWriterInput?
    private var sessionStarted = false
    private var paused = false
    private var pauseStart = CMTime.invalid
    private var pauseOffset = CMTime.zero
    private var resumeAt = CMTime.invalid      // raw host time of the last resume
    private var lastVideoPTS = CMTime.invalid  // last appended (retimed) timestamps, kept increasing
    private var lastAudioPTS = CMTime.invalid
    private var lastMicPTS = CMTime.invalid
    private var micAllowed = true
    private var gifDir: URL?
    private var gifFrames: [(URL, CMTime)] = []
    private var gifInterval = CMTime(value: 1, timescale: 15)
    private var stopTime = CMTime.invalid
    private var frameCount = 0
    // Game controller overlay, burned into the frames (touched on `queue`).
    private var pad: GamepadMonitor?
    private var padCorner = PadCorner.bottomRight
    private var padOpacity: CGFloat = 1
    private var padScale: CGFloat = 1             // frame pixels per point
    private var padTimer: DispatchSourceTimer?
    private var padSeen = 0                       // the monitor's change count when the last frame was drawn
    private var lastFrame: CVPixelBuffer?         // the last frame sent, with the pad drawn on it
    private var lastFramePTS = CMTime.invalid     // raw host time it was captured at
    private var underPad: (rect: CGRect, bytes: Data)?  // what the last frame looked like under the pad

    private let tmpURL: URL
    private var startDate = Date()
    private var pausedTotal: TimeInterval = 0
    private var pausedAt: Date?
    var isPaused: Bool { pausedAt != nil }

    static func start(gif: Bool, target: RecordTarget) {
        guard current == nil else { return }
        let r = Recorder(gif: gif, target: target)
        current = r
        r.begin()
    }

    private init(gif: Bool, target: RecordTarget) {
        self.gif = gif
        self.target = target
        tmpURL = FileManager.default.temporaryDirectory.appendingPathComponent("ather-\(UUID().uuidString).\(gif ? "gif" : "mp4")")
        super.init()
    }

    private var regionRect: CGRect {
        switch target {
        case .region(let r): return r
        case .window(let w): return w.frame
        }
    }

    private func begin() {
        let secs = s.int("CountdownSeconds")
        guard secs > 0 else { return startCapture() }
        countdown = Countdown(over: regionRect, seconds: secs, done: { [weak self] ok in
            guard let self, !self.finished else { return }
            self.countdown = nil
            if ok { self.startCapture() } else { self.abort(nil) }
        })
    }

    private func startCapture() {
        phase = .starting
        if s.bool("ShowGamepad") {
            pad = GamepadMonitor()
            padCorner = PadCorner.parse(s.string("GamepadCorner"))
            padOpacity = CGFloat(max(10, min(100, s.int("GamepadOpacity")))) / 100
        }
        if case .region(let r) = target { chrome.showFrame(around: r) }
        chrome.showBar(near: regionRect, recorder: self)
        Task { @MainActor in
            do {
                if s.bool("RecordMicrophone"), !gif {
                    self.micAllowed = await AVCaptureDevice.requestAccess(for: .audio)
                    if !self.micAllowed {
                        Toast.shared.show("Recording without the microphone", "Allow Ather Screenshot in System Settings › Privacy & Security › Microphone.", ms: 6000)
                    }
                }
                try await self.startStream()
                if self.cancelWhileStarting {  // stop/discard was pressed while the stream was being set up
                    self.queue.sync { self.finished = true }  // sample callbacks stop touching the writer
                    try? await self.stream?.stopCapture()
                    self.queue.sync { self.writer?.cancelWriting() }
                    return self.abort(nil, notice: "Recording cancelled")
                }
                self.phase = .recording
                self.startDate = Date()
                self.chrome.startTimer()
                if self.s.bool("ShowClicks") || self.s.bool("ShowKeys") {
                    self.viz = InputViz(clicks: self.s.bool("ShowClicks"), keys: self.s.bool("ShowKeys"), region: self.regionRect)
                }
            } catch {
                self.queue.sync {
                    self.finished = true
                    self.writer?.cancelWriting()
                }
                self.abort(self.cancelWhileStarting ? nil : error.localizedDescription, notice: self.cancelWhileStarting ? "Recording cancelled" : nil)
            }
        }
    }

    private func startStream() async throws {
        guard await Capture.resolvePermission() else { throw CaptureError.permission }
        let content = try await Capture.content()
        let cfg = SCStreamConfiguration()
        let filter: SCContentFilter
        var pxSize: CGSize
        switch target {
        case .region(let r):
            let center = r.center
            guard let d = content.displays.first(where: { CGDisplayBounds($0.displayID).contains(center) }) ?? content.displays.first else {
                throw CaptureError.failed("No display to record.")
            }
            let frame = CGDisplayBounds(d.displayID)
            let local = r.intersection(frame).offsetBy(dx: -frame.minX, dy: -frame.minY)
            filter = SCContentFilter(display: d, excludingWindows: Capture.excludedWindows(content))
            cfg.sourceRect = local
            pxSize = CGSize(width: local.width * Capture.scale(of: d.displayID), height: local.height * Capture.scale(of: d.displayID))
        case .window(let w):
            guard let scw = content.windows.first(where: { $0.windowID == w.id }) else { throw CaptureError.failed("That window is gone.") }
            filter = SCContentFilter(desktopIndependentWindow: scw)
            let k = CGFloat(filter.pointPixelScale)
            pxSize = CGSize(width: filter.contentRect.width * k, height: filter.contentRect.height * k)
        }
        guard pxSize.width >= 2, pxSize.height >= 2, pxSize.width.isFinite, pxSize.height.isFinite, regionRect.width > 0 else {
            throw CaptureError.failed("That area is too small to record.")
        }
        if gif {
            // GIFs at 1× and at most 1280 wide: Retina GIFs get huge for no visible gain.
            let k = min(1, 1280 / pxSize.width, 1 / max(1, pxSize.width / regionRect.width))
            pxSize = CGSize(width: pxSize.width * k, height: pxSize.height * k)
        } else {
            let k = min(1, 4096 / pxSize.width, 4096 / pxSize.height)  // H.264 limit
            pxSize = CGSize(width: pxSize.width * k, height: pxSize.height * k)
        }
        let W = max(2, Int(pxSize.width) & ~1), H = max(2, Int(pxSize.height) & ~1)
        padScale = CGFloat(W) / max(1, regionRect.width)
        let fps = gif ? max(1, min(50, s.int("GifFps"))) : max(1, min(120, s.int("VideoFps")))
        cfg.width = W
        cfg.height = H
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        cfg.showsCursor = s.bool("RecordCursor")
        cfg.queueDepth = 6
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        cfg.colorSpaceName = CGColorSpace.sRGB
        cfg.scalesToFit = true
        let systemAudio = !gif && s.bool("RecordSystemAudio")
        let mic = !gif && s.bool("RecordMicrophone") && micAllowed
        if systemAudio {
            cfg.capturesAudio = true
            cfg.excludesCurrentProcessAudio = true
            cfg.sampleRate = 48000
            cfg.channelCount = 2
        }
        if mic, #available(macOS 15.0, *) {
            cfg.captureMicrophone = true
            cfg.microphoneCaptureDeviceID = AVCaptureDevice.default(for: .audio)?.uniqueID
        }

        if gif {
            let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-gif-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            gifDir = dir
            gifInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        } else {
            let w = try AVAssetWriter(outputURL: tmpURL, fileType: .mp4)
            let v = AVAssetWriterInput(mediaType: .video, outputSettings: [
                AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: W, AVVideoHeightKey: H,
                AVVideoCompressionPropertiesKey: [
                    AVVideoAverageBitRateKey: max(2_000_000, min(40_000_000, W * H * fps / 6)),
                    AVVideoExpectedSourceFrameRateKey: fps, AVVideoMaxKeyFrameIntervalKey: fps * 2,
                    AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                ],
            ])
            v.expectsMediaDataInRealTime = true
            w.add(v)
            videoIn = v
            func audioInput() -> AVAssetWriterInput {
                let a = AVAssetWriterInput(mediaType: .audio, outputSettings: [
                    AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2, AVEncoderBitRateKey: 160_000,
                ])
                a.expectsMediaDataInRealTime = true
                w.add(a)
                return a
            }
            if systemAudio { audioIn = audioInput() }
            if mic, #available(macOS 15.0, *) { micIn = audioInput() }
            guard w.startWriting() else { throw w.error ?? CaptureError.failed("Can't start the video writer.") }
            writer = w
        }

        let st = SCStream(filter: filter, configuration: cfg, delegate: self)
        try st.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        if systemAudio { try st.addStreamOutput(self, type: .audio, sampleHandlerQueue: queue) }
        if mic, #available(macOS 15.0, *) { try st.addStreamOutput(self, type: .microphone, sampleHandlerQueue: queue) }
        try await st.startCapture()
        stream = st
        if pad != nil { startPadTimer(fps: fps) }
    }

    // MARK: samples

    private static var hostNow: CMTime { CMClockGetTime(CMClockGetHostTimeClock()) }

    private func retimed(_ sb: CMSampleBuffer) -> CMSampleBuffer? {
        guard pauseOffset != .zero else { return sb }
        var count: CMItemCount = 0
        CMSampleBufferGetSampleTimingInfoArray(sb, entryCount: 0, arrayToFill: nil, entriesNeededOut: &count)
        var infos = [CMSampleTimingInfo](repeating: CMSampleTimingInfo(), count: count)
        CMSampleBufferGetSampleTimingInfoArray(sb, entryCount: count, arrayToFill: &infos, entriesNeededOut: &count)
        for i in infos.indices {
            infos[i].presentationTimeStamp = CMTimeSubtract(infos[i].presentationTimeStamp, pauseOffset)
            if infos[i].decodeTimeStamp.isValid { infos[i].decodeTimeStamp = CMTimeSubtract(infos[i].decodeTimeStamp, pauseOffset) }
        }
        var out: CMSampleBuffer?
        CMSampleBufferCreateCopyWithNewTiming(allocator: nil, sampleBuffer: sb, sampleTimingEntryCount: count, sampleTimingArray: &infos, sampleBufferOut: &out)
        return out
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard sb.isValid, !paused, !finished else { return }
        // Captured during a pause but delivered after resume: retiming would move it before earlier samples.
        if resumeAt.isValid, sb.presentationTimeStamp < resumeAt { return }
        switch type {
        case .screen:
            guard let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
                  let raw = atts.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete else { return }
            if let px = sb.imageBuffer { drawPad(on: px, at: sb.presentationTimeStamp) }
            if gif { return gifFrame(sb) }
            appendVideo(sb)
        case .audio:
            appendAudio(sb, to: audioIn, last: &lastAudioPTS)
        default:
            if #available(macOS 15.0, *), type == .microphone { appendAudio(sb, to: micIn, last: &lastMicPTS) }
        }
    }

    private func appendVideo(_ sb: CMSampleBuffer) {
        guard let w = writer, let v = videoIn, let sb2 = retimed(sb) else { return }
        if !sessionStarted {
            w.startSession(atSourceTime: sb2.presentationTimeStamp)
            sessionStarted = true
        }
        let pts = sb2.presentationTimeStamp
        if lastVideoPTS.isValid && pts <= lastVideoPTS { return }  // the writer fails on non-increasing timestamps
        if v.isReadyForMoreMediaData, v.append(sb2) {
            frameCount += 1
            lastVideoPTS = pts
        }
    }

    // MARK: game controller

    // Draws the latched pad state onto a fresh frame, keeping a clean copy of what's under it.
    private func drawPad(on px: CVPixelBuffer, at pts: CMTime) {
        guard let pad else { return }
        padSeen = pad.changeCount
        let state = pad.take()
        let w = CVPixelBufferGetWidth(px), h = CVPixelBufferGetHeight(px)
        let rect = Gamepad.footprint(frameW: w, frameH: h, corner: padCorner, scale: padScale)
        underPad = Recorder.copyRows(px, rect).map { (rect, $0) }
        Gamepad.draw(into: px, state: state, corner: padCorner, scale: padScale, opacity: padOpacity)
        lastFrame = px
        lastFramePTS = pts
    }

    // ScreenCaptureKit sends frames only when the screen changes. While it's still, a press must still show:
    // resend the last frame with the new pad state. While paused, keep emptying the latch, so presses made
    // during the pause don't all show at once after it.
    private func startPadTimer(fps: Int) {
        let t = DispatchSource.makeTimerSource(queue: queue)
        let interval = 1.0 / Double(fps)
        t.schedule(deadline: .now() + interval, repeating: interval)
        t.setEventHandler { [weak self] in self?.padTick(interval: interval) }
        t.resume()
        padTimer = t
    }

    private func padTick(interval: Double) {
        guard let pad, !finished else { return }
        if paused { _ = pad.take(); return }
        guard pad.changeCount != padSeen, let last = lastFrame, let under = underPad, lastFramePTS.isValid else { return }
        let now = Recorder.hostNow
        guard CMTimeGetSeconds(CMTimeSubtract(now, lastFramePTS)) >= interval * 0.9 else { return }  // a real frame is due anyway
        guard let copy = Recorder.copy(last) else { return }
        Recorder.pasteRows(copy, under.rect, under.bytes)
        drawPad(on: copy, at: now)
        var fmt: CMVideoFormatDescription?
        CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: copy, formatDescriptionOut: &fmt)
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: now, decodeTimeStamp: .invalid)
        var sb: CMSampleBuffer?
        guard let fmt, CMSampleBufferCreateReadyWithImageBuffer(allocator: nil, imageBuffer: copy, formatDescription: fmt, sampleTiming: &timing, sampleBufferOut: &sb) == noErr,
              let sb else { return }
        if resumeAt.isValid, now < resumeAt { return }
        if gif { gifFrame(sb) } else { appendVideo(sb) }
    }

    static func copyRows(_ px: CVPixelBuffer, _ r: CGRect) -> Data? {
        guard !r.isEmpty else { return nil }
        CVPixelBufferLockBaseAddress(px, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(px, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(px) else { return nil }
        let bpr = CVPixelBufferGetBytesPerRow(px), x = Int(r.minX) * 4, n = Int(r.width) * 4
        var d = Data(count: n * Int(r.height))
        d.withUnsafeMutableBytes { dst in
            for row in 0..<Int(r.height) {
                memcpy(dst.baseAddress! + row * n, base + (Int(r.minY) + row) * bpr + x, n)
            }
        }
        return d
    }

    static func pasteRows(_ px: CVPixelBuffer, _ r: CGRect, _ d: Data) {
        CVPixelBufferLockBaseAddress(px, [])
        defer { CVPixelBufferUnlockBaseAddress(px, []) }
        guard let base = CVPixelBufferGetBaseAddress(px) else { return }
        let bpr = CVPixelBufferGetBytesPerRow(px), x = Int(r.minX) * 4, n = Int(r.width) * 4
        d.withUnsafeBytes { src in
            for row in 0..<Int(r.height) { memcpy(base + (Int(r.minY) + row) * bpr + x, src.baseAddress! + row * n, n) }
        }
    }

    static func copy(_ px: CVPixelBuffer) -> CVPixelBuffer? {
        let w = CVPixelBufferGetWidth(px), h = CVPixelBufferGetHeight(px)
        var out: CVPixelBuffer?
        let attrs = [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attrs, &out) == kCVReturnSuccess, let out else { return nil }
        CVPixelBufferLockBaseAddress(px, .readOnly)
        CVPixelBufferLockBaseAddress(out, [])
        defer {
            CVPixelBufferUnlockBaseAddress(out, [])
            CVPixelBufferUnlockBaseAddress(px, .readOnly)
        }
        guard let src = CVPixelBufferGetBaseAddress(px), let dst = CVPixelBufferGetBaseAddress(out) else { return nil }
        let sb = CVPixelBufferGetBytesPerRow(px), db = CVPixelBufferGetBytesPerRow(out)
        for row in 0..<h { memcpy(dst + row * db, src + row * sb, min(sb, db)) }
        return out
    }

    private func appendAudio(_ sb: CMSampleBuffer, to input: AVAssetWriterInput?, last: inout CMTime) {
        guard sessionStarted, let input, input.isReadyForMoreMediaData, let sb2 = retimed(sb) else { return }
        let pts = sb2.presentationTimeStamp
        if last.isValid && pts <= last { return }
        if input.append(sb2) { last = pts }
    }

    private func gifFrame(_ sb: CMSampleBuffer) {
        guard let dir = gifDir, let px = sb.imageBuffer else { return }
        let t = CMTimeSubtract(sb.presentationTimeStamp, pauseOffset)
        // The stream already paces frames at the GIF rate; only drop ones that arrive well early (jitter is normal).
        if let last = gifFrames.last?.1, CMTimeSubtract(t, last) < CMTimeMultiplyByFloat64(gifInterval, multiplier: 0.6) { return }
        guard let img = sharedCIContext.createCGImage(CIImage(cvPixelBuffer: px), from: CGRect(x: 0, y: 0, width: CVPixelBufferGetWidth(px), height: CVPixelBufferGetHeight(px))),
              let data = img.pngData() else { return }
        let url = dir.appendingPathComponent(String(format: "%06d.png", gifFrames.count))
        if (try? data.write(to: url)) != nil { gifFrames.append((url, t)) }
    }

    // The stream ended on its own (stopped from the menu bar, window closed, display gone): keep what was recorded.
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async { self.stop(reason: error.localizedDescription) }
    }

    // MARK: control

    func togglePause() {
        guard phase == .recording, !finished else { return }
        if let at = pausedAt {
            pausedTotal += Date().timeIntervalSince(at)
            pausedAt = nil
            queue.async {
                let now = Recorder.hostNow
                self.pauseOffset = CMTimeAdd(self.pauseOffset, CMTimeSubtract(now, self.pauseStart))
                self.resumeAt = now
                self.paused = false
            }
        } else {
            pausedAt = Date()
            queue.async {
                self.pauseStart = Recorder.hostNow
                self.paused = true
            }
        }
        chrome.update()
    }

    var elapsed: TimeInterval {
        Date().timeIntervalSince(startDate) - pausedTotal - (pausedAt.map { Date().timeIntervalSince($0) } ?? 0)
    }

    // Called once the recording is fully over (saved, discarded or failed); used to quit cleanly.
    static var onFinished: [() -> Void] = []

    private func finishedCleanup() {
        if Recorder.current === self { Recorder.current = nil }
        queue.async {
            self.padTimer?.cancel()
            self.padTimer = nil
            self.lastFrame = nil
            self.underPad = nil
            self.pad = nil
        }
        AppDelegate.shared?.recordingChanged()
        let f = Recorder.onFinished
        Recorder.onFinished = []
        f.forEach { $0() }
    }

    private func abort(_ error: String?, notice: String? = nil) {
        finished = true
        countdown?.close()
        countdown = nil
        chrome.close()
        viz?.stop()
        try? FileManager.default.removeItem(at: tmpURL)
        if let d = gifDir { try? FileManager.default.removeItem(at: d) }
        finishedCleanup()
        if let error { Toast.shared.show("Recording failed", error, ms: 6000) } else if let notice { Toast.shared.show(notice) }
    }

    func discard() { stop(discard: true) }

    // `reason`: the stream stopped by itself; what was recorded is still saved.
    func stop(discard: Bool = false, reason: String? = nil) {
        guard !finished else { return }
        switch phase {
        case .countdown:
            return abort(nil, notice: "Recording cancelled")
        case .starting:
            // startCapture's task sees this once the stream is up and tears it down.
            cancelWhileStarting = true
            chrome.close()
            return
        case .recording: break
        }
        if pausedAt != nil { togglePause() }  // before `finished`, so the pause is accounted for
        finished = true
        let duration = elapsed
        chrome.close()
        viz?.stop()
        let st = stream
        Task { @MainActor in
            try? await st?.stopCapture()
            self.queue.async {
                self.stopTime = CMTimeSubtract(Recorder.hostNow, self.pauseOffset)
                if self.gif { self.finishGif(discard: discard, duration: duration, reason: reason) }
                else { self.finishMp4(discard: discard, duration: duration, reason: reason) }
            }
        }
    }

    private func finishMp4(discard: Bool, duration: TimeInterval, reason: String?) {
        guard let w = writer, sessionStarted, !discard else {
            writer?.cancelWriting()
            try? FileManager.default.removeItem(at: tmpURL)
            let err = discard ? nil : (reason ?? "No frames were recorded.")
            return DispatchQueue.main.async { self.done(nil, duration: duration, error: err) }
        }
        [videoIn, audioIn, micIn].forEach { $0?.markAsFinished() }
        w.finishWriting {
            let ok = w.status == .completed
            if !ok { try? FileManager.default.removeItem(at: self.tmpURL) }
            DispatchQueue.main.async {
                self.done(ok ? self.tmpURL : nil, duration: duration, error: ok ? nil : (w.error?.localizedDescription ?? "Couldn't write the video."), note: reason)
            }
        }
    }

    private func finishGif(discard: Bool, duration: TimeInterval, reason: String?) {
        defer { if let d = gifDir { try? FileManager.default.removeItem(at: d) } }
        guard !discard, !gifFrames.isEmpty,
              let dst = CGImageDestinationCreateWithURL(tmpURL as CFURL, UTType.gif.identifier as CFString, gifFrames.count, nil) else {
            let err = discard ? nil : (reason ?? "No frames were recorded.")
            return DispatchQueue.main.async { self.done(nil, duration: duration, error: err) }
        }
        CGImageDestinationSetProperties(dst, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        for (i, f) in gifFrames.enumerated() {
            guard let img = CGImage.load(f.0) else { continue }
            let next = i + 1 < gifFrames.count ? gifFrames[i + 1].1 : stopTime
            let delay = max(0.02, CMTimeGetSeconds(CMTimeSubtract(next, f.1)))
            CGImageDestinationAddImage(dst, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFUnclampedDelayTime: delay, kCGImagePropertyGIFDelayTime: delay]] as CFDictionary)
        }
        let ok = CGImageDestinationFinalize(dst)
        DispatchQueue.main.async { self.done(ok ? self.tmpURL : nil, duration: duration, error: ok ? nil : "Couldn't write the GIF.", note: reason) }
    }

    private func done(_ tmp: URL?, duration: TimeInterval, error: String?, note: String? = nil) {
        defer { finishedCleanup() }
        if let error { return Toast.shared.show("Recording failed", error, ms: 6000) }
        guard let tmp else { return Toast.shared.show("Recording discarded") }
        var info = NameInfo()
        if case .window(let w) = target { info.app = w.app; info.window = w.title }
        let url = Output.newCaptureURL(ext: gif ? "gif" : "mp4", info: info)
        do {
            try FileManager.default.moveItem(at: tmp, to: url)
        } catch {
            return Toast.shared.show("Save failed", error.localizedDescription)
        }
        copyFile(url)
        AppDelegate.shared?.setLast(nil, url: url)
        let size = ByteCountFormatter.string(fromByteCount: Int64((try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0), countStyle: .file)
        Toast.shared.show(note == nil ? (gif ? "GIF saved and copied" : "Video saved and copied") : "Recording stopped: \(note!)",
                          "\(url.lastPathComponent)  ·  \(formatTime(duration))  ·  \(size)",
                          ms: Settings.shared.int("ToastMs") + 2500) { if self.gif { Output.open(url) } else { VideoEditor.open(url) } }
    }
}

func formatTime(_ t: TimeInterval) -> String {
    let s = max(0, Int(t))
    return String(format: "%d:%02d", s / 60, s % 60)
}

// MARK: - Countdown

private final class Countdown {
    private let panel: KeyablePanel
    private let label = NSTextField(labelWithString: "")
    private var left: Int
    private var timer: Timer?
    private let done: (Bool) -> Void

    init(over r: CGRect, seconds: Int, done: @escaping (Bool) -> Void) {
        left = seconds
        self.done = done
        let ns = Geo.toNS(r)
        let size: CGFloat = 132
        panel = roundedPanel(NSRect(x: ns.midX - size / 2, y: ns.midY - size / 2, width: size, height: size), level: .screenSaver)
        excludeFromCapture(panel)
        panel.ignoresMouseEvents = false
        let v = ClickView(frame: NSRect(x: 0, y: 0, width: size, height: size))
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.bg.withAlphaComponent(0.88).cgColor
        v.layer?.cornerRadius = size / 2
        v.layer?.borderColor = Theme.accent.cgColor
        v.layer?.borderWidth = 2
        v.onClick = { [weak self] in self?.finish(false) }
        label.font = .monospacedDigitSystemFont(ofSize: 64, weight: .bold)
        label.textColor = Theme.accent
        label.alignment = .center
        label.frame = NSRect(x: 0, y: (size - 78) / 2 + 6, width: size, height: 78)
        let hint = NSTextField(labelWithString: "click to cancel")
        hint.font = Theme.font(10)
        hint.textColor = Theme.muted
        hint.alignment = .center
        hint.frame = NSRect(x: 0, y: 18, width: size, height: 14)
        v.addSubview(label)
        v.addSubview(hint)
        panel.contentView = v
        tick()
        panel.orderFrontRegardless()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
    }

    private func tick() {
        if left <= 0 { return finish(true) }
        label.stringValue = "\(left)"
        NSSound(named: "Tink")?.play()
        left -= 1
    }

    func close() {
        timer?.invalidate()
        panel.orderOut(nil)
    }

    private func finish(_ ok: Bool) {
        close()
        // Give the window server a frame to drop the countdown before the stream starts.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { self.done(ok) }
    }
}

final class ClickView: NSView {
    var onClick: (() -> Void)?
    override func mouseDown(with event: NSEvent) { onClick?() }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}

// MARK: - Frame and control bar

private final class RecordingChrome {
    private var frame: NSPanel?
    private var bar: KeyablePanel?
    private var time = NSTextField(labelWithString: "0:00")
    private var dot = NSView()
    private var pauseButton: NSButton?
    private var timer: Timer?
    private weak var recorder: Recorder?

    func showFrame(around r: CGRect) {
        let ns = Geo.toNS(r).insetBy(dx: -3, dy: -3)
        let p = NSPanel(contentRect: ns, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.isOpaque = false
        p.backgroundColor = .clear
        p.ignoresMouseEvents = true
        p.level = .statusBar
        p.hasShadow = false
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        excludeFromCapture(p)
        let v = NSView(frame: NSRect(origin: .zero, size: ns.size))
        v.wantsLayer = true
        let border = CAShapeLayer()
        border.path = CGPath(rect: CGRect(origin: .zero, size: ns.size).insetBy(dx: 1, dy: 1), transform: nil)
        border.fillColor = nil
        border.strokeColor = Theme.accent.cgColor
        border.lineWidth = 2
        border.lineDashPattern = [8, 5]
        v.layer?.addSublayer(border)
        p.contentView = v
        p.orderFrontRegardless()
        frame = p
    }

    func showBar(near r: CGRect, recorder: Recorder) {
        self.recorder = recorder
        let w: CGFloat = 268, h: CGFloat = 40
        let ns = Geo.toNS(r)
        let screen = NSScreen.screens.first { $0.frame.intersects(ns) } ?? Geo.mouseScreen
        let vis = screen.visibleFrame
        var y = ns.minY - h - 10
        if y < vis.minY + 4 { y = ns.maxY + 10 }
        if y + h > vis.maxY - 4 { y = max(vis.minY + 12, ns.minY + 12) }  // full-screen region: float inside it
        let x = min(max(vis.minX + 4, ns.midX - w / 2), vis.maxX - w - 4)
        let p = roundedPanel(NSRect(x: x, y: y, width: w, height: h), level: .statusBar)
        excludeFromCapture(p)
        p.isMovableByWindowBackground = true
        let v = NSView(frame: NSRect(x: 0, y: 0, width: w, height: h))
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.surface.cgColor
        v.layer?.cornerRadius = 10
        v.layer?.borderColor = Theme.border.cgColor
        v.layer?.borderWidth = 1
        dot.wantsLayer = true
        dot.layer?.backgroundColor = NSColor.systemRed.cgColor
        dot.layer?.cornerRadius = 5
        time.font = .monospacedDigitSystemFont(ofSize: 13, weight: .semibold)
        time.textColor = Theme.text
        func btn(_ symbol: String, _ tip: String, _ sel: Selector) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip)!, target: self, action: sel)
            b.isBordered = false
            b.toolTip = tip
            b.contentTintColor = Theme.text
            b.symbolConfiguration = .init(pointSize: 14, weight: .medium)
            return b
        }
        let pause = btn("pause.fill", "Pause / resume", #selector(pauseClicked))
        pauseButton = pause
        let stop = btn("stop.fill", "Stop and save", #selector(stopClicked))
        stop.contentTintColor = Theme.accent
        let discard = btn("trash", "Discard", #selector(discardClicked))
        discard.contentTintColor = Theme.muted
        let kind = NSTextField(labelWithString: recorder.gif ? "GIF" : "MP4")
        kind.font = Theme.font(10, .bold)
        kind.textColor = Theme.muted
        let stack = NSStackView(views: [dot, time, kind, NSView(), pause, stop, discard])
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 0, left: 14, bottom: 0, right: 14)
        stack.frame = v.bounds
        stack.autoresizingMask = [.width, .height]
        dot.widthAnchor.constraint(equalToConstant: 10).isActive = true
        dot.heightAnchor.constraint(equalToConstant: 10).isActive = true
        v.addSubview(stack)
        p.contentView = v
        p.orderFrontRegardless()
        bar = p
    }

    func startTimer() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in self?.update() }
    }

    func update() {
        guard let r = recorder else { return }
        time.stringValue = formatTime(r.elapsed)
        pauseButton?.image = NSImage(systemSymbolName: r.isPaused ? "play.fill" : "pause.fill", accessibilityDescription: nil)
        dot.layer?.backgroundColor = r.isPaused ? Theme.muted.cgColor : NSColor.systemRed.cgColor
        dot.layer?.opacity = r.isPaused ? 1 : (Int(Date().timeIntervalSince1970 * 2) % 2 == 0 ? 1 : 0.35)
    }

    func close() {
        timer?.invalidate()
        frame?.orderOut(nil)
        bar?.orderOut(nil)
    }

    @objc private func pauseClicked() { recorder?.togglePause() }
    @objc private func stopClicked() { recorder?.stop() }
    @objc private func discardClicked() { recorder?.discard() }
}

// MARK: - Click ripples and keystroke pill (these do show up in the recording)

final class InputViz {
    private var monitors: [Any] = []
    private var keyPanel: NSPanel?
    private var keyLabel: NSTextField?
    private var keyText = ""
    private var keyTimer: Timer?
    private let region: CGRect

    init(clicks: Bool, keys: Bool, region: CGRect) {
        self.region = region
        if clicks, let m = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown], handler: { [weak self] e in
            self?.ripple(at: NSEvent.mouseLocation, right: e.type == .rightMouseDown)
        }) { monitors.append(m) }
        if keys {
            if !AXIsProcessTrustedWithOptions([kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary) {
                Toast.shared.show("Keystrokes need Accessibility access", "Allow Ather Screenshot in System Settings › Privacy & Security › Accessibility.", ms: 6000)
            }
            if let m = NSEvent.addGlobalMonitorForEvents(matching: [.keyDown], handler: { [weak self] e in self?.key(e) }) { monitors.append(m) }
        }
    }

    func stop() {
        for m in monitors { NSEvent.removeMonitor(m) }
        monitors.removeAll()
        keyTimer?.invalidate()
        keyPanel?.orderOut(nil)
    }

    private func ripple(at p: NSPoint, right: Bool) {
        let size: CGFloat = 64
        let panel = NSPanel(contentRect: NSRect(x: p.x - size / 2, y: p.y - size / 2, width: size, height: size),
                            styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.ignoresMouseEvents = true
        panel.hasShadow = false
        panel.level = .screenSaver
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        let v = NSView(frame: NSRect(x: 0, y: 0, width: size, height: size))
        v.wantsLayer = true
        let ring = CAShapeLayer()
        ring.frame = v.bounds
        ring.path = CGPath(ellipseIn: v.bounds.insetBy(dx: 4, dy: 4), transform: nil)
        ring.fillColor = (right ? NSColor.systemBlue : Theme.accent).withAlphaComponent(0.25).cgColor
        ring.strokeColor = (right ? NSColor.systemBlue : Theme.accent).cgColor
        ring.lineWidth = 3
        v.layer?.addSublayer(ring)
        panel.contentView = v
        panel.orderFrontRegardless()
        let scale = CABasicAnimation(keyPath: "transform.scale")
        scale.fromValue = 0.3
        scale.toValue = 1
        let fade = CABasicAnimation(keyPath: "opacity")
        fade.fromValue = 1
        fade.toValue = 0
        let g = CAAnimationGroup()
        g.animations = [scale, fade]
        g.duration = 0.5
        g.fillMode = .forwards
        g.isRemovedOnCompletion = false
        ring.add(g, forKey: "ripple")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.55) { panel.orderOut(nil) }
    }

    private func key(_ e: NSEvent) {
        let hk = Hotkey.from(event: e)
        let special = hk.keyName.map { Hotkey.glyphs[$0] != nil || $0.count > 1 } ?? false
        let combo = !hk.mods.subtracting(.shift).isEmpty || special
        let piece = combo ? (hk.display.isEmpty ? (e.charactersIgnoringModifiers ?? "") : hk.display) : (e.characters ?? "")
        if combo { keyText = piece } else { keyText = String((keyText + piece).suffix(28)) }
        showKeys()
    }

    private func showKeys() {
        if keyPanel == nil {
            let p = NSPanel(contentRect: .zero, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
            p.isOpaque = false
            p.backgroundColor = .clear
            p.ignoresMouseEvents = true
            p.level = .screenSaver
            p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            let l = NSTextField(labelWithString: "")
            l.font = Theme.font(22, .semibold)
            l.textColor = .white
            l.alignment = .center
            l.wantsLayer = true
            l.drawsBackground = true
            l.backgroundColor = NSColor.black.withAlphaComponent(0.72)
            l.layer?.cornerRadius = 12
            p.contentView = l
            keyPanel = p
            keyLabel = l
        }
        guard let p = keyPanel, let l = keyLabel else { return }
        l.stringValue = "  \(keyText)  "
        let size = NSSize(width: l.fittingSize.width + 16, height: 46)
        let ns = Geo.toNS(region)
        p.setFrame(NSRect(x: ns.midX - size.width / 2, y: ns.minY + 28, width: size.width, height: size.height), display: true)
        p.alphaValue = 1
        p.orderFrontRegardless()
        keyTimer?.invalidate()
        keyTimer = Timer.scheduledTimer(withTimeInterval: 1.4, repeats: false) { [weak self] _ in
            self?.keyText = ""
            self?.keyPanel?.orderOut(nil)
        }
    }
}
