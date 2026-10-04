import AVKit
import AppKit
import Carbon.HIToolbox
import Combine
import QuickLookThumbnailing
import SwiftUI
import UniformTypeIdentifiers

// Capture gallery: sidebar (library, collections, smart folders, tags, apps) · filter bar + justified/grid/list
// browser · inspector (tags, rating, comment, palette, OCR text) · full-window preview with zoom and slideshow.

enum Scope: Hashable {
    case all, uncategorized, recent, rated, duplicates
    case type(MediaType)
    case collection(UUID)
    case smart(UUID)
    case similar(URL)
}

enum GalleryLayout: String, CaseIterable { case justified, grid, list }

private func C(_ c: NSColor) -> Color { Color(nsColor: c) }

final class GalleryModel: ObservableObject {
    let lib: Library
    @Published var scope: Scope = .all { didSet { if oldValue != scope { scopeChanged() } } }
    @Published var filter = Filter() { didSet { if oldValue != filter { recompute() } } }
    @Published var sort: GallerySort = .newest { didSet { recompute() } }
    @Published var layout: GalleryLayout = GalleryLayout(rawValue: UserDefaults.standard.string(forKey: "GalleryLayout") ?? "") ?? .justified {
        didSet { UserDefaults.standard.set(layout.rawValue, forKey: "GalleryLayout") }
    }
    @Published var thumbSize: Double = UserDefaults.standard.object(forKey: "GalleryThumb") as? Double ?? 190 {
        didSet { UserDefaults.standard.set(thumbSize, forKey: "GalleryThumb") }
    }
    // Canvas-first: sidebar and inspector start hidden; ⌃⌘S and ⌘I bring them in.
    @Published var showSidebar = UserDefaults.standard.bool(forKey: "GallerySidebar") {
        didSet { UserDefaults.standard.set(showSidebar, forKey: "GallerySidebar") }
    }
    @Published var showInspector = UserDefaults.standard.bool(forKey: "GalleryInspectorShown") {
        didSet {
            UserDefaults.standard.set(showInspector, forKey: "GalleryInspectorShown")
            if showInspector { inspectorDiscovered = true }
        }
    }
    // The action bar spells out "⌘I" until the inspector has been opened once.
    @Published private(set) var inspectorDiscovered = UserDefaults.standard.bool(forKey: "GalleryInspectorDiscovered") {
        didSet { UserDefaults.standard.set(inspectorDiscovered, forKey: "GalleryInspectorDiscovered") }
    }
    @Published var showShortcuts = false
    @Published var stackEdits = UserDefaults.standard.object(forKey: "GalleryStacks") as? Bool ?? true {
        didSet { UserDefaults.standard.set(stackEdits, forKey: "GalleryStacks"); recompute() }
    }
    @Published var expanded: Set<String> = [] { didSet { recompute() } }   // stack roots shown open
    @Published private(set) var stacks: [URL: Int] = [:]       // shown capture → versions in its stack
    @Published private(set) var relatedCount = 0               // search results that matched only related words
    @Published var compare: (URL, URL)?
    private(set) var versionsByRoot: [String: [URL]] = [:]     // every stack with 2+ versions, newest first
    private(set) var rootOf: [String: String] = [:]
    @Published var showNames = UserDefaults.standard.bool(forKey: "GalleryNames") { didSet { UserDefaults.standard.set(showNames, forKey: "GalleryNames") } }
    @Published private(set) var visible: [URL] = []
    @Published private(set) var groups: [[URL]] = []           // duplicates scope
    @Published private(set) var distances: [URL: Float] = [:]  // similar scope
    @Published var selection: Set<URL> = []
    @Published var focus: URL?
    @Published var preview: URL?
    @Published var slideshow = false
    @Published var focusSearch = 0
    var anchor: URL?
    var rows: [JustifiedRow] = []
    var columns = 4
    var dragging: [URL] = []
    private var seed = UInt64.random(in: 1...UInt64.max)
    private var bag: Set<AnyCancellable> = []

    init(library: Library = .shared) {
        lib = library
        lib.objectWillChange.debounce(for: .milliseconds(120), scheduler: RunLoop.main).sink { [weak self] in self?.recompute() }.store(in: &bag)
        recompute()
    }

    var selected: [URL] { visible.filter(selection.contains) }
    var single: URL? { selection.count == 1 ? selection.first : nil }

    var title: String {
        switch scope {
        case .all: return "All captures"
        case .uncategorized: return "Uncategorized"
        case .recent: return "Last 7 days"
        case .rated: return "Rated"
        case .duplicates: return "Duplicates"
        case .type(let t): return t.label
        case .collection(let id): return lib.collections.first { $0.id == id }?.name ?? "Collection"
        case .smart(let id): return lib.smartFolders.first { $0.id == id }?.name ?? "Smart folder"
        case .similar(let u): return "Similar to \(u.lastPathComponent)"
        }
    }

    private func scopeChanged() {
        if case .smart(let id) = scope, let s = lib.smartFolders.first(where: { $0.id == id }) { filter = s.filter }
        selection = []
        focus = nil
        recompute()
    }

    func shuffle() {
        seed = UInt64.random(in: 1...UInt64.max)
        recompute()
    }

    func inScope(_ u: URL, _ m: ItemMeta) -> Bool {
        switch scope {
        case .all, .smart, .similar, .duplicates: return true
        case .uncategorized: return m.tags.isEmpty && m.collections.isEmpty
        case .recent: return m.mtime >= Date().timeIntervalSince1970 - 7 * 86400
        case .rated: return m.rating > 0
        case .type(let t): return MediaType.of(u) == t
        case .collection(let id): return m.collections.contains(id)
        }
    }

    func recompute() {
        let lib = self.lib
        if case .duplicates = scope {
            distances = [:]
            groups = lib.duplicateGroups().map { $0.filter { filter.matches($0, lib.meta($0)) } }.filter { $0.count > 1 }
            visible = groups.flatMap { $0 }
        } else if case .similar(let u) = scope {
            let res = lib.similar(to: u).filter { filter.matches($0.0, lib.meta($0.0)) }
            distances = Dictionary(res.map { ($0.0, $0.1) }, uniquingKeysWith: { a, _ in a })
            visible = [u] + res.map(\.0)
            groups = []
        } else {
            groups = []
            distances = [:]
            var kinds: [URL: SearchQuery.Match] = [:]
            var list = lib.urls.filter { u in
                let m = lib.meta(u)
                guard inScope(u, m), let k = filter.match(u, m) else { return false }
                kinds[u] = k
                return true
            }
            switch sort {
            case .newest: list.sort { lib.meta($0).mtime > lib.meta($1).mtime }
            case .oldest: list.sort { lib.meta($0).mtime < lib.meta($1).mtime }
            case .name: list.sort { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
            case .size: list.sort { lib.meta($0).size > lib.meta($1).size }
            case .dimensions: list.sort { lib.meta($0).w * lib.meta($0).h > lib.meta($1).w * lib.meta($1).h }
            case .rating: list.sort { (lib.meta($0).rating, lib.meta($0).mtime) > (lib.meta($1).rating, lib.meta($1).mtime) }
            case .random:
                var g = SeededRandom(seed: seed)
                list.shuffle(using: &g)
            }
            // Exact matches first; captures that only matched related words follow.
            let related = list.filter { kinds[$0] == .related }
            relatedCount = related.count
            if !related.isEmpty { list = list.filter { kinds[$0] != .related } + related }
            visible = collapseStacks(list)
        }
        selection = selection.filter(Set(visible).contains)
        if let f = focus, !visible.contains(f) { focus = nil }
    }

    // MARK: version stacks

    // Groups each original with its edits (following "edited from" links) and, while browsing,
    // shows only the newest version of each until the stack is expanded.
    private func collapseStacks(_ list: [URL]) -> [URL] {
        var roots: [String: String] = [:]
        func root(_ u: URL) -> String {
            if let r = roots[u.path] { return r }
            var cur = u.path
            var seen: Set<String> = [cur]
            while let p = lib.meta[cur]?.editedFrom, lib.meta[p] != nil, seen.insert(p).inserted { cur = p }
            roots[u.path] = cur
            return cur
        }
        var groups: [String: [URL]] = [:]
        for u in lib.urls { groups[root(u), default: []].append(u) }
        versionsByRoot = groups.filter { $0.value.count > 1 }.mapValues { $0.sorted { lib.meta($0).mtime > lib.meta($1).mtime } }
        rootOf = [:]
        for (r, vs) in versionsByRoot { for v in vs { rootOf[v.path] = r } }
        stacks = [:]
        guard stackEdits, filter.text.isEmpty else { return list }
        var out: [URL] = []
        var done = Set<String>()
        for u in list {
            guard let r = rootOf[u.path], let vs = versionsByRoot[r] else { out.append(u); continue }
            guard done.insert(r).inserted else { continue }
            let shown = vs.filter(Set(list).contains)
            if expanded.contains(r) {
                out += shown
                for v in shown { stacks[v] = vs.count }
            } else if let cover = shown.first {
                out.append(cover)
                stacks[cover] = vs.count
            }
        }
        return out
    }

    func versions(of u: URL) -> [URL] { rootOf[u.path].flatMap { versionsByRoot[$0] } ?? [] }
    func isExpanded(_ u: URL) -> Bool { rootOf[u.path].map(expanded.contains) ?? false }
    func toggleStack(_ u: URL) {
        guard let r = rootOf[u.path] else { return }
        if expanded.contains(r) { expanded.remove(r) } else { expanded.insert(r) }
    }

    func compareSelected() {
        let ts = targets.filter(isImage)
        if ts.count == 2 { compare = (ts[1], ts[0]); return }
        if ts.count == 1, let o = lib.meta(ts[0]).editedFrom, lib.meta[o] != nil { compare = (URL(fileURLWithPath: o), ts[0]); return }
        if ts.count == 1, let older = versions(of: ts[0]).first(where: { $0 != ts[0] }) { compare = (older, ts[0]); return }
        Toast.shared.show("Select two screenshots to compare", "Or one edited capture to compare with its original")
    }

    // MARK: selection

    func click(_ u: URL) {
        GalleryWindow.shared?.endTyping()   // clicking a capture takes focus off the search field, so shortcuts apply
        let f = NSEvent.modifierFlags
        if f.contains(.shift), let a = anchor, let i = visible.firstIndex(of: a), let j = visible.firstIndex(of: u) {
            let range = visible[min(i, j)...max(i, j)]
            selection = f.contains(.command) ? selection.union(range) : Set(range)
        } else if f.contains(.command) {
            if selection.contains(u) { selection.remove(u) } else { selection.insert(u) }
            anchor = u
        } else {
            selection = [u]
            anchor = u
        }
        focus = u
    }

    func move(_ d: Int, extend: Bool = false) {
        guard !visible.isEmpty else { return }
        let cur = focus ?? selection.first
        let i = cur.flatMap { visible.firstIndex(of: $0) } ?? (d > 0 ? -1 : visible.count)
        let n = visible[min(max(0, i + d), visible.count - 1)]
        go(n, extend: extend)
    }

    // Up/down by visual row in the justified layout, by columns elsewhere.
    func moveRow(_ dir: Int, extend: Bool = false) {
        guard layout == .justified, let cur = focus ?? selection.first,
              let ri = rows.firstIndex(where: { $0.items.contains(cur) }) else { return move(dir * (layout == .list ? 1 : columns), extend: extend) }
        let ni = ri + dir
        guard rows.indices.contains(ni) else { return }
        let row = rows[ri]
        let x = row.items.prefix { $0 != cur }.reduce(0) { $0 + lib.meta($1).aspect * row.height } + lib.meta(cur).aspect * row.height / 2
        var acc: CGFloat = 0
        let target = rows[ni].items.first { u in
            acc += lib.meta(u).aspect * rows[ni].height
            return acc >= x
        } ?? rows[ni].items.last!
        go(target, extend: extend)
    }

    private func go(_ u: URL, extend: Bool) {
        if extend, let a = anchor, let i = visible.firstIndex(of: a), let j = visible.firstIndex(of: u) {
            selection = Set(visible[min(i, j)...max(i, j)])
        } else {
            selection = [u]
            anchor = u
        }
        focus = u
        if preview != nil { preview = u }
    }

    func selectAll() { selection = Set(visible) }
    func zoom(_ factor: Double) { thumbSize = min(360, max(110, thumbSize * factor)) }

    func step(_ d: Int) {
        guard let p = preview, let i = visible.firstIndex(of: p), !visible.isEmpty else { return }
        let n = visible[(i + d + visible.count) % visible.count]
        preview = n
        selection = [n]
        focus = n
    }

    // MARK: actions

    var targets: [URL] { selected.isEmpty ? (focus.map { [$0] } ?? []) : selected }

    func isImage(_ u: URL) -> Bool { MediaType.of(u) == .image }

    func open(_ u: URL? = nil) {
        guard let u = u ?? focus ?? selected.first else { return }
        if isImage(u) { Editor.open(url: u) } else { Output.open(u) }
    }

    func copy() {
        let ts = targets
        guard !ts.isEmpty else { return }
        if ts.count == 1, isImage(ts[0]), let img = CGImage.load(ts[0]) { copyImage(img) } else {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.writeObjects(ts.map { $0 as NSURL })
        }
        Toast.shared.show("Copied", ts.count == 1 ? ts[0].lastPathComponent : "\(ts.count) files")
    }

    func pin() { for u in targets where isImage(u) { if let img = CGImage.load(u) { Pin.show(img) } } }
    func reveal() { if !targets.isEmpty { NSWorkspace.shared.activateFileViewerSelecting(targets) } }

    func collage() {
        let imgs = targets.filter(isImage)
        guard imgs.count >= 2 else { return Toast.shared.show("Select two or more screenshots for a collage") }
        Editor.openCollage(imgs)
    }

    var editorOpen: Bool { !Editor.instances.isEmpty }

    func addToEditor() {
        guard let e = Editor.instances.last else { return Toast.shared.show("No editor is open", "Open a screenshot to annotate first") }
        e.insertImages(targets.filter(isImage))
    }

    // Selects a capture in the gallery, widening the view if it's filtered out.
    func reveal(inGallery u: URL) {
        guard lib.urls.contains(u) else { return Toast.shared.show("The original is no longer in the gallery") }
        if !visible.contains(u), let r = rootOf[u.path] { expanded.insert(r) }   // hidden inside a collapsed stack
        if !visible.contains(u) { scope = .all; filter = Filter() }
        selection = [u]
        focus = u
        anchor = u
    }
    func upload() { for u in targets { AppDelegate.shared?.upload(u) } }

    func copyText() {
        let ts = targets.filter(isImage)
        let text = ts.compactMap { lib.meta($0).text }.filter { !$0.isEmpty }.joined(separator: "\n\n")
        if !text.isEmpty { copiedText(text); return }
        guard let u = ts.first, let img = CGImage.load(u) else { return }
        OCR.async({ try OCR.text(img) }) { r in
            if case .success(let t) = r, !t.isEmpty { self.copiedText(t) } else { Toast.shared.show("No text found") }
        }
    }

    private func copiedText(_ t: String) {
        AtherScreenshot.copyText(t)
        Toast.shared.show("Text copied", String(t.prefix(280)))
    }

    func rate(_ r: Int) {
        let ts = targets
        guard !ts.isEmpty else { return }
        lib.setRating(r, ts)
        Toast.shared.show(r == 0 ? "Rating cleared" : String(repeating: "★", count: r), ts.count == 1 ? ts[0].lastPathComponent : "\(ts.count) captures")
    }

    func rename() {
        let ts = targets
        guard let first = ts.first else { return }
        if ts.count == 1 {
            Palette.shared.prompt("Rename  ·  ↩ renames, ⎋ keeps the current name", initial: first.deletingPathExtension().lastPathComponent) { name in
                guard let n = Output.rename(first, to: name) else { return Toast.shared.show("Rename failed", first.lastPathComponent) }
                self.lib.moved(from: first, to: n)
                self.selection = [n]
                self.focus = n
                if self.preview == first { self.preview = n }
                self.lib.refresh()
            }
        } else {
            batchRename(ts)
        }
    }

    // Template tokens: {n} running number, {name} current name, {date} capture date, {app} source app.
    func batchRename(_ ts: [URL]) {
        Palette.shared.prompt("Rename \(ts.count) files  ·  {n} number · {name} current name · {date} · {app}", initial: "{name}") { tmpl in
            guard !tmpl.isEmpty else { return }
            let df = DateFormatter()
            df.locale = Locale(identifier: "en_US_POSIX")
            df.calendar = Calendar(identifier: .gregorian)
            df.dateFormat = "yyyy-MM-dd"
            var renamed: [URL] = []
            for (i, u) in ts.enumerated() {
                let m = self.lib.meta(u)
                let digits = String(ts.count).count
                var name = tmpl.replacingOccurrences(of: "{n}", with: String(format: "%0\(digits)d", i + 1))
                name = name.replacingOccurrences(of: "{name}", with: u.deletingPathExtension().lastPathComponent)
                name = name.replacingOccurrences(of: "{date}", with: df.string(from: Date(timeIntervalSince1970: m.mtime)))
                name = name.replacingOccurrences(of: "{app}", with: m.app.isEmpty ? "screen" : m.app)
                if let n = Output.rename(u, to: name) {
                    self.lib.moved(from: u, to: n)
                    if self.preview == u { self.preview = n }
                    renamed.append(n)
                }
            }
            self.selection = Set(renamed)
            self.lib.refresh()
            Toast.shared.show("Renamed \(renamed.count) of \(ts.count)")
        }
    }

    func trash(_ list: [URL]? = nil) {
        let ts = list ?? targets
        guard !ts.isEmpty else { return }
        let next = ts.last.flatMap { visible.firstIndex(of: $0) }.flatMap { i -> URL? in
            visible[(i + 1)...].first { !ts.contains($0) } ?? visible[..<i].last { !ts.contains($0) }
        }
        NSWorkspace.shared.recycle(ts) { done, err in DispatchQueue.main.async {
            let gone = Array(done.keys)
            self.lib.removed(gone)
            if let n = next { self.selection = [n]; self.focus = n }
            if let p = self.preview, gone.contains(p) { self.preview = next }
            if let err, gone.isEmpty { return Toast.shared.show("Couldn't move to Trash", err.localizedDescription) }
            Toast.shared.show("Moved to Trash", gone.count == 1 ? gone[0].lastPathComponent : "\(gone.count) files")
        } }
    }

    // Files dropped on the gallery or a collection: our own tiles are filed, outside files are imported.
    func handleDrop(_ providers: [NSItemProvider], into col: UUID?) {
        let dragged = dragging
        dragging = []
        loadURLs(providers) { urls in
            // The drag pasteboard only carries the tile that was grabbed; a multi-selection drag files them all.
            let ours = !dragged.isEmpty && urls.contains(where: dragged.contains) ? dragged : urls.filter(self.lib.isInLibrary)
            let outside = urls.filter { !self.lib.isInLibrary($0) }
            if let col, !ours.isEmpty {
                self.lib.add(ours, toCollection: col)
                let name = self.lib.collections.first { $0.id == col }?.name ?? "collection"
                Toast.shared.show("Added to \(name)", "\(ours.count) capture\(ours.count == 1 ? "" : "s")")
            }
            if !outside.isEmpty { self.importFiles(outside, into: col) }
        }
    }

    func findSimilar(_ u: URL? = nil) {
        guard let u = u ?? focus ?? selected.first else { return }
        guard lib.hasFeatures else { return Toast.shared.show("Still indexing", "Try again once the gallery finishes indexing.") }
        filter = Filter()
        scope = .similar(u)
        selection = [u]
        focus = u
    }

    // Eagle's "T": add tags to the selection, picking existing ones or typing new ones.
    func tagPicker() {
        let ts = targets
        guard !ts.isEmpty else { return }
        let have = Set(ts.flatMap { lib.meta($0).tags.map { $0.lowercased() } })
        let items = lib.allTags.map { t, n in
            PaletteItem(id: "tag." + t, title: t, keywords: "tag", icon: have.contains(t.lowercased()) ? "checkmark.circle.fill" : "tag",
                        hint: "\(n)") { [weak self] in
                guard let self else { return }
                if have.contains(t.lowercased()) && ts.allSatisfy({ self.lib.meta($0).tags.contains { $0.lowercased() == t.lowercased() } }) {
                    self.lib.removeTag(t, from: ts)
                } else { self.lib.addTags([t], to: ts) }
            }
        }
        Palette.shared.show(items, placeholder: "Add a tag to \(ts.count == 1 ? ts[0].lastPathComponent : "\(ts.count) captures") (comma-separated to add several)…",
                            mruKey: "TagPaletteMRU", create: ("Add tag", { [weak self] text in
                                self?.lib.addTags(text.split(separator: ",").map(String.init), to: ts)
                            }))
    }

    // Eagle's "F": file the selection into a collection (or a new one).
    func collectionPicker() {
        let ts = targets
        guard !ts.isEmpty else { return }
        let items = lib.collections.map { c in
            PaletteItem(id: "col." + c.id.uuidString, title: c.name, keywords: "collection folder " + c.autoTags.joined(separator: " "),
                        icon: "folder", hint: "\(lib.count(in: c.id))") { [weak self] in
                self?.lib.add(ts, toCollection: c.id)
                Toast.shared.show("Added to \(c.name)", ts.count == 1 ? ts[0].lastPathComponent : "\(ts.count) captures")
            }
        }
        Palette.shared.show(items, placeholder: "Add to collection…", mruKey: "CollectionPaletteMRU", create: ("New collection", { [weak self] name in
            guard let self, !name.isEmpty else { return }
            let c = self.lib.createCollection(name)
            self.lib.add(ts, toCollection: c.id)
            Toast.shared.show("Added to \(c.name)", "\(ts.count) capture\(ts.count == 1 ? "" : "s")")
        }))
    }

    func saveSmartFolder() {
        guard !filter.isEmpty else { return Toast.shared.show("Set a filter first", "A smart folder saves the current search and filters.") }
        if case .smart(let id) = scope {
            lib.updateSmartFolder(id, filter: filter)
            return Toast.shared.show("Smart folder updated")
        }
        Palette.shared.prompt("Name this smart folder", initial: filter.text.isEmpty ? "Smart folder" : filter.text) { name in
            guard !name.isEmpty else { return }
            let s = self.lib.saveSmartFolder(name, self.filter)
            self.scope = .smart(s.id)
        }
    }

    func palette() {
        let n = targets.count
        var acts: [(String, String, String, String, () -> Void)] = [
            ("Find similar captures", "reverse image search look alike", "sparkle.magnifyingglass", "", { self.findSimilar() }),
            ("Add tags…", "tag label", "tag", "T", { self.tagPicker() }),
            ("Add to collection…", "folder file category", "folder.badge.plus", "F", { self.collectionPicker() }),
            ("Preview", "quick look view full screen", "eye", "Space", { self.preview = self.focus ?? self.selected.first }),
            ("Annotate / open", "edit", "pencil.and.outline", "↩", { self.open() }),
            ("Copy", "clipboard", "doc.on.doc", "⌘C", { self.copy() }),
            ("Copy text (OCR)", "ocr", "text.viewfinder", "⌘T", { self.copyText() }),
            ("Pin to screen", "float", "pin", "⌘P", { self.pin() }),
            (n > 1 ? "Batch rename…" : "Rename…", "name", "character.cursor.ibeam", "⌘R", { self.rename() }),
            ("Upload and copy link", "share imgur s3", "icloud.and.arrow.up", "⌘U", { self.upload() }),
            ("Show in Finder", "reveal folder", "folder", "⌘O", { self.reveal() }),
            ("Move to Trash", "delete remove", "trash", "⌘⌫", { self.trash() }),
        ]
        for r in 0...5 { acts.append((r == 0 ? "Clear rating" : "Rate " + String(repeating: "★", count: r), "stars rating", "star", "\(r)", { self.rate(r) })) }
        acts += [
            ("Save filters as smart folder", "saved search", "folder.badge.gearshape", "", { self.saveSmartFolder() }),
            ("Show duplicates", "identical similar", "square.on.square", "", { self.scope = .duplicates }),
            ("Shuffle (random order)", "random", "shuffle", "", { self.sort = .random; self.shuffle() }),
            ("Import files…", "add", "square.and.arrow.down", "", { self.importPanel() }),
        ]
        Palette.shared.show(acts.enumerated().map { i, a in PaletteItem(id: "g\(i)", title: a.0, keywords: a.1, icon: a.2, hint: a.3, action: a.4) },
                            placeholder: n > 0 ? "Actions for \(n == 1 ? targets[0].lastPathComponent : "\(n) captures")…" : "Gallery actions…",
                            mruKey: "GalleryPaletteMRU")
    }

    func importPanel() {
        let p = NSOpenPanel()
        p.allowsMultipleSelection = true
        p.allowedContentTypes = [.image, .movie, .gif]
        if p.runModal() == .OK { importFiles(p.urls) }
    }

    func importFiles(_ files: [URL], into col: UUID? = nil) {
        lib.importFiles(files, into: col) { out in
            Toast.shared.show(out.isEmpty ? "Nothing to import" : "Imported \(out.count) file\(out.count == 1 ? "" : "s")")
            self.selection = Set(out)
        }
    }
}

// Vision feature-print distance → a rough similarity percentage (identical 0 → 100%, unrelated screens ≈ 1.0 → ~20%).
func similarity(_ d: Float) -> Double { max(0, min(100, 100 * (1 - Double(d) / 1.25))) }

struct SeededRandom: RandomNumberGenerator {
    var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func next() -> UInt64 {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        var z = state
        z = (z ^ (z >> 33)) &* 0xff51afd7ed558ccd
        return z ^ (z >> 33)
    }
}

// MARK: - Thumbnails

final class ThumbCache {
    static let shared = ThumbCache()
    private let cache = NSCache<NSString, NSImage>()

    func get(_ u: URL, side: CGFloat, done: @escaping (NSImage?) -> Void) {
        let bucket = side <= 200 ? 256 : side <= 400 ? 512 : 1024
        let key = "\(u.path)#\(bucket)" as NSString
        if let i = cache.object(forKey: key) { return done(i) }
        let req = QLThumbnailGenerator.Request(fileAt: u, size: CGSize(width: bucket, height: bucket), scale: 1, representationTypes: .thumbnail)
        QLThumbnailGenerator.shared.generateBestRepresentation(for: req) { rep, _ in
            let img = rep?.nsImage
            if let img { self.cache.setObject(img, forKey: key) }
            DispatchQueue.main.async { done(img) }
        }
    }
}

private struct ThumbImage: View {
    let url: URL
    let side: CGFloat
    var fill = true
    @State private var image: NSImage?

    var body: some View {
        ZStack {
            C(Theme.raised)
            if let image {
                Image(nsImage: image).resizable().interpolation(.high).aspectRatio(contentMode: fill ? .fill : .fit)
            }
        }
        .clipped()
        .onAppear { load() }
        .onChange(of: url) { load() }
    }

    private func load() { ThumbCache.shared.get(url, side: side) { image = $0 } }
}

private struct Tile: View {
    @ObservedObject var model: GalleryModel
    let url: URL
    let width: CGFloat
    let height: CGFloat
    var fill = true
    var caption = false
    @State private var hover = false

    var body: some View {
        let m = model.lib.meta(url)
        let sel = model.selection.contains(url)
        VStack(alignment: .leading, spacing: 4) {
            ZStack(alignment: .bottomLeading) {
                ThumbImage(url: url, side: max(width, height), fill: fill)
                    .frame(width: width, height: height)
                    .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                HStack(spacing: 4) {
                    if MediaType.of(url) != .image { badge(MediaType.of(url) == .gif ? "GIF" : (m.duration.map(formatTime) ?? "MP4")) }
                    if m.rating > 0 { badge(String(repeating: "★", count: m.rating)) }
                    if let d = model.distances[url] { badge(String(format: "%.0f%%", similarity(d))) }
                    if hover && !caption {
                        Text(url.deletingPathExtension().lastPathComponent).font(.system(size: 10, weight: .medium)).lineLimit(1).truncationMode(.middle)
                            .padding(.horizontal, 6).padding(.vertical, 2)
                            .background(.ultraThinMaterial, in: Capsule()).foregroundColor(.white)
                    }
                }
                .padding(6)
                .frame(maxWidth: width, alignment: .leading)
            }
            .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous).stroke(sel ? C(Theme.accent) : (model.focus == url ? Color.white.opacity(0.35) : Color.clear), lineWidth: sel ? 2.5 : 1)
                .padding(-2))
            .overlay(alignment: .topTrailing) {
                if let n = model.stacks[url] {
                    Button { model.toggleStack(url) } label: {
                        HStack(spacing: 3) {
                            Image(systemName: model.isExpanded(url) ? "rectangle.compress.vertical" : "square.stack")
                            Text("\(n)")
                        }
                        .font(.system(size: 10, weight: .bold)).padding(.horizontal, 7).padding(.vertical, 3)
                        .background(.ultraThinMaterial, in: Capsule()).foregroundColor(.white)
                    }
                    .buttonStyle(.plain).padding(6)
                    .help(model.isExpanded(url) ? "Collapse versions" : "\(n) versions: the original and its edits. Click to show all.")
                }
            }
            .onHover { hover = $0 }
            if caption {
                Text(url.lastPathComponent).font(.system(size: 11)).foregroundColor(C(sel ? Theme.text : Theme.textDim)).lineLimit(1).truncationMode(.middle)
                    .frame(width: width, alignment: .leading)
            }
        }
        .contentShape(Rectangle())
        .help(url.lastPathComponent + (m.tags.isEmpty ? "" : "\n" + m.tags.joined(separator: ", ")))
        .onTapGesture(count: 2) { model.open(url) }
        .simultaneousGesture(TapGesture().onEnded { model.click(url) })
        .onDrag {
            model.dragging = model.selection.contains(url) ? model.selected : [url]
            return NSItemProvider(object: url as NSURL)
        }
        .contextMenu { ItemMenu(model: model, url: url) }
        .id(url)
    }

    private func badge(_ s: String) -> some View {
        Text(s).font(.system(size: 9, weight: .bold)).padding(.horizontal, 6).padding(.vertical, 2)
            .background(.ultraThinMaterial, in: Capsule()).foregroundColor(.white)
    }
}

private struct ItemMenu: View {
    @ObservedObject var model: GalleryModel
    let url: URL
    var body: some View {
        let ensure = { if !model.selection.contains(url) { model.selection = [url]; model.focus = url } }
        Button(model.isImage(url) ? "Annotate" : "Open") { model.open(url) }
        Button("Preview") { ensure(); model.preview = url }
        Button("Find similar") { model.findSimilar(url) }
        if model.selection.count > 1 && model.selection.contains(url) { Button("Make collage") { model.collage() } }
        if model.editorOpen { Button("Add to open editor") { ensure(); model.addToEditor() } }
        if model.selection.count == 2 && model.selection.contains(url) { Button("Compare") { model.compareSelected() } }
        else if model.versions(of: url).count > 1 { Button("Compare with previous version") { model.selection = [url]; model.focus = url; model.compareSelected() } }
        if model.stacks[url] != nil { Button(model.isExpanded(url) ? "Collapse versions" : "Show all versions") { model.toggleStack(url) } }
        Divider()
        Button("Add tags…") { ensure(); model.tagPicker() }
        Button("Add to collection…") { ensure(); model.collectionPicker() }
        Menu("Rating") { ForEach(0...5, id: \.self) { r in Button(r == 0 ? "None" : String(repeating: "★", count: r)) { ensure(); model.rate(r) } } }
        if case .collection(let id) = model.scope {
            Button("Remove from this collection") { ensure(); model.lib.remove(model.targets, fromCollection: id) }
        }
        Divider()
        Button("Copy") { ensure(); model.copy() }
        Button("Copy text (OCR)") { ensure(); model.copyText() }
        Button("Pin to screen") { ensure(); model.pin() }
        Button(model.selection.count > 1 ? "Batch rename…" : "Rename…") { ensure(); model.rename() }
        Button("Upload and copy link") { ensure(); model.upload() }
        Button("Show in Finder") { ensure(); model.reveal() }
        Divider()
        Button("Move to Trash") { ensure(); model.trash() }
    }
}

// MARK: - Browser

private struct Browser: View {
    @ObservedObject var model: GalleryModel
    var top: CGFloat = 0       // height of the floating toolbar the content scrolls under
    private let spacing: CGFloat = 8
    private let bottom: CGFloat = 76
    @State private var pinchBase: Double?

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width - 32
            ScrollViewReader { proxy in
                Group {
                    if model.visible.isEmpty {
                        empty.frame(maxWidth: .infinity, maxHeight: .infinity)
                    } else if case .duplicates = model.scope {
                        duplicates(width)
                    } else if model.layout == .list {
                        list.padding(.top, top)
                    } else {
                        ScrollView {
                            Color.clear.frame(height: top)
                            if case .similar(let u) = model.scope { similarHeader(u) }
                            if model.layout == .justified { justified(width) } else { grid(width) }
                        }
                    }
                }
                .onChange(of: model.focus) { if let f = model.focus { withAnimation(.easeOut(duration: 0.1)) { proxy.scrollTo(f) } } }
            }
            .onAppear { model.columns = max(1, Int((width + spacing) / (model.thumbSize + spacing))) }
            .onChange(of: geo.size.width) { model.columns = max(1, Int((width + spacing) / (model.thumbSize + spacing))) }
        }
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            model.handleDrop(providers, into: nil)
            return true
        }
        .simultaneousGesture(MagnificationGesture()
            .onChanged { v in
                guard model.layout != .list else { return }
                let base = pinchBase ?? model.thumbSize
                pinchBase = base
                model.thumbSize = min(360, max(110, base * Double(v)))
            }
            .onEnded { _ in pinchBase = nil })
    }

    private func justified(_ width: CGFloat) -> some View {
        let rows = justifiedRows(model.visible, aspect: { model.lib.meta($0).aspect }, width: width, target: model.thumbSize, spacing: spacing)
        model.rows = rows
        return LazyVStack(alignment: .leading, spacing: spacing + (model.showNames ? 18 : 0)) {
            ForEach(rows) { row in
                HStack(spacing: spacing) {
                    ForEach(row.items, id: \.self) { u in
                        Tile(model: model, url: u, width: max(20, model.lib.meta(u).aspect.clamped(0.25, 4) * row.height), height: row.height, caption: model.showNames)
                    }
                }
            }
        }
        .padding(.horizontal, 16).padding(.top, 8).padding(.bottom, bottom)
    }

    private func grid(_ width: CGFloat) -> some View {
        let side = model.thumbSize
        return LazyVGrid(columns: [GridItem(.adaptive(minimum: side, maximum: side * 1.3), spacing: spacing)], spacing: spacing + 6) {
            ForEach(model.visible, id: \.self) { u in
                Tile(model: model, url: u, width: side, height: side * 0.72, fill: false, caption: true)
            }
        }
        .padding(.horizontal, 16).padding(.top, 8).padding(.bottom, bottom)
    }

    private struct Row: Identifiable {
        let url: URL
        let m: ItemMeta
        var id: URL { url }
        var name: String { url.lastPathComponent }
        var pixels: Int { m.w * m.h }
    }

    private var list: some View {
        let rows = model.visible.map { Row(url: $0, m: model.lib.meta($0)) }
        return Table(rows, selection: $model.selection) {
            TableColumn("") { r in ThumbImage(url: r.url, side: 64).frame(width: 44, height: 30).clipShape(RoundedRectangle(cornerRadius: 3)) }.width(52)
            TableColumn("Name") { r in Text(r.name).lineLimit(1).truncationMode(.middle) }
            TableColumn("Type") { r in Text(MediaType.of(r.url).label.dropLast()) }.width(80)
            TableColumn("Dimensions") { r in Text(r.m.w > 0 ? String(r.m.w) + " × " + String(r.m.h) : "—").monospacedDigit() }.width(100)
            TableColumn("Size") { r in Text(ByteCountFormatter.string(fromByteCount: Int64(r.m.size), countStyle: .file)).monospacedDigit() }.width(76)
            TableColumn("Date") { r in Text(Library.dateFormat.string(from: Date(timeIntervalSince1970: r.m.mtime))).monospacedDigit() }.width(120)
            TableColumn("Rating") { r in Text(String(repeating: "★", count: r.m.rating)).foregroundColor(C(Theme.accent)) }.width(70)
            TableColumn("Tags") { r in Text(r.m.tags.joined(separator: ", ")).foregroundColor(.secondary).lineLimit(1) }
        }
        .contextMenu(forSelectionType: URL.self) { sel in
            if let u = sel.first { ItemMenu(model: model, url: u) }
        } primaryAction: { sel in
            if let u = sel.first { model.open(u) }
        }
        .onChange(of: model.selection) { if model.selection.count == 1 { model.focus = model.selection.first; model.anchor = model.focus } }
    }

    private func similarHeader(_ u: URL) -> some View {
        HStack(spacing: 10) {
            Image(systemName: "sparkle.magnifyingglass").foregroundColor(C(Theme.accent))
            Text("Most similar first. Percentages are visual similarity.").foregroundColor(C(Theme.textDim))
            Spacer()
            Button("Back to all") { model.scope = .all }
        }
        .font(.system(size: 12)).padding(.horizontal, 16).padding(.top, 12)
    }

    private func duplicates(_ width: CGFloat) -> some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 18) {
                Text("\(model.groups.count) group\(model.groups.count == 1 ? "" : "s") of identical or near-identical captures. The newest copy is first.")
                    .font(.system(size: 12)).foregroundColor(C(Theme.textDim))
                ForEach(Array(model.groups.enumerated()), id: \.offset) { i, g in
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text("\(g.count) copies").font(.system(size: 12, weight: .semibold)).foregroundColor(C(Theme.text))
                            Text(ByteCountFormatter.string(fromByteCount: Int64(g.dropFirst().reduce(0) { $0 + model.lib.meta($1).size }), countStyle: .file) + " reclaimable")
                                .font(.system(size: 11)).foregroundColor(C(Theme.muted))
                            Spacer()
                            Button("Keep newest, trash \(g.count - 1)") { model.trash(Array(g.dropFirst())) }
                        }
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(spacing: 8) {
                                ForEach(g, id: \.self) { u in
                                    Tile(model: model, url: u, width: min(260, model.lib.meta(u).aspect.clamped(0.4, 3) * 140), height: 140, caption: true)
                                }
                            }
                        }
                    }
                    .padding(12)
                    .background(RoundedRectangle(cornerRadius: 10).fill(C(Theme.surface)))
                    .id("g\(i)")
                }
            }
            .padding(.horizontal, 16).padding(.top, top + 8).padding(.bottom, bottom)
        }
    }

    private var empty: some View {
        VStack(spacing: 10) {
            Image(systemName: model.lib.urls.isEmpty ? "camera.viewfinder" : "line.3.horizontal.decrease.circle")
                .font(.system(size: 36)).foregroundColor(C(Theme.muted))
            if model.lib.urls.isEmpty {
                Text("No captures yet").font(.system(size: 15, weight: .semibold)).foregroundColor(C(Theme.text))
                Text("Press \(Hotkey.display(Settings.shared.hotkey(.region))) to capture a region, or drop images here to import them.").foregroundColor(C(Theme.muted))
            } else if case .duplicates = model.scope {
                Text("No duplicates found").font(.system(size: 15, weight: .semibold)).foregroundColor(C(Theme.text))
            } else {
                Text("Nothing matches").font(.system(size: 15, weight: .semibold)).foregroundColor(C(Theme.text))
                if !model.filter.isEmpty { Button("Clear filters") { model.filter = Filter() } }
            }
        }
        .font(.system(size: 12))
    }
}

extension CGFloat {
    func clamped(_ a: CGFloat, _ b: CGFloat) -> CGFloat { Swift.min(b, Swift.max(a, self)) }
}

func loadURLs(_ providers: [NSItemProvider], _ done: @escaping ([URL]) -> Void) {
    var out: [URL] = []
    let g = DispatchGroup()
    for p in providers where p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
        g.enter()
        _ = p.loadObject(ofClass: URL.self) { u, _ in
            if let u { DispatchQueue.main.async { out.append(u) } }
            g.leave()
        }
    }
    g.notify(queue: .main) { done(out) }
}

// MARK: - Sidebar

private struct Sidebar: View {
    @ObservedObject var model: GalleryModel
    @ObservedObject var lib: Library
    @State private var showAllTags = false
    @AppStorage("GallerySidebarTagsCollapsed") private var tagsCollapsed = true

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 2) {
                header("Library")
                row(.all, "All captures", "square.grid.2x2", nil)
                row(.recent, "Last 7 days", "clock", nil)
                row(.rated, "Rated", "star", nil)
                row(.uncategorized, "Uncategorized", "tray", nil)
                row(.duplicates, "Duplicates", "square.on.square", nil)

                if !lib.collections.isEmpty { header("Collections", add: newCollection) }
                ForEach(lib.collections) { c in
                    row(.collection(c.id), c.name, c.autoTags.isEmpty ? "folder" : "folder.badge.gearshape", nil)
                        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
                            model.handleDrop(providers, into: c.id)
                            return true
                        }
                        .contextMenu {
                            Button("Rename…") { Palette.shared.prompt("Rename collection", initial: c.name) { if !$0.isEmpty { lib.renameCollection(c.id, $0) } } }
                            Button("Auto tags…") {
                                Palette.shared.prompt("Tags added to everything dropped into “\(c.name)” (comma-separated)", initial: c.autoTags.joined(separator: ", ")) {
                                    lib.setAutoTags(c.id, $0.split(separator: ",").map(String.init))
                                }
                            }
                            Divider()
                            Button("Delete collection (keeps the files)") {
                                if model.scope == .collection(c.id) { model.scope = .all }
                                lib.deleteCollection(c.id)
                            }
                        }
                }

                if !lib.smartFolders.isEmpty { header("Smart folders", add: { model.saveSmartFolder() }) }
                ForEach(lib.smartFolders) { s in
                    row(.smart(s.id), s.name, "folder.badge.gearshape", nil)
                        .contextMenu {
                            Button("Rename…") { Palette.shared.prompt("Rename smart folder", initial: s.name) { if !$0.isEmpty { lib.updateSmartFolder(s.id, name: $0) } } }
                            Button("Update with current filters") { lib.updateSmartFolder(s.id, filter: model.filter) }
                            Divider()
                            Button("Delete smart folder") {
                                if model.scope == .smart(s.id) { model.scope = .all }
                                lib.deleteSmartFolder(s.id)
                            }
                        }
                }

                let tags = lib.allTags
                if !tags.isEmpty {
                    header("Tags", collapsed: $tagsCollapsed)
                    if !tagsCollapsed { FlowLayout(spacing: 5) {
                        ForEach((showAllTags ? tags : Array(tags.prefix(24))), id: \.0) { t, n in
                            let on = model.filter.tags.contains { $0.lowercased() == t.lowercased() }
                            Button { toggleTag(t) } label: {
                                Text("\(t) \(Text("\(n)").foregroundColor(C(Theme.muted)))")
                                    .font(.system(size: 11)).padding(.horizontal, 8).padding(.vertical, 3)
                                    .background(Capsule().fill(Color.white.opacity(on ? 0.2 : 0.06)))
                                    .foregroundColor(on ? C(Theme.text) : C(Theme.textDim))
                            }
                            .buttonStyle(.plain)
                            .contextMenu {
                                Button("Rename tag…") { Palette.shared.prompt("Rename tag “\(t)” everywhere", initial: t) { if !$0.isEmpty { lib.renameTag(t, to: $0) } } }
                                Button("Delete tag from all captures") { lib.deleteTag(t) }
                            }
                        }
                    }
                    .padding(.horizontal, 10).padding(.vertical, 4)
                    if tags.count > 24 { Button(showAllTags ? "Fewer" : "All \(tags.count) tags") { showAllTags.toggle() }.buttonStyle(.link).font(.system(size: 11)).padding(.leading, 12) }
                    }
                }

            }
            .padding(.top, 44).padding(.bottom, 10)
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            HStack {
                Menu {
                    Button("New collection…", action: newCollection)
                    Button(model.filter.isEmpty ? "New smart folder (set filters first)" : "New smart folder from filters") { model.saveSmartFolder() }
                        .disabled(model.filter.isEmpty)
                } label: { Image(systemName: "plus") }
                    .menuStyle(.button).buttonStyle(GlassButtonStyle()).menuIndicator(.hidden).fixedSize().help("New collection or smart folder")
                Spacer()
            }
            .padding(8)
        }
        .background(.regularMaterial)
    }

    private func newCollection() {
        Palette.shared.prompt("New collection", initial: "") { name in
            if !name.isEmpty { model.scope = .collection(lib.createCollection(name).id) }
        }
    }

    private func toggleTag(_ t: String) {
        if let i = model.filter.tags.firstIndex(where: { $0.lowercased() == t.lowercased() }) { model.filter.tags.remove(at: i) } else { model.filter.tags.append(t) }
    }

    private func header(_ s: String, add: (() -> Void)? = nil, collapsed: Binding<Bool>? = nil) -> some View {
        HStack(spacing: 4) {
            Text(s).font(.system(size: 11, weight: .semibold)).foregroundColor(C(Theme.muted))
            if let collapsed {
                Image(systemName: collapsed.wrappedValue ? "chevron.right" : "chevron.down").font(.system(size: 8, weight: .bold)).foregroundColor(C(Theme.muted))
            }
            Spacer()
            if let add { Button(action: add) { Image(systemName: "plus") }.buttonStyle(.plain).foregroundColor(C(Theme.muted)).help("New") }
        }
        .contentShape(Rectangle())
        .onTapGesture { collapsed?.wrappedValue.toggle() }
        .padding(.horizontal, 16).padding(.top, 14).padding(.bottom, 4)
    }

    private func rowLabel(_ title: String, _ icon: String, _ count: Int?, _ on: Bool) -> some View {
        HStack(spacing: 8) {
            Image(systemName: icon).frame(width: 16).foregroundColor(on ? C(Theme.text) : C(Theme.muted))
            Text(title).lineLimit(1).foregroundColor(on ? C(Theme.text) : C(Theme.textDim))
            Spacer()
            if let count { Text("\(count)").font(.system(size: 11)).monospacedDigit().foregroundColor(C(Theme.muted)) }
        }
        .font(.system(size: 13))
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(on ? Color.white.opacity(0.10) : Color.clear))
        .contentShape(Rectangle())
        .padding(.horizontal, 6)
    }

    private func row(_ s: Scope, _ title: String, _ icon: String, _ count: Int?) -> some View {
        rowLabel(title, icon, count, model.scope == s).onTapGesture { model.scope = s }
    }
}

// Wrapping row layout for tag chips.
struct FlowLayout: Layout {
    var spacing: CGFloat = 6
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let w = proposal.width ?? 240
        var x: CGFloat = 0, y: CGFloat = 0, lineH: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x + sz.width > w && x > 0 { x = 0; y += lineH + spacing; lineH = 0 }
            x += sz.width + spacing
            lineH = max(lineH, sz.height)
        }
        return CGSize(width: w, height: y + lineH)
    }
    func placeSubviews(in b: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x = b.minX, y = b.minY, lineH: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x + sz.width > b.maxX && x > b.minX { x = b.minX; y += lineH + spacing; lineH = 0 }
            s.place(at: CGPoint(x: x, y: y), proposal: .unspecified)
            x += sz.width + spacing
            lineH = max(lineH, sz.height)
        }
    }
}

// MARK: - Floating chrome

// Translucent floating surface for the toolbar, action bar and overlays.
private extension View {
    func glass(_ radius: CGFloat = 18) -> some View {
        background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).strokeBorder(Color.white.opacity(0.12), lineWidth: 0.5))
            .shadow(color: .black.opacity(0.35), radius: 16, y: 6)
    }
}

private struct GlassButtonStyle: ButtonStyle {
    var on = false
    func makeBody(configuration: Configuration) -> some View { Face(configuration: configuration, on: on) }

    struct Face: View {
        let configuration: Configuration
        let on: Bool
        @State private var hover = false
        var body: some View {
            configuration.label
                .font(.system(size: 13, weight: .medium))
                .foregroundColor(on ? C(Theme.text) : C(Theme.textDim))
                .frame(minWidth: 30, minHeight: 28).padding(.horizontal, 2)
                .background(Capsule().fill(Color.white.opacity(configuration.isPressed ? 0.16 : on ? 0.12 : hover ? 0.08 : 0)))
                .contentShape(Capsule())
                .onHover { hover = $0 }
        }
    }
}

extension Filter {
    // Everything except the search text, which lives in the search field.
    var hasConstraints: Bool { var f = self; f.text = ""; return !f.isEmpty }
}

private struct TopBar: View {
    @ObservedObject var model: GalleryModel
    @ObservedObject var lib: Library
    @FocusState private var searchFocused: Bool
    @State private var filtersOpen = false

    var body: some View {
        VStack(spacing: 8) {
            HStack(spacing: 2) {
                Button { model.showSidebar.toggle() } label: { Image(systemName: "sidebar.left") }
                    .buttonStyle(GlassButtonStyle(on: model.showSidebar)).help(model.showSidebar ? "Hide sidebar (⌃⌘S)" : "Show sidebar (⌃⌘S)")
                if !model.showSidebar { scopeMenu }
                search
                Button { filtersOpen.toggle() } label: {
                    Image(systemName: "line.3.horizontal.decrease")
                        .overlay(alignment: .topTrailing) {
                            if model.filter.hasConstraints { Circle().fill(C(Theme.accent)).frame(width: 6, height: 6).offset(x: 5, y: -3) }
                        }
                }
                .buttonStyle(GlassButtonStyle(on: filtersOpen)).help("Filters")
                .popover(isPresented: $filtersOpen, arrowEdge: .bottom) { FilterPanel(model: model, lib: lib) }
                viewMenu
                if lib.progress.total > 0 {
                    ProgressView(value: Double(lib.progress.done), total: Double(max(1, lib.progress.total)))
                        .progressViewStyle(.circular).controlSize(.small).frame(width: 28)
                        .help("Indexing \(lib.progress.done) of \(lib.progress.total)…")
                }
            }
            .padding(4)
            .glass(20)
            if model.filter.hasConstraints { ActiveFilters(model: model) }
            else if model.relatedCount > 0 && !model.filter.text.isEmpty {
                Text(model.relatedCount == model.visible.count ? "No exact matches; showing \(model.relatedCount) related by meaning"
                     : "Plus \(model.relatedCount) related by meaning, after the exact matches")
                    .font(.system(size: 11)).foregroundColor(C(Theme.textDim))
                    .padding(.horizontal, 10).padding(.vertical, 5).glass(12)
            }
        }
        .onChange(of: model.focusSearch) { searchFocused = true }
    }

    private var scopeMenu: some View {
        Menu {
            Section("Library") {
                scope(.all, "All captures"); scope(.recent, "Last 7 days"); scope(.rated, "Rated")
                scope(.uncategorized, "Uncategorized"); scope(.duplicates, "Duplicates")
            }
            Section("Types") { ForEach(MediaType.allCases) { t in scope(.type(t), t.label) } }
            Section("Collections") {
                ForEach(lib.collections) { c in scope(.collection(c.id), c.name) }
                Button("New collection…") {
                    Palette.shared.prompt("New collection", initial: "") { name in
                        if !name.isEmpty { model.scope = .collection(lib.createCollection(name).id) }
                    }
                }
            }
            if !lib.smartFolders.isEmpty {
                Section("Smart folders") { ForEach(lib.smartFolders) { s in scope(.smart(s.id), s.name) } }
            }
        } label: {
            HStack(spacing: 4) {
                Text(model.title).font(.system(size: 13, weight: .semibold)).lineLimit(1).frame(maxWidth: 180)
                Image(systemName: "chevron.down").font(.system(size: 9, weight: .bold)).foregroundColor(C(Theme.muted))
            }
            .padding(.horizontal, 8)
        }
        .menuStyle(.button).buttonStyle(GlassButtonStyle(on: false)).menuIndicator(.hidden).fixedSize()
        .help("\(lib.urls.count) captures in your library")
    }

    private func scope(_ s: Scope, _ title: String) -> some View {
        Button { model.scope = s } label: {
            if model.scope == s { Label(title, systemImage: "checkmark") } else { Text(title) }
        }
    }

    private var search: some View {
        let wide = searchFocused || !model.filter.text.isEmpty
        return HStack(spacing: 6) {
            Image(systemName: "magnifyingglass").foregroundColor(C(Theme.muted))
            TextField(model.visible.count == 1 ? "Search 1 capture" : "Search \(model.visible.count.formatted()) captures", text: $model.filter.text)
                .textFieldStyle(.plain).focused($searchFocused)
                .onSubmit { searchFocused = false }
            if !model.filter.text.isEmpty {
                Button { model.filter.text = "" } label: { Image(systemName: "xmark.circle.fill") }.buttonStyle(.plain).foregroundColor(C(Theme.muted))
            }
        }
        .font(.system(size: 13))
        .padding(.horizontal, 10)
        .frame(width: wide ? 340 : 240, height: 28)
        .background(Capsule().fill(Color.white.opacity(wide ? 0.10 : 0.05)))
        .animation(.easeOut(duration: 0.15), value: wide)
    }

    private var viewMenu: some View {
        Menu {
            Picker("Layout", selection: $model.layout) {
                Label("Rows", systemImage: "rectangle.split.3x3").tag(GalleryLayout.justified)
                Label("Grid", systemImage: "square.grid.3x3").tag(GalleryLayout.grid)
                Label("List", systemImage: "list.bullet").tag(GalleryLayout.list)
            }
            .pickerStyle(.inline)
            Picker("Sort by", selection: $model.sort) { ForEach(GallerySort.allCases) { Text($0.label).tag($0) } }.pickerStyle(.menu)
            if model.sort == .random { Button("Shuffle again") { model.shuffle() } }
            Divider()
            Button("Larger thumbnails  ⌘+") { model.zoom(1.15) }.disabled(model.layout == .list)
            Button("Smaller thumbnails  ⌘−") { model.zoom(1 / 1.15) }.disabled(model.layout == .list)
            Toggle("Show names", isOn: $model.showNames)
            Toggle("Stack edited versions", isOn: $model.stackEdits)
            Divider()
            Toggle("Sidebar  ⌃⌘S", isOn: $model.showSidebar)
            Toggle("Inspector  ⌘I", isOn: $model.showInspector)
            Button("Keyboard shortcuts  ⌘/") { model.showShortcuts = true }
        } label: {
            Image(systemName: "ellipsis")
        }
        .menuStyle(.button).buttonStyle(GlassButtonStyle(on: false)).menuIndicator(.hidden).fixedSize()
        .help("View options")
    }
}

// Chips for the filters in effect, under the toolbar. Hidden when nothing is filtered.
private struct ActiveFilters: View {
    @ObservedObject var model: GalleryModel

    var body: some View {
        HStack(spacing: 6) {
            ForEach(Array(items.enumerated()), id: \.offset) { _, item in
                Button(action: item.1) {
                    HStack(spacing: 5) {
                        Text(item.0).lineLimit(1)
                        Image(systemName: "xmark").font(.system(size: 8, weight: .bold)).foregroundColor(C(Theme.muted))
                    }
                    .font(.system(size: 11, weight: .medium)).foregroundColor(C(Theme.text))
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .glass(12)
                }
                .buttonStyle(.plain).help("Remove filter")
            }
            Button(isSmart ? "Update smart folder" : "Save as smart folder") { model.saveSmartFolder() }
                .buttonStyle(.plain).font(.system(size: 11)).foregroundColor(C(Theme.textDim)).padding(.leading, 4)
        }
    }

    private var isSmart: Bool { if case .smart = model.scope { return true } else { return false } }

    private var items: [(String, () -> Void)] {
        let f = model.filter
        var out: [(String, () -> Void)] = []
        if !f.types.isEmpty { out.append((f.types.map(\.label).joined(separator: ", "), { model.filter.types = [] })) }
        if f.untagged { out.append(("Untagged", { model.filter.untagged = false })) }
        for t in f.tags { out.append(("#" + t, { model.filter.tags.removeAll { $0 == t } })) }
        if f.minRating > 0 { out.append((String(repeating: "★", count: f.minRating) + "+", { model.filter.minRating = 0 })) }
        if let c = f.color { out.append(("Color " + c, { model.filter.color = nil })) }
        if let s = f.shape { out.append((s.label, { model.filter.shape = nil })) }
        if f.minWidth > 0 || f.minHeight > 0 { out.append(("≥ \(f.minWidth) × \(f.minHeight)", { model.filter.minWidth = 0; model.filter.minHeight = 0 })) }
        if let d = f.date { out.append((d.label, { model.filter.date = nil })) }
        if let s = f.size { out.append((s.label, { model.filter.size = nil })) }
        for a in f.apps { out.append((a, { model.filter.apps.removeAll { $0 == a } })) }
        return out
    }
}

private let presetColors: [String] = ["#FF3B30", "#FF9500", "#FFCC00", "#34C759", "#00C7BE", "#0A84FF", "#5E5CE6", "#BF5AF2", "#FF2D55",
                                      "#A2845E", "#FFFFFF", "#8E8E93", "#1C1C1E", "#D4FF00"]

// The filter popover: every filter in one place, colors inline.
private struct FilterPanel: View {
    @ObservedObject var model: GalleryModel
    @ObservedObject var lib: Library
    @State private var custom = Color.red

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            FlowLayout(spacing: 6) {
                chip("Type", model.filter.types.isEmpty ? nil : model.filter.types.map { $0.label }.joined(separator: ", ")) {
                    ForEach(MediaType.allCases) { t in
                        Toggle(t.label, isOn: Binding(get: { model.filter.types.contains(t) }, set: { on in
                            if on { model.filter.types.append(t) } else { model.filter.types.removeAll { $0 == t } }
                        }))
                    }
                }
                chip("Tags", model.filter.untagged ? "Untagged" : model.filter.tags.isEmpty ? nil : model.filter.tags.joined(separator: model.filter.anyTag ? " or " : " + ")) {
                    Toggle("Untagged only", isOn: $model.filter.untagged)
                    Toggle("Match any tag (instead of all)", isOn: $model.filter.anyTag)
                    Divider()
                    ForEach(lib.allTags.prefix(40), id: \.0) { t, n in
                        Toggle("\(t)  (\(n))", isOn: Binding(get: { model.filter.tags.contains { $0.lowercased() == t.lowercased() } }, set: { on in
                            if on { model.filter.tags.append(t) } else { model.filter.tags.removeAll { $0.lowercased() == t.lowercased() } }
                        }))
                    }
                }
                chip("Rating", model.filter.minRating > 0 ? String(repeating: "★", count: model.filter.minRating) + "+" : nil) {
                    Picker("", selection: $model.filter.minRating) {
                        Text("Any").tag(0)
                        ForEach(1...5, id: \.self) { Text(String(repeating: "★", count: $0) + ($0 < 5 ? " or more" : "")).tag($0) }
                    }.pickerStyle(.inline)
                }
                chip("Shape", model.filter.shape?.label) {
                    Picker("", selection: $model.filter.shape) {
                        Text("Any").tag(ShapeFilter?.none)
                        ForEach(ShapeFilter.allCases) { Text($0.label).tag(ShapeFilter?.some($0)) }
                    }.pickerStyle(.inline)
                }
                chip("Dimensions", model.filter.minWidth > 0 || model.filter.minHeight > 0 ? "≥ \(model.filter.minWidth) × \(model.filter.minHeight)" : nil) {
                    ForEach([(0, 0, "Any"), (800, 600, "At least 800 × 600"), (1280, 720, "At least 1280 × 720 (HD)"), (1920, 1080, "At least 1920 × 1080 (Full HD)"),
                             (2560, 1440, "At least 2560 × 1440"), (3840, 2160, "At least 3840 × 2160 (4K)")], id: \.0) { w, h, label in
                        Button(label) { model.filter.minWidth = w; model.filter.minHeight = h }
                    }
                }
                chip("Date", model.filter.date?.label) {
                    Picker("", selection: $model.filter.date) {
                        Text("Any time").tag(DateFilter?.none)
                        ForEach(DateFilter.allCases) { Text($0.label).tag(DateFilter?.some($0)) }
                    }.pickerStyle(.inline)
                }
                chip("File size", model.filter.size?.label) {
                    Picker("", selection: $model.filter.size) {
                        Text("Any size").tag(SizeFilter?.none)
                        ForEach(SizeFilter.allCases) { Text($0.label).tag(SizeFilter?.some($0)) }
                    }.pickerStyle(.inline)
                }
                if !lib.allApps.isEmpty {
                    chip("App", model.filter.apps.isEmpty ? nil : model.filter.apps.joined(separator: ", ")) {
                        Button("Any app") { model.filter.apps = [] }
                        ForEach(lib.allApps, id: \.0) { a, n in
                            Toggle("\(a)  (\(n))", isOn: Binding(get: { model.filter.apps.contains(a) }, set: { on in
                                if on { model.filter.apps.append(a) } else { model.filter.apps.removeAll { $0 == a } }
                            }))
                        }
                    }
                }
            }
            Text("Contains color").font(.system(size: 11, weight: .medium)).foregroundColor(C(Theme.muted))
            LazyVGrid(columns: Array(repeating: GridItem(.fixed(22), spacing: 9), count: 8), alignment: .leading, spacing: 9) {
                ForEach(presetColors, id: \.self) { hex in
                    let c = Filter.rgb(hex)!
                    let on = model.filter.color == hex
                    Circle().fill(Color(red: Double(c.0) / 255, green: Double(c.1) / 255, blue: Double(c.2) / 255)).frame(width: 22, height: 22)
                        .overlay(Circle().stroke(on ? Color.white : Color.white.opacity(0.18), lineWidth: on ? 2 : 0.5))
                        .onTapGesture { model.filter.color = on ? nil : hex }
                        .help(hex)
                }
                ColorPicker("", selection: $custom, supportsOpacity: false).labelsHidden()
                    .onChange(of: custom) { model.filter.color = NSColor(custom).hex }
                    .help("Custom color")
            }
            if model.filter.hasConstraints {
                Divider()
                HStack {
                    Button("Clear filters") { model.filter = Filter(text: model.filter.text) }
                    Spacer()
                    Button("Save as smart folder") { model.saveSmartFolder() }
                }
                .font(.system(size: 12))
            }
        }
        .padding(14).frame(width: 340)
    }

    private func chip<Content: View>(_ name: String, _ value: String?, @ViewBuilder content: () -> Content) -> some View {
        Menu { content() } label: {
            HStack(spacing: 4) {
                Text(value.map { "\(name): \($0)" } ?? name).lineLimit(1)
                Image(systemName: "chevron.down").font(.system(size: 8, weight: .bold)).foregroundColor(C(Theme.muted))
            }
            .font(.system(size: 12, weight: .medium))
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(Capsule().fill(Color.white.opacity(value != nil ? 0.18 : 0.07)))
            .foregroundColor(value != nil ? C(Theme.text) : C(Theme.textDim))
        }
        .menuStyle(.button).buttonStyle(.plain).menuIndicator(.hidden).fixedSize()
    }
}

// Floats over the bottom of the canvas while something is selected.
private struct ActionBar: View {
    @ObservedObject var model: GalleryModel

    var body: some View {
        let n = model.selection.count
        HStack(spacing: 2) {
            Button { model.selection = [] } label: { Image(systemName: "xmark") }.buttonStyle(GlassButtonStyle()).help("Deselect (Esc)")
            Text(n == 1 ? "1 selected" : "\(n) selected").font(.system(size: 12)).monospacedDigit().foregroundColor(C(Theme.textDim))
                .padding(.trailing, 6)
            divider
            action("doc.on.doc", "Copy (⌘C)") { model.copy() }
            action("tag", "Add tags (T)") { model.tagPicker() }
            action("folder.badge.plus", "Add to collection (F)") { model.collectionPicker() }
            Menu {
                ForEach(0...5, id: \.self) { r in Button(r == 0 ? "No rating" : String(repeating: "★", count: r)) { model.rate(r) } }
            } label: { Image(systemName: "star") }
                .menuStyle(.button).buttonStyle(GlassButtonStyle()).menuIndicator(.hidden).fixedSize().help("Rate (0–5)")
            if n == 2 { action("square.split.2x1", "Compare (C)") { model.compareSelected() } }
            if n > 1 { action("rectangle.3.group", "Make collage (⌘G)") { model.collage() } }
            action("pin", "Pin to screen (⌘P)") { model.pin() }
            action("square.and.arrow.up", "Upload and copy link (⌘U)") { model.upload() }
            action("trash", "Move to Trash (⌘⌫)") { model.trash() }
            divider
            Button { model.showInspector.toggle() } label: {
                HStack(spacing: 6) {
                    Image(systemName: "info.circle")
                    if !model.inspectorDiscovered {
                        Text("Details")
                        Text("⌘I").font(.system(size: 10, weight: .semibold)).foregroundColor(C(Theme.muted))
                            .padding(.horizontal, 5).padding(.vertical, 1)
                            .background(RoundedRectangle(cornerRadius: 4).stroke(Color.white.opacity(0.2), lineWidth: 0.5))
                    }
                }
                .padding(.horizontal, model.inspectorDiscovered ? 0 : 6)
            }
            .buttonStyle(GlassButtonStyle(on: model.showInspector)).help(model.showInspector ? "Hide details (⌘I)" : "Show details (⌘I)")
        }
        .padding(4)
        .glass(20)
    }

    private var divider: some View { Rectangle().fill(Color.white.opacity(0.12)).frame(width: 0.5, height: 18).padding(.horizontal, 4) }

    private func action(_ icon: String, _ help: String, _ run: @escaping () -> Void) -> some View {
        Button(action: run) { Image(systemName: icon) }.buttonStyle(GlassButtonStyle()).help(help)
    }
}

// ⌘/ cheat sheet; replaces the old always-on shortcut footer.
private struct ShortcutsCard: View {
    @ObservedObject var model: GalleryModel
    private let rows: [(String, String)] = [
        ("Space", "Preview"), ("↩", "Open or annotate"), ("← → ↑ ↓", "Move selection"), ("T", "Add tags"), ("F", "Add to collection"),
        ("1–5, 0", "Rate, clear rating"), ("C", "Compare versions"), ("/  or  ⌘F", "Search"), ("⌘C", "Copy"), ("⌘T", "Copy text (OCR)"),
        ("⌘P", "Pin to screen"), ("⌘R", "Rename"), ("⌘U", "Upload and copy link"), ("⌘O", "Show in Finder"), ("⌘⌫", "Move to Trash"),
        ("⌘I", "Details"), ("⌃⌘S", "Sidebar"), ("⌘+  ⌘−", "Thumbnail size"), ("⌘⇧S", "Save as smart folder"), ("⌘G", "Make collage"), ("⌘K", "All actions"),
    ]

    var body: some View {
        ZStack {
            Color.black.opacity(0.35).ignoresSafeArea().onTapGesture { model.showShortcuts = false }
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Keyboard shortcuts").font(.system(size: 15, weight: .semibold)).foregroundColor(C(Theme.text))
                    Spacer()
                    Button { model.showShortcuts = false } label: { Image(systemName: "xmark") }.buttonStyle(GlassButtonStyle()).help("Close (Esc)")
                }
                Grid(alignment: .leading, horizontalSpacing: 28, verticalSpacing: 8) {
                    ForEach(0..<(rows.count + 1) / 2, id: \.self) { i in
                        GridRow {
                            cell(rows[i * 2])
                            if i * 2 + 1 < rows.count { cell(rows[i * 2 + 1]) }
                        }
                    }
                }
            }
            .padding(20)
            .frame(width: 560)
            .glass(18)
        }
    }

    @ViewBuilder private func cell(_ r: (String, String)) -> some View {
        Text(r.0).font(.system(size: 12, weight: .medium)).monospacedDigit().foregroundColor(C(Theme.text)).frame(width: 74, alignment: .leading)
        Text(r.1).font(.system(size: 12)).foregroundColor(C(Theme.textDim)).frame(width: 170, alignment: .leading)
    }
}

// MARK: - Inspector

private struct Inspector: View {
    @ObservedObject var model: GalleryModel
    @ObservedObject var lib: Library
    @State private var newTag = ""
    @State private var comment = ""
    @State private var commentFor: URL?
    @State private var showText = false
    @State private var addingTag = false
    @State private var editingComment = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if let u = model.single ?? (model.selection.isEmpty ? model.focus : nil) {
                    single(u)
                } else if model.selected.count > 1 {
                    multiple(model.selected)
                } else {
                    overview
                }
            }
            .padding(14)
        }
        .background(C(Theme.surface))
        .onChange(of: model.single) { addingTag = false; editingComment = false; newTag = "" }
    }

    private func section(_ s: String) -> some View {
        Text(s).font(.system(size: 11, weight: .semibold)).foregroundColor(C(Theme.muted))
    }

    private var overview: some View {
        VStack(alignment: .leading, spacing: 10) {
            section("Library")
            let bytes = lib.urls.reduce(0) { $0 + lib.meta($1).size }
            info("Captures", "\(lib.urls.count)")
            ForEach(MediaType.allCases) { t in info(t.label, "\(lib.urls.filter { MediaType.of($0) == t }.count)") }
            info("On disk", ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file))
            info("Tags", "\(lib.allTags.count)")
            info("Collections", "\(lib.collections.count)")
            Divider().overlay(C(Theme.border))
            Text("Select a capture to tag, rate and comment on it. Space previews, T adds tags, F files it into a collection, 1–5 rates.")
                .font(.system(size: 11)).foregroundColor(C(Theme.muted))
        }
    }

    private func info(_ k: String, _ v: String) -> some View {
        HStack(alignment: .top) {
            Text(k).foregroundColor(C(Theme.muted)).frame(width: 82, alignment: .leading)
            Text(v).foregroundColor(C(Theme.textDim)).textSelection(.enabled).lineLimit(3)
            Spacer(minLength: 0)
        }
        .font(.system(size: 11))
    }

    private func stars(_ rating: Int, _ set: @escaping (Int) -> Void) -> some View {
        HStack(spacing: 3) {
            ForEach(1...5, id: \.self) { i in
                Image(systemName: i <= rating ? "star.fill" : "star")
                    .foregroundColor(i <= rating ? C(Theme.accent) : Color.white.opacity(0.25))
                    .onTapGesture { set(i == rating ? 0 : i) }
            }
        }
        .font(.system(size: 12))
    }

    private func tagChips(_ tags: [String], remove: @escaping (String) -> Void) -> some View {
        FlowLayout(spacing: 5) {
            ForEach(tags, id: \.self) { t in
                HStack(spacing: 4) {
                    Text(t)
                    Image(systemName: "xmark").font(.system(size: 8, weight: .bold)).onTapGesture { remove(t) }
                }
                .font(.system(size: 11)).padding(.horizontal, 7).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 5).fill(C(Theme.raised))).foregroundColor(C(Theme.text))
                .onTapGesture(count: 2) { model.filter.tags = [t] }
            }
        }
    }

    private func tagField(_ targets: [URL]) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            TextField("Add tags, comma-separated", text: $newTag)
                .textFieldStyle(.plain).font(.system(size: 12))
                .padding(.horizontal, 8).padding(.vertical, 5)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.06)))
                .onSubmit {
                    lib.addTags(newTag.split(separator: ",").map(String.init), to: targets)
                    newTag = ""
                    addingTag = false
                }
                .onExitCommand { newTag = ""; addingTag = false }
            let q = newTag.split(separator: ",").last.map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
            if !q.isEmpty {
                let have = Set(targets.flatMap { lib.meta($0).tags.map { $0.lowercased() } })
                let sugg = lib.allTags.map(\.0).filter { $0.lowercased().hasPrefix(q) && !have.contains($0.lowercased()) }.prefix(6)
                FlowLayout(spacing: 4) {
                    ForEach(Array(sugg), id: \.self) { s in
                        Text(s).font(.system(size: 11)).padding(.horizontal, 6).padding(.vertical, 2)
                            .background(RoundedRectangle(cornerRadius: 4).stroke(C(Theme.border))).foregroundColor(C(Theme.textDim))
                            .onTapGesture { lib.addTags([s], to: targets); newTag = "" }
                    }
                }
            }
        }
    }

    private func collectionsRow(_ targets: [URL], member: Set<UUID>) -> some View {
        FlowLayout(spacing: 5) {
            ForEach(lib.collections.filter { member.contains($0.id) }) { c in
                HStack(spacing: 4) {
                    Image(systemName: "folder").font(.system(size: 9))
                    Text(c.name)
                    Image(systemName: "xmark").font(.system(size: 8, weight: .bold)).onTapGesture { lib.remove(targets, fromCollection: c.id) }
                }
                .font(.system(size: 11)).padding(.horizontal, 7).padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 5).fill(C(Theme.raised))).foregroundColor(C(Theme.text))
                .onTapGesture(count: 2) { model.scope = .collection(c.id) }
            }
            Button { model.collectionPicker() } label: {
                Label("Add", systemImage: "plus").font(.system(size: 11))
            }.buttonStyle(.plain).foregroundColor(C(Theme.accent))
        }
    }

    // Only what the capture has: empty tags, collections and comment collapse into "+" buttons.
    @ViewBuilder
    private func single(_ u: URL) -> some View {
        let m = lib.meta(u)
        VStack(alignment: .leading, spacing: 4) {
            Text(u.deletingPathExtension().lastPathComponent).font(.system(size: 13, weight: .semibold)).foregroundColor(C(Theme.text))
                .lineLimit(2).truncationMode(.middle).textSelection(.enabled)
                .onTapGesture(count: 2) { model.rename() }
                .help("Double-click to rename")
            Text(summary(u, m)).font(.system(size: 11)).monospacedDigit().foregroundColor(C(Theme.muted))
        }
        stars(m.rating) { model.lib.setRating($0, [u]) }

        let members = lib.collections.filter { m.collections.contains($0.id) }
        if !m.tags.isEmpty || !members.isEmpty {
            FlowLayout(spacing: 5) {
                ForEach(m.tags, id: \.self) { t in chip(t, icon: nil, open: { model.filter.tags = [t] }) { lib.removeTag(t, from: [u]) } }
                ForEach(members) { c in chip(c.name, icon: "folder", open: { model.scope = .collection(c.id) }) { lib.remove([u], fromCollection: c.id) } }
            }
        }
        let sugg = AutoTag.pending(u, m)
        if !sugg.isEmpty {
            FlowLayout(spacing: 5) {
                ForEach(sugg, id: \.self) { t in
                    Button { lib.addTags([t], to: [u]) } label: {
                        Text("+ " + t).font(.system(size: 11)).padding(.horizontal, 8).padding(.vertical, 3)
                            .overlay(Capsule().strokeBorder(Color.white.opacity(0.22), style: StrokeStyle(lineWidth: 0.75, dash: [3, 2])))
                            .foregroundColor(C(Theme.textDim))
                    }
                    .buttonStyle(.plain)
                    .help("Suggested tag: click to add. Right-click to stop suggesting it.")
                    .contextMenu { Button("Don't suggest “\(t)” for this capture") { lib.dismissSuggestion(t, for: [u]) } }
                }
                if sugg.count > 1 {
                    Button("Add all") { lib.addTags(sugg, to: [u]) }.buttonStyle(.plain).font(.system(size: 11)).foregroundColor(C(Theme.muted))
                }
            }
        }
        if addingTag { tagField([u]) }
        if editingComment || !m.comment.isEmpty { commentEditor(u, m) }
        HStack(spacing: 4) {
            if !addingTag { ghost("Tag", "tag") { addingTag = true } }
            ghost("Collection", "folder") { model.collectionPicker() }
            if !editingComment && m.comment.isEmpty { ghost("Comment", "text.bubble") { editingComment = true } }
        }

        if !m.colors.isEmpty { paletteBar(m) }

        if !m.app.isEmpty || !m.window.isEmpty || m.editedFrom != nil || !m.includes.isEmpty {
            VStack(alignment: .leading, spacing: 5) {
                if !m.app.isEmpty { info("App", m.app) }
                if !m.window.isEmpty { info("Window", m.window) }
                if let o = m.editedFrom {
                    HStack(alignment: .top) {
                        Text("Edited from").foregroundColor(C(Theme.muted)).frame(width: 82, alignment: .leading)
                        Button(URL(fileURLWithPath: o).deletingPathExtension().lastPathComponent) { model.reveal(inGallery: URL(fileURLWithPath: o)) }
                            .buttonStyle(.link).lineLimit(1).truncationMode(.middle)
                    }
                    .font(.system(size: 11))
                }
                if !m.includes.isEmpty {
                    HStack(alignment: .top) {
                        Text("Includes").foregroundColor(C(Theme.muted)).frame(width: 82, alignment: .leading)
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(m.includes.prefix(6), id: \.self) { o in
                                Button(URL(fileURLWithPath: o).deletingPathExtension().lastPathComponent) { model.reveal(inGallery: URL(fileURLWithPath: o)) }
                                    .buttonStyle(.link).lineLimit(1).truncationMode(.middle)
                            }
                            if m.includes.count > 6 { Text("and \(m.includes.count - 6) more").foregroundColor(C(Theme.muted)) }
                        }
                    }
                    .font(.system(size: 11))
                }
            }
        }

        let vs = model.versions(of: u)
        if vs.count > 1 {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Versions").font(.system(size: 11, weight: .medium)).foregroundColor(C(Theme.textDim))
                    Spacer()
                    Button { model.compareSelected() } label: { Label("Compare", systemImage: "square.split.2x1") }
                        .buttonStyle(.plain).font(.system(size: 11)).foregroundColor(C(Theme.muted)).help("Compare with the previous version (C)")
                }
                ForEach(vs, id: \.self) { v in
                    HStack(spacing: 6) {
                        Image(systemName: v == u ? "circle.inset.filled" : lib.meta(v).editedFrom == nil ? "circle" : "pencil.circle").font(.system(size: 10))
                        Text(v.deletingPathExtension().lastPathComponent).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Text(Library.dateFormat.string(from: Date(timeIntervalSince1970: lib.meta(v).mtime))).foregroundColor(C(Theme.muted)).monospacedDigit()
                    }
                    .font(.system(size: 11)).foregroundColor(v == u ? C(Theme.text) : C(Theme.textDim))
                    .contentShape(Rectangle())
                    .onTapGesture { if v != u { model.reveal(inGallery: v) } }
                }
            }
        }

        if let t = m.text, !t.isEmpty {
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 6) {
                    Button { showText.toggle() } label: {
                        HStack(spacing: 5) {
                            Image(systemName: showText ? "chevron.down" : "chevron.right").font(.system(size: 8, weight: .bold))
                            Text("Text in image")
                            Text("\(t.split(whereSeparator: \.isWhitespace).count) words").foregroundColor(C(Theme.muted))
                        }
                    }
                    .buttonStyle(.plain)
                    Spacer()
                    Button { copyText(t); Toast.shared.show("Text copied") } label: { Image(systemName: "doc.on.doc") }
                        .buttonStyle(.plain).foregroundColor(C(Theme.muted)).help("Copy text (⌘T)")
                }
                .font(.system(size: 11, weight: .medium)).foregroundColor(C(Theme.textDim))
                if showText { Text(t).font(.system(size: 11)).foregroundColor(C(Theme.textDim)).textSelection(.enabled) }
            }
        }

        HStack(spacing: 4) {
            ghost(model.isImage(u) ? "Annotate" : "Open", model.isImage(u) ? "pencil.tip.crop.circle" : "play") { model.open(u) }
            ghost("Find similar", "sparkle.magnifyingglass") { model.findSimilar(u) }
        }
        .padding(.top, 4)
    }

    private func summary(_ u: URL, _ m: ItemMeta) -> String {
        var parts: [String] = []
        if m.w > 0 { parts.append("\(m.w) × \(m.h)") }
        if let d = m.duration, MediaType.of(u) != .image { parts.append(formatTime(d)) }
        parts.append(ByteCountFormatter.string(fromByteCount: Int64(m.size), countStyle: .file))
        parts.append(Library.dateFormat.string(from: Date(timeIntervalSince1970: m.mtime)))
        return parts.joined(separator: "  ·  ")
    }

    private func chip(_ s: String, icon: String?, open: @escaping () -> Void, remove: @escaping () -> Void) -> some View {
        HStack(spacing: 4) {
            if let icon { Image(systemName: icon).font(.system(size: 9)) }
            Text(s).onTapGesture(count: 2, perform: open)
            Image(systemName: "xmark").font(.system(size: 7, weight: .bold)).foregroundColor(C(Theme.muted)).onTapGesture(perform: remove)
        }
        .font(.system(size: 11)).padding(.horizontal, 8).padding(.vertical, 3)
        .background(Capsule().fill(Color.white.opacity(0.08))).foregroundColor(C(Theme.text))
        .help("Double-click to show all")
    }

    private func ghost(_ title: String, _ icon: String, _ run: @escaping () -> Void) -> some View {
        Button(action: run) { Label(title, systemImage: icon).labelStyle(.titleAndIcon).font(.system(size: 11)).padding(.horizontal, 6) }
            .buttonStyle(GlassButtonStyle())
    }

    private func commentEditor(_ u: URL, _ m: ItemMeta) -> some View {
        TextEditor(text: $comment)
            .font(.system(size: 12)).scrollContentBackground(.hidden)
            .frame(minHeight: 40, maxHeight: 120)
            .padding(4).background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.05)))
            .onAppear { comment = m.comment; commentFor = u }
            .onChange(of: u) {
                if let p = commentFor, comment != lib.meta(p).comment { lib.setComment(comment, p) }
                comment = lib.meta(u).comment; commentFor = u; editingComment = false
            }
            .onChange(of: comment) { if commentFor == u && comment != lib.meta(u).comment { lib.setComment(comment, u) } }
    }

    private func paletteBar(_ m: ItemMeta) -> some View {
        let cols = Array(m.colors.prefix(6))
        let total = cols.reduce(0) { $0 + $1.ratio }
        return GeometryReader { g in
            HStack(spacing: 0) {
                ForEach(cols, id: \.self) { s in
                    Rectangle().fill(Color(nsColor: s.color))
                        .frame(width: max(3, g.size.width * s.ratio / max(0.0001, total)))
                        .help("\(s.hex)  ·  \(Int(s.ratio * 100))%  ·  click to find this color, right-click to copy")
                        .onTapGesture { model.filter.color = s.hex; if case .similar = model.scope { model.scope = .all } }
                        .contextMenu { Button("Copy \(s.hex)") { copyText(s.hex); Toast.shared.show("\(s.hex) copied") } }
                }
            }
        }
        .frame(height: 12).clipShape(Capsule())
    }

    @ViewBuilder
    private func multiple(_ us: [URL]) -> some View {
        Text("\(us.count) captures selected").font(.system(size: 13, weight: .semibold)).foregroundColor(C(Theme.text))
        let bytes = us.reduce(0) { $0 + lib.meta($1).size }
        Text(ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .file)).font(.system(size: 11)).foregroundColor(C(Theme.muted))
        let rs = Set(us.map { lib.meta($0).rating })
        stars(rs.count == 1 ? rs.first! : 0) { lib.setRating($0, us) }
        section("Tags on all of them")
        let common = us.map { Set(lib.meta($0).tags.map { $0.lowercased() }) }.reduce(Set(lib.meta(us[0]).tags.map { $0.lowercased() })) { $0.intersection($1) }
        let shown = lib.meta(us[0]).tags.filter { common.contains($0.lowercased()) }
        if !shown.isEmpty { tagChips(shown) { lib.removeTag($0, from: us) } }
        tagField(us)
        section("Collections")
        let memberAll = us.map { Set(lib.meta($0).collections) }.reduce(Set(lib.meta(us[0]).collections)) { $0.intersection($1) }
        collectionsRow(us, member: memberAll)
        ghost("Batch rename", "character.cursor.ibeam") { model.batchRename(us) }
    }
}

// MARK: - Preview (zoomable image, GIF, video) with slideshow

private struct ZoomableImage: NSViewRepresentable {
    let url: URL

    final class Coordinator { var url: URL? }
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let s = NSScrollView()
        s.contentView = CenteringClipView()
        s.allowsMagnification = true
        s.minMagnification = 0.05
        s.maxMagnification = 16
        s.hasVerticalScroller = true
        s.hasHorizontalScroller = true
        s.drawsBackground = false
        s.scrollerStyle = .overlay
        let iv = NSImageView()
        iv.imageScaling = .scaleProportionallyUpOrDown
        iv.animates = true
        s.documentView = iv
        return s
    }

    func updateNSView(_ s: NSScrollView, context: Context) {
        guard context.coordinator.url != url, let iv = s.documentView as? NSImageView else { return }
        context.coordinator.url = url
        let img = NSImage(contentsOf: url)
        iv.image = img
        // Fit, at most 1:1 in points for Retina captures.
        DispatchQueue.main.async {
            guard let img, let rep = img.representations.first else { return }
            let px = CGSize(width: rep.pixelsWide, height: rep.pixelsHigh)
            let scale = NSScreen.main?.backingScaleFactor ?? 2
            let natural = CGSize(width: px.width / scale, height: px.height / scale)
            iv.frame = CGRect(origin: .zero, size: natural)
            let fit = min(1, s.bounds.width / natural.width, s.bounds.height / natural.height)
            s.magnification = fit
            let clip = s.contentView
            var b = clip.bounds
            b.origin = CGPoint(x: (iv.frame.width - b.width) / 2, y: (iv.frame.height - b.height) / 2)
            clip.scroll(to: clip.constrainBoundsRect(b).origin)
            s.reflectScrolledClipView(clip)
        }
    }
}

// Keeps a document smaller than the viewport centred instead of pinned to a corner.
private final class CenteringClipView: NSClipView {
    override func constrainBoundsRect(_ proposed: NSRect) -> NSRect {
        var r = super.constrainBoundsRect(proposed)
        guard let doc = documentView else { return r }
        if doc.frame.width < r.width { r.origin.x = (doc.frame.width - r.width) / 2 }
        if doc.frame.height < r.height { r.origin.y = (doc.frame.height - r.height) / 2 }
        return r
    }
}

private struct PreviewOverlay: View {
    @ObservedObject var model: GalleryModel
    let url: URL
    @State private var player: AVPlayer?

    var body: some View {
        let idx = (model.visible.firstIndex(of: url) ?? 0) + 1
        ZStack {
            Color.black.opacity(0.94)
            Group {
                if MediaType.of(url) == .video {
                    VideoPlayer(player: player).onAppear { startPlayer() }.onChange(of: url) { startPlayer() }
                } else {
                    ZoomableImage(url: url)
                }
            }
            .padding(.top, 48).padding(.bottom, 16).padding(.horizontal, 16)
            VStack {
                HStack(spacing: 14) {
                    Text(url.lastPathComponent).font(.system(size: 13, weight: .semibold)).foregroundColor(.white).lineLimit(1)
                    Text("\(idx) / \(model.visible.count)").font(.system(size: 12)).monospacedDigit().foregroundColor(.gray)
                    let m = model.lib.meta(url)
                    if m.w > 0 { Text("\(m.w) × \(m.h)").font(.system(size: 12)).foregroundColor(.gray) }
                    Spacer()
                    Button { model.step(-1) } label: { Image(systemName: "chevron.left") }.help("Previous (←)")
                    Button { model.slideshow.toggle() } label: { Image(systemName: model.slideshow ? "pause.fill" : "play.fill") }.help("Slideshow")
                    Button { model.step(1) } label: { Image(systemName: "chevron.right") }.help("Next (→)")
                    Button { model.open(url) } label: { Image(systemName: model.isImage(url) ? "pencil.and.outline" : "arrow.up.forward.app") }.help("Annotate / open (↩)")
                    Button { model.preview = nil; model.slideshow = false } label: { Image(systemName: "xmark") }.help("Close (Space / Esc)")
                }
                .buttonStyle(.plain).foregroundColor(.white).font(.system(size: 14))
                .padding(.horizontal, 18).padding(.vertical, 12)
                .background(Color.black.opacity(0.5))
                Spacer()
            }
        }
        .task(id: model.slideshow) {
            // One loop per slideshow run; view updates (e.g. indexing progress) don't reset it.
            while model.slideshow, !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 3_000_000_000)
                if model.slideshow && !Task.isCancelled { model.step(1) }
            }
        }
        .onChange(of: url) { if MediaType.of(url) != .video { player?.pause(); player = nil } }
        .onDisappear { player?.pause() }
    }

    private func startPlayer() {
        player?.pause()
        player = AVPlayer(url: url)
        player?.play()
    }
}

// MARK: - Compare

// Before/after: a draggable divider over the two images, or side by side.
private struct CompareOverlay: View {
    @ObservedObject var model: GalleryModel
    let before: URL
    let after: URL
    @State private var split: CGFloat = 0.5
    @State private var sideBySide = false
    @State private var a: NSImage?
    @State private var b: NSImage?

    var body: some View {
        ZStack {
            Color.black.opacity(0.94).ignoresSafeArea()
            VStack(spacing: 12) {
                HStack(spacing: 10) {
                    label("Before", before)
                    Image(systemName: "arrow.right").foregroundColor(C(Theme.muted)).font(.system(size: 11))
                    label("After", after)
                    Spacer()
                    Picker("", selection: $sideBySide) { Text("Slider").tag(false); Text("Side by side").tag(true) }
                        .pickerStyle(.segmented).labelsHidden().frame(width: 180)
                    Button { model.compare = (after, before) } label: { Image(systemName: "arrow.left.arrow.right") }
                        .buttonStyle(GlassButtonStyle()).help("Swap")
                    Button { model.compare = nil } label: { Image(systemName: "xmark") }.buttonStyle(GlassButtonStyle()).help("Close (Esc)")
                }
                .padding(.horizontal, 20).padding(.top, 52)
                GeometryReader { g in
                    if sideBySide {
                        HStack(spacing: 12) {
                            pane(a).frame(width: (g.size.width - 12) / 2)
                            pane(b).frame(width: (g.size.width - 12) / 2)
                        }
                    } else {
                        let box = fitted(g.size)
                        ZStack(alignment: .topLeading) {
                            pane(b).frame(width: box.width, height: box.height)
                            pane(a).frame(width: box.width, height: box.height)
                                .mask(alignment: .leading) { Rectangle().frame(width: box.width * split) }
                            Rectangle().fill(Color.white).frame(width: 2, height: box.height).offset(x: box.width * split - 1)
                            Circle().fill(Color.white).frame(width: 26, height: 26)
                                .overlay(Image(systemName: "arrow.left.and.right").font(.system(size: 11, weight: .bold)).foregroundColor(.black))
                                .offset(x: box.width * split - 13, y: box.height / 2 - 13)
                        }
                        .frame(width: box.width, height: box.height)
                        .contentShape(Rectangle())
                        .gesture(DragGesture(minimumDistance: 0).onChanged { v in split = min(1, max(0, v.location.x / max(1, box.width))) })
                        .position(x: g.size.width / 2, y: g.size.height / 2)
                    }
                }
                .padding(.horizontal, 20).padding(.bottom, 20)
            }
        }
        .task {
            a = NSImage(contentsOf: before)
            b = NSImage(contentsOf: after)
        }
    }

    // One frame for both images, sized by the larger of the two.
    private func fitted(_ s: CGSize) -> CGSize {
        let sizes = [a?.size, b?.size].compactMap { $0 }
        let w = sizes.map(\.width).max() ?? 16, h = sizes.map(\.height).max() ?? 10
        let k = min(s.width / max(1, w), s.height / max(1, h), 2)
        return CGSize(width: w * k, height: h * k)
    }

    private func pane(_ img: NSImage?) -> some View {
        Group {
            if let img { Image(nsImage: img).resizable().interpolation(.high).aspectRatio(contentMode: .fit) } else { Color.clear }
        }
    }

    private func label(_ title: String, _ u: URL) -> some View {
        HStack(spacing: 6) {
            Text(title).font(.system(size: 10, weight: .bold)).foregroundColor(C(Theme.muted))
            Text(u.deletingPathExtension().lastPathComponent).font(.system(size: 12, weight: .medium)).foregroundColor(C(Theme.text)).lineLimit(1)
        }
    }
}

// MARK: - Window

struct GalleryView: View {
    @ObservedObject var model: GalleryModel
    @ObservedObject var lib: Library

    var body: some View {
        ZStack {
            HStack(spacing: 0) {
                if model.showSidebar {
                    Sidebar(model: model, lib: lib).frame(width: 216)
                        .transition(.move(edge: .leading).combined(with: .opacity))
                    hairline
                }
                Browser(model: model, top: model.filter.hasConstraints || (model.relatedCount > 0 && !model.filter.text.isEmpty) ? 92 : 56)
                    .background(C(Theme.bg))
                    .overlay(alignment: .top) { TopBar(model: model, lib: lib).padding(.top, 8).padding(.horizontal, 12) }
                    .overlay(alignment: .bottom) {
                        if !model.selection.isEmpty {
                            ActionBar(model: model).padding(.bottom, 14).transition(.move(edge: .bottom).combined(with: .opacity))
                        }
                    }
                    .frame(minWidth: 480)
                if model.showInspector {
                    hairline
                    inspector.frame(width: 290).transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
            .animation(.snappy(duration: 0.25), value: model.showSidebar)
            .animation(.snappy(duration: 0.25), value: model.showInspector)
            .animation(.snappy(duration: 0.2), value: model.selection.isEmpty)
            if model.showShortcuts { ShortcutsCard(model: model).transition(.opacity) }
            if let p = model.preview { PreviewOverlay(model: model, url: p) }
            if let c = model.compare { CompareOverlay(model: model, before: c.0, after: c.1).id(c.0.path + c.1.path) }
        }
        .ignoresSafeArea()
        .background(C(Theme.bg))
        .preferredColorScheme(.dark)
        .tint(C(Theme.accent))
    }

    private var hairline: some View { Rectangle().fill(Color.white.opacity(0.08)).frame(width: 0.5) }

    private var inspector: some View {
        VStack(spacing: 0) {
            HStack {
                Spacer()
                Button { model.showInspector = false } label: { Image(systemName: "xmark") }
                    .buttonStyle(GlassButtonStyle()).help("Hide details (⌘I)")
            }
            .padding(.horizontal, 8).frame(height: 52)
            Inspector(model: model, lib: lib)
        }
        .background(C(Theme.surface))
    }
}

final class GalleryWindow: NSObject, NSWindowDelegate {
    static var shared: GalleryWindow?
    let model = GalleryModel()
    let window: NSWindow
    private var monitor: Any?

    static func show() {
        if let s = shared {
            Library.shared.refresh()
            activateApp()
            s.window.makeKeyAndOrderFront(nil)
            return
        }
        shared = GalleryWindow()
    }

    private override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 820), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = "Capture gallery"
        window.styleMask.insert(.fullSizeContentView)
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.toolbar = NSToolbar()          // taller title bar, so the traffic lights line up with the floating toolbar
        window.toolbarStyle = .unified
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.minSize = NSSize(width: 640, height: 420)
        window.contentView = NSHostingView(rootView: GalleryView(model: model, lib: Library.shared))
        window.setFrameAutosaveName("AtherGallery")
        if window.frame.origin == .zero { window.center() }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self, e.window === self.window, !Palette.shared.isVisible else { return e }
            return self.key(e) ? nil : e
        }
        Library.shared.refresh { if self.model.focus == nil { self.model.focus = self.model.visible.first } }
        AppDelegate.shared?.windowOpened(window)
        activateApp()
        window.makeKeyAndOrderFront(nil)
        DispatchQueue.main.async { self.endTyping() }   // SwiftUI focuses the search field on open
    }

    private var typing: Bool { window.firstResponder is NSTextView }

    func endTyping() { if typing { window.makeFirstResponder(nil) } }

    // Exposed for tests.
    func key(_ e: NSEvent) -> Bool {
        let m = model
        let f = e.modifierFlags
        let cmd = f.contains(.command), shift = f.contains(.shift)
        let code = Int(e.keyCode)

        if m.showShortcuts {
            if code == kVK_Escape || (cmd && code == kVK_ANSI_Slash) || (shift && code == kVK_ANSI_Slash) { m.showShortcuts = false; return true }
            return true
        }
        if m.compare != nil {
            if code == kVK_Escape || code == kVK_Space || code == kVK_ANSI_C { m.compare = nil }
            return true
        }
        if m.preview != nil {
            switch code {
            case kVK_LeftArrow, kVK_UpArrow: m.step(-1)
            case kVK_RightArrow, kVK_DownArrow: m.step(1)
            case kVK_Escape, kVK_Space: m.preview = nil; m.slideshow = false
            case kVK_Return: m.open(m.preview)
            default: return cmd ? commandKey(code, f) : false
            }
            return true
        }
        if typing {
            if code == kVK_Escape { window.makeFirstResponder(nil); return true }
            if cmd && [kVK_ANSI_I, kVK_ANSI_K, kVK_ANSI_W, kVK_ANSI_S, kVK_ANSI_Slash, kVK_ANSI_G].contains(code) { return commandKey(code, f) }
            if (code == kVK_DownArrow || code == kVK_Return) && window.firstResponder is NSTextView && !(window.firstResponder as! NSTextView).isFieldEditorMultiline {
                window.makeFirstResponder(nil)
                if m.focus == nil { m.move(1) }
                return true
            }
            return false
        }
        if cmd { return commandKey(code, f) }
        switch code {
        case kVK_LeftArrow: m.move(-1, extend: shift)
        case kVK_RightArrow: m.move(1, extend: shift)
        case kVK_UpArrow: m.moveRow(-1, extend: shift)
        case kVK_DownArrow: m.moveRow(1, extend: shift)
        case kVK_Space: m.preview = m.focus ?? m.selected.first
        case kVK_Return, kVK_ANSI_KeypadEnter: m.open()
        case kVK_Escape:
            if case .similar = m.scope { m.scope = .all }
            else if !m.selection.isEmpty { m.selection = [] }
            else if !m.filter.isEmpty { m.filter = Filter() }
            else { window.close() }
        case kVK_ANSI_T: m.tagPicker()
        case kVK_ANSI_F: m.collectionPicker()
        case kVK_ANSI_C: m.compareSelected()
        case kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4, kVK_ANSI_5:
            m.rate(Int(e.charactersIgnoringModifiers ?? "0") ?? 0)
        case kVK_Delete, kVK_ForwardDelete: break
        case kVK_ANSI_Slash: if shift { m.showShortcuts = true } else { m.focusSearch += 1 }
        default: return false
        }
        return true
    }

    private func commandKey(_ code: Int, _ flags: NSEvent.ModifierFlags) -> Bool {
        let m = model
        let shift = flags.contains(.shift), ctrl = flags.contains(.control)
        switch code {
        case kVK_ANSI_S where ctrl: m.showSidebar.toggle()
        case kVK_ANSI_Slash: m.showShortcuts.toggle()
        case kVK_ANSI_Equal: m.zoom(1.15)
        case kVK_ANSI_Minus: m.zoom(1 / 1.15)
        case kVK_ANSI_A: m.selectAll()
        case kVK_ANSI_C: m.copy()
        case kVK_ANSI_P: m.pin()
        case kVK_ANSI_R: m.rename()
        case kVK_ANSI_U: m.upload()
        case kVK_ANSI_T: m.copyText()
        case kVK_ANSI_O: m.reveal()
        case kVK_ANSI_K: m.palette()
        case kVK_ANSI_F: m.focusSearch += 1
        case kVK_ANSI_I: m.showInspector.toggle()
        case kVK_ANSI_G: m.collage()
        case kVK_ANSI_S where shift: m.saveSmartFolder()
        case kVK_Delete, kVK_ForwardDelete: m.trash()
        case kVK_ANSI_W: window.close()
        default: return false
        }
        return true
    }

    func windowWillClose(_ notification: Notification) {
        if let monitor { NSEvent.removeMonitor(monitor) }
        GalleryWindow.shared = nil
    }
}

private extension NSTextView {
    var isFieldEditorMultiline: Bool { !isFieldEditor }
}
