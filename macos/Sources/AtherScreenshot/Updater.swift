import AppKit
import CryptoKit

// Updates in place, without Sparkle. The newest GitHub Release carries latest.json (shared with Windows); its
// "macos" entry names the newest build, a zip of the .app, its SHA-256 and size. The app checks it, downloads and
// verifies the zip (size, hash, then the bundle id, build and signature inside), and a small helper swaps the
// bundle once the app has quit, then relaunches it. Windows: src/updater.cpp.
//
// Set ATHER_UPDATE_URL to try another manifest (http is allowed for localhost only, for testing).

struct UpdateInfo: Equatable {
    var version = ""   // the build, e.g. 0.0.2.1
    var url = ""
    var sha256 = ""
    var notes = ""
    var size: Int64 = 0
}

enum Updater {
    enum Failure: LocalizedError {
        case message(String)
        var errorDescription: String? { if case .message(let s) = self { return s }; return nil }
    }

    static let manifestURL = "https://github.com/AskTinNguyen/AtherScreenshot/releases/latest/download/latest.json"
    static let downloadPage = URL(string: "https://github.com/AskTinNguyen/AtherScreenshot#download")!
    private static let repoPath = "/AskTinNguyen/AtherScreenshot/"
    static let maxZip: Int64 = 128 << 20
    private static let maxManifest = 64 << 10

    // The running build (CFBundleVersion), which is what the manifest is compared with, never the shown version.
    static var currentBuild: String = {
        guard Bundle.main.bundleIdentifier == kBundleID else { return "0" }  // tests set this themselves
        return Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "0"
    }()

    // -1, 0 or 1, comparing dotted numbers ("0.0.10" > "0.0.9", "0.0.2" == "0.0.2.0").
    static func compareVersions(_ a: String, _ b: String) -> Int {
        let x = a.split(separator: ".").map { Int($0) ?? 0 }, y = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p < q ? -1 : 1 }
        }
        return 0
    }

    private static func isLocalhost(_ host: String?) -> Bool { host == "localhost" || host == "127.0.0.1" }

    // Updates come only from this repository on GitHub, over HTTPS. With a test manifest, this Mac is allowed too.
    static func allowedURL(_ s: String, allowLocalhost: Bool) -> Bool {
        guard let u = URL(string: s), let host = u.host?.lowercased() else { return false }
        if allowLocalhost && isLocalhost(host) { return u.scheme == "http" || u.scheme == "https" }
        guard u.scheme == "https" else { return false }
        return (host == "github.com" || host == "raw.githubusercontent.com") && u.path.hasPrefix(repoPath)
    }

    // Where a download may be redirected: GitHub's own hosts, HTTPS only.
    static func allowedRedirect(_ u: URL?, allowLocalhost: Bool) -> Bool {
        guard let u, let host = u.host?.lowercased() else { return false }
        if allowLocalhost && isLocalhost(host) { return true }
        return u.scheme == "https" && (host == "github.com" || host.hasSuffix(".githubusercontent.com"))
    }

    // The macOS entry of a manifest, if it's well-formed and points at this repository over HTTPS.
    static func parseManifest(_ data: Data, allowLocalhost: Bool = false) -> UpdateInfo? {
        var d = data
        if d.starts(with: [0xEF, 0xBB, 0xBF]) { d = d.dropFirst(3) }  // a BOM from a Windows editor
        guard d.count <= maxManifest, let root = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let m = root["macos"] as? [String: Any] else { return nil }
        var u = UpdateInfo()
        u.version = m["version"] as? String ?? ""
        u.url = m["url"] as? String ?? ""
        u.sha256 = (m["sha256"] as? String ?? "").lowercased()
        u.notes = String((m["notes"] as? String ?? "").prefix(300))
        u.size = (m["size"] as? NSNumber)?.int64Value ?? 0
        let versionOk = !u.version.isEmpty && u.version.count <= 20 && u.version.first!.isNumber && u.version.allSatisfy { $0.isASCII && ($0.isNumber || $0 == ".") }
        let hashOk = u.sha256.count == 64 && u.sha256.allSatisfy(\.isHexDigit)
        guard versionOk, hashOk, u.size > 0, u.size <= maxZip, allowedURL(u.url, allowLocalhost: allowLocalhost) else { return nil }
        return u
    }

    private static var testManifest: String? {
        let s = ProcessInfo.processInfo.environment["ATHER_UPDATE_URL"] ?? ""
        return s.isEmpty ? nil : s
    }

    // MARK: network

    private final class Guard: NSObject, URLSessionTaskDelegate {
        let local: Bool
        init(local: Bool) { self.local = local }
        func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                        newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
            completionHandler(Updater.allowedRedirect(request.url, allowLocalhost: local) ? request : nil)
        }
    }

    private static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.requestCachePolicy = .reloadIgnoringLocalCacheData
        c.timeoutIntervalForRequest = 30
        return URLSession(configuration: c)
    }()

    // GET, handing the body to `sink` in chunks; anything but 200 is an error.
    private static func get(_ url: URL, max: Int64, local: Bool, progress: ((Int64, Int64) -> Void)? = nil, sink: (Data) throws -> Void) async throws {
        let (bytes, response): (URLSession.AsyncBytes, URLResponse)
        do { (bytes, response) = try await session.bytes(from: url, delegate: Guard(local: local)) }
        catch { throw Failure.message("Can't reach GitHub (\(error.localizedDescription)).") }
        guard let http = response as? HTTPURLResponse else { throw Failure.message("The update server didn't answer.") }
        guard http.statusCode == 200 else { throw Failure.message("The update server answered \(http.statusCode).") }
        let total = http.expectedContentLength
        var got: Int64 = 0
        var buf = Data()
        buf.reserveCapacity(1 << 16)
        do {
            for try await b in bytes {
                buf.append(b)
                if buf.count >= 1 << 16 {
                    got += Int64(buf.count)
                    guard got <= max else { throw Failure.message("The download is larger than expected.") }
                    try sink(buf)
                    buf.removeAll(keepingCapacity: true)
                    progress?(got, total)
                }
            }
        } catch let f as Failure { throw f } catch { throw Failure.message("The download stopped (\(error.localizedDescription)).") }
        got += Int64(buf.count)
        guard got <= max else { throw Failure.message("The download is larger than expected.") }
        if !buf.isEmpty { try sink(buf) }
        progress?(got, total)
    }

    // The newer build, or nil when this one is current.
    static func check() async throws -> UpdateInfo? {
        let custom = testManifest
        guard let url = URL(string: custom ?? manifestURL) else { throw Failure.message("The update address is wrong.") }
        var body = Data()
        try await get(url, max: Int64(maxManifest), local: custom != nil) { body.append($0) }
        guard let info = parseManifest(body, allowLocalhost: custom != nil) else {
            throw Failure.message("The update information on GitHub couldn't be read.")
        }
        return compareVersions(info.version, currentBuild) > 0 ? info : nil
    }

    // MARK: staging and verifying

    // Downloads land here (never next to the app). Only the running instance clears it, at launch.
    static var stagingFolder: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
            .appendingPathComponent(kBundleID, isDirectory: true).appendingPathComponent("Update", isDirectory: true)
    }

    // Downloads and verifies the update; returns the unpacked .app, ready to swap in.
    static func download(_ info: UpdateInfo, into dir: URL = stagingFolder, progress: @escaping (Double) -> Void) async throws -> URL {
        guard let url = URL(string: info.url), allowedURL(info.url, allowLocalhost: testManifest != nil) else { throw Failure.message("The update address isn't GitHub.") }
        try? FileManager.default.removeItem(at: dir)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let zip = dir.appendingPathComponent("update.zip")
        guard FileManager.default.createFile(atPath: zip.path, contents: nil), let h = try? FileHandle(forWritingTo: zip) else {
            throw Failure.message("Can't write the download.")
        }
        do {
            try await get(url, max: min(maxZip, info.size), local: testManifest != nil, progress: { got, _ in progress(min(1, Double(got) / Double(info.size))) }) {
                try h.write(contentsOf: $0)
            }
            try h.close()
            return try verify(zip: zip, info, unpackInto: dir.appendingPathComponent("app", isDirectory: true))
        } catch {
            try? h.close()
            try? FileManager.default.removeItem(at: dir)
            throw error
        }
    }

    static func sha256(_ url: URL) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        var hasher = SHA256()
        while let d = try? h.read(upToCount: 1 << 20), !d.isEmpty { hasher.update(data: d) }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // True when the zip is exactly the update the manifest describes, unpacked into `into`: the size and hash,
    // then the bundle inside (our bundle id, the promised build compared as numbers, a valid signature, and the
    // same team as this copy once builds are signed).
    static func verify(zip: URL, _ info: UpdateInfo, unpackInto into: URL) throws -> URL {
        do { return try check(zip: zip, info, unpackInto: into) } catch {
            try? FileManager.default.removeItem(at: into)  // nothing half-checked stays behind
            throw error
        }
    }

    private static func check(zip: URL, _ info: UpdateInfo, unpackInto into: URL) throws -> URL {
        let size = (try? zip.resourceValues(forKeys: [.fileSizeKey]).fileSize).map(Int64.init) ?? -1
        guard size == info.size else { throw Failure.message("The download is incomplete.") }
        guard sha256(zip) == info.sha256 else { throw Failure.message("The download doesn't match its checksum, so it wasn't installed.") }
        try? FileManager.default.removeItem(at: into)
        try FileManager.default.createDirectory(at: into, withIntermediateDirectories: true)
        guard run("/usr/bin/ditto", ["-x", "-k", zip.path, into.path]) == 0 else { throw Failure.message("The download couldn't be unpacked.") }
        let apps = (try? FileManager.default.contentsOfDirectory(at: into, includingPropertiesForKeys: nil))?.filter { $0.pathExtension == "app" } ?? []
        guard apps.count == 1, let app = apps.first else { throw Failure.message("The download doesn't hold the app.") }
        let plist = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")) as? [String: Any] ?? [:]
        let build = plist["CFBundleVersion"] as? String ?? ""
        guard plist["CFBundleIdentifier"] as? String == kBundleID, !build.isEmpty, compareVersions(build, info.version) == 0 else {
            throw Failure.message("The download isn't Ather Screenshot \(info.version), so it wasn't installed.")
        }
        guard run("/usr/bin/codesign", ["--verify", "--deep", "--strict", app.path]) == 0 else {
            throw Failure.message("The download's signature isn't valid, so it wasn't installed.")
        }
        if let mine = teamID(Bundle.main.bundleURL), teamID(app) != mine {
            throw Failure.message("The download isn't signed by the same developer, so it wasn't installed.")
        }
        return app
    }

    // The signing team, nil for ad-hoc (unsigned) builds.
    static func teamID(_ app: URL) -> String? {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(app as CFURL, [], &code) == errSecSuccess, let code else { return nil }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let d = info as? [String: Any] else { return nil }
        return d[kSecCodeInfoTeamIdentifier as String] as? String
    }

    @discardableResult
    static func run(_ tool: String, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    // MARK: swapping in

    // A copy running from a disk image or a translocated folder can't replace itself.
    static func canReplace(_ target: URL) -> String? {
        if target.path.contains("/AppTranslocation/") || target.path.hasPrefix("/Volumes/") {
            return "Move Ather Screenshot to your Applications folder first, then update."
        }
        if !FileManager.default.isWritableFile(atPath: target.deletingLastPathComponent().path) {
            return "Ather Screenshot can't write to \(target.deletingLastPathComponent().path)."
        }
        return nil
    }

    // The helper: waits for the app (pid) to quit, swaps the bundle (keeping the old one until the new one is in
    // place), relaunches it, and removes the old bundle. On any failure the old one goes back and is relaunched.
    static let helperScript = """
    #!/bin/sh
    pid="$1"; target="$2"; staged="$3"; relaunch="$4"
    old="$target.old"
    i=0
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 3000 ]; do sleep 0.1; i=$((i+1)); done
    rm -rf "$old"
    if mv "$target" "$old"; then
      if mv "$staged" "$target"; then
        rm -rf "$old"
        [ "$relaunch" = 1 ] && open "$target" --args --after-update
        exit 0
      fi
      mv "$old" "$target"
    fi
    [ "$relaunch" = 1 ] && open "$target" --args --update-failed
    exit 1
    """

    // Starts the helper for `pid`; the caller then quits. `relaunch: false` is for tests.
    static func startSwap(staged: URL, target: URL = Bundle.main.bundleURL, pid: Int32 = ProcessInfo.processInfo.processIdentifier,
                          relaunch: Bool = true) throws -> Process {
        if let why = canReplace(target) { throw Failure.message(why) }
        let script = staged.deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("swap.sh")
        try helperScript.write(to: script, atomically: true, encoding: .utf8)
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = [script.path, String(pid), target.path, staged.path, relaunch ? "1" : "0"]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { throw Failure.message("Couldn't start the update (\(error.localizedDescription)).") }
        return p
    }

    // Leftovers of an earlier update: the staging folder and a stray old bundle. Only the running instance calls
    // this, at launch (a second copy exits before getting here).
    static func cleanUp(target: URL = Bundle.main.bundleURL) {
        try? FileManager.default.removeItem(at: stagingFolder)
        try? FileManager.default.removeItem(at: URL(fileURLWithPath: target.path + ".old"))
    }
}

// MARK: - The update flow in the app

final class UpdateController {
    static let shared = UpdateController()
    private(set) var available: UpdateInfo?  // a newer build found by the last check
    private var announced = ""               // the build the automatic check already announced
    private var installing = false
    var quitting = false                     // the app is shutting down: a finished download must not restart it
    private var timer: Timer?

    // A minute after launch, then daily, unless [Updates] CheckAutomatically is off.
    func start() {
        timer = Timer.scheduledTimer(withTimeInterval: 60, repeats: false) { [weak self] _ in
            self?.automatic()
            self?.timer = Timer.scheduledTimer(withTimeInterval: 24 * 60 * 60, repeats: true) { [weak self] _ in self?.automatic() }
        }
    }

    private func automatic() { if Settings.shared.bool("CheckAutomatically") { check(manual: false) } }

    func check(manual: Bool) {
        Task { @MainActor in
            do {
                let info = try await Updater.check()
                self.available = info
                guard let info else {
                    if manual { Toast.shared.show("Ather Screenshot is up to date", "Version \(AppDelegate.version)") }
                    return
                }
                if !manual && self.announced == info.version { return }  // once per build, unless asked
                self.announced = info.version
                let size = ByteCountFormatter.string(fromByteCount: info.size, countStyle: .file)
                Toast.shared.show("Ather Screenshot \(info.version) is available",
                                  (info.notes.isEmpty ? "" : info.notes + "\n") + "Click to update (\(size)). Settings, captures and the gallery stay.",
                                  ms: 15000) { [weak self] in self?.install() }
            } catch {
                if manual { Toast.shared.show("Couldn't check for updates", error.localizedDescription, ms: 6000) }
            }
        }
    }

    // The update restarts the app, so it waits until nothing is in progress, and asks before closing pins
    // (they live only on screen).
    func blocked() -> Bool {
        if Recorder.isActive { Toast.shared.show("Finish the recording first", "Then update from the menu bar."); return true }
        if Overlay.active != nil || ScrollCapture.active != nil { Toast.shared.show("Finish the capture first", "Then update from the menu bar."); return true }
        if !Editor.instances.isEmpty || !VideoEditor.instances.isEmpty {
            Toast.shared.show("Close the editors first", "Updating restarts Ather Screenshot. Save and close the editors, then update from the menu bar.", ms: 6000)
            return true
        }
        let pins = Pin.all.count
        if pins > 0 {
            activateApp()
            let a = NSAlert()
            a.messageText = "Update now?"
            a.informativeText = "Updating restarts Ather Screenshot, which closes \(pins) pinned screenshot\(pins == 1 ? "" : "s")."
            a.addButton(withTitle: "Update")
            a.addButton(withTitle: "Cancel")
            if a.runModal() != .alertFirstButtonReturn { return true }
        }
        return false
    }

    // Only on a click (the notification or the menu-bar item).
    func install() {
        guard let info = available, !installing, !quitting, !blocked() else { return }
        if let why = Updater.canReplace(Bundle.main.bundleURL) { return failed(why) }
        installing = true
        Toast.shared.show("Downloading Ather Screenshot \(info.version)…", "0%", ms: 600_000)
        Task { @MainActor in
            defer { self.installing = false }
            do {
                var last = -1
                let app = try await Updater.download(info) { p in
                    let pct = Int((p * 100).rounded())
                    guard pct != last else { return }
                    last = pct
                    DispatchQueue.main.async { Toast.shared.show("Downloading Ather Screenshot \(info.version)…", "\(pct)%", ms: 600_000) }
                }
                // Quitting meanwhile, or something started while it downloaded: check again before restarting.
                if self.quitting { return }
                if self.blocked() { try? FileManager.default.removeItem(at: Updater.stagingFolder); return }
                _ = try Updater.startSwap(staged: app)
                Toast.shared.show("Restarting…", "Ather Screenshot \(info.version)")
                NSApp.terminate(nil)  // the helper swaps the app in once this one has quit
            } catch {
                self.failed(error.localizedDescription)
            }
        }
    }

    // Keeps the current version and offers the download page.
    func failed(_ why: String) {
        Toast.shared.show("The update didn't install", why + "\nClick to download it from GitHub instead.", ms: 12000) { NSWorkspace.shared.open(Updater.downloadPage) }
    }
}
