import Foundation

// QuickPrompts: saved phrases and AI-agent prompts. Typing a prompt's trigger and then Space in any app replaces the
// trigger with the prompt (QuickPromptsMac.swift listens and pastes); the picker (⌃⌥P) lists them all.
// The Swift side of src/quickprompts.h/.cpp: the same model, file format, matcher and placeholders, so one
// quickprompts.json / quickprompts-team.json works on both platforms.
//
// Three sources, one list:
//   builtin  the team starters compiled into the app (stable ids "builtin.*"; text can't be edited, only turned off)
//   team     a shared quickprompts-team.json named in Settings (TeamFile); read-only here
//   user     the person's own, in quickprompts.json in the support folder
// When two enabled prompts share a trigger, the person's own wins, then the team's, then the built-in one.
//
// quickprompts.json (UTF-8):
//   {"version": 1,
//    "prompts": [{"id", "trigger", "title", "text", "enabled", "caseSensitive", "hotkey", "tags": [...], "uses", "lastUsed"}],
//    "state": {"<builtin or team id>": {"enabled": false, "uses": 3, "lastUsed": "..."}}}
// A team file has the same "prompts" array (other fields are ignored).

struct QuickPrompt: Equatable {
    var id = ""
    var trigger = ""
    var title = ""
    var text = ""
    var source = "user"  // user | team | builtin
    var enabled = true
    var caseSensitive = false
    var hotkey = ""  // its own global shortcut, e.g. Ctrl+Alt+1 (user prompts only)
    var tags: [String] = []
    var uses: Int64 = 0
    var lastUsed = ""  // ISO 8601 UTC

    var readOnly: Bool { source != "user" }
    var name: String { title.isEmpty ? trigger : title }
}

enum QuickPrompts {
    // "" if the trigger is fine, else why not: 2-32 characters, no spaces or control characters.
    static func checkTrigger(_ trigger: String) -> String {
        if trigger.count < 2 { return "A trigger needs at least 2 characters." }
        if trigger.count > 32 { return "A trigger can have at most 32 characters." }
        for u in trigger.unicodeScalars where CharacterSet.whitespacesAndNewlines.contains(u) || CharacterSet.controlCharacters.contains(u) {
            return "A trigger can't contain spaces."
        }
        return ""
    }

    // The team starters (D11 of docs/intent/text-shortcuts/prompt.md), word for word as on Windows.
    static let builtins: [QuickPrompt] = {
        let b: [(String, String, String, String, String)] = [
            // The team's own starters (from the owner, 2026-11-14), listed first.
            ("builtin.awaypls", "awaypls", "Away for 8 hours: full autonomy",
             "I'll be away from the computer for the next 8 hours, so don't stop to ask for my permission: proceed until "
                 + "the plan is thoroughly complete and validated with visual proofs. If you think any areas should be added to "
                 + "make it better and richer, or any tools should be installed and used to do your job better, go ahead and "
                 + "record your decision. You have full computer-use autonomy.",
             "agent autonomy overnight"),
            ("builtin.bedtimepls", "bedtimepls", "Overnight run with a decision log",
             "I'll be going to bed for the next 8 hours. I want you to keep working towards the outcomes outlined and keep "
                 + "pushing for quality. I authorize you with full permissions to make the decisions needed.\n\n"
                 + "Maintain a decision log in HTML for any out-of-the-ordinary decisions you have to make over the next 8 "
                 + "hours, so I can review it tomorrow.\n\n"
                 + "I entrust you with this responsibility. Godspeed.",
             "agent autonomy overnight log"),
            ("builtin.workbackpls", "workbackpls", "Working Backwards plan",
             "Use Amazon's Working Backwards method: describe the outcomes that would make our players feel joy and value "
                 + "in [Feature]. Based on that, create a living document that details the plan to reach the ultimate outcome, "
                 + "and the features and UX/UI improvements we need to get there.",
             "product planning ux"),
            ("builtin.interiorpls", "interiorpls", "Interior design rendering album",
             "Based on these reference images, create a full interior design concept rendering album of [Number] images "
                 + "covering every interior aspect of a [Luxury Beach Boutique Resort Villa Hotel]: [Front Drop-off, "
                 + "Reception, Grand Welcome Entrance, Lift Lobby, Guest Lounge, Signature Restaurants, Room Corridor, Room "
                 + "Signage, Room Closet, Toilet, Bathtub, Bedroom, Minibar Cupboard, Room Balcony], across all room types.",
             "concept art images interior"),
            ("builtin.archpls", "archpls", "Architecture exploration (50 images)",
             "Based on the established building and level compositions, generate 50 images that focus on the "
                 + "architectural designs, covering all angles and key areas of each design. I want to explore the spectrum of "
                 + "colors and materials that are [Genre] but tastefully blended with [Genre], with touches of [Theme A], "
                 + "[Theme B] and [Theme C].",
             "concept art images architecture"),
            // The first starters.
            ("builtin.doitforme", "doitforme", "Do it for me",
             "Please do this end to end:\n"
                 + "1. Read the relevant code and docs first, so the change fits how this project already works.\n"
                 + "2. Make the change completely, including the tests it needs.\n"
                 + "3. Build and run the tests.\n"
                 + "4. Report what you changed, what you verified and how, and anything you couldn't verify.\n\n"
                 + "Task: {cursor}",
             "agent"),
            ("builtin.planpls", "planpls", "Plan first",
             "Before writing any code, propose a short plan: the files you'll touch, the approach, the risks and how "
                 + "you'll test it. Then wait for my go-ahead.\n\nTask: {cursor}",
             "agent"),
            ("builtin.reviewpls", "reviewpls", "Review this change",
             "Review the current changes for bugs, edge cases, missing tests and anything that breaks existing "
                 + "behaviour. List each finding with file:line, why it matters and a suggested fix, most severe first. "
                 + "Skip style nits.",
             "agent review"),
            ("builtin.explainpls", "explainpls", "Explain this",
             "Explain what this does, step by step, for someone new to this codebase: the purpose, the main flow, and "
                 + "anything surprising.\n\n{cursor}",
             "learn"),
            ("builtin.bugpls", "bugpls", "Describe this bug",
             "Turn the attached screenshot into a bug report with: a one-line title, steps to reproduce, expected "
                 + "result, actual result, and severity (blocker / major / minor). Ask me for anything you can't tell "
                 + "from the picture.\n\n{cursor}{lastcapture:image}",
             "bug screenshot"),
            ("builtin.fixthispls", "fixthispls", "Fix from screenshot",
             "The attached screenshot shows a problem. Find its cause in the code, fix it, and tell me how you "
                 + "verified the fix.\n\n{cursor}{lastcapture:image}",
             "agent bug screenshot"),
            ("builtin.testpls", "testpls", "Write tests",
             "Write tests for this change that cover the normal path, the edge cases and the failure cases. Run them "
                 + "and report the results.\n\n{cursor}",
             "agent test"),
            ("builtin.shorterpls", "shorterpls", "Make it shorter",
             "Rewrite the text below at about half the length. Keep every fact, number and name; drop filler.\n\n"
                 + "{clipboard}",
             "writing"),
            ("builtin.vienpls", "vienpls", "Translate VI ↔ EN",
             "Translate the text below between Vietnamese and English (whichever it isn't in). Keep technical terms, "
                 + "names and the tone.\n\n{clipboard}",
             "writing vietnamese"),
            ("builtin.commitpls", "commitpls", "Commit message",
             "Write a commit message for the staged changes: a summary line under 72 characters, a blank line, then "
                 + "what changed and why in a few short lines.",
             "git"),
            ("builtin.playtestpls", "playtestpls", "Playtest notes",
             "Summarise these playtest notes into a list of issues. For each: a title, steps to reproduce, how often "
                 + "it happened, severity, and which team should own it.\n\n{clipboard}",
             "playtest qa"),
            ("builtin.standupme", "standupme", "Stand-up",
             "Draft my stand-up update from these notes, in three short parts: Yesterday, Today, Blockers.\n\n"
                 + "{clipboard}",
             "team"),
        ]
        return b.map { QuickPrompt(id: $0.0, trigger: $0.1, title: $0.2, text: $0.3, source: "builtin", tags: $0.4.split(separator: " ").map(String.init)) }
    }()

    // MARK: placeholders

    struct Context {
        var clipboard = ""      // {clipboard}
        var lastCapture = ""    // {lastcapture}
        var now = DateComponents(year: 2000, month: 1, day: 1, hour: 0, minute: 0, second: 0)  // {date} {time} {datetime} {date:FORMAT}

        static func current(clipboard: String, lastCapture: String) -> Context {
            Context(clipboard: clipboard, lastCapture: lastCapture,
                    now: Calendar.current.dateComponents([.year, .month, .day, .hour, .minute, .second], from: Date()))
        }
    }

    struct Expansion: Equatable {
        var text = ""            // with \n line breaks
        var cursorBack = 0       // ← presses after pasting, for {cursor} (a line break counts as one)
        var pasteImage = false   // {lastcapture:image}: paste the last capture after the text
    }

    private static let known: Set<String> = ["cursor", "clipboard", "date", "time", "datetime", "lastcapture", "lastcapture:image"]
    static func isKnownPlaceholder(_ name: String) -> Bool {
        let n = name.lowercased()
        return known.contains(n) || n.hasPrefix("date:")
    }

    static func formatDate(_ fmt: String, _ t: DateComponents) -> String {
        let c = Array(fmt)
        var o = "", i = 0
        func at(_ tok: String) -> Bool { i + tok.count <= c.count && String(c[i..<i + tok.count]) == tok }
        func pad(_ v: Int?) -> String { String(format: "%02d", v ?? 0) }
        while i < c.count {
            if at("yyyy") { o += String(t.year ?? 0); i += 4 }
            else if at("MM") { o += pad(t.month); i += 2 }
            else if at("dd") { o += pad(t.day); i += 2 }
            else if at("HH") { o += pad(t.hour); i += 2 }
            else if at("mm") { o += pad(t.minute); i += 2 }
            else if at("ss") { o += pad(t.second); i += 2 }
            else { o.append(c[i]); i += 1 }
        }
        return o
    }

    // {cursor} {clipboard} {date} {time} {datetime} {date:FORMAT} {lastcapture} {lastcapture:image}; {{ and }} are
    // literal braces; anything else in braces is left as typed.
    static func expand(_ text: String, _ ctx: Context) -> Expansion {
        var e = Expansion()
        let c = Array(text)
        var out: [Character] = []
        var cursorAt: Int?
        var i = 0
        while i < c.count {
            let ch = c[i]
            if ch == "{" && i + 1 < c.count && c[i + 1] == "{" { out.append("{"); i += 2; continue }
            if ch == "}" && i + 1 < c.count && c[i + 1] == "}" { out.append("}"); i += 2; continue }
            if ch == "\r" { i += 1; continue }  // \r\n -> \n
            if ch == "\r\n" { out.append("\n"); i += 1; continue }  // Swift keeps \r\n as one Character
            if ch != "{" { out.append(ch); i += 1; continue }
            guard let close = c[(i + 1)...].firstIndex(of: "}"), close - i <= 64 else { out.append(ch); i += 1; continue }
            let name = String(c[(i + 1)..<close]), lower = name.lowercased()
            switch lower {
            case "cursor": if cursorAt == nil { cursorAt = out.count }
            case "clipboard": out += Array(ctx.clipboard.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: ""))
            case "date": out += Array(formatDate("yyyy-MM-dd", ctx.now))
            case "time": out += Array(formatDate("HH:mm", ctx.now))
            case "datetime": out += Array(formatDate("yyyy-MM-dd HH:mm", ctx.now))
            case "lastcapture": out += Array(ctx.lastCapture)
            case "lastcapture:image": e.pasteImage = true
            default:
                if lower.hasPrefix("date:") { out += Array(formatDate(String(name.dropFirst(5)), ctx.now)) }
                else { out += c[i...close] }  // not ours: leave it as typed
            }
            i = close + 1
        }
        if let at = cursorAt { e.cursorBack = out.count - at }
        e.text = String(out)
        return e
    }

    // The placeholders a text uses that aren't known (the editor marks them).
    static func unknownPlaceholders(_ text: String) -> [String] {
        let c = Array(text)
        var v: [String] = []
        var i = 0
        while i < c.count {
            defer { i += 1 }
            guard c[i] == "{" else { continue }
            if i + 1 < c.count && c[i + 1] == "{" { i += 1; continue }
            guard let close = c[(i + 1)...].firstIndex(of: "}"), close - i <= 64 else { continue }
            let name = String(c[(i + 1)..<close])
            if !name.isEmpty && !isKnownPlaceholder(name) && !v.contains(name) { v.append(name) }
            i = close
        }
        return v
    }

    // The Try it box: if `text` ends with "<trigger> ", the text with it expanded and where the caret goes
    // (a Character offset).
    static func tryExpand(_ text: String, active: [QuickPrompt], _ ctx: Context) -> (text: String, caret: Int)? {
        guard text.last == " " else { return nil }
        var m = TriggerMatcher()
        m.setTriggers(active.enumerated().map { .init(trigger: $1.trigger, caseSensitive: $1.caseSensitive, tag: $0) })
        let c = Array(text)
        for ch in c.dropLast() {
            if ch == " " { _ = m.space() } else { m.char(ch) }
        }
        guard let hit = m.space() else { return nil }
        let x = expand(active[hit.tag].text, ctx)
        let before = String(c.dropLast(hit.erase))
        let out = before + x.text + (x.pasteImage ? " [last screenshot]" : "")
        return (out, before.count + x.text.count - x.cursorBack)
    }

    // MARK: import / export formats

    static func parsePrompts(_ data: Data) -> [QuickPrompt]? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return parsePrompts(root)
    }

    static func parsePrompts(_ root: [String: Any]) -> [QuickPrompt] {
        func str(_ v: Any?) -> String { v as? String ?? "" }
        func bool(_ v: Any?, _ d: Bool) -> Bool { (v as? NSNumber).map { CFGetTypeID($0) == CFBooleanGetTypeID() ? $0.boolValue : d } ?? d }
        var v: [QuickPrompt] = []
        for case let o as [String: Any] in root["prompts"] as? [Any] ?? [] {
            var p = QuickPrompt()
            p.id = str(o["id"])
            p.trigger = str(o["trigger"])
            p.title = str(o["title"])
            p.text = str(o["text"])
            p.enabled = bool(o["enabled"], true)
            p.caseSensitive = bool(o["caseSensitive"], false)
            p.hotkey = str(o["hotkey"])
            p.tags = (o["tags"] as? [Any] ?? []).map(str)
            p.uses = (o["uses"] as? NSNumber)?.int64Value ?? 0
            p.lastUsed = str(o["lastUsed"])
            if p.text.isEmpty && p.trigger.isEmpty { continue }
            v.append(p)
        }
        return v
    }

    static func promptJSON(_ p: QuickPrompt, full: Bool) -> [String: Any] {
        var o: [String: Any] = ["id": p.id, "trigger": p.trigger, "title": p.title, "text": p.text, "tags": p.tags]
        if full {
            o["enabled"] = p.enabled
            o["caseSensitive"] = p.caseSensitive
            o["hotkey"] = p.hotkey
            o["uses"] = p.uses
            o["lastUsed"] = p.lastUsed
        }
        return o
    }

    static func encode(_ root: [String: Any]) -> Data {
        (try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])) ?? Data("{}".utf8)
    }

    // RFC 4180-ish: trigger,text[,title]. A first row "trigger,text..." is a header.
    static func parseCsv(_ text: String) -> [QuickPrompt] {
        var rows: [[String]] = [], row: [String] = []
        var field = "", quoted = false, any = false
        let s = Array(text.unicodeScalars)
        var i = 0
        while i < s.count {
            let c = s[i]
            defer { i += 1 }
            if quoted {
                if c == "\"" && i + 1 < s.count && s[i + 1] == "\"" { field.unicodeScalars.append("\""); i += 1 }
                else if c == "\"" { quoted = false }
                else { field.unicodeScalars.append(c) }
                continue
            }
            if c == "\"" { quoted = true; any = true }
            else if c == "," { row.append(field); field = ""; any = true }
            else if c == "\n" || c == "\r" {
                if c == "\r" && i + 1 < s.count && s[i + 1] == "\n" { i += 1 }
                if any || !field.isEmpty { row.append(field) }
                if !row.isEmpty { rows.append(row) }
                row = []; field = ""; any = false
            } else { field.unicodeScalars.append(c) }
        }
        if any || !field.isEmpty { row.append(field) }
        if !row.isEmpty { rows.append(row) }
        var v: [QuickPrompt] = []
        for (r, cols) in rows.enumerated() where cols.count >= 2 {
            if r == 0 && cols[0].lowercased() == "trigger" { continue }
            v.append(QuickPrompt(trigger: cols[0], title: cols.count > 2 ? cols[2] : "", text: cols[1]))
        }
        return v
    }

    static func csvField(_ s: String) -> String {
        guard s.unicodeScalars.contains(where: { ",\"\r\n".unicodeScalars.contains($0) }) else { return s }
        return "\"" + s.replacingOccurrences(of: "\"", with: "\"\"") + "\""
    }

    static func csv(_ prompts: [QuickPrompt]) -> Data {
        var t = "trigger,text,title\r\n"
        for p in prompts { t += csvField(p.trigger) + "," + csvField(p.text) + "," + csvField(p.title) + "\r\n" }
        return Data([0xEF, 0xBB, 0xBF]) + Data(t.utf8)
    }

    // macOS writes Text Replacements.plist (dragged out of System Settings › Keyboard › Text Replacements) as an
    // array of dicts with "phrase" and "shortcut".
    static func parseApplePlist(_ data: Data) -> [QuickPrompt] {
        if let arr = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [Any] {
            let v = arr.compactMap { item -> QuickPrompt? in
                guard let d = item as? [String: Any], let s = d["shortcut"] as? String, let p = d["phrase"] as? String, !s.isEmpty, !p.isEmpty else { return nil }
                return QuickPrompt(trigger: s, text: p)
            }
            if !v.isEmpty { return v }
        }
        return parseApplePlistText(decodeText(data))
    }

    // The Windows reader, for plists PropertyListSerialization refuses (no DOCTYPE, odd entities).
    static func parseApplePlistText(_ text: String) -> [QuickPrompt] {
        var v: [QuickPrompt] = []
        var rest = text[...]
        while let d = rest.range(of: "<dict>"), let e = rest.range(of: "</dict>", range: d.upperBound..<rest.endIndex) {
            let dict = rest[d.upperBound..<e.lowerBound]
            var phrase = "", shortcut = ""
            var k = dict.startIndex
            while let ks = dict.range(of: "<key>", range: k..<dict.endIndex), let ke = dict.range(of: "</key>", range: ks.upperBound..<dict.endIndex) {
                let key = String(dict[ks.upperBound..<ke.lowerBound])
                var val = ""
                let nextKey = dict.range(of: "<key>", range: ke.upperBound..<dict.endIndex)?.lowerBound ?? dict.endIndex
                if let ss = dict.range(of: "<string>", range: ke.upperBound..<dict.endIndex), let se = dict.range(of: "</string>", range: ke.upperBound..<dict.endIndex),
                   ss.upperBound <= se.lowerBound, ss.lowerBound < nextKey {
                    val = xmlUnescape(String(dict[ss.upperBound..<se.lowerBound]))
                }
                if key == "phrase" { phrase = val } else if key == "shortcut" { shortcut = val }
                k = ke.upperBound
            }
            if !shortcut.isEmpty && !phrase.isEmpty { v.append(QuickPrompt(trigger: shortcut, text: phrase)) }
            rest = rest[e.upperBound...]
        }
        return v
    }

    static func xmlUnescape(_ s: String) -> String {
        var o = ""
        var i = s.startIndex
        while i < s.endIndex {
            guard s[i] == "&", let semi = s[i...].firstIndex(of: ";"), s.distance(from: i, to: semi) <= 10 else {
                o.append(s[i]); i = s.index(after: i); continue
            }
            let ent = String(s[s.index(after: i)..<semi])
            switch ent {
            case "amp": o += "&"
            case "lt": o += "<"
            case "gt": o += ">"
            case "quot": o += "\""
            case "apos": o += "'"
            default:
                if ent.hasPrefix("#") {
                    let hex = ent.hasPrefix("#x") || ent.hasPrefix("#X")
                    if let cp = UInt32(ent.dropFirst(hex ? 2 : 1), radix: hex ? 16 : 10), let u = Unicode.Scalar(cp), cp != 0 { o.unicodeScalars.append(u) }
                } else { o += s[i...semi] }
            }
            i = s.index(after: semi)
        }
        return o
    }

    // Text files from other apps: UTF-8 (with or without BOM) or UTF-16 with BOM.
    static func decodeText(_ d: Data) -> String {
        if d.starts(with: [0xFF, 0xFE]) { return String(data: d.dropFirst(2), encoding: .utf16LittleEndian) ?? "" }
        if d.starts(with: [0xFE, 0xFF]) { return String(data: d.dropFirst(2), encoding: .utf16BigEndian) ?? "" }
        let body = d.starts(with: [0xEF, 0xBB, 0xBF]) ? d.dropFirst(3) : d[...]
        return String(decoding: body, as: UTF8.self)
    }

    static func makeId() -> String {
        var g = SystemRandomNumberGenerator()
        return "p" + (0..<8).map { _ in String(format: "%02x", UInt8.random(in: 0...255, using: &g)) }.joined()
    }

    static func nowIso() -> String {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime]
        return f.string(from: Date())
    }
}

// MARK: - matching typed text

// Watches typed characters for "<trigger><Space>". Pure logic: the listener feeds it keys, tests feed it strings.
// The buffer is the last 64 characters typed, in memory only: never saved, logged or shared.
struct TriggerMatcher {
    struct Entry {
        var trigger: String
        var caseSensitive = false
        var tag = 0  // returned on a match (an index into the caller's list)
    }
    static let maxBuffer = 64
    private var entries: [(chars: [Character], caseSensitive: Bool, tag: Int)] = []
    private(set) var buffer: [Character] = []

    mutating func setTriggers(_ list: [Entry]) {
        // Longest first, so "quickfix" beats "fix" (stable for equal lengths).
        entries = list.enumerated().sorted { a, b in
            a.element.trigger.count != b.element.trigger.count ? a.element.trigger.count > b.element.trigger.count : a.offset < b.offset
        }.map { (Array($0.element.trigger), $0.element.caseSensitive, $0.element.tag) }
    }

    // A printable character that isn't a space.
    mutating func char(_ c: Character) {
        buffer.append(c)
        if buffer.count > Self.maxBuffer { buffer.removeFirst(buffer.count - Self.maxBuffer) }
    }

    mutating func backspace() { if !buffer.isEmpty { buffer.removeLast() } }

    // The caret may have moved: focus change, click, arrows, Enter, Tab, shortcuts...
    mutating func reset() { buffer.removeAll(keepingCapacity: true) }

    static func isWordChar(_ c: Character) -> Bool { c.isLetter || c.isNumber || c == "_" }

    private static func same(_ a: Character, _ b: Character, _ cs: Bool) -> Bool { cs ? a == b : a.lowercased() == b.lowercased() }

    // Space typed: the matching entry's tag (longest trigger wins) and the number of characters to delete (the
    // trigger and the space), or nil. On a match the buffer starts over.
    mutating func space() -> (tag: Int, erase: Int)? {
        for e in entries where !e.chars.isEmpty && e.chars.count <= buffer.count {
            let start = buffer.count - e.chars.count
            guard zip(buffer[start...], e.chars).allSatisfy({ Self.same($0, $1, e.caseSensitive) }) else { continue }
            // A whole word only: "redoitforme" must not fire "doitforme". Triggers that start with punctuation
            // (";dfm") still need a boundary before them, which a space or the start of typing gives.
            if start > 0 {
                let prev = buffer[start - 1]
                if Self.isWordChar(prev) || (!Self.isWordChar(e.chars[0]) && !prev.isWhitespace) { continue }
            }
            buffer.removeAll(keepingCapacity: true)
            return (e.tag, e.chars.count + 1)
        }
        char(" ")
        return nil
    }

    // Feeds a whole string (tests, the editor's Try it box): " " is Space, "\u{8}" Backspace, "\n" Reset. Returns the
    // matches, in order.
    @discardableResult
    mutating func feed(_ typed: String) -> [(tag: Int, erase: Int)] {
        var hits: [(Int, Int)] = []
        for c in typed {
            switch c {
            case " ": if let h = space() { hits.append(h) }
            case "\u{8}": backspace()
            case "\n": reset()
            default: char(c)
            }
        }
        return hits
    }
}

// MARK: - the store

final class QuickPromptStore {
    static let shared = QuickPromptStore()
    static let changed = Notification.Name("QuickPromptsChanged")

    private struct State {
        var enabled = true
        var uses: Int64 = 0
        var lastUsed = ""
    }
    private let lock = NSRecursiveLock()
    private(set) var folder: URL?
    private(set) var teamFile = ""
    private var teamErr = ""
    private var user: [QuickPrompt] = []
    private var team: [QuickPrompt] = []
    private var state: [String: State] = [:]
    private var userStamp: Date?, teamStamp: Date?
    private var userBad = false  // the file didn't parse: don't overwrite it
    private(set) var version: UInt64 = 1  // bumps on every change, for the listener and the UI

    var userURL: URL? { folder?.appendingPathComponent("quickprompts.json") }

    private func locked<T>(_ body: () -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body()
    }

    private static func stamp(_ u: URL?) -> Date? {
        guard let u else { return nil }
        return (try? FileManager.default.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date
    }

    private func bump() {
        version += 1
        let n = Notification(name: Self.changed, object: self)
        if Thread.isMainThread { NotificationCenter.default.post(n) } else { DispatchQueue.main.async { NotificationCenter.default.post(n) } }
    }

    private static func readFile(_ u: URL) -> Data? {
        guard let size = (try? FileManager.default.attributesOfItem(atPath: u.path))?[.size] as? NSNumber, size.intValue <= 16 << 20 else { return nil }
        guard var d = try? Data(contentsOf: u) else { return nil }
        if d.starts(with: [0xEF, 0xBB, 0xBF]) { d.removeFirst(3) }
        return d
    }

    // Reads quickprompts.json from `folder` (the support folder) and the team file. Missing files are fine; a file
    // that doesn't parse is kept as quickprompts.json.bad and never overwritten (the error says so).
    @discardableResult
    func load(folder: URL, teamFile: String) -> String {
        locked {
            self.folder = folder
            self.teamFile = teamFile
            user = []
            state = [:]
            userBad = false
            var error = ""
            let path = userURL!
            userStamp = Self.stamp(path)
            if let bytes = Self.readFile(path) {
                if let root = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any] {
                    user = QuickPrompts.parsePrompts(root)
                    for (id, v) in root["state"] as? [String: Any] ?? [:] {
                        guard let st = v as? [String: Any] else { continue }
                        state[id] = State(enabled: (st["enabled"] as? Bool) ?? true, uses: (st["uses"] as? NSNumber)?.int64Value ?? 0,
                                          lastUsed: st["lastUsed"] as? String ?? "")
                    }
                } else {
                    userBad = true  // keep it as it is; a copy shows what went wrong
                    let bad = path.appendingPathExtension("bad")
                    try? FileManager.default.removeItem(at: bad)
                    try? FileManager.default.copyItem(at: path, to: bad)
                    error = "quickprompts.json couldn't be read, so it was left as it is (a copy is in quickprompts.json.bad)."
                }
                for i in user.indices {
                    user[i].source = "user"
                    if user[i].id.isEmpty { user[i].id = QuickPrompts.makeId() }
                }
            }
            loadTeam()
            bump()
            return error
        }
    }

    private func teamURL() -> URL? {
        teamFile.isEmpty ? nil : URL(fileURLWithPath: (teamFile as NSString).expandingTildeInPath)
    }

    private func loadTeam() {
        team = []
        teamErr = ""
        teamStamp = Self.stamp(teamURL())
        guard let u = teamURL() else { return }
        guard let bytes = Self.readFile(u) else { teamErr = "The team prompts file can't be opened: \(teamFile)"; return }
        guard let list = QuickPrompts.parsePrompts(bytes) else { teamErr = "The team prompts file isn't valid JSON: \(teamFile)"; return }
        team = list.enumerated().map { n, p in
            var p = p
            p.source = "team"
            p.hotkey = ""
            p.enabled = true
            if p.id.isEmpty { p.id = "team." + (p.trigger.isEmpty ? String(n) : p.trigger.lowercased()) }
            else if !p.id.hasPrefix("team.") { p.id = "team." + p.id }
            return p
        }
    }

    func setTeamFile(_ path: String) {
        locked {
            guard path != teamFile else { return }
            teamFile = path
            loadTeam()
            bump()
        }
    }

    // Re-reads files changed on disk since the last load or save. True if anything changed.
    @discardableResult
    func reloadIfChanged() -> Bool {
        locked {
            if let f = folder, Self.stamp(userURL) != userStamp {
                load(folder: f, teamFile: teamFile)
                return true
            }
            if Self.stamp(teamURL()) != teamStamp {
                loadTeam()
                bump()
                return true
            }
            return false
        }
    }

    // Atomically (temp + rename).
    @discardableResult
    func save() -> Bool {
        locked {
            bump()
            guard let u = userURL, !userBad else { return false }
            var st: [String: Any] = [:]
            for (id, s) in state where !(s.enabled && s.uses == 0) {
                st[id] = ["enabled": s.enabled, "uses": s.uses, "lastUsed": s.lastUsed] as [String: Any]
            }
            let root: [String: Any] = ["version": 1, "prompts": user.map { QuickPrompts.promptJSON($0, full: true) }, "state": st]
            try? FileManager.default.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
            let ok = (try? QuickPrompts.encode(root).write(to: u, options: .atomic)) != nil
            userStamp = Self.stamp(u)
            return ok
        }
    }

    // Every prompt: built-in, team, then the person's own, each with its enabled state and use count applied.
    var all: [QuickPrompt] {
        locked {
            func withState(_ p: QuickPrompt) -> QuickPrompt {
                guard let s = state[p.id] else { return p }
                var p = p
                p.enabled = s.enabled
                p.uses = s.uses
                p.lastUsed = s.lastUsed
                return p
            }
            return QuickPrompts.builtins.map(withState) + team.map(withState) + user
        }
    }

    // The prompts typed triggers can reach: enabled, one per trigger (user > team > builtin).
    var active: [QuickPrompt] {
        let rank = ["user": 0, "team": 1, "builtin": 2]
        let sorted = all.enumerated().sorted { a, b in
            let ra = rank[a.element.source] ?? 2, rb = rank[b.element.source] ?? 2
            return ra != rb ? ra < rb : a.offset < b.offset
        }.map(\.element)
        var out: [QuickPrompt] = [], seen = Set<String>()
        for p in sorted where p.enabled && QuickPrompts.checkTrigger(p.trigger).isEmpty {
            if seen.insert(p.trigger.lowercased()).inserted { out.append(p) }
        }
        return out
    }

    func find(_ id: String) -> QuickPrompt? { all.first { $0.id == id } }
    var teamError: String { locked { teamErr } }

    // Another enabled prompt with this trigger (case-insensitive), other than `exceptId`; nil if none.
    func triggerOwner(_ trigger: String, except exceptId: String = "") -> String? {
        let t = trigger.lowercased()
        return all.first { $0.id != exceptId && $0.enabled && $0.trigger.lowercased() == t }?.id
    }

    private func validate(_ p: QuickPrompt) -> String {
        let e = QuickPrompts.checkTrigger(p.trigger)
        if !e.isEmpty { return e }
        // Another of the person's own prompts with the same trigger would never be reached.
        if let q = user.first(where: { $0.id != p.id && $0.enabled && p.enabled && $0.trigger.lowercased() == p.trigger.lowercased() }) {
            return "“\(p.trigger)” is already the trigger of “\(q.name)”."
        }
        return ""
    }

    // Changes (each saves). add/update return "" or the reason it was refused (bad or duplicate trigger).
    @discardableResult
    func add(_ p: QuickPrompt) -> (error: String, id: String) {
        locked {
            var p = p
            p.source = "user"
            if p.id.isEmpty || user.contains(where: { $0.id == p.id }) { p.id = QuickPrompts.makeId() }
            let e = validate(p)
            if !e.isEmpty { return (e, "") }
            user.append(p)
            save()
            return ("", p.id)
        }
    }

    @discardableResult
    func update(_ p: QuickPrompt) -> String {
        locked {
            guard let i = user.firstIndex(where: { $0.id == p.id }) else { return "Only your own prompts can be changed. Duplicate it to edit a copy." }
            let e = validate(p)
            if !e.isEmpty { return e }
            var next = p
            next.source = "user"
            user[i] = next
            save()
            return ""
        }
    }

    func remove(_ id: String) {
        locked {
            user.removeAll { $0.id == id }
            save()
        }
    }

    // Any source.
    func setEnabled(_ id: String, _ on: Bool) {
        locked {
            if let i = user.firstIndex(where: { $0.id == id }) { user[i].enabled = on } else { state[id, default: State()].enabled = on }
            save()
        }
    }

    func noteUsed(_ id: String) {
        locked {
            let now = QuickPrompts.nowIso()
            if let i = user.firstIndex(where: { $0.id == id }) {
                user[i].uses += 1
                user[i].lastUsed = now
            } else {
                state[id, default: State()].uses += 1
                state[id, default: State()].lastUsed = now
            }
            save()
        }
    }

    // Import: how many were added; triggers already taken (or not valid) are skipped and listed.
    func importFile(_ u: URL) -> (added: Int, skipped: [String], error: String) {
        guard let data = try? Data(contentsOf: u) else { return (0, [], "Couldn't open \(u.path)") }
        var incoming: [QuickPrompt]
        switch u.pathExtension.lowercased() {
        case "plist": incoming = QuickPrompts.parseApplePlist(data)
        case "csv": incoming = QuickPrompts.parseCsv(QuickPrompts.decodeText(data))
        default:
            guard let list = QuickPrompts.parsePrompts(Data(QuickPrompts.decodeText(data).utf8)) else { return (0, [], "That file isn't a QuickPrompts JSON file.") }
            incoming = list
        }
        return locked {
            var added = 0, skipped: [String] = []
            for var p in incoming {
                p.id = ""
                p.source = "user"
                p.uses = 0
                p.lastUsed = ""
                p.enabled = true
                if !QuickPrompts.checkTrigger(p.trigger).isEmpty || triggerOwner(p.trigger) != nil {
                    skipped.append(p.trigger)
                    continue
                }
                p.id = QuickPrompts.makeId()
                user.append(p)
                added += 1
            }
            if added > 0 { save() }
            return (added, skipped, "")
        }
    }

    // A team file (prompts' id, trigger, title, text and tags) of the given prompts.
    func exportJson(to u: URL, ids: [String]) -> Bool {
        let list = all.filter { ids.contains($0.id) }.map { p -> [String: Any] in
            var q = p
            if q.id.hasPrefix("team.") { q.id = String(q.id.dropFirst(5)) }
            return QuickPrompts.promptJSON(q, full: false)
        }
        return (try? QuickPrompts.encode(["version": 1, "prompts": list]).write(to: u, options: .atomic)) != nil
    }

    func exportCsv(to u: URL, ids: [String]) -> Bool {
        (try? QuickPrompts.csv(all.filter { ids.contains($0.id) }).write(to: u, options: .atomic)) != nil
    }

    // A trigger nobody uses yet, from `base` (newprompt, newprompt2, ...).
    func uniqueTrigger(_ base: String) -> String {
        let b = String(base.prefix(28))
        if triggerOwner(b) == nil && QuickPrompts.checkTrigger(b).isEmpty { return b }
        var i = 2
        while triggerOwner(b + String(i)) != nil { i += 1 }
        return b + String(i)
    }
}
