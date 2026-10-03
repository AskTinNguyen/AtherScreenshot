import AppKit
import Carbon.HIToolbox
import UniformTypeIdentifiers

final class Editor: NSObject, NSWindowDelegate {
    static var instances: [Editor] = []
    static var annotClipboard: [Annot] = []

    let base: CGImage
    let unit: CGFloat
    let window: NSWindow
    let canvas: EditorCanvas
    private let toolbar: EditorToolbar
    private let status = NSTextField(labelWithString: "")
    var styled = Settings.shared.bool("StyledExport")
    var dirty = false
    private var redacting = false

    @discardableResult
    static func open(_ img: CGImage, scale: CGFloat? = nil) -> Editor {
        let e = Editor(img, unit: scale ?? Geo.mouseScreen.backingScaleFactor)
        instances.append(e)
        return e
    }

    static func open(url: URL) {
        guard let img = CGImage.load(url) else {
            Toast.shared.show("Can't open that image", url.lastPathComponent)
            return
        }
        // Treat files as Retina-scale when they came from a Retina display (most captures do).
        open(img, scale: Geo.mouseScreen.backingScaleFactor)
    }

    private init(_ img: CGImage, unit: CGFloat) {
        base = img
        self.unit = max(1, unit)
        canvas = EditorCanvas(base: img, unit: self.unit)
        toolbar = EditorToolbar()
        let vis = Geo.mouseScreen.visibleFrame
        let want = NSSize(width: CGFloat(img.width) / self.unit + 48, height: CGFloat(img.height) / self.unit + 48 + 52 + 26)
        let size = NSSize(width: min(max(want.width, 1080), vis.width * 0.92), height: min(max(want.height, 520), vis.height * 0.92))
        window = NSWindow(contentRect: NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = "Annotate — \(img.width) × \(img.height)"
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 1080, height: 420)  // fits the whole toolbar
        window.delegate = self
        window.tabbingMode = .disallowed

        let root = NSView()
        toolbar.editor = self
        canvas.editor = self
        status.font = Theme.font(11)
        status.textColor = Theme.muted
        status.lineBreakMode = .byTruncatingTail
        let statusBar = NSView()
        statusBar.wantsLayer = true
        statusBar.layer?.backgroundColor = Theme.surface.cgColor
        for v in [toolbar, canvas, statusBar, status] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        NSLayoutConstraint.activate([
            toolbar.topAnchor.constraint(equalTo: root.topAnchor, constant: 28),
            toolbar.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            toolbar.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            toolbar.heightAnchor.constraint(equalToConstant: 48),
            canvas.topAnchor.constraint(equalTo: toolbar.bottomAnchor),
            canvas.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            canvas.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            canvas.bottomAnchor.constraint(equalTo: statusBar.topAnchor),
            statusBar.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            statusBar.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            statusBar.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            statusBar.heightAnchor.constraint(equalToConstant: 26),
            status.leadingAnchor.constraint(equalTo: statusBar.leadingAnchor, constant: 14),
            status.trailingAnchor.constraint(equalTo: statusBar.trailingAnchor, constant: -14),
            status.centerYAnchor.constraint(equalTo: statusBar.centerYAnchor),
        ])
        window.contentView = root
        toolbar.build()
        refreshChrome()
        AppDelegate.shared?.windowOpened(window)
        activateApp()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(canvas)
    }

    func refreshChrome() {
        toolbar.sync()
        let st = canvas.state
        let size = st.crop.map { "\(Int($0.width)) × \(Int($0.height))" } ?? "\(base.width) × \(base.height)"
        status.stringValue = "\(canvas.tool.info.name):  \(canvas.tool.info.hint)     ·     \(size)  ·  \(Int((canvas.zoom * 100 * unit).rounded()))%"
            + (styled ? "  ·  Styled export" : "") + "     ·     ⌘K all commands"
    }

    // MARK: actions

    func export() -> CGImage {
        canvas.commitText()
        let img = Render.compose(base, canvas.state, pixel: canvas.pixelCache)
        return styled ? Render.styled(img, unit: unit) : img
    }

    func copy() {
        let img = export()
        copyImage(img)
        AppDelegate.shared?.setLast(img, url: nil)
        Toast.shared.show("Copied to clipboard", "\(img.width) × \(img.height)")
    }

    func save(close: Bool = false) {
        let img = export()
        let url = Output.newCaptureURL(ext: "png", info: NameInfo(w: img.width, h: img.height))
        Output.savePNG(img, to: url) { ok in
            if ok { AppDelegate.shared?.setLast(img, url: url) }
            Toast.shared.show(ok ? (close ? "Copied and saved" : "Saved") : "Save failed", url.lastPathComponent, image: ok ? img : nil) {
                Output.reveal(url)
            }
        }
        dirty = false
    }

    func saveAs() {
        let img = export()
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.png]
        panel.nameFieldStringValue = Output.newCaptureURL(ext: "png").deletingPathExtension().lastPathComponent
        panel.beginSheetModal(for: window) { r in
            guard r == .OK, let url = panel.url else { return }
            Output.savePNG(img, to: url) { ok in Toast.shared.show(ok ? "Saved" : "Save failed", url.lastPathComponent) }
            self.dirty = false
        }
    }

    func pin() { Pin.show(export(), scale: unit) }

    func done() {
        let img = export()
        copyImage(img)
        if Settings.shared.bool("SaveToFile") { save(close: true) } else { Toast.shared.show("Copied to clipboard", "\(img.width) × \(img.height)") }
        AppDelegate.shared?.setLast(img, url: nil)
        dirty = false
        window.close()
    }

    func toggleStyled() {
        styled.toggle()
        Toast.shared.show(styled ? "Styled export: On" : "Styled export: Off", "Gradient backdrop, padding, rounded corners and a shadow")
        refreshChrome()
    }

    func autoRedact() {
        guard !redacting else { return }
        canvas.commitText()
        redacting = true
        Toast.shared.show("Looking for sensitive text…", "Emails, IPs, keys, tokens, card and phone numbers", ms: 8000)
        let img = base, unit = unit
        OCR.async({ OCR.findSensitive(try OCR.words(img)) }) { [weak self] r in
            guard let self else { return }
            self.redacting = false
            switch r {
            case .failure(let e): Toast.shared.show("Auto-redact failed", e.localizedDescription)
            case .success(let rects):
                if rects.isEmpty { Toast.shared.show("Nothing sensitive found", "It can only redact text that OCR can read."); return }
                self.canvas.pushUndo()
                for r in rects {
                    self.canvas.state.annots.append(Annot(tool: .pixelate, color: 0, level: 2, unit: unit, pts: [r.origin, CGPoint(x: r.maxX, y: r.maxY)]))
                }
                self.canvas.changed(pixels: true)
                self.canvas.setTool(.select)
                Toast.shared.show("Redacted \(rects.count) item\(rects.count == 1 ? "" : "s")", "Each box is editable: select it to move or delete. Check important redactions.")
            }
        }
    }

    func copyAnnotations() {
        Editor.annotClipboard = canvas.state.annots
        Toast.shared.show("Annotations copied", "\(canvas.state.annots.count) item(s) · ⌘⇧V in another editor pastes them")
    }

    func pasteAnnotations() {
        guard !Editor.annotClipboard.isEmpty else { return }
        canvas.pushUndo()
        canvas.state.annots += Editor.annotClipboard
        canvas.changed(pixels: true)
    }

    func commandPalette() {
        var items: [PaletteItem] = Tool.allCases.map { t in
            PaletteItem(id: "tool.\(t)", title: t.info.name, keywords: "tool " + t.info.hint, icon: t.info.symbol, hint: String(t.info.key).uppercased()) { [weak self] in
                self?.canvas.setTool(t)
            }
        }
        let acts: [(String, String, String, String, () -> Void)] = [
            ("Done: copy, save and close", "finish", "checkmark.circle", "↩", { [weak self] in self?.done() }),
            ("Copy", "clipboard", "doc.on.doc", "⌘C", { [weak self] in self?.copy() }),
            ("Save to captures", "png file", "square.and.arrow.down", "⌘S", { [weak self] in self?.save() }),
            ("Save as…", "png file export", "square.and.arrow.down.on.square", "⌘⇧S", { [weak self] in self?.saveAs() }),
            ("Pin to screen", "float sticky", "pin", "⌘P", { [weak self] in self?.pin() }),
            ("Undo", "", "arrow.uturn.backward", "⌘Z", { [weak self] in self?.canvas.undo() }),
            ("Redo", "", "arrow.uturn.forward", "⌘⇧Z", { [weak self] in self?.canvas.redo() }),
            ("Auto-redact sensitive text", "privacy ocr email key token", "eye.slash", "⌘R", { [weak self] in self?.autoRedact() }),
            ("Styled export (background, shadow, rounded corners)", "pretty share", "sparkles", styled ? "On  ⌘E" : "Off  ⌘E", { [weak self] in self?.toggleStyled() }),
            ("Copy annotations", "shapes", "square.on.square", "⌘⇧C", { [weak self] in self?.copyAnnotations() }),
            ("Paste annotations", "shapes", "square.on.square.dashed", "⌘⇧V", { [weak self] in self?.pasteAnnotations() }),
            ("Duplicate selected", "copy", "plus.square.on.square", "⌘D", { [weak self] in self?.canvas.duplicateSelected() }),
            ("Close editor", "quit", "xmark", "⎋", { [weak self] in self?.window.performClose(nil) }),
        ]
        for (i, a) in acts.enumerated() { items.append(PaletteItem(id: "act.\(i)", title: a.0, keywords: a.1, icon: a.2, hint: a.3, action: a.4)) }
        for (i, n) in kColorNames.enumerated() {
            items.append(PaletteItem(id: "color.\(i)", title: "Color: \(n)", keywords: "colour", icon: "paintpalette", hint: "\(i + 1)") { [weak self] in self?.canvas.setColor(i) })
        }
        Palette.shared.show(items, placeholder: "Editor commands…", mruKey: "EditorPaletteMRU")
    }

    // MARK: window

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        canvas.commitText()
        guard dirty else { return true }
        let a = NSAlert()
        a.messageText = "Close the editor and discard your changes?"
        a.addButton(withTitle: "Keep Editing")
        a.addButton(withTitle: "Discard")
        a.beginSheetModal(for: window) { r in
            if r == .alertSecondButtonReturn {
                self.dirty = false
                self.window.close()
            }
        }
        return false
    }

    func windowWillClose(_ notification: Notification) {
        Editor.instances.removeAll { $0 === self }
    }

    func windowDidResize(_ notification: Notification) { refreshChrome() }
}

// MARK: - Canvas

final class EditorCanvas: NSView {
    weak var editor: Editor?
    let base: CGImage
    let unit: CGFloat
    var state = DocState()
    var pixelCache: CGImage
    private var undoStack: [DocState] = []
    private var redoStack: [DocState] = []

    var tool: Tool = .arrow
    var color = 0
    var level = 1
    var selected: Int?
    private var live: Annot?
    private var editingText: Annot?
    private var moving: (index: Int, last: CGPoint)?
    private var wheelAccum: CGFloat = 0

    init(base: CGImage, unit: CGFloat) {
        self.base = base
        self.unit = unit
        pixelCache = base
        super.init(frame: .zero)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    var extent: CGRect { CGRect(x: 0, y: 0, width: base.width, height: base.height) }
    var visible: CGRect { state.crop ?? extent }

    // Fit, never larger than 1 point per capture point.
    var zoom: CGFloat {
        let avail = bounds.insetBy(dx: 24, dy: 24)
        return max(0.02, min(1 / unit, avail.width / visible.width, avail.height / visible.height))
    }
    var origin: CGPoint {
        let z = zoom
        return CGPoint(x: ((bounds.width - visible.width * z) / 2).rounded(), y: ((bounds.height - visible.height * z) / 2).rounded())
    }
    func toImage(_ p: NSPoint) -> CGPoint {
        let z = zoom, o = origin
        return CGPoint(x: visible.minX + (p.x - o.x) / z, y: visible.minY + (p.y - o.y) / z)
    }

    func pushUndo() {
        undoStack.append(state)
        if undoStack.count > 200 { undoStack.removeFirst() }
        redoStack.removeAll()
        editor?.dirty = true
    }

    func changed(pixels: Bool) {
        if pixels { pixelCache = Render.pixelBase(base, state.annots) }
        needsDisplay = true
        editor?.refreshChrome()
    }

    func undo() {
        commitText()
        guard let s = undoStack.popLast() else { return }
        redoStack.append(state)
        state = s
        selected = nil
        changed(pixels: true)
    }

    func redo() {
        guard let s = redoStack.popLast() else { return }
        undoStack.append(state)
        state = s
        selected = nil
        changed(pixels: true)
    }

    func setTool(_ t: Tool) {
        commitText()
        tool = t
        if t != .select { selected = nil }
        needsDisplay = true
        window?.invalidateCursorRects(for: self)
        editor?.refreshChrome()
    }

    func setColor(_ i: Int) {
        color = i
        if let s = selected, state.annots.indices.contains(s) {
            pushUndo()
            state.annots[s].color = i
            changed(pixels: false)
        }
        editingText?.color = i
        needsDisplay = true
        editor?.refreshChrome()
    }

    func setLevel(_ l: Int) {
        level = min(kLevels - 1, max(0, l))
        if let s = selected, state.annots.indices.contains(s) {
            pushUndo()
            state.annots[s].level = level
            changed(pixels: state.annots[s].tool.isPixel)
        }
        editingText?.level = level
        needsDisplay = true
        editor?.refreshChrome()
    }

    func deleteSelected() {
        guard let s = selected, state.annots.indices.contains(s) else { return }
        pushUndo()
        let px = state.annots[s].tool.isPixel
        state.annots.remove(at: s)
        selected = nil
        changed(pixels: px)
    }

    func duplicateSelected() {
        guard let s = selected, state.annots.indices.contains(s) else { return }
        pushUndo()
        var a = state.annots[s].moved(16 * unit, 16 * unit)
        if a.tool == .step { a.step = nextStep }
        state.annots.append(a)
        selected = state.annots.count - 1
        changed(pixels: a.tool.isPixel)
    }

    var nextStep: Int { (state.annots.filter { $0.tool == .step }.map(\.step).max() ?? 0) + 1 }

    func commitText() {
        guard let t = editingText else { return }
        editingText = nil
        if !t.text.isEmpty {
            pushUndo()
            state.annots.append(t)
        }
        changed(pixels: false)
    }

    private func newAnnot(at p: CGPoint) -> Annot {
        Annot(tool: tool, color: color, level: level, unit: unit, pts: [p, p])
    }

    private func constrain(_ a: CGPoint, _ b: CGPoint, shift: Bool) -> CGPoint {
        guard shift else { return b }
        let dx = b.x - a.x, dy = b.y - a.y
        if tool == .arrow || tool == .line {
            let ang = (atan2(dy, dx) / (.pi / 4)).rounded() * (.pi / 4), len = hypot(dx, dy)
            return CGPoint(x: a.x + cos(ang) * len, y: a.y + sin(ang) * len)
        }
        let s = max(abs(dx), abs(dy))
        return CGPoint(x: a.x + (dx < 0 ? -s : s), y: a.y + (dy < 0 ? -s : s))
    }

    private func hitTest(_ p: CGPoint) -> Int? {
        let slop = 6 / zoom
        return state.annots.indices.reversed().first { Render.hit(state.annots[$0], p, slop: slop) }
    }

    // MARK: mouse

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        let p = toImage(convert(e.locationInWindow, from: nil))
        if editingText != nil && tool == .text {
            commitText()
            return
        }
        switch tool {
        case .select:
            selected = hitTest(p)
            if let s = selected {
                if e.clickCount == 2, state.annots[s].tool == .text {  // edit text again
                    pushUndo()
                    editingText = state.annots.remove(at: s)
                    selected = nil
                    changed(pixels: false)
                    return
                }
                pushUndo()
                moving = (s, p)
            }
            needsDisplay = true
        case .step:
            pushUndo()
            var a = newAnnot(at: p)
            a.pts = [p]
            a.step = nextStep
            state.annots.append(a)
            changed(pixels: false)
        case .text:
            var a = newAnnot(at: p)
            a.pts = [p]
            editingText = a
            needsDisplay = true
        case .pen:
            var a = newAnnot(at: p)
            a.pts = [p]
            live = a
        default:
            live = newAnnot(at: p)
        }
    }

    override func mouseDragged(with e: NSEvent) {
        let p = toImage(convert(e.locationInWindow, from: nil))
        if let m = moving {
            state.annots[m.index] = state.annots[m.index].moved(p.x - m.last.x, p.y - m.last.y)
            moving = (m.index, p)
            if state.annots[m.index].tool.isPixel { pixelCache = Render.pixelBase(base, state.annots) }
            needsDisplay = true
            return
        }
        guard var a = live else { return }
        if a.tool == .pen {
            if let last = a.pts.last, hypot(p.x - last.x, p.y - last.y) * zoom >= 1.5 { a.pts.append(p) }
        } else {
            a.pts[1] = constrain(a.pts[0], p, shift: e.modifierFlags.contains(.shift))
        }
        live = a
        needsDisplay = true
    }

    override func mouseUp(with e: NSEvent) {
        if moving != nil {
            moving = nil
            changed(pixels: true)
            return
        }
        guard let a = live else { return }
        live = nil
        let r = a.rect
        let minSize = 3 / zoom
        if a.tool == .crop {
            if r.width >= minSize && r.height >= minSize {
                pushUndo()
                state.crop = r.integral.intersection(visible)
                changed(pixels: false)
            }
            needsDisplay = true
            return
        }
        if a.tool != .pen && r.width < minSize && r.height < minSize {  // just a click: nothing to add
            needsDisplay = true
            return
        }
        pushUndo()
        state.annots.append(a)
        changed(pixels: a.tool.isPixel)
    }

    override func scrollWheel(with e: NSEvent) {
        wheelAccum += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY / 30 : e.scrollingDeltaY
        if abs(wheelAccum) >= 1 {
            setLevel(level + (wheelAccum > 0 ? 1 : -1))
            wheelAccum = 0
        }
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: tool == .select ? .arrow : tool == .text ? .iBeam : .crosshair)
    }

    // MARK: keyboard

    override func keyDown(with e: NSEvent) {
        guard let ed = editor else { return }
        let f = e.modifierFlags
        let cmd = f.contains(.command), shift = f.contains(.shift)
        let code = Int(e.keyCode)
        if cmd {
            switch code {
            case kVK_ANSI_Z: shift ? redo() : undo()
            case kVK_ANSI_Y: redo()
            case kVK_ANSI_C: shift ? ed.copyAnnotations() : ed.copy()
            case kVK_ANSI_V:
                if shift { ed.pasteAnnotations() }
                else if editingText != nil, let s = NSPasteboard.general.string(forType: .string) { editingText?.text += s; needsDisplay = true }
            case kVK_ANSI_D: duplicateSelected()
            case kVK_ANSI_E: ed.toggleStyled()
            case kVK_ANSI_R: ed.autoRedact()
            case kVK_ANSI_S: shift ? ed.saveAs() : ed.save()
            case kVK_ANSI_P: ed.pin()
            case kVK_ANSI_K: ed.commandPalette()
            case kVK_ANSI_W: ed.window.performClose(nil)
            default: super.keyDown(with: e)
            }
            return
        }
        if var t = editingText {
            switch code {
            case kVK_Return, kVK_ANSI_KeypadEnter:
                if shift { t.text += "\n"; editingText = t } else { commitText() }
            case kVK_Delete: if !t.text.isEmpty { t.text.removeLast(); editingText = t }
            case kVK_Escape: commitText()
            default:
                if let s = e.characters, !s.isEmpty, s.unicodeScalars.allSatisfy({ $0.value >= 32 && $0.value != 127 && !(0xF700...0xF8FF).contains($0.value) }) {
                    t.text += s
                    editingText = t
                }
            }
            needsDisplay = true
            return
        }
        if live != nil || moving != nil {
            if code == kVK_Escape { live = nil; moving = nil; needsDisplay = true }
            return
        }
        switch code {
        case kVK_Escape: ed.window.performClose(nil); return
        case kVK_Return, kVK_ANSI_KeypadEnter: ed.done(); return
        case kVK_Delete, kVK_ForwardDelete: deleteSelected(); return
        case kVK_ANSI_LeftBracket: setLevel(level - 1); return
        case kVK_ANSI_RightBracket: setLevel(level + 1); return
        default: break
        }
        guard let ch = e.charactersIgnoringModifiers?.lowercased().first else { return }
        if let n = ch.wholeNumberValue, (1...kColors.count).contains(n) { return setColor(n - 1) }
        if let t = Tool.allCases.first(where: { $0.info.key == ch }) { setTool(t) }
    }

    // MARK: drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        Theme.bg.setFill()
        bounds.fill()
        let z = zoom, o = origin, vis = visible
        let shown = CGRect(x: o.x, y: o.y, width: vis.width * z, height: vis.height * z)
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -2), blur: 18, color: NSColor.black.withAlphaComponent(0.6).cgColor)
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(shown)
        ctx.restoreGState()

        ctx.saveGState()
        ctx.clip(to: shown)
        ctx.translateBy(x: o.x, y: o.y)
        ctx.scaleBy(x: z, y: z)
        ctx.translateBy(x: -vis.minX, y: -vis.minY)
        ctx.interpolationQuality = z < 1 ? .high : .none
        drawImageFlipped(ctx, pixelCache, in: extent)
        let liveSpot = live.flatMap { $0.tool == .spotlight ? $0.rect : nil }
        Render.spotlight(ctx, state.annots, extent: extent, extra: liveSpot)
        for a in state.annots where a.tool != .blur && a.tool != .pixelate { Render.draw(a, ctx) }
        if let live { Render.draw(live, ctx, preview: true) }
        if let t = editingText {
            Render.draw(t, ctx)
            let r = Render.textRect(t)
            let lastLine = Render.textRect(Annot(tool: .text, color: t.color, level: t.level, unit: t.unit, pts: t.pts,
                                                 text: String(t.text.split(separator: "\n", omittingEmptySubsequences: false).last ?? "")))
            let lineH = t.font.ascender - t.font.descender + t.font.leading
            ctx.setFillColor(Theme.accent.cgColor)
            ctx.fill(CGRect(x: r.minX + lastLine.width + 1 * unit, y: r.maxY - lineH, width: max(1.5, 2 * unit), height: lineH))
        }
        if let s = selected, state.annots.indices.contains(s) {
            ctx.setStrokeColor(Theme.accent.cgColor)
            ctx.setLineWidth(1.25 / z)
            ctx.setLineDash(phase: 0, lengths: [4 / z, 3 / z])
            ctx.stroke(Render.bounds(state.annots[s]).insetBy(dx: -4 / z, dy: -4 / z))
        }
        ctx.restoreGState()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        editor?.refreshChrome()
    }
}

// MARK: - Toolbar

final class EditorToolbar: NSView {
    weak var editor: Editor?
    private var toolButtons: [Tool: NSButton] = [:]
    private var swatches: [Swatch] = []
    private var sizeLabel = NSTextField(labelWithString: "")
    private var styledButton: NSButton?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Theme.surface.cgColor
    }
    required init?(coder: NSCoder) { fatalError() }

    private func button(_ symbol: String, _ tip: String, _ action: Selector, tag: Int = 0) -> NSButton {
        let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip) ?? NSImage(), target: self, action: action)
        b.bezelStyle = .regularSquare
        b.isBordered = false
        b.toolTip = tip
        b.tag = tag
        b.contentTintColor = Theme.textDim
        b.symbolConfiguration = .init(pointSize: 15, weight: .regular)
        b.wantsLayer = true
        b.layer?.cornerRadius = 7
        b.widthAnchor.constraint(equalToConstant: 30).isActive = true
        b.heightAnchor.constraint(equalToConstant: 32).isActive = true
        return b
    }

    private func separator() -> NSView {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.backgroundColor = Theme.border.cgColor
        v.widthAnchor.constraint(equalToConstant: 1).isActive = true
        v.heightAnchor.constraint(equalToConstant: 24).isActive = true
        return v
    }

    func build() {
        var views: [NSView] = []
        for t in Tool.allCases {
            let b = button(t.info.symbol, "\(t.info.name)  (\(String(t.info.key).uppercased()))", #selector(toolClicked(_:)), tag: t.rawValue)
            toolButtons[t] = b
            views.append(b)
        }
        views.append(separator())
        for i in kColors.indices {
            let s = Swatch(color: kColors[i], index: i)
            s.toolTip = "\(kColorNames[i])  (\(i + 1))"
            s.onClick = { [weak self] i in self?.editor?.canvas.setColor(i) }
            swatches.append(s)
            views.append(s)
        }
        views.append(separator())
        views.append(button("minus", "Smaller  ([)", #selector(smaller)))
        sizeLabel.font = Theme.mono(11)
        sizeLabel.textColor = Theme.textDim
        sizeLabel.alignment = .center
        sizeLabel.widthAnchor.constraint(equalToConstant: 34).isActive = true
        views.append(sizeLabel)
        views.append(button("plus", "Larger  (])", #selector(larger)))

        let left = NSStackView(views: views)
        left.spacing = 2
        left.orientation = .horizontal
        left.alignment = .centerY

        let styled = button("sparkles", "Styled export  (⌘E)", #selector(styledClicked))
        styledButton = styled
        let done = NSButton(title: "Done", target: self, action: #selector(doneClicked))
        done.bezelStyle = .rounded
        done.isBordered = false
        done.wantsLayer = true
        done.layer?.backgroundColor = Theme.accent.cgColor
        done.layer?.cornerRadius = 7
        done.attributedTitle = NSAttributedString(string: "Done", attributes: [.foregroundColor: Theme.onAccent, .font: Theme.font(13, .semibold)])
        done.toolTip = "Copy, save and close  (↩)"
        done.widthAnchor.constraint(equalToConstant: 64).isActive = true
        done.heightAnchor.constraint(equalToConstant: 30).isActive = true
        let right = NSStackView(views: [
            button("arrow.uturn.backward", "Undo  (⌘Z)", #selector(undoClicked)),
            button("arrow.uturn.forward", "Redo  (⌘⇧Z)", #selector(redoClicked)),
            button("eye.slash", "Auto-redact  (⌘R)", #selector(redactClicked)),
            styled,
            button("pin", "Pin to screen  (⌘P)", #selector(pinClicked)),
            button("doc.on.doc", "Copy  (⌘C)", #selector(copyClicked)),
            button("square.and.arrow.down", "Save  (⌘S)", #selector(saveClicked)),
            done,
        ])
        right.spacing = 2
        right.orientation = .horizontal
        for s in [left, right] {
            s.translatesAutoresizingMaskIntoConstraints = false
            addSubview(s)
        }
        let line = NSView()
        line.wantsLayer = true
        line.layer?.backgroundColor = Theme.border.cgColor
        line.translatesAutoresizingMaskIntoConstraints = false
        addSubview(line)
        NSLayoutConstraint.activate([
            left.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            left.centerYAnchor.constraint(equalTo: centerYAnchor),
            right.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            right.centerYAnchor.constraint(equalTo: centerYAnchor),
            right.leadingAnchor.constraint(greaterThanOrEqualTo: left.trailingAnchor, constant: 8),
            line.leadingAnchor.constraint(equalTo: leadingAnchor), line.trailingAnchor.constraint(equalTo: trailingAnchor),
            line.bottomAnchor.constraint(equalTo: bottomAnchor), line.heightAnchor.constraint(equalToConstant: 1),
        ])
        left.setHuggingPriority(.defaultHigh, for: .horizontal)
        left.setClippingResistancePriority(.defaultLow, for: .horizontal)
    }

    func sync() {
        guard let c = editor?.canvas else { return }
        for (t, b) in toolButtons {
            let on = t == c.tool
            b.layer?.backgroundColor = on ? Theme.selected.cgColor : NSColor.clear.cgColor
            b.contentTintColor = on ? Theme.accent : Theme.textDim
        }
        for s in swatches { s.selected = s.index == c.color }
        sizeLabel.stringValue = "\(c.level + 1)/\(kLevels)"
        styledButton?.contentTintColor = editor?.styled == true ? Theme.accent : Theme.textDim
        styledButton?.layer?.backgroundColor = editor?.styled == true ? Theme.selected.cgColor : NSColor.clear.cgColor
    }

    @objc private func toolClicked(_ b: NSButton) { editor?.canvas.setTool(Tool(rawValue: b.tag) ?? .arrow) }
    @objc private func smaller() { editor.map { $0.canvas.setLevel($0.canvas.level - 1) } }
    @objc private func larger() { editor.map { $0.canvas.setLevel($0.canvas.level + 1) } }
    @objc private func undoClicked() { editor?.canvas.undo() }
    @objc private func redoClicked() { editor?.canvas.redo() }
    @objc private func redactClicked() { editor?.autoRedact() }
    @objc private func styledClicked() { editor?.toggleStyled() }
    @objc private func pinClicked() { editor?.pin() }
    @objc private func copyClicked() { editor?.copy() }
    @objc private func saveClicked() { editor?.save() }
    @objc private func doneClicked() { editor?.done() }
}

private final class Swatch: NSView {
    let color: NSColor
    let index: Int
    var onClick: ((Int) -> Void)?
    var selected = false { didSet { needsDisplay = true } }

    init(color: NSColor, index: Int) {
        self.color = color
        self.index = index
        super.init(frame: .zero)
        widthAnchor.constraint(equalToConstant: 21).isActive = true
        heightAnchor.constraint(equalToConstant: 32).isActive = true
    }
    required init?(coder: NSCoder) { fatalError() }

    override func draw(_ dirtyRect: NSRect) {
        let r = NSRect(x: bounds.midX - 7, y: bounds.midY - 7, width: 14, height: 14)
        if selected {
            Theme.text.setStroke()
            let ring = NSBezierPath(ovalIn: r.insetBy(dx: -3, dy: -3))
            ring.lineWidth = 1.5
            ring.stroke()
        }
        color.setFill()
        NSBezierPath(ovalIn: r).fill()
        Theme.border.setStroke()
        NSBezierPath(ovalIn: r).stroke()
    }

    override func mouseDown(with event: NSEvent) { onClick?(index) }
}
