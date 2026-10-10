import AVFoundation
import AppKit
import Carbon.HIToolbox
import ImageIO
import Speech
import UniformTypeIdentifiers

// A small video editor for screen recordings: trim, crop, speed, mute, captions (typed or
// transcribed on device) and markup (text, emoji, callouts, blur, zoom, title cards),
// saved as a new MP4 or GIF next to the original.

struct Caption: Equatable {
    var id = UUID()
    var start: Double          // seconds on the timeline
    var end: Double
    var text: String
    var position = CaptionPosition.bottom
    var center: CGPoint?            // dragged on the video: center as a fraction of the frame; overrides `position`
    var words: [CaptionWord] = []   // from auto captions: when each word is spoken
}

struct CaptionWord: Equatable {
    var start: Double
    var end: Double
    var text: String
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
    var captionSize = 2        // 0…4
    var captionColor = 6       // text, index into kColors (white)
    var captionEdge = 7        // box, bar or outline color (black)
    var marks: [Mark] = []     // text, emoji, callouts, blur, zoom, title cards
    var captionLook = CaptionLook.pill
    var captionStyle = AnimStyle.auto
    var highlightWords = true  // auto captions: the spoken word lights up
    var clips: [Clip] = []     // played back to back; every time above is timeline time
    var frame = CGSize.zero    // the sequence frame: the first video's upright size, kept when clips change

    static let speeds: [Double] = [0.5, 1, 1.5, 2, 4]
    static let captionScale: [CGFloat] = [0.03, 0.037, 0.045, 0.055, 0.068]
    static let captionSizes = ["Extra small", "Small", "Medium", "Large", "Extra large"]

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

    // Developer hooks for the bench (VideoBench.swift): sees every frame an export renders, and notes how it ran.
    final class Probe {
        private let lock = NSLock()
        private(set) var frames = 0
        private(set) var notes: [String] = []
        // Output frame index and the rendered frame (a CVPixelBuffer or a CGImage). Called from any thread.
        var tap: ((Int, AnyObject) -> Void)?
        func frame(_ index: Int, _ img: @autoclosure () -> AnyObject) {
            lock.lock(); frames += 1; lock.unlock()
            tap?(index, img())
        }
        func note(_ s: String) { lock.lock(); notes.append(s); lock.unlock() }
    }
    nonisolated(unsafe) static var probe: Probe?

    struct Prepared {
        let composition: AVMutableComposition
        let video: AVMutableVideoComposition
        let size: CGSize
    }

    static func prepare(_ e: VideoEdit) async throws -> Prepared {
        // Every frame goes through the same renderer the preview uses.
        let renderer = FrameRenderer(edit: e, full: e.frame, preview: false)
        let b = try await VideoSequence.build(e.clips, frame: e.frame, range: e.trimStart...max(e.trimStart, e.trimEnd), speed: e.speed, muted: e.muted,
                                              box: RendererBox(renderer))
        // Like the HighestQuality preset did: at most H.264 level 5.1's macroblock rate, so 3K at 60 fps saves at 30
        // (1440p60 stays 60). Smoother would mean twice the frames to encode, and files fewer players can decode.
        let blocks = ceil(renderer.out.width / 16) * ceil(renderer.out.height / 16)
        let k = Int32(max(1, ceil(blocks / b.video.frameDuration.seconds / 983_040 - 1e-9)))
        if k > 1 { b.video.frameDuration = CMTimeMultiply(b.video.frameDuration, multiplier: k) }
        return Prepared(composition: b.composition, video: b.video, size: renderer.out)
    }

    static func captionAttributes(_ size: CGFloat, dim: Bool = false, look: CaptionLook = .pill,
                                  color: NSColor = .white, edge: NSColor = .black) -> [NSAttributedString.Key: Any] {
        let p = NSMutableParagraphStyle()
        p.alignment = .center
        var a: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: size, weight: look == .outline ? .heavy : .semibold),
                                                .foregroundColor: color.withAlphaComponent(dim ? 0.5 : 1), .paragraphStyle: p]
        if look == .outline {   // text with an edge in the edge color, no box
            a[.strokeColor] = edge
            a[.strokeWidth] = -2.5
            let sh = NSShadow()
            sh.shadowBlurRadius = size * 0.05
            sh.shadowOffset = NSSize(width: 0, height: -size * 0.03)
            sh.shadowColor = NSColor.black.withAlphaComponent(0.45)
            a[.shadow] = sh
        }
        return a
    }

    static func run(_ s: AVAssetExportSession) async throws {
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in s.exportAsynchronously { k.resume() } }
        if s.status != .completed { throw Failure.failed(s.error?.localizedDescription ?? "Export failed.") }
    }

    static func mp4(_ e: VideoEdit, to url: URL, encoders: Int? = nil) async throws {
        let p = try await prepare(e)
        try await write(p, speed: e.speed, encoders: encoders, to: url)
    }

    // Speech in the trimmed range, as caption-sized chunks. On device when the Mac supports it.
    static func transcribe(_ clips: [Clip], frame: CGSize, from start: Double, to end: Double) async throws -> [Caption] {
        let status = await withCheckedContinuation { k in SFSpeechRecognizer.requestAuthorization { k.resume(returning: $0) } }
        guard status == .authorized else {
            throw Failure.failed("Allow Ather Screenshot in System Settings › Privacy & Security › Speech Recognition.")
        }
        guard let rec = SFSpeechRecognizer(locale: Locale.current) ?? SFSpeechRecognizer(locale: Locale(identifier: "en-US")), rec.isAvailable else {
            throw Failure.failed("Speech recognition isn't available for your language.")
        }
        // The sequence's sound in the trimmed range, mixed into one file.
        let seq = try await VideoSequence.build(clips, frame: frame, range: start...max(start, end), box: RendererBox(FrameRenderer(edit: VideoEdit(trimEnd: 0), full: frame, preview: true)))
        let comp = seq.composition
        guard !comp.tracks(withMediaType: .audio).isEmpty else {
            throw Failure.failed("This video has no sound. For recordings, turn on system audio or the microphone in Settings › Recording.")
        }
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
                c.words.append(CaptionWord(start: s, end: e, text: w))
                cur = c
            } else {
                if let c = cur { out.append(c) }
                cur = Caption(start: s, end: max(e, s + 0.4), text: w, words: [CaptionWord(start: s, end: e, text: w)])
            }
        }
        if let c = cur { out.append(c) }
        // Keep each caption on screen until the next one, up to a short hold.
        for i in out.indices {
            let next = i + 1 < out.count ? out[i + 1].start : out[i].end + 0.8
            out[i].end = min(next, out[i].end + 0.8)
            if var last = out[i].words.last { last.end = out[i].end; out[i].words[out[i].words.count - 1] = last }   // the last word stays lit
        }
        return out
    }
}

// MARK: - Window

final class VideoEditor: NSObject, NSWindowDelegate {
    static var instances: [VideoEditor] = []
    static let quickEmoji = ["✅", "❌", "⚠️", "👉", "👀", "💡", "🎉", "🔥", "⭐️", "❤️", "👍", "🤔"]

    let url: URL
    let player = AVPlayer()
    let window: NSWindow
    var duration: Double = 0
    var videoSize = CGSize(width: 16, height: 9)
    var edit = VideoEdit(trimEnd: 0) { didSet { if edit != oldValue { changed() } } }
    var selected: UUID? { didSet { if selected != oldValue { rebuildInspector() }; timeline.needsDisplay = true; stage.needsDisplay = true } }
    var cropping = false { didSet { stage.needsDisplay = true; syncToolbar() } }
    var dirty = false
    private var undoStack: [VideoEdit] = []
    private var timeObserver: Any?
    private var busy = false
    private var previewBox: RendererBox?
    private var previewComposition: AVVideoComposition?
    private var builtClips: [Clip] = []          // what the preview player plays now
    private var unplayable: Set<UUID> = []        // clips whose file can't be read: playback skips them
    private var pendingSeek: Double?              // where to go once the rebuilt sequence is in the player
    private var refreshPending = false

    private let stage = VideoStage()
    let timeline = Timeline()
    private let playButton = NSButton()
    private let timeLabel = NSTextField(labelWithString: "")
    private let speedPopup = NSPopUpButton()
    private let captionsMenu = NSPopUpButton(frame: .zero, pullsDown: true)
    private let aspectPopup = NSPopUpButton()
    private let addPopup = NSPopUpButton(frame: .zero, pullsDown: true)
    private let muteButton = NSButton()
    private let cropButton = NSButton()
    private let inspector = NSStackView()
    private let hint = NSTextField(labelWithString: "")
    private var actions: [MenuAction] = []
    private var inspectorActions: [MenuAction] = []
    private lazy var timelineHeight = timeline.heightAnchor.constraint(equalToConstant: Timeline.height)

    static func open(_ url: URL) {
        if let e = instances.first(where: { $0.url == url }) { activateApp(); e.window.makeKeyAndOrderFront(nil); return }
        instances.append(VideoEditor(url))
    }

    private init(_ url: URL) {
        self.url = url
        let vis = Geo.mouseScreen.visibleFrame
        let size = NSSize(width: min(1240, vis.width * 0.92), height: min(880, vis.height * 0.92))
        window = NSWindow(contentRect: NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = "Edit video — \(url.lastPathComponent)"
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 980, height: 600)
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

    @MainActor private func load() async {   // touches the window after each await
        let clip: Clip
        do {
            clip = try await VideoSource.probe(url)
        } catch {
            Toast.shared.show("Can't open this video", error.localizedDescription)
            return window.close()
        }
        builtClips = [clip]  // built just below
        edit = VideoEdit(trimEnd: clip.duration, clips: [clip], frame: CGSize(width: clip.w, height: clip.h))
        undoStack = []
        dirty = false
        await rebuildPlayer()
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 30), queue: .main) { [weak self] t in self?.tick(t.seconds) }
        changed()
    }

    // The preview plays the sequence through the export renderer. Rebuilt when the clips change.
    @MainActor func rebuildPlayer() async {
        let clips = edit.clips
        builtClips = clips
        duration = VideoSequence.total(clips)
        videoSize = edit.frame
        let box = RendererBox(FrameRenderer(edit: edit, full: videoSize, preview: true))
        do {
            let b = try await VideoSequence.build(clips, frame: edit.frame, box: box)
            guard clips == edit.clips else { return }  // changed again meanwhile: that rebuild wins
            // The playhead stays where playback is (an undo while playing doesn't jump back to where Play was
            // pressed), unless a clip change asked for a spot.
            let at = pendingSeek ?? now
            pendingSeek = nil
            let rate = player.rate
            previewBox = box
            box.renderer = FrameRenderer(edit: edit, full: videoSize, preview: true)
            let item = AVPlayerItem(asset: b.composition)
            item.videoComposition = b.video
            previewComposition = b.video
            unplayable = b.unplayable
            player.replaceCurrentItem(with: item)
            await player.seek(to: CMTime(seconds: min(max(0, at), duration), preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
            if rate != 0 { player.rate = rate }
            timeline.needsDisplay = true
            stage.needsDisplay = true
            updateTime()
        } catch {
            Toast.shared.show("Can't play this video", error.localizedDescription)
        }
        timeline.loadThumbnails()
    }

    // Hands the latest edit to the preview; while paused, re-renders the current frame.
    private func refreshPreview() {
        guard let box = previewBox else { return }
        let r = FrameRenderer(edit: edit, full: videoSize, preview: true)
        r.zoomInPreview = player.rate != 0
        box.renderer = r
        guard player.rate == 0, !refreshPending, let vc = previewComposition else { return }
        refreshPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0 / 30) { [weak self] in
            guard let self else { return }
            self.refreshPending = false
            self.player.currentItem?.videoComposition = vc.mutableCopy() as? AVVideoComposition
        }
    }

    // MARK: layout

    private func build() {
        let root = NSView()
        let top = NSStackView()
        top.orientation = .horizontal
        top.spacing = 8
        top.edgeInsets = NSEdgeInsets(top: 0, left: 14, bottom: 0, right: 14)

        style(cropButton, "Crop", "crop", "Crop (C): drag on the video", toggle: true) { [weak self] in self?.cropping.toggle() }
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
        style(muteButton, "Mute", "speaker.slash", "Remove the sound", toggle: true) { [weak self] in guard let self else { return }; self.pushUndo(); self.edit.muted.toggle() }

        // One "Add" menu instead of a button per tool.
        addPopup.bezelStyle = .recessed
        addPopup.addItem(withTitle: "Add")
        addPopup.item(at: 0)?.image = NSImage(systemSymbolName: "plus", accessibilityDescription: "Add")
        let entries: [(String, String, String, () -> Void)] = [("Caption", "captions.bubble", "T", { [weak self] in self?.addCaption() })]
            + MarkKind.allCases.map { k in (k.label, k.symbol, k.key, { [weak self] in self?.addMark(k) }) }
            + [("Video clip…", "film.stack", "⌘O", { [weak self] in self?.addClipDialog() })]
        for (title, symbol, key, run) in entries {
            let it = NSMenuItem(title: key.isEmpty ? title : "\(title)    \(key)", action: #selector(MenuAction.fire), keyEquivalent: "")
            it.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            let a = MenuAction(run)
            actions.append(a)
            it.target = a
            addPopup.menu?.addItem(it)
            if title == "Caption" || title == "Step number" || title == "Pixelate" || title == MarkKind.allCases.last?.label { addPopup.menu?.addItem(.separator()) }
        }
        addPopup.toolTip = "Add text, emoji, callouts, blur, zoom or a title card at the playhead, or join another video"

        let auto = barButton("Auto captions", "waveform.badge.mic", "Transcribe speech into captions, on this Mac") { [weak self] in self?.autoCaptions() }
        captionsMenu.bezelStyle = .recessed
        captionsMenu.toolTip = "Caption look, animation and size, for the whole video"
        rebuildCaptionsMenu()

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
        for v in [cropButton, aspectPopup, separator(), speedPopup, muteButton, separator(), addPopup, auto, captionsMenu, spacer, gif, save] as [NSView] { top.addArrangedSubview(v) }

        inspector.orientation = .horizontal
        inspector.spacing = 8
        hint.font = Theme.font(11)
        hint.textColor = Theme.muted
        hint.stringValue = "Space plays · I and O trim · S split · T caption · A arrow · R box · E emoji · N step · X blur · Z zoom · C crop · drop videos to join them"

        playButton.bezelStyle = .regularSquare
        playButton.isBordered = false
        playButton.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: "Play")
        playButton.symbolConfiguration = .init(pointSize: 16, weight: .semibold)
        playButton.contentTintColor = Theme.text
        playButton.toolTip = "Play / pause (Space)"
        bind(playButton) { [weak self] in self?.togglePlay() }
        timeLabel.font = Theme.mono(11)
        timeLabel.textColor = Theme.textDim
        timeLabel.maximumNumberOfLines = 2

        for v in [top, stage, inspector, hint, playButton, timeLabel, timeline] as [NSView] {
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
            stage.bottomAnchor.constraint(equalTo: inspector.topAnchor, constant: -8),
            inspector.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            inspector.bottomAnchor.constraint(equalTo: timeline.topAnchor, constant: -8),
            inspector.heightAnchor.constraint(equalToConstant: 26),
            inspector.leadingAnchor.constraint(greaterThanOrEqualTo: root.leadingAnchor, constant: 14),
            hint.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            hint.centerYAnchor.constraint(equalTo: inspector.centerYAnchor),
            playButton.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 14),
            playButton.topAnchor.constraint(equalTo: timeline.topAnchor, constant: 10),
            playButton.widthAnchor.constraint(equalToConstant: 30),
            timeLabel.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 10),
            timeLabel.topAnchor.constraint(equalTo: playButton.bottomAnchor, constant: 10),
            timeline.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 100),
            timeline.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            timeline.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -14),
            timelineHeight,
        ])
        window.contentView = root
        rebuildInspector()
        syncToolbar()
    }

    private func style(_ b: NSButton, _ title: String, _ symbol: String, _ tip: String, toggle: Bool, _ run: @escaping () -> Void) {
        b.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
        b.imagePosition = .imageLeading
        b.bezelStyle = .recessed
        if toggle { b.setButtonType(.pushOnPushOff) }
        b.contentTintColor = Theme.text
        b.attributedTitle = NSAttributedString(string: title, attributes: [.foregroundColor: Theme.text, .font: Theme.font(12)])
        b.toolTip = tip
        bind(b, run)
    }

    private func bind(_ c: NSControl, _ run: @escaping () -> Void) {
        let a = MenuAction(run)
        actions.append(a)
        c.target = a
        c.action = #selector(MenuAction.fire)
    }

    private func barButton(_ title: String, _ symbol: String, _ tip: String, _ run: @escaping () -> Void) -> NSButton {
        let b = NSButton(title: title, image: NSImage(systemSymbolName: symbol, accessibilityDescription: title) ?? NSImage(), target: nil, action: nil)
        style(b, title, symbol, tip, toggle: false, run)
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

    // MARK: inspector row (whatever is selected)

    func rebuildInspector() {
        for v in inspector.arrangedSubviews { inspector.removeArrangedSubview(v); v.removeFromSuperview() }
        inspectorActions = []
        let ci = selectedCaptionIndex, mi = selectedMarkIndex, k = selectedClipIndex
        inspector.isHidden = ci == nil && mi == nil && k == nil
        hint.isHidden = !inspector.isHidden
        if let ci {
            let c = edit.captions[ci]
            inspector.addArrangedSubview(field(c.text, "Caption text", tag: 1, width: 300))
            let pos = popup(CaptionPosition.allCases.map(\.label) + (c.center != nil ? ["Custom"] : []), c.center != nil ? 3 : c.position.rawValue) { [weak self] i in
                guard i < 3 else { return }
                self?.updateCaption { $0.position = CaptionPosition(rawValue: i) ?? .bottom; $0.center = nil }
                self?.rebuildInspector()
            }
            pos.toolTip = "Or drag the caption on the video"
            inspector.addArrangedSubview(pos)
            // Style is shared by every caption, so they stay consistent.
            let all = NSTextField(labelWithString: "All captions:")
            all.font = Theme.font(11)
            all.textColor = Theme.muted
            inspector.addArrangedSubview(all)
            inspector.addArrangedSubview(swatchPopup(edit.captionColor, prefix: "Text") { [weak self] i in self?.setCaptions { $0.captionColor = i } })
            inspector.addArrangedSubview(swatchPopup(edit.captionEdge, prefix: edit.captionLook == .outline ? "Outline" : "Box") { [weak self] i in self?.setCaptions { $0.captionEdge = i } })
            inspector.addArrangedSubview(popup(VideoEdit.captionSizes, edit.captionSize) { [weak self] i in self?.setCaptions { $0.captionSize = i } })
        } else if let mi {
            let m = edit.marks[mi]
            switch m.kind {
            case .title:
                inspector.addArrangedSubview(field(m.text, "Title", tag: 1, width: 240))
                inspector.addArrangedSubview(field(m.subtitle, "Subtitle (optional)", tag: 2, width: 220))
                inspector.addArrangedSubview(colorPopup(m.color, prefix: "Background"))
            case .emoji:
                inspector.addArrangedSubview(field(m.text, "Emoji", tag: 1, width: 70))
                for e in VideoEditor.quickEmoji {
                    let b = NSButton(title: e, target: nil, action: nil)
                    b.bezelStyle = .recessed
                    b.font = NSFont.systemFont(ofSize: 15)
                    link(b) { [weak self] in self?.updateMark { $0.text = e } ; self?.rebuildInspector() }
                    inspector.addArrangedSubview(b)
                }
                let more = NSButton(title: "More…", target: nil, action: nil)
                more.bezelStyle = .recessed
                link(more) { [weak self] in
                    guard let self, let f = self.inspector.arrangedSubviews.first as? NSTextField else { return }
                    self.window.makeFirstResponder(f)
                    NSApp.orderFrontCharacterPalette(nil)
                }
                inspector.addArrangedSubview(more)
            case .text, .bubble:
                inspector.addArrangedSubview(field(m.text, m.kind == .bubble ? "Bubble text" : "Text", tag: 1, width: 300))
                inspector.addArrangedSubview(colorPopup(m.color, prefix: m.kind == .bubble ? "Bubble" : "Color"))
                inspector.addArrangedSubview(levelPopup(m.level))
            case .arrow, .box, .ellipse, .step:
                inspector.addArrangedSubview(colorPopup(m.color, prefix: "Color"))
                inspector.addArrangedSubview(levelPopup(m.level))
            case .blur, .pixelate:
                inspector.addArrangedSubview(popup(["Blur", "Pixelate"], m.kind == .blur ? 0 : 1) { [weak self] i in self?.updateMark { $0.kind = i == 0 ? .blur : .pixelate } })
                inspector.addArrangedSubview(popup((1...5).map { "Strength \($0)" }, m.level) { [weak self] i in self?.updateMark { $0.level = i } })
            case .zoom:
                inspector.addArrangedSubview(popup(["Smooth zoom", "Snappy zoom"], m.snappy ? 1 : 0) { [weak self] i in self?.updateMark { $0.snappy = i == 1 } })
                let l = NSTextField(labelWithString: "The box sets how far it zooms · plays back zoomed")
                l.font = Theme.font(11)
                l.textColor = Theme.muted
                inspector.addArrangedSubview(l)
            }
            if !m.kind.styles.isEmpty {
                inspector.addArrangedSubview(animationMenu(m))
                let replay = NSButton(image: NSImage(systemSymbolName: "play.circle", accessibilityDescription: "Replay") ?? NSImage(), target: nil, action: nil)
                replay.bezelStyle = .recessed
                replay.toolTip = "Play this item from just before it appears"
                link(replay) { [weak self] in self?.replay(m) }
                inspector.addArrangedSubview(replay)
            }
        }
        if let k, ci == nil, mi == nil {
            let c = edit.clips[k]
            let label = NSTextField(labelWithString: "Clip \(k + 1) of \(edit.clips.count):  \(c.name)  ·  \(clock(c.duration))")
            label.font = Theme.font(12)
            label.textColor = Theme.text
            label.lineBreakMode = .byTruncatingMiddle
            inspector.addArrangedSubview(label)
            func button(_ title: String, _ symbol: String, _ tip: String, _ run: @escaping () -> Void) {
                let b = NSButton(title: title, image: NSImage(systemSymbolName: symbol, accessibilityDescription: title) ?? NSImage(), target: nil, action: nil)
                b.bezelStyle = .recessed
                b.imagePosition = .imageLeading
                b.toolTip = tip
                link(b, run)
                inspector.addArrangedSubview(b)
            }
            button("Split at playhead", "scissors", "Cut this clip in two at the playhead (S)") { [weak self] in self?.splitAtPlayhead() }
            if k > 0 { button("Earlier", "arrow.left", "Play this clip before the one on its left") { [weak self] in self?.moveClip(k, to: k - 1) } }
            if k + 1 < edit.clips.count { button("Later", "arrow.right", "Play this clip after the one on its right") { [weak self] in self?.moveClip(k, to: k + 1) } }
            if edit.clips.count > 1 { button("Remove", "trash", "Take this clip out (⌫)") { [weak self] in self?.removeClip(k) } }
        }
        if ci != nil || mi != nil {
            let del = NSButton(title: "Delete", image: NSImage(systemSymbolName: "trash", accessibilityDescription: "Delete") ?? NSImage(), target: nil, action: nil)
            del.bezelStyle = .recessed
            del.imagePosition = .imageLeading
            del.toolTip = "Delete (⌫)"
            link(del) { [weak self] in self?.deleteSelected() }
            inspector.addArrangedSubview(del)
        }
    }

    // One menu: the style (in and out), an optional effect while on screen, and the advanced options.
    private func animationMenu(_ m: Mark) -> NSPopUpButton {
        let p = NSPopUpButton(frame: .zero, pullsDown: true)
        p.bezelStyle = .recessed
        let menu = p.menu!
        let style = m.style == .auto ? "Auto (\(m.kind.defaultStyle.label))" : m.style.label
        var title = "Animation: " + style
        if m.emphasis != .none { title += " · " + m.emphasis.label }
        if let x = m.exit { title += " → " + (x == .auto ? m.kind.defaultStyle.label : x.label) }
        menu.addItem(withTitle: title, action: nil, keyEquivalent: "")
        func item(_ t: String, on: Bool, indent: Int = 0, _ run: @escaping () -> Void) -> NSMenuItem {
            let it = NSMenuItem(title: t, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction(run)
            inspectorActions.append(a)
            it.target = a
            it.state = on ? .on : .off
            it.indentationLevel = indent
            return it
        }
        for st in [AnimStyle.auto] + m.kind.styles {
            menu.addItem(item(st == .auto ? "Auto (\(m.kind.defaultStyle.label))" : st.label, on: m.style == st) { [weak self] in self?.pickAnimation(m.id) { $0.style = st } })
        }
        if m.kind != .blur && m.kind != .pixelate {
            menu.addItem(.separator())
            let h = NSMenuItem(title: "While on screen", action: nil, keyEquivalent: "")
            h.isEnabled = false
            menu.addItem(h)
            for e in Emphasis.allCases {
                menu.addItem(item(e.label, on: m.emphasis == e, indent: 1) { [weak self] in self?.pickAnimation(m.id) { $0.emphasis = e } })
            }
        }
        menu.addItem(.separator())
        menu.addItem(item("Different exit animation", on: m.exit != nil) { [weak self] in self?.pickAnimation(m.id) { $0.exit = $0.exit == nil ? .fade : nil } })
        if m.exit != nil {
            let sub = NSMenu()
            for st in m.kind.styles where st != .drawOn && st != .typewriter {
                sub.addItem(item(st.label, on: m.exit == st) { [weak self] in self?.pickAnimation(m.id) { $0.exit = st } })
            }
            let it = NSMenuItem(title: "Exit", action: nil, keyEquivalent: "")
            it.submenu = sub
            it.indentationLevel = 1
            menu.addItem(it)
        }
        menu.addItem(item("Apply to all \(m.kind.plural)", on: false) { [weak self] in
            guard let self else { return }
            self.pushUndo()
            for i in self.edit.marks.indices where self.edit.marks[i].kind == m.kind {
                self.edit.marks[i].style = m.style; self.edit.marks[i].exit = m.exit; self.edit.marks[i].emphasis = m.emphasis
            }
            let n = self.edit.marks.filter { $0.kind == m.kind }.count
            Toast.shared.show("Applied to \(n) \(n == 1 ? m.kind.label.lowercased() : m.kind.plural)", m.style == .auto ? "Auto" : m.style.label)
        })
        return p
    }

    // Toolbar "Captions" menu: look, animation, word highlight and size, for the whole video.
    private func rebuildCaptionsMenu() {
        let menu = captionsMenu.menu!
        menu.removeAllItems()
        actions.removeAll { $0.tag == 1 }
        menu.addItem(withTitle: "Captions", action: nil, keyEquivalent: "")
        captionsMenu.item(at: 0)?.image = NSImage(systemSymbolName: "captions.bubble", accessibilityDescription: "Captions")
        func item(_ t: String, on: Bool, _ run: @escaping () -> Void) {
            let it = NSMenuItem(title: t, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction(run)
            a.tag = 1
            actions.append(a)
            it.target = a
            it.state = on ? .on : .off
            menu.addItem(it)
        }
        func header(_ t: String) {
            let h = NSMenuItem(title: t, action: nil, keyEquivalent: "")
            h.isEnabled = false
            menu.addItem(h)
        }
        header("Look")
        for l in CaptionLook.allCases { item("   " + l.label, on: edit.captionLook == l) { [weak self] in self?.setCaptions { $0.captionLook = l } } }
        menu.addItem(.separator())
        header("Animation")
        for st in AnimStyle.captionStyles { item("   " + (st == .auto ? "Auto (Fade)" : st.label), on: edit.captionStyle == st) { [weak self] in self?.setCaptions { $0.captionStyle = st } } }
        menu.addItem(.separator())
        item("Highlight the spoken word", on: edit.highlightWords) { [weak self] in self?.setCaptions { $0.highlightWords.toggle() } }
        menu.addItem(.separator())
        header("Size")
        for (i, n) in VideoEdit.captionSizes.enumerated() { item("   " + n, on: edit.captionSize == i) { [weak self] in self?.setCaptions { $0.captionSize = i } } }
        menu.addItem(.separator())
        for (title, current, set) in [("Text color", edit.captionColor, { (e: inout VideoEdit, i: Int) in e.captionColor = i }),
                                      (edit.captionLook == .outline ? "Outline color" : "Box color", edit.captionEdge, { (e: inout VideoEdit, i: Int) in e.captionEdge = i })] {
            let sub = NSMenu()
            for (i, n) in kColorNames.enumerated() {
                let it = NSMenuItem(title: n, action: #selector(MenuAction.fire), keyEquivalent: "")
                let a = MenuAction { [weak self] in self?.setCaptions { set(&$0, i) } }
                a.tag = 1
                actions.append(a)
                it.target = a
                it.state = current == i ? .on : .off
                it.image = NSImage(size: NSSize(width: 12, height: 12), flipped: false) { r in kColors[i].setFill(); NSBezierPath(ovalIn: r.insetBy(dx: 1, dy: 1)).fill(); return true }
                sub.addItem(it)
            }
            let it = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            it.submenu = sub
            menu.addItem(it)
        }
    }

    private func setCaptions(_ f: (inout VideoEdit) -> Void) {
        pushUndo()
        f(&edit)
        if selectedCaptionIndex != nil { rebuildInspector() }
    }

    // Animation picks: update the menu (title and checkmark) and replay the item so the choice shows.
    private func pickAnimation(_ id: UUID, _ f: (inout Mark) -> Void) {
        guard let i = edit.marks.firstIndex(where: { $0.id == id }) else { return }
        pushUndo()
        f(&edit.marks[i])
        rebuildInspector()
        replay(edit.marks[i])
    }

    func replay(_ m: Mark) {
        player.pause()
        seek(max(edit.trimStart, m.start - 0.4))
        player.rate = Float(edit.speed)
        playStateChanged()
    }

    private func link(_ c: NSControl, _ run: @escaping () -> Void) {
        let a = MenuAction(run)
        inspectorActions.append(a)
        c.target = a
        c.action = #selector(MenuAction.fire)
    }

    private func field(_ value: String, _ placeholder: String, tag: Int, width: CGFloat) -> NSTextField {
        let f = NSTextField(string: value)
        f.placeholderString = placeholder
        f.font = Theme.font(13)
        f.tag = tag
        f.delegate = self
        f.widthAnchor.constraint(equalToConstant: width).isActive = true
        return f
    }

    private func popup(_ items: [String], _ selected: Int, _ run: @escaping (Int) -> Void) -> NSPopUpButton {
        let p = NSPopUpButton()
        p.addItems(withTitles: items)
        p.selectItem(at: selected)
        p.bezelStyle = .recessed
        link(p) { [weak p] in run(p?.indexOfSelectedItem ?? 0) }
        return p
    }

    private func colorPopup(_ selected: Int, prefix: String) -> NSPopUpButton {
        swatchPopup(selected, prefix: prefix) { [weak self] i in self?.updateMark { $0.color = i } }
    }

    private func swatchPopup(_ selected: Int, prefix: String, _ run: @escaping (Int) -> Void) -> NSPopUpButton {
        let p = popup(kColorNames.map { "\(prefix): \($0)" }, selected, run)
        for (i, it) in p.itemArray.enumerated() {
            let img = NSImage(size: NSSize(width: 12, height: 12), flipped: false) { r in
                kColors[i].setFill()
                NSBezierPath(ovalIn: r.insetBy(dx: 1, dy: 1)).fill()
                return true
            }
            it.image = img
        }
        return p
    }

    private func levelPopup(_ selected: Int) -> NSPopUpButton {
        popup((1...kLevels).map { "Size \($0)" }, selected) { [weak self] i in self?.updateMark { $0.level = i } }
    }

    // MARK: state

    var now: Double { player.currentTime().seconds.isFinite ? player.currentTime().seconds : 0 }
    var selectedCaptionIndex: Int? { selected.flatMap { id in edit.captions.firstIndex { $0.id == id } } }
    var selectedMarkIndex: Int? { selected.flatMap { id in edit.marks.firstIndex { $0.id == id } } }
    var selectedClipIndex: Int? { selected.flatMap { id in edit.clips.firstIndex { $0.id == id } } }
    var viewRect: CGRect {
        let f = CGRect(origin: .zero, size: videoSize)
        let c = edit.crop.map { $0.intersection(f) } ?? f
        return c.isNull || c.width < 16 ? f : c
    }

    func pushUndo() {
        undoStack.append(edit)
        if undoStack.count > 200 { undoStack.removeFirst() }
        dirty = true
    }

    func undo() {
        guard let e = undoStack.popLast() else { return }
        edit = e
        if let s = selected, !edit.captions.contains(where: { $0.id == s }) && !edit.marks.contains(where: { $0.id == s }) && !edit.clips.contains(where: { $0.id == s }) { selected = nil }
        rebuildInspector()
    }

    private func changed() {
        duration = VideoSequence.total(edit.clips)
        if edit.clips != builtClips, !edit.clips.isEmpty {
            builtClips = edit.clips
            Task { @MainActor in await self.rebuildPlayer() }
        }
        if timelineHeight.constant != timeline.wantedHeight { timelineHeight.constant = timeline.wantedHeight }
        player.isMuted = edit.muted
        stage.needsDisplay = true
        timeline.needsDisplay = true
        syncToolbar()
        updateTime()
        refreshPreview()
    }

    private func syncToolbar() {
        cropButton.state = cropping ? .on : .off
        aspectPopup.isHidden = !cropping
        muteButton.state = edit.muted ? .on : .off
        speedPopup.selectItem(at: VideoEdit.speeds.firstIndex(of: edit.speed) ?? 1)
        rebuildCaptionsMenu()
    }

    private func updateTime() {
        timeLabel.stringValue = "\(clock(now))\n\(clock(edit.outputDuration)) out"
    }

    func clock(_ t: Double) -> String {
        let s = max(0, t)
        return String(format: "%d:%02d.%d", Int(s) / 60, Int(s) % 60, Int((s * 10).truncatingRemainder(dividingBy: 10)))
    }

    // MARK: playback

    private func tick(_ t: Double) {
        if player.rate != 0, !unplayable.isEmpty, let (k, _) = VideoSequence.locate(edit.clips, t), unplayable.contains(edit.clips[k].id) {
            // A clip that can't play: on to the next one that can, instead of stopping there.
            let starts = VideoSequence.starts(edit.clips)
            if let next = (k + 1..<edit.clips.count).first(where: { !unplayable.contains(edit.clips[$0].id) }), starts[next] < edit.trimEnd {
                seek(starts[next])
            } else { player.pause(); seek(edit.trimStart); playStateChanged() }
            return
        }
        if player.rate != 0 && t >= edit.trimEnd - 0.01 {
            player.pause()
            seek(edit.trimStart)
            playStateChanged()
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
        playStateChanged()
    }

    private func playStateChanged() {
        playButton.image = NSImage(systemSymbolName: player.rate != 0 ? "pause.fill" : "play.fill", accessibilityDescription: "Play")
        refreshPreview()   // zoom shows while playing
    }

    func seek(_ t: Double) {
        player.seek(to: CMTime(seconds: min(max(0, t), duration), preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
        timeline.needsDisplay = true
        stage.needsDisplay = true
        updateTime()
    }

    func step(_ frames: Double) {
        player.pause()
        playStateChanged()
        seek(now + frames / 30)
    }

    // MARK: editing

    func setTrim(start: Double? = nil, end: Double? = nil) {
        var e = edit
        if let start { e.trimStart = min(max(0, start), e.trimEnd - 0.1) }
        if let end { e.trimEnd = max(min(duration, end), e.trimStart + 0.1) }
        edit = e
    }

    // MARK: clips

    func setClips(_ clips: [Clip], seekTo t: Double? = nil) {
        guard !clips.isEmpty, clips != edit.clips else { return }
        player.pause()
        playStateChanged()
        pushUndo()
        var e = edit
        e.applyClips(clips)
        pendingSeek = t
        edit = e
    }

    // Joins videos after the selected clip (or at the end). Any shape works: each is fitted into the frame.
    func addClips(_ urls: [URL]) {
        Task { @MainActor in
            var add: [Clip] = []
            var bad: [String] = []
            for u in urls {
                if MediaFiles.isVideo(u), let c = try? await VideoSource.probe(u) { add.append(c) } else { bad.append(u.lastPathComponent) }
            }
            if !bad.isEmpty { Toast.shared.show("Can't add that as a video", bad.joined(separator: ", "), ms: 4000) }
            guard !add.isEmpty else { return }
            var clips = edit.clips
            let at = selectedClipIndex.map { $0 + 1 } ?? clips.count
            clips.insert(contentsOf: add, at: at)
            setClips(clips, seekTo: VideoSequence.starts(clips)[at])
            selected = add[0].id
        }
    }

    func addClipDialog() {
        let p = NSOpenPanel()
        p.allowsMultipleSelection = true
        p.allowedContentTypes = MediaFiles.contentTypes(pictures: false, videos: true)
        p.message = "Add videos after this one"
        p.beginSheetModal(for: window) { [weak self] r in if r == .OK { self?.addClips(p.urls) } }
    }

    // Cuts the clip under the playhead in two (then a middle part can be removed, or the halves reordered).
    func splitAtPlayhead() {
        let t = now
        guard let (k, ft) = VideoSequence.locate(edit.clips, t) else { return }
        let c = edit.clips[k]
        guard ft - c.inPoint >= 0.1, c.outPoint - ft >= 0.1 else { return Toast.shared.show("Move the playhead into a clip to split it", ms: 2000) }
        var clips = edit.clips
        var second = c
        second.id = UUID()
        second.inPoint = ft
        clips[k].outPoint = ft
        clips.insert(second, at: k + 1)
        setClips(clips, seekTo: t)
        selected = second.id
    }

    func moveClip(_ from: Int, to: Int) {
        guard from != to, edit.clips.indices.contains(from), edit.clips.indices.contains(to) else { return }
        var clips = edit.clips
        let c = clips.remove(at: from)
        clips.insert(c, at: to)
        setClips(clips, seekTo: VideoSequence.starts(clips)[to])
    }

    func removeClip(_ i: Int) {
        guard edit.clips.indices.contains(i), edit.clips.count > 1 else { return }  // the last clip stays
        var clips = edit.clips
        clips.remove(at: i)
        setClips(clips, seekTo: VideoSequence.starts(clips)[min(i, clips.count - 1)])
        selected = nil
    }

    // The shortest a clip can be trimmed to; a clip already shorter keeps its own length, so a click on its edge
    // never pushes it past the end of its file.
    static func shortest(_ c: Clip) -> Double { min(0.2, c.duration) }

    func trimClip(_ i: Int, in newIn: Double? = nil, out newOut: Double? = nil) {
        guard edit.clips.indices.contains(i) else { return }
        var clips = edit.clips
        var c = clips[i]
        let s = VideoEditor.shortest(c)
        if let newIn { c.inPoint = min(max(0, newIn), c.outPoint - s) }
        if let newOut { c.outPoint = min(max(newOut, c.inPoint + s), max(c.length, c.inPoint + s)) }
        clips[i] = c
        setClips(clips)
    }

    private var insertTime: Double { min(max(now, edit.trimStart), max(edit.trimStart, edit.trimEnd - 0.5)) }

    func addCaption() {
        pushUndo()
        let start = insertTime
        let c = Caption(start: start, end: min(edit.trimEnd, start + 3), text: "")
        edit.captions.append(c)
        edit.captions.sort { $0.start < $1.start }
        selected = c.id
        focusField()
    }

    func addMark(_ k: MarkKind) {
        pushUndo()
        let v = viewRect, w = v.width, h = v.height
        func box(_ fw: CGFloat, _ fh: CGFloat) -> (CGPoint, CGPoint) {
            (CGPoint(x: v.midX - w * fw / 2, y: v.midY - h * fh / 2), CGPoint(x: v.midX + w * fw / 2, y: v.midY + h * fh / 2))
        }
        var (a, b) = box(0.3, 0.22)
        var color = 0, level = 2, text = ""
        switch k {
        case .arrow: (a, b) = (CGPoint(x: v.minX + w * 0.36, y: v.minY + h * 0.66), CGPoint(x: v.minX + w * 0.5, y: v.minY + h * 0.47))
        case .step:
            let s = h * 0.08
            (a, b) = (CGPoint(x: v.midX - s / 2, y: v.midY - s / 2), CGPoint(x: v.midX + s / 2, y: v.midY + s / 2))
        case .text: (a, b) = (CGPoint(x: v.minX + w * 0.08, y: v.minY + h * 0.1), CGPoint(x: v.minX + w * 0.6, y: v.minY + h * 0.2)); color = 6
        case .bubble: (a, b) = (CGPoint(x: v.midX - w * 0.16, y: v.minY + h * 0.18), CGPoint(x: v.midX + w * 0.16, y: v.minY + h * 0.3)); color = 6; level = 2
        case .emoji:
            let s = h * 0.14
            (a, b) = (CGPoint(x: v.midX - s / 2, y: v.midY - s / 2), CGPoint(x: v.midX + s / 2, y: v.midY + s / 2)); text = "✅"
        case .zoom: (a, b) = box(0.4, 0.4)
        case .title: (a, b) = (v.origin, CGPoint(x: v.maxX, y: v.maxY)); color = 7
        case .blur, .pixelate: level = 2
        case .box, .ellipse: break
        }
        let start = insertTime
        var m = Mark(kind: k, start: start, end: min(edit.trimEnd, start + k.defaultSeconds), a: a, b: b, text: text, color: color, level: level)
        if k == .step { m.step = (edit.marks.filter { $0.kind == .step }.map(\.step).max() ?? 0) + 1 }
        edit.marks.append(m)
        selected = m.id
        if k == .text || k == .bubble || k == .title { focusField() }
    }

    private func focusField() {
        if let f = inspector.arrangedSubviews.first as? NSTextField { window.makeFirstResponder(f) }
    }

    func updateMark(_ f: (inout Mark) -> Void) {
        guard let i = selectedMarkIndex else { return }
        pushUndo()
        f(&edit.marks[i])
    }

    func updateCaption(_ f: (inout Caption) -> Void) {
        guard let i = selectedCaptionIndex else { return }
        pushUndo()
        f(&edit.captions[i])
    }

    func deleteSelected() {
        if let k = selectedClipIndex { return removeClip(k) }
        guard selected != nil else { return }
        pushUndo()
        edit.captions.removeAll { $0.id == selected }
        edit.marks.removeAll { $0.id == selected }
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
        let (clips, frame, s, e) = (edit.clips, edit.frame, edit.trimStart, edit.trimEnd)
        Task { @MainActor in
            defer { self.busy = false }
            do {
                var caps = try await VideoExport.transcribe(clips, frame: frame, from: s, to: e)
                if self.edit.clips != clips {  // the clips changed while it ran: move the captions with their footage
                    var then = VideoEdit(trimEnd: VideoSequence.total(clips), clips: clips, frame: frame)
                    then.captions = caps
                    then.applyClips(self.edit.clips)
                    caps = then.captions
                }
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
        window.makeFirstResponder(stage)   // commits a field being edited
        busy = true
        player.pause()
        playStateChanged()
        let src = Library.shared.meta(url)
        let info = NameInfo(app: src.app, window: src.window)
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("ather-\(UUID().uuidString).\(gif ? "gif" : "mp4")")
        Toast.shared.show(gif ? "Saving GIF…" : "Saving video…", "\(clock(edit.outputDuration)) long", ms: 600_000)
        let e = edit
        Task { @MainActor in
            defer { self.busy = false }
            do {
                if gif { try await VideoExport.gif(e, to: tmp) } else { try await VideoExport.mp4(e, to: tmp) }
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
        // While the mouse holds a clip drag, keys don't edit: only Esc, which cancels it.
        if timeline.isDragging {
            if code == kVK_Escape { timeline.cancelDrag() }
            return true
        }
        if cmd {
            switch code {
            case kVK_ANSI_O: addClipDialog()
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
        case kVK_ANSI_S: splitAtPlayhead()
        case kVK_ANSI_I: pushUndo(); setTrim(start: now)
        case kVK_ANSI_O: pushUndo(); setTrim(end: now)
        case kVK_ANSI_T: addCaption()
        case kVK_ANSI_A: addMark(.arrow)
        case kVK_ANSI_R: addMark(.box)
        case kVK_ANSI_E: addMark(.emoji)
        case kVK_ANSI_N: addMark(.step)
        case kVK_ANSI_X: addMark(.blur)
        case kVK_ANSI_Z: addMark(.zoom)
        case kVK_ANSI_C: cropping.toggle()
        case kVK_Delete, kVK_ForwardDelete: deleteSelected()
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

    // Live: the preview updates as you type.
    func controlTextDidChange(_ obj: Notification) {
        guard let f = obj.object as? NSTextField else { return }
        if let i = selectedCaptionIndex { edit.captions[i].text = f.stringValue }
        if let i = selectedMarkIndex {
            if f.tag == 2 { edit.marks[i].subtitle = f.stringValue } else { edit.marks[i].text = f.stringValue }
        }
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy sel: Selector) -> Bool {
        if sel == #selector(NSResponder.insertNewline(_:)) || sel == #selector(NSResponder.cancelOperation(_:)) {
            window.makeFirstResponder(stage)
            return true
        }
        return false
    }
}

// MARK: - Stage (video, crop, markup handles)

final class VideoStage: NSView {
    weak var editor: VideoEditor?
    private let playerView = PlayerView()
    var playerLayer: AVPlayerLayer { playerView.playerLayer }
    // Guides and handles draw in a view above the video; a layer-backed view's own drawing sits under its sublayers.
    private let overlay = StageOverlay()
    private enum Drag { case crop(CGPoint), move(UUID, CGPoint, Mark), handle(UUID, Int, Mark), caption(UUID, CGPoint, CGRect) }
    private var drag: Drag?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.cornerRadius = 8
        layer?.masksToBounds = true
        playerView.autoresizingMask = [.width, .height]
        addSubview(playerView)
        overlay.stage = self
        overlay.autoresizingMask = [.width, .height]
        addSubview(overlay)
        registerForDraggedTypes([.fileURL])
    }
    required init?(coder: NSCoder) { fatalError() }

    // Dropping videos joins them after the selected clip.
    private func videos(_ info: NSDraggingInfo) -> [URL] {
        (info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []).filter(MediaFiles.isVideo)
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { videos(sender).isEmpty ? [] : .copy }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let v = videos(sender)
        guard !v.isEmpty else { return false }
        editor?.addClips(v)
        return true
    }

    override var needsDisplay: Bool {
        get { super.needsDisplay }
        set { super.needsDisplay = newValue; if newValue { overlay.needsDisplay = true } }
    }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override func layout() {
        super.layout()
        playerView.frame = bounds
        overlay.frame = bounds
    }

    // Where the video is drawn, in view coordinates.
    var videoRect: CGRect {
        guard let e = editor else { return bounds }
        let s = min(bounds.width / e.videoSize.width, bounds.height / e.videoSize.height)
        let w = e.videoSize.width * s, h = e.videoSize.height * s
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }
    private var scale: CGFloat { videoRect.width / max(1, editor?.videoSize.width ?? 1) }
    func toVideo(_ p: CGPoint) -> CGPoint {
        let r = videoRect, s = scale
        return CGPoint(x: (p.x - r.minX) / s, y: (p.y - r.minY) / s)
    }
    func toView(_ r: CGRect) -> CGRect {
        let v = videoRect, s = scale
        return CGRect(x: v.minX + r.minX * s, y: v.minY + r.minY * s, width: r.width * s, height: r.height * s)
    }
    func toView(_ p: CGPoint) -> CGPoint {
        let v = videoRect, s = scale
        return CGPoint(x: v.minX + p.x * s, y: v.minY + p.y * s)
    }

    override func keyDown(with e: NSEvent) { if editor?.key(e) != true { super.keyDown(with: e) } }

    // Handle points of a mark, in video coordinates: arrow ends, or the rect's corners.
    func handles(_ m: Mark) -> [CGPoint] {
        if m.kind == .title { return [] }
        if m.kind.isLine { return [m.a, m.b] }
        let r = m.rect
        return [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)]
    }

    private func hit(_ m: Mark, _ p: CGPoint, slop: CGFloat) -> Bool {
        if m.kind == .title { return true }
        if m.kind.isLine {
            let dx = m.b.x - m.a.x, dy = m.b.y - m.a.y, l2 = dx * dx + dy * dy
            let t = l2 == 0 ? 0 : max(0, min(1, ((p.x - m.a.x) * dx + (p.y - m.a.y) * dy) / l2))
            return hypot(p.x - (m.a.x + t * dx), p.y - (m.a.y + t * dy)) <= slop * 1.5
        }
        var r = m.rect
        if m.kind == .bubble { r.size.height *= 1.32 }
        return r.insetBy(dx: -slop, dy: -slop).contains(p)
    }

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        guard let ed = editor else { return }
        let vp = toVideo(convert(e.locationInWindow, from: nil))
        let slop = 8 / max(0.01, scale)
        if ed.cropping {
            ed.pushUndo()
            drag = .crop(vp)
            return
        }
        // Handles of the selected mark first.
        if let i = ed.selectedMarkIndex, ed.edit.marks[i].active(ed.now) {
            let m = ed.edit.marks[i]
            if let h = handles(m).firstIndex(where: { hypot($0.x - vp.x, $0.y - vp.y) <= slop }) {
                ed.pushUndo()
                drag = .handle(m.id, h, m)
                return
            }
        }
        let t = ed.now
        if let m = ed.edit.marks.reversed().first(where: { $0.active(t) && $0.kind != .title && hit($0, vp, slop: slop) })
            ?? ed.edit.marks.reversed().first(where: { $0.active(t) && $0.kind == .title }) {
            ed.selected = m.id
            if e.clickCount == 2, m.kind.hasText { ed.focusFieldPublic(); return }
            ed.pushUndo()
            drag = .move(m.id, vp, m)
            return
        }
        if let c = ed.edit.captions.first(where: { $0.active(t) && captionRect($0).contains(vp) }) {
            ed.selected = c.id
            if e.clickCount == 2 { ed.focusFieldPublic(); return }
            ed.pushUndo()
            drag = .caption(c.id, vp, captionRect(c))
            return
        }
        if ed.selected != nil { ed.selected = nil } else { ed.togglePlay() }
    }

    // Where a caption sits on the video, in video coordinates.
    func captionRect(_ c: Caption) -> CGRect {
        guard let ed = editor else { return .zero }
        let v = ed.viewRect
        let r = FrameRenderer(edit: ed.edit, full: ed.videoSize, preview: true)
        guard let (_, pr) = r.captionImage(c, in: v.size) else { return .zero }
        return pr.offsetBy(dx: v.minX, dy: v.minY)
    }

    override func mouseDragged(with e: NSEvent) {
        guard let ed = editor, let d = drag else { return }
        let p = toVideo(convert(e.locationInWindow, from: nil))
        switch d {
        case .crop(let a):
            var b = p
            if let k = ed.aspect {   // keep the chosen shape
                let w = abs(b.x - a.x), h = abs(b.y - a.y)
                let W = max(w, h * k), H = W / k
                b = CGPoint(x: a.x + (b.x < a.x ? -W : W), y: a.y + (b.y < a.y ? -H : H))
            }
            ed.setCrop(Geo.norm(a, b))
        case .caption(let id, let from, let r):
            guard let i = ed.edit.captions.firstIndex(where: { $0.id == id }) else { return }
            let v = ed.viewRect
            let mid = CGPoint(x: r.midX + p.x - from.x, y: r.midY + p.y - from.y)
            ed.edit.captions[i].center = CGPoint(x: min(1, max(0, (mid.x - v.minX) / v.width)), y: min(1, max(0, (mid.y - v.minY) / v.height)))
        case .move(let id, let from, let o):
            guard let i = ed.edit.marks.firstIndex(where: { $0.id == id }), o.kind != .title else { return }
            let dx = p.x - from.x, dy = p.y - from.y
            ed.edit.marks[i].a = CGPoint(x: o.a.x + dx, y: o.a.y + dy)
            ed.edit.marks[i].b = CGPoint(x: o.b.x + dx, y: o.b.y + dy)
        case .handle(let id, let h, let o):
            guard let i = ed.edit.marks.firstIndex(where: { $0.id == id }) else { return }
            if o.kind.isLine {
                if h == 0 { ed.edit.marks[i].a = p } else { ed.edit.marks[i].b = p }
            } else {
                let r = o.rect
                let anchor = CGPoint(x: h % 2 == 0 ? r.maxX : r.minX, y: h < 2 ? r.maxY : r.minY)
                var q = p
                if o.kind == .emoji || o.kind == .step || e.modifierFlags.contains(.shift) {   // keep the shape
                    let k = r.width / max(1, r.height)
                    let w = max(abs(q.x - anchor.x), abs(q.y - anchor.y) * k)
                    q = CGPoint(x: anchor.x + (q.x < anchor.x ? -w : w), y: anchor.y + (q.y < anchor.y ? -w / k : w / k))
                }
                ed.edit.marks[i].a = anchor
                ed.edit.marks[i].b = q
            }
        }
    }

    override func mouseUp(with e: NSEvent) {
        if case .caption = drag { editor?.rebuildInspector() }   // the position menu now reads "Custom"
        drag = nil
    }

    fileprivate func drawOverlay() {
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
        // Selection: outline and handles. Zoom and blur regions show their box only while selected.
        if let i = ed.selectedMarkIndex {
            let m = ed.edit.marks[i]
            let on = m.active(ed.now)
            ctx.setStrokeColor((on ? Theme.accent : Theme.accent.withAlphaComponent(0.4)).cgColor)
            ctx.setLineWidth(1.5)
            ctx.setLineDash(phase: 0, lengths: [5, 4])
            if m.kind == .zoom {
                ctx.stroke(toView(FrameRenderer.zoomTarget(m.rect, view: ed.viewRect)))
                ctx.setLineDash(phase: 0, lengths: [2, 3])
                ctx.stroke(toView(m.rect))
            } else if m.kind.isLine {
                ctx.strokeLineSegments(between: [toView(m.a), toView(m.b)])
            } else if m.kind != .title {
                var r = m.rect
                if m.kind == .bubble { r.size.height *= 1.32 }
                ctx.stroke(toView(r).insetBy(dx: -3, dy: -3))
            }
            ctx.setLineDash(phase: 0, lengths: [])
            if on {
                for h in handles(m) {
                    let c = toView(h)
                    let box = CGRect(x: c.x - 5, y: c.y - 5, width: 10, height: 10)
                    ctx.setFillColor(NSColor.white.cgColor)
                    ctx.fillEllipse(in: box)
                    ctx.setStrokeColor(Theme.accent.cgColor)
                    ctx.strokeEllipse(in: box)
                }
            }
        }
        if let i = ed.selectedCaptionIndex, ed.edit.captions[i].active(ed.now) {
            Theme.accent.setStroke()
            let ring = NSBezierPath(roundedRect: toView(captionRect(ed.edit.captions[i])).insetBy(dx: -3, dy: -3), xRadius: 8, yRadius: 8)
            ring.lineWidth = 1.5
            ring.stroke()
        }
    }
}

extension VideoEditor {
    func focusFieldPublic() {
        if let f = inspector.arrangedSubviews.first(where: { $0 is NSTextField && ($0 as! NSTextField).isEditable }) { window.makeFirstResponder(f) }
    }
}

private final class PlayerView: NSView {
    let playerLayer = AVPlayerLayer()
    override init(frame: NSRect) {
        super.init(frame: frame)
        playerLayer.videoGravity = .resizeAspect
        layer = playerLayer
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError() }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

private final class StageOverlay: NSView {
    weak var stage: VideoStage?
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }   // clicks go to the stage
    override func draw(_ dirtyRect: NSRect) { stage?.drawOverlay() }
}

// MARK: - Timeline (thumbnails, trim handles, playhead, caption and markup lanes)

final class Timeline: NSView {
    weak var editor: VideoEditor?
    private var thumbs: [CGImage] = []
    private enum Target { case caption(UUID), mark(UUID) }
    private enum Drag {
        case start, end, playhead, item(Target, edge: Int, grab: Double, start: Double, end: Double)
        case clipIn(Int, grab: Double, value: Double), clipOut(Int, grab: Double, value: Double)  // applied on mouse-up
        case clipMove(Int, downX: CGFloat, target: Int?)
    }
    private var drag: Drag?

    static let height: CGFloat = 90 + 3 * 18
    private let stripH: CGFloat = 52
    private let capH: CGFloat = 22
    private let rowH: CGFloat = 18
    // The clip lane sits above the thumbnails, and only once there's more than one clip.
    var laneH: CGFloat { (editor?.edit.clips.count ?? 0) > 1 ? 24 : 0 }
    private var stripY: CGFloat { laneH }
    private var capY: CGFloat { laneH + 62 }
    private var markY: CGFloat { laneH + 90 }

    var isDragging: Bool { drag != nil }

    // Esc, or the window losing the mouse mid-drag. Item drags keep what they did (one undo step);
    // clip drags only apply on release, so nothing changes.
    func cancelDrag() {
        guard drag != nil else { return }
        drag = nil
        needsDisplay = true
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        NotificationCenter.default.removeObserver(self, name: NSWindow.didResignKeyNotification, object: nil)
        if let w = window { NotificationCenter.default.addObserver(self, selector: #selector(lostMouse), name: NSWindow.didResignKeyNotification, object: w) }
    }
    @objc private func lostMouse() { cancelDrag() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func keyDown(with e: NSEvent) { if editor?.key(e) != true { super.keyDown(with: e) } }

    // Frames straight from each clip's file, upright, without the sequence's black bars. A newer edit cancels
    // the job in flight, so stale frames are never decoded or shown.
    private var thumbJob: Task<Void, Never>?
    func loadThumbnails() {
        thumbJob?.cancel()
        guard let e = editor, e.duration > 0 else { return }
        let clips = e.edit.clips, total = e.duration
        let count = 16
        thumbJob = Task { @MainActor in
            var gens: [String: AVAssetImageGenerator] = [:]
            var out: [CGImage] = []
            for i in 0..<count {
                guard !Task.isCancelled else { return }
                guard let (k, ft) = VideoSequence.locate(clips, total * (Double(i) + 0.5) / Double(count)) else { continue }
                let path = clips[k].path
                let gen = gens[path] ?? {
                    let g = AVAssetImageGenerator(asset: AVURLAsset(url: clips[k].url))
                    g.appliesPreferredTrackTransform = true
                    g.maximumSize = CGSize(width: 240, height: 240)
                    gens[path] = g
                    return g
                }()
                if let img = try? await gen.image(at: CMTime(seconds: ft, preferredTimescale: 600)).image { out.append(img) }
            }
            guard !Task.isCancelled else { return }
            self.thumbs = out
            self.needsDisplay = true
        }
    }

    func x(_ t: Double) -> CGFloat { bounds.width * CGFloat(t / max(0.001, editor?.duration ?? 1)) }
    func t(_ x: CGFloat) -> Double { Double(min(max(0, x), bounds.width) / max(1, bounds.width)) * (editor?.duration ?? 0) }

    // Markup bars, one row per overlapping item; the timeline grows to fit (three rows minimum).
    private func rows() -> [(Mark, Int)] {
        guard let e = editor else { return [] }
        var ends: [Double] = []
        return e.edit.marks.sorted { $0.start < $1.start }.map { m in
            if let r = ends.firstIndex(where: { $0 <= m.start }) { ends[r] = m.end; return (m, r) }
            ends.append(m.end)
            return (m, ends.count - 1)
        }
    }

    var rowCount: Int { max(3, min(8, (rows().map(\.1).max() ?? 0) + 1)) }
    var wantedHeight: CGFloat { markY + CGFloat(rowCount) * rowH }

    private func barRect(_ s: Double, _ e: Double, y: CGFloat, h: CGFloat) -> CGRect {
        CGRect(x: x(s), y: y, width: max(6, x(e) - x(s)), height: h)
    }

    override func mouseDown(with ev: NSEvent) {
        window?.makeFirstResponder(self)
        guard let e = editor else { return }
        let p = convert(ev.locationInWindow, from: nil)
        // The playhead knob sits over the clip lane: it scrubs, whatever is under it.
        let onKnob = abs(p.x - x(e.now)) <= 7 && p.y < 8
        if laneH > 0, p.y < laneH, !onKnob {
            // The selected clip's edges win over the neighbour's body.
            if let k = e.selectedClipIndex {
                let st = VideoSequence.starts(e.edit.clips)[k], c = e.edit.clips[k]
                let d0 = abs(p.x - x(st)), d1 = abs(p.x - x(st + c.duration))
                if min(d0, d1) < 6 {
                    e.player.pause()
                    drag = d0 <= d1 ? .clipIn(k, grab: t(p.x), value: c.inPoint) : .clipOut(k, grab: t(p.x), value: c.outPoint)
                    return
                }
            }
            if let (k, _) = VideoSequence.locate(e.edit.clips, t(p.x)) {
                e.selected = e.edit.clips[k].id
                drag = .clipMove(k, downX: p.x, target: nil)
            } else { e.selected = nil }
            needsDisplay = true
            return
        }
        if onKnob {
            drag = .playhead
            e.player.pause()
            return
        }
        func grab(_ target: Target, _ s: Double, _ en: Double) {
            let edge = abs(p.x - x(s)) < 6 ? -1 : abs(p.x - x(en)) < 6 ? 1 : 0
            e.pushUndo()
            drag = .item(target, edge: edge, grab: t(p.x), start: s, end: en)
            if ev.clickCount == 2 { e.seek(s); e.focusFieldPublic() }
        }
        if p.y >= markY {
            if let (m, _) = rows().last(where: { barRect($0.0.start, $0.0.end, y: markY + CGFloat($0.1) * rowH, h: rowH - 2).insetBy(dx: -4, dy: 0).contains(p) }) {
                e.selected = m.id
                grab(.mark(m.id), m.start, m.end)
            } else { e.selected = nil; e.seek(t(p.x)) }
            return
        }
        if p.y >= capY {
            if let c = e.edit.captions.last(where: { x($0.start) - 4 <= p.x && p.x <= x($0.end) + 4 }) {
                e.selected = c.id
                grab(.caption(c.id), c.start, c.end)
            } else { e.selected = nil; e.seek(t(p.x)) }
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
        let now = t(convert(ev.locationInWindow, from: nil).x)
        switch d {
        case .start: e.setTrim(start: now); e.seek(e.edit.trimStart)
        case .end: e.setTrim(end: now); e.seek(e.edit.trimEnd)
        case .playhead: e.seek(now)
        case .clipIn(let k, let grab, _), .clipOut(let k, let grab, _):
            guard e.edit.clips.indices.contains(k) else { return cancelDrag() }
            let c = e.edit.clips[k], s = VideoEditor.shortest(c)
            let isIn: Bool = { if case .clipIn = d { return true }; return false }()
            let v = (isIn ? c.inPoint : c.outPoint) + (now - grab)
            let value = isIn ? min(max(0, v), c.outPoint - s) : min(max(v, c.inPoint + s), max(c.length, c.inPoint + s))
            drag = isIn ? .clipIn(k, grab: grab, value: value) : .clipOut(k, grab: grab, value: value)
            if value >= c.inPoint && value <= c.outPoint { e.seek(VideoSequence.starts(e.edit.clips)[k] + value - c.inPoint) }  // the frame at the cut
        case .clipMove(let k, let downX, let target):
            let px = convert(ev.locationInWindow, from: nil).x
            guard e.edit.clips.indices.contains(k), target != nil || abs(px - downX) >= 5 else { break }
            let starts = VideoSequence.starts(e.edit.clips)
            // How many of the other clips end up before it.
            let n = e.edit.clips.indices.filter { $0 != k && x(starts[$0] + e.edit.clips[$0].duration / 2) < px }.count
            drag = .clipMove(k, downX: downX, target: n)
        case .item(let target, let edge, let grab, let s0, let e0):
            var s = s0, en = e0
            switch edge {
            case -1: s = min(now, e0 - 0.2)
            case 1: en = max(now, s0 + 0.2)
            default:
                let len = e0 - s0
                s = min(max(0, s0 + now - grab), e.duration - len)
                en = s + len
            }
            switch target {
            case .caption(let id): if let i = e.edit.captions.firstIndex(where: { $0.id == id }) { e.edit.captions[i].start = s; e.edit.captions[i].end = en }
            case .mark(let id): if let i = e.edit.marks.firstIndex(where: { $0.id == id }) { e.edit.marks[i].start = s; e.edit.marks[i].end = en }
            }
            e.seek(edge == 1 ? en - 0.01 : s)
        }
        needsDisplay = true
    }

    override func mouseUp(with ev: NSEvent) {
        let d = drag
        drag = nil
        guard let e = editor else { return }
        switch d {
        case .item: e.edit.captions.sort { $0.start < $1.start }
        case .clipIn(let k, _, let v): e.trimClip(k, in: v)
        case .clipOut(let k, _, let v): e.trimClip(k, out: v)
        case .clipMove(let k, _, let target?): e.moveClip(k, to: target)
        default: break
        }
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let e = editor, let ctx = NSGraphicsContext.current?.cgContext else { return }
        let strip = CGRect(x: 0, y: stripY, width: bounds.width, height: stripH)
        ctx.saveGState()
        ctx.addPath(CGPath(roundedRect: strip, cornerWidth: 6, cornerHeight: 6, transform: nil))
        ctx.clip()
        Theme.surface.setFill()
        strip.fill()
        if !thumbs.isEmpty {
            let w = strip.width / CGFloat(thumbs.count)
            for (i, img) in thumbs.enumerated() {
                let cell = CGRect(x: CGFloat(i) * w, y: stripY, width: w, height: stripH)
                let s = max(cell.width / CGFloat(img.width), cell.height / CGFloat(img.height))
                let dw = CGFloat(img.width) * s, dh = CGFloat(img.height) * s
                ctx.saveGState()
                ctx.clip(to: cell)
                drawImageFlipped(ctx, img, in: CGRect(x: cell.midX - dw / 2, y: cell.midY - dh / 2, width: dw, height: dh))
                ctx.restoreGState()
            }
        }
        NSColor.black.withAlphaComponent(0.65).setFill()
        CGRect(x: 0, y: stripY, width: x(e.edit.trimStart), height: stripH).fill()
        CGRect(x: x(e.edit.trimEnd), y: stripY, width: bounds.width - x(e.edit.trimEnd), height: stripH).fill()
        ctx.restoreGState()

        let kept = CGRect(x: x(e.edit.trimStart), y: stripY, width: x(e.edit.trimEnd) - x(e.edit.trimStart), height: stripH)
        Theme.accent.setStroke()
        let frame = NSBezierPath(roundedRect: kept.insetBy(dx: 1, dy: 1), xRadius: 5, yRadius: 5)
        frame.lineWidth = 2.5
        frame.stroke()
        Theme.accent.setFill()
        for hx in [kept.minX, kept.maxX] {
            NSBezierPath(roundedRect: CGRect(x: hx - 4, y: stripY + 8, width: 8, height: stripH - 16), xRadius: 3, yRadius: 3).fill()
        }
        if laneH > 0 { drawClips(e, strip) }

        func lane(_ r: CGRect, _ empty: String, _ isEmpty: Bool) {
            NSColor.white.withAlphaComponent(0.04).setFill()
            NSBezierPath(roundedRect: r, xRadius: 5, yRadius: 5).fill()
            if isEmpty {
                let s = NSAttributedString(string: empty, attributes: [.font: Theme.font(10), .foregroundColor: Theme.muted])
                s.draw(at: CGPoint(x: 8, y: r.minY + (min(r.height, 22) - s.size().height) / 2))
            }
        }
        func bar(_ r: CGRect, _ label: String, _ symbol: String?, selected: Bool) {
            (selected ? Theme.accent : NSColor.white.withAlphaComponent(0.18)).setFill()
            NSBezierPath(roundedRect: r, xRadius: 4, yRadius: 4).fill()
            ctx.saveGState()
            ctx.clip(to: r.insetBy(dx: 3, dy: 0))
            var x0 = r.minX + 5
            if let symbol, let img = NSImage(systemSymbolName: symbol, accessibilityDescription: nil)?.withSymbolConfiguration(.init(pointSize: 9, weight: .semibold)) {
                let tinted = NSImage(size: img.size, flipped: false) { rr in
                    img.draw(in: rr)
                    (selected ? Theme.onAccent : Theme.text).set()
                    rr.fill(using: .sourceAtop)
                    return true
                }
                tinted.draw(in: CGRect(x: x0, y: r.midY - img.size.height / 2, width: img.size.width, height: img.size.height), from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: nil)
                x0 += img.size.width + 4
            }
            let s = NSAttributedString(string: label, attributes: [.font: Theme.font(10, .medium), .foregroundColor: selected ? Theme.onAccent : Theme.text])
            s.draw(at: CGPoint(x: x0, y: r.midY - s.size().height / 2))
            ctx.restoreGState()
        }
        lane(CGRect(x: 0, y: capY, width: bounds.width, height: capH), "Captions", e.edit.captions.isEmpty)
        for c in e.edit.captions {
            bar(barRect(c.start, c.end, y: capY + 2, h: capH - 4), c.text.isEmpty ? "…" : c.text, nil, selected: c.id == e.selected)
        }
        lane(CGRect(x: 0, y: markY, width: bounds.width, height: rowH * CGFloat(rowCount)), "Text, emoji, callouts, blur, zoom and titles: Add ▾", e.edit.marks.isEmpty)
        for (m, row) in rows() where row < rowCount {
            let label = m.kind.hasText && !m.text.isEmpty ? m.text : m.kind == .step ? "Step \(m.step)" : m.kind.label
            bar(barRect(m.start, m.end, y: markY + CGFloat(row) * rowH + 1, h: rowH - 2), label, m.kind.symbol, selected: m.id == e.selected)
        }

        let px = x(e.now)
        NSColor.white.setFill()
        CGRect(x: px - 1, y: -2, width: 2, height: bounds.height + 2).fill()
        NSBezierPath(ovalIn: CGRect(x: px - 5, y: -4, width: 10, height: 10)).fill()
    }

    // One bar per clip (name and length), cut lines across the thumbnails, and what a drag would do.
    private func drawClips(_ e: VideoEditor, _ strip: CGRect) {
        let starts = VideoSequence.starts(e.edit.clips), h = laneH - 4
        for (i, c) in e.edit.clips.enumerated() {
            let t0 = starts[i], t1 = t0 + c.duration
            var r = CGRect(x: x(t0) + 1, y: 0, width: x(t1) - x(t0) - 2, height: h)
            switch drag {  // the proposed cut
            case .clipIn(i, _, let v)?: r = CGRect(x: x(t0 + v - c.inPoint), y: 0, width: r.maxX - x(t0 + v - c.inPoint), height: h)
            case .clipOut(i, _, let v)?: r.size.width = x(t0 + v - c.inPoint) - r.minX
            default: break
            }
            let sel = e.selected == c.id
            var lifted = false
            if case .clipMove(i, _, _?)? = drag { lifted = true }
            (sel ? Theme.accent.withAlphaComponent(lifted ? 0.45 : 1) : NSColor.white.withAlphaComponent(lifted ? 0.08 : 0.18)).setFill()
            NSBezierPath(roundedRect: r, xRadius: 4, yRadius: 4).fill()
            NSGraphicsContext.current?.cgContext.saveGState()
            NSGraphicsContext.current?.cgContext.clip(to: r.insetBy(dx: 3, dy: 0))
            let label = NSAttributedString(string: "\(c.name)  \(e.clock(c.duration))", attributes: [.font: Theme.font(10, .medium), .foregroundColor: sel ? Theme.onAccent : Theme.text])
            label.draw(at: CGPoint(x: r.minX + 7, y: r.midY - label.size().height / 2))
            NSGraphicsContext.current?.cgContext.restoreGState()
            if sel {  // trim grips
                Theme.onAccent.setFill()
                for hx in [r.minX, r.maxX] { NSBezierPath(roundedRect: CGRect(x: hx - 2, y: 4, width: 4, height: h - 8), xRadius: 2, yRadius: 2).fill() }
            }
            if i > 0 {
                NSColor.black.withAlphaComponent(0.9).setFill()
                CGRect(x: x(t0) - 1, y: strip.minY, width: 2, height: strip.height).fill()
            }
        }
        if case .clipMove(let k, _, let target?)? = drag, e.edit.clips.indices.contains(k) {  // where the clip would go
            var order = e.edit.clips
            order.remove(at: k)
            let mx = x(target < order.count ? VideoSequence.starts(order)[target] : VideoSequence.total(order))
            Theme.accent.setFill()
            CGRect(x: mx - 1.5, y: -2, width: 3, height: strip.maxY + 2).fill()
        }
    }
}
