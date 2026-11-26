import AVFoundation
import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

// The Meetings window (palette › "Meetings", the menu bar, or the "Meeting saved" notification): every meeting on the
// left, searchable through titles and transcripts; the selected one on the right with its waveform, the player, and
// the transcript as an editor. Click a word to hear it; ⇧-click to select a stretch; ⌫ cuts it (struck through), and
// the player skips cuts, so you hear the edit. Clean up removes filler words, repeated words and long pauses. Export
// writes the edited audio and transcript; transcript.json next to the audio is kept up to date for AI agents.

private func col(_ n: NSColor) -> Color { Color(nsColor: n) }

final class MeetingsModel: ObservableObject {
    struct Word {
        var line: Int
        var index: Int
        var word: TimedWord
        var cut: Bool
    }
    enum Tab: String, CaseIterable { case transcript = "Transcript", summary = "Summary & outputs", notes = "Notes" }

    let store: MeetingStore
    @Published var query = ""
    @Published var selectedID: UUID? { didSet { if oldValue != selectedID { load() } } }
    @Published private(set) var meeting: Meeting?
    @Published private(set) var words: [Word] = []
    @Published private(set) var lineStart: [Int] = []          // index of each line's first word in `words`
    @Published var anchor: Int?
    @Published var focus: Int?
    @Published private(set) var time: Double = 0
    @Published private(set) var playing = false
    @Published var rate: Float = 1 { didSet { if playing { player.rate = rate } } }
    @Published var skipCuts = true
    @Published private(set) var peaks: [Float] = []
    @Published var tab = Tab.transcript
    @Published var status = ""
    @Published var follow = true
    @Published private(set) var canUndo = false
    @Published private(set) var canRedo = false
    @Published var working = ""                                 // a long action in progress ("Exporting…")
    let player = AVPlayer()
    private var observer: Any?
    private var subs: Set<AnyCancellable> = []
    private var undo: [[MeetingCut]] = []
    private var redo: [[MeetingCut]] = []
    private var spans: [MeetingEdit.Span] = []
    private var notesWork: DispatchWorkItem?
    var peaksProvider: (URL) -> [Float] = { Waveform.peaks($0) }

    init(store: MeetingStore = .shared) {
        self.store = store
        store.loadIfNeeded()
        observer = player.addPeriodicTimeObserver(forInterval: CMTime(value: 1, timescale: 30), queue: .main) { [weak self] t in self?.tick(t) }
        NotificationCenter.default.publisher(for: MeetingStore.changed, object: store).receive(on: RunLoop.main).sink { [weak self] n in
            guard let self, let id = n.userInfo?["id"] as? UUID, id == self.selectedID else { return }
            self.refresh()
        }.store(in: &subs)
        NotificationCenter.default.publisher(for: .AVPlayerItemDidPlayToEndTime).receive(on: RunLoop.main).sink { [weak self] _ in
            self?.playing = false
        }.store(in: &subs)
        selectedID = store.meetings.first?.id
        if selectedID != nil { load() }
    }

    deinit { observer.map(player.removeTimeObserver) }

    var list: [Meeting] { store.search(query) }

    // MARK: loading

    private func load() {
        player.pause()
        playing = false
        time = 0
        anchor = nil
        focus = nil
        undo = []; redo = []; syncUndo()
        meeting = selectedID.flatMap(store.meeting)
        rebuild()
        peaks = []
        guard let m = meeting else { player.replaceCurrentItem(with: nil); return }
        player.replaceCurrentItem(with: FileManager.default.fileExists(atPath: m.audioURL.path) ? AVPlayerItem(url: m.audioURL) : nil)
        let url = m.audioURL, id = m.id, provider = peaksProvider
        DispatchQueue.global(qos: .userInitiated).async {
            let p = provider(url)
            DispatchQueue.main.async { if self.selectedID == id { self.peaks = p } }
        }
    }

    // The meeting changed on disk or in the store (transcript arrived, renamed…): keep the player going.
    private func refresh() {
        guard let id = selectedID, let m = store.meeting(id) else { meeting = nil; rebuild(); return }
        let audioChanged = m.audioURL != meeting?.audioURL
        meeting = m
        rebuild()
        if audioChanged { load() }
    }

    func rebuild() {
        var out: [Word] = []
        var starts: [Int] = []
        spans = MeetingEdit.merged(meeting?.cuts ?? [])
        for (li, l) in (meeting?.lines ?? []).enumerated() {
            starts.append(out.count)
            let ws = l.words.isEmpty ? [TimedWord(start: l.start, end: l.end, text: l.text)] : l.words
            for (wi, w) in ws.enumerated() { out.append(Word(line: li, index: wi, word: w, cut: MeetingEdit.isCut(w, spans))) }
        }
        words = out
        lineStart = starts
        if let a = anchor, a >= out.count { anchor = nil; focus = nil }
    }

    func wordRange(line: Int) -> Range<Int> {
        guard line < lineStart.count else { return 0..<0 }
        return lineStart[line]..<(line + 1 < lineStart.count ? lineStart[line + 1] : words.count)
    }

    // MARK: playback

    private func tick(_ t: CMTime) {
        let s = CMTimeGetSeconds(t)
        guard s.isFinite else { return }
        if playing, skipCuts, let end = MeetingEdit.cutEnd(at: s, spans) {
            seek(end, keepPlaying: true)
            return
        }
        time = s
    }

    func togglePlay() {
        guard player.currentItem != nil else { return }
        if playing { player.pause(); playing = false; return }
        if let m = meeting, time >= m.duration - 0.05 { seek(0) }
        if skipCuts, let end = MeetingEdit.cutEnd(at: time, spans) { seek(end) }
        player.rate = rate
        playing = true
    }

    func seek(_ t: Double, keepPlaying: Bool = false) {
        let d = meeting?.duration ?? 0
        let c = max(0, d > 0 ? min(t, d) : t)
        time = c
        player.seek(to: CMTime(seconds: c, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero)
    }

    func skip(_ by: Double) { seek(time + by) }

    var currentLine: Int? {
        guard let m = meeting else { return nil }
        return m.lines.lastIndex { $0.start <= time + 0.05 }.flatMap { time <= m.lines[$0].end + 1.0 ? $0 : nil }
    }
    func isCurrent(_ w: Word) -> Bool { playing || time > 0 ? (time >= w.word.start && time < max(w.word.end, w.word.start + 0.12)) : false }

    // MARK: selection and cuts

    var selection: ClosedRange<Int>? {
        guard let a = anchor, let f = focus, a < words.count, f < words.count else { return nil }
        return min(a, f)...max(a, f)
    }

    func click(_ i: Int, shift: Bool) {
        if shift, anchor != nil { focus = i } else { anchor = i; focus = i }
        seek(words[i].word.start)
    }

    func selectLine(_ li: Int) {
        let r = wordRange(line: li)
        guard !r.isEmpty else { return }
        anchor = r.lowerBound
        focus = r.upperBound - 1
        seek(words[r.lowerBound].word.start)
    }

    var selectionIsCut: Bool { selection.map { r in r.allSatisfy { words[$0].cut } } ?? false }

    private func edit(_ name: String, _ change: (inout Meeting) -> Void) {
        guard var m = meeting else { return }
        undo.append(m.cuts)
        redo = []
        change(&m)
        meeting = m
        store.save(m)
        rebuild()
        syncUndo()
        status = name
    }

    private func syncUndo() { canUndo = !undo.isEmpty; canRedo = !redo.isEmpty }

    // The selected words' audio, from the first's start to the last's end (or to the next word, when the cut reaches
    // the end of a line, so the pause after goes too).
    private func selectedSpan() -> (Double, Double)? {
        guard let r = selection else { return nil }
        let a = words[r.lowerBound].word, b = words[r.upperBound].word
        var end = b.end
        if r.upperBound + 1 < words.count, words[r.upperBound + 1].line == words[r.upperBound].line { end = max(end, min(words[r.upperBound + 1].word.start, b.end + 0.3)) }
        return (a.start, end)
    }

    func deleteSelection() {
        guard let (s, e) = selectedSpan() else { return }
        if selectionIsCut {
            edit("Restored \(MeetingFormat.clock(s))–\(MeetingFormat.clock(e))") { MeetingEdit.restore(&$0, s, e) }
        } else {
            edit("Cut \(String(format: "%.1f", e - s)) s") { MeetingEdit.cut(&$0, s, e) }
            if let r = selection, r.upperBound + 1 < words.count { anchor = r.upperBound + 1; focus = r.upperBound + 1 }
        }
    }

    func restoreAll() { edit("Restored everything") { $0.cuts = [] } }

    func trimStart() { let t = time; edit("Trimmed the start to \(MeetingFormat.clock(t))") { m in m.cuts.removeAll { $0.reason == .trim && $0.start < 0.01 }; MeetingEdit.cut(&m, 0, t, reason: .trim) } }
    func trimEnd() { let t = time; edit("Trimmed the end from \(MeetingFormat.clock(t))") { m in m.cuts.removeAll { $0.reason == .trim && $0.start > 0.01 }; MeetingEdit.cut(&m, t, m.duration, reason: .trim) } }

    func undoCut() {
        guard var m = meeting, let prev = undo.popLast() else { return }
        redo.append(m.cuts)
        m.cuts = prev
        meeting = m
        store.save(m)
        rebuild()
        syncUndo()
        status = "Undone"
    }

    func redoCut() {
        guard var m = meeting, let next = redo.popLast() else { return }
        undo.append(m.cuts)
        m.cuts = next
        meeting = m
        store.save(m)
        rebuild()
        syncUndo()
        status = "Redone"
    }

    func cleanUp(fillers: Bool, repeats: Bool, pauses: Bool) {
        guard let m0 = meeting else { return }
        working = "Cleaning up…"
        let url = m0.audioURL
        DispatchQueue.global(qos: .userInitiated).async {
            let levels = pauses ? MeetingEdit.rms(url) : []
            DispatchQueue.main.async {
                self.working = ""
                var result = MeetingEdit.CleanupResult()
                self.edit("") { m in result = MeetingEdit.cleanUp(&m, fillers: fillers, repeats: repeats, pauses: pauses, rms: levels) }
                var parts: [String] = []
                if fillers { parts.append("\(result.fillers) filler word\(result.fillers == 1 ? "" : "s")") }
                if repeats { parts.append("\(result.repeats) repeat\(result.repeats == 1 ? "" : "s")") }
                if pauses { parts.append("\(result.pauses) long pause\(result.pauses == 1 ? "" : "s")") }
                self.status = "Removed " + parts.joined(separator: ", ") + " · \(String(format: "%.1f", result.saved)) s shorter"
            }
        }
    }

    // MARK: meeting actions

    func rename(_ t: String) { if let id = selectedID { store.rename(id, to: t) } }

    func setNotes(_ text: String) {
        guard var m = meeting, m.notes != text else { return }
        m.notes = text
        meeting = m
        notesWork?.cancel()
        let w = DispatchWorkItem { [weak self] in
            guard let self, let id = self.selectedID else { return }
            self.store.update(id) { $0.notes = text }
        }
        notesWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6, execute: w)
    }

    func setSpeakerName(_ s: Speaker, _ name: String) {
        guard let id = selectedID else { return }
        store.update(id) { $0.names[s.rawValue] = name.trimmingCharacters(in: .whitespaces) }
    }

    func transcribeAgain() { if let id = selectedID { MeetingPipeline.shared.process(id, summarize: Settings.shared.bool("MeetingSummarize")) } }

    func summarize() {
        guard let id = selectedID else { return }
        Task { @MainActor in await MeetingPipeline.shared.summarizeNow(id) }
    }

    func create(_ kind: String) {
        guard let id = selectedID else { return }
        working = "Writing \(kind.lowercased())…"
        Task { @MainActor in
            let err = await MeetingPipeline.shared.createOutput(kind, for: id)
            working = ""
            status = err ?? "\(kind) is ready"
            tab = .summary
        }
    }

    func removeOutput(_ o: MeetingOutput) { if let id = selectedID { store.update(id) { $0.outputs.removeAll { $0.id == o.id } } } }

    func copyTranscript(edited: Bool) {
        guard let m = meeting else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(MeetingExport.markdown(m, edited: edited), forType: .string)
        status = "Copied the transcript as Markdown"
    }

    func exportTranscript(_ kind: MeetingExport, edited: Bool) {
        guard let m = meeting else { return }
        let p = NSSavePanel()
        p.nameFieldStringValue = Output.sanitize(m.title) + (edited ? " (edited)" : "") + "." + kind.ext
        p.directoryURL = m.folder
        guard p.runModal() == .OK, let u = p.url else { return }
        do {
            try store.export(m, as: kind, edited: edited, to: u)
            status = "Saved \(u.lastPathComponent)"
        } catch { status = "Couldn't save: \(error.localizedDescription)" }
    }

    func exportAudio(enhance: Bool) {
        guard let m = meeting else { return }
        working = enhance ? "Exporting and enhancing…" : "Exporting…"
        let out = m.editedAudioURL
        Task { @MainActor in
            do {
                try await MeetingEdit.exportAudio(source: m.audioURL, duration: m.duration, cuts: m.cuts, enhance: enhance, to: out)
                if let id = selectedID { try? store.export(store.meeting(id) ?? m, as: .markdown, edited: true, to: m.folder.appendingPathComponent("edited.md")) }
                working = ""
                status = "Saved edited.m4a (\(MeetingFormat.clock(MeetingEdit.editedDuration(m))))"
                Output.reveal(out)
            } catch {
                working = ""
                status = "Export failed: \(error.localizedDescription)"
            }
        }
    }

    func reveal() { if let m = meeting { Output.reveal(FileManager.default.fileExists(atPath: m.audioURL.path) ? m.audioURL : m.folder) } }

    func delete() {
        guard let m = meeting else { return }
        let a = NSAlert()
        a.messageText = "Move “\(m.title)” to the Trash?"
        a.informativeText = "Its audio, transcript and notes go to the Trash, where you can put them back."
        a.addButton(withTitle: "Move to Trash")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        player.replaceCurrentItem(with: nil)
        store.delete(m.id)
        selectedID = store.meetings.first?.id
    }

    func importFiles(_ urls: [URL]) {
        for u in urls { MeetingImport.run(u) { [weak self] id in if let id { self?.selectedID = id } } }
    }
}

// MARK: - Opening any audio (or a video's sound) as a meeting to edit

enum MeetingImport {
    static var types: [UTType] { [.audio, .movie] }

    static func run(_ url: URL, done: @escaping (UUID?) -> Void) {
        Toast.shared.show("Opening \(url.lastPathComponent)…", "Its sound is copied into Meetings and transcribed on this Mac.")
        Task { @MainActor in
            do {
                let id = try await importFile(url)
                done(id)
                if Settings.shared.bool("MeetingTranscribe") { MeetingPipeline.shared.process(id) }
            } catch {
                Toast.shared.show("Couldn't open \(url.lastPathComponent)", error.localizedDescription, ms: 7000)
                done(nil)
            }
        }
    }

    @MainActor
    static func importFile(_ url: URL) async throws -> UUID {
        let asset = AVURLAsset(url: url)
        guard try await !asset.loadTracks(withMediaType: .audio).isEmpty else { throw MeetingEdit.Failure.failed("It has no sound.") }
        let duration = CMTimeGetSeconds(try await asset.load(.duration))
        var m = Meeting()
        m.title = url.deletingPathExtension().lastPathComponent
        m.created = (try? url.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? Date()
        m.duration = duration
        m.hasOthers = true
        m.imported = true
        m.transcript = Settings.shared.bool("MeetingTranscribe") ? .pending : .none
        let store = MeetingStore.shared
        m.folder = store.uniqueFolder(store.folderName(created: m.created, title: m.title))
        try FileManager.default.createDirectory(at: m.folder, withIntermediateDirectories: true)
        do {
            try await MeetingRecorder.mix([url], to: m.audioURL)
        } catch {
            try? FileManager.default.removeItem(at: m.folder)
            throw error
        }
        store.save(m)
        return m.id
    }
}

// MARK: - Window

final class MeetingsWindow: NSObject, NSWindowDelegate {
    static var shared: MeetingsWindow?
    let window: NSWindow
    let model: MeetingsModel

    static func show(select: UUID? = nil) {
        let w = shared ?? MeetingsWindow()
        shared = w
        if let select { w.model.selectedID = select }
        activateApp()
        w.window.makeKeyAndOrderFront(nil)
    }

    static func openAudio() {
        let p = NSOpenPanel()
        p.allowedContentTypes = MeetingImport.types
        p.allowsMultipleSelection = true
        p.message = "Choose audio (or a video) to transcribe and edit"
        activateApp()
        guard p.runModal() == .OK else { return }
        show()
        shared?.model.importFiles(p.urls)
    }

    init(model: MeetingsModel = MeetingsModel(), register: Bool = true) {
        self.model = model
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 760), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = "Meetings — Ather Screenshot"
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.minSize = NSSize(width: 860, height: 560)
        window.contentView = NSHostingView(rootView: MeetingsView(m: model))
        window.setFrameAutosaveName("MeetingsWindow")
        if register {
            window.center()
            AppDelegate.shared?.windowOpened(window)
        }
    }

    func windowWillClose(_ notification: Notification) {
        model.player.pause()
        if MeetingsWindow.shared === self { MeetingsWindow.shared = nil }
    }
}

// MARK: - Views

struct MeetingsView: View {
    @ObservedObject var m: MeetingsModel
    @ObservedObject var store: MeetingStore
    @State private var dropping = false

    init(m: MeetingsModel) {
        self.m = m
        store = m.store
    }

    var body: some View {
        HStack(spacing: 0) {
            MeetingsSidebar(m: m, store: store).frame(width: 290)
            Rectangle().fill(col(Theme.border)).frame(width: 1)
            Group {
                if let meeting = m.meeting { MeetingDetail(m: m, meeting: meeting, busy: store.busy.contains(meeting.id)) }
                else { MeetingsEmpty(hasAny: !store.meetings.isEmpty) }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(col(Theme.bg))
        .overlay(dropping ? RoundedRectangle(cornerRadius: 10).stroke(col(Theme.accent), lineWidth: 2).padding(4) : nil)
        .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { u, _ in if let u { DispatchQueue.main.async { m.importFiles([u]) } } }
            }
            return true
        }
        .preferredColorScheme(.dark)
    }
}

struct MeetingsSidebar: View {
    @ObservedObject var m: MeetingsModel
    @ObservedObject var store: MeetingStore
    @State private var tick = Date()
    private let timer = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(spacing: 0) {
            recordButton.padding(12)
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass").foregroundColor(col(Theme.muted))
                TextField("Search meetings and transcripts", text: $m.query).textFieldStyle(.plain)
            }
            .padding(.horizontal, 10).padding(.vertical, 7)
            .background(RoundedRectangle(cornerRadius: 8).fill(col(Theme.raised)))
            .padding(.horizontal, 12).padding(.bottom, 8)
            ScrollView {
                LazyVStack(spacing: 2) {
                    ForEach(m.list) { meeting in
                        MeetingRow(meeting: meeting, selected: meeting.id == m.selectedID, busy: store.busy.contains(meeting.id))
                            .contentShape(Rectangle())
                            .onTapGesture { m.selectedID = meeting.id }
                    }
                    if m.list.isEmpty && !m.query.isEmpty {
                        Text("No meeting matches “\(m.query)”.").font(.system(size: 12)).foregroundColor(col(Theme.muted)).padding(.top, 20)
                    }
                }
                .padding(.horizontal, 8)
            }
            Rectangle().fill(col(Theme.border)).frame(height: 1)
            Button { MeetingsWindow.openAudio() } label: {
                Label("Open audio to edit…", systemImage: "waveform.badge.plus").frame(maxWidth: .infinity, alignment: .leading)
            }
            .buttonStyle(.plain).foregroundColor(col(Theme.textDim)).padding(12)
        }
        .background(col(Theme.surface))
        .onReceive(timer) { tick = $0 }
    }

    @ViewBuilder private var recordButton: some View {
        let rec = MeetingRecorder.current
        Button {
            if let r = MeetingRecorder.current { r.stop() } else { MeetingRecorder.start() }
            tick = Date()
        } label: {
            HStack(spacing: 8) {
                Image(systemName: rec == nil ? "record.circle" : "stop.circle.fill").font(.system(size: 15, weight: .semibold))
                Text(rec.map { "Stop recording · \(MeetingFormat.clock($0.elapsed))" } ?? "Record a meeting").font(.system(size: 13, weight: .semibold))
                Spacer()
                Text(Hotkey.display(Settings.shared.hotkey(.recordMeeting))).font(.system(size: 11)).opacity(0.7)
            }
            .padding(.horizontal, 12).padding(.vertical, 9)
            .foregroundColor(rec == nil ? col(Theme.onAccent) : .white)
            .background(RoundedRectangle(cornerRadius: 9).fill(rec == nil ? col(Theme.accent) : Color.red.opacity(0.85)))
        }
        .buttonStyle(.plain)
        .id(tick)
    }
}

struct MeetingRow: View {
    let meeting: Meeting
    let selected: Bool
    let busy: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(meeting.title).font(.system(size: 13, weight: .semibold)).foregroundColor(col(Theme.text)).lineLimit(1)
            HStack(spacing: 6) {
                Text(MeetingFormat.dateTime.string(from: meeting.created))
                Text("·")
                Text(MeetingFormat.clock(meeting.duration)).monospacedDigit()
                Spacer()
                if busy { ProgressView().controlSize(.mini) }
                else if meeting.transcript == .failed { Image(systemName: "exclamationmark.triangle.fill").foregroundColor(.orange) }
                else if !meeting.cuts.isEmpty { Image(systemName: "scissors").foregroundColor(col(Theme.accent)) }
                if meeting.summary != nil { Image(systemName: "sparkles").foregroundColor(col(Theme.accent)) }
            }
            .font(.system(size: 11)).foregroundColor(col(Theme.muted))
        }
        .padding(.horizontal, 10).padding(.vertical, 8)
        .background(RoundedRectangle(cornerRadius: 8).fill(selected ? col(Theme.selected) : .clear))
        .overlay(RoundedRectangle(cornerRadius: 8).stroke(selected ? col(Theme.accent).opacity(0.5) : .clear, lineWidth: 1))
    }
}

struct MeetingsEmpty: View {
    let hasAny: Bool
    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: "waveform.circle").font(.system(size: 54, weight: .light)).foregroundColor(col(Theme.accent))
            Text(hasAny ? "Pick a meeting" : "No meetings yet").font(.system(size: 20, weight: .semibold)).foregroundColor(col(Theme.text))
            Text("During a Zoom, Meet, Teams or Slack call, press \(Hotkey.display(Settings.shared.hotkey(.recordMeeting))) to record its sound:\neveryone else and you, on separate tracks. You get a transcript that says who spoke,\nthe moments you marked, and a summary, all made on this Mac.")
                .multilineTextAlignment(.center).font(.system(size: 13)).foregroundColor(col(Theme.muted)).lineSpacing(3)
            HStack(spacing: 10) {
                Button("Record a meeting") { MeetingRecorder.start() }.buttonStyle(AccentButton())
                Button("Open audio to edit…") { MeetingsWindow.openAudio() }.buttonStyle(QuietButton())
            }
            .padding(.top, 6)
        }
        .padding(40)
    }
}

struct AccentButton: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(.system(size: 12.5, weight: .semibold)).padding(.horizontal, 13).padding(.vertical, 7)
            .foregroundColor(col(Theme.onAccent)).background(RoundedRectangle(cornerRadius: 7).fill(col(Theme.accent)))
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

struct QuietButton: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(.system(size: 12.5, weight: .medium)).padding(.horizontal, 11).padding(.vertical, 6)
            .foregroundColor(col(Theme.text)).background(RoundedRectangle(cornerRadius: 7).fill(col(Theme.raised)))
            .overlay(RoundedRectangle(cornerRadius: 7).stroke(col(Theme.border), lineWidth: 1))
            .opacity(configuration.isPressed ? 0.75 : 1)
    }
}

// MARK: detail

struct MeetingDetail: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting
    let busy: Bool
    @State private var title = ""
    @FocusState private var titleFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header.padding(.horizontal, 22).padding(.top, 18).padding(.bottom, 12)
            WaveformView(m: m, meeting: meeting).frame(height: 84).padding(.horizontal, 22)
            transport.padding(.horizontal, 22).padding(.vertical, 10)
            Rectangle().fill(col(Theme.border)).frame(height: 1)
            tabs.padding(.horizontal, 22).padding(.top, 10)
            Group {
                switch m.tab {
                case .transcript: TranscriptEditor(m: m, meeting: meeting, busy: busy)
                case .summary: SummaryPane(m: m, meeting: meeting, busy: busy)
                case .notes: NotesPane(m: m, meeting: meeting)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            statusBar
        }
        .onAppear { title = meeting.title }
        .onChange(of: meeting.id) { _, _ in title = meeting.title }
        .onChange(of: meeting.title) { _, t in if !titleFocused { title = t } }
    }

    private var header: some View {
        HStack(alignment: .top) {
            VStack(alignment: .leading, spacing: 4) {
                TextField("Title", text: $title)
                    .textFieldStyle(.plain).font(.system(size: 22, weight: .bold)).foregroundColor(col(Theme.text))
                    .focused($titleFocused)
                    .onSubmit { m.rename(title) }
                    .onChange(of: titleFocused) { _, f in if !f { m.rename(title) } }
                HStack(spacing: 8) {
                    Text(MeetingFormat.dateTime.string(from: meeting.created))
                    dot
                    Text(MeetingFormat.clock(meeting.duration)).monospacedDigit()
                    let saved = MeetingEdit.savedSeconds(meeting)
                    if saved > 0.05 {
                        Text("→ \(MeetingFormat.clock(MeetingEdit.editedDuration(meeting))) edited").foregroundColor(col(Theme.accent)).monospacedDigit()
                    }
                    dot
                    Text(meeting.sourcesLabel)
                    if !meeting.markers.isEmpty { dot; Label("\(meeting.markers.count)", systemImage: "flag.fill").foregroundColor(col(Theme.accent)) }
                }
                .font(.system(size: 12)).foregroundColor(col(Theme.muted))
            }
            Spacer(minLength: 16)
            HStack(spacing: 8) {
                exportMenu
                Menu {
                    Button("Show in Finder") { m.reveal() }
                    Button("Copy transcript (Markdown)") { m.copyTranscript(edited: !meeting.cuts.isEmpty) }
                    Divider()
                    Button("Transcribe again") { m.transcribeAgain() }.disabled(busy)
                    Menu("Speaker names") {
                        ForEach(meeting.tracks, id: \.self) { s in
                            Button("Rename “\(meeting.name(s))”…") { renameSpeaker(s) }
                        }
                    }
                    Divider()
                    Button("Move to Trash…", role: .destructive) { m.delete() }
                } label: { Image(systemName: "ellipsis.circle").font(.system(size: 15)) }
                .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            }
        }
    }

    private var dot: some View { Text("·") }

    private var exportMenu: some View {
        Menu {
            Section("Audio") {
                Button("Edited audio, enhanced (.m4a)") { m.exportAudio(enhance: true) }
                Button("Edited audio (.m4a)") { m.exportAudio(enhance: false) }
            }
            Section("Edited transcript") {
                ForEach(MeetingExport.allCases, id: \.self) { k in Button(k.title) { m.exportTranscript(k, edited: true) } }
            }
            Section("Full transcript") {
                ForEach([MeetingExport.markdown, .text, .srt], id: \.self) { k in Button(k.title) { m.exportTranscript(k, edited: false) } }
            }
        } label: {
            Label("Export", systemImage: "square.and.arrow.up").font(.system(size: 12.5, weight: .semibold))
        }
        .menuStyle(.borderlessButton).fixedSize()
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 7).fill(col(Theme.raised)))
        .overlay(RoundedRectangle(cornerRadius: 7).stroke(col(Theme.border), lineWidth: 1))
    }

    private func renameSpeaker(_ s: Speaker) {
        let a = NSAlert()
        a.messageText = "Name for “\(meeting.name(s))”"
        a.informativeText = s == .me ? "Shown for what the microphone recorded." : "Shown for what the system audio recorded (everyone else in the call)."
        let f = NSTextField(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        f.stringValue = meeting.name(s)
        a.accessoryView = f
        a.addButton(withTitle: "Rename")
        a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn { m.setSpeakerName(s, f.stringValue) }
    }

    private var transport: some View {
        HStack(spacing: 14) {
            Button { m.skip(-15) } label: { Image(systemName: "gobackward.15") }.buttonStyle(.plain).help("Back 15 s (⌥←)")
            Button { m.togglePlay() } label: {
                Image(systemName: m.playing ? "pause.fill" : "play.fill").font(.system(size: 15, weight: .bold))
                    .frame(width: 34, height: 34).foregroundColor(col(Theme.onAccent)).background(Circle().fill(col(Theme.accent)))
            }
            .buttonStyle(.plain).help("Play / pause (Space)")
            Button { m.skip(15) } label: { Image(systemName: "goforward.15") }.buttonStyle(.plain).help("Forward 15 s (⌥→)")
            Text("\(MeetingFormat.clock(m.time)) / \(MeetingFormat.clock(meeting.duration))").font(.system(size: 12, design: .monospaced)).foregroundColor(col(Theme.textDim))
            Picker("", selection: $m.rate) {
                ForEach([Float(0.75), 1, 1.25, 1.5, 2], id: \.self) { Text(String(format: "%g×", $0)).tag($0) }
            }
            .labelsHidden().frame(width: 74)
            Spacer()
            Toggle(isOn: $m.skipCuts) { Text("Play the edit").font(.system(size: 12)) }
                .toggleStyle(.switch).controlSize(.mini).tint(col(Theme.accent))
                .help("Skip what you cut while playing")
            Menu {
                Button("Trim start to the playhead") { m.trimStart() }
                Button("Trim end from the playhead") { m.trimEnd() }
                Divider()
                Button("Restore everything") { m.restoreAll() }.disabled(meeting.cuts.isEmpty)
            } label: { Label("Trim", systemImage: "timeline.selection").font(.system(size: 12)) }
            .menuStyle(.borderlessButton).fixedSize()
        }
        .foregroundColor(col(Theme.text))
    }

    private var tabs: some View {
        HStack(spacing: 4) {
            ForEach(MeetingsModel.Tab.allCases, id: \.self) { t in
                Button { m.tab = t } label: {
                    Text(t.rawValue).font(.system(size: 12.5, weight: m.tab == t ? .semibold : .regular))
                        .padding(.horizontal, 11).padding(.vertical, 6)
                        .foregroundColor(m.tab == t ? col(Theme.text) : col(Theme.muted))
                        .background(RoundedRectangle(cornerRadius: 7).fill(m.tab == t ? col(Theme.raised) : .clear))
                }
                .buttonStyle(.plain)
            }
            Spacer()
        }
    }

    private var statusBar: some View {
        HStack(spacing: 8) {
            if !m.working.isEmpty { ProgressView().controlSize(.small); Text(m.working) }
            else if busy { ProgressView().controlSize(.small); Text(meeting.transcript == .running ? "Transcribing on this Mac…" : "Writing with Apple Intelligence…") }
            else { Text(m.status) }
            Spacer()
            if meeting.transcript == .done {
                Text("transcript.json is kept up to date for AI agents").foregroundColor(col(Theme.muted).opacity(0.7))
            }
        }
        .font(.system(size: 11.5)).foregroundColor(col(Theme.muted))
        .padding(.horizontal, 22).padding(.vertical, 8)
        .background(col(Theme.surface))
    }
}

// MARK: waveform

struct WaveformView: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting

    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let d = max(0.001, meeting.duration)
            ZStack(alignment: .topLeading) {
                Canvas { ctx, size in
                    let peaks = m.peaks
                    let mid = size.height / 2
                    ctx.fill(Path(roundedRect: CGRect(origin: .zero, size: size), cornerRadius: 8), with: .color(col(Theme.surface)))
                    if !peaks.isEmpty {
                        let n = peaks.count
                        let bar = max(1, size.width / CGFloat(n))
                        let played = CGFloat(m.time / d) * size.width
                        var pastPath = Path(), restPath = Path()
                        for i in 0..<n {
                            let x = CGFloat(i) / CGFloat(n) * size.width
                            let ph = max(1, CGFloat(peaks[i]) * (size.height / 2 - 6))
                            let r = CGRect(x: x, y: mid - ph, width: max(1, bar - 0.6), height: ph * 2)
                            if x < played { pastPath.addRect(r) } else { restPath.addRect(r) }
                        }
                        ctx.fill(restPath, with: .color(col(Theme.textDim).opacity(0.55)))
                        ctx.fill(pastPath, with: .color(col(Theme.accent)))
                    } else {
                        ctx.fill(Path(CGRect(x: 0, y: mid - 0.5, width: size.width, height: 1)), with: .color(col(Theme.muted).opacity(0.5)))
                    }
                    for c in MeetingEdit.merged(meeting.cuts) {
                        let x0 = CGFloat(c.start / d) * size.width, x1 = CGFloat(c.end / d) * size.width
                        let r = CGRect(x: x0, y: 0, width: max(1, x1 - x0), height: size.height)
                        ctx.fill(Path(r), with: .color(Color.red.opacity(0.22)))
                        ctx.fill(Path(CGRect(x: x0, y: size.height - 3, width: max(1, x1 - x0), height: 3)), with: .color(Color.red.opacity(0.8)))
                    }
                    for mk in meeting.markers {
                        let x = CGFloat(mk.time / d) * size.width
                        ctx.fill(Path(CGRect(x: x - 0.75, y: 0, width: 1.5, height: size.height)), with: .color(col(Theme.accent).opacity(0.9)))
                        var flag = Path()
                        flag.move(to: CGPoint(x: x, y: 2)); flag.addLine(to: CGPoint(x: x + 9, y: 6)); flag.addLine(to: CGPoint(x: x, y: 10)); flag.closeSubpath()
                        ctx.fill(flag, with: .color(col(Theme.accent)))
                    }
                    let px = CGFloat(m.time / d) * size.width
                    ctx.fill(Path(CGRect(x: px - 1, y: 0, width: 2, height: size.height)), with: .color(.white))
                }
                ForEach(meeting.markers) { mk in
                    Color.clear.frame(width: 14, height: 14).contentShape(Rectangle())
                        .help("\(MeetingFormat.clock(mk.time)) \(mk.label)")
                        .position(x: CGFloat(mk.time / d) * w + 4, y: 7)
                        .onTapGesture { m.seek(mk.time) }
                }
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { v in m.seek(Double(max(0, min(w, v.location.x)) / w) * d) })
            .frame(width: w, height: h)
        }
    }
}

// MARK: transcript editor

struct WordFlow: Layout {
    var spacing: CGFloat = 1
    var lineSpacing: CGFloat = 3

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let maxW = proposal.width ?? 640
        var x: CGFloat = 0, y: CGFloat = 0, rowH: CGFloat = 0, widest: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x > 0, x + sz.width > maxW { y += rowH + lineSpacing; x = 0; rowH = 0 }
            x += sz.width + spacing
            rowH = max(rowH, sz.height)
            widest = max(widest, x)
        }
        return CGSize(width: min(maxW, widest), height: y + rowH)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x: CGFloat = 0, y: CGFloat = 0, rowH: CGFloat = 0
        for s in subviews {
            let sz = s.sizeThatFits(.unspecified)
            if x > 0, x + sz.width > bounds.width { y += rowH + lineSpacing; x = 0; rowH = 0 }
            s.place(at: CGPoint(x: bounds.minX + x, y: bounds.minY + y), proposal: ProposedViewSize(sz))
            x += sz.width + spacing
            rowH = max(rowH, sz.height)
        }
    }
}

struct TranscriptEditor: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting
    let busy: Bool
    @FocusState private var focused: Bool

    var body: some View {
        VStack(spacing: 0) {
            toolbar.padding(.horizontal, 22).padding(.vertical, 8)
            content
        }
        .focusable()
        .focusEffectDisabled()
        .focused($focused)
        .onAppear { focused = true }
        .onKeyPress(keys: [.delete, .deleteForward]) { _ in m.deleteSelection(); return .handled }
        .onKeyPress(.space) { m.togglePlay(); return .handled }
        .onKeyPress(.escape) { m.anchor = nil; m.focus = nil; return .handled }
        .onKeyPress(characters: CharacterSet(charactersIn: "z"), phases: .down) { k in
            guard k.modifiers.contains(.command) else { return .ignored }
            if k.modifiers.contains(.shift) { m.redoCut() } else { m.undoCut() }
            return .handled
        }
        .onKeyPress(keys: [.leftArrow, .rightArrow]) { k in
            if k.modifiers.contains(.option) { m.skip(k.key == .leftArrow ? -15 : 15); return .handled }
            guard let f = m.focus ?? m.anchor else { return .ignored }
            let n = max(0, min(m.words.count - 1, f + (k.key == .leftArrow ? -1 : 1)))
            if k.modifiers.contains(.shift) { m.focus = n } else { m.anchor = n; m.focus = n }
            m.seek(m.words[n].word.start)
            return .handled
        }
    }

    private var toolbar: some View {
        HStack(spacing: 8) {
            Menu {
                Button("Remove filler words (um, uh…)") { m.cleanUp(fillers: true, repeats: false, pauses: false) }
                Button("Remove repeated words (“the the”)") { m.cleanUp(fillers: false, repeats: true, pauses: false) }
                Button("Shorten long pauses (over 1 s)") { m.cleanUp(fillers: false, repeats: false, pauses: true) }
                Divider()
                Button("All of the above") { m.cleanUp(fillers: true, repeats: true, pauses: true) }
            } label: { Label("Clean up", systemImage: "wand.and.stars").font(.system(size: 12.5, weight: .semibold)) }
            .menuStyle(.borderlessButton).fixedSize()
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 7).fill(col(Theme.selected)))
            .overlay(RoundedRectangle(cornerRadius: 7).stroke(col(Theme.accent).opacity(0.6), lineWidth: 1))
            .disabled(meeting.lines.isEmpty)
            Button(m.selectionIsCut ? "Restore" : "Cut") { m.deleteSelection() }.buttonStyle(QuietButton())
                .disabled(m.selection == nil).help(m.selectionIsCut ? "Bring the selected words back" : "Cut the selected words (⌫)")
            Button { m.undoCut() } label: { Image(systemName: "arrow.uturn.backward") }.buttonStyle(QuietButton()).disabled(!m.canUndo).help("Undo (⌘Z)")
            Button { m.redoCut() } label: { Image(systemName: "arrow.uturn.forward") }.buttonStyle(QuietButton()).disabled(!m.canRedo).help("Redo (⇧⌘Z)")
            Spacer()
            Toggle(isOn: $m.follow) { Text("Follow playback").font(.system(size: 12)) }.toggleStyle(.checkbox)
            Text("Click a word to play from it · ⇧-click to select · ⌫ cuts").font(.system(size: 11)).foregroundColor(col(Theme.muted))
        }
    }

    @ViewBuilder private var content: some View {
        switch meeting.transcript {
        case .done where meeting.lines.isEmpty:
            placeholder("No speech was recognized", "The recording may be silent, or in another language: pick it in Settings › Meetings and choose Transcribe again.")
        case .done:
            lines
        case .failed:
            placeholder("Not transcribed", meeting.transcriptError ?? "", action: ("Transcribe again", { m.transcribeAgain() }))
        case .running, .pending:
            VStack(spacing: 10) {
                ProgressView()
                Text(busy ? "Transcribing on this Mac…" : "Waiting to transcribe…").foregroundColor(col(Theme.muted))
                if !busy { Button("Transcribe now") { m.transcribeAgain() }.buttonStyle(QuietButton()) }
            }.frame(maxWidth: .infinity, maxHeight: .infinity)
        case .none:
            placeholder("Not transcribed yet", "Transcripts are made on this Mac and never leave it.", action: ("Transcribe", { m.transcribeAgain() }))
        }
    }

    private func placeholder(_ title: String, _ text: String, action: (String, () -> Void)? = nil) -> some View {
        VStack(spacing: 8) {
            Text(title).font(.system(size: 15, weight: .semibold)).foregroundColor(col(Theme.text))
            Text(text).font(.system(size: 12)).foregroundColor(col(Theme.muted)).multilineTextAlignment(.center).frame(maxWidth: 460)
            if let action { Button(action.0, action: action.1).buttonStyle(QuietButton()).padding(.top, 4) }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var lines: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 12) {
                    ForEach(Array(meeting.lines.enumerated()), id: \.element.id) { li, line in
                        LineView(m: m, meeting: meeting, li: li, line: line, current: m.currentLine == li).id(line.id)
                    }
                }
                .padding(.horizontal, 22).padding(.vertical, 12)
            }
            .onChange(of: m.currentLine) { _, li in
                guard m.follow, m.playing, let li, li < meeting.lines.count else { return }
                withAnimation(.easeInOut(duration: 0.25)) { proxy.scrollTo(meeting.lines[li].id, anchor: .center) }
            }
        }
    }
}

struct LineView: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting
    let li: Int
    let line: MeetingLine
    let current: Bool

    var body: some View {
        let range = m.wordRange(line: li)
        let allCut = !range.isEmpty && range.allSatisfy { m.words[$0].cut }
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .trailing, spacing: 2) {
                Text(meeting.name(line.speaker)).font(.system(size: 12, weight: .semibold))
                    .foregroundColor(line.speaker == .me ? col(Theme.accent) : Color(red: 0.55, green: 0.78, blue: 1))
                Text(MeetingFormat.clock(line.start)).font(.system(size: 11, design: .monospaced)).foregroundColor(col(Theme.muted))
            }
            .frame(width: 76, alignment: .trailing)
            .contentShape(Rectangle())
            .onTapGesture { m.selectLine(li) }
            .help("Select this line")
            WordFlow {
                ForEach(Array(range), id: \.self) { i in WordChip(m: m, i: i) }
            }
            .opacity(allCut ? 0.55 : 1)
        }
        .padding(.vertical, 6).padding(.horizontal, 8)
        .background(RoundedRectangle(cornerRadius: 8).fill(current ? col(Theme.surface) : .clear))
    }
}

struct WordChip: View {
    @ObservedObject var m: MeetingsModel
    let i: Int

    var body: some View {
        let w = m.words[i]
        let selected = m.selection?.contains(i) ?? false
        let current = m.isCurrent(w)
        Text(w.word.text)
            .font(.system(size: 14.5))
            .strikethrough(w.cut, color: .red)
            .foregroundColor(w.cut ? col(Theme.muted).opacity(0.7) : current ? col(Theme.onAccent) : col(Theme.text))
            .padding(.horizontal, 2.5).padding(.vertical, 1.5)
            .background(RoundedRectangle(cornerRadius: 4).fill(current && !w.cut ? col(Theme.accent) : selected ? Color.blue.opacity(0.42) : w.cut ? Color.red.opacity(0.10) : .clear))
            .contentShape(Rectangle())
            .onTapGesture { m.click(i, shift: NSEvent.modifierFlags.contains(.shift)) }
    }
}

// MARK: summary and outputs

struct SummaryPane: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting
    let busy: Bool

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack(spacing: 8) {
                    Button(meeting.summary == nil ? "Summarize" : "Summarize again") { m.summarize() }
                        .buttonStyle(AccentButton()).disabled(busy || meeting.lines.isEmpty || !MeetingAI.available)
                    Menu {
                        ForEach(MeetingAI.outputKinds, id: \.self) { k in Button(k) { m.create(k) } }
                    } label: { Label("Create", systemImage: "sparkles").font(.system(size: 12.5, weight: .semibold)) }
                    .menuStyle(.borderlessButton).fixedSize()
                    .padding(.horizontal, 10).padding(.vertical, 5)
                    .background(RoundedRectangle(cornerRadius: 7).fill(col(Theme.raised)))
                    .disabled(busy || meeting.lines.isEmpty || !MeetingAI.available)
                    Spacer()
                    Text("On this Mac, with Apple Intelligence").font(.system(size: 11)).foregroundColor(col(Theme.muted))
                }
                if let why = MeetingAI.unavailableReason {
                    note("Summaries and outputs aren't available", why + " The transcript, exports and transcript.json work without it, so an AI agent can summarize instead.")
                }
                if let e = meeting.summaryError { note("The last summary failed", e) }
                if let s = meeting.summary, !s.isEmpty {
                    if !s.overview.isEmpty { section("Summary") { Text(s.overview).font(.system(size: 14)).foregroundColor(col(Theme.text)).lineSpacing(3).textSelection(.enabled) } }
                    bullets("Key points", s.keyPoints, icon: "circle.fill")
                    bullets("Decisions", s.decisions, icon: "checkmark.seal.fill")
                    bullets("Action items", s.actionItems, icon: "square")
                } else if MeetingAI.available && !busy {
                    Text(meeting.lines.isEmpty ? "A summary is written once there's a transcript." : "No summary yet.").foregroundColor(col(Theme.muted))
                }
                if !meeting.markers.isEmpty {
                    section("Marked moments") {
                        VStack(alignment: .leading, spacing: 6) {
                            ForEach(meeting.markers.sorted { $0.time < $1.time }) { mk in
                                Button { m.seek(mk.time); m.tab = .transcript } label: {
                                    HStack(spacing: 8) {
                                        Image(systemName: "flag.fill").foregroundColor(col(Theme.accent))
                                        Text(MeetingFormat.clock(mk.time)).font(.system(size: 12, design: .monospaced)).foregroundColor(col(Theme.muted))
                                        Text(mk.label).foregroundColor(col(Theme.text))
                                    }
                                }.buttonStyle(.plain)
                            }
                        }
                    }
                }
                ForEach(meeting.outputs) { o in
                    section(o.kind) {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(o.text).font(.system(size: 13.5)).foregroundColor(col(Theme.text)).lineSpacing(3).textSelection(.enabled)
                            HStack {
                                Button("Copy") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(o.text, forType: .string); m.status = "Copied \(o.kind.lowercased())" }.buttonStyle(QuietButton())
                                Button("Remove") { m.removeOutput(o) }.buttonStyle(QuietButton())
                            }
                        }
                        .padding(12).background(RoundedRectangle(cornerRadius: 9).fill(col(Theme.surface)))
                    }
                }
            }
            .padding(22)
            .frame(maxWidth: 820, alignment: .leading)
        }
    }

    private func section<C: View>(_ title: String, @ViewBuilder _ c: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title.uppercased()).font(.system(size: 11, weight: .bold)).tracking(0.6).foregroundColor(col(Theme.muted))
            c()
        }
    }

    @ViewBuilder private func bullets(_ title: String, _ items: [String], icon: String) -> some View {
        if !items.isEmpty {
            section(title) {
                VStack(alignment: .leading, spacing: 7) {
                    ForEach(Array(items.enumerated()), id: \.offset) { _, t in
                        HStack(alignment: .firstTextBaseline, spacing: 9) {
                            Image(systemName: icon).font(.system(size: icon == "circle.fill" ? 5 : 11)).foregroundColor(col(Theme.accent))
                            Text(t).font(.system(size: 13.5)).foregroundColor(col(Theme.text)).textSelection(.enabled)
                        }
                    }
                }
            }
        }
    }

    private func note(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(title).font(.system(size: 12.5, weight: .semibold)).foregroundColor(col(Theme.text))
            Text(text).font(.system(size: 12)).foregroundColor(col(Theme.muted))
        }
        .padding(12).frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 9).fill(col(Theme.surface)))
    }
}

struct NotesPane: View {
    @ObservedObject var m: MeetingsModel
    let meeting: Meeting
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Your notes are saved with the meeting, in transcript.md and for agents in transcript.json.")
                .font(.system(size: 11.5)).foregroundColor(col(Theme.muted))
            TextEditor(text: $text)
                .font(.system(size: 14)).scrollContentBackground(.hidden)
                .padding(10).background(RoundedRectangle(cornerRadius: 9).fill(col(Theme.surface)))
                .onChange(of: text) { _, t in m.setNotes(t) }
        }
        .padding(22)
        .onAppear { text = meeting.notes }
        .onChange(of: meeting.id) { _, _ in text = meeting.notes }
    }
}
