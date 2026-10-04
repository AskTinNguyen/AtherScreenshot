import AVFoundation
import AppKit
import Carbon.HIToolbox
import ImageIO
import Speech
import UniformTypeIdentifiers

// A small video editor for screen recordings: trim, crop, speed, mute and captions
// (typed or transcribed on device), saved as a new MP4 or GIF next to the original.

struct Caption: Equatable {
    var id = UUID()
    var start: Double          // seconds in the source video
    var end: Double
    var text: String
    var position = CaptionPosition.bottom
}

enum CaptionPosition: Int, CaseIterable {
    case bottom, middle, top
    var label: String { ["Bottom", "Middle", "Top"][rawValue] }
}

struct VideoEdit: Equatable {
    var trimStart: Double = 0
    var trimEnd: Double
    var crop: CGRect?          // in video pixels, top-left origin
    var speed: Double = 1
    var muted = false
    var captions: [Caption] = []
    var captionSize = 1        // small, medium, large

    static let speeds: [Double] = [0.5, 1, 1.5, 2, 4]
    static let captionScale: [CGFloat] = [0.034, 0.045, 0.06]

    var outputDuration: Double { max(0, trimEnd - trimStart) / speed }
}

// MARK: - Export

enum VideoExport {
    enum Failure: LocalizedError {
        case noVideo, failed(String)
        var errorDescription: String? {
            switch self {
            case .noVideo: return "This file has no video track."
            case .failed(let s): return s
            }
        }
    }

    struct Prepared {
        let composition: AVMutableComposition
        let video: AVMutableVideoComposition
        let size: CGSize
    }

    static func displaySize(_ asset: AVAsset) async throws -> CGSize {
        guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.noVideo }
        let (n, t) = try await vt.load(.naturalSize, .preferredTransform)
        let r = CGRect(origin: .zero, size: n).applying(t)
        return CGSize(width: abs(r.width), height: abs(r.height))
    }

    static func prepare(_ asset: AVAsset, _ e: VideoEdit) async throws -> Prepared {
        guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.noVideo }
        let comp = AVMutableComposition()
        let range = CMTimeRange(start: CMTime(seconds: e.trimStart, preferredTimescale: 600), end: CMTime(seconds: e.trimEnd, preferredTimescale: 600))
        guard let cv = comp.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid) else { throw Failure.noVideo }
        try cv.insertTimeRange(range, of: vt, at: .zero)
        if !e.muted {
            for at in try await asset.loadTracks(withMediaType: .audio) {
                let ca = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid)
                try ca?.insertTimeRange(range, of: at, at: .zero)
            }
        }
        if e.speed != 1 {
            comp.scaleTimeRange(CMTimeRange(start: .zero, duration: range.duration), toDuration: CMTimeMultiplyByFloat64(range.duration, multiplier: 1 / e.speed))
        }
        let (natural, t, fps) = try await vt.load(.naturalSize, .preferredTransform, .nominalFrameRate)
        let shown = CGRect(origin: .zero, size: natural).applying(t)
        let full = CGRect(x: 0, y: 0, width: abs(shown.width), height: abs(shown.height))
        var crop = (e.crop ?? full).intersection(full).integral
        if crop.isNull || crop.width < 16 || crop.height < 16 { crop = full }
        let size = CGSize(width: floor(crop.width / 2) * 2, height: floor(crop.height / 2) * 2)   // H.264 wants even sizes

        let vc = AVMutableVideoComposition()
        vc.renderSize = size
        vc.frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps > 1 ? min(60, fps.rounded()) : 30))
        let ins = AVMutableVideoCompositionInstruction()
        ins.timeRange = CMTimeRange(start: .zero, duration: comp.duration)
        let li = AVMutableVideoCompositionLayerInstruction(assetTrack: cv)
        li.setTransform(t.concatenating(CGAffineTransform(translationX: -shown.minX - crop.minX, y: -shown.minY - crop.minY)), at: .zero)
        ins.layerInstructions = [li]
        vc.instructions = [ins]

        let caps = e.captions.filter { $0.end > e.trimStart && $0.start < e.trimEnd && !$0.text.trimmingCharacters(in: .whitespaces).isEmpty }
        if !caps.isEmpty {
            // Built off the main thread: an explicit transaction makes sure the layers are committed.
            CATransaction.begin()
            defer { CATransaction.commit() }
            let parent = CALayer(), videoLayer = CALayer()
            parent.frame = CGRect(origin: .zero, size: size)
            parent.isGeometryFlipped = true
            videoLayer.frame = parent.frame
            parent.addSublayer(videoLayer)
            for c in caps {
                let l = captionLayer(c.text, position: c.position, in: size, scale: VideoEdit.captionScale[e.captionSize])
                // One keyframe track over the whole video: hidden, shown for the caption's time, hidden.
                let total = max(0.1, comp.duration.seconds)
                let on = max(0, c.start - e.trimStart) / e.speed, off = min(total, (min(c.end, e.trimEnd) - e.trimStart) / e.speed)
                l.opacity = 0
                let a = CAKeyframeAnimation(keyPath: "opacity")
                a.values = [0, 1, 0]
                a.keyTimes = [0, NSNumber(value: on / total), NSNumber(value: off / total), 1]   // discrete: one more time than values
                a.calculationMode = .discrete
                a.beginTime = AVCoreAnimationBeginTimeAtZero
                a.duration = total
                a.isRemovedOnCompletion = false
                a.fillMode = .both
                l.add(a, forKey: "show")
                parent.addSublayer(l)
            }
            vc.animationTool = AVVideoCompositionCoreAnimationTool(postProcessingAsVideoLayer: videoLayer, in: parent)
        }
        return Prepared(composition: comp, video: vc, size: size)
    }

    // A caption pill: white text on a translucent dark background.
    static func captionLayer(_ text: String, position: CaptionPosition, in size: CGSize, scale: CGFloat) -> CALayer {
        let fontSize = max(14, size.height * scale)
        let attrs = captionAttributes(fontSize)
        let maxW = size.width * 0.86
        let bounds = NSAttributedString(string: text, attributes: attrs).boundingRect(with: CGSize(width: maxW, height: 10_000), options: [.usesLineFragmentOrigin])
        let pad = fontSize * 0.45
        let w = ceil(bounds.width) + pad * 2, h = ceil(bounds.height) + pad * 1.2
        let margin = size.height * 0.06
        let y: CGFloat = position == .top ? margin : position == .middle ? (size.height - h) / 2 : size.height - margin - h
        let box = CALayer()
        box.frame = CGRect(x: (size.width - w) / 2, y: y, width: w, height: h)
        box.backgroundColor = NSColor.black.withAlphaComponent(0.62).cgColor
        box.cornerRadius = fontSize * 0.35
        let tl = CATextLayer()
        tl.string = NSAttributedString(string: text, attributes: attrs)
        tl.isWrapped = true
        tl.alignmentMode = .center
        tl.contentsScale = 2
        tl.frame = CGRect(x: pad, y: pad * 0.6, width: w - pad * 2, height: ceil(bounds.height) + 2)
        box.addSublayer(tl)
        return box
    }

    static func captionAttributes(_ size: CGFloat) -> [NSAttributedString.Key: Any] {
        let p = NSMutableParagraphStyle()
        p.alignment = .center
        return [.font: NSFont.systemFont(ofSize: size, weight: .semibold), .foregroundColor: NSColor.white, .paragraphStyle: p]
    }

    static func run(_ s: AVAssetExportSession) async throws {
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in s.exportAsynchronously { k.resume() } }
        if s.status != .completed { throw Failure.failed(s.error?.localizedDescription ?? "Export failed.") }
    }

    static func mp4(_ asset: AVAsset, _ e: VideoEdit, to url: URL) async throws {
        let p = try await prepare(asset, e)
        guard let s = AVAssetExportSession(asset: p.composition, presetName: AVAssetExportPresetHighestQuality) else { throw Failure.failed("Can't export this video.") }
        s.videoComposition = p.video
        s.audioTimePitchAlgorithm = .spectral   // sped-up audio keeps its pitch
        s.outputURL = url
        s.outputFileType = .mp4
        s.shouldOptimizeForNetworkUse = true
        try await run(s)
    }

    // Renders the edit to MP4 first, then samples it into a GIF (≤ 960 px wide).
    static func gif(_ asset: AVAsset, _ e: VideoEdit, to url: URL, fps: Double = 12) async throws {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("ather-\(UUID().uuidString).mp4")
        defer { try? FileManager.default.removeItem(at: tmp) }
        try await mp4(asset, e, to: tmp)
        let src = AVURLAsset(url: tmp)
        let duration = try await src.load(.duration).seconds
        let gen = AVAssetImageGenerator(asset: src)
        gen.appliesPreferredTrackTransform = true
        gen.maximumSize = CGSize(width: 960, height: 960)
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = CMTime(seconds: 0.5 / fps, preferredTimescale: 600)
        let n = max(1, Int(duration * fps))
        guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.gif.identifier as CFString, n, nil) else { throw Failure.failed("Can't write the GIF.") }
        CGImageDestinationSetProperties(dest, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        let frame = [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: 1 / fps]] as CFDictionary
        for i in 0..<n {
            let img = try await gen.image(at: CMTime(seconds: Double(i) / fps, preferredTimescale: 600)).image
            CGImageDestinationAddImage(dest, img, frame)
        }
        guard CGImageDestinationFinalize(dest) else { throw Failure.failed("Can't write the GIF.") }
    }

    // Speech in the trimmed range, as caption-sized chunks. On device when the Mac supports it.
    static func transcribe(_ asset: AVAsset, from start: Double, to end: Double) async throws -> [Caption] {
        let status = await withCheckedContinuation { k in SFSpeechRecognizer.requestAuthorization { k.resume(returning: $0) } }
        guard status == .authorized else {
            throw Failure.failed("Allow Ather Screenshot in System Settings › Privacy & Security › Speech Recognition.")
        }
        guard let rec = SFSpeechRecognizer(locale: Locale.current) ?? SFSpeechRecognizer(locale: Locale(identifier: "en-US")), rec.isAvailable else {
            throw Failure.failed("Speech recognition isn't available for your language.")
        }
        let tracks = try await asset.loadTracks(withMediaType: .audio)
        guard !tracks.isEmpty else { throw Failure.failed("This recording has no audio. Turn on system audio or the microphone in Settings › Recording.") }
        // Mix the audio tracks of the trimmed range into one file.
        let comp = AVMutableComposition()
        let range = CMTimeRange(start: CMTime(seconds: start, preferredTimescale: 600), end: CMTime(seconds: end, preferredTimescale: 600))
        for t in tracks { try comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid)?.insertTimeRange(range, of: t, at: .zero) }
        let audio = FileManager.default.temporaryDirectory.appendingPathComponent("ather-\(UUID().uuidString).m4a")
        defer { try? FileManager.default.removeItem(at: audio) }
        guard let s = AVAssetExportSession(asset: comp, presetName: AVAssetExportPresetAppleM4A) else { throw Failure.failed("Can't read the audio.") }
        s.outputURL = audio
        s.outputFileType = .m4a
        try await run(s)

        let req = SFSpeechURLRecognitionRequest(url: audio)
        req.shouldReportPartialResults = false
        req.addsPunctuation = true
        if rec.supportsOnDeviceRecognition { req.requiresOnDeviceRecognition = true }
        let segments: [SFTranscriptionSegment] = try await withCheckedThrowingContinuation { k in
            var done = false
            rec.recognitionTask(with: req) { result, error in
                guard !done else { return }
                if let result, result.isFinal { done = true; k.resume(returning: result.bestTranscription.segments) }
                else if let error { done = true; k.resume(throwing: Failure.failed("Couldn't transcribe: \(error.localizedDescription)")) }
            }
        }
        return chunk(segments.map { (start + $0.timestamp, start + $0.timestamp + $0.duration, $0.substring) })
    }

    // Groups words into short captions: a new one after a pause, ~42 characters, or 3.5 seconds.
    static func chunk(_ words: [(Double, Double, String)]) -> [Caption] {
        var out: [Caption] = []
        var cur: Caption?
        for (s, e, w) in words {
            if var c = cur, s - c.end < 0.7, c.text.count + w.count < 42, e - c.start < 3.5 {
                c.text += " " + w
                c.end = e
                cur = c
            } else {
                if let c = cur { out.append(c) }
                cur = Caption(start: s, end: max(e, s + 0.4), text: w)
            }
        }
        if let c = cur { out.append(c) }
        // Keep each caption on screen until the next one, up to a short hold.
        for i in out.indices {
            let next = i + 1 < out.count ? out[i + 1].start : out[i].end + 0.8
            out[i].end = min(next, out[i].end + 0.8)
        }
        return out
    }
}

// MARK: - Window

final class VideoEditor: NSObject, NSWindowDelegate {
    static var instances: [VideoEditor] = []

    let url: URL
    let asset: AVURLAsset
    let player: AVPlayer
    let window: NSWindow
    var duration: Double = 0
    var videoSize = CGSize(width: 16, height: 9)
    var edit = VideoEdit(trimEnd: 0) { didSet { if edit != oldValue { changed() } } }
    var selected: UUID? { didSet { syncCaptionBar(); timeline.needsDisplay = true; stage.needsDisplay = true } }
    var cropping = false { didSet { stage.needsDisplay = true; syncToolbar() } }
    var dirty = false
    private var undoStack: [VideoEdit] = []
    private var timeObserver: Any?
    private var busy = false

    private let stage = VideoStage()
    private let timeline = Timeline()
    private let playButton = NSButton()
    private let timeLabel = NSTextField(labelWithString: "")
    private let speedPopup = NSPopUpButton()
    private let sizePopup = NSPopUpButton()
    private let aspectPopup = NSPopUpButton()
    private let muteButton = NSButton()
    private let cropButton = NSButton()
    private let captionField = NSTextField()
    private let positionPopup = NSPopUpButton()
    private let captionBar = NSStackView()
    private let hint = NSTextField(labelWithString: "")
    private var actions: [MenuAction] = []

    static func open(_ url: URL) {
        if let e = instances.first(where: { $0.url == url }) { activateApp(); e.window.makeKeyAndOrderFront(nil); return }
        instances.append(VideoEditor(url))
    }

    private init(_ url: URL) {
        self.url = url
        asset = AVURLAsset(url: url)
        player = AVPlayer(playerItem: AVPlayerItem(asset: asset))
        let vis = Geo.mouseScreen.visibleFrame
        let size = NSSize(width: min(1180, vis.width * 0.9), height: min(820, vis.height * 0.9))
        window = NSWindow(contentRect: NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = "Edit video — \(url.lastPathComponent)"
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 900, height: 560)
        window.delegate = self
        stage.editor = self
        timeline.editor = self
        stage.playerLayer.player = player
        build()
        AppDelegate.shared?.windowOpened(window)
        activateApp()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(stage)
        Task { @MainActor in await self.load() }
    }

    private func load() async {
        do {
            duration = try await asset.load(.duration).seconds
            videoSize = try await VideoExport.displaySize(asset)
        } catch {
            Toast.shared.show("Can't open this video", error.localizedDescription)
            return window.close()
        }
        edit = VideoEdit(trimEnd: duration)
        undoStack = []
        dirty = false
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 30), queue: .main) { [weak self] t in self?.tick(t.seconds) }
        timeline.loadThumbnails()
        changed()
    }

    // MARK: layout

    private func build() {
        let root = NSView()
        let top = NSStackView()
        top.orientation = .horizontal
        top.spacing = 8
        top.edgeInsets = NSEdgeInsets(top: 0, left: 14, bottom: 0, right: 14)

        cropButton.title = "Crop"
        cropButton.image = NSImage(systemSymbolName: "crop", accessibilityDescription: "Crop")
        cropButton.imagePosition = .imageLeading
        cropButton.contentTintColor = Theme.text
        cropButton.attributedTitle = NSAttributedString(string: cropButton.title, attributes: [.foregroundColor: Theme.text, .font: Theme.font(12)])
        cropButton.bezelStyle = .recessed
        cropButton.setButtonType(.pushOnPushOff)
        cropButton.toolTip = "Crop (C): drag on the video"
        bind(cropButton) { [weak self] in self?.cropping.toggle() }
        aspectPopup.addItems(withTitles: ["Free", "16:9", "4:3", "1:1", "9:16"])
        aspectPopup.bezelStyle = .recessed
        aspectPopup.toolTip = "Crop shape"
        bind(aspectPopup) { [weak self] in self?.applyAspect() }

        speedPopup.addItems(withTitles: VideoEdit.speeds.map { $0 == 1 ? "Speed 1×" : "Speed \(String(format: "%g", $0))×" })
        speedPopup.bezelStyle = .recessed
        bind(speedPopup) { [weak self] in
            guard let self else { return }
            self.pushUndo()
            self.edit.speed = VideoEdit.speeds[self.speedPopup.indexOfSelectedItem]
            if self.player.rate != 0 { self.player.rate = Float(self.edit.speed) }
        }
        muteButton.bezelStyle = .recessed
        muteButton.setButtonType(.pushOnPushOff)
        muteButton.title = "Mute"
        muteButton.image = NSImage(systemSymbolName: "speaker.slash", accessibilityDescription: "Mute")
        muteButton.imagePosition = .imageLeading
        muteButton.contentTintColor = Theme.text
        muteButton.attributedTitle = NSAttributedString(string: muteButton.title, attributes: [.foregroundColor: Theme.text, .font: Theme.font(12)])
        bind(muteButton) { [weak self] in guard let self else { return }; self.pushUndo(); self.edit.muted.toggle() }

        let addCaption = barButton("Caption", "captions.bubble", "Add a caption at the playhead (T)") { [weak self] in self?.addCaption() }
        let auto = barButton("Auto captions", "waveform.badge.mic", "Transcribe speech into captions, on this Mac") { [weak self] in self?.autoCaptions() }
        sizePopup.addItems(withTitles: ["Small text", "Medium text", "Large text"])
        sizePopup.bezelStyle = .recessed
        bind(sizePopup) { [weak self] in guard let self else { return }; self.pushUndo(); self.edit.captionSize = self.sizePopup.indexOfSelectedItem }

        let gif = barButton("Save GIF", "photo.stack", "Save as a GIF (⌘⇧S)") { [weak self] in self?.save(gif: true) }
        let save = NSButton(title: "Save", target: nil, action: nil)
        save.bezelStyle = .rounded
        save.isBordered = false
        save.wantsLayer = true
        save.layer?.backgroundColor = Theme.accent.cgColor
        save.layer?.cornerRadius = 7
        save.attributedTitle = NSAttributedString(string: "Save", attributes: [.foregroundColor: Theme.onAccent, .font: Theme.font(13, .semibold)])
        save.toolTip = "Save as a new MP4 in your captures (⌘S)"
        save.widthAnchor.constraint(equalToConstant: 64).isActive = true
        save.heightAnchor.constraint(equalToConstant: 28).isActive = true
        bind(save) { [weak self] in self?.save(gif: false) }

        let spacer = NSView()
        spacer.setContentHuggingPriority(.init(1), for: .horizontal)
        for v in [cropButton, aspectPopup, separator(), speedPopup, muteButton, separator(), addCaption, auto, sizePopup, spacer, gif, save] as [NSView] { top.addArrangedSubview(v) }

        // Caption editing row, shown while a caption is selected.
        captionField.placeholderString = "Caption text"
        captionField.font = Theme.font(13)
        captionField.delegate = self
        captionField.widthAnchor.constraint(greaterThanOrEqualToConstant: 380).isActive = true
        positionPopup.addItems(withTitles: CaptionPosition.allCases.map(\.label))
        positionPopup.bezelStyle = .recessed
        bind(positionPopup) { [weak self] in
            guard let self, let i = self.selectedIndex else { return }
            self.pushUndo()
            self.edit.captions[i].position = CaptionPosition(rawValue: self.positionPopup.indexOfSelectedItem) ?? .bottom
        }
        let del = barButton("Delete", "trash", "Delete caption (⌫)") { [weak self] in self?.deleteCaption() }
        for v in [captionField, positionPopup, del] as [NSView] { captionBar.addArrangedSubview(v) }
        captionBar.orientation = .horizontal
        captionBar.spacing = 8
        hint.font = Theme.font(11)
        hint.textColor = Theme.muted
        hint.stringValue = "Space plays · I and O set the start and end · T adds a caption · C crops · ⌘Z undoes"

        playButton.bezelStyle = .regularSquare
        playButton.isBordered = false
        playButton.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: "Play")
        playButton.symbolConfiguration = .init(pointSize: 16, weight: .semibold)
        playButton.contentTintColor = Theme.text
        playButton.toolTip = "Play / pause (Space)"
        bind(playButton) { [weak self] in self?.togglePlay() }
        timeLabel.font = Theme.mono(11)
        timeLabel.textColor = Theme.textDim

        for v in [top, stage, captionBar, hint, playButton, timeLabel, timeline] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        NSLayoutConstraint.activate([
            top.topAnchor.constraint(equalTo: root.topAnchor, constant: 32),
            top.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            top.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            top.heightAnchor.constraint(equalToConstant: 34),
            stage.topAnchor.constraint(equalTo: top.bottomAnchor, constant: 8),
            stage.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 14),
            stage.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            stage.bottomAnchor.constraint(equalTo: captionBar.topAnchor, constant: -8),
            captionBar.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            captionBar.bottomAnchor.constraint(equalTo: timeline.topAnchor, constant: -8),
            captionBar.heightAnchor.constraint(equalToConstant: 26),
            hint.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            hint.centerYAnchor.constraint(equalTo: captionBar.centerYAnchor),
            playButton.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 14),
            playButton.centerYAnchor.constraint(equalTo: timeline.topAnchor, constant: 26),
            playButton.widthAnchor.constraint(equalToConstant: 30),
            timeLabel.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 10),
            timeLabel.topAnchor.constraint(equalTo: playButton.bottomAnchor, constant: 10),
            timeline.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 100),
            timeline.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            timeline.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -14),
            timeline.heightAnchor.constraint(equalToConstant: 92),
        ])
        window.contentView = root
        syncCaptionBar()
        syncToolbar()
    }

    private func bind(_ c: NSControl, _ run: @escaping () -> Void) {
        let a = MenuAction(run)
        actions.append(a)
        c.target = a
        c.action = #selector(MenuAction.fire)
    }

    private func barButton(_ title: String, _ symbol: String, _ tip: String, _ run: @escaping () -> Void) -> NSButton {
        let b = NSButton(title: title, image: NSImage(systemSymbolName: symbol, accessibilityDescription: title) ?? NSImage(), target: nil, action: nil)
        b.bezelStyle = .recessed
        b.imagePosition = .imageLeading
        b.toolTip = tip
        b.contentTintColor = Theme.text
        b.attributedTitle = NSAttributedString(string: title, attributes: [.foregroundColor: Theme.text, .font: Theme.font(12)])
        bind(b, run)
        return b
    }

    private func separator() -> NSView {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.border.cgColor
        v.widthAnchor.constraint(equalToConstant: 1).isActive = true
        v.heightAnchor.constraint(equalToConstant: 20).isActive = true
        return v
    }

    // MARK: state

    var now: Double { player.currentTime().seconds.isFinite ? player.currentTime().seconds : 0 }
    var selectedIndex: Int? { selected.flatMap { id in edit.captions.firstIndex { $0.id == id } } }

    func pushUndo() {
        undoStack.append(edit)
        if undoStack.count > 200 { undoStack.removeFirst() }
        dirty = true
    }

    func undo() {
        guard let e = undoStack.popLast() else { return }
        edit = e
        if let s = selected, !edit.captions.contains(where: { $0.id == s }) { selected = nil }
    }

    private func changed() {
        player.isMuted = edit.muted
        stage.needsDisplay = true
        timeline.needsDisplay = true
        syncToolbar()
        syncCaptionBar()
        updateTime()
    }

    private func syncToolbar() {
        cropButton.state = cropping ? .on : .off
        aspectPopup.isHidden = !cropping
        muteButton.state = edit.muted ? .on : .off
        speedPopup.selectItem(at: VideoEdit.speeds.firstIndex(of: edit.speed) ?? 1)
        sizePopup.selectItem(at: edit.captionSize)
    }

    private func syncCaptionBar() {
        let i = selectedIndex
        captionBar.isHidden = i == nil
        hint.isHidden = i != nil
        if let i {
            if window.firstResponder !== captionField.currentEditor() { captionField.stringValue = edit.captions[i].text }
            positionPopup.selectItem(at: edit.captions[i].position.rawValue)
        }
    }

    private func updateTime() {
        let out = edit.outputDuration
        timeLabel.stringValue = "\(clock(now))\n\(clock(out)) out"
        timeLabel.maximumNumberOfLines = 2
    }

    func clock(_ t: Double) -> String {
        let s = max(0, t)
        return String(format: "%d:%02d.%d", Int(s) / 60, Int(s) % 60, Int((s * 10).truncatingRemainder(dividingBy: 10)))
    }

    // MARK: playback

    private func tick(_ t: Double) {
        if player.rate != 0 && t >= edit.trimEnd - 0.01 {
            player.pause()
            seek(edit.trimStart)
            playButton.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: "Play")
        }
        timeline.needsDisplay = true
        stage.needsDisplay = true
        updateTime()
    }

    func togglePlay() {
        if player.rate != 0 {
            player.pause()
        } else {
            if now < edit.trimStart || now >= edit.trimEnd - 0.05 { seek(edit.trimStart) }
            player.rate = Float(edit.speed)
        }
        playButton.image = NSImage(systemSymbolName: player.rate != 0 ? "pause.fill" : "play.fill", accessibilityDescription: "Play")
    }

    func seek(_ t: Double) {
        player.seek(to: CMTime(seconds: min(max(0, t), duration), preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
        timeline.needsDisplay = true
        stage.needsDisplay = true
        updateTime()
    }

    func step(_ frames: Double) {
        player.pause()
        seek(now + frames / 30)
    }

    // MARK: editing

    func setTrim(start: Double? = nil, end: Double? = nil) {
        var e = edit
        if let start { e.trimStart = min(max(0, start), e.trimEnd - 0.1) }
        if let end { e.trimEnd = max(min(duration, end), e.trimStart + 0.1) }
        edit = e
    }

    func addCaption() {
        pushUndo()
        let start = min(max(now, edit.trimStart), max(edit.trimStart, edit.trimEnd - 0.5))
        let c = Caption(start: start, end: min(edit.trimEnd, start + 3), text: "")
        edit.captions.append(c)
        edit.captions.sort { $0.start < $1.start }
        selected = c.id
        window.makeFirstResponder(captionField)
    }

    func deleteCaption() {
        guard let i = selectedIndex else { return }
        pushUndo()
        edit.captions.remove(at: i)
        selected = nil
        window.makeFirstResponder(stage)
    }

    func setCrop(_ r: CGRect?) {
        let full = CGRect(origin: .zero, size: videoSize)
        edit.crop = r.map { $0.intersection(full).integral }.flatMap { $0.width >= 16 && $0.height >= 16 && $0 != full ? $0 : nil }
    }

    var aspect: CGFloat? { ([nil, 16.0 / 9, 4.0 / 3, 1, 9.0 / 16] as [CGFloat?])[max(0, aspectPopup.indexOfSelectedItem)] }

    private func applyAspect() {
        guard let a = aspect else { return }
        pushUndo()
        let w = min(videoSize.width, videoSize.height * a), h = w / a
        setCrop(CGRect(x: (videoSize.width - w) / 2, y: (videoSize.height - h) / 2, width: w, height: h))
    }

    private func autoCaptions() {
        guard !busy else { return }
        busy = true
        Toast.shared.show("Transcribing…", "Turning speech into captions on this Mac", ms: 120_000)
        let (a, s, e) = (asset, edit.trimStart, edit.trimEnd)
        Task { @MainActor in
            defer { self.busy = false }
            do {
                let caps = try await VideoExport.transcribe(a, from: s, to: e)
                Toast.shared.hide()
                guard !caps.isEmpty else { return Toast.shared.show("No speech found", "Add captions by hand with T.") }
                self.pushUndo()
                self.edit.captions = (self.edit.captions.filter { !$0.text.isEmpty } + caps).sorted { $0.start < $1.start }
                Toast.shared.show("Added \(caps.count) caption\(caps.count == 1 ? "" : "s")", "Click one on the timeline to edit or move it.")
            } catch {
                Toast.shared.show("Auto captions failed", error.localizedDescription, ms: 8000)
            }
        }
    }

    func save(gif: Bool) {
        guard !busy else { return }
        busy = true
        player.pause()
        let src = Library.shared.meta(url)
        let info = NameInfo(app: src.app, window: src.window)
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("ather-\(UUID().uuidString).\(gif ? "gif" : "mp4")")
        Toast.shared.show(gif ? "Saving GIF…" : "Saving video…", "\(clock(edit.outputDuration)) long", ms: 600_000)
        let (a, e) = (asset, edit)
        Task { @MainActor in
            defer { self.busy = false }
            do {
                if gif { try await VideoExport.gif(a, e, to: tmp) } else { try await VideoExport.mp4(a, e, to: tmp) }
                let out = Output.newCaptureURL(ext: gif ? "gif" : "mp4", info: info)
                Library.shared.noteEdit(out, from: self.url, info: info, edited: true)
                try FileManager.default.moveItem(at: tmp, to: out)
                self.dirty = false
                let size = ByteCountFormatter.string(fromByteCount: Int64((try? out.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0), countStyle: .file)
                Toast.shared.show(gif ? "GIF saved" : "Video saved", "\(out.lastPathComponent)  ·  \(size)  ·  click to show in Finder") { Output.reveal(out) }
            } catch {
                try? FileManager.default.removeItem(at: tmp)
                Toast.shared.show("Save failed", error.localizedDescription, ms: 8000)
            }
        }
    }

    // MARK: keys

    func key(_ e: NSEvent) -> Bool {
        let cmd = e.modifierFlags.contains(.command), shift = e.modifierFlags.contains(.shift)
        let code = Int(e.keyCode)
        if cmd {
            switch code {
            case kVK_ANSI_S: save(gif: shift)
            case kVK_ANSI_Z: undo()
            case kVK_ANSI_W: window.performClose(nil)
            default: return false
            }
            return true
        }
        switch code {
        case kVK_Space: togglePlay()
        case kVK_LeftArrow: step(shift ? -30 : -1)
        case kVK_RightArrow: step(shift ? 30 : 1)
        case kVK_ANSI_I: pushUndo(); setTrim(start: now)
        case kVK_ANSI_O: pushUndo(); setTrim(end: now)
        case kVK_ANSI_T: addCaption()
        case kVK_ANSI_C: cropping.toggle()
        case kVK_Delete, kVK_ForwardDelete: deleteCaption()
        case kVK_Escape:
            if cropping { cropping = false } else if selected != nil { selected = nil } else { window.performClose(nil) }
        default: return false
        }
        return true
    }

    // MARK: window

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard dirty else { return true }
        let a = NSAlert()
        a.messageText = "Close the video editor and discard your changes?"
        a.addButton(withTitle: "Keep Editing")
        a.addButton(withTitle: "Discard")
        a.beginSheetModal(for: window) { r in
            if r == .alertSecondButtonReturn { self.dirty = false; self.window.close() }
        }
        return false
    }

    func windowWillClose(_ notification: Notification) {
        player.pause()
        if let timeObserver { player.removeTimeObserver(timeObserver) }
        VideoEditor.instances.removeAll { $0 === self }
    }
}

extension VideoEditor: NSTextFieldDelegate {
    func controlTextDidBeginEditing(_ obj: Notification) { pushUndo() }   // one undo step per editing session

    func controlTextDidChange(_ obj: Notification) {
        guard let i = selectedIndex else { return }
        edit.captions[i].text = captionField.stringValue   // live preview on the video
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy sel: Selector) -> Bool {
        if sel == #selector(NSResponder.insertNewline(_:)) || sel == #selector(NSResponder.cancelOperation(_:)) {
            window.makeFirstResponder(stage)
            return true
        }
        return false
    }
}

// MARK: - Stage (video, crop, caption preview)

final class VideoStage: NSView {
    weak var editor: VideoEditor?
    let playerLayer = AVPlayerLayer()
    private var dragFrom: CGPoint?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.cornerRadius = 8
        playerLayer.videoGravity = .resizeAspect
        layer?.addSublayer(playerLayer)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override var wantsUpdateLayer: Bool { false }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        playerLayer.frame = bounds
        CATransaction.commit()
    }

    // Where the video is drawn, in view coordinates.
    var videoRect: CGRect {
        guard let e = editor else { return bounds }
        let s = min(bounds.width / e.videoSize.width, bounds.height / e.videoSize.height)
        let w = e.videoSize.width * s, h = e.videoSize.height * s
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }
    private func toVideo(_ p: CGPoint) -> CGPoint {
        let r = videoRect, s = (editor?.videoSize.width ?? 1) / max(1, r.width)
        return CGPoint(x: (p.x - r.minX) * s, y: (p.y - r.minY) * s)
    }
    private func toView(_ r: CGRect) -> CGRect {
        let v = videoRect, s = v.width / max(1, editor?.videoSize.width ?? 1)
        return CGRect(x: v.minX + r.minX * s, y: v.minY + r.minY * s, width: r.width * s, height: r.height * s)
    }

    override func keyDown(with e: NSEvent) { if editor?.key(e) != true { super.keyDown(with: e) } }

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        guard let ed = editor else { return }
        if ed.cropping {
            ed.pushUndo()
            dragFrom = toVideo(convert(e.locationInWindow, from: nil))
        } else {
            ed.togglePlay()
        }
    }

    override func mouseDragged(with e: NSEvent) {
        guard let ed = editor, let a = dragFrom else { return }
        var b = toVideo(convert(e.locationInWindow, from: nil))
        if let k = ed.aspect {   // keep the chosen shape
            let w = abs(b.x - a.x), h = abs(b.y - a.y)
            let W = max(w, h * k), H = W / k
            b = CGPoint(x: a.x + (b.x < a.x ? -W : W), y: a.y + (b.y < a.y ? -H : H))
        }
        ed.setCrop(Geo.norm(a, b))
    }

    override func mouseUp(with e: NSEvent) { dragFrom = nil }

    override func draw(_ dirtyRect: NSRect) {
        guard let ed = editor, let ctx = NSGraphicsContext.current?.cgContext else { return }
        let v = videoRect
        if let c = ed.edit.crop {
            let r = toView(c)
            let p = CGMutablePath()
            p.addRect(v)
            p.addRect(r)
            ctx.addPath(p)
            ctx.setFillColor(NSColor.black.withAlphaComponent(ed.cropping ? 0.55 : 0.7).cgColor)
            ctx.fillPath(using: .evenOdd)
            ctx.setStrokeColor(Theme.accent.cgColor)
            ctx.setLineWidth(1.5)
            if !ed.cropping { ctx.setLineDash(phase: 0, lengths: [5, 4]) }
            ctx.stroke(r)
            ctx.setLineDash(phase: 0, lengths: [])
        } else if ed.cropping {
            let s = NSAttributedString(string: "Drag to crop", attributes: [.font: Theme.font(13, .semibold), .foregroundColor: NSColor.white])
            let sz = s.size()
            NSColor.black.withAlphaComponent(0.55).setFill()
            NSBezierPath(roundedRect: CGRect(x: v.midX - sz.width / 2 - 12, y: v.midY - sz.height / 2 - 6, width: sz.width + 24, height: sz.height + 12), xRadius: 8, yRadius: 8).fill()
            s.draw(at: CGPoint(x: v.midX - sz.width / 2, y: v.midY - sz.height / 2))
        }
        // Captions showing at the playhead, as they'll look in the export.
        let t = ed.now
        let area = ed.edit.crop.map(toView) ?? v
        let scale = area.height / max(1, (ed.edit.crop ?? CGRect(origin: .zero, size: ed.videoSize)).height)
        for c in ed.edit.captions where t >= c.start && t < c.end {
            let text = c.text.isEmpty ? "Type a caption…" : c.text
            let full = (ed.edit.crop ?? CGRect(origin: .zero, size: ed.videoSize)).size
            let l = VideoExport.captionLayer(text, position: c.position, in: full, scale: VideoEdit.captionScale[ed.edit.captionSize])
            let f = l.frame
            let box = CGRect(x: area.minX + f.minX * scale, y: area.minY + f.minY * scale, width: f.width * scale, height: f.height * scale)
            NSColor.black.withAlphaComponent(0.62).setFill()
            NSBezierPath(roundedRect: box, xRadius: l.cornerRadius * scale, yRadius: l.cornerRadius * scale).fill()
            let fs = max(14, full.height * VideoEdit.captionScale[ed.edit.captionSize]) * scale
            var attrs = VideoExport.captionAttributes(fs)
            if c.text.isEmpty { attrs[.foregroundColor] = NSColor.white.withAlphaComponent(0.5) }
            let pad = max(14, full.height * VideoEdit.captionScale[ed.edit.captionSize]) * 0.45 * scale
            NSAttributedString(string: text, attributes: attrs).draw(with: box.insetBy(dx: pad, dy: pad * 0.6), options: [.usesLineFragmentOrigin])
            if c.id == ed.selected {
                Theme.accent.setStroke()
                let ring = NSBezierPath(roundedRect: box.insetBy(dx: -3, dy: -3), xRadius: 8, yRadius: 8)
                ring.lineWidth = 1.5
                ring.stroke()
            }
        }
    }
}

// MARK: - Timeline (thumbnails, trim handles, playhead, caption lane)

final class Timeline: NSView {
    weak var editor: VideoEditor?
    private var thumbs: [CGImage] = []
    private enum Drag { case start, end, playhead, caption(UUID, edge: Int, grab: Double, orig: Caption) }
    private var drag: Drag?

    private let stripH: CGFloat = 52
    private let laneY: CGFloat = 62
    private let laneH: CGFloat = 26

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with e: NSEvent) { if editor?.key(e) != true { super.keyDown(with: e) } }

    func loadThumbnails() {
        guard let e = editor, e.duration > 0 else { return }
        let gen = AVAssetImageGenerator(asset: e.asset)
        gen.appliesPreferredTrackTransform = true
        gen.maximumSize = CGSize(width: 240, height: 240)
        let count = 16
        let times = (0..<count).map { CMTime(seconds: e.duration * (Double($0) + 0.5) / Double(count), preferredTimescale: 600) }
        Task { @MainActor in
            var out: [CGImage] = []
            for t in times { if let img = try? await gen.image(at: t).image { out.append(img) } }
            self.thumbs = out
            self.needsDisplay = true
        }
    }

    private func x(_ t: Double) -> CGFloat { bounds.width * CGFloat(t / max(0.001, editor?.duration ?? 1)) }
    private func t(_ x: CGFloat) -> Double { Double(min(max(0, x), bounds.width) / max(1, bounds.width)) * (editor?.duration ?? 0) }

    override func mouseDown(with ev: NSEvent) {
        window?.makeFirstResponder(self)
        guard let e = editor else { return }
        let p = convert(ev.locationInWindow, from: nil)
        if p.y >= laneY {   // caption lane
            if let c = e.edit.captions.last(where: { x($0.start) - 4 <= p.x && p.x <= x($0.end) + 4 }) {
                e.selected = c.id
                let edge = abs(p.x - x(c.start)) < 6 ? -1 : abs(p.x - x(c.end)) < 6 ? 1 : 0
                e.pushUndo()
                drag = .caption(c.id, edge: edge, grab: t(p.x), orig: c)
                if ev.clickCount == 2 { e.window.makeFirstResponder(nil); e.seek(c.start) }
            } else {
                e.selected = nil
                e.seek(t(p.x))
            }
            return
        }
        if abs(p.x - x(e.edit.trimStart)) < 8 { e.pushUndo(); drag = .start; return }
        if abs(p.x - x(e.edit.trimEnd)) < 8 { e.pushUndo(); drag = .end; return }
        drag = .playhead
        e.player.pause()
        e.seek(t(p.x))
    }

    override func mouseDragged(with ev: NSEvent) {
        guard let e = editor, let d = drag else { return }
        let p = convert(ev.locationInWindow, from: nil), now = t(p.x)
        switch d {
        case .start: e.setTrim(start: now); e.seek(e.edit.trimStart)
        case .end: e.setTrim(end: now); e.seek(e.edit.trimEnd)
        case .playhead: e.seek(now)
        case .caption(let id, let edge, let grab, let o):
            guard let i = e.edit.captions.firstIndex(where: { $0.id == id }) else { return }
            var c = o
            switch edge {
            case -1: c.start = min(now, o.end - 0.2)
            case 1: c.end = max(now, o.start + 0.2)
            default:
                let len = o.end - o.start
                c.start = min(max(0, o.start + now - grab), e.duration - len)
                c.end = c.start + len
            }
            e.edit.captions[i] = c
        }
        needsDisplay = true
    }

    override func mouseUp(with ev: NSEvent) {
        if case .caption = drag, let e = editor { e.edit.captions.sort { $0.start < $1.start } }
        drag = nil
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let e = editor, let ctx = NSGraphicsContext.current?.cgContext else { return }
        let strip = CGRect(x: 0, y: 0, width: bounds.width, height: stripH)
        ctx.saveGState()
        ctx.addPath(CGPath(roundedRect: strip, cornerWidth: 6, cornerHeight: 6, transform: nil))
        ctx.clip()
        Theme.surface.setFill()
        strip.fill()
        if !thumbs.isEmpty {
            let w = strip.width / CGFloat(thumbs.count)
            for (i, img) in thumbs.enumerated() {
                let cell = CGRect(x: CGFloat(i) * w, y: 0, width: w, height: stripH)
                let s = max(cell.width / CGFloat(img.width), cell.height / CGFloat(img.height))
                let dw = CGFloat(img.width) * s, dh = CGFloat(img.height) * s
                ctx.saveGState()
                ctx.clip(to: cell)
                drawImageFlipped(ctx, img, in: CGRect(x: cell.midX - dw / 2, y: cell.midY - dh / 2, width: dw, height: dh))
                ctx.restoreGState()
            }
        }
        // Outside the trim: dimmed.
        NSColor.black.withAlphaComponent(0.65).setFill()
        CGRect(x: 0, y: 0, width: x(e.edit.trimStart), height: stripH).fill()
        CGRect(x: x(e.edit.trimEnd), y: 0, width: bounds.width - x(e.edit.trimEnd), height: stripH).fill()
        ctx.restoreGState()

        // Trim frame and handles.
        let kept = CGRect(x: x(e.edit.trimStart), y: 0, width: x(e.edit.trimEnd) - x(e.edit.trimStart), height: stripH)
        Theme.accent.setStroke()
        let frame = NSBezierPath(roundedRect: kept.insetBy(dx: 1, dy: 1), xRadius: 5, yRadius: 5)
        frame.lineWidth = 2.5
        frame.stroke()
        Theme.accent.setFill()
        for hx in [kept.minX, kept.maxX] {
            NSBezierPath(roundedRect: CGRect(x: hx - 4, y: 8, width: 8, height: stripH - 16), xRadius: 3, yRadius: 3).fill()
        }

        // Caption lane.
        let lane = CGRect(x: 0, y: laneY, width: bounds.width, height: laneH)
        NSColor.white.withAlphaComponent(0.04).setFill()
        NSBezierPath(roundedRect: lane, xRadius: 5, yRadius: 5).fill()
        if e.edit.captions.isEmpty {
            let s = NSAttributedString(string: "Captions appear here", attributes: [.font: Theme.font(10), .foregroundColor: Theme.muted])
            s.draw(at: CGPoint(x: 8, y: lane.midY - s.size().height / 2))
        }
        for c in e.edit.captions {
            let r = CGRect(x: x(c.start), y: laneY + 2, width: max(6, x(c.end) - x(c.start)), height: laneH - 4)
            let sel = c.id == e.selected
            (sel ? Theme.accent : NSColor.white.withAlphaComponent(0.18)).setFill()
            NSBezierPath(roundedRect: r, xRadius: 4, yRadius: 4).fill()
            let s = NSAttributedString(string: c.text.isEmpty ? "…" : c.text, attributes: [.font: Theme.font(10, .medium), .foregroundColor: sel ? Theme.onAccent : Theme.text])
            ctx.saveGState()
            ctx.clip(to: r.insetBy(dx: 4, dy: 0))
            s.draw(at: CGPoint(x: r.minX + 5, y: r.midY - s.size().height / 2))
            ctx.restoreGState()
        }

        // Playhead.
        let px = x(e.now)
        NSColor.white.setFill()
        CGRect(x: px - 1, y: -2, width: 2, height: laneY + laneH + 2).fill()
        NSBezierPath(ovalIn: CGRect(x: px - 5, y: -4, width: 10, height: 10)).fill()
    }
}
