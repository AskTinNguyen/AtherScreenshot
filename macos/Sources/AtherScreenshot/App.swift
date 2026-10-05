import AppKit
import ServiceManagement

enum After { case normal, pin, ocr, edit, redact, upload }

private let kCliNote = Notification.Name("com.ather.screenshot.cli")

@main
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    static var shared: AppDelegate!

    static func main() {
        var args = Array(CommandLine.arguments.dropFirst())
        // Relaunched by the update helper (the old copy has quit by then).
        let afterUpdate = args.first == "--after-update", updateFailed = args.first == "--update-failed"
        if afterUpdate || updateFailed { args.removeFirst() }
        if args.first == "--write-iconset", args.count >= 2 {
            exit(Logo.writeIconset(URL(fileURLWithPath: args[1])) ? 0 : 1)
        }
        if args.first == "--help" || args.first == "-h" {
            print("""
            Ather Screenshot \(version)
            usage: AtherScreenshot region|fullscreen|monitor|window|last|scroll|ruler|ocr|color
                                   record|gif|recordwindow|stop|pause|history|palette|folder|settings|quit
                                   [--pin|--edit|--upload|--redact|--ocr] [--delay N]
                   AtherScreenshot edit|pin|upload <file>
            Any shortcut name from Settings works as a command too (e.g. CaptureRegionPin).
            If the app is already running, the command goes to that instance.
            Commands from a terminal are trusted. Commands from atherscreenshot:// links or from
            tools without a terminal (Shortcuts, Raycast, cron…) ask first and never upload or open files.
            """)
            exit(0)
        }
        // Forward commands to the running instance.
        let me = ProcessInfo.processInfo.processIdentifier
        let others = NSRunningApplication.runningApplications(withBundleIdentifier: kBundleID).filter { $0.processIdentifier != me }
        if !others.isEmpty {
            if !args.isEmpty {
                let absolute = args.map { a -> String in
                    a.hasPrefix("-") || !FileManager.default.fileExists(atPath: a) ? a : URL(fileURLWithPath: a).standardizedFileURL.path
                }
                // The token proves the command came from this user's own CLI, not some other process.
                // Only a command typed in a terminal carries the token; launches by other apps don't.
                let payload = (interactiveLaunch ? cliToken() : "") + "\u{1e}" + absolute.joined(separator: "\u{1f}")
                DistributedNotificationCenter.default().postNotificationName(kCliNote, object: payload, userInfo: nil, deliverImmediately: true)
            } else {
                others.first?.activate()
            }
            exit(0)
        }
        let app = NSApplication.shared
        let d = AppDelegate()
        shared = d
        d.launchArgs = args
        d.afterUpdate = afterUpdate
        d.updateFailed = updateFailed
        app.delegate = d
        app.setActivationPolicy(.accessory)
        app.run()
    }

    // A per-user secret in Application Support (mode 0600). Distributed notifications can be posted by any
    // process, including sandboxed ones, so commands without it are treated like links from the web.
    // Started from a terminal (or a script attached to one), as opposed to by another app with arguments.
    static var interactiveLaunch: Bool { isatty(STDIN_FILENO) != 0 || isatty(STDERR_FILENO) != 0 }

    static func cliToken() -> String {
        let u = Settings.supportFolder.appendingPathComponent("cli-token")
        if let t = try? String(contentsOf: u, encoding: .utf8), t.count >= 32 { return t }
        let t = (0..<4).map { _ in UUID().uuidString }.joined()
        FileManager.default.createFile(atPath: u.path, contents: Data(t.utf8), attributes: [.posixPermissions: 0o600])
        return t
    }

    static var version: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "dev" }

    let s = Settings.shared
    private var statusItem: NSStatusItem!
    private var launchArgs: [String] = []
    private var afterUpdate = false
    private var updateFailed = false
    private var trackedWindows: [NSWindow] = []
    private var hotkeysSuspended = false

    private(set) var lastImage: CGImage?
    private(set) var lastURL: URL?
    private var lastScale: CGFloat = 2
    private var lastRegion: CGRect?
    private var nameInfo = NameInfo()
    private var delayTimer: Timer?

    // MARK: lifecycle

    func applicationDidFinishLaunching(_ n: Notification) {
        NSApp.applicationIconImage = Logo.appIcon
        Library.shared.loadIfNeeded()
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = Logo.menuBarIcon
        statusItem.button?.toolTip = kProductName
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
        installMainMenu()

        s.onChange = { [weak self] key in
            if key.hasPrefix("Hotkey.") { self?.registerHotkeys() }
        }
        registerHotkeys()
        let token = AppDelegate.cliToken()
        DistributedNotificationCenter.default().addObserver(forName: kCliNote, object: nil, queue: .main) { [weak self] n in
            guard let s = n.object as? String else { return }
            let parts = s.components(separatedBy: "\u{1e}")
            let trusted = parts.count == 2 && parts[0] == token
            self?.runCli((parts.last ?? "").components(separatedBy: "\u{1f}"), trusted: trusted)
        }
        try? FileManager.default.createDirectory(at: s.capturesFolder, withIntermediateDirectories: true)
        Updater.cleanUp()  // only this, the running instance, clears an earlier update's leftovers
        UpdateController.shared.start()

        if afterUpdate {
            Toast.shared.show("Updated to Ather Screenshot \(AppDelegate.version)", "Your settings, captures and gallery are just as you left them.", ms: 6000)
        } else if updateFailed {
            UpdateController.shared.failed("The new version couldn't be put in place, so this one kept running.")
        } else if !launchArgs.isEmpty {
            runCli(launchArgs, trusted: AppDelegate.interactiveLaunch)
        } else if !UserDefaults.standard.bool(forKey: "Welcomed") {
            UserDefaults.standard.set(true, forKey: "Welcomed")
            let hk = Hotkey.display(s.hotkey(.region)), pal = Hotkey.display(s.hotkey(.palette))
            Toast.shared.show("Ather Screenshot is running", "\(hk) captures a region, \(pal) opens the command palette. It lives in the menu bar.", ms: 9000)
            if !Capture.hasPermission() { _ = Capture.ensurePermission() }
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { Palette.shared.show(paletteItems()) }
        return false
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        for u in urls {
            if u.isFileURL { openToEdit(u) }
            else if u.scheme == "atherscreenshot" {  // atherscreenshot://region?pin
                var args = [u.host ?? ""]
                for q in URLComponents(url: u, resolvingAgainstBaseURL: false)?.queryItems ?? [] {
                    args.append("--" + q.name)
                    if let v = q.value { args.append(v) }
                }
                runCli(args, trusted: false)  // any web page can open these links
            }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    // Finish an active recording and pending writes before quitting, so nothing is lost.
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        UpdateController.shared.quitting = true  // a download finishing now must not install or relaunch
        let finishWrites = {
            Output.waitForPendingSaves(timeout: 5)  // first: their completions may schedule library saves
            Library.shared.flush()
        }
        guard let r = Recorder.current, r.hasFootage else {
            Recorder.current?.stop()  // countdown or still starting: nothing recorded yet, just cancel
            finishWrites()
            return .terminateNow
        }
        Toast.shared.show("Saving the recording…", "Quitting when it's done")
        var replied = false
        let finish = {
            guard !replied else { return }
            replied = true
            finishWrites()
            sender.reply(toApplicationShouldTerminate: true)
        }
        Recorder.onFinished.append { DispatchQueue.main.async { finish() } }
        r.stop()
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) { finish() }  // never hang a logout
        return .terminateLater
    }

    // Show in the Dock and ⌘-Tab while a real window (editor, history, settings) is open.
    func windowOpened(_ w: NSWindow) {
        trackedWindows.append(w)
        NSApp.setActivationPolicy(.regular)
        NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: w, queue: .main) { [weak self] n in
            guard let self else { return }
            self.trackedWindows.removeAll { $0 === n.object as AnyObject }
            if self.trackedWindows.isEmpty {
                NSApp.setActivationPolicy(.accessory)
            }
        }
    }

    private func installMainMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About \(kProductName)", action: #selector(menuAbout), keyEquivalent: "").target = self
        appMenu.addItem(withTitle: "Settings…", action: #selector(menuSettings), keyEquivalent: ",").target = self
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        appMenu.addItem(withTitle: "Quit \(kProductName)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)
        let editItem = NSMenuItem()
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = edit
        main.addItem(editItem)
        NSApp.mainMenu = main
    }

    // MARK: hotkeys

    func registerHotkeys() {
        HotkeyCenter.shared.unregisterAll()
        var problems: [String: String] = [:]
        if !hotkeysSuspended {
            for c in kCmds {
                let text = s.hotkey(c.cmd)
                if text.isEmpty { continue }
                guard let hk = Hotkey.parse(text) else {
                    problems[c.cmd.rawValue] = "“\(text)” isn’t a valid shortcut"
                    continue
                }
                if !HotkeyCenter.shared.register(hk, action: { [weak self] in self?.execute(c.cmd) }) {
                    problems[c.cmd.rawValue] = "\(hk.display) is taken by macOS or another app"
                }
            }
        }
        HotkeyStatus.shared.problems = problems
        if !problems.isEmpty && !hotkeysSuspended && !reportedProblems {
            reportedProblems = true
            let list = problems.compactMap { k, v in Cmd(rawValue: k).map { "\(cmdDef($0).title): \(v)" } }.sorted().joined(separator: "\n")
            Toast.shared.show("Some shortcuts are unavailable", list, ms: 9000) { SettingsWindow.show() }
        }
    }
    private var reportedProblems = false

    // While a shortcut field is recording, our own hotkeys must not fire.
    func suspendHotkeys(_ on: Bool) {
        guard on != hotkeysSuspended else { return }
        hotkeysSuspended = on
        registerHotkeys()
    }

    // MARK: status menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        func add(_ c: Cmd, _ title: String? = nil) {
            let d = cmdDef(c)
            let item = NSMenuItem(title: title ?? d.title, action: #selector(menuCommand(_:)), keyEquivalent: "")
            item.representedObject = c.rawValue
            item.target = self
            item.image = NSImage(systemSymbolName: d.icon, accessibilityDescription: nil)
            if let hk = Hotkey.parse(s.hotkey(c)), !hk.menuKey.isEmpty {
                item.keyEquivalent = hk.menuKey
                item.keyEquivalentModifierMask = hk.mods
            }
            if let on = toggleState(c) { item.state = on ? .on : .off }
            menu.addItem(item)
        }
        if let r = Recorder.current {
            add(.stopRecording, "Stop recording (\(formatTime(r.elapsed)))")
            add(.pauseRecording, r.isPaused ? "Resume recording" : "Pause recording")
            menu.addItem(.separator())
        }
        for c in [Cmd.region, .regionEdit, .fullscreen, .monitor, .window, .scrolling, .ocr] { add(c) }
        menu.addItem(.separator())
        for c in [Cmd.recordVideo, .recordGif, .recordWindow] { add(c) }
        menu.addItem(.separator())
        for c in [Cmd.colorPicker, .ruler, .history, .palette] { add(c) }
        menu.addItem(.separator())
        let more = NSMenuItem(title: "Options", action: nil, keyEquivalent: "")
        let sub = NSMenu()
        let save = menu
        for c in [Cmd.toggleClipboard, .toggleSave, .toggleCursor, .toggleAutoRedact, .toggleSystemAudio, .toggleMic, .toggleClicks, .toggleKeys, .toggleGamepad, .toggleLogin] {
            let d = cmdDef(c)
            let i = NSMenuItem(title: d.title, action: #selector(menuCommand(_:)), keyEquivalent: "")
            i.representedObject = c.rawValue
            i.target = self
            i.state = toggleState(c) == true ? .on : .off
            sub.addItem(i)
        }
        more.submenu = sub
        save.addItem(more)
        add(.openFolder)
        add(.settings, "Settings…")
        menu.addItem(.separator())
        if let u = UpdateController.shared.available {
            // Not a palette command: it starts the update itself.
            let item = NSMenuItem(title: "Update to version \(u.version)…", action: #selector(menuInstallUpdate), keyEquivalent: "")
            item.target = self
            item.image = NSImage(systemSymbolName: "arrow.down.circle", accessibilityDescription: nil)
            menu.addItem(item)
        } else {
            add(.checkUpdates)
        }
        add(.about)
        add(.quit)
    }

    @objc private func menuCommand(_ item: NSMenuItem) {
        guard let raw = item.representedObject as? String, let c = Cmd(rawValue: raw) else { return }
        // Let the menu close before a capture freezes the screen.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { self.execute(c) }
    }
    @objc private func menuAbout() { execute(.about) }
    @objc func menuInstallUpdate() { UpdateController.shared.install() }
    @objc private func menuSettings() { execute(.settings) }

    func toggleState(_ c: Cmd) -> Bool? {
        switch c {
        case .toggleClipboard: return s.bool("CopyToClipboard")
        case .toggleSave: return s.bool("SaveToFile")
        case .toggleCursor: return s.bool("CaptureCursor")
        case .toggleAutoRedact: return s.bool("AutoRedact")
        case .toggleSystemAudio: return s.bool("RecordSystemAudio")
        case .toggleMic: return s.bool("RecordMicrophone")
        case .toggleClicks: return s.bool("ShowClicks")
        case .toggleKeys: return s.bool("ShowKeys")
        case .toggleGamepad: return s.bool("ShowGamepad")
        case .toggleLogin: return SMAppService.mainApp.status == .enabled
        default: return nil
        }
    }

    func paletteItems() -> [PaletteItem] {
        var items: [PaletteItem] = kCmds.filter { $0.cmd != .palette }.compactMap { d in
            if (d.cmd == .stopRecording || d.cmd == .pauseRecording) && !Recorder.isActive { return nil }
            return PaletteItem(id: d.cmd.rawValue, title: d.title, keywords: d.keywords, icon: d.icon, hint: Hotkey.display(s.hotkey(d.cmd)),
                               toggled: toggleState(d.cmd)) { [weak self] in self?.execute(d.cmd) }
        }
        for u in Output.listCaptures().prefix(5) {
            items.append(PaletteItem(id: "recent", title: "Open recent: \(u.lastPathComponent)", keywords: "recent last history file", icon: "clock",
                                     hint: Library.dateFormat.string(from: (try? u.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? Date())) {
                if u.pathExtension.lowercased() == "gif" { Output.open(u) } else { self.openToEdit(u) }
            })
        }
        return items
    }

    // MARK: commands

    // False while running a command a link or another app asked for: nothing leaves the machine,
    // even when "After capture" is set to upload.
    private var uploadAllowed = true

    func execute(_ c: Cmd, after: After? = nil, delay: Int? = nil, untrusted: Bool = false) {
        uploadAllowed = !untrusted
        if let delay, delay > 0 { return delayed(c, seconds: delay, after: after, untrusted: untrusted) }
        if cmdDef(c).capture {
            Palette.shared.close()
            Overlay.active?.finish(nil)
        }
        switch c {
        case .palette: Palette.shared.show(paletteItems())
        case .region: captureRegion(after ?? .normal)
        case .regionEdit: captureRegion(.edit)
        case .regionPin: captureRegion(.pin)
        case .regionRedact: captureRegion(.redact)
        case .regionUpload: captureRegion(.upload)
        case .ocr: captureRegion(.ocr)
        case .fullscreen: captureFull(display: false, after ?? .normal)
        case .monitor: captureFull(display: true, after ?? .normal)
        case .window: captureActiveWindow(after ?? .normal)
        case .lastRegion: captureLastRegion(after ?? .normal)
        case .regionDelayed: delayed(.region, seconds: s.int("DelaySeconds"), after: after, untrusted: untrusted)
        case .fullscreenDelayed: delayed(.fullscreen, seconds: s.int("DelaySeconds"), after: after, untrusted: untrusted)
        case .colorPicker: pickColor()
        case .ruler: ruler()
        case .scrolling: scrolling(after ?? .normal)
        case .recordVideo, .recordGif:
            if Recorder.isActive { Recorder.current?.stop() } else { record(gif: c == .recordGif) }
        case .recordWindow:
            if Recorder.isActive { Recorder.current?.stop() } else { recordWindow() }
        case .stopRecording:
            if Recorder.isActive { Recorder.current?.stop() } else if ScrollCapture.active != nil { ScrollCapture.cancel() } else { Toast.shared.show("Not recording") }
        case .pauseRecording: Recorder.current?.togglePause()
        case .checkUpdates: UpdateController.shared.check(manual: true)
        case .history: GalleryWindow.show()
        case .editLast:
            if let img = lastImage { Editor.open(img, scale: lastScale) } else if let u = lastImageURL { Editor.open(url: u) } else { Toast.shared.show("Nothing captured yet") }
        case .openImage: openImage()
        case .pinLast:
            if let img = lastImage ?? lastImageURL.flatMap(CGImage.load) { Pin.show(img, scale: lastScale) } else { Toast.shared.show("Nothing captured yet") }
        case .copyLast:
            if let img = lastImage { copyImage(img); Toast.shared.show("Copied to clipboard") }
            else if let u = lastURL ?? Output.listCaptures().first { copyFile(u); Toast.shared.show("Copied", u.lastPathComponent) }
            else { Toast.shared.show("Nothing captured yet") }
        case .openLast:
            if let u = lastURL ?? Output.listCaptures().first { Output.open(u) } else { Toast.shared.show("Nothing saved yet") }
        case .renameLast:
            if let u = lastURL ?? Output.listCaptures().first { promptRename(u) } else { Toast.shared.show("Nothing saved yet") }
        case .uploadLast:
            if let u = lastURL ?? Output.listCaptures().first { upload(u) } else { Toast.shared.show("Nothing saved yet") }
        case .uploadFile: uploadFile()
        case .openFolder:
            try? FileManager.default.createDirectory(at: s.capturesFolder, withIntermediateDirectories: true)
            Output.open(s.capturesFolder)
        case .closeAllPins: Pin.closeAll()
        case .toggleClipboard: notifyToggle(c, s.toggle("CopyToClipboard"))
        case .toggleSave: notifyToggle(c, s.toggle("SaveToFile"))
        case .toggleCursor: notifyToggle(c, s.toggle("CaptureCursor"))
        case .toggleAutoRedact: notifyToggle(c, s.toggle("AutoRedact"))
        case .toggleSystemAudio: notifyToggle(c, s.toggle("RecordSystemAudio"))
        case .toggleMic: notifyToggle(c, s.toggle("RecordMicrophone"))
        case .toggleClicks: notifyToggle(c, s.toggle("ShowClicks"))
        case .toggleKeys: notifyToggle(c, s.toggle("ShowKeys"))
        case .toggleGamepad: notifyToggle(c, s.toggle("ShowGamepad"))
        case .toggleLogin:
            setLaunchAtLogin(SMAppService.mainApp.status != .enabled)
            notifyToggle(c, SMAppService.mainApp.status == .enabled)
        case .settings: SettingsWindow.show()
        case .about: about()
        case .quit: NSApp.terminate(nil)
        }
    }

    private var lastImageURL: URL? {
        (lastURL.map { [$0] } ?? Output.listCaptures()).first { ["png", "jpg", "jpeg", "heic", "tiff"].contains($0.pathExtension.lowercased()) }
    }

    private func notifyToggle(_ c: Cmd, _ on: Bool) { Toast.shared.show("\(cmdDef(c).title): \(on ? "On" : "Off")") }

    func setLaunchAtLogin(_ on: Bool) {
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            Toast.shared.show("Couldn't change Launch at login", error.localizedDescription + " (move the app to /Applications first)")
        }
    }

    private func delayed(_ c: Cmd, seconds: Int, after: After?, untrusted: Bool = false) {
        delayTimer?.invalidate()
        var left = max(1, seconds)
        let id = Toast.shared.post("Capturing in \(left)…", cmdDef(c).title, ms: (left + 1) * 1000)
        delayTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] t in
            left -= 1
            if left <= 0 {
                t.invalidate()
                Toast.shared.hide()
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { self?.execute(c, after: after, untrusted: untrusted) }
            } else {
                Toast.shared.update(id, body: "\(cmdDef(c).title) in \(left)…")
            }
        }
    }

    // MARK: capture pipeline

    private func withPermission(_ body: @escaping () -> Void) {
        if Capture.hasPermission() { return body() }
        Task { @MainActor in
            if await Capture.resolvePermission() { return body() }
            _ = Capture.ensurePermission()
            Toast.shared.show("Screen Recording permission needed", CaptureError.permission.errorDescription!, ms: 9000) {
                NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!)
            }
        }
    }

    private func frontInfo() -> NameInfo {
        let w = Capture.activeWindow()
        return NameInfo(app: w?.app ?? NSWorkspace.shared.frontmostApplication?.localizedName ?? "", window: w?.title ?? "")
    }

    private func snapshotThen(_ body: @escaping (Snapshot) -> Void) {
        withPermission {
            Toast.shared.hide()
            let cursor = self.s.bool("CaptureCursor")
            Task { @MainActor in
                do { body(try await Capture.snapshot(cursor: cursor)) }
                catch { Toast.shared.show("Capture failed", error.localizedDescription) }
            }
        }
    }

    private func captureRegion(_ after: After) {
        let allow = uploadAllowed  // fixed now; later commands must not change it for this capture
        let info = frontInfo()
        snapshotThen { snap in
            Overlay.run(.region, snapshot: snap) { [weak self] r in
                guard let self, let r else { return }
                self.lastRegion = r.rect
                self.nameInfo = r.window.map { NameInfo(app: $0.app, window: $0.title) } ?? info
                self.deliver(snap.crop(r.rect), scale: snap.scale(for: r.rect), where: r.rect, after, allowUpload: allow)
            }
        }
    }

    private func captureFull(display: Bool, _ after: After) {
        let allow = uploadAllowed  // fixed now; later commands must not change it for this capture
        let info = frontInfo()
        let mouse = Geo.mouse
        snapshotThen { snap in
            let rect = display ? (snap.shot(at: mouse)?.frame ?? snap.bounds) : snap.bounds
            self.nameInfo = display ? info : NameInfo()
            self.deliver(snap.crop(rect), scale: snap.scale(for: rect), where: rect, after, allowUpload: allow)
        }
    }

    private func captureActiveWindow(_ after: After) {
        let allow = uploadAllowed  // fixed now; later commands must not change it for this capture
        withPermission {
            guard let w = Capture.activeWindow() else { return Toast.shared.show("No window to capture") }
            let cursor = self.s.bool("CaptureCursor")
            Task { @MainActor in
                do {
                    let img = try await Capture.window(w.id, cursor: cursor)
                    self.nameInfo = NameInfo(app: w.app, window: w.title)
                    self.deliver(img, scale: CGFloat(img.width) / max(1, w.frame.width), where: w.frame, after, allowUpload: allow)
                } catch { Toast.shared.show("Capture failed", error.localizedDescription) }
            }
        }
    }

    private func captureLastRegion(_ after: After) {
        let allow = uploadAllowed  // fixed now; later commands must not change it for this capture
        guard let r = lastRegion else { return captureRegion(after) }
        withPermission {
            let cursor = self.s.bool("CaptureCursor")
            self.nameInfo = Capture.window(at: r.center, in: Capture.windows()).map { NameInfo(app: $0.app, window: $0.title) } ?? NameInfo()
            Task { @MainActor in
                do {
                    let img = try await Capture.rect(r, cursor: cursor)
                    self.deliver(img, scale: CGFloat(img.width) / max(1, r.width), where: r, after, allowUpload: allow)
                } catch { Toast.shared.show("Capture failed", error.localizedDescription) }
            }
        }
    }

    private func pickColor() {
        snapshotThen { snap in
            Overlay.run(.color, snapshot: snap) { r in
                guard let r, let c = snap.color(at: r.point) else { return }
                let rgb = c.usingColorSpace(.sRGB)!
                let text = c.hex
                copyText(text)
                Toast.shared.show("\(text) copied", String(format: "rgb(%.0f, %.0f, %.0f)", rgb.redComponent * 255, rgb.greenComponent * 255, rgb.blueComponent * 255))
            }
        }
    }

    private func ruler() { snapshotThen { snap in Overlay.run(.ruler, snapshot: snap) { _ in } } }

    private func scrolling(_ after: After) {
        let allow = uploadAllowed  // fixed now; later commands must not change it for this capture
        snapshotThen { snap in
            Overlay.run(.region, snapshot: snap) { [weak self] r in
                guard let self, let r else { return }
                self.nameInfo = r.window.map { NameInfo(app: $0.app, window: $0.title) } ?? NameInfo()
                Toast.shared.show("Scrolling capture…", "Esc stops", ms: 600_000)
                ScrollCapture.start(region: r.rect, delayMs: self.s.int("ScrollDelayMs"), maxFrames: self.s.int("ScrollMaxFrames")) { img, frames, err in
                    Toast.shared.hide()
                    if let err { return Toast.shared.show("Scrolling capture failed", err, ms: 8000) }
                    self.deliver(img, scale: snap.scale(for: r.rect), where: r.rect, after, note: "\(frames) frames", allowUpload: allow)
                }
            }
        }
    }

    private func record(gif: Bool) {
        snapshotThen { snap in
            Overlay.run(.region, snapshot: snap) { [weak self] r in
                guard let r else { return }
                Recorder.start(gif: gif, target: .region(r.rect))
                self?.recordingChanged()
            }
        }
    }

    private func recordWindow() {
        snapshotThen { snap in
            Overlay.run(.window, snapshot: snap) { [weak self] r in
                guard let w = r?.window else { return }
                Recorder.start(gif: false, target: .window(w))
                self?.recordingChanged()
            }
        }
    }

    func recordingChanged() {
        statusItem.button?.contentTintColor = Recorder.isActive ? .systemRed : nil
    }

    // Keeps "last capture" commands pointing at the right file after a rename or trash elsewhere.
    func fileMoved(from a: URL, to b: URL) { if lastURL == a { lastURL = b } }
    func filesRemoved(_ us: Set<URL>) { if let u = lastURL, us.contains(u) { lastURL = nil } }

    func setLast(_ img: CGImage?, url: URL?) {
        if let img { lastImage = img }
        if let url { lastURL = url }
        if img != nil && url == nil { lastURL = nil }
    }

    // `redacted < 0`: auto-redact hasn't run for this image yet.
    func deliver(_ img: CGImage?, scale: CGFloat, where rect: CGRect?, _ after: After, redacted: Int = -1, note: String = "", allowUpload: Bool = true) {
        guard let img else { return Toast.shared.show("Capture failed") }
        if redacted < 0 && (after == .redact || (s.bool("AutoRedact") && after != .ocr)) {
            Toast.shared.show("Redacting sensitive text…", ms: 10000)
            OCR.async({ () -> (CGImage, Int) in
                let rects = OCR.findSensitive(try OCR.words(img))
                return (try OCR.pixelate(img, rects: rects), rects.count)
            }) { [weak self] r in
                Toast.shared.hide()
                switch r {
                case .success(let (clean, n)):
                    self?.deliver(clean, scale: scale, where: rect, after == .redact ? .normal : after, redacted: n, note: note, allowUpload: allowUpload)
                case .failure(let e):
                    // Never hand out an unredacted image when redaction was asked for: let the user do it by hand.
                    Toast.shared.show("Auto-redact failed — nothing was copied, saved or uploaded",
                                      "\(e.localizedDescription) Opened in the editor so you can redact it yourself.", ms: 9000)
                    Editor.open(img, scale: scale, info: self?.nameInfo ?? NameInfo())
                }
            }
            return
        }
        lastImage = img
        lastURL = nil
        lastScale = scale
        var dims = "\(img.width) × \(img.height)"
        if redacted > 0 { dims += "  ·  \(redacted) redacted" }
        if !note.isEmpty { dims += "  ·  " + note }
        let afterSetting = s.string("AfterCapture")

        if after == .ocr {
            Toast.shared.show("Reading text…", dims, ms: 15000)
            OCR.async({ try OCR.text(img) }) { r in
                switch r {
                case .failure(let e): Toast.shared.show("OCR failed", e.localizedDescription)
                case .success(let t) where t.isEmpty: Toast.shared.show("No text found", "Try a larger or sharper region.")
                case .success(let t):
                    copyText(t)
                    Toast.shared.show("Text copied", t.count > 280 ? String(t.prefix(280)) + "…" : t, ms: self.s.int("ToastMs") + 2000)
                }
            }
            return
        }
        if after == .edit || (after == .normal && afterSetting == "edit") {
            Editor.open(img, scale: scale, info: nameInfo)  // the editor copies/saves the result on Done
            return
        }
        let copied = s.bool("CopyToClipboard") && copyImage(img)
        let pin = after == .pin || afterSetting == "pin"
        if pin { Pin.show(img, at: rect, scale: scale) }
        let upload = allowUpload && (after == .upload || afterSetting == "upload")
        var toast: UInt64 = 0
        let save = s.bool("SaveToFile")
        if s.bool("ShowToast") && !pin && !upload {
            let info = nameInfo
            toast = Toast.shared.post(copied ? "Copied to clipboard" : "Captured", dims + (save ? "  ·  saving…" : ""), image: img) {
                Editor.open(img, scale: scale, info: info)
            }
        }
        guard save || upload else { return }
        var info = nameInfo
        info.w = img.width
        info.h = img.height
        // Uploading without saving still needs a file: use a temporary one and delete it afterwards.
        let url = save ? Output.newCaptureURL(ext: "png", info: info)
            : Output.makeCaptureURL(base: FileManager.default.temporaryDirectory.appendingPathComponent(kAppName), ext: "png", info: info)
        Output.savePNG(img, to: url) { [weak self] ok in
            guard let self else { return }
            if !ok { return Toast.shared.show("Save failed", url.path) }
            if save && self.lastImage == img { self.lastURL = url }
            if toast != 0 { Toast.shared.update(toast, body: dims + "  ·  " + url.lastPathComponent) }
            if upload { self.upload(url, deleteAfter: !save) }
            guard save else { return }
            if afterSetting == "open" { Output.open(url) }
            if self.s.bool("AskForName") { self.promptRename(url) }
        }
    }

    func upload(_ url: URL, deleteAfter: Bool = false) {
        let cfg = s.uploadConfig
        if let p = Upload.problem(cfg) {
            return Toast.shared.show("Upload not configured", p) { SettingsWindow.show() }
        }
        Toast.shared.show("Uploading…", url.lastPathComponent, ms: 60000)
        Upload.upload(url, cfg) { r in
            if deleteAfter { try? FileManager.default.removeItem(at: url) }
            switch r {
            case .failure(let e): Toast.shared.show("Upload failed", e.localizedDescription, ms: 8000)
            case .success(let link):
                copyText(link)
                Toast.shared.show("Link copied", link, ms: self.s.int("ToastMs") + 2500) { NSWorkspace.shared.open(URL(string: link)!) }
            }
        }
    }

    private func uploadFile() {
        activateApp()
        let p = NSOpenPanel()
        p.directoryURL = s.capturesFolder
        p.canChooseFiles = true
        if p.runModal() == .OK, let u = p.url { upload(u) }
    }

    private func openImage() {
        activateApp()
        let p = NSOpenPanel()
        p.allowedContentTypes = MediaFiles.contentTypes(pictures: true, videos: true)
        p.directoryURL = s.capturesFolder
        if p.runModal() == .OK, let u = p.url { openToEdit(u) }
    }

    // Any file opened from outside: videos go to the video editor, everything else to the image editor.
    // Edits are saved as new captures; the original is never changed.
    func openToEdit(_ u: URL) {
        if MediaFiles.isVideo(u) { VideoEditor.open(u) } else { Editor.open(url: u) }
    }

    func promptRename(_ url: URL) {
        Palette.shared.prompt("Name this capture  ·  ↩ renames, ⎋ keeps the current name", initial: url.deletingPathExtension().lastPathComponent) { [weak self] name in
            guard let n = Output.rename(url, to: name) else { return Toast.shared.show("Rename failed", url.lastPathComponent) }
            Library.shared.moved(from: url, to: n)  // also updates lastURL
            Toast.shared.show("Renamed", n.lastPathComponent)
        }
    }

    private func about() {
        activateApp()
        let a = NSAlert()
        a.icon = Logo.appIcon
        a.messageText = "\(kProductName) \(AppDelegate.version)"
        a.informativeText = """
        A tiny, fast screenshot and recording tool for macOS.
        Region, window and scrolling captures, annotation, OCR, auto-redact, MP4/GIF recording, pins, history and upload.

        Command palette: \(Hotkey.display(s.hotkey(.palette)))
        Captures: \(s.capturesFolder.path.replacingOccurrences(of: NSHomeDirectory(), with: "~"))
        """
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Open Settings")
        if a.runModal() == .alertSecondButtonReturn { SettingsWindow.show() }
    }

    // MARK: command line

    // `trusted`: typed by the user (launch arguments or our own CLI). Links and unknown processes must not be
    // able to send files or screenshots off the machine, and other commands need the user's OK.
    func runCli(_ args: [String], trusted: Bool = true) {
        guard let first = args.first, !first.isEmpty else { return }
        if !trusted {
            let lower = args.map { $0.lowercased() }
            let sends = lower.contains("--upload") || ["upload", "edit", "pin"].contains(lower[0])
                || [Cmd.regionUpload, .uploadLast, .uploadFile].contains { $0.rawValue.lowercased() == lower[0] }
            if sends {
                return Toast.shared.show("Blocked a request from a link or another app", "“\(args.joined(separator: " "))” can only be run from the command line.", ms: 8000)
            }
            activateApp()
            let a = NSAlert()
            a.messageText = "Run “\(args.joined(separator: " "))”?"
            a.informativeText = "A link or another app asked Ather Screenshot to run this command."
            a.addButton(withTitle: "Run")
            a.addButton(withTitle: "Cancel")
            guard a.runModal() == .alertFirstButtonReturn else { return }
        }
        var after: After?
        var delay: Int?
        var i = 1
        var files: [String] = []
        while i < args.count {
            switch args[i].lowercased() {
            case "--pin": after = .pin
            case "--edit": after = .edit
            case "--upload": after = .upload
            case "--redact": after = .redact
            case "--ocr": after = .ocr
            case "--delay":
                i += 1
                delay = i < args.count ? Int(args[i]) : nil
            default: files.append(args[i])
            }
            i += 1
        }
        switch first.lowercased() {
        case "edit": files.forEach { openToEdit(URL(fileURLWithPath: $0)) }; return
        case "pin":
            for f in files.map({ URL(fileURLWithPath: $0) }) {
                if MediaFiles.isVideo(f) { Toast.shared.show("Videos can't be pinned", f.lastPathComponent) }
                else if let img = CGImage.load(f) { Pin.show(img) }
            }
            return
        case "upload": files.forEach { upload(URL(fileURLWithPath: $0)) }; return
        default: break
        }
        let cmd = kCliNames[first.lowercased()] ?? Cmd.allCases.first { $0.rawValue.lowercased() == first.lowercased() }
        guard let cmd else {
            if trusted, FileManager.default.fileExists(atPath: first) { openToEdit(URL(fileURLWithPath: first)) }
            else { Toast.shared.show("Unknown command", first) }
            return
        }
        // Region-based commands honour --pin/--edit/...; others ignore it.
        let regional: Set<Cmd> = [.region, .fullscreen, .monitor, .window, .lastRegion, .scrolling]
        execute(cmd, after: regional.contains(cmd) ? after : nil, delay: delay, untrusted: !trusted)
    }
}
