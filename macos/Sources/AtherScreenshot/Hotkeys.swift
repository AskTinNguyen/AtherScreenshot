import AppKit
import Carbon.HIToolbox

// Shortcut text uses the same "Ctrl+Alt+Shift+Cmd+Key" form as the Windows settings.ini,
// and is displayed with the macOS glyphs (⌃⌥⇧⌘).
struct Hotkey: Equatable {
    var keyCode: UInt32
    var mods: NSEvent.ModifierFlags

    static let keyNames: [(String, Int)] = [
        ("A", kVK_ANSI_A), ("B", kVK_ANSI_B), ("C", kVK_ANSI_C), ("D", kVK_ANSI_D), ("E", kVK_ANSI_E),
        ("F", kVK_ANSI_F), ("G", kVK_ANSI_G), ("H", kVK_ANSI_H), ("I", kVK_ANSI_I), ("J", kVK_ANSI_J),
        ("K", kVK_ANSI_K), ("L", kVK_ANSI_L), ("M", kVK_ANSI_M), ("N", kVK_ANSI_N), ("O", kVK_ANSI_O),
        ("P", kVK_ANSI_P), ("Q", kVK_ANSI_Q), ("R", kVK_ANSI_R), ("S", kVK_ANSI_S), ("T", kVK_ANSI_T),
        ("U", kVK_ANSI_U), ("V", kVK_ANSI_V), ("W", kVK_ANSI_W), ("X", kVK_ANSI_X), ("Y", kVK_ANSI_Y),
        ("Z", kVK_ANSI_Z),
        ("0", kVK_ANSI_0), ("1", kVK_ANSI_1), ("2", kVK_ANSI_2), ("3", kVK_ANSI_3), ("4", kVK_ANSI_4),
        ("5", kVK_ANSI_5), ("6", kVK_ANSI_6), ("7", kVK_ANSI_7), ("8", kVK_ANSI_8), ("9", kVK_ANSI_9),
        ("F1", kVK_F1), ("F2", kVK_F2), ("F3", kVK_F3), ("F4", kVK_F4), ("F5", kVK_F5), ("F6", kVK_F6),
        ("F7", kVK_F7), ("F8", kVK_F8), ("F9", kVK_F9), ("F10", kVK_F10), ("F11", kVK_F11), ("F12", kVK_F12),
        ("F13", kVK_F13), ("F14", kVK_F14), ("F15", kVK_F15), ("F16", kVK_F16), ("F17", kVK_F17),
        ("F18", kVK_F18), ("F19", kVK_F19), ("F20", kVK_F20),
        ("Space", kVK_Space), ("Return", kVK_Return), ("Tab", kVK_Tab), ("Escape", kVK_Escape),
        ("Delete", kVK_Delete), ("ForwardDelete", kVK_ForwardDelete), ("Home", kVK_Home), ("End", kVK_End),
        ("PageUp", kVK_PageUp), ("PageDown", kVK_PageDown),
        ("Left", kVK_LeftArrow), ("Right", kVK_RightArrow), ("Up", kVK_UpArrow), ("Down", kVK_DownArrow),
        ("-", kVK_ANSI_Minus), ("=", kVK_ANSI_Equal), ("[", kVK_ANSI_LeftBracket), ("]", kVK_ANSI_RightBracket),
        (";", kVK_ANSI_Semicolon), ("'", kVK_ANSI_Quote), (",", kVK_ANSI_Comma), (".", kVK_ANSI_Period),
        ("/", kVK_ANSI_Slash), ("\\", kVK_ANSI_Backslash), ("`", kVK_ANSI_Grave),
    ]
    static let glyphs: [String: String] = [
        "Space": "Space", "Return": "↩", "Tab": "⇥", "Escape": "⎋", "Delete": "⌫", "ForwardDelete": "⌦",
        "Home": "↖", "End": "↘", "PageUp": "⇞", "PageDown": "⇟", "Left": "←", "Right": "→", "Up": "↑", "Down": "↓",
    ]

    static func parse(_ text: String) -> Hotkey? {
        var mods: NSEvent.ModifierFlags = []
        var key: Int?
        for raw in text.split(separator: "+", omittingEmptySubsequences: false).map({ String($0).trimmingCharacters(in: .whitespaces) }) {
            let part = raw.isEmpty ? "+" : raw
            switch part.lowercased() {
            case "ctrl", "control": mods.insert(.control)
            case "alt", "opt", "option": mods.insert(.option)
            case "shift": mods.insert(.shift)
            case "cmd", "command", "win": mods.insert(.command)
            default:
                guard key == nil, let k = keyNames.first(where: { $0.0.lowercased() == part.lowercased() }) else { return nil }
                key = k.1
            }
        }
        guard let k = key else { return nil }
        return Hotkey(keyCode: UInt32(k), mods: mods)
    }

    var keyName: String? { Hotkey.keyNames.first { $0.1 == Int(keyCode) }?.0 }

    var text: String {
        guard let name = keyName else { return "" }
        var parts: [String] = []
        if mods.contains(.control) { parts.append("Ctrl") }
        if mods.contains(.option) { parts.append("Alt") }
        if mods.contains(.shift) { parts.append("Shift") }
        if mods.contains(.command) { parts.append("Cmd") }
        return (parts + [name]).joined(separator: "+")
    }

    var display: String {
        guard let name = keyName else { return "" }
        var s = ""
        if mods.contains(.control) { s += "⌃" }
        if mods.contains(.option) { s += "⌥" }
        if mods.contains(.shift) { s += "⇧" }
        if mods.contains(.command) { s += "⌘" }
        return s + (Hotkey.glyphs[name] ?? name)
    }

    static func display(_ text: String) -> String { parse(text)?.display ?? text }

    var carbonMods: UInt32 {
        var m: UInt32 = 0
        if mods.contains(.control) { m |= UInt32(controlKey) }
        if mods.contains(.option) { m |= UInt32(optionKey) }
        if mods.contains(.shift) { m |= UInt32(shiftKey) }
        if mods.contains(.command) { m |= UInt32(cmdKey) }
        return m
    }

    // For NSMenuItem key equivalents.
    var menuKey: String {
        guard let name = keyName else { return "" }
        if name.count == 1 { return name.lowercased() }
        switch name {
        case "Space": return " "
        case "Return": return "\r"
        case "Tab": return "\t"
        case "Escape": return "\u{1b}"
        case "Delete": return "\u{8}"
        default:
            if name.hasPrefix("F"), let n = Int(name.dropFirst()) { return String(UnicodeScalar(NSF1FunctionKey + n - 1)!) }
            return ""
        }
    }

    static func from(event: NSEvent) -> Hotkey {
        Hotkey(keyCode: UInt32(event.keyCode), mods: event.modifierFlags.intersection([.control, .option, .shift, .command]))
    }

    var isFunctionKey: Bool { keyName.map { $0.count > 1 && $0.hasPrefix("F") } ?? false }
}

// Global hotkeys through Carbon's RegisterEventHotKey: no Accessibility permission needed.
final class HotkeyCenter {
    static let shared = HotkeyCenter()
    private var refs: [UInt32: EventHotKeyRef] = [:]
    private var actions: [UInt32: () -> Void] = [:]
    private var nextID: UInt32 = 1
    private var handler: EventHandlerRef?

    private init() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, _ -> OSStatus in
            var id = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil,
                              MemoryLayout<EventHotKeyID>.size, nil, &id)
            let action = HotkeyCenter.shared.actions[id.id]
            DispatchQueue.main.async { action?() }
            return noErr
        }, 1, &spec, nil, &handler)
    }

    func unregisterAll() {
        for (_, ref) in refs { UnregisterEventHotKey(ref) }
        refs.removeAll()
        actions.removeAll()
    }

    // Returns false if the shortcut is taken (by macOS, another app, or one of ours).
    func register(_ hk: Hotkey, action: @escaping () -> Void) -> Bool {
        let id = nextID
        nextID += 1
        var ref: EventHotKeyRef?
        let status = RegisterEventHotKey(hk.keyCode, hk.carbonMods, EventHotKeyID(signature: OSType(0x4154_4852), id: id),
                                         GetApplicationEventTarget(), 0, &ref)
        guard status == noErr, let ref else { return false }
        refs[id] = ref
        actions[id] = action
        return true
    }
}
