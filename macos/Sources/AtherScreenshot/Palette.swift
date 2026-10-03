import AppKit
import Carbon.HIToolbox

struct PaletteItem {
    var id: String
    var title: String
    var keywords = ""
    var icon = "circle"
    var hint = ""          // shortcut or detail on the right
    var toggled: Bool? = nil
    var action: () -> Void
}

// ⌘K-style launcher. Also doubles as a one-line text prompt (rename).
final class Palette: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate, NSWindowDelegate {
    static let shared = Palette()

    private var panel: KeyablePanel!
    private var field: NSTextField!
    private var promptLabel: NSTextField!
    private var table: NSTableView!
    private var scroll: NSScrollView!
    private var heightC: NSLayoutConstraint!
    private var all: [PaletteItem] = []
    private var shown: [PaletteItem] = []
    private var mruKey = "PaletteMRU"
    private var onSubmit: ((String) -> Void)?
    private var previousApp: NSRunningApplication?
    var isVisible: Bool { panel?.isVisible ?? false }

    private let width: CGFloat = 640, rowH: CGFloat = 40, maxRows = 9

    private func build() {
        panel = roundedPanel(NSRect(x: 0, y: 0, width: width, height: 120), level: .modalPanel, key: true)
        panel.delegate = self
        excludeFromCapture(panel)
        panel.onKey = { [weak self] e in self?.key(e) ?? false }

        let root = NSView()
        root.wantsLayer = true
        root.layer?.backgroundColor = Theme.surface.cgColor
        root.layer?.cornerRadius = 14
        root.layer?.borderColor = Theme.border.cgColor
        root.layer?.borderWidth = 1
        root.layer?.masksToBounds = true

        promptLabel = NSTextField(labelWithString: "")
        promptLabel.font = Theme.font(11, .semibold)
        promptLabel.textColor = Theme.muted

        field = NSTextField()
        field.isBordered = false
        field.drawsBackground = false
        field.focusRingType = .none
        field.font = Theme.font(18)
        field.textColor = Theme.text
        field.delegate = self
        field.cell?.isScrollable = true

        table = NSTableView()
        table.headerView = nil
        table.backgroundColor = .clear
        table.rowHeight = rowH
        table.intercellSpacing = .zero
        table.selectionHighlightStyle = .regular
        table.addTableColumn(NSTableColumn(identifier: .init("c")))
        table.dataSource = self
        table.delegate = self
        table.target = self
        table.action = #selector(clicked)
        table.style = .plain

        scroll = NSScrollView()
        scroll.documentView = table
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay

        let sep = NSBox()
        sep.boxType = .custom
        sep.borderWidth = 0
        sep.fillColor = Theme.border

        for v in [promptLabel!, field!, sep, scroll!] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        heightC = scroll.heightAnchor.constraint(equalToConstant: 0)
        NSLayoutConstraint.activate([
            promptLabel.topAnchor.constraint(equalTo: root.topAnchor, constant: 12),
            promptLabel.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 20),
            promptLabel.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -20),
            field.topAnchor.constraint(equalTo: promptLabel.bottomAnchor, constant: 4),
            field.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 18),
            field.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -18),
            sep.topAnchor.constraint(equalTo: field.bottomAnchor, constant: 12),
            sep.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            sep.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            sep.heightAnchor.constraint(equalToConstant: 1),
            scroll.topAnchor.constraint(equalTo: sep.bottomAnchor, constant: 6),
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 6),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -6),
            scroll.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -6),
            heightC,
            root.widthAnchor.constraint(equalToConstant: width),
        ])
        panel.contentView = root
    }

    func show(_ items: [PaletteItem], placeholder: String = "Type a command…", mruKey: String = "PaletteMRU") {
        if panel == nil { build() }
        if isVisible && onSubmit == nil && self.mruKey == mruKey { return close() }  // the hotkey again closes it
        self.mruKey = mruKey
        onSubmit = nil
        all = items
        promptLabel.stringValue = ""
        promptLabel.isHidden = true
        field.placeholderAttributedString = NSAttributedString(string: placeholder, attributes: [.foregroundColor: Theme.muted, .font: Theme.font(18)])
        field.stringValue = ""
        scroll.isHidden = false
        filter()
        present()
    }

    func prompt(_ title: String, initial: String, done: @escaping (String) -> Void) {
        if panel == nil { build() }
        onSubmit = done
        all = []
        shown = []
        table.reloadData()
        promptLabel.stringValue = title.uppercased()
        promptLabel.isHidden = false
        field.placeholderAttributedString = nil
        field.stringValue = initial
        scroll.isHidden = true
        heightC.constant = 0
        present()
        field.currentEditor()?.selectAll(nil)
    }

    private func present() {
        let front = NSWorkspace.shared.frontmostApplication
        if front?.processIdentifier != ProcessInfo.processInfo.processIdentifier { previousApp = front }
        panel.layoutIfNeeded()
        let size = panel.contentView!.fittingSize
        let vis = Geo.mouseScreen.visibleFrame
        panel.setFrame(NSRect(x: vis.midX - size.width / 2, y: vis.maxY - vis.height * 0.22 - size.height, width: size.width, height: size.height), display: true)
        activateApp()
        panel.makeKeyAndOrderFront(nil)
        panel.makeFirstResponder(field)
    }

    func close() {
        guard isVisible else { return }
        panel.orderOut(nil)
        onSubmit = nil
    }

    // Hands focus back to whatever app was in front before the palette, so "capture active window" works.
    private func restoreFocus() {
        previousApp?.activate()
        previousApp = nil
    }

    func windowDidResignKey(_ notification: Notification) {
        if isVisible { close() }
    }

    // MARK: filtering

    private var mru: [String] {
        get { UserDefaults.standard.stringArray(forKey: mruKey) ?? [] }
        set { UserDefaults.standard.set(Array(newValue.prefix(12)), forKey: mruKey) }
    }

    private func score(_ item: PaletteItem, _ words: [String]) -> Int? {
        let title = item.title.lowercased(), hay = title + " " + item.keywords.lowercased() + " " + item.hint.lowercased()
        var s = 0
        for w in words {
            guard let r = hay.range(of: w) else { return nil }
            let titleHit = title.range(of: w)
            if title.hasPrefix(w) { s += 100 }
            else if let t = titleHit, t.lowerBound == title.startIndex || title[title.index(before: t.lowerBound)] == " " { s += 60 }
            else if titleHit != nil { s += 30 }
            else if r.lowerBound == hay.startIndex || hay[hay.index(before: r.lowerBound)] == " " { s += 15 }
            else { s += 5 }
        }
        if let i = mru.firstIndex(of: item.id) { s += 12 - i }
        return s
    }

    private func filter() {
        let q = field.stringValue.lowercased().split(separator: " ").map(String.init)
        if q.isEmpty {
            let recent = mru
            shown = all.sorted { a, b in
                let ia = recent.firstIndex(of: a.id) ?? Int.max, ib = recent.firstIndex(of: b.id) ?? Int.max
                return ia < ib
            }
        } else {
            shown = all.compactMap { i in score(i, q).map { (i, $0) } }.sorted { $0.1 > $1.1 }.map(\.0)
        }
        table.reloadData()
        heightC.constant = CGFloat(min(maxRows, max(1, shown.count))) * rowH
        if !shown.isEmpty { table.selectRowIndexes([0], byExtendingSelection: false); table.scrollRowToVisible(0) }
        if isVisible {
            panel.layoutIfNeeded()
            var f = panel.frame
            let h = panel.contentView!.fittingSize.height
            f.origin.y += f.height - h
            f.size.height = h
            panel.setFrame(f, display: true)
        }
    }

    func controlTextDidChange(_ obj: Notification) { if onSubmit == nil { filter() } }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy sel: Selector) -> Bool {
        switch sel {
        case #selector(NSResponder.moveDown(_:)): move(1)
        case #selector(NSResponder.moveUp(_:)): move(-1)
        case #selector(NSResponder.insertNewline(_:)): run()
        case #selector(NSResponder.cancelOperation(_:)): close(); restoreFocus()
        default: return false
        }
        return true
    }

    private func key(_ e: NSEvent) -> Bool {
        if e.keyCode == UInt16(kVK_ANSI_K) && e.modifierFlags.contains(.command) { close(); restoreFocus(); return true }
        return false
    }

    private func move(_ d: Int) {
        guard !shown.isEmpty else { return }
        let r = (table.selectedRow + d + shown.count) % shown.count
        table.selectRowIndexes([r], byExtendingSelection: false)
        table.scrollRowToVisible(r)
    }

    @objc private func clicked() { if table.clickedRow >= 0 { run() } }

    private func run() {
        if let submit = onSubmit {
            let text = field.stringValue
            close()
            restoreFocus()
            submit(text)
            return
        }
        let row = table.selectedRow
        guard row >= 0, row < shown.count else { return }
        let item = shown[row]
        var m = mru
        m.removeAll { $0 == item.id }
        m.insert(item.id, at: 0)
        mru = m
        close()
        restoreFocus()
        DispatchQueue.main.async { item.action() }
    }

    // MARK: table

    func numberOfRows(in tableView: NSTableView) -> Int { shown.count }

    func tableView(_ tableView: NSTableView, rowViewForRow row: Int) -> NSTableRowView? { PaletteRowView() }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let item = shown[row]
        let v = NSView()
        let icon = NSImageView(image: NSImage(systemSymbolName: item.icon, accessibilityDescription: nil) ?? NSImage())
        icon.contentTintColor = Theme.textDim
        icon.symbolConfiguration = .init(pointSize: 14, weight: .regular)
        let title = NSTextField(labelWithString: item.title)
        title.font = Theme.font(14)
        title.textColor = Theme.text
        title.lineBreakMode = .byTruncatingTail
        var right: NSView?
        if let on = item.toggled {
            let l = NSTextField(labelWithString: on ? "ON" : "OFF")
            l.font = Theme.font(10, .bold)
            l.textColor = on ? Theme.onAccent : Theme.muted
            l.wantsLayer = true
            l.drawsBackground = true
            l.backgroundColor = on ? Theme.accent : Theme.raised
            l.alignment = .center
            l.layer?.cornerRadius = 4
            l.widthAnchor.constraint(equalToConstant: 38).isActive = true
            right = l
        } else if !item.hint.isEmpty {
            let l = NSTextField(labelWithString: item.hint)
            l.font = Theme.font(12, .medium)
            l.textColor = Theme.muted
            l.lineBreakMode = .byTruncatingHead
            right = l
        }
        for x in [icon, title] + (right.map { [$0] } ?? []) {
            x.translatesAutoresizingMaskIntoConstraints = false
            v.addSubview(x)
        }
        NSLayoutConstraint.activate([
            icon.leadingAnchor.constraint(equalTo: v.leadingAnchor, constant: 14),
            icon.centerYAnchor.constraint(equalTo: v.centerYAnchor),
            icon.widthAnchor.constraint(equalToConstant: 20),
            title.leadingAnchor.constraint(equalTo: icon.trailingAnchor, constant: 12),
            title.centerYAnchor.constraint(equalTo: v.centerYAnchor),
        ])
        if let right {
            right.setContentCompressionResistancePriority(.required, for: .horizontal)
            NSLayoutConstraint.activate([
                right.trailingAnchor.constraint(equalTo: v.trailingAnchor, constant: -14),
                right.centerYAnchor.constraint(equalTo: v.centerYAnchor),
                title.trailingAnchor.constraint(lessThanOrEqualTo: right.leadingAnchor, constant: -12),
                right.widthAnchor.constraint(lessThanOrEqualToConstant: 260),
            ])
        } else {
            title.trailingAnchor.constraint(lessThanOrEqualTo: v.trailingAnchor, constant: -14).isActive = true
        }
        return v
    }
}

private final class PaletteRowView: NSTableRowView {
    override func drawSelection(in dirtyRect: NSRect) {
        let r = bounds.insetBy(dx: 2, dy: 2)
        Theme.selected.setFill()
        NSBezierPath(roundedRect: r, xRadius: 8, yRadius: 8).fill()
        Theme.accent.setFill()
        NSBezierPath(roundedRect: NSRect(x: r.minX + 3, y: r.midY - 9, width: 3, height: 18), xRadius: 1.5, yRadius: 1.5).fill()
    }
    override var isEmphasized: Bool { get { true } set {} }
}
