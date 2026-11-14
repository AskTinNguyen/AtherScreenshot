import AVFoundation
import AppKit
import Carbon.HIToolbox
import Speech

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
    var bin: [Clip] = []       // pieces taken off the timeline, kept to put back: never played or saved
    var overlays: [VideoOverlay] = []   // videos on top (silent), in the order they're drawn
    var frame = CGSize.zero    // the sequence frame: the first video's upright size, kept when clips change

    static let speeds: [Double] = [0.5, 1, 1.5, 2, 4]
    static func speedLabel(_ s: Double) -> String { String(format: "%g×", s) }

    // Speed belongs to each clip (Clip.speed). This reads the speed when every clip shares it (else 1) and sets every
    // clip, as the whole-video speed did before speed per clip.
    var speed: Double {
        get { clips.first.map { s in clips.allSatisfy { $0.speed == s.speed } ? s.speed : 1 } ?? 1 }
        set { for i in clips.indices { clips[i].speed = newValue } }
    }
    // How long timeline [a, b) runs in the output.
    func outputSpan(_ a: Double, _ b: Double) -> Double { b <= a ? 0 : VideoSequence.outputTime(clips, b) - VideoSequence.outputTime(clips, a) }
    var anySpeedChange: Bool { clips.contains { $0.speed != 1 } }
    static let captionScale: [CGFloat] = [0.03, 0.037, 0.045, 0.055, 0.068]
    static let captionSizes = ["Extra small", "Small", "Medium", "Large", "Extra large"]

    var outputDuration: Double { outputSpan(trimStart, trimEnd) }
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

        func time(ofFrame i: Int) -> CMTime { CMTimeMultiply(video.frameDuration, multiplier: Int32(i)) }
    }

    // `probe`: the bench's (it sees every frame the compositor renders, through the renderer box).
    static func prepare(_ e: VideoEdit, probe: Probe? = nil) async throws -> Prepared {
        // Every frame goes through the same renderer the preview uses.
        let renderer = FrameRenderer(edit: e, full: e.frame, preview: false)
        let b = try await VideoSequence.build(e.clips, frame: e.frame, range: e.trimStart...max(e.trimStart, e.trimEnd), muted: e.muted, overlays: e.overlays,
                                              box: RendererBox(renderer, probe: probe))
        return Prepared(composition: b.composition, video: b.video, size: renderer.out)
    }

    // One frame of the edit as the export draws it (crop, captions and markup at timeline time `t`), at the export's size.
    static func frame(_ e: VideoEdit, at t: Double) async throws -> CGImage {
        let b = try await VideoSequence.build(e.clips, frame: e.frame, overlays: e.overlays, box: RendererBox(FrameRenderer(edit: e, full: e.frame, preview: false)))
        let gen = AVAssetImageGenerator(asset: b.composition)
        gen.videoComposition = b.video
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = .zero
        return try await gen.image(at: CMTime(seconds: VideoSequence.outputTime(e.clips, t), preferredTimescale: VideoSequence.timeScale)).image
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
        let seq = try await VideoSequence.build(clips, frame: frame, range: start...max(start, end), atSpeed: false, box: RendererBox(FrameRenderer(edit: VideoEdit(trimEnd: 0), full: frame, preview: true)))
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
    var selected: UUID? { didSet { if selected != oldValue { rebuildInspector(); syncFlipMenu() }; timeline.needsDisplay = true; stage.needsDisplay = true } }
    var cropping = false { didSet { stage.needsDisplay = true; syncToolbar() } }
    var dirty = false
    private var undoStack: [VideoEdit] = []
    private var timeObserver: Any?
    private(set) var busy = false
    private var previewBox: RendererBox?
    private var previewComposition: AVVideoComposition?
    private var builtClips: [Clip] = []          // what the preview player plays now
    private var unplayable: Set<UUID> = []        // clips whose file can't be read: playback skips them
    private var pendingSeek: Double?              // where to go once the rebuilt sequence is in the player
    private var refreshPending = false
    private var builtOverlays: [VideoOverlay] = []    // the videos on top it has (their timing)
    private var playingClips: [Clip] = []        // what the player's item was built from: maps its time to timeline time
    private(set) var builds = 0
    private(set) var reversing = false
    func setBusy(_ b: Bool, reversing r: Bool) { busy = b; reversing = r; syncReverse() }
    var lastReverseError: String?   // tests
    var lastWheelScale = Date.distantPast                    // tests: how many times the player was given a rebuilt sequence

    private let stage = VideoStage()
    var stageView: VideoStage { stage }   // tests
    let timeline = Timeline()
    private let playButton = NSButton()
    let loopButton = NSButton()
    // Loop: playback goes round the selected clip (as much of it as is in the trim), else the trim, instead of stopping.
    // A preview setting, not part of the edit; it stays on for the next video opened.
    static var lastLoop = false
    var loop = VideoEditor.lastLoop { didSet { VideoEditor.lastLoop = loop; syncLoop() } }
    private(set) var loops = 0   // tests: times playback went round
    private let timeLabel = NSTextField(labelWithString: "")
    var reverseButton = NSButton()
    let speedMenu = NSPopUpButton(frame: .zero, pullsDown: true)   // Speed ▸ the selected clip's (else the one at the playhead), Every clip at
    private let captionsMenu = NSPopUpButton(frame: .zero, pullsDown: true)
    private let aspectPopup = NSPopUpButton()
    private let addPopup = NSPopUpButton(frame: .zero, pullsDown: true)
    let smallerMenu = NSPopUpButton(frame: .zero, pullsDown: true)   // Save smaller ▸ Under 30 / 8 / 25 / 50 MB, Other size…
    let copyMenu = NSPopUpButton(frame: .zero, pullsDown: true)      // Copy ▸ Copy clip ⌘C, Copy frame ⌘⇧C
    var pasteboard = NSPasteboard.general                             // where ⌘C puts things (tests pass their own)
    // The marked range (⇧-drag on the timeline, ⇧I / ⇧O): the part ⌘C copies as a clip. A selection, not an edit: on frame
    // boundaries, inside the trim, never in undo or Save. Esc, a plain click outside it or a change to the clips clears it.
    var marked: Range<Double>? { didSet { timeline.needsDisplay = true } }
    private(set) var lastCopiedClip: URL?       // tests
    private(set) var lastCopiedFrame: CGImage?
    private let muteButton = NSButton()
    private let cropButton = NSButton()
    let guidesMenu = NSPopUpButton(frame: .zero, pullsDown: true)   // Guides ▸ golden section, thirds, phi grid, diagonals, Monochrome view
    // Guides and the monochrome view are the preview's only: never saved, and they stay as set for the next video opened.
    static var lastGuides: Set<GuideKind> = [], lastGuidesHidden = false, lastMonochrome = false
    var guides = VideoEditor.lastGuides { didSet { VideoEditor.lastGuides = guides; guidesChanged() } }
    var guidesHidden = VideoEditor.lastGuidesHidden { didSet { VideoEditor.lastGuidesHidden = guidesHidden; guidesChanged() } }
    var monochrome = VideoEditor.lastMonochrome { didSet { VideoEditor.lastMonochrome = monochrome; guidesChanged(); refreshPreview() } }
    var shownGuides: [GuideKind] { guidesHidden ? [] : GuideKind.allCases.filter(guides.contains) }
    let flipMenu = NSPopUpButton(frame: .zero, pullsDown: true)   // Flip ▸ horizontally / vertically: the selected clip (or video on top)
    private let inspector = NSStackView()
    private let hint = NSTextField(labelWithString: "")
    private var actions: [MenuAction] = []
    private var inspectorActions: [MenuAction] = []
    private lazy var timelineHeight = timeline.heightAnchor.constraint(equalToConstant: Timeline.height)
    private lazy var playTop = playButton.topAnchor.constraint(equalTo: timeline.topAnchor, constant: 10)   // beside the clips, under the bin

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
        timeObserver = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 30), queue: .main) { [weak self] t in
            guard let self else { return }
            self.tick(VideoSequence.timelineTime(self.playingClips, t.seconds))
        }
        changed()
    }

    // The preview plays the sequence through the export renderer. Rebuilt when the clips change.
    @MainActor func rebuildPlayer() async {
        let clips = edit.clips, overlays = edit.overlays
        builtClips = clips
        builtOverlays = overlays.map(\.timing)
        duration = VideoSequence.total(clips)
        videoSize = edit.frame
        let box = RendererBox(FrameRenderer(edit: edit, full: videoSize, preview: true))
        do {
            let b = try await VideoSequence.build(clips, frame: edit.frame, overlays: overlays, box: box)
            // Changed again meanwhile: that rebuild wins.
            guard VideoSequence.sameFootage(clips, edit.clips), overlays.map(\.timing) == edit.overlays.map(\.timing) else { return }
            // The playhead stays where playback is (an undo while playing doesn't jump back to where Play was
            // pressed), unless a clip change asked for a spot.
            let at = pendingSeek ?? now
            pendingSeek = nil
            let rate = player.rate
            previewBox = box
            box.renderer = previewRenderer()
            let item = AVPlayerItem(asset: b.composition)
            item.videoComposition = b.video
            item.audioTimePitchAlgorithm = .spectral   // sped-up clips keep their pitch
            playingClips = clips
            previewComposition = b.video
            unplayable = b.unplayable
            player.replaceCurrentItem(with: item)
            await player.seek(to: CMTime(seconds: VideoSequence.outputTime(clips, min(max(0, at), duration)), preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
            if rate != 0 { player.rate = rate }
            builds += 1
            timeline.needsDisplay = true
            stage.needsDisplay = true
            updateTime()
        } catch {
            Toast.shared.show("Can't play this video", error.localizedDescription)
        }
        timeline.loadThumbnails()
    }

    func previewRenderer() -> FrameRenderer {
        let r = FrameRenderer(edit: edit, full: videoSize, preview: true)
        r.zoomInPreview = player.rate != 0
        r.monochrome = monochrome
        return r
    }

    // Hands the latest edit to the preview; while paused, re-renders the current frame.
    private func refreshPreview() {
        guard let box = previewBox else { return }
        box.renderer = previewRenderer()
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

        speedMenu.bezelStyle = .recessed
        speedMenu.menu?.delegate = self
        speedMenu.menu?.autoenablesItems = false
        style(muteButton, "Mute", "speaker.slash", "Remove the sound", toggle: true) { [weak self] in guard let self else { return }; self.pushUndo(); self.edit.muted.toggle() }

        // One "Add" menu instead of a button per tool.
        addPopup.bezelStyle = .recessed
        addPopup.addItem(withTitle: "Add")
        addPopup.item(at: 0)?.image = NSImage(systemSymbolName: "plus", accessibilityDescription: "Add")
        let entries: [(String, String, String, () -> Void)] = [("Caption", "captions.bubble", "T", { [weak self] in self?.addCaption() })]
            + MarkKind.allCases.map { k in (k.label, k.symbol, k.key, { [weak self] in self?.addMark(k) }) }
            + [("Video clip…", "film.stack", "⌘O", { [weak self] in self?.addClipDialog() }),
               ("Video on top (picture in picture)…", "pip", "", { [weak self] in self?.addOverlayDialog(.pip) }),
               ("Video side by side (split screen)…", "rectangle.split.2x1", "", { [weak self] in self?.addOverlayDialog(.splitRight) })]
        for (title, symbol, key, run) in entries {
            let it = NSMenuItem(title: key.isEmpty ? title : "\(title)    \(key)", action: #selector(MenuAction.fire), keyEquivalent: "")
            it.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            let a = MenuAction(run)
            actions.append(a)
            it.target = a
            addPopup.menu?.addItem(it)
            if title == "Caption" || title == "Step number" || title == "Pixelate" || title == MarkKind.allCases.last?.label { addPopup.menu?.addItem(.separator()) }
        }
        addPopup.toolTip = "Add text, emoji, callouts, blur, zoom or a title card at the playhead, join another video, or show one on top or side by side"

        reverseButton = barButton("Reverse", "backward", "Play the video backwards: the end becomes the start") { [weak self] in self?.reverse() }
        let auto = barButton("Auto captions", "waveform.badge.mic", "Transcribe speech into captions, on this Mac") { [weak self] in self?.autoCaptions() }
        captionsMenu.bezelStyle = .recessed
        captionsMenu.toolTip = "Caption look, animation and size, for the whole video"
        rebuildCaptionsMenu()

        smallerMenu.bezelStyle = .recessed
        smallerMenu.toolTip = "Save the edit as an MP4 that fits a size: Discord, email or any limit you type"
        rebuildSmallerMenu()
        buildCopyMenu()
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
        buildFlipMenu()
        guidesMenu.bezelStyle = .recessed
        guidesMenu.toolTip = "Composition guides over the preview (G hides them), and a black-and-white view. Never saved."
        guidesMenu.menu?.delegate = self
        guidesChanged()
        for v in [cropButton, aspectPopup, flipMenu, guidesMenu, separator(), speedMenu, reverseButton, muteButton, separator(), addPopup, auto, captionsMenu, spacer, copyMenu, smallerMenu, gif, save] as [NSView] { top.addArrangedSubview(v) }

        inspector.orientation = .horizontal
        inspector.spacing = 8
        hint.font = Theme.font(11)
        hint.textColor = Theme.muted
        hint.stringValue = "Space plays · I and O trim · S split · ⌫ to bin · ⇧-drag mark · ⌘C copy · T caption · A arrow · R box · E emoji · N step · X blur · Z zoom · C crop · drop videos to join them"

        playButton.bezelStyle = .regularSquare
        playButton.isBordered = false
        playButton.image = NSImage(systemSymbolName: "play.fill", accessibilityDescription: "Play")
        playButton.symbolConfiguration = .init(pointSize: 16, weight: .semibold)
        playButton.contentTintColor = Theme.text
        playButton.toolTip = "Play / pause (Space)"
        bind(playButton) { [weak self] in self?.togglePlay() }
        loopButton.bezelStyle = .regularSquare
        loopButton.isBordered = false
        loopButton.wantsLayer = true
        loopButton.layer?.cornerRadius = 6
        loopButton.image = NSImage(systemSymbolName: "repeat", accessibilityDescription: "Loop")
        loopButton.symbolConfiguration = .init(pointSize: 13, weight: .semibold)
        bind(loopButton) { [weak self] in self?.loop.toggle() }
        syncLoop()
        timeLabel.font = Theme.mono(11)
        timeLabel.textColor = Theme.textDim
        timeLabel.maximumNumberOfLines = 2

        for v in [top, stage, inspector, hint, playButton, loopButton, timeLabel, timeline] as [NSView] {
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
            playTop,
            playButton.widthAnchor.constraint(equalToConstant: 30),
            loopButton.leadingAnchor.constraint(equalTo: playButton.trailingAnchor, constant: 6),
            loopButton.centerYAnchor.constraint(equalTo: playButton.centerYAnchor),
            loopButton.widthAnchor.constraint(equalToConstant: 30),
            loopButton.heightAnchor.constraint(equalToConstant: 26),
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

    func actionsKeep(_ a: MenuAction) { actions.append(a) }

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
        let ci = selectedCaptionIndex, mi = selectedMarkIndex, k = selectedClipIndex, bi = selectedBinIndex, oi = selectedOverlayIndex
        inspector.isHidden = ci == nil && mi == nil && k == nil && bi == nil && oi == nil
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
        func rowLabel(_ text: String) {
            let label = NSTextField(labelWithString: text)
            label.font = Theme.font(12)
            label.textColor = Theme.text
            label.lineBreakMode = .byTruncatingMiddle
            inspector.addArrangedSubview(label)
        }
        func button(_ title: String, _ symbol: String, _ tip: String, _ run: @escaping () -> Void) {
            let b = NSButton(title: title, image: NSImage(systemSymbolName: symbol, accessibilityDescription: title) ?? NSImage(), target: nil, action: nil)
            b.bezelStyle = .recessed
            b.imagePosition = .imageLeading
            b.toolTip = tip
            link(b, run)
            inspector.addArrangedSubview(b)
        }
        if let k, ci == nil, mi == nil {
            let c = edit.clips[k]
            rowLabel("Clip \(k + 1) of \(edit.clips.count):  \(c.displayName)  ·  \(clock(c.duration))")
            button("Split at playhead", "scissors", "Cut this clip in two at the playhead (S)") { [weak self] in self?.splitAtPlayhead() }
            if k > 0 { button("Earlier", "arrow.left", "Play this clip before the one on its left") { [weak self] in self?.moveClip(k, to: k - 1) } }
            if k + 1 < edit.clips.count { button("Later", "arrow.right", "Play this clip after the one on its right") { [weak self] in self?.moveClip(k, to: k + 1) } }
            if edit.clips.count > 1 { button("To bin", "tray.and.arrow.up", "Take this clip out into the bin, to put back later (⌫, or drag it up)") { [weak self] in self?.moveToBin(k) } }
        }
        if let oi {
            let o = edit.overlays[oi]
            rowLabel("Video on top:  \(o.clip.displayName)  ·  \(clock(o.clip.duration))  ·  no sound")
            let layout = popup(OverlayLayout.allCases.map(\.label), o.layout.rawValue) { [weak self] i in
                self?.updateOverlay { $0.layout = OverlayLayout(rawValue: i) ?? .pip }
                self?.rebuildInspector()
            }
            layout.toolTip = "In a box on top, or side by side with the video"
            inspector.addArrangedSubview(layout)
            if o.isSplit {
                inspector.addArrangedSubview(popup(["Fit: all of each picture, with bars", "Fill: each half filled, cut to fit"], o.fill ? 1 : 0) { [weak self] i in
                    self?.updateOverlay { $0.fill = i == 1 }
                })
            } else {
                let cur = VideoOverlay.sizes.indices.min { abs(VideoOverlay.sizes[$0].1 - o.width) < abs(VideoOverlay.sizes[$1].1 - o.width) } ?? 1
                let size = popup(VideoOverlay.sizes.map(\.0), cur) { [weak self] i in self?.updateOverlay { $0.width = VideoOverlay.sizes[i].1 } }
                size.toolTip = "Its size (or drag its corner on the video, or scroll over it)"
                inspector.addArrangedSubview(size)
            }
            button("To bin", "tray.and.arrow.up", "Take it off into the bin (⌫)") { [weak self] in self?.deleteSelected() }
        }
        if let bi, ci == nil, mi == nil {
            let c = edit.bin[bi]
            rowLabel("In the bin:  \(c.displayName)  ·  \(clock(c.inPoint))–\(clock(c.outPoint))  ·  \(clock(c.duration))")
            button("Add at end", "plus", "Put it back at the end of the video (or drag it onto the timeline)") { [weak self] in
                guard let self else { return }
                self.placeFromBin(bi, at: self.edit.clips.count)
            }
            button("On top", "pip", "Show it in a box on top of the video, from the playhead") { [weak self] in
                guard let self else { return }
                self.binToOverlay(bi, at: self.insertTime)
            }
            button("Delete", "trash", "Delete this piece for good (⌫)") { [weak self] in self?.deleteFromBin(bi) }
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
    // Save smaller's sizes: the last one used first (Compress.sizes), then Other size….
    func rebuildSmallerMenu() {
        let menu = smallerMenu.menu!
        menu.removeAllItems()
        actions.removeAll { $0.tag == 2 }
        menu.addItem(withTitle: "Save smaller", action: nil, keyEquivalent: "")
        smallerMenu.item(at: 0)?.image = NSImage(systemSymbolName: "arrow.down.right.and.arrow.up.left", accessibilityDescription: "Save smaller")
        func item(_ t: String, _ run: @escaping () -> Void) {
            let it = NSMenuItem(title: t, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction(run)
            a.tag = 2
            actions.append(a)
            it.target = a
            menu.addItem(it)
        }
        for mb in Compress.sizes(for: [url]) {
            item("Under \(Compress.label(mb))" + (mb == 8 ? "  (Discord)" : mb == 25 ? "  (email)" : "")) { [weak self] in self?.saveSmaller(mb) }
        }
        menu.addItem(.separator())
        item("Other size…") { [weak self] in self?.askSmallerSize() }
    }

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
        player.rate = 1   // each clip at its own speed
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

    // The player plays the output (each clip at its speed); the editor works in timeline time.
    var now: Double { player.currentTime().seconds.isFinite ? VideoSequence.timelineTime(playingClips, player.currentTime().seconds) : 0 }
    var selectedCaptionIndex: Int? { selected.flatMap { id in edit.captions.firstIndex { $0.id == id } } }
    var selectedMarkIndex: Int? { selected.flatMap { id in edit.marks.firstIndex { $0.id == id } } }
    var selectedClipIndex: Int? { selected.flatMap { id in edit.clips.firstIndex { $0.id == id } } }
    var selectedOverlayIndex: Int? { selected.flatMap { id in edit.overlays.firstIndex { $0.id == id } } }
    var selectedBinIndex: Int? { selected.flatMap { id in edit.bin.firstIndex { $0.id == id } } }   // a clip keeps its id going to the bin and back
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
        if let s = selected, !edit.captions.contains(where: { $0.id == s }) && !edit.marks.contains(where: { $0.id == s }) && !edit.clips.contains(where: { $0.id == s })
            && !edit.bin.contains(where: { $0.id == s }) && !edit.overlays.contains(where: { $0.id == s }) { selected = nil }
        rebuildInspector()
    }

    private func changed() {
        duration = VideoSequence.total(edit.clips)
        let footageChanged = !VideoSequence.sameFootage(edit.clips, builtClips)
        if footageChanged || edit.overlays.map(\.timing) != builtOverlays, !edit.clips.isEmpty {   // a flip alone doesn't rebuild the player
            builtClips = edit.clips
            builtOverlays = edit.overlays.map(\.timing)
            if footageChanged { marked = nil }   // its times would now show other footage
            Task { @MainActor in await self.rebuildPlayer() }
        }
        if let m = marked { setMarked(m.lowerBound, m.upperBound) }   // still inside the trim
        if timelineHeight.constant != timeline.wantedHeight { timelineHeight.constant = timeline.wantedHeight }
        if playTop.constant != timeline.laneY + 10 { playTop.constant = timeline.laneY + 10 }
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
        syncFlipMenu()
        muteButton.state = edit.muted ? .on : .off
        syncSpeedMenu()
        syncReverse()
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
        if player.rate != 0 {
            let (from, to) = loop ? loopRange : (edit.trimStart, edit.trimEnd)
            if loop && t >= to - 0.01 {   // round again
                loops += 1
                seek(from)
            } else if !loop && t >= edit.trimEnd - 0.01 {
                player.pause()
                seek(edit.trimStart)
                playStateChanged()
            }
        }
        timeline.needsDisplay = true
        stage.needsDisplay = true
        updateTime()
    }

    func togglePlay() {
        if player.rate != 0 {
            player.pause()
        } else {
            let (from, to) = loop ? loopRange : (edit.trimStart, edit.trimEnd)
            if now < from || now >= to - 0.05 { seek(from) }
            player.rate = 1   // each clip at its own speed
        }
        playStateChanged()
    }

    var loopRange: (Double, Double) {
        var a = edit.trimStart, b = edit.trimEnd
        if let i = selectedClipIndex {
            let c0 = VideoSequence.starts(edit.clips)[i], c1 = c0 + edit.clips[i].duration
            if max(a, c0) < min(b, c1) - 0.05 { a = max(a, c0); b = min(b, c1) }
        }
        return (a, b)
    }

    private func syncLoop() {
        loopButton.contentTintColor = loop ? Theme.accent : Theme.textDim
        loopButton.layer?.backgroundColor = loop ? Theme.selected.cgColor : NSColor.clear.cgColor
        loopButton.toolTip = loop ? "Loop is on (⌘L): plays the selected clip, or the trim, over and over"
                                  : "Loop (⌘L): play the selected clip, or the trim, over and over"
    }

    private func playStateChanged() {
        playButton.image = NSImage(systemSymbolName: player.rate != 0 ? "pause.fill" : "play.fill", accessibilityDescription: "Play")
        refreshPreview()   // zoom shows while playing
    }

    func seek(_ t: Double) {
        player.seek(to: CMTime(seconds: VideoSequence.outputTime(playingClips, min(max(0, t), duration)), preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
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

    // MARK: bin

    // One undo step for the clips and the bin together (a piece going from one to the other).
    func setClipsAndBin(_ clips: [Clip], _ bin: [Clip], seekTo t: Double? = nil) {
        guard !clips.isEmpty, clips != edit.clips || bin != edit.bin else { return }
        player.pause()
        playStateChanged()
        pushUndo()
        var e = edit
        e.bin = bin
        e.applyClips(clips)
        if clips != edit.clips { pendingSeek = t }
        edit = e
        if let t, clips == builtClips { seek(t) }
    }

    // Takes a clip off the timeline into the bin, to put back later or delete there. The last clip stays. Markup and
    // captions on it go, as when a clip is cut shorter.
    func moveToBin(_ i: Int) {
        guard edit.clips.indices.contains(i) else { return }
        guard edit.clips.count > 1 else { return Toast.shared.show("The last clip stays on the timeline", ms: 2000) }
        var clips = edit.clips, bin = edit.bin
        bin.append(clips.remove(at: i))
        setClipsAndBin(clips, bin, seekTo: VideoSequence.starts(clips)[min(i, clips.count - 1)])
        selected = nil
    }

    // Puts bin piece `i` back on the timeline as clip `at`.
    func placeFromBin(_ i: Int, at: Int) {
        guard edit.bin.indices.contains(i) else { return }
        var clips = edit.clips, bin = edit.bin
        let c = bin.remove(at: i)
        let k = min(max(0, at), clips.count)
        clips.insert(c, at: k)
        setClipsAndBin(clips, bin, seekTo: VideoSequence.starts(clips)[k])
        selected = c.id
    }

    // Deletes bin piece `i` for good (undo brings it back).
    func deleteFromBin(_ i: Int) {
        guard edit.bin.indices.contains(i) else { return }
        pushUndo()
        edit.bin.remove(at: i)
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

    var insertTime: Double { min(max(now, edit.trimStart), max(edit.trimStart, edit.trimEnd - 0.5)) }

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
        if let k = selectedClipIndex { return moveToBin(k) }   // Del takes a clip out into the bin
        if let k = selectedBinIndex { return deleteFromBin(k) }
        guard selected != nil else { return }
        pushUndo()
        if let o = edit.overlays.first(where: { $0.id == selected }) {   // a video on top's footage goes to the bin, like a clip's
            var c = o.clip
            c.id = UUID()
            edit.bin.append(c)
            edit.overlays.removeAll { $0.id == o.id }
        }
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

    // Save smaller: the edit as a new MP4 in the captures folder that fits in `mb` (Compress.edit: H.264 at the rate that
    // fits its length, scaled down when that's low, made again smaller when it comes out over).
    private(set) var lastSmallerError: String?   // tests
    func saveSmaller(_ mb: Double) {
        guard !busy, mb > 0 else { return }
        window.makeFirstResponder(stage)   // commits a field being edited
        busy = true
        player.pause()
        playStateChanged()
        Settings.shared.set("CompressVideoMB", mb)   // offered first next time, here and in the gallery
        rebuildSmallerMenu()
        let src = Library.shared.meta(url)
        let info = NameInfo(app: src.app, window: src.window)
        let label = Compress.label(mb)
        let toast = Toast.shared.post("Saving under \(label)…", "\(clock(edit.outputDuration)) long", ms: 600_000)
        let e = edit
        Task { @MainActor in
            defer { self.busy = false }
            var tmp: URL?
            do {
                var r = try await Compress.edit(e, cap: Compress.bytes(mb: mb)) { f, attempt in
                    let body = "\(Int(f * 100))%" + (attempt > 0 ? ", smaller try \(attempt + 1)" : "")
                    DispatchQueue.main.async { Toast.shared.update(toast, body: body) }
                }
                tmp = r.tmp
                let out = Output.newCaptureURL(ext: "mp4", info: info)
                r.out = out
                Library.shared.noteEdit(out, from: self.url, info: info, edited: true)
                try Compress.finish(&r)
                self.dirty = false
                self.lastSmallerError = nil
                let size = ByteCountFormatter.string(fromByteCount: Int64(Compress.fileBytes(out)), countStyle: .file)
                Toast.shared.show("Saved under \(label)", "\(out.lastPathComponent)  ·  \(size)  ·  click to show in Finder") { Output.reveal(out) }
            } catch {
                if let tmp { try? FileManager.default.removeItem(at: tmp) }
                self.lastSmallerError = error.localizedDescription
                Toast.shared.show("Couldn't save under \(label)", error.localizedDescription, ms: 8000)
            }
        }
    }

    private func askSmallerSize() {
        let last = Compress.sizes(for: [url]).first ?? Compress.defaultVideoMB
        Palette.shared.prompt("Save smaller: under how many MB?  ·  ↩ saves, ⎋ cancels", initial: String(format: "%g", last)) { [weak self] text in
            let mb = Compress.parseMegabytes(text)
            guard mb > 0 else { return Toast.shared.show("Type a size in MB", "For example 10 or 2.5") }
            self?.saveSmaller(mb)
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
            case kVK_ANSI_C: shift ? copyFrame() : copyPicked()
            case kVK_ANSI_Z: undo()
            case kVK_ANSI_L: loop.toggle()
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
        case kVK_ANSI_I: if shift { markAtPlayhead(start: true) } else { pushUndo(); setTrim(start: now) }   // ⇧: the marked range
        case kVK_ANSI_O: if shift { markAtPlayhead(start: false) } else { pushUndo(); setTrim(end: now) }
        case kVK_ANSI_T: addCaption()
        case kVK_ANSI_A: addMark(.arrow)
        case kVK_ANSI_R: addMark(.box)
        case kVK_ANSI_E: addMark(.emoji)
        case kVK_ANSI_N: addMark(.step)
        case kVK_ANSI_X: addMark(.blur)
        case kVK_ANSI_Z: addMark(.zoom)
        case kVK_ANSI_C: cropping.toggle()
        case kVK_ANSI_G: toggleGuides()
        case kVK_Delete, kVK_ForwardDelete: deleteSelected()
        case kVK_Escape:
            if marked != nil { marked = nil } else if cropping { cropping = false } else if selected != nil { selected = nil } else { window.performClose(nil) }
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

// MARK: - Copy (⌘C): the marked range or the selected clip as an H.264 clip, or the frame on screen

extension VideoEditor: NSMenuDelegate {
    // Copy ▾ next to Save: both copies, so the keys can be found.
    fileprivate func buildCopyMenu() {
        copyMenu.bezelStyle = .recessed
        copyMenu.toolTip = "Copy the marked range (⇧-drag on the timeline) or the selected clip as an MP4 to paste into Slack, or the frame on screen as a picture"
        let menu = copyMenu.menu!
        menu.autoenablesItems = false
        menu.delegate = self
        menu.addItem(withTitle: "Copy", action: nil, keyEquivalent: "")
        copyMenu.item(at: 0)?.image = NSImage(systemSymbolName: "doc.on.doc", accessibilityDescription: "Copy")
        let entries: [(String, String, () -> Void)] = [("Copy clip    ⌘C", "film", { [weak self] in self?.copyPicked() }),
                                                       ("Copy frame    ⌘⇧C", "photo", { [weak self] in self?.copyFrame() })]
        for (title, symbol, run) in entries {
            let it = NSMenuItem(title: title, action: #selector(MenuAction.fire), keyEquivalent: "")
            it.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            let a = MenuAction(run)
            actions.append(a)
            it.target = a
            menu.addItem(it)
        }
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        if menu === copyMenu.menu { copyMenu.item(at: 1)?.isEnabled = canCopyClip }   // Copy clip: with a range or a selected clip
        if menu === flipMenu.menu { syncFlipMenu() }
        if menu === speedMenu.menu { syncSpeedMenu() }
        if menu === guidesMenu.menu { fillGuidesMenu(menu, title: true) }
    }

    // The timeline's frame rate: the first clip's, as the preview and the export step through it (at most 60).
    var frameRate: Double { edit.clips.first.map { $0.fps > 1 ? min(60, $0.fps.rounded()) : 30 } ?? 30 }

    // The frame boundary nearest `t` (the end of the timeline counts as one).
    func snapEdge(_ t: Double) -> Double {
        let f = min(duration, (t * frameRate).rounded() / frameRate)
        return abs(duration - t) < abs(f - t) ? duration : f
    }

    // From `a` to `b` (either way round), inside the trim; none when that leaves less than a frame.
    func setMarked(_ a: Double, _ b: Double) {
        let from = max(min(a, b), edit.trimStart), to = min(max(a, b), edit.trimEnd)
        marked = to - from < 0.5 / frameRate ? nil : from..<to
    }

    // ⇧I / ⇧O: the range starts or ends at the playhead, as I and O set the trim. The other end stays, or is the trim's
    // when there's no range yet or it would come out empty.
    func markAtPlayhead(start: Bool) {
        let t = snapEdge(now)
        var from = marked?.lowerBound ?? edit.trimStart, to = marked?.upperBound ?? edit.trimEnd
        if start {
            from = t
            if to <= from { to = edit.trimEnd }
        } else {
            to = t
            if to <= from { from = edit.trimStart }
        }
        setMarked(from, to)
    }

    // "6.2 s · ⌘C copies": how long the copied clip runs (at the edit's speed).
    var markedLabel: String { marked.map { "\(lengthText(edit.outputSpan($0.lowerBound, $0.upperBound))) · ⌘C copies" } ?? "" }
    func lengthText(_ s: Double) -> String { s < 60 ? String(format: "%.1f s", s) : clock(s) }

    var canCopyClip: Bool { marked != nil || selectedClipIndex != nil }

    // ⌘C: the marked range as a clip; with no range, the clip selected in the clip lane (as edited, inside the trim);
    // else the frame on screen.
    func copyPicked() {
        if let r = marked { return copyClip(r) }
        if let k = selectedClipIndex {
            let t0 = VideoSequence.starts(edit.clips)[k]
            let a = max(t0, edit.trimStart), b = min(t0 + edit.clips[k].duration, edit.trimEnd)
            guard b - a >= 0.5 / frameRate else {
                return Toast.shared.show("This clip is outside the trim", "Nothing of it would be saved. Mark a range with ⇧-drag instead.", ms: 3000)
            }
            return copyClip(a..<b)
        }
        copyFrame()
    }

    // Copy clip: the edit over the range saved as Save would write that part (crop, speed, mute, captions and markup), but
    // as H.264 + AAC (Compress.clip), into the captures folder, stacked with the original in the gallery; then the file goes
    // on the pasteboard, so ⌘V in Slack, Teams or Finder attaches it.
    func copyClip(_ r: Range<Double>) {
        guard !busy else { return }
        window.makeFirstResponder(stage)   // commits a field being edited
        busy = true
        player.pause()
        playStateChanged()
        let src = Library.shared.meta(url)
        let info = NameInfo(app: src.app, window: src.window)
        var e = edit
        e.trimStart = r.lowerBound
        e.trimEnd = r.upperBound - 1e-4   // the frame starting right at the end stays out, whatever the rounding
        let length = lengthText(e.outputSpan(r.lowerBound, r.upperBound))
        let toast = Toast.shared.post("Copying clip…", "\(length) long", ms: 600_000)
        let pb = pasteboard
        Task { @MainActor in
            defer { self.busy = false }
            do {
                let tmp = try await Compress.clip(e) { f in DispatchQueue.main.async { Toast.shared.update(toast, body: "\(Int(f * 100))%") } }
                let out = Output.newCaptureURL(ext: "mp4", info: info)
                Library.shared.noteEdit(out, from: self.url, info: info, edited: true)
                do { try FileManager.default.moveItem(at: tmp, to: out) } catch { try? FileManager.default.removeItem(at: tmp); throw error }
                self.lastCopiedClip = out
                let size = ByteCountFormatter.string(fromByteCount: Int64(Compress.fileBytes(out)), countStyle: .file)
                if copyFile(out, to: pb) {
                    Toast.shared.show("Clip copied · \(length) · \(size)", "Paste it in Slack or anywhere  ·  click to show in Finder") { Output.reveal(out) }
                } else {
                    Toast.shared.show("Clip saved, but the clipboard was busy", "\(out.lastPathComponent)  ·  click to show it in Finder and drag it in instead", ms: 8000) { Output.reveal(out) }
                }
            } catch {
                Toast.shared.show("Copy clip failed", error.localizedDescription, ms: 8000)
            }
        }
    }

    // Copy frame (⌘⇧C, or ⌘C with nothing picked): the frame on screen as the export draws it (crop, captions and markup at
    // that time), at the export's size, on the pasteboard as PNG and TIFF. Not saved to a file.
    func copyFrame() {
        let e = edit, t = now, pb = pasteboard
        Task { @MainActor in
            do {
                let img = try await VideoExport.frame(e, at: t)
                guard copyImage(img, to: pb) else { return Toast.shared.show("Couldn't copy the frame", "The clipboard is busy. Try again.", ms: 3000) }
                self.lastCopiedFrame = img
                Toast.shared.show("Frame copied", "\(self.clock(t))  ·  \(img.width) × \(img.height)", image: img)
            } catch {
                Toast.shared.show("Couldn't copy the frame", error.localizedDescription, ms: 6000)
            }
        }
    }
}

// MARK: - Stage (video, crop, markup handles)

final class VideoStage: NSView {
    weak var editor: VideoEditor?
    private let playerView = PlayerView()
    var playerLayer: AVPlayerLayer { playerView.playerLayer }
    // Guides and handles draw in a view above the video; a layer-backed view's own drawing sits under its sublayers.
    private let overlay = StageOverlay()
    private enum Drag {
        case crop(CGPoint), move(UUID, CGPoint, Mark), handle(UUID, Int, Mark), caption(UUID, CGPoint, CGRect)
        case overlayBox(UUID, handle: Int, from: CGPoint, VideoOverlay)   // handle 0 moves it; 1–4 (corners) size it
    }

    // A video on top under `vp` (video coordinates), topmost first: a corner of the selected box (1 top-left,
    // 2 top-right, 3 bottom-left, 4 bottom-right: to size it), or a box or a split's half (0: to move or select it).
    func overlayAt(_ vp: CGPoint) -> (Int, Int)? {
        guard let ed = editor else { return nil }
        let t = ed.now, slop = 12 / max(0.01, scale)
        if let i = ed.selectedOverlayIndex, ed.edit.overlays[i].active(t), !ed.edit.overlays[i].isSplit {
            let r = ed.overlayOnVideo(ed.edit.overlays[i])
            let corners = [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)]
            if let k = corners.firstIndex(where: { hypot($0.x - vp.x, $0.y - vp.y) <= slop }) { return (i, k + 1) }
        }
        for i in ed.edit.overlays.indices.reversed() where ed.edit.overlays[i].active(t) && ed.overlayOnVideo(ed.edit.overlays[i]).contains(vp) { return (i, 0) }
        return nil
    }

    // The wheel over a box sizes it.
    override func scrollWheel(with e: NSEvent) {
        guard let ed = editor, let (i, _) = overlayAt(toVideo(convert(e.locationInWindow, from: nil))), !ed.edit.overlays[i].isSplit, e.scrollingDeltaY != 0 else {
            return super.scrollWheel(with: e)
        }
        ed.scaleOverlay(i, by: e.scrollingDeltaY > 0 ? 1.08 : 1 / 1.08)
    }
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
        // Videos on top are drawn over the markup, so they're picked first; cropping goes on around them (and over one
        // that isn't selected).
        if let (i, h) = overlayAt(vp), !ed.cropping || ed.selectedOverlayIndex == i {
            let o = ed.edit.overlays[i]
            ed.cropping = false
            ed.selected = o.id
            if o.isSplit { return }
            ed.pushUndo()
            drag = .overlayBox(o.id, handle: h, from: vp, o)
            return
        }
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
        case .overlayBox(let id, let h, let from, let o0):   // the box moves, or (its corner) grows and shrinks
            guard let i = ed.edit.overlays.firstIndex(where: { $0.id == id }) else { return }
            let v = ed.viewRect
            var o = ed.edit.overlays[i]
            if h == 0 {
                o.pos = CGPoint(x: o0.pos.x + (p.x - from.x) / v.width, y: o0.pos.y + (p.y - from.y) / v.height)
            } else {   // a corner: the opposite one stays put, the shape stays the video's
                let r0 = o0.rect(v.size)
                let left = h == 1 || h == 3, top = h <= 2
                let ax = left ? r0.maxX : r0.minX, ay = top ? r0.maxY : r0.minY
                let shape = r0.height / max(1, r0.width)
                let px = p.x - v.minX, py = p.y - v.minY
                var w = max(left ? ax - px : px - ax, (top ? ay - py : py - ay) / shape)
                w = max(0.08 * v.width, min(w, min(left ? ax : v.width - ax, (top ? ay : v.height - ay) / shape)))   // stays on the frame
                o.width = w / v.width
                o.pos = CGPoint(x: (left ? ax - w : ax) / v.width, y: (top ? ay - w * shape : ay) / v.height)
            }
            let r = o.rect(v.size)   // kept on the frame
            o.pos = CGPoint(x: r.minX / v.width, y: r.minY / v.height)
            ed.edit.overlays[i] = o
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

    // A right click on the video: the Guides menu.
    override func menu(for event: NSEvent) -> NSMenu? {
        guard let ed = editor else { return nil }
        let m = NSMenu()
        ed.fillGuidesMenu(m, title: false)
        return m
    }

    // The guides over the part the saved video shows (the crop), with a dark edge so they read on any picture.
    private func drawGuides(_ ed: VideoEditor, _ ctx: CGContext) {
        let r = toView(ed.viewRect)
        for g in ed.shownGuides {
            let lines = g.lines(r)
            for (width, color) in [(3.0, NSColor.black.withAlphaComponent(0.45)), (1.0, g.color)] as [(CGFloat, NSColor)] {
                ctx.setStrokeColor(color.cgColor)
                ctx.setLineWidth(width)
                for (a, b) in lines { ctx.strokeLineSegments(between: [a, b]) }
            }
        }
    }

    fileprivate func drawOverlay() {
        guard let ed = editor, let ctx = NSGraphicsContext.current?.cgContext else { return }
        let v = videoRect
        if !ed.shownGuides.isEmpty && !ed.cropping { drawGuides(ed, ctx) }
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
        // A selected video on top: its box (or half) and, for a box, the corners that size it.
        if let i = ed.selectedOverlayIndex, ed.edit.overlays[i].active(ed.now) {
            let o = ed.edit.overlays[i]
            let r = toView(ed.overlayOnVideo(o))
            ctx.setStrokeColor(Theme.accent.cgColor)
            ctx.setLineWidth(1.5)
            ctx.stroke(r.insetBy(dx: 0.75, dy: 0.75))
            if !o.isSplit {
                for c in [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)] {
                    let box = CGRect(x: c.x - 5, y: c.y - 5, width: 10, height: 10)
                    ctx.setFillColor(NSColor.white.cgColor)
                    ctx.fillEllipse(in: box)
                    ctx.setStrokeColor(Theme.accent.cgColor)
                    ctx.strokeEllipse(in: box)
                }
            }
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
    private var thumbTimes: [Double] = []   // the timeline time each thumbnail shows (it follows its clip's flip)
    private enum Target { case caption(UUID), mark(UUID) }
    private enum Drag {
        case start, end, playhead, item(Target, edge: Int, grab: Double, start: Double, end: Double)
        case clipIn(Int, grab: Double, value: Double), clipOut(Int, grab: Double, value: Double)  // applied on mouse-up
        case clipMove(Int, down: CGPoint, target: Int?, toBin: Bool)   // dragged up over the bin: it goes there
        case binMove(Int, down: CGPoint, at: CGPoint, target: Int?, moved: Bool, toOverlay: Bool)  // a bin piece carried onto the clip lane or on top
        case overlayItem(UUID, edge: Int, grab: Double, VideoOverlay)   // a video on top's bar: moved, or (an end) trimmed
        case range(anchor: Double, before: Range<Double>?)   // marking a range (⇧-drag, or a grip): Esc puts `before` back
    }
    private var drag: Drag?

    static let height: CGFloat = 90 + 3 * 18
    private let stripH: CGFloat = 52
    private let capH: CGFloat = 22
    private let rowH: CGFloat = 18
    // The clip lane sits above the thumbnails, and the bin lane above that (clear of the playhead knob), once there's
    // more than one clip or anything in the bin.
    var lanesShown: Bool { (editor?.edit.clips.count ?? 0) > 1 || !(editor?.edit.bin.isEmpty ?? true) }
    var binH: CGFloat { lanesShown ? 30 : 0 }
    var laneY: CGFloat { binH > 0 ? binH + 12 : 0 }   // the top of the timeline proper: the clip lane, or the thumbnails
    var laneH: CGFloat { lanesShown ? 24 : 0 }
    private var stripY: CGFloat { laneY + laneH }
    private var capY: CGFloat { stripY + 62 }
    private var markY: CGFloat { stripY + 90 }

    var isDragging: Bool { drag != nil }

    // Esc, or the window losing the mouse mid-drag. Item drags keep what they did (one undo step);
    // clip drags only apply on release, so nothing changes.
    func cancelDrag() {
        guard drag != nil else { return }
        if case .range(_, let before)? = drag { editor?.marked = before }
        drag = nil
        needsDisplay = true
    }

    // Videos dropped on the lane of videos on top go on top there; anywhere else on the timeline they join the clips.
    private func videos(_ info: NSDraggingInfo) -> [URL] {
        (info.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []).filter(MediaFiles.isVideo)
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { videos(sender).isEmpty ? [] : .copy }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let v = videos(sender)
        guard let e = editor, !v.isEmpty else { return false }
        let p = convert(sender.draggingLocation, from: nil)
        if p.y >= ovY - 6 { e.addOverlay(v[0], at: t(p.x), layout: .pip) } else { e.addClips(v) }
        return true
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        registerForDraggedTypes([.fileURL])
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
            var out: [CGImage] = [], times: [Double] = []
            for i in 0..<count {
                guard !Task.isCancelled else { return }
                let tt = total * (Double(i) + 0.5) / Double(count)
                guard let (k, ft) = VideoSequence.locate(clips, tt) else { continue }
                let path = clips[k].path
                let gen = gens[path] ?? {
                    let g = AVAssetImageGenerator(asset: AVURLAsset(url: clips[k].url))
                    g.appliesPreferredTrackTransform = true
                    g.maximumSize = CGSize(width: 240, height: 240)
                    gens[path] = g
                    return g
                }()
                if let img = try? await gen.image(at: CMTime(seconds: ft, preferredTimescale: 600)).image { out.append(img); times.append(tt) }
            }
            guard !Task.isCancelled else { return }
            self.thumbs = out
            self.thumbTimes = times
            self.needsDisplay = true
        }
    }

    // Where a clip dropped at `px` lands: how many of the clips (other than `skip`) end up before it.
    func insertIndex(_ px: CGFloat, skip: Int?) -> Int {
        guard let e = editor else { return 0 }
        let starts = VideoSequence.starts(e.edit.clips)
        return e.edit.clips.indices.filter { $0 != skip && x(starts[$0] + e.edit.clips[$0].duration / 2) < px }.count
    }

    // Each bin piece's chip, left to right after the "Bin" label: as long as on the timeline, shorter when they don't all fit.
    static let binLabelW: CGFloat = 34
    func binChips() -> [CGRect] {
        guard let e = editor, !e.edit.bin.isEmpty, binH > 0 else { return [] }
        let n = CGFloat(e.edit.bin.count), gap: CGFloat = 4, minW: CGFloat = 64, left = Timeline.binLabelW
        let avail = max(n, bounds.width - left - gap * (n + 1))
        let total = e.edit.bin.reduce(0) { $0 + $1.duration }
        let k = min(bounds.width / CGFloat(max(0.001, e.duration)), avail / CGFloat(max(0.001, total)))
        var w = e.edit.bin.map { max(minW, CGFloat($0.duration) * k) }
        if w.reduce(0, +) > avail { w = w.map { _ in avail / n } }
        var x0 = left + gap
        return w.map { wi in defer { x0 += wi + gap }; return CGRect(x: x0, y: 3, width: wi, height: binH - 6) }
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
    // The videos on top: a lane at the bottom, under the markup's rows.
    var ovY: CGFloat { markY + CGFloat(rowCount) * rowH + 4 }
    let ovH: CGFloat = 22
    var wantedHeight: CGFloat { ovY + ovH }

    private func barRect(_ s: Double, _ e: Double, y: CGFloat, h: CGFloat) -> CGRect {
        CGRect(x: x(s), y: y, width: max(6, x(e) - x(s)), height: h)
    }

    override func mouseDown(with ev: NSEvent) {
        window?.makeFirstResponder(self)
        guard let e = editor else { return }
        let p = convert(ev.locationInWindow, from: nil)
        if ev.modifierFlags.contains(.shift) { return beginMark(e, p, anchor: nil) }   // ⇧-drag anywhere on the timeline marks a range
        if let m = e.marked {
            let d0 = abs(p.x - x(m.lowerBound)), d1 = abs(p.x - x(m.upperBound))
            if p.y >= stripY, p.y < stripY + stripH, min(d0, d1) < 6 { return beginMark(e, p, anchor: d0 <= d1 ? m.upperBound : m.lowerBound) }   // its grips
            if !m.contains(t(p.x)) { e.marked = nil }   // a plain click outside it clears it
        }
        // The bin lane: a click selects a piece, a drag carries it onto the timeline, a double-click puts it back at the end.
        if binH > 0, p.y < binH + 4 {
            if let i = binChips().firstIndex(where: { $0.contains(p) }) {
                if ev.clickCount == 2 { e.placeFromBin(i, at: e.edit.clips.count); return }
                e.selected = e.edit.bin[i].id
                drag = .binMove(i, down: p, at: p, target: nil, moved: false, toOverlay: false)
            } else { e.selected = nil }
            needsDisplay = true
            return
        }
        // The playhead knob sits over the clip lane: it scrubs, whatever is under it.
        let onKnob = abs(p.x - x(e.now)) <= 7 && p.y >= laneY - 6 && p.y < laneY + 8
        if laneH > 0, p.y >= laneY, p.y < laneY + laneH, !onKnob {
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
                drag = .clipMove(k, down: p, target: nil, toBin: false)
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
        if p.y >= ovY - 2 {   // the lane of videos on top: move a bar, or drag its ends to trim it
            for o in e.edit.overlays.reversed() {
                let x0 = x(o.start), x1 = x(o.end)
                guard p.x >= x0 - 4, p.x <= max(x0 + 6, x1) + 4 else { continue }
                e.selected = o.id
                e.pushUndo()
                drag = .overlayItem(o.id, edge: abs(p.x - x0) < 6 ? -1 : abs(p.x - x1) < 6 ? 1 : 0, grab: t(p.x), o)
                if ev.clickCount == 2 { e.seek(o.start) }
                return
            }
            e.selected = nil
            e.seek(t(p.x))
            return
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

    // A range drag: from `anchor` (a grip being dragged keeps the other end), or a new range from the press.
    private func beginMark(_ e: VideoEditor, _ p: CGPoint, anchor: Double?) {
        e.player.pause()
        drag = .range(anchor: anchor ?? e.snapEdge(min(max(t(p.x), e.edit.trimStart), e.edit.trimEnd)), before: e.marked)
        if anchor == nil { e.marked = nil }
        needsDisplay = true
    }

    override func mouseDragged(with ev: NSEvent) {
        guard let e = editor, let d = drag else { return }
        let now = t(convert(ev.locationInWindow, from: nil).x)
        switch d {
        case .range(let anchor, _):
            let edge = e.snapEdge(min(max(now, e.edit.trimStart), e.edit.trimEnd))
            e.setMarked(anchor, edge)
            if let m = e.marked { e.seek(edge < anchor ? m.lowerBound : m.upperBound - 1 / e.frameRate) }   // shows the frame at the edge that moves
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
        case .clipMove(let k, let down, let target, let toBin):
            let p = convert(ev.locationInWindow, from: nil)
            guard e.edit.clips.indices.contains(k), target != nil || toBin || abs(p.x - down.x) >= 5 || abs(p.y - down.y) >= 5 else { break }
            let up = binH > 0 && p.y < binH + 4   // up over the bin
            drag = .clipMove(k, down: down, target: up ? nil : insertIndex(p.x, skip: k), toBin: up)
        case .binMove(let i, let down, _, _, let moved, _):
            let p = convert(ev.locationInWindow, from: nil)
            guard e.edit.bin.indices.contains(i), moved || abs(p.x - down.x) >= 5 || abs(p.y - down.y) >= 5 else { break }
            let overClips = p.y >= binH + 4 && p.y <= stripY + stripH   // the clip lane or the thumbnails under it
            let onTop = p.y >= ovY - 6 && p.y <= ovY + ovH + 10   // the lane of videos on top
            drag = .binMove(i, down: down, at: p, target: overClips ? insertIndex(p.x, skip: nil) : nil, moved: true, toOverlay: onTop)
        case .overlayItem(let id, let edge, let grab, let o0):
            let len = o0.clip.duration, most = max(o0.clip.length, o0.clip.outPoint) - o0.clip.inPoint   // footage after its in point
            let shortest = min(0.2, len)   // a piece already shorter than that keeps its length
            guard let i = e.edit.overlays.firstIndex(where: { $0.id == id }) else { return cancelDrag() }
            var o = e.edit.overlays[i]
            switch edge {
            case -1:   // its start and in point move together
                let d = min(max(now - o0.start, max(-o0.clip.inPoint, -o0.start)), len - shortest)
                o.start = o0.start + d
                o.clip.inPoint = o0.clip.inPoint + d
                e.seek(o.start)
            case 1:
                let end = min(max(now, o0.start + shortest), o0.start + most)
                o.clip.outPoint = o0.clip.inPoint + (end - o0.start)
                e.seek(end - 0.01)
            default:
                o.start = min(max(0, o0.start + now - grab), max(0, e.duration - 0.1))
                e.seek(o.start)
            }
            e.edit.overlays[i] = o
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
        case .clipMove(let k, _, _, true): e.moveToBin(k)
        case .clipMove(let k, _, let target?, false): e.moveClip(k, to: target)
        case .binMove(let i, _, let at, _, true, true): e.binToOverlay(i, at: e.snapEdge(t(at.x)))
        case .binMove(let i, _, _, let target?, true, false): e.placeFromBin(i, at: target)
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
                if i < thumbTimes.count, let k = VideoSequence.locate(e.edit.clips, thumbTimes[i])?.index, e.edit.clips[k].flipped {
                    let c = e.edit.clips[k]
                    ctx.translateBy(x: c.flipH ? cell.midX * 2 : 0, y: c.flipV ? cell.midY * 2 : 0)
                    ctx.scaleBy(x: c.flipH ? -1 : 1, y: c.flipV ? -1 : 1)
                }
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
        if binH > 0 { drawBin(e) }

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

        var taking = false   // a bin piece carried over the lane
        if case .binMove(_, _, _, _, true, true)? = drag { taking = true }
        let ol = CGRect(x: 0, y: ovY, width: bounds.width, height: ovH)
        lane(ol, "Videos on top: Add ▾ › Video on top or side by side, or drag a piece here from the bin", e.edit.overlays.isEmpty && !taking)
        if taking, case .binMove(_, _, let at, _, _, _)? = drag {
            Theme.accent.setStroke()
            let path = NSBezierPath(roundedRect: ol.insetBy(dx: 0.5, dy: 0.5), xRadius: 5, yRadius: 5)
            path.lineWidth = 1.5
            path.stroke()
            Theme.accent.setFill()
            CGRect(x: x(e.snapEdge(t(at.x))) - 1.5, y: ol.minY, width: 3, height: ol.height).fill()
        }
        for o in e.edit.overlays {
            bar(barRect(o.start, o.end, y: ovY + 2, h: ovH - 4), (o.isSplit ? "Split · " : "On top · ") + o.clip.displayName + o.clip.badges,
                o.isSplit ? "rectangle.split.2x1" : "pip", selected: o.id == e.selected)
        }

        if let m = e.marked { drawMarked(e, m) }

        let px = x(e.now)
        NSColor.white.setFill()
        CGRect(x: px - 1, y: laneY - 2, width: 2, height: bounds.height - laneY + 2).fill()
        NSBezierPath(ovalIn: CGRect(x: px - 5, y: laneY - 4, width: 10, height: 10)).fill()
        if case .binMove(let i, _, let at, _, true, _)? = drag, e.edit.bin.indices.contains(i) {   // the piece beside the mouse
            let w = min(binChips()[i].width, 180)
            chip(CGRect(x: at.x + 10, y: at.y - (binH - 6) / 2, width: w, height: binH - 6), e.edit.bin[i], selected: true, alpha: 0.8)
        }
    }

    // A clip's bar: its name and length (and, at a glance, how it's changed), clipped to the bar.
    func chip(_ r: CGRect, _ c: Clip, selected sel: Bool, alpha: CGFloat = 1) {
        guard let e = editor, let ctx = NSGraphicsContext.current?.cgContext else { return }
        (sel ? Theme.accent.withAlphaComponent(alpha) : NSColor.white.withAlphaComponent(0.18 * alpha)).setFill()
        NSBezierPath(roundedRect: r, xRadius: 4, yRadius: 4).fill()
        ctx.saveGState()
        ctx.clip(to: r.insetBy(dx: 3, dy: 0))
        let label = NSAttributedString(string: "\(c.displayName)  \(e.clock(c.duration))\(c.badges)",
                                       attributes: [.font: Theme.font(10, .medium), .foregroundColor: (sel ? Theme.onAccent : Theme.text).withAlphaComponent(max(0.5, alpha))])
        label.draw(at: CGPoint(x: r.minX + 7, y: r.midY - label.size().height / 2))
        ctx.restoreGState()
    }

    // The bin lane: a chip per piece taken off the timeline. It lights up while a clip is dragged up to it.
    private func drawBin(_ e: VideoEditor) {
        let r = CGRect(x: 0, y: 0, width: bounds.width, height: binH)
        var taking = false
        if case .clipMove(_, _, _, true)? = drag { taking = true }
        (taking ? Theme.accent.withAlphaComponent(0.2) : NSColor.white.withAlphaComponent(0.04)).setFill()
        let lane = NSBezierPath(roundedRect: r, xRadius: 5, yRadius: 5)
        lane.fill()
        if taking { Theme.accent.setStroke(); lane.lineWidth = 1.5; lane.stroke() }
        let title = NSAttributedString(string: "Bin", attributes: [.font: Theme.font(10, .semibold), .foregroundColor: Theme.textDim])
        title.draw(at: CGPoint(x: 9, y: r.midY - title.size().height / 2))
        if e.edit.bin.isEmpty {
            let s = NSAttributedString(string: "Clips taken out go here: ⌫, or drag a clip up · drag them back down to use them again",
                                       attributes: [.font: Theme.font(10), .foregroundColor: Theme.muted])
            s.draw(at: CGPoint(x: Timeline.binLabelW + 4, y: r.midY - s.size().height / 2))
            return
        }
        var carried: Int?
        if case .binMove(let i, _, _, _, true, _)? = drag { carried = i }
        for (i, cr) in binChips().enumerated() { chip(cr, e.edit.bin[i], selected: e.selected == e.edit.bin[i].id, alpha: carried == i ? 0.35 : 1) }
    }

    // The marked range: a band down the timeline from the thumbnails, grips on its edges, and a label with its length and
    // the key that copies it.
    private func drawMarked(_ e: VideoEditor, _ m: Range<Double>) {
        let x0 = x(m.lowerBound), x1 = x(m.upperBound), h = bounds.height - stripY
        Theme.accent.withAlphaComponent(0.22).setFill()
        CGRect(x: x0, y: stripY, width: x1 - x0, height: h).fill()
        Theme.accent.setFill()
        for hx in [x0, x1] { CGRect(x: hx - 1, y: stripY, width: 2, height: h).fill() }
        for hx in [x0, x1] {
            let grip = NSBezierPath(roundedRect: CGRect(x: hx - 4, y: stripY + 12, width: 8, height: stripH - 24), xRadius: 3, yRadius: 3)
            NSColor.white.setFill()
            grip.fill()
            Theme.onAccent.setStroke()
            grip.lineWidth = 1
            grip.stroke()
        }
        // The label sits on the band, or beside it when the band is too narrow, and stays on view.
        let label = NSAttributedString(string: e.markedLabel, attributes: [.font: Theme.font(10, .semibold), .foregroundColor: Theme.onAccent])
        let w = label.size().width + 16, lh: CGFloat = 18
        var lx = (x0 + x1) / 2 - w / 2
        if x1 - x0 < w + 16 { lx = x1 + 8 + w <= bounds.width ? x1 + 8 : x0 - 8 - w }
        lx = min(max(0, lx), max(0, bounds.width - w))
        let pill = CGRect(x: lx, y: stripY + (stripH - lh) / 2, width: w, height: lh)
        Theme.accent.setFill()
        NSBezierPath(roundedRect: pill, xRadius: 5, yRadius: 5).fill()
        label.draw(at: CGPoint(x: pill.minX + 8, y: pill.midY - label.size().height / 2))
    }

    // One bar per clip (name and length), cut lines across the thumbnails, and what a drag would do.
    private func drawClips(_ e: VideoEditor, _ strip: CGRect) {
        let starts = VideoSequence.starts(e.edit.clips), h = laneH - 4, y0 = laneY
        for (i, c) in e.edit.clips.enumerated() {
            let t0 = starts[i], t1 = t0 + c.duration
            var r = CGRect(x: x(t0) + 1, y: y0, width: x(t1) - x(t0) - 2, height: h)
            switch drag {  // the proposed cut
            case .clipIn(i, _, let v)?: r = CGRect(x: x(t0 + v - c.inPoint), y: y0, width: r.maxX - x(t0 + v - c.inPoint), height: h)
            case .clipOut(i, _, let v)?: r.size.width = x(t0 + v - c.inPoint) - r.minX
            default: break
            }
            let sel = e.selected == c.id
            var lifted = false
            if case .clipMove(i, _, let target, let toBin)? = drag, target != nil || toBin { lifted = true }
            chip(r, c, selected: sel, alpha: lifted ? 0.45 : 1)
            if sel {  // trim grips
                Theme.onAccent.setFill()
                for hx in [r.minX, r.maxX] { NSBezierPath(roundedRect: CGRect(x: hx - 2, y: y0 + 4, width: 4, height: h - 8), xRadius: 2, yRadius: 2).fill() }
            }
            if i > 0 {
                NSColor.black.withAlphaComponent(0.9).setFill()
                CGRect(x: x(t0) - 1, y: strip.minY, width: 2, height: strip.height).fill()
            }
        }
        var order: [Clip]?, target = 0
        if case .clipMove(let k, _, let t?, false)? = drag, e.edit.clips.indices.contains(k) {  // where the clip would go
            order = e.edit.clips
            order?.remove(at: k)
            target = t
        }
        if case .binMove(_, _, _, let t?, true, false)? = drag { order = e.edit.clips; target = t }   // where the bin piece would go
        if let order {
            let mx = x(target < order.count ? VideoSequence.starts(order)[target] : VideoSequence.total(order))
            Theme.accent.setFill()
            CGRect(x: mx - 1.5, y: y0 - 2, width: 3, height: strip.maxY - y0 + 2).fill()
        }
    }
}

// MARK: - Flip

extension VideoEditor {
    // What Flip acts on: the selected clip, else the clip at the playhead.
    var flipTarget: Int? { selectedClipIndex ?? VideoSequence.locate(edit.clips, now)?.index }
    var flipClip: Clip? { selectedOverlayIndex.map { edit.overlays[$0].clip } ?? flipTarget.map { edit.clips[$0] } }

    fileprivate func buildFlipMenu() {
        flipMenu.bezelStyle = .recessed
        let menu = flipMenu.menu!
        menu.autoenablesItems = false
        menu.delegate = self
        menu.addItem(withTitle: "Flip", action: nil, keyEquivalent: "")
        flipMenu.item(at: 0)?.image = NSImage(systemSymbolName: "arrow.left.and.right.righttriangle.left.righttriangle.right", accessibilityDescription: "Flip")
        for (title, h) in [("Flip horizontally (mirror left–right)", true), ("Flip vertically (upside down)", false)] {
            let it = NSMenuItem(title: title, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction { [weak self] in self?.toggleFlip(horizontal: h) }
            actionsKeep(a)
            it.target = a
            menu.addItem(it)
        }
        syncFlipMenu()
    }

    func syncFlipMenu() {
        guard flipMenu.numberOfItems >= 3 else { return }
        let c = flipClip
        flipMenu.item(at: 0)?.title = c.map { $0.flipH && $0.flipV ? "Flipped ↔ ↕" : $0.flipH ? "Flipped ↔" : $0.flipV ? "Flipped ↕" : "Flip" } ?? "Flip"
        flipMenu.item(at: 1)?.state = c?.flipH == true ? .on : .off
        flipMenu.item(at: 2)?.state = c?.flipV == true ? .on : .off
        flipMenu.item(at: 1)?.isEnabled = c != nil
        flipMenu.item(at: 2)?.isEnabled = c != nil
        flipMenu.toolTip = selectedOverlayIndex != nil ? "Mirror the selected video on top" : selectedClipIndex != nil ? "Mirror the selected clip (left–right or upside down)"
            : edit.clips.count > 1 ? "Mirror the clip at the playhead (select a clip to pick another)" : "Mirror the video (left–right or upside down)"
    }

    // One undo step: one clip's picture mirrored one way. The markup that sits wholly on it is mirrored with it, so it
    // stays on the same footage; the crop (all clips share it) and captions stay put.
    func toggleFlip(horizontal: Bool) {
        if let k = selectedOverlayIndex {   // a video on top: just its picture
            pushUndo()
            if horizontal { edit.overlays[k].clip.flipH.toggle() } else { edit.overlays[k].clip.flipV.toggle() }
            return syncFlipMenu()
        }
        guard let i = flipTarget else { return }
        pushUndo()
        let t0 = VideoSequence.starts(edit.clips)[i], t1 = t0 + edit.clips[i].duration
        let W = videoSize.width, H = videoSize.height
        var e = edit
        func mirror(_ p: inout CGPoint) { if horizontal { p.x = W - p.x } else { p.y = H - p.y } }
        for k in e.marks.indices where e.marks[k].start >= t0 - 1e-6 && e.marks[k].end <= t1 + 1e-6 {
            mirror(&e.marks[k].a)
            mirror(&e.marks[k].b)
        }
        if horizontal { e.clips[i].flipH.toggle() } else { e.clips[i].flipV.toggle() }
        edit = e
        syncFlipMenu()
    }
}

// MARK: - Guides and monochrome view

extension VideoEditor {
    // The Guides menu (also on a right click on the video): each guide, Show all, G to hide, Monochrome view.
    func fillGuidesMenu(_ menu: NSMenu, title: Bool) {
        menu.removeAllItems()
        menu.autoenablesItems = false
        if title {
            menu.addItem(withTitle: "Guides", action: nil, keyEquivalent: "")
            menu.item(at: 0)?.image = NSImage(systemSymbolName: "grid", accessibilityDescription: "Guides")
        }
        func item(_ t: String, on: Bool, _ run: @escaping () -> Void) {
            let it = NSMenuItem(title: t, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction(run)
            actionsKeep(a)
            it.target = a
            it.state = on ? .on : .off
            menu.addItem(it)
        }
        for g in GuideKind.allCases {
            item(g.label, on: shownGuides.contains(g)) { [weak self] in
                guard let self else { return }
                if self.guidesHidden { self.guides = [g]; self.guidesHidden = false }   // hidden: show just this one
                else if self.guides.contains(g) { self.guides.remove(g) } else { self.guides.insert(g) }
            }
        }
        item("Show all", on: false) { [weak self] in self?.guides = Set(GuideKind.allCases); self?.guidesHidden = false }
        item(guidesHidden ? "Show guides    G" : "Hide guides    G", on: false) { [weak self] in self?.toggleGuides() }
        menu.items.last?.isEnabled = !guides.isEmpty
        menu.addItem(.separator())
        item("Monochrome view", on: monochrome) { [weak self] in self?.monochrome.toggle() }
    }

    // G: hides the guides, and brings the same ones back (rule of thirds when none were picked yet).
    func toggleGuides() {
        if guides.isEmpty { guides = [.thirds]; guidesHidden = false } else { guidesHidden.toggle() }
    }

    func guidesChanged() {
        if let m = guidesMenu.menu { fillGuidesMenu(m, title: true) }
        let on = !shownGuides.isEmpty
        guidesMenu.item(at: 0)?.title = monochrome ? (on ? "Guides · B&W" : "B&W view") : "Guides"
        stage.needsDisplay = true
    }
}

// MARK: - Speed per clip

extension VideoEditor {
    // What Speed sets: the selected clip, else the clip at the playhead (so after a split, just that piece).
    var speedTarget: Int? { selectedClipIndex ?? VideoSequence.locate(edit.clips, now)?.index }

    func syncSpeedMenu() {
        guard let menu = speedMenu.menu else { return }
        menu.removeAllItems()
        let k = speedTarget
        let cur = k.map { edit.clips[$0].speed } ?? 1
        menu.addItem(withTitle: "Speed " + VideoEdit.speedLabel(cur), action: nil, keyEquivalent: "")
        speedMenu.item(at: 0)?.image = NSImage(systemSymbolName: "gauge.with.dots.needle.67percent", accessibilityDescription: "Speed")
        func item(_ menu: NSMenu, _ t: String, on: Bool, _ run: @escaping () -> Void) {
            let it = NSMenuItem(title: t, action: #selector(MenuAction.fire), keyEquivalent: "")
            let a = MenuAction(run)
            actionsKeep(a)
            it.target = a
            it.state = on ? .on : .off
            menu.addItem(it)
        }
        for v in VideoEdit.speeds { item(menu, VideoEdit.speedLabel(v), on: k != nil && cur == v) { [weak self] in self?.setSpeed(v, every: false) } }
        if edit.clips.count > 1 {
            menu.addItem(.separator())
            let sub = NSMenu()
            for v in VideoEdit.speeds { item(sub, VideoEdit.speedLabel(v), on: edit.clips.allSatisfy { $0.speed == v }) { [weak self] in self?.setSpeed(v, every: true) } }
            let all = NSMenuItem(title: "Every clip at", action: nil, keyEquivalent: "")
            all.submenu = sub
            menu.addItem(all)
        }
        speedMenu.toolTip = edit.clips.count < 2 ? "Playback speed of the saved video (sound keeps its pitch)"
            : selectedClipIndex != nil ? "Playback speed of the selected clip in the saved video"
            : "Playback speed of the clip at the playhead in the saved video (select a clip to pick another)"
    }

    // One undo step: how fast the clip (or every clip) plays. The timeline stays as it is.
    func setSpeed(_ v: Double, every: Bool) {
        guard let k = speedTarget else { return }
        var clips = edit.clips
        for i in clips.indices where every || i == k { clips[i].speed = v }
        guard clips != edit.clips else { return }
        pushUndo()
        pendingSeek = now
        edit.clips = clips
        syncSpeedMenu()
    }
}

// MARK: - Videos on top (picture in picture, split screen)

extension VideoEditor {
    // Puts `clip` on top from timeline time `at` (picture in picture, or one half of a split screen), selected.
    func addOverlay(_ clip: Clip, at: Double, layout: OverlayLayout) {
        player.pause()
        playStateChanged()
        pushUndo()
        cropping = false   // so the next drag on the video moves the box
        let o = VideoOverlay(clip: clip, start: min(max(0, at), max(0, duration - 0.1)), layout: layout)
        edit.overlays.append(o)
        selected = o.id
    }

    func addOverlay(_ url: URL, at: Double, layout: OverlayLayout) {
        Task { @MainActor in
            guard MediaFiles.isVideo(url), let c = try? await VideoSource.probe(url) else {
                return Toast.shared.show("Can't add that as a video", url.lastPathComponent, ms: 4000)
            }
            self.addOverlay(c, at: at, layout: layout)
        }
    }

    func addOverlayDialog(_ layout: OverlayLayout) {
        let p = NSOpenPanel()
        p.allowedContentTypes = MediaFiles.contentTypes(pictures: false, videos: true)
        p.message = layout == .pip ? "Video to show in a box on top" : "Video to show side by side"
        let at = insertTime
        p.beginSheetModal(for: window) { [weak self] r in if r == .OK, let u = p.url { self?.addOverlay(u, at: at, layout: layout) } }
    }

    // Bin piece `i` goes on top from `at` (one undo step with taking it from the bin).
    func binToOverlay(_ i: Int, at: Double) {
        guard edit.bin.indices.contains(i) else { return }
        player.pause()
        playStateChanged()
        pushUndo()
        cropping = false
        var e = edit
        let o = VideoOverlay(clip: e.bin.remove(at: i), start: min(max(0, at), max(0, duration - 0.1)))
        e.overlays.append(o)
        edit = e
        selected = o.id
    }

    // One undo step changing the selected overlay.
    func updateOverlay(_ f: (inout VideoOverlay) -> Void) {
        guard let i = selectedOverlayIndex else { return }
        pushUndo()
        f(&edit.overlays[i])
    }

    // Where an overlay shows on the video (video coordinates), in the preview's frame (the crop).
    func overlayOnVideo(_ o: VideoOverlay) -> CGRect {
        let v = viewRect
        return o.rect(v.size).offsetBy(dx: v.minX, dy: v.minY)
    }

    // The scroll wheel over a box: `factor` times as wide, about its middle (a run of wheel turns is one undo step).
    func scaleOverlay(_ i: Int, by factor: Double) {
        guard edit.overlays.indices.contains(i) else { return }
        let t = Date()
        if selected != edit.overlays[i].id || t.timeIntervalSince(lastWheelScale) > 0.8 { pushUndo() }
        lastWheelScale = t
        selected = edit.overlays[i].id
        let v = viewRect.size
        var o = edit.overlays[i]
        let r0 = o.rect(v)
        o.width = min(max(o.width * factor, 0.08), 1)
        let r1 = o.rect(v)
        o.pos = CGPoint(x: (r0.midX - r1.width / 2) / v.width, y: (r0.midY - r1.height / 2) / v.height)
        let r = o.rect(v)   // kept on the frame
        o.pos = CGPoint(x: r.minX / v.width, y: r.minY / v.height)
        edit.overlays[i] = o
    }
}

// MARK: - Reverse

extension VideoEditor {
    func syncReverse() {
        reverseButton.attributedTitle = NSAttributedString(string: reversing ? "Reversing…" : "Reverse", attributes: [.foregroundColor: Theme.text, .font: Theme.font(12)])
        reverseButton.isEnabled = !reversing
        reverseButton.toolTip = selectedClipIndex != nil ? "Play the selected clip backwards, end to start"
            : edit.clips.count > 1 ? "Play the whole timeline backwards: the end becomes the start (select a clip to reverse just that one)"
            : "Play the video backwards: the end becomes the start"
    }

    // Reverse plays footage end to start: the selected clip, else the whole timeline (its clips in the opposite order,
    // each one backwards, and everything on it mirrored in time). Reversed footage is written to a temp file first, with
    // a progress toast; reversing again (or ⌘Z) goes back to playing forward.
    func reverse() {
        guard !busy, !edit.clips.isEmpty else { return }
        window.makeFirstResponder(stage)   // commits a field being edited
        player.pause()
        playStateChanged()
        let only = selectedClipIndex
        let before = edit.clips
        setBusy(true, reversing: true)
        let toast = Toast.shared.post("Reversing…", only != nil || before.count == 1 ? "Playing the clip end to start" : "The whole timeline, end to start", ms: 600_000)
        Task { @MainActor in
            var clips = before
            do {
                let ks = only.map { [$0] } ?? Array(before.indices)
                for (n, k) in ks.enumerated() {
                    clips[k] = try await VideoReverse.toggled(before[k]) { p in
                        let pct = Int((Double(n) + p) * 100 / Double(ks.count))
                        DispatchQueue.main.async { Toast.shared.update(toast, body: "\(pct)%") }
                    }
                }
            } catch {
                self.setBusy(false, reversing: false)
                lastReverseError = error.localizedDescription
                return Toast.shared.show("Reverse failed", error.localizedDescription, ms: 8000)
            }
            self.setBusy(false, reversing: false)
            self.reverseDone(before: before, clips: clips, only: only)
        }
    }

    // One undo step: the reversed clips in place. On the clip (or the timeline) markup and captions are mirrored in time,
    // so each shows over the same footage; the trim and the videos on top follow on a whole timeline.
    func reverseDone(before: [Clip], clips: [Clip], only: Int?) {
        guard edit.clips == before else { return Toast.shared.show("The clips changed while reversing", "Reverse again to play them backwards.", ms: 4000) }
        pushUndo()
        let starts = VideoSequence.starts(before)
        var t0 = 0.0, t1 = VideoSequence.total(before)
        if let k = only { t0 = starts[k]; t1 = t0 + before[k].duration }
        var e = edit
        func mirrored(_ a: Double, _ b: Double) -> (Double, Double) { (t0 + t1 - b, t0 + t1 - a) }
        func inside(_ a: Double, _ b: Double) -> Bool { only == nil || (a >= t0 - 1e-6 && b <= t1 + 1e-6) }
        for i in e.marks.indices where inside(e.marks[i].start, e.marks[i].end) { (e.marks[i].start, e.marks[i].end) = mirrored(e.marks[i].start, e.marks[i].end) }
        for i in e.captions.indices where inside(e.captions[i].start, e.captions[i].end) {
            var c = e.captions[i]
            (c.start, c.end) = mirrored(c.start, c.end)
            for w in c.words.indices { (c.words[w].start, c.words[w].end) = mirrored(c.words[w].start, c.words[w].end) }
            c.words.reverse()
            e.captions[i] = c
        }
        e.captions.sort { $0.start < $1.start }
        e.clips = clips
        let at = now
        if only == nil {
            e.clips.reverse()
            for i in e.overlays.indices { e.overlays[i].start = max(0, t1 - e.overlays[i].end) }
            let a = e.trimStart
            e.trimStart = max(0, t1 - e.trimEnd)
            e.trimEnd = min(t1, t1 - a)
        }
        pendingSeek = only != nil ? t0 : min(max(t1 - at, e.trimStart), e.trimEnd)
        edit = e
        Toast.shared.show(clips[only ?? 0].reversed ? "Reversed" : "Back to playing forward", "⌘Z undoes it", ms: 2500)
    }
}
