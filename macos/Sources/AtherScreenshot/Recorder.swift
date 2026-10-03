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

    // Touched only on `queue`.
    private var writer: AVAssetWriter?
    private var videoIn: AVAssetWriterInput?
    private var audioIn: AVAssetWriterInput?
    private var micIn: AVAssetWriterInput?
    private var sessionStarted = false
    private var paused = false
    private var pauseStart = CMTime.invalid
    private var pauseOffset = CMTime.zero
    private var gifDir: URL?
    private var gifFrames: [(URL, CMTime)] = []
    private var gifInterval = CMTime(value: 1, timescale: 15)
    private var stopTime = CMTime.invalid
    private var frameCount = 0

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
            guard let self else { return }
            self.countdown = nil
            if ok { self.startCapture() } else { self.abort(nil) }
        })
    }

    private func startCapture() {
        if case .region(let r) = target { chrome.showFrame(around: r) }
        chrome.showBar(near: regionRect, recorder: self)
        Task { @MainActor in
            do {
                if s.bool("RecordMicrophone"), !gif { _ = await AVCaptureDevice.requestAccess(for: .audio) }
                try await self.startStream()
                self.startDate = Date()
                self.chrome.startTimer()
                if self.s.bool("ShowClicks") || self.s.bool("ShowKeys") {
                    self.viz = InputViz(clicks: self.s.bool("ShowClicks"), keys: self.s.bool("ShowKeys"), region: self.regionRect)
                }
            } catch {
                self.abort(error.localizedDescription)
            }
        }
    }

    private func startStream() async throws {
        guard Capture.hasPermission() else { throw CaptureError.permission }
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
        if gif {
            // GIFs at 1× and at most 1280 wide: Retina GIFs get huge for no visible gain.
            let k = min(1, 1280 / pxSize.width, 1 / max(1, pxSize.width / regionRect.width))
            pxSize = CGSize(width: pxSize.width * k, height: pxSize.height * k)
        } else {
            let k = min(1, 4096 / pxSize.width, 4096 / pxSize.height)  // H.264 limit
            pxSize = CGSize(width: pxSize.width * k, height: pxSize.height * k)
        }
        let W = max(2, Int(pxSize.width) & ~1), H = max(2, Int(pxSize.height) & ~1)
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
        let mic = !gif && s.bool("RecordMicrophone")
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
        switch type {
        case .screen:
            guard let atts = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
                  let raw = atts.first?[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete else { return }
            if gif { return gifFrame(sb) }
            guard let w = writer, let v = videoIn, let sb2 = retimed(sb) else { return }
            if !sessionStarted {
                w.startSession(atSourceTime: sb2.presentationTimeStamp)
                sessionStarted = true
            }
            if v.isReadyForMoreMediaData, v.append(sb2) { frameCount += 1 }
        case .audio:
            appendAudio(sb, to: audioIn)
        default:
            if #available(macOS 15.0, *), type == .microphone { appendAudio(sb, to: micIn) }
        }
    }

    private func appendAudio(_ sb: CMSampleBuffer, to input: AVAssetWriterInput?) {
        guard sessionStarted, let input, input.isReadyForMoreMediaData, let sb2 = retimed(sb) else { return }
        input.append(sb2)
    }

    private func gifFrame(_ sb: CMSampleBuffer) {
        guard let dir = gifDir, let px = sb.imageBuffer else { return }
        let t = CMTimeSubtract(sb.presentationTimeStamp, pauseOffset)
        if let last = gifFrames.last?.1, CMTimeSubtract(t, last) < gifInterval { return }
        guard let img = sharedCIContext.createCGImage(CIImage(cvPixelBuffer: px), from: CGRect(x: 0, y: 0, width: CVPixelBufferGetWidth(px), height: CVPixelBufferGetHeight(px))),
              let data = img.pngData() else { return }
        let url = dir.appendingPathComponent(String(format: "%06d.png", gifFrames.count))
        if (try? data.write(to: url)) != nil { gifFrames.append((url, t)) }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async { self.stop(error: error.localizedDescription) }
    }

    // MARK: control

    func togglePause() {
        if let at = pausedAt {
            pausedTotal += Date().timeIntervalSince(at)
            pausedAt = nil
            queue.async {
                self.pauseOffset = CMTimeAdd(self.pauseOffset, CMTimeSubtract(Recorder.hostNow, self.pauseStart))
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

    private func abort(_ error: String?) {
        countdown?.close()
        chrome.close()
        viz?.stop()
        Recorder.current = nil
        if let error { Toast.shared.show("Recording failed", error, ms: 6000) }
    }

    func discard() { stop(discard: true) }

    func stop(discard: Bool = false, error: String? = nil) {
        guard !finished else { return }
        finished = true
        let duration = elapsed
        chrome.close()
        viz?.stop()
        if pausedAt != nil { togglePause() }
        let st = stream
        Task { @MainActor in
            try? await st?.stopCapture()
            self.queue.async {
                self.stopTime = CMTimeSubtract(Recorder.hostNow, self.pauseOffset)
                if self.gif { self.finishGif(discard: discard || error != nil, duration: duration, error: error) }
                else { self.finishMp4(discard: discard || error != nil, duration: duration, error: error) }
            }
        }
    }

    private func finishMp4(discard: Bool, duration: TimeInterval, error: String?) {
        guard let w = writer, sessionStarted else {
            writer?.cancelWriting()
            return DispatchQueue.main.async { self.done(nil, duration: duration, error: error ?? (discard ? nil : "No frames were recorded.")) }
        }
        if discard {
            w.cancelWriting()
            try? FileManager.default.removeItem(at: tmpURL)
            return DispatchQueue.main.async { self.done(nil, duration: duration, error: error) }
        }
        [videoIn, audioIn, micIn].forEach { $0?.markAsFinished() }
        w.finishWriting {
            DispatchQueue.main.async {
                self.done(w.status == .completed ? self.tmpURL : nil, duration: duration, error: w.status == .completed ? nil : (w.error?.localizedDescription ?? "Couldn't write the video."))
            }
        }
    }

    private func finishGif(discard: Bool, duration: TimeInterval, error: String?) {
        defer { if let d = gifDir { try? FileManager.default.removeItem(at: d) } }
        guard !discard, !gifFrames.isEmpty,
              let dst = CGImageDestinationCreateWithURL(tmpURL as CFURL, UTType.gif.identifier as CFString, gifFrames.count, nil) else {
            return DispatchQueue.main.async { self.done(nil, duration: duration, error: error ?? (discard ? nil : "No frames were recorded.")) }
        }
        CGImageDestinationSetProperties(dst, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        for (i, f) in gifFrames.enumerated() {
            guard let img = CGImage.load(f.0) else { continue }
            let next = i + 1 < gifFrames.count ? gifFrames[i + 1].1 : stopTime
            let delay = max(0.02, CMTimeGetSeconds(CMTimeSubtract(next, f.1)))
            CGImageDestinationAddImage(dst, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFUnclampedDelayTime: delay, kCGImagePropertyGIFDelayTime: delay]] as CFDictionary)
        }
        let ok = CGImageDestinationFinalize(dst)
        DispatchQueue.main.async { self.done(ok ? self.tmpURL : nil, duration: duration, error: ok ? nil : "Couldn't write the GIF.") }
    }

    private func done(_ tmp: URL?, duration: TimeInterval, error: String?) {
        Recorder.current = nil
        AppDelegate.shared?.recordingChanged()
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
        Toast.shared.show(gif ? "GIF saved and copied" : "Video saved and copied", "\(url.lastPathComponent)  ·  \(formatTime(duration))  ·  \(size)",
                          ms: Settings.shared.int("ToastMs") + 2500) { Output.open(url) }
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
