import AVFoundation
import AppKit
import Combine
import ImageIO
import Vision

// The gallery's metadata layer: every file in the captures folder plus what the user and the indexer
// know about it (tags, rating, comment, collections, source app, OCR text, palette, perceptual hash,
// Vision feature print). Stored in ~/Library/Application Support/AtherScreenshot/library.json.

enum MediaType: String, Codable, CaseIterable, Identifiable {
    case image, gif, video
    var id: String { rawValue }
    static func of(_ u: URL) -> MediaType {
        switch u.pathExtension.lowercased() {
        case "gif": return .gif
        case "mp4", "mov", "m4v": return .video
        default: return .image
        }
    }
    var label: String { self == .image ? "Screenshots" : self == .gif ? "GIFs" : "Videos" }
    var symbol: String { self == .image ? "photo" : self == .gif ? "photo.stack" : "film" }
    var words: String { self == .image ? "image png screenshot" : self == .gif ? "gif animation" : "video mp4 recording movie" }
}

struct PaletteColor: Codable, Hashable {
    var r: UInt8, g: UInt8, b: UInt8
    var ratio: Double
    var hex: String { String(format: "#%02X%02X%02X", r, g, b) }
    var color: NSColor { Theme.rgb(Int(r), Int(g), Int(b)) }
}

struct ItemMeta: Codable {
    var mtime: Double = 0
    var size: Int = 0
    var w = 0, h = 0
    var duration: Double?
    var tags: [String] = []
    var rating = 0
    var comment = ""
    var collections: [UUID] = []
    var app = ""
    var window = ""
    var text: String?          // OCR; nil until indexed
    var colors: [PaletteColor] = []
    var dhash: UInt64?
    var indexed = 0            // indexer version that produced the technical fields

    var aspect: CGFloat { w > 0 && h > 0 ? CGFloat(w) / CGFloat(h) : 16 / 10 }
}

struct LibCollection: Codable, Identifiable, Hashable {
    var id = UUID()
    var name: String
    var autoTags: [String] = []   // added to everything dropped into this collection
}

struct SmartFolder: Codable, Identifiable, Hashable {
    var id = UUID()
    var name: String
    var filter: Filter
}

// MARK: - Filters (the search bar, and what a smart folder saves)

enum ShapeFilter: String, Codable, CaseIterable, Identifiable {
    case landscape, portrait, square, wide, tall
    var id: String { rawValue }
    var label: String {
        switch self {
        case .landscape: return "Landscape"
        case .portrait: return "Portrait"
        case .square: return "Square-ish"
        case .wide: return "Panorama (> 2:1)"
        case .tall: return "Long page (< 1:2)"
        }
    }
    func matches(_ a: CGFloat) -> Bool {
        switch self {
        case .landscape: return a > 1.1
        case .portrait: return a < 0.9
        case .square: return a >= 0.9 && a <= 1.1
        case .wide: return a > 2
        case .tall: return a < 0.5
        }
    }
}

enum DateFilter: String, Codable, CaseIterable, Identifiable {
    case today, week, month, year
    var id: String { rawValue }
    var label: String { ["today": "Today", "week": "Last 7 days", "month": "Last 30 days", "year": "Last 12 months"][rawValue]! }
    var since: Date {
        switch self {
        case .today: return Calendar.current.startOfDay(for: Date())
        case .week: return Date().addingTimeInterval(-7 * 86400)
        case .month: return Date().addingTimeInterval(-30 * 86400)
        case .year: return Date().addingTimeInterval(-365 * 86400)
        }
    }
}

enum SizeFilter: String, Codable, CaseIterable, Identifiable {
    case small, medium, large, huge
    var id: String { rawValue }
    var label: String { ["small": "Under 200 KB", "medium": "200 KB – 2 MB", "large": "2 – 20 MB", "huge": "Over 20 MB"][rawValue]! }
    func matches(_ b: Int) -> Bool {
        switch self {
        case .small: return b < 200_000
        case .medium: return b >= 200_000 && b < 2_000_000
        case .large: return b >= 2_000_000 && b < 20_000_000
        case .huge: return b >= 20_000_000
        }
    }
}

struct Filter: Codable, Equatable, Hashable {
    var text = ""
    var types: [MediaType] = []
    var tags: [String] = []
    var anyTag = false
    var untagged = false
    var minRating = 0
    var color: String?         // hex
    var shape: ShapeFilter?
    var date: DateFilter?
    var size: SizeFilter?
    var apps: [String] = []
    var minWidth = 0
    var minHeight = 0

    var isEmpty: Bool { self == Filter() }
    var activeCount: Int {
        [!text.isEmpty, !types.isEmpty, !tags.isEmpty || untagged, minRating > 0, color != nil, shape != nil, date != nil,
         size != nil, !apps.isEmpty, minWidth > 0 || minHeight > 0].filter { $0 }.count
    }

    func matches(_ u: URL, _ m: ItemMeta) -> Bool {
        if !types.isEmpty && !types.contains(MediaType.of(u)) { return false }
        if untagged && !m.tags.isEmpty { return false }
        if !tags.isEmpty {
            let mine = Set(m.tags.map { $0.lowercased() })
            let want = tags.map { $0.lowercased() }
            if anyTag ? !want.contains(where: mine.contains) : !want.allSatisfy(mine.contains) { return false }
        }
        if minRating > 0 && m.rating < minRating { return false }
        if let shape, !shape.matches(m.aspect) { return false }
        if let date, m.mtime < date.since.timeIntervalSince1970 { return false }
        if let size, !size.matches(m.size) { return false }
        if !apps.isEmpty && !apps.contains(m.app) { return false }
        if minWidth > 0 && m.w < minWidth { return false }
        if minHeight > 0 && m.h < minHeight { return false }
        if let hex = color, let target = Filter.rgb(hex) {
            guard m.colors.contains(where: { $0.ratio >= 0.03 && Filter.distance(target, ($0.r, $0.g, $0.b)) < 100 }) else { return false }
        }
        let words = text.lowercased().split(separator: " ").map(String.init)
        if !words.isEmpty {
            let hay = [u.lastPathComponent, Library.dateFormat.string(from: Date(timeIntervalSince1970: m.mtime)), MediaType.of(u).words,
                       m.text ?? "", m.comment, m.tags.joined(separator: " "), m.app, m.window].joined(separator: " ").lowercased()
            if !words.allSatisfy(hay.contains) { return false }
        }
        return true
    }

    static func rgb(_ hex: String) -> (UInt8, UInt8, UInt8)? {
        let s = hex.trimmingCharacters(in: CharacterSet(charactersIn: "#"))
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return nil }
        return (UInt8(v >> 16 & 255), UInt8(v >> 8 & 255), UInt8(v & 255))
    }

    // "Redmean" colour distance: cheap and close to perceptual. 0 … ~765.
    static func distance(_ a: (UInt8, UInt8, UInt8), _ b: (UInt8, UInt8, UInt8)) -> Double {
        let rm = (Double(a.0) + Double(b.0)) / 2
        let dr = Double(a.0) - Double(b.0), dg = Double(a.1) - Double(b.1), db = Double(a.2) - Double(b.2)
        return sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db)
    }
}

// MARK: - Store

final class Library: ObservableObject {
    static let shared = Library()
    static let indexVersion = 1

    @Published private(set) var urls: [URL] = []
    @Published private(set) var meta: [String: ItemMeta] = [:]
    @Published var collections: [LibCollection] = []
    @Published var smartFolders: [SmartFolder] = []
    @Published private(set) var progress: (done: Int, total: Int) = (0, 0)

    private var features: [String: Data] = [:]                         // archived VNFeaturePrintObservation
    private var printCache: [String: VNFeaturePrintObservation] = [:]
    private var pending: [String: NameInfo] = [:]                     // source app of captures not yet scanned
    private var saveWork: DispatchWorkItem?
    private var indexing = false
    private var loaded = false
    private let io = DispatchQueue(label: "ather.library.io")

    static let dateFormat: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm"
        return f
    }()

    var folder: URL { Settings.shared.capturesFolder }
    private var fileURL: URL { Settings.supportFolder.appendingPathComponent("library.json") }
    private var featuresURL: URL { Settings.supportFolder.appendingPathComponent("features.plist") }

    private struct Saved: Codable {
        var version = 1
        var items: [String: ItemMeta]
        var collections: [LibCollection]
        var smartFolders: [SmartFolder]
    }

    // For tests: an isolated store.
    init(empty: Bool = false) {
        if empty { loaded = true }
    }

    func loadIfNeeded() {
        guard !loaded else { return }
        loaded = true
        if let d = try? Data(contentsOf: fileURL), let s = try? JSONDecoder().decode(Saved.self, from: d) {
            meta = s.items
            collections = s.collections
            smartFolders = s.smartFolders
        } else {
            importLegacyOCR()
        }
        if let d = try? Data(contentsOf: featuresURL), let f = try? PropertyListDecoder().decode([String: Data].self, from: d) { features = f }
    }

    // The first gallery used ocr-index.json; reuse its text so nothing gets OCR'd twice.
    private func importLegacyOCR() {
        struct Old: Codable { var mtime: Double; var text: String }
        let u = Settings.supportFolder.appendingPathComponent("ocr-index.json")
        guard let d = try? Data(contentsOf: u), let old = try? JSONDecoder().decode([String: Old].self, from: d) else { return }
        for (path, o) in old { meta[path, default: ItemMeta()].text = o.text; meta[path]?.mtime = o.mtime }
    }

    func save() {
        saveWork?.cancel()
        let snapshot = Saved(items: meta, collections: collections, smartFolders: smartFolders)
        let feats = features
        let url = fileURL, furl = featuresURL
        let w = DispatchWorkItem {
            if let d = try? JSONEncoder().encode(snapshot) { try? d.write(to: url, options: .atomic) }
            let enc = PropertyListEncoder()
            enc.outputFormat = .binary
            if let d = try? enc.encode(feats) { try? d.write(to: furl, options: .atomic) }
        }
        saveWork = w
        io.asyncAfter(deadline: .now() + 0.8, execute: w)
    }

    // MARK: scanning

    func refresh(done: (() -> Void)? = nil) {
        loadIfNeeded()
        DispatchQueue.global(qos: .userInitiated).async {
            let list = Output.listCaptures()
            let stats: [(URL, Double, Int)] = list.map { u in
                let v = try? u.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
                return (u, v?.contentModificationDate?.timeIntervalSince1970 ?? 0, v?.fileSize ?? 0)
            }
            DispatchQueue.main.async {
                self.apply(stats)
                done?()
                self.indexInBackground()
            }
        }
    }

    // Exposed for tests.
    func apply(_ stats: [(URL, Double, Int)]) {
        var m = meta
        let live = Set(stats.map { $0.0.path })
        for (u, mtime, size) in stats {
            var e = m[u.path] ?? ItemMeta()
            if e.mtime != mtime { e.indexed = 0 }  // changed on disk: re-index the technical fields
            e.mtime = mtime
            e.size = size
            if let info = pending.removeValue(forKey: u.path) {
                if e.app.isEmpty { e.app = info.app }
                if e.window.isEmpty { e.window = info.window }
            }
            m[u.path] = e
        }
        for k in m.keys where !live.contains(k) && k.hasPrefix(folder.path) { m.removeValue(forKey: k) }
        meta = m
        urls = stats.map(\.0)
        save()
    }

    // Called when a capture file name is chosen, so the gallery knows which app it came from.
    func noteCapture(_ url: URL, info: NameInfo) {
        guard !info.app.isEmpty || !info.window.isEmpty else { return }
        pending[url.path] = info
    }

    // MARK: indexing

    private func indexInBackground() {
        guard !indexing else { return }
        let todo = urls.filter { (meta[$0.path]?.indexed ?? 0) < Library.indexVersion }
        guard !todo.isEmpty else { return }
        indexing = true
        progress = (0, todo.count)
        let haveText = Set(meta.filter { $0.value.text != nil && $0.value.indexed == 0 }.map(\.key))
        Task.detached(priority: .utility) {
            for (i, u) in todo.enumerated() {
                let r = await Indexer.index(u, ocr: !haveText.contains(u.path))
                await MainActor.run {
                    if var e = self.meta[u.path] {
                        e.w = r.w
                        e.h = r.h
                        e.duration = r.duration
                        e.colors = r.colors
                        e.dhash = r.dhash
                        if let t = r.text { e.text = t } else if e.text == nil { e.text = "" }
                        e.indexed = Library.indexVersion
                        self.meta[u.path] = e
                    }
                    if let f = r.feature { self.features[u.path] = f; self.printCache[u.path] = nil }
                    self.progress = (i + 1, todo.count)
                    if (i + 1) % 20 == 0 || i == todo.count - 1 { self.save() }
                }
            }
            await MainActor.run {
                self.indexing = false
                self.progress = (0, 0)
                self.indexInBackground()  // files that arrived meanwhile
            }
        }
    }

    // MARK: mutations

    private func edit(_ us: [URL], _ body: (inout ItemMeta) -> Void) {
        var m = meta
        for u in us { body(&m[u.path, default: ItemMeta()]) }
        meta = m
        save()
    }

    static func normalize(_ tags: [String]) -> [String] {
        var seen = Set<String>(), out: [String] = []
        for t in tags.map({ $0.trimmingCharacters(in: .whitespacesAndNewlines) }) where !t.isEmpty && seen.insert(t.lowercased()).inserted { out.append(t) }
        return out
    }

    func addTags(_ tags: [String], to us: [URL]) { edit(us) { $0.tags = Library.normalize($0.tags + tags) } }
    func removeTag(_ tag: String, from us: [URL]) { edit(us) { $0.tags.removeAll { $0.lowercased() == tag.lowercased() } } }
    func setRating(_ r: Int, _ us: [URL]) { edit(us) { $0.rating = min(5, max(0, r)) } }
    func setComment(_ c: String, _ u: URL) { edit([u]) { $0.comment = c } }

    func renameTag(_ old: String, to new: String) {
        let us = urls.filter { meta[$0.path]?.tags.contains { $0.lowercased() == old.lowercased() } ?? false }
        edit(us) { $0.tags = Library.normalize($0.tags.map { $0.lowercased() == old.lowercased() ? new : $0 }) }
        for i in smartFolders.indices { smartFolders[i].filter.tags = smartFolders[i].filter.tags.map { $0.lowercased() == old.lowercased() ? new : $0 } }
    }
    func deleteTag(_ tag: String) { removeTag(tag, from: urls) }

    @discardableResult
    func createCollection(_ name: String) -> LibCollection {
        let c = LibCollection(name: name.trimmingCharacters(in: .whitespaces))
        collections.append(c)
        save()
        return c
    }
    func renameCollection(_ id: UUID, _ name: String) {
        if let i = collections.firstIndex(where: { $0.id == id }) { collections[i].name = name; save() }
    }
    func setAutoTags(_ id: UUID, _ tags: [String]) {
        if let i = collections.firstIndex(where: { $0.id == id }) { collections[i].autoTags = Library.normalize(tags); save() }
    }
    func deleteCollection(_ id: UUID) {
        collections.removeAll { $0.id == id }
        edit(urls) { $0.collections.removeAll { $0 == id } }
    }
    func add(_ us: [URL], toCollection id: UUID) {
        let auto = collections.first { $0.id == id }?.autoTags ?? []
        edit(us) { e in
            if !e.collections.contains(id) { e.collections.append(id) }
            e.tags = Library.normalize(e.tags + auto)
        }
    }
    func remove(_ us: [URL], fromCollection id: UUID) { edit(us) { $0.collections.removeAll { $0 == id } } }

    func saveSmartFolder(_ name: String, _ f: Filter) -> SmartFolder {
        let s = SmartFolder(name: name, filter: f)
        smartFolders.append(s)
        save()
        return s
    }
    func updateSmartFolder(_ id: UUID, filter: Filter? = nil, name: String? = nil) {
        guard let i = smartFolders.firstIndex(where: { $0.id == id }) else { return }
        if let filter { smartFolders[i].filter = filter }
        if let name { smartFolders[i].name = name }
        save()
    }
    func deleteSmartFolder(_ id: UUID) { smartFolders.removeAll { $0.id == id }; save() }

    // Keeps tags, rating etc. when a file is renamed through the app.
    func moved(from a: URL, to b: URL) {
        guard a != b else { return }
        if let e = meta.removeValue(forKey: a.path) { meta[b.path] = e }
        if let f = features.removeValue(forKey: a.path) { features[b.path] = f }
        printCache[a.path] = nil
        if let i = urls.firstIndex(of: a) { urls[i] = b }
        save()
    }

    func removed(_ us: [URL]) {
        let gone = Set(us)
        for u in us { meta.removeValue(forKey: u.path); features.removeValue(forKey: u.path); printCache[u.path] = nil }
        urls.removeAll { gone.contains($0) }
        save()
    }

    // Copies files from elsewhere into <captures>/Imported and indexes them.
    func importFiles(_ files: [URL], into collection: UUID? = nil, done: (([URL]) -> Void)? = nil) {
        let dir = folder.appendingPathComponent("Imported", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var out: [URL] = []
        for f in files where Output.mediaExtensions.contains(f.pathExtension.lowercased()) {
            if f.path.hasPrefix(folder.path) { out.append(f); continue }  // already in the library
            let dst = Output.uniqueURL(dir, f.deletingPathExtension().lastPathComponent, f.pathExtension)
            if (try? FileManager.default.copyItem(at: f, to: dst)) != nil { out.append(dst) }
        }
        refresh {
            if let collection { self.add(out, toCollection: collection) }
            done?(out)
        }
    }

    // MARK: queries

    func meta(_ u: URL) -> ItemMeta { meta[u.path] ?? ItemMeta() }

    var allTags: [(String, Int)] {
        var counts: [String: (String, Int)] = [:]
        for e in meta.values { for t in e.tags { counts[t.lowercased(), default: (t, 0)].1 += 1 } }
        return counts.values.sorted { $0.1 != $1.1 ? $0.1 > $1.1 : $0.0.lowercased() < $1.0.lowercased() }
    }

    var allApps: [(String, Int)] {
        var c: [String: Int] = [:]
        for u in urls { let a = meta(u).app; if !a.isEmpty { c[a, default: 0] += 1 } }
        return c.sorted { $0.value != $1.value ? $0.value > $1.value : $0.key < $1.key }.map { ($0.key, $0.value) }
    }

    func count(in id: UUID) -> Int { urls.filter { meta($0).collections.contains(id) }.count }

    // Groups of identical or near-identical captures, largest first. The perceptual hash finds candidates;
    // the Vision feature print confirms them (a rescaled copy scores ~0.2, a different screen ~1.0).
    func duplicateGroups(maxDistance: Int = 10, maxFeatureDistance: Float = 0.3) -> [[URL]] {
        let items = urls.compactMap { u -> (URL, UInt64)? in meta(u).dhash.map { (u, $0) } }
        var parent = Array(0..<items.count)
        func find(_ i: Int) -> Int { var i = i; while parent[i] != i { parent[i] = parent[parent[i]]; i = parent[i] }; return i }
        for i in items.indices {
            for j in (i + 1)..<max(i + 1, items.count) where (items[i].1 ^ items[j].1).nonzeroBitCount <= maxDistance {
                if let p = featurePrint(items[i].0.path), let q = featurePrint(items[j].0.path) {
                    var d: Float = 0
                    guard (try? p.computeDistance(&d, to: q)) != nil, d <= maxFeatureDistance else { continue }
                } else if (items[i].1 ^ items[j].1).nonzeroBitCount > 3 { continue }  // hash alone: be strict
                let a = find(i), b = find(j)
                if a != b { parent[b] = a }
            }
        }
        var groups: [Int: [URL]] = [:]
        for i in items.indices { groups[find(i), default: []].append(items[i].0) }
        return groups.values.filter { $0.count > 1 }
            .map { $0.sorted { meta($0).mtime > meta($1).mtime } }
            .sorted { $0.count != $1.count ? $0.count > $1.count : meta($0[0]).mtime > meta($1[0]).mtime }
    }

    private func featurePrint(_ path: String) -> VNFeaturePrintObservation? {
        if let p = printCache[path] { return p }
        guard let d = features[path],
              let p = try? NSKeyedUnarchiver.unarchivedObject(ofClass: VNFeaturePrintObservation.self, from: d) else { return nil }
        printCache[path] = p
        return p
    }

    // Reverse image search: everything ordered by visual similarity to `u` (Vision feature prints).
    func similar(to u: URL, limit: Int = 120) -> [(URL, Float)] {
        guard let p = featurePrint(u.path) else { return [] }
        var out: [(URL, Float)] = []
        for v in urls where v != u {
            guard let q = featurePrint(v.path) else { continue }
            var d: Float = 0
            if (try? p.computeDistance(&d, to: q)) != nil { out.append((v, d)) }
        }
        return Array(out.sorted { $0.1 < $1.1 }.prefix(limit))
    }

    var hasFeatures: Bool { !features.isEmpty }

    func testSetHash(_ path: String, _ h: UInt64) { meta[path]?.dhash = h }
    func testSetApp(_ path: String, _ app: String) { meta[path]?.app = app }
    func testSetFeature(_ path: String, _ d: Data) { features[path] = d }
}

// MARK: - Indexer (runs off the main thread)

enum Indexer {
    struct Result {
        var w = 0, h = 0
        var duration: Double?
        var colors: [PaletteColor] = []
        var dhash: UInt64?
        var text: String?
        var feature: Data?
    }

    static func index(_ u: URL, ocr: Bool) async -> Result {
        var r = Result()
        var frame: CGImage?
        var full: CGImage?
        switch MediaType.of(u) {
        case .video:
            let asset = AVURLAsset(url: u)
            if let d = try? await asset.load(.duration) { r.duration = CMTimeGetSeconds(d) }
            if let t = try? await asset.loadTracks(withMediaType: .video).first,
               let (size, tf) = try? await t.load(.naturalSize, .preferredTransform) {
                let s = size.applying(tf)
                r.w = Int(abs(s.width))
                r.h = Int(abs(s.height))
            }
            let gen = AVAssetImageGenerator(asset: asset)
            gen.appliesPreferredTrackTransform = true
            gen.maximumSize = CGSize(width: 512, height: 512)
            let at = CMTime(seconds: min(1, (r.duration ?? 0) / 2), preferredTimescale: 600)
            frame = try? await gen.image(at: at).image
        case .image, .gif:
            guard let src = CGImageSourceCreateWithURL(u as CFURL, nil) else { return r }
            if let p = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any] {
                r.w = p[kCGImagePropertyPixelWidth] as? Int ?? 0
                r.h = p[kCGImagePropertyPixelHeight] as? Int ?? 0
            }
            if MediaType.of(u) == .gif {
                let n = CGImageSourceGetCount(src)
                var total = 0.0
                for i in 0..<n {
                    let gp = (CGImageSourceCopyPropertiesAtIndex(src, i, nil) as? [CFString: Any])?[kCGImagePropertyGIFDictionary] as? [CFString: Any]
                    total += (gp?[kCGImagePropertyGIFUnclampedDelayTime] as? Double) ?? (gp?[kCGImagePropertyGIFDelayTime] as? Double) ?? 0.1
                }
                r.duration = total
            }
            frame = CGImageSourceCreateThumbnailAtIndex(src, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                                                  kCGImageSourceThumbnailMaxPixelSize: 512,
                                                                  kCGImageSourceCreateThumbnailWithTransform: true] as CFDictionary)
            if ocr { full = CGImageSourceCreateImageAtIndex(src, 0, nil) }
        }
        guard let frame else { return r }
        r.colors = palette(frame)
        r.dhash = dhash(frame)
        r.feature = featurePrint(frame)
        if ocr { r.text = ((try? OCR.text(full ?? frame)) ?? "").replacingOccurrences(of: "\n", with: " ") }
        return r
    }

    static func featurePrint(_ img: CGImage) -> Data? {
        let req = VNGenerateImageFeaturePrintRequest()
        guard (try? VNImageRequestHandler(cgImage: img, options: [:]).perform([req])) != nil, let o = req.results?.first else { return nil }
        return try? NSKeyedArchiver.archivedData(withRootObject: o, requiringSecureCoding: true)
    }

    private static func rgba(_ img: CGImage, _ w: Int, _ h: Int) -> [UInt8] {
        var px = [UInt8](repeating: 0, count: w * h * 4)
        px.withUnsafeMutableBytes { buf in
            let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
            ctx?.interpolationQuality = .medium
            ctx?.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        return px
    }

    // Difference hash: 64 bits comparing neighbouring pixels of a 9×8 grayscale thumbnail.
    static func dhash(_ img: CGImage) -> UInt64 {
        let px = rgba(img, 9, 8)
        func lum(_ x: Int, _ y: Int) -> Int { let i = (y * 9 + x) * 4; return Int(px[i]) * 299 + Int(px[i + 1]) * 587 + Int(px[i + 2]) * 114 }
        var h: UInt64 = 0
        for y in 0..<8 { for x in 0..<8 { h = h << 1 | (lum(x, y) > lum(x + 1, y) ? 1 : 0) } }
        return h
    }

    // Dominant colours by k-means on a 48×48 thumbnail, biggest share first.
    static func palette(_ img: CGImage, k: Int = 6) -> [PaletteColor] {
        let n = 48
        let px = rgba(img, n, n)
        var pts: [(Double, Double, Double)] = []
        pts.reserveCapacity(n * n)
        for i in stride(from: 0, to: px.count, by: 4) { pts.append((Double(px[i]), Double(px[i + 1]), Double(px[i + 2]))) }
        // Deterministic seeds spread across the luminance range.
        func lum(_ p: (Double, Double, Double)) -> Double { p.0 + p.1 + p.2 }
        let sorted = pts.sorted { lum($0) < lum($1) }
        var centers: [(Double, Double, Double)] = []
        for j in 0..<k {
            let idx: Int = (j * 2 + 1) * sorted.count / (2 * k)
            centers.append(sorted[min(sorted.count - 1, idx)])
        }
        var assign = [Int](repeating: 0, count: pts.count)
        for _ in 0..<10 {
            var sums = [(Double, Double, Double, Int)](repeating: (0, 0, 0, 0), count: k)
            for (i, p) in pts.enumerated() {
                var best = 0, bd = Double.infinity
                for (j, c) in centers.enumerated() {
                    let dr: Double = p.0 - c.0, dg: Double = p.1 - c.1, db: Double = p.2 - c.2
                    let d: Double = dr * dr + dg * dg + db * db
                    if d < bd { bd = d; best = j }
                }
                assign[i] = best
                sums[best].0 += p.0; sums[best].1 += p.1; sums[best].2 += p.2; sums[best].3 += 1
            }
            for j in 0..<k where sums[j].3 > 0 {
                let n = Double(sums[j].3)
                centers[j] = (sums[j].0 / n, sums[j].1 / n, sums[j].2 / n)
            }
        }
        var counts = [Int](repeating: 0, count: k)
        for a in assign { counts[a] += 1 }
        var out: [PaletteColor] = []
        for j in 0..<k where counts[j] > 0 {
            let c = centers[j]
            let share = Double(counts[j]) / Double(pts.count)
            let s = PaletteColor(r: UInt8(c.0.rounded()), g: UInt8(c.1.rounded()), b: UInt8(c.2.rounded()), ratio: share)
            // Merge near-identical clusters.
            if let i = out.firstIndex(where: { Filter.distance(($0.r, $0.g, $0.b), (s.r, s.g, s.b)) < 30 }) { out[i].ratio += s.ratio } else { out.append(s) }
        }
        return out.sorted { $0.ratio > $1.ratio }
    }
}

// MARK: - Layout helpers

enum GallerySort: String, CaseIterable, Identifiable {
    case newest, oldest, name, size, dimensions, rating, random
    var id: String { rawValue }
    var label: String {
        ["newest": "Newest first", "oldest": "Oldest first", "name": "Name", "size": "File size", "dimensions": "Dimensions", "rating": "Rating", "random": "Random"][rawValue]!
    }
}

struct JustifiedRow: Identifiable {
    var id: Int
    var items: [URL]
    var height: CGFloat
}

// Rows of equal height that fill the width exactly (the last row keeps the target height).
func justifiedRows(_ items: [URL], aspect: (URL) -> CGFloat, width: CGFloat, target: CGFloat, spacing: CGFloat) -> [JustifiedRow] {
    guard width > 0 else { return [] }
    var rows: [JustifiedRow] = []
    var cur: [URL] = []
    var sum: CGFloat = 0
    for u in items {
        let a = min(4, max(0.25, aspect(u)))  // keep panoramas and long pages from swallowing a row
        cur.append(u)
        sum += a
        let w = sum * target + spacing * CGFloat(cur.count - 1)
        if w >= width {
            let h = (width - spacing * CGFloat(cur.count - 1)) / sum
            rows.append(JustifiedRow(id: rows.count, items: cur, height: h))
            cur = []
            sum = 0
        }
    }
    if !cur.isEmpty { rows.append(JustifiedRow(id: rows.count, items: cur, height: target)) }
    return rows
}
