import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

// The QuickPrompts window (palette › "QuickPrompts…"): the switch, the list in three groups (team starters, team,
// mine), the editor (saves as you type) and a Try it box. Also the one-time welcome card and the rows of
// Settings › QuickPrompts. The Windows window is src/quickpromptsui.cpp.

private let chips = ["{cursor}", "{clipboard}", "{date}", "{lastcapture}", "{lastcapture:image}"]

private func c(_ n: NSColor) -> Color { Color(nsColor: n) }

final class QuickPromptsModel: ObservableObject {
    struct Row: Identifiable {
        var header: String?
        var prompt = QuickPrompt()
        var id: String { header.map { "#" + $0 } ?? prompt.id }
    }
    let store: QuickPromptStore
    @Published var query = "" { didSet { rebuild() } }
    @Published private(set) var rows: [Row] = []
    @Published private(set) var selected: QuickPrompt?
    @Published var trigger = "" { didSet { edited() } }
    @Published var title = "" { didSet { edited() } }
    @Published var text = "" { didSet { edited() } }
    @Published var hotkey = "" { didSet { edited() } }
    @Published var tags = "" { didSet { edited() } }
    @Published var status = ""
    @Published var statusError = false
    @Published var tryText = ""
    @Published var focusTrigger = 0  // bumps to put the caret in the trigger field
    var typing: () -> Bool = { Settings.shared.bool("ExpandAsYouType") }
    var setTyping: (Bool) -> Void = { QuickPromptsMac.shared.setTyping($0) }
    var pickerHotkey: () -> String = { Hotkey.display(Settings.shared.hotkey(.insertQuickPrompt)) }
    var hotkeyStatus: (String) -> String? = { QuickPromptsMac.shared.hotkeyStatus[$0] }
    var isSystemReplacement: (String) -> Bool = { QuickPromptsMac.shared.systemReplacements.contains($0.lowercased()) }
    private var loading = false
    private var saveWork: DispatchWorkItem?
    private var observer: NSObjectProtocol?
    private var seenVersion: UInt64 = 0

    init(store: QuickPromptStore = .shared) {
        self.store = store
        rebuild()
        observer = NotificationCenter.default.addObserver(forName: QuickPromptStore.changed, object: store, queue: .main) { [weak self] _ in
            self?.refresh()
        }
    }
    deinit { observer.map(NotificationCenter.default.removeObserver) }

    var editable: Bool { selected.map { !$0.readOnly } ?? false }

    func rebuild() {
        let words = query.lowercased().split(separator: " ").map(String.init)
        var out: [Row] = []
        let all = store.all
        for (source, label) in [("builtin", "Team starters"), ("team", "Team"), ("user", "My prompts")] {
            let list = all.filter { p in
                guard p.source == source else { return false }
                let hay = "\(p.trigger) \(p.title) \(p.text) \(p.tags.joined(separator: " "))".lowercased()
                return words.allSatisfy { hay.contains($0) }
            }
            if list.isEmpty && (source != "user" || !words.isEmpty) { continue }
            out.append(Row(header: label))
            out += list.map { Row(prompt: $0) }
        }
        rows = out
        seenVersion = store.version
    }

    // Another window or the team file changed the store: show it, without losing what's being typed.
    private func refresh() {
        guard store.version != seenVersion else { return }
        rebuild()
        if let id = selected?.id {
            if let p = store.find(id) { selected = p } else { select(nil) }
        }
    }

    func select(_ id: String?) {
        saveNow()
        saveWork?.cancel()
        status = ""
        statusError = false
        let p = id.flatMap(store.find)
        selected = p
        loading = true
        trigger = p?.trigger ?? ""
        title = p?.title ?? ""
        text = p?.text ?? ""
        hotkey = p?.hotkey ?? ""
        tags = p?.tags.joined(separator: " ") ?? ""
        loading = false
        if let p { updateStatus(p) }
        showStoreError()
    }

    // While quickprompts.json can't be read nothing is saved: say so instead of "Saved".
    private func showStoreError(_ err: String? = nil) {
        let e = err ?? store.userError
        guard !e.isEmpty else { return }
        status = e
        statusError = true
    }

    private func edited() {
        guard !loading, editable else { return }
        saveWork?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.saveNow() }
        saveWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.45, execute: w)
    }

    func updateStatus(_ p: QuickPrompt) {
        guard !statusError else { return }
        var notes: [String] = []
        if let u = QuickPrompts.unknownPlaceholders(p.text).first { notes.append("Not a placeholder (left as typed): {\(u)}") }
        if p.enabled, let owner = store.triggerOwner(p.trigger, except: p.id), let o = store.find(owner) {
            let mineWins = p.source == "user" || (p.source == "team" && o.source == "builtin")
            notes.append("“\(p.trigger)” is also the trigger of “\(o.name)”" + (mineWins ? "; this one wins." : "; that one wins."))
        }
        if !p.hotkey.isEmpty, let hs = hotkeyStatus(p.id) { notes.append(hs) }
        if p.enabled && isSystemReplacement(p.trigger) {
            notes.append("“\(p.trigger)” is also one of your macOS Text Replacements, which wins as you type; the picker still pastes this one.")
        }
        status = notes.joined(separator: "   ·   ")
    }

    func saveNow() {
        saveWork?.cancel()
        saveWork = nil
        guard let cur = selected, !cur.readOnly, var p = store.find(cur.id), !p.readOnly else { return }
        let before = p
        p.trigger = trigger
        p.title = title
        p.text = text.replacingOccurrences(of: "\r", with: "")
        p.tags = tags.split(whereSeparator: { $0 == " " || $0 == "," }).map(String.init)
        let hk = hotkey.trimmingCharacters(in: .whitespaces)
        var hkError = ""
        if hk.isEmpty { p.hotkey = "" }
        else if let parsed = Hotkey.parse(hk), !parsed.text.isEmpty { p.hotkey = parsed.text }
        else { hkError = "“\(hk)” isn’t a shortcut macOS accepts (try Ctrl+Alt+1)." }
        guard p != before || !hkError.isEmpty else { return }
        let err = store.update(p)
        statusError = !err.isEmpty || !hkError.isEmpty
        status = !err.isEmpty ? err : hkError
        if err.isEmpty, let saved = store.find(p.id) {
            selected = saved
            rebuild()
            if !statusError { updateStatus(saved) }
            if status.isEmpty { status = "Saved" }
        }
    }

    func new() {
        saveNow()
        var p = QuickPrompt()
        p.trigger = store.uniqueTrigger("newprompt")
        p.title = "New prompt"
        let r = store.add(p)
        guard r.error.isEmpty else { return showStoreError(r.error) }
        query = ""
        select(r.id)
        focusTrigger += 1
    }

    func duplicate() {
        saveNow()
        guard let cur = selected, var p = store.find(cur.id) else { return }
        p.id = ""
        p.source = "user"
        p.trigger = store.uniqueTrigger(p.trigger + "2")
        p.title = p.name + " (copy)"
        p.uses = 0
        p.lastUsed = ""
        p.hotkey = ""
        p.enabled = true
        let r = store.add(p)
        guard r.error.isEmpty else { return showStoreError(r.error) }
        rebuild()
        select(r.id)
        focusTrigger += 1
    }

    func delete(confirm: (QuickPrompt) -> Bool) {
        guard let p = selected, !p.readOnly, confirm(p) else { return }
        let e = store.remove(p.id)
        guard e.isEmpty else { return showStoreError(e) }
        selected = nil
        select(nil)
        rebuild()
    }

    func toggleEnabled() {
        guard let p = selected else { return }
        saveNow()
        let e = store.setEnabled(p.id, !p.enabled)
        rebuild()
        selected = store.find(p.id)
        if !e.isEmpty { return showStoreError(e) }
        if let s = selected { statusError = false; updateStatus(s) }
    }

    func insertChip(_ chip: String, into tv: NSTextView?) {
        guard editable else { return }
        if let tv, tv.window != nil {
            tv.window?.makeFirstResponder(tv)
            tv.insertText(chip, replacementRange: tv.selectedRange())
        } else {
            text += chip
        }
    }

    func importFile(_ u: URL) {
        saveNow()
        let r = store.importFile(u)
        statusError = !r.error.isEmpty
        status = !r.error.isEmpty ? r.error : "Imported \(r.added) " + (r.added == 1 ? "prompt" : "prompts")
        if !r.skipped.isEmpty {
            status += "; skipped \(r.skipped.count) whose trigger is taken or too short: " + r.skipped.prefix(5).joined(separator: ", ") + (r.skipped.count > 5 ? "…" : "")
        }
        rebuild()
    }

    // which: 1 my prompts as a team file, 2 my prompts as CSV, 3 every prompt shown as a team file.
    func exportIds(_ which: Int) -> [String] {
        rows.filter { $0.header == nil && (which == 3 || $0.prompt.source == "user") }.map(\.prompt.id)
    }

    // Try it: typing a trigger and Space expands it right here.
    func tryChanged(_ t: String) -> (String, Int)? {
        let ctx = QuickPrompts.Context.current(clipboard: NSPasteboard.general.string(forType: .string) ?? "",
                                               lastCapture: QuickPromptsMac.lastCapture()?.path ?? "")
        guard let r = QuickPrompts.tryExpand(t, active: store.active, ctx) else { return nil }
        // A one-line box: line breaks show as ⏎.
        var flat = "", caret = 0
        for (i, ch) in r.text.enumerated() {
            if i == r.caret { caret = flat.count }
            flat += ch == "\n" ? " ⏎ " : String(ch)
        }
        if r.caret >= r.text.count { caret = flat.count }
        return (flat, caret)
    }
}

struct QuickPromptsView: View {
    @ObservedObject var m: QuickPromptsModel
    @ObservedObject private var settings = Settings.shared
    @ObservedObject private var perm = QuickPromptsStatus.shared
    @State private var textView: NSTextView?
    @FocusState private var triggerFocused: Bool

    var body: some View {
        VStack(spacing: 0) {
            header
            Rectangle().fill(c(Theme.border)).frame(height: 1)
            HStack(spacing: 0) {
                list.frame(width: 340)
                Rectangle().fill(c(Theme.border)).frame(width: 1)
                editor.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }
        }
        .background(c(Theme.bg))
        .foregroundColor(c(Theme.text))
        .frame(minWidth: 900, minHeight: 600)
        .onChange(of: m.focusTrigger) { DispatchQueue.main.async { triggerFocused = true } }
    }

    private var header: some View {
        let on = m.typing()
        let picker = m.pickerHotkey()
        return HStack(alignment: .center, spacing: 16) {
            VStack(alignment: .leading, spacing: 4) {
                Text("QUICKPROMPTS").font(.system(size: 22, weight: .heavy)).tracking(1.5)
                Text(on ? "Type a trigger, then Space, in any app" + (picker.isEmpty ? "" : "  ·  \(picker) lists them all")
                        : "Off: triggers stay as typed" + (picker.isEmpty ? "" : "  ·  \(picker) still lists every prompt"))
                    .font(.system(size: 12)).foregroundColor(c(Theme.muted))
            }
            Spacer()
            if on && !perm.accessibility {
                HStack(spacing: 8) {
                    Text("Needs Accessibility to spot triggers and paste").font(.system(size: 12)).foregroundColor(.orange)
                    Button("Allow…") { QuickPromptsMac.askForAccessibility() }
                }
            }
            Toggle("Expand triggers as I type", isOn: Binding(get: { m.typing() }, set: { m.setTyping($0); settings.objectWillChange.send() }))
                .toggleStyle(.switch).tint(c(Theme.accent)).font(.system(size: 13, weight: .medium))
        }
        .padding(.horizontal, 20).padding(.vertical, 14)
        .background(c(Theme.surface))
    }

    private var list: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass").foregroundColor(c(Theme.muted))
                TextField("Search prompts", text: $m.query).textFieldStyle(.plain).font(.system(size: 13))
            }
            .padding(.horizontal, 10).padding(.vertical, 7)
            .background(RoundedRectangle(cornerRadius: 7).fill(c(Theme.raised)))
            .padding(12)
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 2) {
                        ForEach(m.rows) { row in
                            if let h = row.header {
                                Text(h.uppercased()).font(.system(size: 10, weight: .bold)).tracking(1).foregroundColor(c(Theme.muted))
                                    .padding(.horizontal, 8).padding(.top, 12).padding(.bottom, 4)
                            } else {
                                promptRow(row.prompt).id(row.id)
                            }
                        }
                        if !m.rows.contains(where: { $0.header == nil }) && !m.query.isEmpty {
                            Text("No prompt matches “\(m.query)”.").font(.system(size: 12)).foregroundColor(c(Theme.muted)).padding(8)
                        }
                    }
                    .padding(.horizontal, 8).padding(.bottom, 8)
                }
                .onAppear { if let id = m.selected?.id { proxy.scrollTo(id, anchor: .center) } }
                .onChange(of: m.selected?.id) { _, id in if let id { withAnimation { proxy.scrollTo(id) } } }
            }
            Rectangle().fill(c(Theme.border)).frame(height: 1)
            HStack(spacing: 6) {
                Button("New") { m.new() }
                Button("Duplicate") { m.duplicate() }.disabled(m.selected == nil)
                Button("Delete") { m.delete(confirm: confirmDelete) }.disabled(!m.editable)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12).padding(.top, 10)
            HStack(spacing: 6) {
                Button("Import…") { importPanel() }
                Menu("Export…") {
                    Button("My prompts as a team file (JSON)…") { export(1) }
                    Button("My prompts as CSV…") { export(2) }
                    Button("Every prompt shown, as a team file (JSON)…") { export(3) }
                }.fixedSize()
                Menu("Team file…") {
                    let cur = settings.string("TeamFile")
                    if !cur.isEmpty { Text("Now: " + cur) }
                    Button("Choose the team prompts file…") { chooseTeam() }
                    if !cur.isEmpty { Button("Stop using the team file") { setTeam("") } }
                }.fixedSize()
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12).padding(.top, 6).padding(.bottom, 12)
        }
        .background(c(Theme.surface))
    }

    private func promptRow(_ p: QuickPrompt) -> some View {
        let sel = m.selected?.id == p.id
        let first = p.text.split(separator: "\n", omittingEmptySubsequences: true).first.map(String.init) ?? ""
        return HStack(alignment: .top, spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(p.trigger).font(.system(size: 13, weight: .semibold, design: .monospaced))
                        .foregroundColor(p.enabled ? c(Theme.accent) : c(Theme.muted))
                    Text(p.title).font(.system(size: 13)).foregroundColor(p.enabled ? c(Theme.text) : c(Theme.muted)).lineLimit(1)
                }
                Text(first).font(.system(size: 11)).foregroundColor(c(Theme.muted)).lineLimit(1)
            }
            Spacer(minLength: 4)
            if !p.enabled {
                Text("Off").font(.system(size: 10, weight: .semibold)).foregroundColor(c(Theme.muted))
                    .padding(.horizontal, 6).padding(.vertical, 2).background(Capsule().fill(c(Theme.raised)))
            } else if p.uses > 0 {
                Text("\(p.uses)×").font(.system(size: 10, weight: .medium)).foregroundColor(c(Theme.muted)).monospacedDigit()
            }
        }
        .padding(.horizontal, 10).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 8).fill(sel ? c(Theme.selected) : Color.clear))
        .overlay(alignment: .leading) {
            if sel { RoundedRectangle(cornerRadius: 1.5).fill(c(Theme.accent)).frame(width: 3, height: 18).padding(.leading, 3) }
        }
        .contentShape(Rectangle())
        .onTapGesture { m.select(p.id) }
    }

    @ViewBuilder private var editor: some View {
        if let p = m.selected {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    HStack(spacing: 10) {
                        let kind = p.source == "builtin" ? "TEAM STARTER" : p.source == "team" ? "TEAM" : "MY PROMPT"
                        Text(kind).font(.system(size: 10, weight: .bold)).tracking(1)
                            .foregroundColor(p.source == "user" ? c(Theme.accent) : c(Theme.textDim))
                            .padding(.horizontal, 8).padding(.vertical, 3)
                            .background(Capsule().fill(p.source == "user" ? c(Theme.selected) : c(Theme.raised)))
                            .overlay(Capsule().stroke(c(Theme.border)))
                        Text((p.source == "builtin" ? "Built into the app. Duplicate it to change the text."
                              : p.source == "team" ? "From the team file. Duplicate it to change the text." : "Saved as you type.")
                             + (p.uses > 0 ? "  ·  Used \(p.uses) " + (p.uses == 1 ? "time" : "times") : ""))
                            .font(.system(size: 12)).foregroundColor(c(Theme.muted))
                        Spacer()
                        Toggle(p.enabled ? "On" : "Off", isOn: Binding(get: { p.enabled }, set: { _ in m.toggleEnabled() }))
                            .toggleStyle(.switch).controlSize(.small).tint(c(Theme.accent)).font(.system(size: 12))
                    }
                    HStack(alignment: .top, spacing: 12) {
                        field("Trigger") {
                            TextField("doitforme", text: $m.trigger).font(.system(size: 13, design: .monospaced))
                                .focused($triggerFocused).disabled(!m.editable)
                        }.frame(width: 200)
                        field("Title") { TextField("What it's for", text: $m.title).disabled(!m.editable) }
                    }
                    field("Prompt") {
                        PromptTextView(text: $m.text, editable: m.editable) { textView = $0 }
                            .frame(minHeight: 220)
                            .background(RoundedRectangle(cornerRadius: 6).fill(c(Theme.raised)))
                            .overlay(RoundedRectangle(cornerRadius: 6).stroke(c(Theme.border)))
                    }
                    if m.editable {
                        HStack(spacing: 6) {
                            ForEach(chips, id: \.self) { chip in
                                Button(chip) { m.insertChip(chip, into: textView) }
                                    .buttonStyle(.plain).font(.system(size: 11, design: .monospaced)).foregroundColor(c(Theme.textDim))
                                    .padding(.horizontal, 8).padding(.vertical, 4)
                                    .background(RoundedRectangle(cornerRadius: 5).fill(c(Theme.raised)))
                                    .overlay(RoundedRectangle(cornerRadius: 5).stroke(c(Theme.border)))
                            }
                        }
                    }
                    HStack(alignment: .top, spacing: 12) {
                        field("Shortcut") { TextField("e.g. Ctrl+Alt+1", text: $m.hotkey).disabled(!m.editable) }.frame(width: 200)
                        field("Tags") { TextField("agent review", text: $m.tags).disabled(!m.editable) }
                    }
                    if !m.status.isEmpty {
                        Text(m.status).font(.system(size: 12)).foregroundColor(m.statusError ? .orange : c(Theme.muted)).fixedSize(horizontal: false, vertical: true)
                    }
                    Rectangle().fill(c(Theme.border)).frame(height: 1).padding(.vertical, 4)
                    field("Try it") {
                        TryItField(text: $m.tryText, placeholder: "Type a trigger and Space, e.g. \(p.trigger) ", expand: m.tryChanged)
                            .frame(height: 24)
                    }
                    Text("Your typing here isn't expanded by the app, so you can try prompts without leaving this window.")
                        .font(.system(size: 11)).foregroundColor(c(Theme.muted))
                }
                .padding(20)
            }
            .textFieldStyle(.roundedBorder)
        } else {
            VStack(alignment: .leading, spacing: 16) {
                Text("Pick a prompt on the left, or New to write one.").font(.system(size: 13)).foregroundColor(c(Theme.muted))
                field("Try it") {
                    TryItField(text: $m.tryText, placeholder: "Type a trigger and Space, e.g. doitforme ", expand: m.tryChanged).frame(height: 24)
                }
                if !m.status.isEmpty { Text(m.status).font(.system(size: 12)).foregroundColor(m.statusError ? .orange : c(Theme.muted)) }
            }
            .padding(20)
        }
    }

    private func field<V: View>(_ label: String, @ViewBuilder _ content: () -> V) -> some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(label.uppercased()).font(.system(size: 10, weight: .bold)).tracking(0.8).foregroundColor(c(Theme.muted))
            content()
        }
    }

    private func confirmDelete(_ p: QuickPrompt) -> Bool {
        let a = NSAlert()
        a.messageText = "Delete “\(p.name)”?"
        a.informativeText = "This can't be undone."
        a.alertStyle = .warning
        a.addButton(withTitle: "Delete")
        a.addButton(withTitle: "Cancel")
        return a.runModal() == .alertFirstButtonReturn
    }

    private func importPanel() {
        let p = NSOpenPanel()
        p.title = "Import prompts (QuickPrompts JSON, CSV, or macOS Text Replacements)"
        p.allowedContentTypes = [.json, .commaSeparatedText, .propertyList, UTType(filenameExtension: "plist") ?? .propertyList]
        p.allowsMultipleSelection = false
        if p.runModal() == .OK, let u = p.url { m.importFile(u) }
    }

    private func export(_ which: Int) {
        m.saveNow()
        let ids = m.exportIds(which)
        guard !ids.isEmpty else {
            m.statusError = false
            m.status = "Nothing to export: there are no prompts of your own yet."
            return
        }
        let p = NSSavePanel()
        p.nameFieldStringValue = which == 2 ? "quickprompts.csv" : "quickprompts-team.json"
        p.allowedContentTypes = [which == 2 ? .commaSeparatedText : .json]
        guard p.runModal() == .OK, let u = p.url else { return }
        let ok = which == 2 ? m.store.exportCsv(to: u, ids: ids) : m.store.exportJson(to: u, ids: ids)
        m.statusError = !ok
        m.status = ok ? "Exported \(ids.count) to \(u.path)" : "Couldn't write \(u.path)"
    }

    private func chooseTeam() {
        let p = NSOpenPanel()
        p.title = "Choose the shared quickprompts-team.json"
        p.allowedContentTypes = [.json]
        if p.runModal() == .OK, let u = p.url { setTeam(u.path) }
    }

    private func setTeam(_ path: String) {
        settings.set("TeamFile", path)  // onChange gives it to the store
        m.store.setTeamFile(path)
        let e = m.store.teamError
        m.statusError = !e.isEmpty
        m.status = !e.isEmpty ? e : path.isEmpty ? "Team file removed" : "Team prompts from " + path
        m.rebuild()
    }
}

// The prompt's text: monospaced, many lines, and the chips insert at the caret.
private struct PromptTextView: NSViewRepresentable {
    @Binding var text: String
    var editable: Bool
    var onMake: (NSTextView) -> Void

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        scroll.drawsBackground = false
        scroll.borderType = .noBorder
        let tv = scroll.documentView as! NSTextView
        tv.delegate = context.coordinator
        tv.font = Theme.mono(13, .regular)
        tv.textColor = Theme.text
        tv.insertionPointColor = Theme.accent
        tv.drawsBackground = false
        tv.isRichText = false
        tv.allowsUndo = true
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.isAutomaticTextReplacementEnabled = false
        tv.isAutomaticSpellingCorrectionEnabled = false
        tv.textContainerInset = NSSize(width: 6, height: 8)
        tv.string = text
        DispatchQueue.main.async { onMake(tv) }
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let tv = scroll.documentView as? NSTextView else { return }
        context.coordinator.parent = self
        tv.isEditable = editable
        tv.textColor = editable ? Theme.text : Theme.textDim
        if tv.string != text { tv.string = text }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: PromptTextView
        init(_ p: PromptTextView) { parent = p }
        func textDidChange(_ n: Notification) {
            guard let tv = n.object as? NSTextView else { return }
            parent.text = tv.string
        }
    }
}

// One line where typing a trigger and Space expands it in place, caret where {cursor} is.
private struct TryItField: NSViewRepresentable {
    @Binding var text: String
    var placeholder: String
    var expand: (String) -> (String, Int)?

    func makeNSView(context: Context) -> NSTextField {
        let f = NSTextField()
        f.delegate = context.coordinator
        f.font = Theme.font(13)
        f.textColor = Theme.text
        f.backgroundColor = Theme.raised
        f.isBezeled = true
        f.bezelStyle = .roundedBezel
        f.focusRingType = .none
        f.cell?.isScrollable = true
        f.cell?.wraps = false
        return f
    }

    func updateNSView(_ f: NSTextField, context: Context) {
        context.coordinator.parent = self
        f.placeholderAttributedString = NSAttributedString(string: placeholder, attributes: [.foregroundColor: Theme.muted, .font: Theme.font(13)])
        if f.stringValue != text { f.stringValue = text }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }
    final class Coordinator: NSObject, NSTextFieldDelegate {
        var parent: TryItField
        init(_ p: TryItField) { parent = p }
        func controlTextDidChange(_ n: Notification) {
            guard let f = n.object as? NSTextField else { return }
            if let (out, caret) = parent.expand(f.stringValue) {
                f.stringValue = out
                (f.currentEditor() as? NSTextView)?.setSelectedRange(NSRange(location: (out.prefix(caret) as Substring).utf16.count, length: 0))
            }
            parent.text = f.stringValue
        }
    }
}

final class QuickPromptsWindow: NSObject, NSWindowDelegate {
    static var shared: QuickPromptsWindow?
    let window: NSWindow
    let model: QuickPromptsModel

    static func show(select: String? = nil, focusTrigger: Bool = false) {
        let w = shared ?? QuickPromptsWindow()
        shared = w
        if let select { w.model.select(select) }
        if focusTrigger { w.model.focusTrigger += 1 }
        activateApp()
        w.window.makeKeyAndOrderFront(nil)
    }

    init(model: QuickPromptsModel = QuickPromptsModel(), register: Bool = true) {
        self.model = model
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 680), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = "QuickPrompts — Ather Screenshot"
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentView = NSHostingView(rootView: QuickPromptsView(m: model))
        window.setFrameAutosaveName("QuickPromptsWindow")
        if register {
            window.center()
            AppDelegate.shared?.windowOpened(window)
        }
    }

    func windowWillClose(_ notification: Notification) {
        model.saveNow()
        if QuickPromptsWindow.shared === self { QuickPromptsWindow.shared = nil }
    }
}

// MARK: - the welcome card (once, at the first start with QuickPrompts)

struct QuickPromptsWelcomeView: View {
    var picker: String
    var done: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("NEW IN ATHER SCREENSHOT").font(.system(size: 11, weight: .bold)).tracking(2).foregroundColor(c(Theme.accent))
            Text("QUICKPROMPTS").font(.system(size: 38, weight: .heavy)).tracking(1).padding(.top, 6)
            Text("Type a short trigger and press Space, in any app, and it turns into a saved prompt or phrase.")
                .font(.system(size: 15)).foregroundColor(c(Theme.textDim)).fixedSize(horizontal: false, vertical: true).padding(.top, 8)
            VStack(spacing: 6) {
                example("doitforme", "Asks an AI agent to do the task end to end and report back")
                example("reviewpls", "Asks for a review of the current change")
                example("bugpls", "Turns your last screenshot into a bug report, picture attached")
            }
            .padding(.top, 18)
            Text("To spot triggers, Ather Screenshot now listens to what you type. It keeps only the last few characters, in memory: never saved, never logged, never shared with AI agents. It's on now; turn it off any time in QuickPrompts or Settings.")
                .font(.system(size: 13)).foregroundColor(c(Theme.muted)).fixedSize(horizontal: false, vertical: true).padding(.top, 18)
            Text("macOS asks once for Accessibility, which lets it see the keys you press and paste the prompt. It sees nothing in password fields.")
                .font(.system(size: 13)).foregroundColor(c(Theme.muted)).fixedSize(horizontal: false, vertical: true).padding(.top, 8)
            if !picker.isEmpty {
                Text("\(picker) lists every prompt, so you don't have to remember the triggers.")
                    .font(.system(size: 13)).foregroundColor(c(Theme.muted)).padding(.top, 8)
            }
            Spacer(minLength: 20)
            HStack(spacing: 12) {
                Spacer()
                Button { done(true) } label: {
                    Text("Open QuickPrompts").font(.system(size: 14, weight: .semibold)).foregroundColor(c(Theme.text))
                        .frame(width: 190, height: 38)
                        .background(RoundedRectangle(cornerRadius: 8).fill(c(Theme.raised)))
                        .overlay(RoundedRectangle(cornerRadius: 8).stroke(c(Theme.border)))
                }.buttonStyle(.plain)
                Button { done(false) } label: {
                    Text("Got it").font(.system(size: 14, weight: .semibold)).foregroundColor(c(Theme.onAccent))
                        .frame(width: 140, height: 38)
                        .background(RoundedRectangle(cornerRadius: 8).fill(c(Theme.accent)))
                }.buttonStyle(.plain).keyboardShortcut(.defaultAction)
            }
        }
        .padding(32)
        .frame(width: 600, height: 520, alignment: .topLeading)
        .background(c(Theme.surface))
        .foregroundColor(c(Theme.text))
        .clipShape(RoundedRectangle(cornerRadius: 14))
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(c(Theme.border)))
    }

    private func example(_ t: String, _ d: String) -> some View {
        HStack(spacing: 0) {
            Text(t).font(.system(size: 14, weight: .semibold, design: .monospaced)).foregroundColor(c(Theme.accent)).frame(width: 130, alignment: .leading)
            Text(d).font(.system(size: 13)).foregroundColor(c(Theme.textDim)).lineLimit(1)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 14).frame(height: 36)
        .background(RoundedRectangle(cornerRadius: 8).fill(c(Theme.raised)))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(c(Theme.border)))
    }
}

enum QuickPromptsWelcome {
    private static var panel: KeyablePanel?

    static func makeView(picker: String, done: @escaping (Bool) -> Void) -> NSView {
        let v = NSHostingView(rootView: QuickPromptsWelcomeView(picker: picker, done: done))
        v.frame = NSRect(x: 0, y: 0, width: 600, height: 520)
        return v
    }

    static func show(picker: String, done: @escaping (Bool) -> Void) {
        guard panel == nil else { return }
        let p = roundedPanel(NSRect(x: 0, y: 0, width: 600, height: 520), level: .floating, key: true)
        p.contentView = makeView(picker: picker) { open in
            panel?.orderOut(nil)
            panel = nil
            done(open)
        }
        p.appearance = NSAppearance(named: .darkAqua)
        let vis = (NSScreen.main ?? NSScreen.screens[0]).visibleFrame
        p.setFrameOrigin(NSPoint(x: vis.midX - 300, y: vis.midY - 260))
        panel = p
        activateApp()
        p.makeKeyAndOrderFront(nil)
    }
}

// MARK: - Settings › QuickPrompts

struct QuickPromptsSettingsRow: View {
    let id: String
    @ObservedObject private var st = QuickPromptsStatus.shared
    private let tick = Timer.publish(every: 2, on: .main, in: .common).autoconnect()

    var body: some View {
        switch id {
        case "access":
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Accessibility (to spot triggers and paste)")
                    Text(st.accessibility ? (st.listening ? "Allowed · listening for triggers" : "Allowed")
                                          : "Not allowed yet: typed triggers don't expand, the picker can't paste")
                        .font(.system(size: 11)).foregroundColor(st.accessibility ? c(Theme.muted) : .orange)
                }
                Spacer()
                Button(st.accessibility ? "Open System Settings" : "Allow…") {
                    if st.accessibility {
                        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
                    } else { QuickPromptsMac.askForAccessibility() }
                }
            }
            .onAppear { st.refresh() }
            .onReceive(tick) { _ in st.refresh() }
        default:
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Your prompts and the team's")
                    Text("Triggers, the team starters and the team file").font(.system(size: 11)).foregroundColor(c(Theme.muted))
                }
                Spacer()
                Button("Open QuickPrompts") { QuickPromptsWindow.show() }
            }
        }
    }
}
