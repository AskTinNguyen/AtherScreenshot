import AppKit
import AVFoundation
import SwiftUI
import CoreAudio

// Notices a call: another app (Zoom, Teams, Meet in a browser…) starts using the microphone. Then it offers, once per
// call, to record the meeting; and when the call ends while one is being recorded, it offers to stop.
// Reads only which processes use audio input (Core Audio's process list); never any sound.
final class MeetingDetector {
    static let shared = MeetingDetector()
    private var timer: Timer?
    private var inCall: String?        // the app's name while a call is on
    private var quietSince: Date?
    private var offeredStop = false

    func apply() {
        if Settings.shared.bool("MeetingDetect") && !Settings.underTests { start() } else { stop() }
    }

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.poll() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        inCall = nil
    }

    private func poll() {
        let app = MeetingDetector.callApp(MeetingDetector.processesUsingInput())
        if let app, inCall == nil {
            inCall = app
            quietSince = nil
            offeredStop = false
            guard !MeetingRecorder.isActive else { return }
            let hk = Hotkey.display(Settings.shared.hotkey(.recordMeeting))
            Toast.shared.show("Record this meeting?", "\(app) is using the microphone. Click to record the call's sound\(hk.isEmpty ? "" : ", or press \(hk)").", ms: 12000) {
                if !MeetingRecorder.isActive { MeetingRecorder.start() }
            }
        } else if app == nil, let was = inCall {
            if quietSince == nil { quietSince = Date() }
            // A short drop (muting, switching microphones) isn't the end of the call.
            if Date().timeIntervalSince(quietSince!) > 20 {
                inCall = nil
                if let r = MeetingRecorder.current, r.hasAudio, !offeredStop {
                    offeredStop = true
                    Toast.shared.show("Did the call end?", "\(was) stopped using the microphone. Click to stop and save the meeting recording.", ms: 15000) {
                        MeetingRecorder.current?.stop()
                    }
                }
            }
        } else if app != nil {
            quietSince = nil
        }
    }

    // The name of a meeting app among these bundle ids, if any (browser helpers count as their browser).
    static func callApp(_ ids: [String]) -> String? {
        for id in ids {
            if id.hasPrefix("com.apple.WebKit") { return "Safari" }
            if let k = MeetingApps.known.first(where: { id == $0.id || id.hasPrefix($0.id + ".") }) { return k.name }
        }
        return nil
    }

    // Bundle ids of the other processes reading audio input right now.
    static func processesUsingInput() -> [String] {
        var addr = AudioObjectPropertyAddress(mSelector: AudioObjectPropertySelector(0x70727323) /* 'prs#' */,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr, size > 0 else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        let me = ProcessInfo.processInfo.processIdentifier
        var out: [String] = []
        for p in ids {
            var running: UInt32 = 0
            var sz = UInt32(MemoryLayout<UInt32>.size)
            var a = AudioObjectPropertyAddress(mSelector: AudioObjectPropertySelector(0x70697269) /* 'piri' */, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            guard AudioObjectGetPropertyData(p, &a, 0, nil, &sz, &running) == noErr, running != 0 else { continue }
            var pid: pid_t = 0
            sz = UInt32(MemoryLayout<pid_t>.size)
            a.mSelector = AudioObjectPropertySelector(0x70706964) /* 'ppid' */
            if AudioObjectGetPropertyData(p, &a, 0, nil, &sz, &pid) == noErr, pid == me { continue }
            var cf: Unmanaged<CFString>?
            sz = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
            a.mSelector = AudioObjectPropertySelector(0x70626964) /* 'pbid' */
            if AudioObjectGetPropertyData(p, &a, 0, nil, &sz, &cf) == noErr, let s = cf?.takeRetainedValue() as String? { out.append(s) }
        }
        return out
    }
}

// MARK: - Settings › Meetings rows that need live choices

enum MeetingSettings {
    static let languages: [(String, String)] = [
        ("", "Same as the system"), ("en-US", "English (US)"), ("en-GB", "English (UK)"), ("en-AU", "English (Australia)"), ("en-IN", "English (India)"),
        ("vi-VN", "Tiếng Việt"), ("es-ES", "Español (España)"), ("es-MX", "Español (México)"), ("fr-FR", "Français"), ("de-DE", "Deutsch"),
        ("it-IT", "Italiano"), ("pt-BR", "Português (Brasil)"), ("ja-JP", "日本語"), ("ko-KR", "한국어"), ("zh-CN", "中文（普通话）"),
        ("zh-TW", "中文（台灣）"), ("yue-CN", "粵語"), ("nl-NL", "Nederlands"), ("sv-SE", "Svenska"), ("ru-RU", "Русский"), ("hi-IN", "हिन्दी"),
    ]

    static func microphones() -> [(String, String)] {
        let found = AVCaptureDevice.DiscoverySession(deviceTypes: [.microphone, .external], mediaType: .audio, position: .unspecified).devices
        var out: [(String, String)] = [("", "System default" + (AVCaptureDevice.default(for: .audio).map { " (\($0.localizedName))" } ?? ""))]
        out += found.map { ($0.uniqueID, $0.localizedName) }
        let cur = Settings.shared.string("MeetingMicDevice")
        if !cur.isEmpty, !out.contains(where: { $0.0 == cur }) { out.append((cur, "A microphone that isn't connected")) }
        return out
    }
}

struct MeetingSettingsRow: View {
    let id: String
    let title: String
    @ObservedObject private var s = Settings.shared
    @State private var options: [(String, String)] = []

    var body: some View {
        Picker(title, selection: s.binding(string: id == "meetingApp" ? "MeetingApp" : "MeetingMicDevice")) {
            ForEach(options, id: \.0) { Text($0.1).tag($0.0) }
        }
        .onAppear { options = id == "meetingApp" ? MeetingApps.choices() : MeetingSettings.microphones() }
    }
}
