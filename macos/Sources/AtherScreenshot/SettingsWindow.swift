import AppKit
import Carbon.HIToolbox
import ServiceManagement
import SwiftUI

private enum Kind {
    case toggle
    case number(ClosedRange<Int>, Int, String)
    case text(String, secure: Bool)
    case choice([(String, String)])
    case folder
    case login
    case hotkey(Cmd)
}

private struct Row: Identifiable {
    let key: String
    let title: String
    let section: String
    var keywords = ""
    let kind: Kind
    var shownWhen: String? = nil  // a toggle this row belongs to: hidden while it's off (unless searched for)
    var id: String { key }
}

private let rows: [Row] = {
    var r: [Row] = [
        Row(key: "CopyToClipboard", title: "Copy to clipboard after capture", section: "General", kind: .toggle),
        Row(key: "SaveToFile", title: "Save to file after capture", section: "General", kind: .toggle),
        Row(key: "CheckAutomatically", title: "Check for updates automatically (daily, from GitHub)", section: "Updates", keywords: "update upgrade version", kind: .toggle),
        Row(key: "AutoTag", title: "Tag captures automatically (otherwise tags are suggested)", section: "Gallery", keywords: "auto tags suggestions categorize", kind: .toggle),
        Row(key: "SaveFolder", title: "Captures folder", section: "General", keywords: "directory path pictures", kind: .folder),
        Row(key: "AfterCapture", title: "After capture", section: "General", keywords: "action pin open edit upload",
            kind: .choice([("none", "Just copy / save"), ("edit", "Open the editor"), ("pin", "Pin to screen"), ("open", "Open the file"), ("upload", "Upload and copy link")])),
        Row(key: "FileNameTemplate", title: "File name template", section: "General", keywords: "{yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}",
            kind: .text(Output.defaultTemplate, secure: false)),
        Row(key: "AskForName", title: "Ask for a name after each capture", section: "General", keywords: "rename prompt", kind: .toggle),
        Row(key: "ShowToast", title: "Show a notification after capture", section: "General", keywords: "toast popup", kind: .toggle),
        Row(key: "ToastMs", title: "Notification duration", section: "General", kind: .number(500...15000, 500, "ms")),
        Row(key: "LaunchAtLogin", title: "Launch at login", section: "General", keywords: "startup boot autostart", kind: .login),
        Row(key: "CaptureCursor", title: "Include the mouse cursor", section: "Capture", keywords: "pointer", kind: .toggle),
        Row(key: "Crosshair", title: "Crosshair in the region selector", section: "Capture", kind: .toggle),
        Row(key: "Magnifier", title: "Magnifier in the region selector", section: "Capture", keywords: "zoom loupe pixel", kind: .toggle),
        Row(key: "DelaySeconds", title: "Delayed capture", section: "Capture", keywords: "timer", kind: .number(1...30, 1, "s")),
        Row(key: "AutoRedact", title: "Auto-redact every capture", section: "Capture", keywords: "privacy ocr pixelate email token", kind: .toggle),
        Row(key: "ScrollDelayMs", title: "Scrolling capture: wait between scrolls", section: "Capture", keywords: "long page", kind: .number(100...3000, 50, "ms")),
        Row(key: "ScrollMaxFrames", title: "Scrolling capture: maximum frames", section: "Capture", keywords: "long page", kind: .number(2...500, 5, "")),
        Row(key: "VideoFps", title: "MP4 frame rate", section: "Recording", keywords: "fps video", kind: .number(5...120, 5, "fps")),
        Row(key: "GifFps", title: "GIF frame rate", section: "Recording", keywords: "fps animation", kind: .number(2...50, 1, "fps")),
        Row(key: "RecordCursor", title: "Show the cursor in recordings", section: "Recording", kind: .toggle),
        Row(key: "RecordSystemAudio", title: "Record system audio", section: "Recording", keywords: "sound", kind: .toggle),
        Row(key: "RecordMicrophone", title: "Record microphone", section: "Recording", keywords: "mic voice", kind: .toggle),
        Row(key: "CountdownSeconds", title: "Countdown before recording", section: "Recording", kind: .number(0...10, 1, "s")),
        Row(key: "ShowClicks", title: "Show clicks in recordings", section: "Recording", keywords: "mouse ripple", kind: .toggle),
        Row(key: "ShowKeys", title: "Show keystrokes in recordings (shows everything you type)", section: "Recording", keywords: "keyboard", kind: .toggle),
        Row(key: "ShowGamepad", title: "Show game controller (while one is connected)", section: "Recording", keywords: "gamepad xbox playstation joystick input overlay", kind: .toggle),
        Row(key: "GamepadCorner", title: "Controller corner", section: "Recording", keywords: "gamepad position",
            kind: .choice(PadCorner.allCases.map { ($0.rawValue, $0.title) }), shownWhen: "ShowGamepad"),
        Row(key: "GamepadOpacity", title: "Controller opacity", section: "Recording", keywords: "gamepad transparent", kind: .number(10...100, 10, "%"), shownWhen: "ShowGamepad"),
        Row(key: "StyledExport", title: "Styled export by default", section: "Editor", keywords: "gradient shadow rounded background", kind: .toggle),
        Row(key: "Uploader", title: "Uploader", section: "Upload", keywords: "share link",
            kind: .choice([("none", "None"), ("imgur", "Imgur"), ("custom", "Custom (multipart POST)"), ("s3", "S3 / R2 / MinIO")])),
        Row(key: "ImgurClientId", title: "Imgur client ID", section: "Upload", kind: .text("", secure: false)),
        Row(key: "CustomUrl", title: "Custom upload URL", section: "Upload", kind: .text("https://example.com/upload", secure: false)),
        Row(key: "CustomFileField", title: "Custom file field", section: "Upload", kind: .text("file", secure: false)),
        Row(key: "CustomHeaders", title: "Custom headers", section: "Upload", kind: .text("Name: value; Name2: value2", secure: false)),
        Row(key: "CustomResponseUrl", title: "Custom response URL path", section: "Upload", kind: .text("data.link (empty = plain-text body)", secure: false)),
        Row(key: "S3Endpoint", title: "S3 endpoint", section: "Upload", kind: .text("https://<account>.r2.cloudflarestorage.com", secure: false)),
        Row(key: "S3Bucket", title: "S3 bucket", section: "Upload", kind: .text("", secure: false)),
        Row(key: "S3Region", title: "S3 region", section: "Upload", kind: .text("auto", secure: false)),
        Row(key: "S3AccessKey", title: "S3 access key", section: "Upload", kind: .text("", secure: false)),
        Row(key: "S3SecretKey", title: "S3 secret key", section: "Upload", kind: .text("", secure: true)),
        Row(key: "S3PublicUrl", title: "S3 public URL", section: "Upload", kind: .text("https://cdn.example.com", secure: false)),
    ]
    for c in kCmds { r.append(Row(key: "Hotkey." + c.cmd.rawValue, title: c.title, section: "Shortcuts", keywords: "hotkey keyboard " + c.keywords, kind: .hotkey(c.cmd))) }
    return r
}()

private let sections = ["General", "Capture", "Recording", "Editor", "Gallery", "Upload", "Updates", "Shortcuts"]

struct SettingsView: View {
    @ObservedObject var s = Settings.shared
    @ObservedObject var status = HotkeyStatus.shared
    @State private var query = ""
    @State private var login = SMAppService.mainApp.status == .enabled

    private var visible: [Row] {
        let w = query.lowercased().split(separator: " ").map(String.init)
        guard !w.isEmpty else { return rows.filter { $0.shownWhen.map(s.bool) ?? true } }
        return rows.filter { r in
            let hay = "\(r.title) \(r.section) \(r.keywords) \(r.key)".lowercased()
            return w.allSatisfy { hay.contains($0) }
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: "magnifyingglass").foregroundColor(Color(nsColor: Theme.muted))
                TextField("Search settings", text: $query).textFieldStyle(.plain).font(.system(size: 14))
            }
            .padding(.horizontal, 16).padding(.vertical, 12)
            .background(Color(nsColor: Theme.surface))
            Rectangle().fill(Color(nsColor: Theme.border)).frame(height: 1)
            Form {
                ForEach(sections, id: \.self) { sec in
                    let items = visible.filter { $0.section == sec }
                    if !items.isEmpty {
                        Section(sec) {
                            ForEach(items) { row in
                                control(row).contextMenu {
                                    Button("Reset to default") {
                                        if case .login = row.kind { return }
                                        s.reset(row.key)
                                    }
                                }
                            }
                        }
                    }
                }
                if visible.isEmpty { Text("No setting matches “\(query)”.").foregroundColor(.secondary) }
                Section {
                    Text("Changes apply right away. Right-click a setting to reset it. Click a shortcut and press the keys: ⎋ cancels, ⌫ clears.")
                        .font(.system(size: 11)).foregroundColor(.secondary)
                }
            }
            .formStyle(.grouped)
            .scrollContentBackground(.hidden)
        }
        .background(Color(nsColor: Theme.bg))
        .frame(minWidth: 560, minHeight: 420)
    }

    @ViewBuilder
    private func control(_ row: Row) -> some View {
        switch row.kind {
        case .toggle:
            Toggle(row.title, isOn: s.binding(bool: row.key)).tint(Color(nsColor: Theme.accent))
        case .number(let range, let step, let unit):
            Stepper(value: s.binding(int: row.key), in: range, step: step) {
                HStack {
                    Text(row.title)
                    Spacer()
                    Text("\(s.int(row.key))\(unit.isEmpty ? "" : " " + unit)").monospacedDigit().foregroundColor(.secondary)
                }
            }
        case .text(let placeholder, let secure):
            if secure {
                SecureField(row.title, text: s.binding(string: row.key), prompt: Text(placeholder))
            } else {
                TextField(row.title, text: s.binding(string: row.key), prompt: Text(placeholder))
            }
        case .choice(let opts):
            Picker(row.title, selection: s.binding(string: row.key)) {
                ForEach(opts, id: \.0) { Text($0.1).tag($0.0) }
            }
        case .folder:
            HStack {
                Text(row.title)
                Spacer()
                Text(s.capturesFolder.path.replacingOccurrences(of: NSHomeDirectory(), with: "~")).foregroundColor(.secondary).lineLimit(1).truncationMode(.middle)
                Button("Choose…") {
                    let p = NSOpenPanel()
                    p.canChooseDirectories = true
                    p.canChooseFiles = false
                    p.canCreateDirectories = true
                    p.directoryURL = s.capturesFolder
                    if p.runModal() == .OK, let u = p.url { s.set(row.key, u.path) }
                }
                Button("Open") { Output.open(s.capturesFolder) }
            }
        case .login:
            Toggle(row.title, isOn: Binding(get: { login }, set: { on in
                AppDelegate.shared?.setLaunchAtLogin(on)
                login = SMAppService.mainApp.status == .enabled
            })).tint(Color(nsColor: Theme.accent))
        case .hotkey(let cmd):
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(row.title)
                    if let warn = status.problems[cmd.rawValue] {
                        Text(warn).font(.system(size: 11)).foregroundColor(.orange)
                    }
                }
                Spacer()
                HotkeyField(cmd: cmd, text: s.hotkey(cmd)).frame(width: 150, height: 24)
            }
        }
    }
}

final class HotkeyStatus: ObservableObject {
    static let shared = HotkeyStatus()
    @Published var problems: [String: String] = [:]
}

// Click, then press the shortcut. ⎋ cancels, ⌫ clears. A shortcut used by another command moves here.
private struct HotkeyField: NSViewRepresentable {
    let cmd: Cmd
    let text: String

    func makeNSView(context: Context) -> RecorderView {
        let v = RecorderView()
        v.cmd = cmd
        return v
    }
    func updateNSView(_ v: RecorderView, context: Context) {
        v.value = text
        v.needsDisplay = true
    }

    final class RecorderView: NSView {
        var cmd: Cmd = .palette
        var value = ""
        private var recording = false { didSet { needsDisplay = true } }

        override var acceptsFirstResponder: Bool { true }
        override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self) }
        override func becomeFirstResponder() -> Bool {
            recording = true
            AppDelegate.shared?.suspendHotkeys(true)
            return true
        }
        override func resignFirstResponder() -> Bool {
            recording = false
            AppDelegate.shared?.suspendHotkeys(false)
            return true
        }

        override func keyDown(with e: NSEvent) {
            switch Int(e.keyCode) {
            case kVK_Escape: window?.makeFirstResponder(nil)
            case kVK_Delete, kVK_ForwardDelete:
                Settings.shared.set("Hotkey." + cmd.rawValue, "")
                window?.makeFirstResponder(nil)
            default:
                let hk = Hotkey.from(event: e)
                // Needs ⌃, ⌥ or ⌘ (⇧ alone would swallow capital letters system-wide), unless it's a function key.
                guard !hk.text.isEmpty, !hk.mods.subtracting(.shift).isEmpty || hk.isFunctionKey else { NSSound.beep(); return }
                for c in kCmds where c.cmd != cmd && Hotkey.parse(Settings.shared.hotkey(c.cmd)) == hk {
                    Settings.shared.set("Hotkey." + c.cmd.rawValue, "")
                }
                Settings.shared.set("Hotkey." + cmd.rawValue, hk.text)
                window?.makeFirstResponder(nil)
            }
        }
        override func performKeyEquivalent(with e: NSEvent) -> Bool {
            guard recording else { return false }
            keyDown(with: e)
            return true
        }

        override func draw(_ dirtyRect: NSRect) {
            let r = bounds.insetBy(dx: 1, dy: 1)
            let p = NSBezierPath(roundedRect: r, xRadius: 6, yRadius: 6)
            (recording ? Theme.selected : Theme.raised).setFill()
            p.fill()
            (recording ? Theme.accent : Theme.border).setStroke()
            p.stroke()
            let s = recording ? "Press shortcut…" : (value.isEmpty ? "None" : Hotkey.display(value))
            let attr = NSAttributedString(string: s, attributes: [
                .font: Theme.font(12, .medium), .foregroundColor: recording ? Theme.accent : (value.isEmpty ? Theme.muted : Theme.text),
            ])
            let sz = attr.size()
            attr.draw(at: NSPoint(x: r.midX - sz.width / 2, y: r.midY - sz.height / 2))
        }
    }
}

final class SettingsWindow: NSObject, NSWindowDelegate {
    static var shared: SettingsWindow?
    let window: NSWindow

    static func show() {
        if let s = shared {
            activateApp()
            s.window.makeKeyAndOrderFront(nil)
            return
        }
        shared = SettingsWindow()
    }

    private override init() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 680, height: 720), styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = "Ather Screenshot Settings"
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.bg
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentView = NSHostingView(rootView: SettingsView())
        window.center()
        AppDelegate.shared?.windowOpened(window)
        activateApp()
        window.makeKeyAndOrderFront(nil)
    }

    func windowWillClose(_ notification: Notification) {
        AppDelegate.shared?.suspendHotkeys(false)
        SettingsWindow.shared = nil
    }
}
