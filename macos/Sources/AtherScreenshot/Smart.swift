import Foundation
import NaturalLanguage

// On-device smarts for the gallery: suggested tags, and search that understands related words and dates.

enum AutoTag {
    // Tag → apps that imply it. Names are matched case-insensitively against the capture's source app.
    private static let apps: [(String, [String])] = [
        ("code", ["xcode", "visual studio code", "code", "cursor", "intellij idea", "pycharm", "webstorm", "android studio", "sublime text", "nova", "zed", "bbedit", "goland", "rider"]),
        ("terminal", ["terminal", "iterm2", "iterm", "warp", "ghostty", "alacritty", "kitty", "hyper", "wezterm"]),
        ("chat", ["slack", "messages", "discord", "whatsapp", "telegram", "signal", "microsoft teams", "teams", "messenger", "wechat", "line", "zoom"]),
        ("email", ["mail", "microsoft outlook", "outlook", "spark", "airmail", "mimestream", "superhuman"]),
        ("design", ["figma", "sketch", "adobe xd", "framer", "affinity designer", "affinity designer 2", "adobe photoshop", "adobe illustrator", "pixelmator pro"]),
        ("web", ["safari", "google chrome", "chrome", "arc", "firefox", "microsoft edge", "brave browser", "orion", "dia", "vivaldi", "opera"]),
        ("document", ["pages", "microsoft word", "notion", "obsidian", "bear", "notes", "craft", "preview", "ulysses"]),
        ("spreadsheet", ["numbers", "microsoft excel"]),
        ("calendar", ["calendar", "fantastical", "busycal", "notion calendar"]),
        ("settings", ["system settings", "system preferences"]),
        ("mobile", ["simulator", "iphone mirroring"]),
    ]

    // Tag → words or phrases in the text in the image.
    private static let words: [(String, [String])] = [
        ("error", ["error", "exception", "failed", "failure", "fatal", "denied", "crash", "crashed", "not found", "unable to", "couldn't", "could not",
                   "cannot", "traceback", "panic", "timed out", "timeout", "invalid", "unexpected", "something went wrong"]),
        ("receipt", ["subtotal", "invoice", "receipt", "amount due", "order total", "order #", "order number", "vat", "total paid"]),
        ("login", ["sign in", "log in", "login", "forgot password", "password", "verification code", "two-factor", "sign up"]),
        ("dashboard", ["dashboard", "analytics", "revenue", "metrics", "conversion", "active users", "kpi"]),
        ("settings", ["preferences", "settings"]),
    ]

    private static let codeSignals = ["func ", "def ", "class ", "import ", "const ", "let ", "var ", "return ", "=>", "};", "</", "#include", "public ", "private "]
    private static let terminalSignals = ["$ ", "% ", "~/", "sudo ", "npm ", "git ", "brew ", "cd ", "ls ", "zsh", "bash"]

    private static var cache: [String: (key: String, tags: [String])] = [:]
    private static let lock = NSLock()

    static func suggest(_ u: URL, _ m: ItemMeta) -> [String] {
        let key = "\(m.mtime)|\(m.text?.count ?? -1)|\(m.app)|\(m.w)x\(m.h)|\(m.colors.count)"
        lock.lock()
        if let c = cache[u.path], c.key == key { lock.unlock(); return c.tags }
        lock.unlock()
        let tags = compute(u, m)
        lock.lock()
        cache[u.path] = (key, tags)
        lock.unlock()
        return tags
    }

    // Suggestions not already on the capture and not turned down.
    static func pending(_ u: URL, _ m: ItemMeta) -> [String] {
        let have = Set((m.tags + m.dismissed).map { $0.lowercased() })
        return suggest(u, m).filter { !have.contains($0) }
    }

    static func compute(_ u: URL, _ m: ItemMeta) -> [String] {
        var out: [String] = []
        func add(_ t: String) { if !out.contains(t) { out.append(t) } }
        let app = m.app.lowercased()
        for (t, names) in apps where names.contains(app) { add(t) }
        let text = (m.text ?? "").lowercased()
        let hay = " " + text.replacingOccurrences(of: "\n", with: " ") + " "
        if !text.isEmpty {
            for (t, ws) in words where ws.contains(where: { has(hay, $0) }) { add(t) }
            if codeSignals.filter({ text.contains($0) }).count >= 3 { add("code") }
            if terminalSignals.filter({ text.contains($0) }).count >= 3 { add("terminal") }
            if text.contains("from:") && text.contains("subject:") { add("email") }
            if text.contains("http://") || text.contains("https://") || text.contains("www.") { add("web") }
        }
        if m.w > 0 && m.h > 0 && Double(m.h) / Double(m.w) >= 1.8 && m.w <= 1400 { add("mobile") }
        if let c = m.colors.max(by: { $0.ratio < $1.ratio }), c.ratio > 0.35 {
            let lum = (0.2126 * Double(c.r) + 0.7152 * Double(c.g) + 0.0722 * Double(c.b)) / 255
            if lum < 0.18 { add("dark") }
        }
        return out
    }

    // Whole-word match, so "pay" doesn't match "display".
    static func has(_ hay: String, _ w: String) -> Bool {
        var r = hay.startIndex..<hay.endIndex
        while let f = hay.range(of: w, range: r) {
            let before = f.lowerBound == hay.startIndex ? " " : hay[hay.index(before: f.lowerBound)]
            let after = f.upperBound == hay.endIndex ? " " : hay[f.upperBound]
            if !before.isLetter && !before.isNumber && !after.isLetter && !after.isNumber { return true }
            r = f.upperBound..<hay.endIndex
        }
        return false
    }
}

// A parsed search: words with related words, plus a date range from phrases like "last week".
struct SearchQuery {
    struct Term { let word: String; let related: [String] }
    var terms: [Term] = []
    var since: Date?
    var until: Date?

    enum Match { case exact, related }

    private static let stop: Set<String> = ["the", "a", "an", "and", "or", "of", "to", "in", "on", "with", "for", "from", "that", "this", "my", "me",
                                            "screenshot", "screenshots", "capture", "captures", "image", "showing", "show", "about", "where", "which", "some", "any"]

    // Screenshot vocabulary the general-purpose word model doesn't connect well.
    private static let groups: [[String]] = [
        ["chart", "graph", "plot", "dashboard", "analytics", "metrics", "stats", "statistics"],
        ["login", "log in", "signin", "sign in", "password", "auth", "authentication", "account"],
        ["error", "errors", "failed", "failure", "exception", "crash", "crashed", "crashes", "bug", "issue", "problem", "warning", "fatal"],
        ["chat", "message", "messages", "conversation", "dm", "slack", "discord", "thread", "reply"],
        ["code", "source", "function", "snippet", "programming", "swift", "python", "javascript", "typescript"],
        ["terminal", "shell", "console", "command", "cli", "bash", "zsh"],
        ["build", "compile", "compiler", "compiled", "tests", "passed"],
        ["payment", "checkout", "card", "billing", "pay", "purchase", "order", "declined"],
        ["receipt", "invoice", "bill", "total", "subtotal"],
        ["website", "web", "site", "page", "browser", "landing", "url"],
        ["design", "mockup", "figma", "ui", "layout", "prototype", "illustration"],
        ["email", "mail", "inbox", "subject"],
        ["settings", "preferences", "options", "config", "configuration"],
        ["phone", "mobile", "iphone", "ios", "android", "app"],
        ["team", "colleagues", "coworkers", "people", "teammates"],
        ["sales", "revenue", "income", "profit", "earnings"],
        ["meeting", "call", "zoom", "calendar", "event", "schedule"],
        ["release", "version", "changelog", "notes", "update"],
        ["docs", "documentation", "document", "guide", "manual"],
        ["dark", "night", "black"],
    ]

    private static var cache: [String: SearchQuery] = [:]
    private static var neighborCache: [String: [String]] = [:]
    private static let lock = NSLock()
    private static let embedding = NLEmbedding.wordEmbedding(for: .english)

    static func parse(_ text: String, now: Date = Date()) -> SearchQuery {
        lock.lock()
        if let q = cache[text] { lock.unlock(); return q }
        lock.unlock()
        var q = SearchQuery()
        var s = " " + text.lowercased() + " "
        let cal = Calendar.current
        let today = cal.startOfDay(for: now)
        let phrases: [(String, Date, Date?)] = [
            ("today", today, nil),
            ("yesterday", today.addingTimeInterval(-86400), today),
            ("this week", cal.dateInterval(of: .weekOfYear, for: now)?.start ?? today, nil),
            ("last week", (cal.dateInterval(of: .weekOfYear, for: now)?.start ?? today).addingTimeInterval(-7 * 86400), cal.dateInterval(of: .weekOfYear, for: now)?.start),
            ("past week", now.addingTimeInterval(-7 * 86400), nil),
            ("this month", cal.dateInterval(of: .month, for: now)?.start ?? today, nil),
            ("last month", cal.date(byAdding: .month, value: -1, to: cal.dateInterval(of: .month, for: now)?.start ?? today) ?? today, cal.dateInterval(of: .month, for: now)?.start),
            ("this year", cal.dateInterval(of: .year, for: now)?.start ?? today, nil),
            ("last year", cal.date(byAdding: .year, value: -1, to: cal.dateInterval(of: .year, for: now)?.start ?? today) ?? today, cal.dateInterval(of: .year, for: now)?.start),
        ]
        for (p, a, b) in phrases where s.contains(" \(p) ") {
            q.since = a
            q.until = b
            s = s.replacingOccurrences(of: " \(p) ", with: " ")
        }
        let words = s.split(whereSeparator: { $0 == " " }).map(String.init).filter { !$0.isEmpty && !stop.contains($0) }
        q.terms = words.map { Term(word: $0, related: related($0)) }
        lock.lock()
        if cache.count > 500 { cache.removeAll() }
        cache[text] = q
        lock.unlock()
        return q
    }

    static func related(_ w: String) -> [String] {
        lock.lock()
        if let r = neighborCache[w] { lock.unlock(); return r }
        lock.unlock()
        var out: [String] = []
        func add(_ x: String) { if x != w && !out.contains(x) { out.append(x) } }
        let stem = stemmed(w)
        for g in groups where g.contains(w) || g.contains(stem) { g.forEach(add) }
        if stem != w { add(stem) }
        if w.count >= 3, let e = embedding {   // close neighbors only: the general word model drifts quickly
            for (n, d) in e.neighbors(for: w, maximumCount: 8) where d < 0.95 && n.count >= 3 { add(n.lowercased()) }
        }
        lock.lock()
        neighborCache[w] = out
        lock.unlock()
        return out
    }

    private static func stemmed(_ w: String) -> String {
        for suf in ["ing", "ed", "es", "s"] where w.count > suf.count + 3 && w.hasSuffix(suf) { return String(w.dropLast(suf.count)) }
        return w
    }

    var isEmpty: Bool { terms.isEmpty && since == nil }

    // `hay` is lowercased searchable text. Exact when every word appears as typed; related when some only match a related word.
    func match(_ hay: String, mtime: Double) -> Match? {
        if let since, mtime < since.timeIntervalSince1970 { return nil }
        if let until, mtime >= until.timeIntervalSince1970 { return nil }
        var exact = true
        let padded = " " + hay + " "
        for t in terms {
            if hay.contains(t.word) { continue }
            guard t.related.contains(where: { AutoTag.has(padded, $0) }) else { return nil }
            exact = false
        }
        return exact ? .exact : .related
    }
}
