import AppKit
import Carbon.HIToolbox
import QuickLookThumbnailing
import SwiftUI

// Thumbnail grid of every capture, newest first. Search matches name, date, type and the OCR'd text.
final class HistoryModel: ObservableObject {
    @Published var items: [URL] = []
    @Published var query = ""
    @Published var selection: URL?
    @Published var ocr: [String: String] = [:]
    var columns = 4
    private var indexing = false

    private static var indexURL: URL { Settings.supportFolder.appendingPathComponent("ocr-index.json") }
    private struct Entry: Codable { var mtime: Double; var text: String }
    private var index: [String: Entry] = [:]

    init() {
        if let d = try? Data(contentsOf: HistoryModel.indexURL), let m = try? JSONDecoder().decode([String: Entry].self, from: d) { index = m }
        ocr = index.mapValues(\.text)
    }

    static let dateFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f
    }()

    static func mtime(_ u: URL) -> Date { (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast }

    var filtered: [URL] {
        let words = query.lowercased().split(separator: " ").map(String.init)
        guard !words.isEmpty else { return items }
        return items.filter { u in
            let ext = u.pathExtension.lowercased()
            let kind = ext == "mp4" || ext == "mov" ? "video mp4 recording" : ext == "gif" ? "gif animation" : "image png screenshot"
            let hay = [u.lastPathComponent, HistoryModel.dateFormat.string(from: HistoryModel.mtime(u)), kind, ocr[u.path] ?? ""].joined(separator: " ").lowercased()
            return words.allSatisfy { hay.contains($0) }
        }
    }

    func reload() {
        DispatchQueue.global(qos: .userInitiated).async {
            let list = Output.listCaptures()
            DispatchQueue.main.async {
                self.items = list
                if let s = self.selection, !list.contains(s) { self.selection = nil }
                if self.selection == nil { self.selection = self.filtered.first }
                self.indexOCR()
            }
        }
    }

    // OCR images in the background; cached by path and modification time.
    private func indexOCR() {
        guard !indexing else { return }
        let todo = items.filter { u in
            ["png", "jpg", "jpeg", "heic", "tiff"].contains(u.pathExtension.lowercased()) &&
                index[u.path]?.mtime != HistoryModel.mtime(u).timeIntervalSince1970
        }
        guard !todo.isEmpty else { return }
        indexing = true
        DispatchQueue.global(qos: .utility).async {
            var batch: [String: Entry] = [:]
            for (i, u) in todo.enumerated() {
                let text = CGImage.load(u).flatMap { try? OCR.text($0) } ?? ""
                batch[u.path] = Entry(mtime: HistoryModel.mtime(u).timeIntervalSince1970, text: text.replacingOccurrences(of: "\n", with: " "))
                if batch.count >= 10 || i == todo.count - 1 {
                    let b = batch
                    batch.removeAll()
                    DispatchQueue.main.async {
                        self.index.merge(b) { $1 }
                        self.ocr.merge(b.mapValues(\.text)) { $1 }
                        if let d = try? JSONEncoder().encode(self.index) { try? d.write(to: HistoryModel.indexURL) }
                    }
                }
            }
            DispatchQueue.main.async { self.indexing = false }
        }
    }

    func move(_ d: Int) {
        let list = filtered
        guard !list.isEmpty else { return }
        let i = selection.flatMap { list.firstIndex(of: $0) } ?? -d
        selection = list[min(max(0, i + d), list.count - 1)]
    }

    // MARK: actions

    var isImage: Bool { selection.map { !["mp4", "mov", "gif"].contains($0.pathExtension.lowercased()) } ?? false }

    func open(_ u: URL? = nil) {
        guard let u = u ?? selection else { return }
        if ["mp4", "mov", "gif"].contains(u.pathExtension.lowercased()) { Output.open(u) } else { Editor.open(url: u) }
    }

    func copy() {
        guard let u = selection else { return }
        if isImage, let img = CGImage.load(u) { copyImage(img) } else { copyFile(u) }
        Toast.shared.show("Copied", u.lastPathComponent)
    }

    func pin() { if let u = selection, isImage, let img = CGImage.load(u) { Pin.show(img) } }
    func reveal() { if let u = selection { Output.reveal(u) } }
    func upload() { if let u = selection { AppDelegate.shared?.upload(u) } }

    func copyOCRText() {
        guard let u = selection, isImage, let img = CGImage.load(u) else { return }
        OCR.async({ try OCR.text(img) }) { r in
            if case .success(let t) = r, !t.isEmpty {
                copyText(t)
                Toast.shared.show("Text copied", String(t.prefix(280)))
            } else { Toast.shared.show("No text found") }
        }
    }

    func rename() {
        guard let u = selection else { return }
        Palette.shared.prompt("Rename  ·  ↩ renames, ⎋ keeps the current name", initial: u.deletingPathExtension().lastPathComponent) { name in
            guard let n = Output.rename(u, to: name) else { return Toast.shared.show("Rename failed", u.lastPathComponent) }
            self.selection = n
            self.reload()
        }
    }

    func trash() {
        guard let u = selection else { return }
        let list = filtered
        let next = list.firstIndex(of: u).flatMap { i in i + 1 < list.count ? list[i + 1] : (i > 0 ? list[i - 1] : nil) }
        NSWorkspace.shared.recycle([u]) { _, err in
            if let err { return Toast.shared.show("Couldn't move to Trash", err.localizedDescription) }
            self.selection = next
            self.reload()
            Toast.shared.show("Moved to Trash", u.lastPathComponent)
        }
    }

    func palette() {
        guard selection != nil else { return }
        let acts: [(String, String, String, String, () -> Void)] = [
            (isImage ? "Annotate" : "Open", "edit", isImage ? "pencil.and.outline" : "play.rectangle", "↩", { self.open() }),
            ("Copy", "clipboard", "doc.on.doc", "⌘C", { self.copy() }),
            ("Pin to screen", "float", "pin", "⌘P", { self.pin() }),
            ("Rename…", "name", "character.cursor.ibeam", "⌘R", { self.rename() }),
            ("Upload and copy link", "share imgur s3", "icloud.and.arrow.up", "⌘U", { self.upload() }),
            ("Copy text (OCR)", "ocr", "text.viewfinder", "⌘T", { self.copyOCRText() }),
            ("Show in Finder", "reveal folder", "folder", "⌘O", { self.reveal() }),
            ("Move to Trash", "delete remove", "trash", "⌘⌫", { self.trash() }),
        ]
        Palette.shared.show(acts.enumerated().map { i, a in PaletteItem(id: "h\(i)", title: a.0, keywords: a.1, icon: a.2, hint: a.3, action: a.4) },
                            placeholder: "Actions for \(selection!.lastPathComponent)…", mruKey: "HistoryPaletteMRU")
    }
}

final class ThumbCache {
    static let shared = ThumbCache()
    private let cache = NSCache<NSURL, NSImage>()

    func get(_ u: URL, size: CGSize, done: @escaping (NSImage?) -> Void) {
        if let i = cache.object(forKey: u as NSURL) { return done(i) }
        let req = QLThumbnailGenerator.Request(fileAt: u, size: size, scale: NSScreen.main?.backingScaleFactor ?? 2, representationTypes: .thumbnail)
        QLThumbnailGenerator.shared.generateBestRepresentation(for: req) { rep, _ in
            let img = rep?.nsImage
            if let img { self.cache.setObject(img, forKey: u as NSURL) }
            DispatchQueue.main.async { done(img) }
        }
    }
}

private struct Thumb: View {
    let url: URL
    let selected: Bool
    @State private var image: NSImage?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            ZStack {
                Color(nsColor: Theme.raised)
                if let image { Image(nsImage: image).resizable().aspectRatio(contentMode: .fit) }
                if ["mp4", "mov", "gif"].contains(url.pathExtension.lowercased()) {
                    VStack {
                        Spacer()
                        HStack {
                            Text(url.pathExtension.uppercased()).font(.system(size: 9, weight: .bold))
                                .padding(.horizontal, 6).padding(.vertical, 2)
                                .background(Color(nsColor: Theme.accent)).foregroundColor(Color(nsColor: Theme.onAccent)).cornerRadius(4)
                            Spacer()
                        }
                    }.padding(6)
                }
            }
            .frame(height: 124)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            Text(url.lastPathComponent).font(.system(size: 11, weight: .medium)).foregroundColor(Color(nsColor: Theme.text)).lineLimit(1).truncationMode(.middle)
            Text(HistoryModel.dateFormat.string(from: HistoryModel.mtime(url))).font(.system(size: 10)).foregroundColor(Color(nsColor: Theme.muted))
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color(nsColor: selected ? Theme.selected : Theme.surface)))
        .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color(nsColor: selected ? Theme.accent : Theme.border), lineWidth: selected ? 1.5 : 1))
        .onAppear { ThumbCache.shared.get(url, size: CGSize(width: 220, height: 140)) { image = $0 } }
    }
}

struct HistoryView: View {
    @ObservedObject var model: HistoryModel
    @FocusState private var searchFocused: Bool
    private let cellW: CGFloat = 196, spacing: CGFloat = 12

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "magnifyingglass").foregroundColor(Color(nsColor: Theme.muted))
                TextField("Search names, dates, types and text inside screenshots", text: $model.query)
                    .textFieldStyle(.plain).font(.system(size: 15)).foregroundColor(Color(nsColor: Theme.text))
                    .focused($searchFocused)
                    .onChange(of: model.query) { model.selection = model.filtered.first }
                Text("\(model.filtered.count) of \(model.items.count)").font(.system(size: 11)).foregroundColor(Color(nsColor: Theme.muted))
            }
            .padding(.horizontal, 18).padding(.vertical, 14)
            .background(Color(nsColor: Theme.surface))
            Rectangle().fill(Color(nsColor: Theme.border)).frame(height: 1)
            GeometryReader { geo in
                ScrollViewReader { proxy in
                    ScrollView {
                        let list = model.filtered
                        LazyVGrid(columns: [GridItem(.adaptive(minimum: cellW, maximum: cellW + 60), spacing: spacing)], spacing: spacing) {
                            ForEach(list, id: \.self) { u in
                                Thumb(url: u, selected: model.selection == u)
                                    .id(u)
                                    .onTapGesture(count: 2) { model.open(u) }
                                    .simultaneousGesture(TapGesture().onEnded { model.selection = u })
                                    .contextMenu {
                                        Button(["mp4", "mov", "gif"].contains(u.pathExtension.lowercased()) ? "Open" : "Annotate") { model.open(u) }
                                        Button("Copy") { model.selection = u; model.copy() }
                                        Button("Pin to screen") { model.selection = u; model.pin() }
                                        Button("Rename…") { model.selection = u; model.rename() }
                                        Button("Upload and copy link") { model.selection = u; model.upload() }
                                        Button("Copy text (OCR)") { model.selection = u; model.copyOCRText() }
                                        Button("Show in Finder") { model.selection = u; model.reveal() }
                                        Divider()
                                        Button("Move to Trash") { model.selection = u; model.trash() }
                                    }
                            }
                        }
                        .padding(16)
                        if list.isEmpty {
                            Text(model.items.isEmpty ? "No captures yet. Press ⌃⌥4 to capture a region." : "Nothing matches “\(model.query)”.")
                                .foregroundColor(Color(nsColor: Theme.muted)).padding(40)
                        }
                    }
                    .onChange(of: model.selection) { if let s = model.selection { withAnimation(.easeOut(duration: 0.12)) { proxy.scrollTo(s) } } }
                    .onAppear { model.columns = max(1, Int((geo.size.width - 32 + spacing) / (cellW + spacing))) }
                    .onChange(of: geo.size.width) { model.columns = max(1, Int((geo.size.width - 32 + spacing) / (cellW + spacing))) }
                }
            }
            Rectangle().fill(Color(nsColor: Theme.border)).frame(height: 1)
            Text("↩ open  ·  ⌘C copy  ·  ⌘P pin  ·  ⌘R rename  ·  ⌘U upload  ·  ⌘T copy text  ·  ⌘O Finder  ·  ⌘⌫ Trash  ·  ⌘K all")
                .font(.system(size: 11)).foregroundColor(Color(nsColor: Theme.muted))
                .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 18).padding(.vertical, 7)
                .background(Color(nsColor: Theme.surface))
        }
        .background(Color(nsColor: Theme.bg))
        .onAppear { searchFocused = true }
    }
}

final class HistoryWindow: NSObject, NSWindowDelegate {
    static var shared: HistoryWindow?
    let model = HistoryModel()
    let window: NSWindow
    private var monitor: Any?

    static func show() {
        if let s = shared {
            s.model.reload()
            activateApp()
            s.window.makeKeyAndOrderFront(nil)
            return
        }
        shared = HistoryWindow()
    }

    private override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = "Capture history"
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.minSize = NSSize(width: 520, height: 360)
        window.contentView = NSHostingView(rootView: HistoryView(model: model))
        window.setFrameAutosaveName("AtherHistory")
        if window.frame.origin == .zero { window.center() }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self, e.window === self.window, !Palette.shared.isVisible else { return e }
            return self.key(e) ? nil : e
        }
        model.reload()
        AppDelegate.shared?.windowOpened(window)
        activateApp()
        window.makeKeyAndOrderFront(nil)
    }

    private func key(_ e: NSEvent) -> Bool {
        let cmd = e.modifierFlags.contains(.command)
        let m = model
        let textSelected = (window.firstResponder as? NSTextView).map { $0.selectedRange().length > 0 } ?? false
        switch Int(e.keyCode) {
        case kVK_LeftArrow where !cmd && m.query.isEmpty: m.move(-1)
        case kVK_RightArrow where !cmd && m.query.isEmpty: m.move(1)
        case kVK_UpArrow: m.move(-m.columns)
        case kVK_DownArrow: m.move(m.columns)
        case kVK_Return, kVK_ANSI_KeypadEnter: m.open()
        case kVK_Escape:
            if m.query.isEmpty { window.close() } else { m.query = "" }
        case kVK_ANSI_C where cmd && !textSelected: m.copy()
        case kVK_ANSI_P where cmd: m.pin()
        case kVK_ANSI_R where cmd: m.rename()
        case kVK_ANSI_U where cmd: m.upload()
        case kVK_ANSI_T where cmd: m.copyOCRText()
        case kVK_ANSI_O where cmd: m.reveal()
        case kVK_ANSI_K where cmd: m.palette()
        case kVK_Delete where cmd: m.trash()
        case kVK_ANSI_W where cmd: window.close()
        default: return false
        }
        return true
    }

    func windowWillClose(_ notification: Notification) {
        if let monitor { NSEvent.removeMonitor(monitor) }
        HistoryWindow.shared = nil
    }
}
