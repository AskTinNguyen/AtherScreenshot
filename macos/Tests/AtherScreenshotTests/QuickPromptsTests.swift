import AppKit
import Carbon
import XCTest
@testable import AtherScreenshot

// QuickPrompts: the same sequences as the Windows self-tests (src/quickprompts.cpp, quickprompts_win.cpp,
// quickpromptsui.cpp), plus the Mac's pasteboard round trip.
final class QuickPromptsTests: XCTestCase {
    private func matcher(_ triggers: [String]) -> TriggerMatcher {
        var m = TriggerMatcher()
        m.setTriggers(triggers.enumerated().map { .init(trigger: $1, caseSensitive: false, tag: $0) })
        return m
    }

    private func tempDir() -> URL {
        let u = FileManager.default.temporaryDirectory.appendingPathComponent("ather-qp-\(UUID().uuidString)", isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    // quickprompts_matcher_expands_after_space_only
    func testMatcherExpandsAfterSpaceOnly() {
        var m = matcher(["doitforme"])
        let h = m.feed("doitforme ")
        XCTAssertEqual(h.count, 1)
        XCTAssertEqual(h.first?.tag, 0)
        XCTAssertEqual(h.first?.erase, 10)
        XCTAssertTrue(m.buffer.isEmpty)
        // Enter, Tab and punctuation never expand (Enter and Tab reset; punctuation is just a character).
        m.reset()
        XCTAssertTrue(m.feed("doitforme\n").isEmpty)
        XCTAssertTrue(m.feed("doitforme. ").isEmpty)
        // Typed with words before it, in the middle of a sentence.
        m.reset()
        XCTAssertEqual(m.feed("hello there doitforme ").count, 1)
    }

    // quickprompts_matcher_whole_words_only
    func testMatcherWholeWordsOnly() {
        var m = matcher(["doitforme"])
        XCTAssertTrue(m.feed("redoitforme ").isEmpty)
        m.reset()
        XCTAssertTrue(m.feed("x_doitforme ").isEmpty)
        m.reset()
        XCTAssertEqual(m.feed("(doitforme ").count, 1)  // after punctuation is a new word
        // Prefixed triggers need a space or the start of typing before them.
        var p = matcher([";dfm"])
        XCTAssertEqual(p.feed(";dfm ").count, 1)
        XCTAssertEqual(p.feed("ok ;dfm ").count, 1)
        XCTAssertTrue(p.feed("a;dfm ").isEmpty)
        XCTAssertTrue(p.feed("x.;dfm ").isEmpty)
    }

    // quickprompts_matcher_backspace_reset_case_longest
    func testMatcherBackspaceResetCaseLongest() {
        var m = matcher(["doitforme"])
        XCTAssertEqual(m.feed("doitfoX\u{8}rme ").count, 1)
        XCTAssertTrue(m.feed("doitfo\nrme ").isEmpty)  // the caret moved in the middle
        m.reset()
        XCTAssertEqual(m.feed("DoItForMe ").count, 1)  // case-insensitive by default
        var cs = TriggerMatcher()
        cs.setTriggers([.init(trigger: "Sig", caseSensitive: true, tag: 7)])
        XCTAssertTrue(cs.feed("sig ").isEmpty)
        cs.reset()
        var h = cs.feed("Sig ")
        XCTAssertTrue(h.count == 1 && h[0].tag == 7 && h[0].erase == 4)
        var l = matcher(["fix", "quickfix"])
        h = l.feed("quickfix ")
        XCTAssertTrue(h.count == 1 && h[0].tag == 1 && h[0].erase == 9)
        l.reset()
        h = l.feed("fix ")
        XCTAssertTrue(h.count == 1 && h[0].tag == 0)
        // The buffer stays bounded however long people type without a reset.
        var b = matcher(["zz"])
        b.feed(String(repeating: "a", count: 500))
        XCTAssertLessThanOrEqual(b.buffer.count, TriggerMatcher.maxBuffer)
        XCTAssertEqual(b.feed(" zz ").count, 1)
    }

    // quickprompts_placeholders
    func testPlaceholders() {
        var ctx = QuickPrompts.Context()
        ctx.clipboard = "copied\r\ntext"
        ctx.lastCapture = "/Users/me/shots/a.png"
        ctx.now = DateComponents(year: 2026, month: 11, day: 4, hour: 9, minute: 5, second: 7)
        var e = QuickPrompts.expand("Hi {cursor}!\r\nbye", ctx)
        XCTAssertEqual(e.text, "Hi !\nbye")
        XCTAssertEqual(e.cursorBack, 5)
        XCTAssertFalse(e.pasteImage)
        e = QuickPrompts.expand("{clipboard}|{date}|{time}|{datetime}|{date:dd/MM/yyyy ss}|{lastcapture}", ctx)
        XCTAssertEqual(e.text, "copied\ntext|2026-11-04|09:05|2026-11-04 09:05|04/11/2026 07|/Users/me/shots/a.png")
        e = QuickPrompts.expand("{{literal}} {unknown} {LastCapture:Image}", ctx)
        XCTAssertEqual(e.text, "{literal} {unknown} ")
        XCTAssertTrue(e.pasteImage)
        XCTAssertEqual(e.cursorBack, 0)
        XCTAssertEqual(QuickPrompts.unknownPlaceholders("{cursor} {nope} {{x}} {date:yyyy} {nope}"), ["nope"])
        // Every built-in prompt uses only known placeholders and has a valid, unique trigger.
        XCTAssertEqual(QuickPrompts.builtins.count, 17)
        for p in QuickPrompts.builtins {
            XCTAssertTrue(QuickPrompts.unknownPlaceholders(p.text).isEmpty, p.trigger)
            XCTAssertEqual(QuickPrompts.checkTrigger(p.trigger), "", p.trigger)
            XCTAssertEqual(QuickPrompts.builtins.filter { $0.trigger.lowercased() == p.trigger.lowercased() }.count, 1)
            XCTAssertEqual(p.id, "builtin." + p.trigger)
        }
        XCTAssertNotEqual(QuickPrompts.checkTrigger("a"), "")
        XCTAssertNotEqual(QuickPrompts.checkTrigger("a b"), "")
        XCTAssertNotEqual(QuickPrompts.checkTrigger(String(repeating: "x", count: 33)), "")
    }

    // quickprompts_store_round_trip_and_precedence
    func testStoreRoundTripAndPrecedence() throws {
        let dir = tempDir()
        let s = QuickPromptStore()
        XCTAssertEqual(s.load(folder: dir, teamFile: ""), "")
        XCTAssertEqual(s.all.count, QuickPrompts.builtins.count)
        var p = QuickPrompt()
        p.trigger = "sigvn"
        p.title = "Chữ ký"
        p.text = "Trân trọng,\nNguyễn {{x}} 🎮"
        p.tags = ["mail"]
        let (err, id) = s.add(p)
        XCTAssertEqual(err, "")
        XCTAssertFalse(id.isEmpty)
        XCTAssertNotEqual(s.add(p).error, "")  // the same trigger twice
        XCTAssertNotEqual(s.add(QuickPrompt(trigger: "a b", text: "x")).error, "")
        s.setEnabled("builtin.planpls", false)
        s.noteUsed("builtin.doitforme")

        let r = QuickPromptStore()
        r.load(folder: dir, teamFile: "")
        let back = try XCTUnwrap(r.find(id))
        XCTAssertEqual(back.text, p.text)
        XCTAssertEqual(back.title, p.title)
        XCTAssertEqual(back.tags, ["mail"])
        XCTAssertEqual(r.find("builtin.planpls")?.enabled, false)
        XCTAssertEqual(r.find("builtin.doitforme")?.uses, 1)
        XCTAssertFalse(r.active.contains { $0.id == "builtin.planpls" })

        // The person's own prompt beats a built-in one with the same trigger; team beats built-in.
        let team = dir.appendingPathComponent("team.json")
        try Data(#"{"prompts": [{"id": "t1", "trigger": "reviewpls", "title": "Team review", "text": "team"}, {"trigger": "doitforme", "text": "team doit"}]}"#.utf8).write(to: team)
        XCTAssertEqual(r.add(QuickPrompt(trigger: "DoItForMe", text: "mine")).error, "")
        r.setTeamFile(team.path)
        XCTAssertEqual(r.teamError, "")
        let active = r.active
        func textOf(_ t: String) -> String { active.first { $0.trigger.lowercased() == t }?.text ?? "" }
        XCTAssertEqual(textOf("doitforme"), "mine")
        XCTAssertEqual(textOf("reviewpls"), "team")
        let t1 = try XCTUnwrap(r.find("team.t1"))
        XCTAssertTrue(t1.readOnly)
        XCTAssertNotEqual(r.update(t1), "")  // read-only
        XCTAssertNotNil(r.find("team.doitforme"))

        // A hand-edited file is picked up; a broken one is never overwritten.
        let file = dir.appendingPathComponent("quickprompts.json")
        try Data(#"{"version": 1, "prompts": [{"id": "h1", "trigger": "handmade", "text": "by hand"}]}"#.utf8).write(to: file)
        try FileManager.default.setAttributes([.modificationDate: Date().addingTimeInterval(5)], ofItemAtPath: file.path)
        XCTAssertTrue(r.reloadIfChanged())
        XCTAssertNotNil(r.find("h1"))
        try Data("{ not json".utf8).write(to: file)
        let bad = QuickPromptStore()
        XCTAssertNotEqual(bad.load(folder: dir, teamFile: ""), "")
        XCTAssertTrue(FileManager.default.fileExists(atPath: dir.appendingPathComponent("quickprompts.json.bad").path))
        XCTAssertNotEqual(bad.add(QuickPrompt(trigger: "zzz", text: "x")).error, "")  // refused: the broken file is kept
        XCTAssertNotEqual(bad.noteUsed("builtin.doitforme"), "")
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), "{ not json")
    }

    // A file that exists but can't be used (too big, "prompts" not a list) is never replaced by an empty list, and
    // the first overwrite of a session keeps the previous file as quickprompts.json.bak.
    func testUnreadableFileIsNeverOverwritten() throws {
        let fm = FileManager.default
        for contents in [Data(#"{"version": 1, "prompts": {"id": "x", "trigger": "keepme", "text": "mine"}}"#.utf8),
                         Data(#"{"version": 1}"#.utf8),
                         Data(count: (16 << 20) + 10)] {
            let dir = tempDir()
            let file = dir.appendingPathComponent("quickprompts.json")
            try contents.write(to: file)
            let s = QuickPromptStore()
            XCTAssertNotEqual(s.load(folder: dir, teamFile: ""), "")
            XCTAssertNotEqual(s.userError, "")
            XCTAssertTrue(fm.fileExists(atPath: dir.appendingPathComponent("quickprompts.json.bad").path))
            XCTAssertNotEqual(s.noteUsed("builtin.doitforme"), "")
            XCTAssertNotEqual(s.setEnabled("builtin.planpls", false), "")
            XCTAssertNotEqual(s.remove("x"), "")
            XCTAssertNotEqual(s.add(QuickPrompt(trigger: "zzz", text: "x")).error, "")
            XCTAssertFalse(s.save())
            XCTAssertEqual(try Data(contentsOf: file), contents)
            XCTAssertEqual(s.find("builtin.planpls")?.enabled, true)  // refused edits don't linger in memory
        }

        let dir = tempDir()
        let file = dir.appendingPathComponent("quickprompts.json")
        let original = Data(#"{"version": 1, "prompts": [{"id": "h1", "trigger": "handmade", "text": "by hand"}]}"#.utf8)
        try original.write(to: file)
        let s = QuickPromptStore()
        XCTAssertEqual(s.load(folder: dir, teamFile: ""), "")
        XCTAssertEqual(s.noteUsed("h1"), "")
        let bak = dir.appendingPathComponent("quickprompts.json.bak")
        XCTAssertEqual(try Data(contentsOf: bak), original)
        XCTAssertEqual(s.noteUsed("h1"), "")
        XCTAssertEqual(try Data(contentsOf: bak), original)  // once per session
        XCTAssertEqual(s.find("h1")?.uses, 2)
    }

    // ⌥⌫ deletes a word and ⌘⌫ a line: the typed buffer is forgotten, not shortened by one character, so a later
    // expansion can't erase the person's text (Windows resets on Ctrl/Alt, src/quickprompts_win.cpp).
    func testDeleteWithModifiersResetsMatcher() {
        typealias K = QuickPromptsMac
        XCTAssertEqual(K.keyAction(code: kVK_Delete, flags: []), .backspace)
        XCTAssertEqual(K.keyAction(code: kVK_Delete, flags: .maskAlternate), .reset)
        XCTAssertEqual(K.keyAction(code: kVK_Delete, flags: .maskCommand), .reset)
        XCTAssertEqual(K.keyAction(code: kVK_Delete, flags: .maskShift), .reset)
        XCTAssertEqual(K.keyAction(code: kVK_LeftArrow, flags: .maskAlternate), .reset)
        XCTAssertEqual(K.keyAction(code: kVK_ForwardDelete, flags: .maskAlternate), .reset)
        XCTAssertEqual(K.keyAction(code: kVK_Space, flags: []), .space)
        XCTAssertEqual(K.keyAction(code: kVK_Space, flags: .maskShift), .typedSpace)
        XCTAssertEqual(K.keyAction(code: kVK_ANSI_E, flags: .maskAlternate), .text)  // ⌥e: an accent, still typing

        // "hello doitforme" then ⌥⌫ (the word is gone) then "x ": nothing expands; a plain ⌫ only drops a character.
        var m = matcher(["doitforme"])
        m.feed("hello doitform")
        m.reset()  // what ⌥⌫ does now
        XCTAssertTrue(m.feed("e ").isEmpty)
        var n = matcher(["doitforme"])
        XCTAssertEqual(n.feed("doitformx\u{8}e ").count, 1)
    }

    // The file on disk has the shape the Windows app reads (and a Windows-written file loads here).
    func testFileFormatMatchesWindows() throws {
        let dir = tempDir()
        let windows = #"""
        {"prompts":[{"caseSensitive":false,"enabled":true,"hotkey":"Ctrl+Alt+1","id":"p0123456789abcdef","lastUsed":"2026-11-14T09:30:00Z","tags":["agent"],"text":"Line 1\nLine 2 {cursor}","title":"From Windows","trigger":"winpls","uses":12}],"state":{"builtin.planpls":{"enabled":false,"lastUsed":"","uses":0}},"version":1}
        """#
        try Data(windows.utf8).write(to: dir.appendingPathComponent("quickprompts.json"))
        let s = QuickPromptStore()
        XCTAssertEqual(s.load(folder: dir, teamFile: ""), "")
        let p = try XCTUnwrap(s.find("p0123456789abcdef"))
        XCTAssertEqual(p.hotkey, "Ctrl+Alt+1")
        XCTAssertEqual(p.uses, 12)
        XCTAssertEqual(p.lastUsed, "2026-11-14T09:30:00Z")
        XCTAssertEqual(s.find("builtin.planpls")?.enabled, false)
        s.noteUsed(p.id)
        let root = try XCTUnwrap(JSONSerialization.jsonObject(with: Data(contentsOf: dir.appendingPathComponent("quickprompts.json"))) as? [String: Any])
        XCTAssertEqual(root["version"] as? Int, 1)
        let saved = try XCTUnwrap((root["prompts"] as? [[String: Any]])?.first)
        XCTAssertEqual(Set(saved.keys), ["id", "trigger", "title", "text", "enabled", "caseSensitive", "hotkey", "tags", "uses", "lastUsed"])
        XCTAssertEqual(saved["uses"] as? Int, 13)
        XCTAssertEqual((root["state"] as? [String: Any])?["builtin.planpls"].map { ($0 as? [String: Any])?["enabled"] as? Bool }, false)
    }

    // quickprompts_import_export
    func testImportExport() throws {
        let csv = QuickPrompts.parseCsv("trigger,text,title\r\nsig,\"Best,\r\n\"\"Tin\"\"\",Signature\naddr,Hà Nội\n")
        XCTAssertEqual(csv.count, 2)
        XCTAssertEqual(csv.first?.trigger, "sig")
        XCTAssertEqual(csv.first?.text, "Best,\r\n\"Tin\"")
        XCTAssertEqual(csv.first?.title, "Signature")
        XCTAssertEqual(csv.last?.text, "Hà Nội")
        let pl = QuickPrompts.parseApplePlist(Data(
            "<?xml version=\"1.0\"?><plist><array><dict><key>phrase</key><string>On my way &amp; soon &#x1F600;</string><key>shortcut</key><string>omw</string></dict><dict><key>shortcut</key><string>x</string></dict></array></plist>".utf8))
        XCTAssertEqual(pl.count, 1)
        XCTAssertEqual(pl.first?.trigger, "omw")
        XCTAssertEqual(pl.first?.text, "On my way & soon 😀")
        // The binary or XML plist macOS itself writes.
        let real = try PropertyListSerialization.data(fromPropertyList: [["phrase": "Ather Screenshot", "shortcut": "ather"]], format: .binary, options: 0)
        XCTAssertEqual(QuickPrompts.parseApplePlist(real).first?.text, "Ather Screenshot")

        let dir = tempDir()
        let s = QuickPromptStore()
        s.load(folder: dir, teamFile: "")
        try (Data([0xEF, 0xBB, 0xBF]) + Data("trigger,text\nnewone,Hello\ndoitforme,taken\na,too short\n".utf8)).write(to: dir.appendingPathComponent("in.csv"))
        let r = s.importFile(dir.appendingPathComponent("in.csv"))
        XCTAssertEqual(r.added, 1)
        XCTAssertEqual(r.skipped.count, 2)
        let ids = s.all.filter { $0.source == "user" || $0.id == "builtin.bugpls" }.map(\.id)
        XCTAssertTrue(s.exportJson(to: dir.appendingPathComponent("team.json"), ids: ids))
        let t = QuickPromptStore()
        t.load(folder: tempDir(), teamFile: dir.appendingPathComponent("team.json").path)
        XCTAssertEqual(t.teamError, "")
        XCTAssertEqual(t.all.filter { $0.source == "team" }.count, 2)
        XCTAssertTrue(s.exportCsv(to: dir.appendingPathComponent("out.csv"), ids: ids))
        let back = QuickPrompts.parseCsv(QuickPrompts.decodeText(try Data(contentsOf: dir.appendingPathComponent("out.csv"))))
        XCTAssertEqual(back.count, 2)
    }

    // quickprompts_try_it_box
    func testTryItBox() {
        let active = [QuickPrompt(trigger: "sig", text: "Best,\nTin{cursor}"), QuickPrompt(trigger: "bugpls", text: "Bug:{lastcapture:image}")]
        let ctx = QuickPrompts.Context()
        let r = QuickPrompts.tryExpand("hello sig ", active: active, ctx)
        XCTAssertEqual(r?.text, "hello Best,\nTin")
        XCTAssertEqual(r?.caret, r?.text.count)
        XCTAssertNil(QuickPrompts.tryExpand("hello sig", active: active, ctx))
        XCTAssertNil(QuickPrompts.tryExpand("design ", active: active, ctx))
        XCTAssertEqual(QuickPrompts.tryExpand("bugpls ", active: active, ctx)?.text, "Bug: [last screenshot]")
    }

    // quickprompts_exclusions_and_paste_keys, the Mac way: bundle ids and app names.
    func testExcludedApps() {
        XCTAssertTrue(QuickPromptsMac.isExcluded(bundleID: "com.agilebits.onepassword7", name: "1Password 7", list: "com.apple.Terminal; com.agilebits.onepassword7"))
        XCTAssertTrue(QuickPromptsMac.isExcluded(bundleID: "org.keepassxc.keepassxc", name: "KeePassXC", list: "keepassxc"))
        XCTAssertTrue(QuickPromptsMac.isExcluded(bundleID: "org.keepassxc.keepassxc", name: "KeePassXC", list: "KeePassXC.app"))
        XCTAssertTrue(QuickPromptsMac.isExcluded(bundleID: "x", name: "KeePass", list: "KeePass.exe"))  // a list shared with Windows
        XCTAssertFalse(QuickPromptsMac.isExcluded(bundleID: "com.apple.TextEdit", name: "TextEdit", list: "com.apple.Text;edit;"))
        XCTAssertFalse(QuickPromptsMac.isExcluded(bundleID: "", name: "", list: "x"))
    }

    // quickprompts_clipboard_round_trip_keeps_formats: what a paste leaves behind is the pasteboard as it was, text
    // and picture both; the temporary item is marked so clipboard managers skip it.
    func testPasteboardRoundTripKeepsFormats() throws {
        let pb = NSPasteboard(name: NSPasteboard.Name("ather-qp-test-\(UUID().uuidString)"))
        defer { pb.releaseGlobally() }
        let ctx = makeContext(width: 8, height: 6)!
        ctx.setFillColor(NSColor.systemTeal.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: 8, height: 6))
        let img = ctx.makeImage()!
        pb.clearContents()
        let item = NSPasteboardItem()
        item.setString("before", forType: .string)
        item.setData(img.pngData()!, forType: .png)
        pb.writeObjects([item])
        let saved = QuickPromptsMac.savePasteboard(pb)
        XCTAssertTrue(saved.complete)
        XCTAssertTrue(QuickPromptsMac.putText("temporary", on: pb))
        XCTAssertEqual(pb.string(forType: .string), "temporary")
        XCTAssertTrue(pb.types?.contains(QuickPromptsMac.transientType) ?? false)
        XCTAssertTrue(pb.types?.contains(QuickPromptsMac.concealedType) ?? false)
        QuickPromptsMac.restorePasteboard(saved, on: pb)
        XCTAssertEqual(pb.string(forType: .string), "before")
        XCTAssertNotNil(pb.data(forType: .png))
        XCTAssertFalse(pb.types?.contains(QuickPromptsMac.concealedType) ?? true)
    }

    // A7: the typed characters never reach a file, a reply or the log. The matcher keeps them only in memory and
    // the store never sees them: after typing a marker, nothing in the support folder mentions it.
    func testTypedTextIsNeverStored() throws {
        let dir = tempDir()
        let s = QuickPromptStore()
        s.load(folder: dir, teamFile: "")
        var m = TriggerMatcher()
        m.setTriggers(s.active.enumerated().map { .init(trigger: $1.trigger, caseSensitive: $1.caseSensitive, tag: $0) })
        let hits = m.feed("secret-marker-7f3a doitforme ")
        XCTAssertEqual(hits.count, 1)
        s.noteUsed(s.active[hits[0].tag].id)
        for f in try FileManager.default.contentsOfDirectory(atPath: dir.path) {
            let text = (try? String(contentsOf: dir.appendingPathComponent(f), encoding: .utf8)) ?? ""
            XCTAssertFalse(text.contains("secret-marker"), f)
        }
        let reply = Agent.listQuickPrompts([:], store: s)
        XCTAssertFalse(MCP.encode(reply).contains("secret-marker"))
    }

    // list_quick_prompts: the same fields as Windows, filters by query, skips turned-off prompts unless asked.
    func testListQuickPromptsTool() throws {
        let s = QuickPromptStore()
        s.load(folder: tempDir(), teamFile: "")
        s.setEnabled("builtin.planpls", false)
        func prompts(_ a: [String: Any]) -> [[String: Any]] {
            ((Agent.listQuickPrompts(a, store: s)["structuredContent"] as? [String: Any])?["prompts"] as? [[String: Any]]) ?? []
        }
        let all = prompts([:])
        XCTAssertEqual(all.count, QuickPrompts.builtins.count - 1)
        XCTAssertEqual(Set(all[0].keys), ["trigger", "title", "text", "source", "enabled", "tags"])
        XCTAssertEqual(prompts(["include_disabled": true]).count, QuickPrompts.builtins.count)
        XCTAssertEqual(prompts(["query": "DOITFOR"]).map { $0["trigger"] as? String }, ["doitforme"])
        XCTAssertEqual(prompts(["query": "doitforme"]).first?["source"] as? String, "builtin")
    }
}
