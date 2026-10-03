import AppKit

// One toast at a time in the corner of the display under the mouse. Clicking it runs its action.
final class Toast {
    static let shared = Toast()
    private var panel: KeyablePanel?
    private var view: ToastView?
    private var timer: Timer?
    private var currentID: UInt64 = 0
    private var action: (() -> Void)?

    func show(_ title: String, _ body: String = "", image: CGImage? = nil, ms: Int? = nil, onClick: (() -> Void)? = nil) {
        _ = post(title, body, image: image, ms: ms, onClick: onClick)
    }

    // Like show(), returning an id for update(_:body:).
    func post(_ title: String, _ body: String = "", image: CGImage? = nil, ms: Int? = nil, onClick: (() -> Void)? = nil) -> UInt64 {
        currentID += 1
        action = onClick
        let v = ToastView(title: title, body: body, image: image)
        v.onClick = { [weak self] in
            let a = self?.action
            self?.hide()
            a?()
        }
        let size = v.fittingSize
        let vis = Geo.mouseScreen.visibleFrame
        let frame = NSRect(x: vis.maxX - size.width - 16, y: vis.minY + 16, width: size.width, height: size.height)
        if panel == nil {
            let p = roundedPanel(frame, level: .statusBar)
            p.ignoresMouseEvents = false
            excludeFromCapture(p)
            panel = p
        }
        guard let panel else { return currentID }
        panel.contentView = v
        panel.setFrame(frame, display: true)
        view = v
        panel.alphaValue = 1
        panel.orderFrontRegardless()
        timer?.invalidate()
        let dur = Double(ms ?? Settings.shared.int("ToastMs")) / 1000
        timer = Timer.scheduledTimer(withTimeInterval: dur, repeats: false) { [weak self] _ in self?.fadeOut() }
        return currentID
    }

    func update(_ id: UInt64, body: String) {
        guard id == currentID, let view, let panel else { return }
        view.setBody(body)
        let size = view.fittingSize
        var f = panel.frame
        f.origin.x = f.maxX - size.width
        f.size = size
        panel.setFrame(f, display: true)
    }

    func hide() {
        timer?.invalidate()
        panel?.orderOut(nil)
    }

    private func fadeOut() {
        guard let panel else { return }
        if panel.frame.contains(NSEvent.mouseLocation) {  // keep it while hovered
            timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: false) { [weak self] _ in self?.fadeOut() }
            return
        }
        NSAnimationContext.runAnimationGroup({ ctx in
            ctx.duration = 0.2
            panel.animator().alphaValue = 0
        }, completionHandler: { [weak self] in
            if panel.alphaValue == 0 { self?.hide() }
        })
    }
}

private final class ToastView: NSView {
    var onClick: (() -> Void)?
    private let bodyLabel: NSTextField

    init(title: String, body: String, image: CGImage?) {
        bodyLabel = NSTextField(wrappingLabelWithString: body)
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = Theme.surface.cgColor
        layer?.cornerRadius = 12
        layer?.borderColor = Theme.border.cgColor
        layer?.borderWidth = 1

        let titleLabel = NSTextField(labelWithString: title)
        titleLabel.font = Theme.font(13, .semibold)
        titleLabel.textColor = Theme.text
        bodyLabel.font = Theme.font(12)
        bodyLabel.textColor = Theme.muted
        bodyLabel.maximumNumberOfLines = 6
        bodyLabel.preferredMaxLayoutWidth = 260
        bodyLabel.isHidden = body.isEmpty

        let bar = NSView()
        bar.wantsLayer = true
        bar.layer?.backgroundColor = Theme.accent.cgColor
        bar.layer?.cornerRadius = 1.5

        let text = NSStackView(views: [titleLabel, bodyLabel])
        text.orientation = .vertical
        text.alignment = .leading
        text.spacing = 3
        var row: [NSView] = [bar]
        if let image {
            let iv = NSImageView(image: image.nsImage())
            iv.imageScaling = .scaleProportionallyUpOrDown
            iv.wantsLayer = true
            iv.layer?.cornerRadius = 6
            iv.layer?.masksToBounds = true
            let aspect = CGFloat(image.width) / CGFloat(max(1, image.height))
            let h: CGFloat = 54
            iv.widthAnchor.constraint(equalToConstant: min(110, max(40, h * aspect))).isActive = true
            iv.heightAnchor.constraint(equalToConstant: h).isActive = true
            row.append(iv)
        }
        row.append(text)
        let stack = NSStackView(views: row)
        stack.orientation = .horizontal
        stack.alignment = .centerY
        stack.spacing = 12
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 16)
        stack.translatesAutoresizingMaskIntoConstraints = false
        bar.widthAnchor.constraint(equalToConstant: 3).isActive = true
        bar.heightAnchor.constraint(equalToConstant: 30).isActive = true
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor), stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor), stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            widthAnchor.constraint(greaterThanOrEqualToConstant: 240), widthAnchor.constraint(lessThanOrEqualToConstant: 420),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    func setBody(_ s: String) {
        bodyLabel.stringValue = s
        bodyLabel.isHidden = s.isEmpty
    }

    override func mouseDown(with event: NSEvent) { onClick?() }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
