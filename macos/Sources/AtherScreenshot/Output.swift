import AppKit

struct NameInfo {
    var app = ""
    var window = ""
    var w = 0
    var h = 0
}

enum Output {
    static let defaultTemplate = "Ather_{yyyy}{MM}{dd}_{HH}{mm}{ss}_{ms}"
    static let mediaExtensions: Set<String> = ["png", "jpg", "jpeg", "gif", "mp4", "mov", "heic", "tiff", "webp"]

    static func sanitize(_ s: String, maxLen: Int = 150) -> String {
        var out = String(s.map { c -> Character in
            (c.asciiValue.map { $0 < 32 } ?? false) || "/:\\".contains(c) ? "_" : c
        })
        if out.count > maxLen { out = String(out.prefix(maxLen)) }
        return out.trimmingCharacters(in: CharacterSet(charactersIn: " ."))
    }

    static func uniqueURL(_ dir: URL, _ stem: String, _ ext: String) -> URL {
        var url = dir.appendingPathComponent(stem).appendingPathExtension(ext)
        var i = 2
        while FileManager.default.fileExists(atPath: url.path), i < 10000 {
            url = dir.appendingPathComponent("\(stem) (\(i))").appendingPathExtension(ext)
            i += 1
        }
        return url
    }

    // <captures>/<yyyy-MM>/<template>.<ext>
    static func makeCaptureURL(base: URL, ext: String, info: NameInfo) -> URL {
        let now = Date()
        let c = Calendar.current.dateComponents([.year, .month, .day, .hour, .minute, .second, .nanosecond], from: now)
        func n(_ v: Int?, _ w: Int) -> String { String(format: "%0\(w)d", v ?? 0) }
        let tokens: [(String, String)] = [
            ("{yyyy}", n(c.year, 4)), ("{MM}", n(c.month, 2)), ("{dd}", n(c.day, 2)), ("{HH}", n(c.hour, 2)),
            ("{mm}", n(c.minute, 2)), ("{ss}", n(c.second, 2)), ("{ms}", n((c.nanosecond ?? 0) / 1_000_000, 3)),
            ("{app}", info.app.isEmpty ? "screen" : info.app),
            ("{window}", info.window.isEmpty ? "screen" : String(info.window.prefix(60))),
            ("{w}", String(info.w)), ("{h}", String(info.h)),
        ]
        let tmpl = Settings.shared.string("FileNameTemplate")
        var name = tmpl.isEmpty ? defaultTemplate : tmpl
        for (t, v) in tokens { name = name.replacingOccurrences(of: t, with: v) }
        name = sanitize(name)
        if name.isEmpty { name = "Ather" }
        let dir = base.appendingPathComponent(String(format: "%04d-%02d", c.year ?? 0, c.month ?? 0), isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return uniqueURL(dir, name, ext)
    }

    static func newCaptureURL(ext: String, info: NameInfo = NameInfo()) -> URL {
        makeCaptureURL(base: Settings.shared.capturesFolder, ext: ext, info: info)
    }

    static func savePNG(_ img: CGImage, to url: URL, completion: @escaping (Bool) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            let ok = (try? img.pngData()?.write(to: url, options: .atomic)) != nil
            DispatchQueue.main.async { completion(ok) }
        }
    }

    // Returns the new URL, or nil.
    static func rename(_ url: URL, to newName: String) -> URL? {
        let stem = sanitize(newName)
        guard !stem.isEmpty else { return nil }
        if stem == url.deletingPathExtension().lastPathComponent { return url }
        let dst = uniqueURL(url.deletingLastPathComponent(), stem, url.pathExtension)
        do {
            try FileManager.default.moveItem(at: url, to: dst)
            return dst
        } catch { return nil }
    }

    // Every capture under the captures folder, newest first.
    static func listCaptures() -> [URL] {
        let base = Settings.shared.capturesFolder
        guard let e = FileManager.default.enumerator(at: base, includingPropertiesForKeys: [.contentModificationDateKey, .isRegularFileKey],
                                                     options: [.skipsHiddenFiles]) else { return [] }
        var items: [(URL, Date)] = []
        for case let u as URL in e where mediaExtensions.contains(u.pathExtension.lowercased()) {
            let date = (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            items.append((u, date))
        }
        return items.sorted { $0.1 > $1.1 }.map(\.0)
    }

    static func open(_ url: URL) { NSWorkspace.shared.open(url) }
    static func reveal(_ url: URL) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
}
