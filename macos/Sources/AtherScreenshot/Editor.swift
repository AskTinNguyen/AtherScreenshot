import AppKit
import Carbon.HIToolbox
import UniformTypeIdentifiers

final class Editor: NSObject, NSWindowDelegate {
    static var instances: [Editor] = []
    static var annotClipboard: [Annot] = []

    let base: CGImage
    let unit: CGFloat
    let window: NSWindow
    private var source: URL?
    private var info = NameInfo()
    let canvas: EditorCanvas
    private let toolbar: EditorToolbar
    private let status = NSTextField(labelWithString: "")
    var styled = Settings.shared.bool("StyledExport")
    var dirty = false
    private var redacting = false
    var collage: CollageSpec?
    private let canvasBar = FloatingBar()
    private let collageBar = FloatingBar()

    // `source`: the file being edited, if any; `info`: where the image was captured. Both carry over to the saved copy.
    @discardableResult
    static func open(_ img: CGImage, scale: CGFloat? = nil, source: URL? = nil, info: NameInfo = NameInfo()) -> Editor {
        let e = Editor(img, unit: scale ?? Geo.mouseScreen.backingScaleFactor)
        e.source = source
        e.info = info
        instances.append(e)
        return e
    }

    // A collage starts as an empty canvas with one image layer per screenshot.
    @discardableResult
    static func openCollage(_ urls: [URL]) -> Editor? {
        let pairs = urls.compactMap { u in CGImage.load(u).map { ($0, u) } }
        guard pairs.count >= 2 else { Toast.shared.show("Pick at least two images for a collage"); return nil }
        let scale = Geo.mouseScreen.backingScaleFactor
        let blank = makeContext(width: 1, height: 1)!.makeImage()!
        let e = Editor(blank, unit: scale)
        e.collage = CollageSpec(images: pairs.map(\.0), sources: pairs.map(\.1))
        e.canvas.tool = .select   // move and swap screenshots first
        instances.append(e)
        e.applyCollage(undoable: false)
        e.fitWindow()
        e.dirty = true
        return e
    }

    static func open(url: URL) {
        guard let img = CGImage.load(url) else {
            Toast.shared.show("Can't open that image", url.lastPathComponent)
            return
        }
        // Treat files as Retina-scale when they came from a Retina display (most captures do).
        open(img, scale: Geo.mouseScreen.backingScaleFactor, source: url)
    }

    private init(_ img: CGImage, unit: CGFloat) {
        base = img
        self.unit = max(1, unit)
        canvas = EditorCanvas(base: img, unit: self.unit)
        toolbar = EditorToolbar()
        let vis = Geo.mouseScreen.visibleFrame
        let want = NSSize(width: CGFloat(img.width) / self.unit + 48, height: CGFloat(img.height) / self.unit + 48 + 52 + 26)
        let size = NSSize(width: min(max(want.width, 1150), vis.width * 0.92), height: min(max(want.height, 520), vis.height * 0.92))
        window = NSWindow(contentRect: NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        window.title = "Annotate — \(img.width) × \(img.height)"
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 1150, height: 420)  // fits the whole toolbar
        window.delegate = self
        window.tabbingMode = .disallowed

        let root = NSView()
        toolbar.editor = self
        canvas.editor = self
        canvas.state.fill = Render.edgeColor(img)
        status.font = Theme.font(11)
        status.textColor = Theme.muted
        status.lineBreakMode = .byTruncatingTail
        let statusBar = NSView()
        statusBar.wantsLayer = true
        statusBar.layer?.backgroundColor = Theme.surface.cgColor
        for v in [toolbar, canvas, statusBar, status, canvasBar, collageBar] as [NSView] {
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
            canvasBar.centerXAnchor.constraint(equalTo: canvas.centerXAnchor),
            canvasBar.bottomAnchor.constraint(equalTo: canvas.bottomAnchor, constant: -14),
            collageBar.centerXAnchor.constraint(equalTo: canvas.centerXAnchor),
            collageBar.bottomAnchor.constraint(equalTo: canvas.bottomAnchor, constant: -14),
        ])
        buildBars()
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
        let f = Render.frame(base, canvas.state)
        let size = "\(Int(f.width)) × \(Int(f.height))"
        window.title = collage.map { "Collage — \($0.images.count) screenshots, \(size)" } ?? "Annotate — \(size)"
        canvasBar.isHidden = canvas.tool != .canvas
        collageBar.isHidden = collage == nil || canvas.tool == .canvas
        let reserve: CGFloat = canvasBar.isHidden && collageBar.isHidden ? 0 : 52
        if canvas.reservedBottom != reserve { canvas.reservedBottom = reserve; canvas.needsDisplay = true }
        status.stringValue = "\(canvas.tool.info.name):  \(canvas.tool.info.hint)     ·     \(size)  ·  \(Int((canvas.zoom * 100 * unit).rounded()))%"
            + (styled ? "  ·  Styled export" : "") + "     ·     ⌘K all commands"
    }

    // MARK: actions

    func export() -> CGImage {
        canvas.commitText()
        let img = Render.compose(base, canvas.state, pixel: (canvas.pixelCache, canvas.pixelRect))
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
        var info = self.info
        info.w = img.width
        info.h = img.height
        let url = Output.newCaptureURL(ext: "png", info: info)
        Library.shared.noteEdit(url, from: source, info: info, edited: !canvas.state.annots.isEmpty || img.width != base.width || img.height != base.height,
                                includes: includes, collage: collage != nil)
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
        panel.nameFieldStringValue = Output.makeCaptureURL(base: FileManager.default.temporaryDirectory, ext: "png", info: NameInfo()).deletingPathExtension().lastPathComponent
        panel.beginSheetModal(for: window) { r in
            guard r == .OK, let url = panel.url else { return }
            Library.shared.noteEdit(url, from: self.source, info: self.info, edited: true, includes: self.includes, collage: self.collage != nil)
            Output.savePNG(img, to: url) { ok in
                Toast.shared.show(ok ? "Saved" : "Save failed", url.lastPathComponent)
                if ok { self.dirty = false }  // a failed save must still warn before closing
            }
        }
    }

    // Screenshots placed on the canvas, recorded on the saved copy.
    private var includes: [URL] {
        var seen = Set<String>()
        return canvas.state.annots.compactMap { $0.layer?.source }.filter { seen.insert($0.path).inserted && $0 != source }
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
        let (img, f) = Render.raster(base, canvas.state), unit = unit   // includes image layers and added space
        OCR.async({ OCR.findSensitive(try OCR.words(img)).map { $0.offsetBy(dx: f.minX, dy: f.minY) } }) { [weak self] r in
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
            ("Insert image…", "screenshot overlay picture layer", "photo.badge.plus", "I", { [weak self] in self?.pickImage() }),
            ("Add space below for notes", "canvas margin extend border", "rectangle.bottomhalf.inset.filled", "", { [weak self] in self?.canvas.extend(.below) }),
            ("Add space on the right", "canvas margin extend border", "rectangle.righthalf.inset.filled", "", { [weak self] in self?.canvas.extend(.right) }),
            ("Add an even margin", "canvas margin extend border padding", "rectangle.inset.filled", "", { [weak self] in self?.canvas.extend(.even) }),
            ("Remove added space and crop", "canvas reset", "arrow.uturn.backward.square", "", { [weak self] in self?.canvas.resetFrame() }),
            ("Bring to front", "image layer order", "square.3.layers.3d.top.filled", "⌘]", { [weak self] in self?.canvas.reorderSelected(front: true) }),
            ("Send to back", "image layer order", "square.3.layers.3d.bottom.filled", "⌘[", { [weak self] in self?.canvas.reorderSelected(front: false) }),
            ("Close editor", "quit", "xmark", "⎋", { [weak self] in self?.window.performClose(nil) }),
        ]
        for (i, a) in acts.enumerated() { items.append(PaletteItem(id: "act.\(i)", title: a.0, keywords: a.1, icon: a.2, hint: a.3, action: a.4)) }
        for (i, n) in kColorNames.enumerated() {
            items.append(PaletteItem(id: "color.\(i)", title: "Color: \(n)", keywords: "colour", icon: "paintpalette", hint: "\(i + 1)") { [weak self] in self?.canvas.setColor(i) })
        }
        Palette.shared.show(items, placeholder: "Editor commands…", mruKey: "EditorPaletteMRU")
    }

    // MARK: images and collage

    func pickImage() {
        canvas.commitText()
        let recent = Library.shared.urls.filter { MediaType.of($0) == .image }
            .sorted { Library.shared.meta($0).mtime > Library.shared.meta($1).mtime }.prefix(300)
        var items = [PaletteItem(id: "pick.file", title: "Choose a file…", keywords: "open finder disk", icon: "folder", hint: "") { [weak self] in
            guard let self else { return }
            let p = NSOpenPanel()
            p.allowedContentTypes = [.image]
            p.allowsMultipleSelection = true
            p.beginSheetModal(for: self.window) { r in if r == .OK { self.insertImages(p.urls) } }
        }]
        for u in recent {
            let m = Library.shared.meta(u)
            items.append(PaletteItem(id: "pick." + u.path, title: u.deletingPathExtension().lastPathComponent,
                                     keywords: ([m.app, m.window] + m.tags).joined(separator: " "), icon: "photo",
                                     hint: m.w > 0 ? "\(m.w) × \(m.h)" : "") { [weak self] in self?.insertImages([u]) })
        }
        Palette.shared.show(items, placeholder: "Insert a screenshot…", mruKey: "EditorInsertMRU")
    }

    func insertImages(_ urls: [URL], at p: CGPoint? = nil) {
        var at = p
        for u in urls {
            guard let img = CGImage.load(u) else { Toast.shared.show("Can't open that image", u.lastPathComponent); continue }
            canvas.insert(img, source: u, at: at)
            at = at.map { CGPoint(x: $0.x + 24 * unit, y: $0.y + 24 * unit) }
        }
        activateApp()
        window.makeKeyAndOrderFront(nil)
    }

    func applyCollage(undoable: Bool = true) {
        guard let spec = collage else { return }
        canvas.commitText()
        if undoable { canvas.pushUndo() }
        let r = Collage.layout(spec, unit: unit)
        var others = canvas.state.annots.filter { $0.layer?.slot == nil }
        var layers: [Annot] = []
        for (pos, slot) in spec.order.enumerated() {
            let rect = r.rects[pos]
            var a = Annot(tool: .image, color: 6, level: 0, unit: unit, pts: [rect.origin, CGPoint(x: rect.maxX, y: rect.maxY)])
            a.layer = ImageLayer(image: spec.images[slot], source: spec.sources[slot], radius: r.radius, shadow: spec.shadow, slot: slot)
            layers.append(a)
        }
        others.insert(contentsOf: layers, at: 0)
        canvas.state.annots = others
        canvas.state.crop = CGRect(origin: .zero, size: r.size)
        canvas.state.fill = CollageSpec.backgrounds[spec.background].1
        canvas.selected = nil
        canvas.changed(pixels: true)
        syncBars()
    }

    // Dropping a collage image on another swaps their places.
    func swapCollage(_ a: Int, _ b: Int) {
        guard var spec = collage, let i = spec.order.firstIndex(of: a), let j = spec.order.firstIndex(of: b) else { return }
        spec.order.swapAt(i, j)
        collage = spec
        applyCollage()
    }

    func fitWindow() {
        let f = Render.frame(base, canvas.state)
        let vis = window.screen?.visibleFrame ?? Geo.mouseScreen.visibleFrame
        let want = NSSize(width: f.width / unit + 48, height: f.height / unit + 48 + 52 + 26 + 60)
        let size = NSSize(width: min(max(want.width, 1150), vis.width * 0.92), height: min(max(want.height, 560), vis.height * 0.92))
        window.setFrame(NSRect(x: vis.midX - size.width / 2, y: vis.midY - size.height / 2, width: size.width, height: size.height), display: true)
    }

    private func buildBars() {
        canvasBar.add(FloatingBar.button("Space below", "rectangle.bottomhalf.inset.filled") { [weak self] in self?.canvas.extend(.below) })
        canvasBar.add(FloatingBar.button("Space right", "rectangle.righthalf.inset.filled") { [weak self] in self?.canvas.extend(.right) })
        canvasBar.add(FloatingBar.button("Even margin", "rectangle.inset.filled") { [weak self] in self?.canvas.extend(.even) })
        canvasBar.addSeparator()
        canvasBar.add(FloatingBar.popup(["Fill: Edge color", "Fill: White", "Fill: Black", "Fill: Transparent"], tag: 1) { [weak self] i in
            guard let self else { return }
            let colors: [CGColor?] = [Render.edgeColor(self.base), CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1), CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 1), nil]
            self.canvas.pushUndo()
            self.canvas.state.fill = colors[i]
            self.canvas.changed(pixels: true)
        })
        canvasBar.add(FloatingBar.button("Reset", "arrow.uturn.backward") { [weak self] in self?.canvas.resetFrame() })

        collageBar.add(FloatingBar.popup(CollageLayout.allCases.map { "Layout: " + $0.label }, tag: 10) { [weak self] i in self?.mutateCollage { $0.layout = CollageLayout(rawValue: i) ?? .auto } })
        collageBar.add(FloatingBar.popup(CollageSpec.stepNames.map { "Gap: " + $0 }, tag: 11) { [weak self] i in self?.mutateCollage { $0.gap = i } })
        collageBar.add(FloatingBar.popup(CollageSpec.stepNames.map { "Margin: " + $0 }, tag: 12) { [weak self] i in self?.mutateCollage { $0.margin = i } })
        collageBar.add(FloatingBar.popup(CollageSpec.backgrounds.map { "Background: " + $0.0 }, tag: 13) { [weak self] i in self?.mutateCollage { $0.background = i } })
        collageBar.add(FloatingBar.popup(["Corners: Rounded", "Corners: Square"], tag: 14) { [weak self] i in self?.mutateCollage { $0.rounded = i == 0 } })
        collageBar.add(FloatingBar.popup(["No shadow", "Shadow"], tag: 15) { [weak self] i in self?.mutateCollage { $0.shadow = i == 1 } })
        collageBar.add(FloatingBar.popup(CollageSize.allCases.map(\.label), tag: 16) { [weak self] i in self?.mutateCollage { $0.size = CollageSize(rawValue: i) ?? .fit } })
    }

    private func mutateCollage(_ f: (inout CollageSpec) -> Void) {
        guard var spec = collage else { return }
        f(&spec)
        collage = spec
        applyCollage()
    }

    private func syncBars() {
        guard let c = collage else { return }
        collageBar.select(tag: 10, c.layout.rawValue)
        collageBar.select(tag: 11, c.gap)
        collageBar.select(tag: 12, c.margin)
        collageBar.select(tag: 13, c.background)
        collageBar.select(tag: 14, c.rounded ? 0 : 1)
        collageBar.select(tag: 15, c.shadow ? 1 : 0)
        collageBar.select(tag: 16, c.size.rawValue)
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
    var pixelRect: CGRect
    private var undoStack: [DocState] = []
    private var redoStack: [DocState] = []

    var tool: Tool = .arrow
    var color = 0
    var level = 1
    var selected: Int?
    private var live: Annot?
    private var editingText: Annot?
    private var moving: (index: Int, last: CGPoint, start: CGPoint)?
    private var resizing: (index: Int, anchor: CGPoint, aspect: CGFloat)?
    private var framing: (edges: Edges, start: CGRect, from: CGPoint)?
    private var frozen: (zoom: CGFloat, origin: CGPoint, viewMin: CGPoint)?   // keeps the view still while the frame changes
    private var wheelAccum: CGFloat = 0
    private var menuActions: [MenuAction] = []
    var reservedBottom: CGFloat = 0   // room for a floating bar under the canvas

    struct Edges: OptionSet {
        let rawValue: Int
        static let left = Edges(rawValue: 1), right = Edges(rawValue: 2), top = Edges(rawValue: 4), bottom = Edges(rawValue: 8)
    }

    enum Extend { case below, right, even }

    init(base: CGImage, unit: CGFloat) {
        self.base = base
        self.unit = unit
        pixelCache = base
        pixelRect = CGRect(x: 0, y: 0, width: base.width, height: base.height)
        super.init(frame: .zero)
        wantsLayer = true
        registerForDraggedTypes([.fileURL, .png, .tiff])
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    var extent: CGRect { CGRect(x: 0, y: 0, width: base.width, height: base.height) }
    var visible: CGRect { Render.frame(base, state) }

    // Fit, never larger than 1 point per capture point.
    var zoom: CGFloat {
        if let f = frozen { return f.zoom }
        let avail = bounds.insetBy(dx: 24, dy: 24)
        return max(0.02, min(1 / unit, avail.width / visible.width, (avail.height - reservedBottom) / visible.height))
    }
    var origin: CGPoint {
        if let f = frozen { return f.origin }
        let z = zoom
        return CGPoint(x: ((bounds.width - visible.width * z) / 2).rounded(), y: ((bounds.height - reservedBottom - visible.height * z) / 2).rounded())
    }
    private var viewMin: CGPoint { frozen?.viewMin ?? visible.origin }
    func toImage(_ p: NSPoint) -> CGPoint {
        let z = zoom, o = origin, m = viewMin
        return CGPoint(x: m.x + (p.x - o.x) / z, y: m.y + (p.y - o.y) / z)
    }

    func pushUndo() {
        undoStack.append(state)
        if undoStack.count > 200 { undoStack.removeFirst() }
        redoStack.removeAll()
        editor?.dirty = true
    }

    func changed(pixels: Bool) {
        if pixels { (pixelCache, pixelRect) = Render.pixelBase(base, state) }
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
        if t == .image { editor?.pickImage(); return }   // an action, not a mode
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
            changed(pixels: state.annots[s].tool == .image)
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
            changed(pixels: state.annots[s].tool.isRaster)
        }
        editingText?.level = level
        needsDisplay = true
        editor?.refreshChrome()
    }

    func deleteSelected() {
        guard let s = selected, state.annots.indices.contains(s) else { return }
        pushUndo()
        let px = state.annots[s].tool.isRaster
        state.annots.remove(at: s)
        selected = nil
        changed(pixels: px)
    }

    func duplicateSelected() {
        guard let s = selected, state.annots.indices.contains(s) else { return }
        pushUndo()
        var a = state.annots[s].moved(16 * unit, 16 * unit)
        if a.tool == .step { a.step = nextStep }
        a.layer?.slot = nil   // a copy is a free layer, not part of the collage layout
        state.annots.append(a)
        selected = state.annots.count - 1
        changed(pixels: a.tool.isRaster)
    }

    func reorderSelected(front: Bool) {
        guard let s = selected, state.annots.indices.contains(s) else { return }
        pushUndo()
        let a = state.annots.remove(at: s)
        if front { state.annots.append(a); selected = state.annots.count - 1 } else { state.annots.insert(a, at: 0); selected = 0 }
        changed(pixels: a.tool.isRaster)
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

    // MARK: image layers

    // Places a screenshot centered at `p` (or the middle of the canvas), at most 60% of the canvas.
    func insert(_ img: CGImage, source: URL?, at p: CGPoint? = nil) {
        commitText()
        let v = visible
        let w = CGFloat(img.width), h = CGFloat(img.height)
        let s = min(1, v.width * 0.6 / w, v.height * 0.6 / h)
        let c = p ?? CGPoint(x: v.midX, y: v.midY)
        let r = CGRect(x: c.x - w * s / 2, y: c.y - h * s / 2, width: w * s, height: h * s).integral
        var a = Annot(tool: .image, color: 6, level: 0, unit: unit, pts: [r.origin, CGPoint(x: r.maxX, y: r.maxY)])
        a.layer = ImageLayer(image: img, source: source, radius: 8 * unit, shadow: true)
        pushUndo()
        state.annots.append(a)
        selected = state.annots.count - 1
        tool = .select
        window?.invalidateCursorRects(for: self)
        changed(pixels: true)
    }

    private func handles(_ r: CGRect) -> [CGPoint] {
        [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)]
    }

    private func editLayer(_ i: Int, _ f: (inout ImageLayer) -> Void) {
        guard state.annots.indices.contains(i), var l = state.annots[i].layer else { return }
        pushUndo()
        f(&l)
        state.annots[i].layer = l
        changed(pixels: true)
    }

    override func rightMouseDown(with e: NSEvent) {
        let p = toImage(convert(e.locationInWindow, from: nil))
        guard let i = annotAt(p), let l = state.annots[i].layer else { return super.rightMouseDown(with: e) }
        selected = i
        needsDisplay = true
        menuActions = []
        let menu = NSMenu()
        func item(_ title: String, on: Bool? = nil, _ run: @escaping () -> Void) {
            let a = MenuAction(run)
            menuActions.append(a)
            let it = NSMenuItem(title: title, action: #selector(MenuAction.fire), keyEquivalent: "")
            it.target = a
            if let on { it.state = on ? .on : .off }
            menu.addItem(it)
        }
        item("Bring to front") { [weak self] in self?.reorderSelected(front: true) }
        item("Send to back") { [weak self] in self?.reorderSelected(front: false) }
        menu.addItem(.separator())
        item("Rounded corners", on: l.radius > 0) { [weak self] in guard let self else { return }; self.editLayer(i) { $0.radius = $0.radius > 0 ? 0 : 8 * self.unit } }
        item("Shadow", on: l.shadow) { [weak self] in self?.editLayer(i) { $0.shadow.toggle() } }
        item("Border (uses the current color and size)", on: l.border) { [weak self] in self?.editLayer(i) { $0.border.toggle() } }
        for o in [1.0, 0.75, 0.5, 0.25] {
            item("Opacity \(Int(o * 100))%", on: abs(l.opacity - o) < 0.01) { [weak self] in self?.editLayer(i) { $0.opacity = o } }
        }
        menu.addItem(.separator())
        item("Actual size") { [weak self] in
            guard let self else { return }
            self.pushUndo()
            let r = self.state.annots[i].rect
            self.state.annots[i].pts = [r.origin, CGPoint(x: r.minX + CGFloat(l.image.width), y: r.minY + CGFloat(l.image.height))]
            self.changed(pixels: true)
        }
        item("Delete") { [weak self] in self?.deleteSelected() }
        NSMenu.popUpContextMenu(menu, with: e, for: self)
    }

    // MARK: canvas frame (added space)

    func extend(_ how: Extend) {
        commitText()
        pushUndo()
        let f = visible
        switch how {
        case .below: state.crop = CGRect(x: f.minX, y: f.minY, width: f.width, height: f.height + max(160 * unit, f.height * 0.35))
        case .right: state.crop = CGRect(x: f.minX, y: f.minY, width: f.width + max(240 * unit, f.width * 0.4), height: f.height)
        case .even:
            let m = max(32 * unit, min(f.width, f.height) * 0.06).rounded()
            state.crop = f.insetBy(dx: -m, dy: -m)
        }
        changed(pixels: true)
    }

    func resetFrame() {
        guard state.crop != nil else { return }
        pushUndo()
        state.crop = nil
        changed(pixels: true)
    }

    private func frameEdges(at p: CGPoint) -> Edges {
        let f = visible, t = 10 / zoom
        guard f.insetBy(dx: -t, dy: -t).contains(p) else { return [] }
        var e: Edges = []
        if abs(p.x - f.minX) <= t { e.insert(.left) }
        if abs(p.x - f.maxX) <= t { e.insert(.right) }
        if abs(p.y - f.minY) <= t { e.insert(.top) }
        if abs(p.y - f.maxY) <= t { e.insert(.bottom) }
        return e
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

    private func annotAt(_ p: CGPoint) -> Int? {
        let slop = 6 / zoom
        return state.annots.indices.reversed().first { Render.hit(state.annots[$0], p, slop: slop) }
    }

    private func freeze() { frozen = (zoom, origin, viewMin) }

    // MARK: mouse

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        let p = toImage(convert(e.locationInWindow, from: nil))
        if editingText != nil {  // a click anywhere finishes the text being typed
            commitText()
            if tool == .text { return }
        }
        // Corner handles of a selected image layer resize it, whatever the tool.
        if let s = selected, state.annots.indices.contains(s), state.annots[s].tool == .image {
            let r = state.annots[s].rect
            if let h = handles(r).first(where: { hypot($0.x - p.x, $0.y - p.y) <= 8 / zoom }) {
                pushUndo()
                resizing = (s, CGPoint(x: h.x == r.minX ? r.maxX : r.minX, y: h.y == r.minY ? r.maxY : r.minY), r.width / max(1, r.height))
                return
            }
        }
        switch tool {
        case .canvas:
            let edges = frameEdges(at: p)
            guard !edges.isEmpty else { return }
            pushUndo()
            freeze()
            framing = (edges, visible, p)
        case .select:
            selected = annotAt(p)
            if let s = selected {
                if e.clickCount == 2, state.annots[s].tool == .text {  // edit text again
                    pushUndo()
                    editingText = state.annots.remove(at: s)
                    selected = nil
                    changed(pixels: false)
                    return
                }
                pushUndo()
                moving = (s, p, p)
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
            a.wrap = max(80 * unit, visible.maxX - p.x - 12 * unit)   // wraps at the canvas edge
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
        if let f = framing {
            let dx = p.x - f.from.x, dy = p.y - f.from.y, both = e.modifierFlags.contains(.option)
            var r = f.start
            let minSide = 16 * unit
            if f.edges.contains(.left) { r.origin.x = min(f.start.maxX - minSide, f.start.minX + dx); r.size.width = f.start.maxX - r.minX }
            if f.edges.contains(.right) { r.size.width = max(minSide, f.start.width + dx) }
            if f.edges.contains(.top) { r.origin.y = min(f.start.maxY - minSide, f.start.minY + dy); r.size.height = f.start.maxY - r.minY }
            if f.edges.contains(.bottom) { r.size.height = max(minSide, f.start.height + dy) }
            if both {   // ⌥: the opposite edge moves too
                let gx = r.width - f.start.width, gy = r.height - f.start.height
                if !f.edges.isDisjoint(with: [.left, .right]) { r = CGRect(x: f.start.minX - gx / 2, y: r.minY, width: f.start.width + gx, height: r.height) }
                if !f.edges.isDisjoint(with: [.top, .bottom]) { r = CGRect(x: r.minX, y: f.start.minY - gy / 2, width: r.width, height: f.start.height + gy) }
            }
            state.crop = r.integral
            needsDisplay = true
            editor?.refreshChrome()
            return
        }
        if let rz = resizing {
            var w = abs(p.x - rz.anchor.x), h = abs(p.y - rz.anchor.y)
            if !e.modifierFlags.contains(.shift) {   // keeps proportions unless ⇧
                if w / max(1, h) > rz.aspect { h = w / rz.aspect } else { w = h * rz.aspect }
            }
            w = max(8, w); h = max(8, h)
            let x = p.x < rz.anchor.x ? rz.anchor.x - w : rz.anchor.x, y = p.y < rz.anchor.y ? rz.anchor.y - h : rz.anchor.y
            state.annots[rz.index].pts = [CGPoint(x: x, y: y), CGPoint(x: x + w, y: y + h)]
            (pixelCache, pixelRect) = Render.pixelBase(base, state)
            needsDisplay = true
            return
        }
        if let m = moving {
            state.annots[m.index] = state.annots[m.index].moved(p.x - m.last.x, p.y - m.last.y)
            moving = (m.index, p, m.start)
            if state.annots[m.index].tool.isRaster { (pixelCache, pixelRect) = Render.pixelBase(base, state) }
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
        if framing != nil {
            framing = nil
            frozen = nil
            changed(pixels: true)
            return
        }
        if resizing != nil {
            resizing = nil
            changed(pixels: true)
            return
        }
        if let m = moving {
            moving = nil
            let a = state.annots[m.index]
            // In a collage, dropping one screenshot on another swaps them.
            if let slot = a.layer?.slot, hypot(m.last.x - m.start.x, m.last.y - m.start.y) * zoom > 12 {
                let c = CGPoint(x: a.rect.midX, y: a.rect.midY)
                if let other = state.annots.indices.first(where: { $0 != m.index && state.annots[$0].layer?.slot != nil && state.annots[$0].rect.contains(c) }),
                   let otherSlot = state.annots[other].layer?.slot {
                    undoStack.removeLast()   // the swap records its own undo step
                    state.annots[m.index] = a.moved(m.start.x - m.last.x, m.start.y - m.last.y)
                    editor?.swapCollage(slot, otherSlot)
                    return
                }
            }
            changed(pixels: true)
            return
        }
        guard let a = live else { return }
        live = nil
        let r = a.rect
        let minSize = 3 / zoom
        if a.tool == .crop {
            // A drag that misses the image would crop to nothing, so it's ignored.
            let c = r.integral.intersection(visible)
            if r.width >= minSize && r.height >= minSize && !c.isNull && c.width >= 1 && c.height >= 1 {
                pushUndo()
                state.crop = c
                changed(pixels: true)
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
        addCursorRect(bounds, cursor: tool == .select || tool == .canvas ? .arrow : tool == .text ? .iBeam : .crosshair)
    }

    // MARK: drag and drop, paste

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let p = toImage(convert(sender.draggingLocation, from: nil))
        let pb = sender.draggingPasteboard
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty {
            editor?.insertImages(urls, at: p)
            return true
        }
        if let img = pb.readObjects(forClasses: [NSImage.self])?.first as? NSImage, let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) {
            insert(cg, source: nil, at: p)
            return true
        }
        return false
    }

    private func pasteImage() -> Bool {
        let pb = NSPasteboard.general
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true, .urlReadingContentsConformToTypes: ["public.image"]]) as? [URL], !urls.isEmpty {
            editor?.insertImages(urls)
            return true
        }
        guard let img = pb.readObjects(forClasses: [NSImage.self])?.first as? NSImage,
              let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return false }
        insert(cg, source: nil)
        return true
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
                else if !pasteImage() { NSSound.beep() }
            case kVK_ANSI_D: duplicateSelected()
            case kVK_ANSI_E: ed.toggleStyled()
            case kVK_ANSI_R: ed.autoRedact()
            case kVK_ANSI_S: shift ? ed.saveAs() : ed.save()
            case kVK_ANSI_P: ed.pin()
            case kVK_ANSI_K: ed.commandPalette()
            case kVK_ANSI_W: ed.window.performClose(nil)
            case kVK_ANSI_RightBracket: reorderSelected(front: true)
            case kVK_ANSI_LeftBracket: reorderSelected(front: false)
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
        if live != nil || moving != nil || resizing != nil || framing != nil {
            if code == kVK_Escape { live = nil; needsDisplay = true }
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
        let z = zoom, o = origin, vis = visible, m = viewMin
        let shown = CGRect(x: o.x + (vis.minX - m.x) * z, y: o.y + (vis.minY - m.y) * z, width: vis.width * z, height: vis.height * z)
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -2), blur: 18, color: NSColor.black.withAlphaComponent(0.6).cgColor)
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(shown)
        ctx.restoreGState()
        if state.fill == nil { drawChecker(ctx, shown) }

        ctx.saveGState()
        ctx.clip(to: shown)
        ctx.translateBy(x: o.x, y: o.y)
        ctx.scaleBy(x: z, y: z)
        ctx.translateBy(x: -m.x, y: -m.y)
        ctx.interpolationQuality = z < 1 ? .high : .none
        if let fill = state.fill, pixelRect != vis {   // while an edge is dragged, the raster lags behind the frame
            ctx.setFillColor(fill)
            ctx.fill(vis)
        }
        drawImageFlipped(ctx, pixelCache, in: pixelRect)
        let liveSpot = live.flatMap { $0.tool == .spotlight ? $0.rect : nil }
        Render.spotlight(ctx, state.annots, extent: vis, extra: liveSpot)
        for a in state.annots where a.tool != .blur && a.tool != .pixelate && a.tool != .image { Render.draw(a, ctx) }
        if let live { Render.draw(live, ctx, preview: true) }
        if let t = editingText {
            Render.draw(t, ctx)
            let r = Render.textRect(t)
            let lastLine = Render.textRect(Annot(tool: .text, color: t.color, level: t.level, unit: t.unit, pts: t.pts,
                                                 text: String(t.text.split(separator: "\n", omittingEmptySubsequences: false).last ?? "")))
            let lineH = t.font.ascender - t.font.descender + t.font.leading
            ctx.setFillColor(Theme.accent.cgColor)
            ctx.fill(CGRect(x: min(r.maxX, r.minX + lastLine.width + 1 * unit), y: r.maxY - lineH, width: max(1.5, 2 * unit), height: lineH))
            if let w = t.wrap {   // the wrap edge, faintly
                ctx.setStrokeColor(Theme.accent.withAlphaComponent(0.35).cgColor)
                ctx.setLineWidth(1 / z)
                ctx.setLineDash(phase: 0, lengths: [3 / z, 3 / z])
                ctx.strokeLineSegments(between: [CGPoint(x: t.pts[0].x + w, y: r.minY), CGPoint(x: t.pts[0].x + w, y: r.maxY)])
                ctx.setLineDash(phase: 0, lengths: [])
            }
        }
        if let s = selected, state.annots.indices.contains(s) {
            let a = state.annots[s]
            ctx.setStrokeColor(Theme.accent.cgColor)
            ctx.setLineWidth(1.25 / z)
            ctx.setLineDash(phase: 0, lengths: [4 / z, 3 / z])
            ctx.stroke(Render.bounds(a).insetBy(dx: -4 / z, dy: -4 / z))
            if a.tool == .image {
                ctx.setLineDash(phase: 0, lengths: [])
                for h in handles(a.rect) {
                    let box = CGRect(x: h.x - 5 / z, y: h.y - 5 / z, width: 10 / z, height: 10 / z)
                    ctx.setFillColor(NSColor.white.cgColor)
                    ctx.fillEllipse(in: box)
                    ctx.strokeEllipse(in: box)
                }
            }
        }
        if tool == .canvas {
            ctx.setLineDash(phase: 0, lengths: [5 / z, 4 / z])
            ctx.setStrokeColor(NSColor.white.withAlphaComponent(0.5).cgColor)
            ctx.setLineWidth(1 / z)
            ctx.stroke(extent)   // where the original screenshot is
            ctx.setLineDash(phase: 0, lengths: [])
            ctx.setFillColor(Theme.accent.cgColor)
            for p in [CGPoint(x: vis.midX, y: vis.minY), CGPoint(x: vis.midX, y: vis.maxY), CGPoint(x: vis.minX, y: vis.midY), CGPoint(x: vis.maxX, y: vis.midY)] {
                let horizontal = p.y == vis.minY || p.y == vis.maxY
                let w = (horizontal ? 36 : 6) / z, h = (horizontal ? 6 : 36) / z
                ctx.addPath(CGPath(roundedRect: CGRect(x: p.x - w / 2, y: p.y - h / 2, width: w, height: h), cornerWidth: 3 / z, cornerHeight: 3 / z, transform: nil))
            }
            ctx.fillPath()
        }
        ctx.restoreGState()
    }

    private func drawChecker(_ ctx: CGContext, _ r: CGRect) {
        ctx.saveGState()
        ctx.clip(to: r)
        ctx.setFillColor(NSColor(white: 0.85, alpha: 1).cgColor)
        ctx.fill(r)
        ctx.setFillColor(NSColor(white: 0.7, alpha: 1).cgColor)
        let s: CGFloat = 8
        var y = r.minY
        var row = 0
        while y < r.maxY {
            var x = r.minX + (row % 2 == 0 ? 0 : s)
            while x < r.maxX { ctx.fill(CGRect(x: x, y: y, width: s, height: s)); x += 2 * s }
            y += s; row += 1
        }
        ctx.restoreGState()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        editor?.refreshChrome()
    }
}

final class MenuAction: NSObject {
    let run: () -> Void
    init(_ run: @escaping () -> Void) { self.run = run }
    @objc func fire() { run() }
}

// Small floating HUD over the canvas (canvas and collage options).
final class FloatingBar: NSVisualEffectView {
    private let stack = NSStackView()
    private var popups: [Int: NSPopUpButton] = [:]
    private var handlers: [MenuAction] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        material = .hudWindow
        blendingMode = .withinWindow
        state = .active
        wantsLayer = true
        layer?.cornerRadius = 12
        layer?.borderWidth = 0.5
        layer?.borderColor = NSColor.white.withAlphaComponent(0.12).cgColor
        isHidden = true
        stack.orientation = .horizontal
        stack.spacing = 4
        stack.edgeInsets = NSEdgeInsets(top: 5, left: 8, bottom: 5, right: 8)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor), stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor), stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }
    required init?(coder: NSCoder) { fatalError() }

    func add(_ v: NSView) {
        if let b = v as? BarButton { handlers.append(b.action0) }
        if let p = v as? BarPopup { handlers.append(p.action0); popups[p.tag] = p }
        stack.addArrangedSubview(v)
    }

    func addSeparator() {
        let v = NSView()
        v.wantsLayer = true
        v.layer?.backgroundColor = NSColor.white.withAlphaComponent(0.15).cgColor
        v.widthAnchor.constraint(equalToConstant: 1).isActive = true
        v.heightAnchor.constraint(equalToConstant: 18).isActive = true
        stack.addArrangedSubview(v)
    }

    func select(tag: Int, _ i: Int) { popups[tag]?.selectItem(at: i) }

    final class BarButton: NSButton { var action0 = MenuAction {} }
    final class BarPopup: NSPopUpButton { var action0 = MenuAction {} }

    static func button(_ title: String, _ symbol: String, _ run: @escaping () -> Void) -> NSView {
        let b = BarButton(title: title, image: NSImage(systemSymbolName: symbol, accessibilityDescription: title) ?? NSImage(), target: nil, action: nil)
        b.action0 = MenuAction(run)
        b.target = b.action0
        b.action = #selector(MenuAction.fire)
        b.bezelStyle = .recessed
        b.imagePosition = .imageLeading
        b.font = Theme.font(12)
        b.contentTintColor = Theme.text
        return b
    }

    static func popup(_ items: [String], tag: Int, _ run: @escaping (Int) -> Void) -> NSView {
        let p = BarPopup(frame: .zero, pullsDown: false)
        p.addItems(withTitles: items)
        p.tag = tag
        p.bezelStyle = .recessed
        p.font = Theme.font(12)
        p.action0 = MenuAction { [weak p] in run(p?.indexOfSelectedItem ?? 0) }
        p.target = p.action0
        p.action = #selector(MenuAction.fire)
        return p
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
