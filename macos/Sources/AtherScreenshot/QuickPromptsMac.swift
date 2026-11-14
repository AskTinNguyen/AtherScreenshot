import AppKit
import Carbon

// The Mac side of QuickPrompts (src/quickprompts_win.cpp on Windows): the listener that watches typing for triggers,
// the paste that inserts a prompt into another app, the picker, per-prompt shortcuts and the first-start card.
//
// The listener is a listen-only CGEventTap on a thread of its own: it never delays anyone's typing (it can't
// change or swallow events) and the main thread being busy doesn't matter. It keeps the last 64 typed characters
// in memory (TriggerMatcher) and nothing else; they are never written, logged or shared. While secure input is
// on (password fields, Terminal's Secure Keyboard Entry) it sees nothing and the buffer is cleared.
// It needs Accessibility, like keystrokes in recordings, which is also what lets it press ⌫ and ⌘V.
final class QuickPromptsMac {
    static let shared = QuickPromptsMac()
    static let transientType = NSPasteboard.PasteboardType("org.nspasteboard.TransientType")
    static let concealedType = NSPasteboard.PasteboardType("org.nspasteboard.ConcealedType")
    static let eventMarker: Int64 = 0x4154_4852_5150  // on the keys we press, so the listener skips them

    let store = QuickPromptStore.shared
    private let listener = Listener()
    private var timer: Timer?
    private(set) var started = false
    private(set) var busy = false
    private(set) var hotkeyStatus: [String: String] = [:]  // prompt id -> why its shortcut isn't active
    private var s: Settings { .shared }

    var listening: Bool { listener.running }
    static var accessibilityGranted: Bool { AXIsProcessTrusted() }

    static func askForAccessibility() {
        if !AXIsProcessTrustedWithOptions([kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary) {
            NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")!)
        }
    }

    // MARK: lifecycle

    // At launch: the prompts, the listener (if on), file watching and, once, the welcome card.
    func start() {
        guard !started else { return }
        started = true
        let err = store.load(folder: Settings.supportFolder, teamFile: s.string("TeamFile"))
        if !err.isEmpty { Toast.shared.show("QuickPrompts", err, ms: 8000) }
        NotificationCenter.default.addObserver(forName: QuickPromptStore.changed, object: store, queue: .main) { [weak self] _ in
            self?.storeChanged()
        }
        DistributedNotificationCenter.default().addObserver(forName: Notification.Name("com.apple.Carbon.TISNotifySelectedKeyboardInputSourceChanged"),
                                                            object: nil, queue: .main) { [weak self] _ in self?.updateInputSource() }
        updateInputSource()
        apply()
        // Picks up hand edits and the team file changing, and starts listening once Accessibility is allowed.
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.store.reloadIfChanged()
            if self.s.bool("ExpandAsYouType") && !self.listener.running && Self.accessibilityGranted { self.apply() }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in self?.showWelcomeOnce() }
    }

    func stop() { listener.stop() }

    // Settings changed: the team file and the listener follow.
    func apply() {
        store.setTeamFile(s.string("TeamFile"))
        refreshTriggers()
        listener.skipOwnPid = ProcessInfo.processInfo.processIdentifier
        if s.bool("ExpandAsYouType") { _ = listener.start() } else { listener.stop() }
        QuickPromptsStatus.shared.refresh()
    }

    private func storeChanged() {
        refreshTriggers()
        AppDelegate.shared?.registerHotkeys()  // prompts' own shortcuts
    }

    private func refreshTriggers() {
        let active = store.active
        listener.setTriggers(active.enumerated().map { .init(trigger: $1.trigger, caseSensitive: $1.caseSensitive, tag: $0) }, ids: active.map(\.id))
    }

    func setTyping(_ on: Bool) {
        s.set("ExpandAsYouType", on)  // onChange calls apply()
        if on && !Self.accessibilityGranted { Self.askForAccessibility() }
    }

    // Vietnamese Telex, Japanese and other input methods compose what is typed: the keys aren't the text, so they
    // never count toward a trigger (the picker still works).
    private func updateInputSource() {
        var composing = false
        if let src = TISCopyCurrentKeyboardInputSource()?.takeRetainedValue(),
           let raw = TISGetInputSourceProperty(src, kTISPropertyInputSourceType) {
            let type = Unmanaged<CFString>.fromOpaque(raw).takeUnretainedValue()
            composing = type != kTISTypeKeyboardLayout
        }
        listener.setComposing(composing)
    }

    // MARK: shortcuts

    // Called by AppDelegate.registerHotkeys after the commands' shortcuts.
    func registerHotkeys() {
        hotkeyStatus = [:]
        for p in store.active where !p.hotkey.isEmpty && p.source == "user" {
            guard let hk = Hotkey.parse(p.hotkey) else { hotkeyStatus[p.id] = "“\(p.hotkey)” isn’t a valid shortcut"; continue }
            let id = p.id
            if !HotkeyCenter.shared.register(hk, action: { [weak self] in self?.hotkeyPressed(id) }) {
                hotkeyStatus[p.id] = "\(hk.display) is taken by another command, app or macOS"
            }
        }
    }

    private func hotkeyPressed(_ id: String) {
        guard let p = store.find(id) else { return }
        insert(p, target: Self.frontOtherApp())
    }

    static func frontOtherApp() -> NSRunningApplication? {
        let f = NSWorkspace.shared.frontmostApplication
        return f?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : f
    }

    // MARK: commands

    func paletteItems(target: NSRunningApplication?) -> [PaletteItem] {
        // Most used first, then as listed.
        let list = store.active.enumerated().sorted { a, b in a.element.uses != b.element.uses ? a.element.uses > b.element.uses : a.offset < b.offset }.map(\.element)
        return list.map { p in
            PaletteItem(id: p.id, title: p.name, keywords: ([p.trigger] + p.tags).joined(separator: " ") + " " + String(p.text.prefix(400)),
                        icon: p.source == "builtin" ? "sparkles" : p.source == "team" ? "person.2" : "text.quote", hint: p.trigger) { [weak self] in
                self?.insert(p, target: target)
            }
        }
    }

    // ⌃⌥Space: every prompt, searchable; ↩ pastes it into the app that was in front.
    func showPicker() {
        store.reloadIfChanged()
        let target = Self.frontOtherApp()
        Palette.shared.show(paletteItems(target: target), placeholder: "Search QuickPrompts…  (↩ pastes it)", mruKey: "QuickPromptsMRU",
                            create: ("New QuickPrompt", { [weak self] text in self?.newPrompt(title: text) }))
    }

    func newPrompt(title: String = "", text: String = "") {
        var p = QuickPrompt()
        p.title = title.isEmpty ? "New prompt" : title
        p.text = text
        let base = String(title.lowercased().filter { $0.isLetter || $0.isNumber }.prefix(24))
        p.trigger = store.uniqueTrigger(base.count >= 2 ? base : "newprompt")
        let r = store.add(p)
        if r.error.isEmpty { QuickPromptsWindow.show(select: r.id, focusTrigger: true) }
    }

    func newFromClipboard() {
        let text = (NSPasteboard.general.string(forType: .string) ?? "").replacingOccurrences(of: "\r", with: "")
        guard !text.isEmpty else { return Toast.shared.show("Nothing to save", "Copy some text first, then run “Save clipboard as a QuickPrompt”.") }
        var p = QuickPrompt()
        p.text = text
        p.title = String(text.prefix { $0 != "\n" }.prefix(48))
        p.trigger = store.uniqueTrigger("clipprompt")
        let r = store.add(p)
        if r.error.isEmpty { QuickPromptsWindow.show(select: r.id, focusTrigger: true) }
    }

    // The path {lastcapture} and {lastcapture:image} use: the last capture, or the newest one in the captures folder.
    static func lastCapture() -> URL? {
        if let u = AppDelegate.shared?.lastURL, FileManager.default.fileExists(atPath: u.path) { return u }
        return Output.listCaptures().first
    }

    // MARK: first start

    func showWelcomeOnce() {
        guard !s.bool("WelcomeShown") else { return }
        // Q3 of docs/intent/text-shortcuts: on from the first start, explained once.
        s.set("WelcomeShown", true)
        if !s.bool("ExpandAsYouType") { s.set("ExpandAsYouType", true) }
        QuickPromptsWelcome.show(picker: Hotkey.display(s.hotkey(.insertQuickPrompt))) { open in
            if !Self.accessibilityGranted { Self.askForAccessibility() }
            if open { QuickPromptsWindow.show() }
        }
    }

    // MARK: a typed trigger

    fileprivate func typed(id: String, erase: Int, keySeq: UInt64, pid: pid_t) {
        guard let front = NSWorkspace.shared.frontmostApplication else { return }
        if pid > 0 && front.processIdentifier != pid { return }
        if front.processIdentifier == ProcessInfo.processInfo.processIdentifier { return }  // our own text boxes (the window has Try it)
        if Self.isExcluded(bundleID: front.bundleIdentifier ?? "", name: front.localizedName ?? "", list: s.string("ExcludedApps")) { return }
        if s.bool("SkipFullscreen") && Self.isFullscreenGame(front) { return }
        guard let p = store.find(id) else { return }
        insert(p, target: front, erase: erase, keySeq: keySeq)
    }

    // `list`: bundle ids or app names separated by ; ("com.agilebits.onepassword7; KeePassXC"). A Windows list's
    // "KeePass.exe" matches the app named KeePass.
    static func isExcluded(bundleID: String, name: String, list: String) -> Bool {
        guard !bundleID.isEmpty || !name.isEmpty else { return false }
        for raw in list.split(separator: ";") {
            var item = raw.trimmingCharacters(in: .whitespaces).lowercased()
            if item.isEmpty { continue }
            if item == bundleID.lowercased() { return true }
            for ext in [".app", ".exe"] where item.hasSuffix(ext) { item = String(item.dropLast(ext.count)) }
            if !name.isEmpty && item == name.lowercased() { return true }
        }
        return false
    }

    // A game in fullscreen: its front window covers a whole display and it says it's a game (or runs from Steam).
    // Native full-screen apps (Safari, Xcode, Slack…) still expand.
    static func isFullscreenGame(_ app: NSRunningApplication) -> Bool {
        guard let info = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]],
              let w = info.first(where: { ($0[kCGWindowOwnerPID as String] as? pid_t) == app.processIdentifier && ($0[kCGWindowLayer as String] as? Int) == 0 }),
              let bd = w[kCGWindowBounds as String] as? NSDictionary, let b = CGRect(dictionaryRepresentation: bd) else { return false }
        guard NSScreen.screens.contains(where: { let f = Geo.toCG($0.frame); return b.minX <= f.minX && b.minY <= f.minY && b.maxX >= f.maxX && b.maxY >= f.maxY }) else { return false }
        let path = app.bundleURL?.path ?? ""
        let category = app.bundleURL.flatMap { Bundle(url: $0)?.infoDictionary?["LSApplicationCategoryType"] as? String } ?? ""
        return category.contains("games") || path.contains("/steamapps/") || app.bundleURL == nil
    }

    // MARK: pasting

    // Pastes `p` into `target` (the app in front when nil) once the modifier keys are up, first deleting `erase` typed
    // characters. With `keySeq` (a typed trigger) it gives up if anything was typed since.
    func insert(_ p: QuickPrompt, target: NSRunningApplication?, erase: Int = 0, keySeq: UInt64? = nil) {
        guard !busy else { return }
        guard Self.accessibilityGranted else {
            return Toast.shared.show("QuickPrompts needs Accessibility", "Allow Ather Screenshot in System Settings › Privacy & Security › Accessibility, so it can paste prompts.", ms: 8000) {
                Self.askForAccessibility()
            }
        }
        let pb = NSPasteboard.general
        let last = Self.lastCapture()
        let exp = QuickPrompts.expand(p.text, .current(clipboard: pb.string(forType: .string) ?? "", lastCapture: last?.path ?? ""))
        var image: CGImage?
        if exp.pasteImage {
            if let u = last, !MediaFiles.isVideo(u), u.pathExtension.lowercased() != "gif" { image = CGImage.load(u) }
            if image == nil {
                Toast.shared.show("No capture to attach", "“\(p.name)” pastes your last screenshot. Take one first; the text was pasted without it.", ms: 6000)
            }
        }
        if exp.text.isEmpty && image == nil { return }
        busy = true
        waitForModifiers(tries: 200) { [weak self] in self?.run(p, exp, image, target: target, erase: erase, keySeq: keySeq) }
    }

    private static var modifiersDown: Bool {
        !CGEventSource.flagsState(.combinedSessionState).intersection([.maskCommand, .maskControl, .maskAlternate, .maskShift]).isEmpty
    }

    private func waitForModifiers(tries: Int, _ then: @escaping () -> Void) {
        if Self.modifiersDown {
            // Held for 3 s: pasting now would turn ⌘V into something else.
            guard tries > 0 else { busy = false; return }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.015) { [weak self] in self?.waitForModifiers(tries: tries - 1, then) }
            return
        }
        then()
    }

    private func whenFront(_ target: NSRunningApplication?, tries: Int = 50, _ then: @escaping (Bool) -> Void) {
        guard let target else { return then(true) }
        if NSWorkspace.shared.frontmostApplication?.processIdentifier == target.processIdentifier { return then(true) }
        if tries == 50 { target.activate() }
        guard tries > 0 else { return then(false) }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.02) { [weak self] in self?.whenFront(target, tries: tries - 1, then) }
    }

    private func run(_ p: QuickPrompt, _ exp: QuickPrompts.Expansion, _ image: CGImage?, target: NSRunningApplication?, erase: Int, keySeq: UInt64?) {
        whenFront(target) { [weak self] ok in
            guard let self else { return }
            // The person kept typing, or the app went away: deleting now would hit their text.
            guard ok, keySeq.map({ $0 == self.listener.keySeq }) ?? true else { self.busy = false; return }
            let pb = NSPasteboard.general
            let saved = Self.savePasteboard(pb)
            var ours = -1
            if !exp.text.isEmpty {
                guard Self.putText(exp.text, on: pb) else { self.busy = false; return }
                ours = pb.changeCount
                Self.press(CGKeyCode(kVK_Delete), times: erase)
                Self.press(CGKeyCode(kVK_ANSI_V), flags: .maskCommand)
            } else {
                Self.press(CGKeyCode(kVK_Delete), times: erase)
            }
            self.store.noteUsed(p.id)
            let finish = {
                // Put back what was on the pasteboard, unless someone copied something new meanwhile.
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                    if pb.changeCount == ours { Self.restorePasteboard(saved, on: pb) }
                    self.busy = false
                }
            }
            let afterText = {
                guard exp.cursorBack > 0 else { return finish() }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
                    Self.press(CGKeyCode(kVK_LeftArrow), times: exp.cursorBack)
                    finish()
                }
            }
            guard let image else { return afterText() }
            DispatchQueue.main.asyncAfter(deadline: .now() + (exp.text.isEmpty ? 0 : 0.25)) {
                if Self.putImage(image, on: pb) {
                    ours = pb.changeCount
                    Self.press(CGKeyCode(kVK_ANSI_V), flags: .maskCommand)
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { afterText() }
            }
        }
    }

    private static func press(_ key: CGKeyCode, flags: CGEventFlags = [], times: Int = 1) {
        guard times > 0, let src = CGEventSource(stateID: .privateState) else { return }
        src.userData = eventMarker
        for _ in 0..<times {
            for down in [true, false] {
                guard let e = CGEvent(keyboardEventSource: src, virtualKey: key, keyDown: down) else { continue }
                e.flags = flags
                e.post(tap: .cghidEventTap)
            }
        }
    }

    // MARK: the pasteboard (exposed for tests, which use their own)

    struct Saved {
        var items: [[(NSPasteboard.PasteboardType, Data)]] = []
        var complete = true  // false: something couldn't be kept (too big)
    }

    static func savePasteboard(_ pb: NSPasteboard) -> Saved {
        var s = Saved()
        var total = 0
        for item in pb.pasteboardItems ?? [] {
            var kept: [(NSPasteboard.PasteboardType, Data)] = []
            for t in item.types {
                guard let d = item.data(forType: t) else { continue }
                if total + d.count > 96 << 20 { s.complete = false; continue }
                total += d.count
                kept.append((t, d))
            }
            if !kept.isEmpty { s.items.append(kept) }
        }
        return s
    }

    static func restorePasteboard(_ s: Saved, on pb: NSPasteboard) {
        pb.clearContents()
        guard !s.items.isEmpty else { return }
        let items = s.items.map { kept -> NSPasteboardItem in
            let item = NSPasteboardItem()
            for (t, d) in kept { item.setData(d, forType: t) }
            // It's back, not new: clipboard managers already have it.
            if !kept.contains(where: { $0.0 == transientType }) { item.setData(Data(), forType: transientType) }
            return item
        }
        pb.writeObjects(items)
    }

    // The temporary entry is marked so clipboard managers (and Universal Clipboard history tools) skip it.
    private static func mark(_ item: NSPasteboardItem) {
        item.setData(Data(), forType: transientType)
        item.setData(Data(), forType: concealedType)
    }

    @discardableResult
    static func putText(_ text: String, on pb: NSPasteboard) -> Bool {
        pb.clearContents()
        let item = NSPasteboardItem()
        item.setString(text, forType: .string)
        mark(item)
        return pb.writeObjects([item])
    }

    @discardableResult
    static func putImage(_ img: CGImage, on pb: NSPasteboard) -> Bool {
        guard let png = img.pngData() else { return false }
        pb.clearContents()
        let item = NSPasteboardItem()
        item.setData(png, forType: .png)
        if let tiff = img.nsImage().tiffRepresentation { item.setData(tiff, forType: .tiff) }
        mark(item)
        return pb.writeObjects([item])
    }
}

// MARK: - the listener (its own thread)

private final class Listener {
    private let lock = NSLock()
    private var entries: [TriggerMatcher.Entry] = []
    private var ids: [String] = []
    private var triggersVersion = 0
    private var composing = false
    private var seq: UInt64 = 0  // key presses seen; an insert gives up if it moved on
    private var thread: Thread?
    private var runLoop: CFRunLoop?
    private var tap: CFMachPort?
    private var stopping = false
    var skipOwnPid: pid_t = 0
    // Developer tool only: also count keys typed by software (scripts that test the listener).
    private let acceptInjected = ProcessInfo.processInfo.environment["ATHER_QP_ACCEPT_INJECTED"] == "1"

    // Only the listener thread touches these.
    private var matcher = TriggerMatcher()
    private var seenVersion = -1
    private var lastPid: Int64 = -1

    var running: Bool { lock.withLock { tap != nil } }
    var keySeq: UInt64 { lock.withLock { seq } }

    func setTriggers(_ e: [TriggerMatcher.Entry], ids: [String]) {
        lock.withLock {
            entries = e
            self.ids = ids
            triggersVersion += 1
        }
    }

    func setComposing(_ on: Bool) { lock.withLock { composing = on } }

    func start() -> Bool {
        if lock.withLock({ thread != nil }) { return running }
        let ready = DispatchSemaphore(value: 0)
        lock.withLock { stopping = false }
        let t = Thread { [weak self] in
            guard let self else { ready.signal(); return }
            let mask = [CGEventType.keyDown, .leftMouseDown, .rightMouseDown, .otherMouseDown].reduce(CGEventMask(0)) { $0 | (1 << $1.rawValue) }
            guard let tap = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly, eventsOfInterest: mask,
                                              callback: listenerCallback, userInfo: Unmanaged.passUnretained(self).toOpaque()) else {
                self.lock.withLock { self.thread = nil }
                ready.signal()
                return
            }
            let src = CFMachPortCreateRunLoopSource(nil, tap, 0)
            let rl = CFRunLoopGetCurrent()
            CFRunLoopAddSource(rl, src, .commonModes)
            CGEvent.tapEnable(tap: tap, enable: true)
            self.lock.withLock {
                self.tap = tap
                self.runLoop = rl
            }
            ready.signal()
            while !self.lock.withLock({ self.stopping }) { _ = CFRunLoopRunInMode(.defaultMode, 1, false) }
            CGEvent.tapEnable(tap: tap, enable: false)
            CFRunLoopRemoveSource(rl, src, .commonModes)
            CFMachPortInvalidate(tap)
            self.lock.withLock {
                self.tap = nil
                self.runLoop = nil
                self.thread = nil
            }
        }
        t.name = "QuickPrompts listener"
        lock.withLock { thread = t }
        t.start()
        _ = ready.wait(timeout: .now() + 3)
        return running
    }

    func stop() {
        let rl: CFRunLoop? = lock.withLock {
            guard thread != nil else { return nil }
            stopping = true
            return runLoop
        }
        if let rl { CFRunLoopStop(rl) }
        // The thread clears its state as it ends; until then `running` stays true for a moment.
        for _ in 0..<100 where lock.withLock({ thread != nil }) { usleep(2000) }
    }

    fileprivate func handle(_ type: CGEventType, _ e: CGEvent) {
        switch type {
        case .tapDisabledByTimeout, .tapDisabledByUserInput:
            if let tap = lock.withLock({ tap }) { CGEvent.tapEnable(tap: tap, enable: true) }
            matcher.reset()
            return
        case .leftMouseDown, .rightMouseDown, .otherMouseDown:
            matcher.reset()  // a click can move the caret
            return
        case .keyDown: break
        default: return
        }
        if e.getIntegerValueField(.eventSourceUserData) == QuickPromptsMac.eventMarker { return }  // our own ⌫ and ⌘V
        if !acceptInjected && e.getIntegerValueField(.eventSourceStateID) == Int64(CGEventSourceStateID.privateState.rawValue) { return }
        let (version, isComposing) = lock.withLock { () -> (Int, Bool) in
            seq += 1
            return (triggersVersion, composing)
        }
        if IsSecureEventInputEnabled() { return matcher.reset() }  // a password field: see nothing, keep nothing
        if version != seenVersion {
            matcher.setTriggers(lock.withLock { entries })
            seenVersion = version
        }
        let pid = e.getIntegerValueField(.eventTargetUnixProcessID)
        if pid != lastPid {  // another app: the caret is somewhere else
            matcher.reset()
            lastPid = pid
        }
        let flags = e.flags
        if flags.contains(.maskCommand) || flags.contains(.maskControl) { return matcher.reset() }  // a shortcut
        let code = Int(e.getIntegerValueField(.keyboardEventKeycode))
        switch code {
        case kVK_Space:
            if flags.contains(.maskShift) { return matcher.char(" ") }  // ⇧Space: a space, never an expansion
            if let hit = matcher.space() { matched(hit, pid: pid_t(truncatingIfNeeded: pid)) }
            return
        case kVK_Delete: return matcher.backspace()
        case kVK_Return, kVK_ANSI_KeypadEnter, kVK_Tab, kVK_Escape, kVK_LeftArrow, kVK_RightArrow, kVK_UpArrow, kVK_DownArrow,
             kVK_Home, kVK_End, kVK_PageUp, kVK_PageDown, kVK_ForwardDelete, kVK_Help:
            return matcher.reset()
        default: break
        }
        if isComposing { return matcher.reset() }
        var len = 0
        var chars = [UniChar](repeating: 0, count: 8)
        e.keyboardGetUnicodeString(maxStringLength: chars.count, actualStringLength: &len, unicodeString: &chars)
        guard len > 0 else { return matcher.reset() }  // a dead key or a function key
        for c in String(utf16CodeUnits: chars, count: len) {
            let odd = c.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) || (0xF700...0xF8FF).contains($0.value) }
            if c.isWhitespace || odd { matcher.reset() } else { matcher.char(c) }
        }
    }

    private func matched(_ hit: (tag: Int, erase: Int), pid: pid_t) {
        let (id, s): (String?, UInt64) = lock.withLock { (hit.tag < ids.count ? ids[hit.tag] : nil, seq) }
        guard let id, pid != skipOwnPid else { return }
        DispatchQueue.main.async { QuickPromptsMac.shared.typed(id: id, erase: hit.erase, keySeq: s, pid: pid) }
    }
}

private let listenerCallback: CGEventTapCallBack = { _, type, event, info in
    if let info { Unmanaged<Listener>.fromOpaque(info).takeUnretainedValue().handle(type, event) }
    return Unmanaged.passUnretained(event)
}

// For SwiftUI (Settings › QuickPrompts): the permission and whether the listener runs.
final class QuickPromptsStatus: ObservableObject {
    static let shared = QuickPromptsStatus()
    @Published var accessibility = QuickPromptsMac.accessibilityGranted
    @Published var listening = false
    func refresh() {
        accessibility = QuickPromptsMac.accessibilityGranted
        listening = QuickPromptsMac.shared.listening
    }
}
