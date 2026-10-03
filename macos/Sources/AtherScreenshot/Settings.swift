import AppKit
import Combine
import SwiftUI

// Backed by UserDefaults (com.ather.screenshot), so `defaults write com.ather.screenshot Key value` works too.
// Keys match the Windows settings.ini names.
final class Settings: ObservableObject {
    static let shared = Settings()
    private let d = UserDefaults.standard
    var onChange: ((String) -> Void)?

    static let defaults: [String: Any] = {
        var m: [String: Any] = [
            "CopyToClipboard": true, "SaveToFile": true, "CaptureCursor": false, "ShowToast": true,
            "Crosshair": true, "Magnifier": true, "ToastMs": 2500, "DelaySeconds": 3, "SaveFolder": "",
            "AfterCapture": "none", "FileNameTemplate": "", "AskForName": false, "AutoRedact": false,
            "VideoFps": 30, "GifFps": 15, "RecordCursor": true, "RecordSystemAudio": true, "RecordMicrophone": false,
            "CountdownSeconds": 3, "ShowClicks": true, "ShowKeys": false,
            "ScrollDelayMs": 400, "ScrollMaxFrames": 60, "StyledExport": false,
            "Uploader": "none", "ImgurClientId": "", "CustomUrl": "", "CustomFileField": "file", "CustomHeaders": "",
            "CustomResponseUrl": "", "S3Endpoint": "", "S3Bucket": "", "S3Region": "auto", "S3AccessKey": "",
            "S3SecretKey": "", "S3PublicUrl": "",
        ]
        for c in kCmds { m["Hotkey." + c.cmd.rawValue] = c.hotkey }
        return m
    }()

    private init() { d.register(defaults: Settings.defaults) }

    func bool(_ k: String) -> Bool { d.bool(forKey: k) }
    func int(_ k: String) -> Int { d.integer(forKey: k) }
    func string(_ k: String) -> String { d.string(forKey: k) ?? "" }

    func set(_ k: String, _ v: Any?) {
        objectWillChange.send()
        d.set(v, forKey: k)
        onChange?(k)
    }
    func reset(_ k: String) {
        objectWillChange.send()
        d.removeObject(forKey: k)
        onChange?(k)
    }
    func isDefault(_ k: String) -> Bool {
        (d.object(forKey: k) as? NSObject) == (Settings.defaults[k] as? NSObject)
    }

    func binding(bool k: String) -> Binding<Bool> { Binding(get: { self.bool(k) }, set: { self.set(k, $0) }) }
    func binding(int k: String) -> Binding<Int> { Binding(get: { self.int(k) }, set: { self.set(k, $0) }) }
    func binding(string k: String) -> Binding<String> { Binding(get: { self.string(k) }, set: { self.set(k, $0) }) }

    func toggle(_ k: String) -> Bool {
        let v = !bool(k)
        set(k, v)
        return v
    }

    func hotkey(_ c: Cmd) -> String { string("Hotkey." + c.rawValue) }

    var capturesFolder: URL {
        let custom = string("SaveFolder")
        if !custom.isEmpty { return URL(fileURLWithPath: (custom as NSString).expandingTildeInPath, isDirectory: true) }
        let pictures = FileManager.default.urls(for: .picturesDirectory, in: .userDomainMask).first!
        return pictures.appendingPathComponent(kAppName, isDirectory: true)
    }

    static var supportFolder: URL {
        let u = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent(kAppName, isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    var uploadConfig: UploadConfig {
        UploadConfig(uploader: string("Uploader"), imgurClientId: string("ImgurClientId"), customUrl: string("CustomUrl"),
                     customFileField: string("CustomFileField"), customHeaders: string("CustomHeaders"),
                     customResponseUrl: string("CustomResponseUrl"), s3Endpoint: string("S3Endpoint"),
                     s3Bucket: string("S3Bucket"), s3Region: string("S3Region"), s3AccessKey: string("S3AccessKey"),
                     s3SecretKey: string("S3SecretKey"), s3PublicUrl: string("S3PublicUrl"))
    }
}
