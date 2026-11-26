import AppKit

// The small floating bar while a meeting is recorded: a red dot and the time, a level meter for each source (so you can
// see both are heard), Mark, Pause, Stop and Discard. It stays out of screenshots and recordings, and can be dragged.
final class MeetingHUD {
    private let panel: KeyablePanel
    private weak var recorder: MeetingRecorder?
    private let dot = NSView()
    private let time = NSTextField(labelWithString: "Starting…")
    private let meterOthers = LevelMeter()
    private let meterYou = LevelMeter()
    private let markCount = NSTextField(labelWithString: "")
    private var pauseButton: NSButton!
    private var markButton: NSButton!
    private var timer: Timer?
    static let size = NSSize(width: 404, height: 46)

    init(recorder: MeetingRecorder) {
        self.recorder = recorder
        let vis = Geo.mouseScreen.visibleFrame
        let s = MeetingHUD.size
        let saved = UserDefaults.standard.string(forKey: "MeetingHUDOrigin").map(NSPointFromString)
        var origin = saved ?? NSPoint(x: vis.midX - s.width / 2, y: vis.maxY - s.height - 12)
        if !NSScreen.screens.contains(where: { $0.visibleFrame.contains(NSRect(origin: origin, size: s)) }) {
            origin = NSPoint(x: vis.midX - s.width / 2, y: vis.maxY - s.height - 12)
        }
        panel = roundedPanel(NSRect(origin: origin, size: s), level: .statusBar)
        excludeFromCapture(panel)
        panel.isMovableByWindowBackground = true
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        panel.contentView = makeView(others: recorder.options.others, you: recorder.options.you)
        panel.orderFrontRegardless()
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in self?.tick() }
        NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: panel, queue: .main) { [weak self] _ in
            guard let self else { return }
            UserDefaults.standard.set(NSStringFromPoint(self.panel.frame.origin), forKey: "MeetingHUDOrigin")
        }
    }

    private func makeView(others: Bool, you: Bool) -> NSView {
        let v = NSView(frame: NSRect(origin: .zero, size: MeetingHUD.size))
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.surface.cgColor
        v.layer?.cornerRadius = 12
        v.layer?.borderColor = Theme.border.cgColor
        v.layer?.borderWidth = 1
        dot.wantsLayer = true
        dot.layer?.backgroundColor = NSColor.systemRed.cgColor
        dot.layer?.cornerRadius = 5
        time.font = .monospacedDigitSystemFont(ofSize: 14, weight: .semibold)
        time.textColor = Theme.text
        func meter(_ m: LevelMeter, _ symbol: String, _ tip: String, on: Bool) -> NSView {
            let icon = NSImageView(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip)!)
            icon.contentTintColor = on ? Theme.textDim : Theme.muted.withAlphaComponent(0.4)
            icon.symbolConfiguration = .init(pointSize: 11, weight: .medium)
            m.enabled = on
            m.toolTip = tip
            let s = NSStackView(views: [icon, m])
            s.spacing = 4
            s.toolTip = tip
            m.widthAnchor.constraint(equalToConstant: 46).isActive = true
            m.heightAnchor.constraint(equalToConstant: 6).isActive = true
            return s
        }
        func btn(_ symbol: String, _ tip: String, _ sel: Selector) -> NSButton {
            let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip)!, target: self, action: sel)
            b.isBordered = false
            b.toolTip = tip
            b.contentTintColor = Theme.text
            b.symbolConfiguration = .init(pointSize: 14, weight: .medium)
            return b
        }
        markButton = btn("flag.fill", "Mark this moment (\(Hotkey.display(Settings.shared.hotkey(.markMeeting))))", #selector(markClicked))
        markButton.contentTintColor = Theme.accent
        markCount.font = Theme.font(10, .bold)
        markCount.textColor = Theme.accent
        pauseButton = btn("pause.fill", "Pause / resume", #selector(pauseClicked))
        let stop = btn("stop.fill", "Stop and save (\(Hotkey.display(Settings.shared.hotkey(.recordMeeting))))", #selector(stopClicked))
        stop.contentTintColor = NSColor.systemRed
        let discard = btn("trash", "Discard", #selector(discardClicked))
        discard.contentTintColor = Theme.muted
        let stack = NSStackView(views: [dot, time, meter(meterOthers, "speaker.wave.2.fill", "Others (system audio)", on: others),
                                        meter(meterYou, "mic.fill", "You (microphone)", on: you), NSView(), markButton, markCount, pauseButton, stop, discard])
        stack.spacing = 10
        stack.setCustomSpacing(2, after: markButton)
        stack.edgeInsets = NSEdgeInsets(top: 0, left: 14, bottom: 0, right: 14)
        stack.frame = v.bounds
        stack.autoresizingMask = [.width, .height]
        dot.widthAnchor.constraint(equalToConstant: 10).isActive = true
        dot.heightAnchor.constraint(equalToConstant: 10).isActive = true
        time.widthAnchor.constraint(greaterThanOrEqualToConstant: 54).isActive = true
        v.addSubview(stack)
        return v
    }

    func started() { update() }

    private var lastSecond = -1
    private func tick() {
        guard let r = recorder else { return }
        let l = r.levels
        meterOthers.level = r.isPaused ? 0 : l.others
        meterYou.level = r.isPaused ? 0 : l.you
        let s = Int(r.elapsed)
        if s != lastSecond || r.phase != .recording { lastSecond = s; update() }
    }

    func update() {
        guard let r = recorder else { return }
        switch r.phase {
        case .starting: time.stringValue = "Starting…"
        case .recording: time.stringValue = MeetingFormat.clock(r.elapsed)
        case .finishing: break
        }
        pauseButton.image = NSImage(systemSymbolName: r.isPaused ? "play.fill" : "pause.fill", accessibilityDescription: nil)
        dot.layer?.backgroundColor = r.isPaused ? Theme.muted.cgColor : NSColor.systemRed.cgColor
        dot.layer?.opacity = r.isPaused || r.phase != .recording ? 1 : (Int(Date().timeIntervalSince1970 * 2) % 2 == 0 ? 1 : 0.35)
        let n = r.meeting.markers.count
        markCount.stringValue = n > 0 ? "\(n)" : ""
    }

    func flashMark() {
        update()
        markButton.layer?.removeAllAnimations()
        let a = CABasicAnimation(keyPath: "transform.scale")
        a.fromValue = 1.5
        a.toValue = 1
        a.duration = 0.3
        markButton.wantsLayer = true
        markButton.layer?.add(a, forKey: "pop")
        NSSound(named: "Tink")?.play()
    }

    func finishing(discard: Bool) {
        timer?.invalidate()
        time.stringValue = discard ? "Discarding…" : "Saving…"
        meterOthers.level = 0
        meterYou.level = 0
        [pauseButton, markButton].forEach { $0?.isEnabled = false }
    }

    func close() {
        timer?.invalidate()
        panel.orderOut(nil)
    }

    @objc private func markClicked() { recorder?.mark() }
    @objc private func pauseClicked() { recorder?.togglePause() }
    @objc private func stopClicked() { recorder?.stop() }
    @objc private func discardClicked() {
        guard let r = recorder else { return }
        let a = NSAlert()
        a.messageText = "Discard this meeting recording?"
        a.informativeText = "\(MeetingFormat.clock(r.elapsed)) of audio will be thrown away."
        a.addButton(withTitle: "Discard")
        a.addButton(withTitle: "Keep Recording")
        a.buttons.first?.hasDestructiveAction = true
        activateApp()
        if a.runModal() == .alertFirstButtonReturn { r.discard() }
    }
}

// A thin horizontal meter: lime when speech is heard, dimmed when the source is off.
final class LevelMeter: NSView {
    var level: Float = 0 { didSet { if abs(level - oldValue) > 0.01 { needsDisplay = true } } }
    var enabled = true { didSet { needsDisplay = true } }

    override func draw(_ dirtyRect: NSRect) {
        let r = bounds
        let bg = NSBezierPath(roundedRect: r, xRadius: r.height / 2, yRadius: r.height / 2)
        Theme.raised.setFill()
        bg.fill()
        guard enabled else { return }
        let w = max(r.height, r.width * CGFloat(level))
        guard level > 0.02 else { return }
        let fg = NSBezierPath(roundedRect: NSRect(x: r.minX, y: r.minY, width: w, height: r.height), xRadius: r.height / 2, yRadius: r.height / 2)
        (level > 0.92 ? NSColor.systemOrange : Theme.accent).setFill()
        fg.fill()
    }
}
