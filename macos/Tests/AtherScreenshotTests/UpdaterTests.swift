import Foundation
import XCTest
@testable import AtherScreenshot

final class UpdaterTests: XCTestCase {
    // Port of updater_compares_versions.
    func testComparesVersions() {
        XCTAssertGreaterThan(Updater.compareVersions("0.0.10", "0.0.9"), 0)
        XCTAssertLessThan(Updater.compareVersions("0.0.2", "0.1"), 0)
        XCTAssertEqual(Updater.compareVersions("1.0", "1.0.0"), 0)
        XCTAssertEqual(Updater.compareVersions("0.0.2", "0.0.2.0"), 0)
        XCTAssertGreaterThan(Updater.compareVersions("0.0.2.2", "0.0.2.1"), 0, "a re-release raises only the build")
    }

    private func manifest(_ url: String, hash: String = String(repeating: "a", count: 64), version: String = "0.0.3", key: String = "macos") -> Data {
        Data("{\"windows\":{\"version\":\"9.9.9\"},\"\(key)\":{\"version\":\"\(version)\",\"url\":\"\(url)\",\"sha256\":\"\(hash)\",\"size\":3705286,\"notes\":\"New things\"}}".utf8)
    }

    // Port of updater_manifest_only_from_this_repo_over_https (the macos entry).
    func testManifestOnlyFromThisRepoOverHTTPS() {
        let good = "https://github.com/AskTinNguyen/AtherScreenshot/releases/download/v0.0.3/AtherScreenshot-0.0.3-macOS.zip"
        let u = Updater.parseManifest(manifest(good))
        XCTAssertEqual(u?.version, "0.0.3")
        XCTAssertEqual(u?.size, 3705286)
        XCTAssertEqual(u?.notes, "New things")
        XCTAssertNotNil(Updater.parseManifest(Data([0xEF, 0xBB, 0xBF]) + manifest(good)), "BOM")
        XCTAssertNotNil(Updater.parseManifest(manifest("https://raw.githubusercontent.com/AskTinNguyen/AtherScreenshot/main/downloads/x.zip")))
        XCTAssertNotNil(Updater.parseManifest(manifest(good, version: "0.0.2.1")), "a build number")
        XCTAssertNil(Updater.parseManifest(manifest("http://github.com/AskTinNguyen/AtherScreenshot/raw/main/x.zip")), "not HTTPS")
        XCTAssertNil(Updater.parseManifest(manifest("https://github.com/SomeoneElse/AtherScreenshot/raw/main/x.zip")), "another repo")
        XCTAssertNil(Updater.parseManifest(manifest("https://evil.example/AskTinNguyen/AtherScreenshot/x.zip")), "another host")
        XCTAssertNil(Updater.parseManifest(manifest(good, hash: "1234")), "bad hash")
        XCTAssertNil(Updater.parseManifest(manifest(good, version: "0.0.3; rm")), "bad version")
        XCTAssertNil(Updater.parseManifest(manifest(good, key: "windows")), "the Windows entry isn't for the Mac")
        XCTAssertNil(Updater.parseManifest(manifest("http://localhost:8000/x.zip")))
        XCTAssertNotNil(Updater.parseManifest(manifest("http://localhost:8000/x.zip"), allowLocalhost: true), "test manifests only")
        // Redirects: GitHub's file hosts over HTTPS only.
        XCTAssertTrue(Updater.allowedRedirect(URL(string: "https://objects.githubusercontent.com/x"), allowLocalhost: false))
        XCTAssertFalse(Updater.allowedRedirect(URL(string: "http://objects.githubusercontent.com/x"), allowLocalhost: false))
        XCTAssertFalse(Updater.allowedRedirect(URL(string: "https://githubusercontent.com.evil.example/x"), allowLocalhost: false))
    }

    // A minimal signed bundle with our id and the given build, zipped like `build.sh package` does.
    private func fakeApp(in dir: URL, build: String, id: String = kBundleID, tamper: Bool = false) throws -> (zip: URL, info: UpdateInfo) {
        let app = dir.appendingPathComponent("Ather Screenshot.app")
        try? FileManager.default.removeItem(at: app)
        let macos = app.appendingPathComponent("Contents/MacOS")
        try FileManager.default.createDirectory(at: macos, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"), to: macos.appendingPathComponent("AtherScreenshot"))
        let plist: NSDictionary = ["CFBundleIdentifier": id, "CFBundleVersion": build, "CFBundleShortVersionString": "0.0.2",
                                   "CFBundleExecutable": "AtherScreenshot", "CFBundlePackageType": "APPL"]
        plist.write(to: app.appendingPathComponent("Contents/Info.plist"), atomically: true)
        XCTAssertEqual(Updater.run("/usr/bin/codesign", ["--force", "--sign", "-", app.path]), 0)
        if tamper { try Data("changed".utf8).write(to: app.appendingPathComponent("Contents/Info.plist")) }
        let zip = dir.appendingPathComponent("update-\(UUID().uuidString).zip")
        XCTAssertEqual(Updater.run("/usr/bin/ditto", ["-c", "-k", "--keepParent", app.path, zip.path]), 0)
        let size = Int64(try zip.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? 0)
        return (zip, UpdateInfo(version: build, url: "https://github.com/AskTinNguyen/AtherScreenshot/releases/download/v0.0.2/x.zip",
                                sha256: Updater.sha256(zip) ?? "", notes: "", size: size))
    }

    private func tempDir() throws -> URL {
        let d = FileManager.default.temporaryDirectory.appendingPathComponent("ather-update-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    private func fails(_ f: () throws -> Any, _ text: String = "", file: StaticString = #filePath, line: UInt = #line) {
        do {
            _ = try f()
            XCTFail("should fail", file: file, line: line)
        } catch {
            if !text.isEmpty { XCTAssertTrue(error.localizedDescription.contains(text), error.localizedDescription, file: file, line: line) }
        }
    }

    // Port of updater_verifies_and_swaps_the_exe: size, checksum, then the bundle inside; and the swap.
    func testVerifiesAndSwapsTheApp() throws {
        let dir = try tempDir()
        let (zip, info) = try fakeApp(in: dir, build: "0.0.2.1")
        XCTAssertEqual(info.sha256.count, 64)
        let unpack = dir.appendingPathComponent("unpacked")
        let app = try Updater.verify(zip: zip, info, unpackInto: unpack)
        XCTAssertEqual(app.lastPathComponent, "Ather Screenshot.app")
        var padded = info
        padded.version += ".0"  // "0.0.2.1.0" is the same build as the "0.0.2.1" inside
        XCTAssertNoThrow(try Updater.verify(zip: zip, padded, unpackInto: unpack))
        let rejected = dir.appendingPathComponent("rejected")
        var wrong = info
        wrong.sha256 = (wrong.sha256.first == "0" ? "1" : "0") + wrong.sha256.dropFirst()
        fails({ try Updater.verify(zip: zip, wrong, unpackInto: rejected) }, "checksum")
        wrong = info
        wrong.version = "9.9.9"  // the right file, but not the build the manifest promised
        fails({ try Updater.verify(zip: zip, wrong, unpackInto: rejected) }, "isn't Ather Screenshot")
        wrong = info
        wrong.size += 1
        fails({ try Updater.verify(zip: zip, wrong, unpackInto: rejected) }, "incomplete")
        let other = try fakeApp(in: try tempDir(), build: "0.0.2.1", id: "com.example.other")
        fails({ try Updater.verify(zip: other.zip, other.info, unpackInto: rejected) }, "isn't Ather Screenshot")
        let tampered = try fakeApp(in: try tempDir(), build: "0.0.2.1", tamper: true)
        fails({ try Updater.verify(zip: tampered.zip, tampered.info, unpackInto: rejected) })
        XCTAssertFalse(FileManager.default.fileExists(atPath: rejected.path), "a rejected download leaves nothing unpacked")

        // The swap: waits for the app to quit, puts the new bundle in place and removes the old one.
        let installed = try tempDir().appendingPathComponent("Ather Screenshot.app")
        try FileManager.default.createDirectory(at: installed, withIntermediateDirectories: true)
        try Data("old".utf8).write(to: installed.appendingPathComponent("marker"))
        let quit = Process()
        quit.executableURL = URL(fileURLWithPath: "/usr/bin/true")
        try quit.run()
        quit.waitUntilExit()  // an app that has already quit
        let p = try Updater.startSwap(staged: app, target: installed, pid: quit.processIdentifier, relaunch: false)
        p.waitUntilExit()
        XCTAssertEqual(p.terminationStatus, 0)
        XCTAssertFalse(FileManager.default.fileExists(atPath: installed.appendingPathComponent("marker").path))
        let plist = NSDictionary(contentsOf: installed.appendingPathComponent("Contents/Info.plist"))
        XCTAssertEqual(plist?["CFBundleVersion"] as? String, "0.0.2.1")
        XCTAssertFalse(FileManager.default.fileExists(atPath: app.path), "the staged copy moved")
        XCTAssertFalse(FileManager.default.fileExists(atPath: installed.path + ".old"), "the old one is gone")
        // A missing staged bundle leaves the installed one where it was.
        let p2 = try Updater.startSwap(staged: unpack.appendingPathComponent("Nothing.app"), target: installed, pid: quit.processIdentifier, relaunch: false)
        p2.waitUntilExit()
        XCTAssertNotEqual(p2.terminationStatus, 0)
        XCTAssertEqual((NSDictionary(contentsOf: installed.appendingPathComponent("Contents/Info.plist"))?["CFBundleVersion"] as? String), "0.0.2.1")
        // A copy running from a disk image can't replace itself.
        XCTAssertNotNil(Updater.canReplace(URL(fileURLWithPath: "/Volumes/Ather Screenshot/Ather Screenshot.app")))
    }

    // End to end over the network, opt-in: ATHER_UPDATE_URL=<a test server's latest.json> with a "macos" entry
    // for a build newer than 0.0.1 (the test pretends to be 0.0.1).
    func testFindsDownloadsAndVerifiesFromATestServer() async throws {
        guard ProcessInfo.processInfo.environment["ATHER_UPDATE_URL"] != nil else { throw XCTSkip("set ATHER_UPDATE_URL") }
        let saved = Updater.currentBuild
        Updater.currentBuild = "0.0.1"
        defer { Updater.currentBuild = saved }
        let info = try await Updater.check()
        let found = try XCTUnwrap(info, "a newer build")
        let dir = try tempDir()
        var last = 0.0
        let app = try await Updater.download(found, into: dir) { last = $0 }
        XCTAssertGreaterThan(last, 0.99)
        let plist = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist"))
        XCTAssertEqual(plist?["CFBundleVersion"] as? String, found.version)
        // A tampered manifest (wrong checksum) never leaves anything behind.
        var bad = found
        bad.sha256 = (bad.sha256.first == "0" ? "1" : "0") + bad.sha256.dropFirst()
        let dir2 = try tempDir()
        do {
            _ = try await Updater.download(bad, into: dir2) { _ in }
            XCTFail("tampered download installed")
        } catch {
            XCTAssertTrue(error.localizedDescription.contains("checksum"))
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: dir2.path))
    }
}
