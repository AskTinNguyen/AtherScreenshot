import AVFoundation
import AppKit

// The tools AI agents get over MCP (see MCP.swift): screenshots, recording with markers, the gallery, video frames,
// OCR, tags, settings and every palette command. Same names, arguments and results as the Windows app (agent.cpp).
enum Agent {
    typealias JSON = MCP.JSON
    private static let s = Settings.shared
    private static var server: MCP.HTTPServer?
    private static var client = "AI agent"  // the last client that connected (note author)
    private static var announced: [String: Date] = [:]

    // MARK: lifecycle

    static var token: String { AppDelegate.privateSecret("agent-token") }
    static var port: Int { min(65535, max(1024, s.int("AgentPort"))) }
    static var running: Bool { server?.running ?? false }

    // Starts or stops the server to match Settings (AllowAgents, AgentPort).
    static func apply() {
        let want = s.bool("AllowAgents")
        if want, let srv = server, srv.running, srv.port == port { return }
        server?.stop()
        server = nil
        guard want else { return }
        let srv = MCP.HTTPServer(server: makeServer(), token: token)
        do {
            try srv.start(port: port)
            server = srv
        } catch {
            Toast.shared.show("AI agents can't connect", error.localizedDescription, ms: 6000)
        }
    }

    static func stop() {
        server?.stop()
        server = nil
    }

    static func makeServer() -> MCP.Server {
        let srv = MCP.Server(info: .init(name: "ather-screenshot", title: "Ather Screenshot", version: build, instructions: instructions), tools: tools())
        srv.onInitialize = { name in
            DispatchQueue.main.async {
                client = name.isEmpty ? "AI agent" : name
                if let last = announced[client], Date().timeIntervalSince(last) < 600 { return }
                announced[client] = Date()
                Toast.shared.show("\(client) connected", "It can take screenshots and record with Ather Screenshot. Turn this off in Settings › AI agents.", ms: 5000)
            }
        }
        return srv
    }

    static var build: String { Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? AppDelegate.version }

    // What to paste into an agent's configuration.
    static var setupText: String {
        let exe = Bundle.main.executableURL?.path ?? "/Applications/Ather Screenshot.app/Contents/MacOS/AtherScreenshot"
        let quoted = exe.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"")
        return """
        Ather Screenshot for AI agents (MCP)

        Claude Code:
          claude mcp add ather-screenshot -- "\(quoted)" --mcp

        Codex (~/.codex/config.toml):
          [mcp_servers.ather-screenshot]
          command = "\(quoted)"
          args = ["--mcp"]

        Claude Desktop, Cursor and other JSON configs:
          "ather-screenshot": { "command": "\(quoted)", "args": ["--mcp"] }

        Clients that connect to a URL (Streamable HTTP):
          http://127.0.0.1:\(port)/mcp
          Header: Authorization: Bearer \(token)
        """
    }

    static func copySetup() {
        copyText(setupText)
        Toast.shared.show("AI agent setup copied", s.bool("AllowAgents") ? "Paste it into your agent's MCP settings."
            : "Paste it into your agent's MCP settings, then turn on “Allow AI agents (MCP)”.", ms: 5000)
    }

    // `AtherScreenshot --mcp`: the stdio server an agent starts. It relays to the running app, starting it if needed.
    static func runBridge() -> Int32 {
        MCP.runStdioBridge(port: port, token: token) {
            let me = ProcessInfo.processInfo.processIdentifier
            if NSRunningApplication.runningApplications(withBundleIdentifier: kBundleID).contains(where: { $0.processIdentifier != me && $0.activationPolicy != .prohibited }) {
                return false  // running: it just isn't accepting agents
            }
            let app = Bundle.main.bundleURL
            guard app.pathExtension == "app" else { return false }
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/open")
            p.arguments = ["-g", app.path]  // in the background, without stealing focus
            return (try? p.run()) != nil
        }
    }

    // MARK: helpers

    private static func rect(_ r: CGRect) -> JSON {
        guard !r.isNull, !r.isInfinite else { return ["x": 0, "y": 0, "width": 0, "height": 0] }  // no displays (headless)
        return ["x": Int(r.minX.rounded()), "y": Int(r.minY.rounded()), "width": Int(r.width.rounded()), "height": Int(r.height.rounded())]
    }

    private static func iso(_ t: Double) -> String {
        let f = ISO8601DateFormatter()
        return f.string(from: Date(timeIntervalSince1970: t))
    }

    private static func round(_ v: Double, _ places: Double = 100) -> Double { (v * places).rounded() / places }

    static func shrink(_ img: CGImage, _ side: Int) -> CGImage {
        guard side > 0, max(img.width, img.height) > side else { return img }
        let k = Double(side) / Double(max(img.width, img.height))
        let w = max(1, Int((Double(img.width) * k).rounded())), h = max(1, Int((Double(img.height) * k).rounded()))
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return img }
        ctx.interpolationQuality = .high
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage() ?? img
    }

    // "preview" (the default: longest side 1568 px, what vision models read best), "full" or "none".
    private static func imageSide(_ a: JSON) -> Int {
        switch MCP.str(a["image"]) {
        case "none": return -1
        case "full": return 0
        default: return 1568
        }
    }

    private static func picture(_ img: CGImage?, _ side: Int) -> [JSON] {
        guard side >= 0, let img, let png = shrink(img, side).pngData() else { return [] }
        return [MCP.image(png: png)]
    }

    private static func checkMedia(_ path: String?) throws -> URL {
        guard let path, !path.isEmpty else { throw MCP.failed("Pass the file's path (list_captures gives them).") }
        let u = URL(fileURLWithPath: (path as NSString).expandingTildeInPath)
        var dir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: u.path, isDirectory: &dir), !dir.boolValue else { throw MCP.failed("File not found: \(path)") }
        guard MediaFiles.isMedia(u) else { throw MCP.failed("Not a picture or video: \(path)") }
        return u
    }

    // Runs `body` on the main actor and replies with its result, or its error as a tool error.
    private static func onMain(_ reply: @escaping MCP.Reply, _ body: @escaping @MainActor () async throws -> JSON) {
        Task { @MainActor in
            do { reply(try await body()) } catch { reply(MCP.error(error.localizedDescription)) }
        }
    }

    private static func offMain(_ reply: @escaping MCP.Reply, _ body: @escaping () async throws -> JSON) {
        Task.detached {
            do { reply(try await body()) } catch { reply(MCP.error(error.localizedDescription)) }
        }
    }

    // MARK: displays, windows, targets

    struct Display {
        var rect: CGRect   // CG global points
        var primary: Bool
        var scale: CGFloat
    }

    static func displays() -> [Display] {
        NSScreen.screens.enumerated().map { i, sc in Display(rect: Geo.toCG(sc.frame), primary: i == 0, scale: sc.backingScaleFactor) }
    }

    private static func windowJSON(_ w: WindowInfo, active: CGWindowID?) -> JSON {
        ["id": Int(w.id), "title": w.title, "app": w.app, "rect": rect(w.frame), "active": w.id == active, "minimized": false]
    }

    private static func findWindow(_ a: JSON) throws -> WindowInfo {
        let list = Capture.windows()
        if let id = MCP.int(a["window_id"]) {
            guard let w = list.first(where: { Int($0.id) == id }) else { throw MCP.failed("No window with id \(id) (call list_windows for current ids).") }
            return w
        }
        if let q = MCP.str(a["window_title"])?.lowercased(), !q.isEmpty {
            if let w = list.first(where: { $0.title.lowercased() == q }) { return w }
            if let w = list.first(where: { $0.title.lowercased().contains(q) || $0.app.lowercased().contains(q) }) { return w }
            throw MCP.failed("No window matches \"\(q)\" (call list_windows to see them; minimized windows aren't listed).")
        }
        guard let w = Capture.activeWindow() else { throw MCP.failed("There's no active window. Name one with window_id or window_title.") }
        return w
    }

    struct Target {
        var kind = "screen"
        var rect = CGRect.zero
        var window: WindowInfo?
        var info = NameInfo()
    }

    static func resolve(_ a: JSON) throws -> Target {
        var t = Target()
        t.kind = MCP.str(a["target"]) ?? "screen"
        switch t.kind {
        case "screen":
            t.rect = Geo.virtualRect
        case "display":
            let ds = displays(), i = MCP.int(a["display"]) ?? 0
            guard ds.indices.contains(i) else { throw MCP.failed("There is no display \(i) (there are \(ds.count); 0 is the main one).") }
            t.rect = ds[i].rect
        case "window":
            let w = try findWindow(a)
            t.window = w
            t.rect = w.frame
            t.info = NameInfo(app: w.app, window: w.title)
        case "region":
            guard let x = MCP.num(a["x"]), let y = MCP.num(a["y"]), let w = MCP.num(a["width"]), let h = MCP.num(a["height"]) else {
                throw MCP.failed("A region needs x, y, width and height (screen points).")
            }
            let r = CGRect(x: x, y: y, width: w, height: h).intersection(Geo.virtualRect)
            guard !r.isNull, r.width >= 1, r.height >= 1 else { throw MCP.failed("The region is off screen. get_status lists the displays.") }
            t.rect = r
        default:
            throw MCP.failed("target must be screen, display, window or region.")
        }
        if t.info.app.isEmpty, let w = Capture.activeWindow() { t.info = NameInfo(app: w.app, window: w.title) }
        return t
    }

    @MainActor
    private static func ensurePermission() async throws {
        if Capture.hasPermission() { return }
        if await Capture.resolvePermission() { return }
        _ = Capture.ensurePermission()
        throw MCP.failed("Ather Screenshot needs the Screen Recording permission: " + (CaptureError.permission.errorDescription ?? ""))
    }

    @MainActor
    static func grab(_ t: Target, _ a: JSON) async throws -> CGImage {
        try await ensurePermission()
        let cursor = MCP.bool(a["include_cursor"]) ?? s.bool("CaptureCursor")
        Toast.shared.hide()  // our own notification must not be in the shot
        if let w = t.window {
            if MCP.bool(a["bring_to_front"]) == true {
                NSRunningApplication(processIdentifier: w.pid)?.activate()
                try await Task.sleep(nanoseconds: 250_000_000)
            }
            return try await Capture.window(w.id, cursor: cursor)  // works while covered too
        }
        return try await Capture.rect(t.rect, cursor: cursor)
    }

    // MARK: screenshots

    @MainActor
    private static func takeScreenshot(_ a: JSON) async throws -> JSON {
        var t = try resolve(a)
        let delay = min(60, max(0, MCP.num(a["delay_seconds"]) ?? 0))
        if delay > 0 {
            try await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            if let w = t.window { t.window = Capture.windows().first { $0.id == w.id } ?? w }  // it may have moved
        }
        var img = try await grab(t, a)
        var d: JSON = ["target": t.kind, "rect": rect(t.window?.frame ?? t.rect), "width": img.width, "height": img.height]
        if MCP.bool(a["redact"]) ?? s.bool("AutoRedact") {
            let source = img
            let (clean, n) = try await Task.detached { () -> (CGImage, Int) in
                let rects = OCR.findSensitive(try OCR.words(source))
                return (try OCR.pixelate(source, rects: rects), rects.count)
            }.value
            img = clean
            d["redacted"] = n
        }
        if MCP.bool(a["copy_to_clipboard"]) == true { d["copied_to_clipboard"] = copyImage(img) }
        let side = imageSide(a)
        guard MCP.bool(a["save"]) ?? true else {
            AppDelegate.shared?.setLast(img, url: nil)
            d["saved"] = false
            return MCP.result(d, extra: picture(img, side))
        }
        var info = t.info
        info.w = img.width
        info.h = img.height
        let url = Output.newCaptureURL(ext: "png", info: info)
        let tags = MCP.strings(a["tags"])
        Library.shared.noteCaptureDetails(url, tags: tags, comment: MCP.str(a["comment"]) ?? "")
        let ok = await withCheckedContinuation { c in Output.savePNG(img, to: url) { c.resume(returning: $0) } }
        guard ok else { throw MCP.failed("Couldn't save \(url.path)") }
        AppDelegate.shared?.setLast(img, url: url)
        d["saved"] = true
        d["path"] = url.path
        if !tags.isEmpty { d["tags"] = tags }
        return MCP.result(d, extra: picture(img, side))
    }

    // MARK: recording

    struct Marker {
        var seconds: Double, label: String, kind: String
    }

    private static var ours = false            // the running recording was started by an agent
    private static var markers: [Marker] = []
    private static var markersFor: Int?  // the Recorder.serial the markers belong to
    private static var recordingFps = 30.0
    private static var generation = 0
    private static var finishedUnread: JSON?   // the agent's recording ended by the user or max_seconds: the next stop_recording reports it
    private static var waiting: [MCP.Reply] = []
    private static var recTags: [String] = []
    private static var recComment = ""

    private static func markersJSON(_ ms: [Marker]) -> [JSON] {
        ms.map { ["seconds": round($0.seconds, 1000), "label": $0.label, "kind": $0.kind] }
    }

    // The markers on the running recording (markers left from an earlier one don't count).
    private static var currentMarkers: [Marker] {
        guard let r = Recorder.current, markersFor == r.serial else { return [] }
        return markers
    }

    private static var recordingSeconds: Double {
        guard let r = Recorder.current, r.hasFootage else { return 0 }
        return max(0, r.elapsed)
    }

    // Markers become the video's review notes, "<video>.notes.json" (the Windows video editor's format). Never over
    // notes that are already there.
    private static func writeNotes(_ video: URL, _ ms: [Marker], fps: Double, author: String) -> URL? {
        let u = URL(fileURLWithPath: video.path + ".notes.json")
        guard !ms.isEmpty, MediaFiles.isVideo(video), !FileManager.default.fileExists(atPath: u.path) else { return nil }
        let now = ISO8601DateFormatter().string(from: Date())
        let notes: [JSON] = ms.enumerated().map { i, m in
            ["id": i + 1, "frame": Int((m.seconds * fps).rounded()), "time": round(m.seconds, 1000), "kind": m.kind, "resolved": false,
             "author": author, "text": m.label, "created": now]
        }
        let doc: JSON = ["app": "Ather Screenshot", "format": 1, "video": video.lastPathComponent, "fps": fps, "notes": notes]
        return (try? Data(MCP.encode(doc).utf8).write(to: u, options: .atomic)) != nil ? u : nil
    }

    // What stop_recording reports for a finished recording; markers are saved with an MP4.
    private static func outcomeJSON(_ o: Recorder.Outcome, _ ms: [Marker], fps: Double) -> JSON {
        guard let u = o.url else { return o.error.map { ["error": $0] } ?? ["discarded": true] }
        var d: JSON = ["path": u.path, "format": u.pathExtension.lowercased(), "seconds": round(o.duration),
                       "bytes": (try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0, "markers": markersJSON(ms)]
        if let n = writeNotes(u, ms, fps: fps, author: client) { d["notes_file"] = n.path }
        else if !ms.isEmpty, MediaFiles.isVideo(u) { d["notes_warning"] = "The markers couldn't be saved as review notes." }
        return d
    }

    private static func toolResult(_ d: JSON) -> JSON {
        if let e = d["error"] { return MCP.error("The recording failed: \(e)") }
        return MCP.result(d)
    }

    // On the main thread, right as the recording ends (before quitting can finish), so the notes and tags are saved.
    private static func recordingFinished(_ o: Recorder.Outcome) {
        if let u = o.url { Library.shared.noteCaptureDetails(u, tags: recTags, comment: recComment) }
        let d = outcomeJSON(o, markers, fps: recordingFps)
        ours = false
        markers = []
        markersFor = nil
        let w = waiting
        waiting = []
        if w.isEmpty { finishedUnread = d }
        for reply in w { reply(toolResult(d)) }
    }

    @MainActor
    private static func startRecording(_ a: JSON) async throws -> JSON {
        guard !Recorder.isActive else { throw MCP.failed("A recording is already running. Call stop_recording first.") }
        var t = try resolve(a)
        try await ensurePermission()
        var note: String?
        if t.window == nil {  // a region records on one display: the one under its centre, like the recorder picks
            let ds = displays()
            let center = CGPoint(x: t.rect.midX, y: t.rect.midY)
            if let d = ds.first(where: { $0.rect.contains(center) }) ?? ds.first(where: { $0.rect.intersects(t.rect) }) {
                let clipped = t.rect.intersection(d.rect)
                if clipped != t.rect {
                    note = "A recording covers one display, so this records \(t.kind == "screen" ? "the display in the middle of the screen area" : "the part of the region on one display"). Pass target: \"display\" to pick one."
                    t.rect = clipped
                }
            }
        }
        guard t.rect.width >= 16, t.rect.height >= 16 else { throw MCP.failed("The area is too small to record (16 × 16 at least).") }
        let gif = MCP.str(a["format"]) == "gif"
        var o: [String: Any] = ["CountdownSeconds": min(10, max(0, MCP.int(a["countdown_seconds"]) ?? 0))]
        if let fps = MCP.int(a["fps"]) { o[gif ? "GifFps" : "VideoFps"] = gif ? min(50, max(1, fps)) : min(120, max(1, fps)) }
        let keys = ["include_cursor": "RecordCursor", "system_audio": "RecordSystemAudio", "microphone": "RecordMicrophone",
                    "show_clicks": "ShowClicks", "show_keys": "ShowKeys", "show_gamepad": "ShowGamepad"]
        for (arg, key) in keys { if let v = MCP.bool(a[arg]) { o[key] = v } }
        let follow = MCP.bool(a["follow_window"]) ?? true
        let target: RecordTarget = t.window.map { follow ? .window($0) : .region($0.frame) } ?? .region(t.rect)
        let fps = (o[gif ? "GifFps" : "VideoFps"] as? Int) ?? (gif ? min(50, max(1, s.int("GifFps"))) : min(120, max(1, s.int("VideoFps"))))
        guard let started = Recorder.start(gif: gif, target: target, configure: { r in
            r.overrides = o
            r.copiesToClipboard = false
            r.onFinish = { recordingFinished($0) }  // already on the main thread
        }) else { throw MCP.failed("Couldn't start recording.") }
        generation += 1
        let gen = generation, serialStarted = started.serial
        finishedUnread = nil
        markers = []
        markersFor = started.serial
        recTags = MCP.strings(a["tags"])
        recComment = MCP.str(a["comment"]) ?? ""
        recordingFps = Double(fps)
        ours = true
        AppDelegate.shared?.recordingChanged()
        let countdown = o["CountdownSeconds"] as? Int ?? 0
        let maxSeconds = min(MCP.num(a["max_seconds"]) ?? 0, 24 * 3600)
        if maxSeconds > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + maxSeconds + Double(countdown) + 0.5) {
                // Only this recording: not a later one, nor one the user started.
                if gen == generation, ours, let r = Recorder.current, r.serial == serialStarted { r.stop() }
            }
        }
        var d: JSON = ["recording": true, "format": gif ? "gif" : "mp4", "target": t.kind, "rect": rect(t.window?.frame ?? t.rect), "fps": fps,
                       "countdown_seconds": countdown, "path": "named when it's saved: stop_recording returns it"]
        if t.window != nil { d["follows_window"] = follow }
        if maxSeconds > 0 { d["max_seconds"] = maxSeconds }
        if let note { d["note"] = note }
        return MCP.result(d)
    }

    private static func stopRecording(_ a: JSON, _ reply: @escaping MCP.Reply) {
        let discard = MCP.bool(a["discard"]) ?? false
        if let d = finishedUnread {  // the agent's recording already ended (the user stopped it, or max_seconds): that's the answer
            finishedUnread = nil
            return reply(toolResult(d))
        }
        guard let r = Recorder.current else { return reply(MCP.error("Nothing is being recorded.")) }
        if ours {
            waiting.append(reply)
            return r.stop(discard: discard)
        }
        // Started from a shortcut or the menu: the recorder says how it ended.
        guard r.onFinish == nil else { return reply(MCP.error("That recording is already being stopped.")) }
        let ms = currentMarkers, fps = Double(min(120, max(1, s.int("VideoFps"))))
        markers = []
        markersFor = nil
        r.onFinish = { reply(toolResult(outcomeJSON($0, ms, fps: fps))) }
        r.stop(discard: discard)
    }

    private static func pauseRecording(_ a: JSON) throws -> JSON {
        guard let r = Recorder.current else { throw MCP.failed("Nothing is being recorded.") }
        let want = MCP.bool(a["paused"]) ?? true
        if r.hasFootage, r.isPaused != want { r.togglePause() }
        var d: JSON = ["paused": r.isPaused, "seconds": round(recordingSeconds)]
        if !r.hasFootage { d["note"] = "The recording hasn't started yet (countdown)." }
        return MCP.result(d)
    }

    private static func addMarker(_ a: JSON) throws -> JSON {
        guard let r = Recorder.current else { throw MCP.failed("Markers go on a running recording, and nothing is being recorded.") }
        guard let label = MCP.str(a["label"]), !label.isEmpty else { throw MCP.failed("A marker needs a label.") }
        let kind = ["note", "issue", "question", "good"].contains(MCP.str(a["kind"]) ?? "") ? MCP.str(a["kind"])! : "note"
        let at = MCP.num(a["seconds"]).map { min(max(0, $0), 24 * 3600) } ?? recordingSeconds
        if markersFor != r.serial {  // a new recording: earlier markers were for another one
            markers = []
            markersFor = r.serial
        }
        let m = Marker(seconds: at, label: label, kind: kind)
        markers.append(m)
        return MCP.result(["seconds": round(m.seconds, 1000), "label": m.label, "kind": m.kind, "markers": markers.count])
    }

    private static func recordingState() -> JSON {
        var j: JSON = ["state": "idle"]
        if let r = Recorder.current {
            j["state"] = !r.hasFootage ? "starting" : r.isPaused ? "paused" : "recording"
            j["seconds"] = round(recordingSeconds)
            j["started_by_agent"] = ours
            j["markers"] = markersJSON(currentMarkers)
        }
        if let f = finishedUnread { j["finished_unread"] = f }
        return j
    }

    // MARK: the gallery

    private static func captureJSON(_ u: URL, _ m: ItemMeta) -> JSON {
        var j: JSON = ["path": u.path, "type": MediaType.of(u).rawValue, "bytes": m.size, "modified": iso(m.mtime), "tags": m.tags]
        if m.w > 0 { j["width"] = m.w; j["height"] = m.h }
        if let d = m.duration { j["duration_seconds"] = round(d) }
        if m.rating > 0 { j["rating"] = m.rating }
        if !m.comment.isEmpty { j["comment"] = m.comment }
        if !m.app.isEmpty { j["app"] = m.app }
        if !m.window.isEmpty { j["window"] = m.window }
        if let t = m.text, !t.isEmpty { j["text"] = t.count > 300 ? String(t.prefix(300)) + "…" : t }
        let notes = u.path + ".notes.json"
        if MediaFiles.isVideo(u), FileManager.default.fileExists(atPath: notes) { j["notes_file"] = notes }
        return j
    }

    private static func listCaptures(_ a: JSON, _ reply: @escaping MCP.Reply) {
        Library.shared.refresh {
            let lib = Library.shared
            var f = Filter(text: MCP.str(a["query"]) ?? "", tags: MCP.strings(a["tags"]), minRating: min(5, max(0, MCP.int(a["min_rating"]) ?? 0)))
            if let t = MCP.str(a["type"]), let mt = MediaType(rawValue: t) { f.types = [mt] }
            let hits = lib.urls.filter { f.matches($0, lib.meta($0)) }.sorted { lib.meta($0).mtime > lib.meta($1).mtime }
            let offset = max(0, MCP.int(a["offset"]) ?? 0), limit = min(200, max(1, MCP.int(a["limit"]) ?? 20))
            let page = hits.dropFirst(offset).prefix(limit).map { captureJSON($0, lib.meta($0)) }
            reply(MCP.result(["total": hits.count, "captures": Array(page), "captures_folder": s.capturesFolder.path]))
        }
    }

    struct VideoInfo {
        var duration: Double, w: Int, h: Int, fps: Double
    }

    static func videoInfo(_ u: URL) async throws -> VideoInfo {
        let asset = AVURLAsset(url: u)
        let duration = try await asset.load(.duration).seconds
        guard let t = try await asset.loadTracks(withMediaType: .video).first else { throw MCP.failed("No video in \(u.lastPathComponent).") }
        let (size, tf, fps) = try await t.load(.naturalSize, .preferredTransform, .nominalFrameRate)
        let shown = CGRect(origin: .zero, size: size).applying(tf)
        return VideoInfo(duration: duration.isFinite ? duration : 0, w: Int(abs(shown.width).rounded()), h: Int(abs(shown.height).rounded()), fps: Double(fps))
    }

    static func frames(_ u: URL, at times: [Double], maxSide: Int) async throws -> [(Double, CGImage)] {
        let gen = AVAssetImageGenerator(asset: AVURLAsset(url: u))
        gen.appliesPreferredTrackTransform = true
        if maxSide > 0 { gen.maximumSize = CGSize(width: maxSide, height: maxSide) }
        let tol = CMTime(seconds: 0.02, preferredTimescale: 600)
        gen.requestedTimeToleranceBefore = tol
        gen.requestedTimeToleranceAfter = tol
        var out: [(Double, CGImage)] = []
        for t in times {
            if let img = try? await gen.image(at: CMTime(seconds: t, preferredTimescale: 600)).image { out.append((t, img)) }
        }
        return out
    }

    private static func getCapture(_ a: JSON) async throws -> JSON {
        let u = try checkMedia(MCP.str(a["path"]))
        let side = MCP.int(a["max_size"]).map { max(0, $0) } ?? 1568
        var d: JSON = ["path": u.path, "type": MediaType.of(u).rawValue]
        let img: CGImage
        if MediaFiles.isVideo(u) {
            let vi = try await videoInfo(u)
            let at = min(max(0, MCP.num(a["time_seconds"]) ?? 0), vi.duration)
            guard let f = try await frames(u, at: [at], maxSide: side).first else { throw MCP.failed("Couldn't read a frame from \(u.path)") }
            img = f.1
            d["width"] = vi.w; d["height"] = vi.h; d["duration_seconds"] = round(vi.duration); d["fps"] = round(vi.fps); d["time_seconds"] = at
        } else {
            guard let full = CGImage.load(u) else { throw MCP.failed("Couldn't open \(u.path)") }
            d["width"] = full.width
            d["height"] = full.height
            img = shrink(full, side)
        }
        return MCP.result(d, extra: picture(img, 0))
    }

    // Frames side by side, `cols` across, on black.
    static func contactSheet(_ imgs: [CGImage], cols: Int, cell: Int) -> CGImage? {
        guard let first = imgs.first else { return nil }
        let aspect = Double(first.width) / Double(max(1, first.height))
        let cellH = max(1, Int((Double(cell) / aspect).rounded())), gap = 4
        let rows = (imgs.count + cols - 1) / cols
        let W = cols * cell + (cols + 1) * gap, H = rows * cellH + (rows + 1) * gap
        guard let ctx = CGContext(data: nil, width: W, height: H, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.setFillColor(CGColor(gray: 0.06, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: W, height: H))
        ctx.interpolationQuality = .high
        for (i, img) in imgs.enumerated() {
            let k = min(Double(cell) / Double(img.width), Double(cellH) / Double(img.height))
            let w = Double(img.width) * k, h = Double(img.height) * k
            let x = Double(gap + (i % cols) * (cell + gap)) + (Double(cell) - w) / 2
            let top = Double(gap + (i / cols) * (cellH + gap)) + (Double(cellH) - h) / 2
            ctx.draw(img, in: CGRect(x: x, y: Double(H) - top - h, width: w, height: h))  // CG's origin is bottom-left
        }
        return ctx.makeImage()
    }

    private static func videoFrames(_ a: JSON) async throws -> JSON {
        let u = try checkMedia(MCP.str(a["path"]))
        guard MediaFiles.isVideo(u) else { throw MCP.failed("Not a video. Use get_capture for pictures and GIFs.") }
        let vi = try await videoInfo(u)
        guard vi.duration > 0 else { throw MCP.failed("Couldn't read \(u.path)") }
        var times = (a["times"] as? [Any] ?? []).compactMap { MCP.num($0) }.map { min(max(0, $0), vi.duration) }
        if times.isEmpty {  // evenly spread over [start, end]
            let count = min(16, max(1, MCP.int(a["count"]) ?? 6))
            let lo = min(max(0, MCP.num(a["start_seconds"]) ?? 0), vi.duration)
            let hi = min(max(lo, MCP.num(a["end_seconds"]) ?? vi.duration), vi.duration)
            times = (0..<count).map { count == 1 ? lo : lo + (hi - lo) * (Double($0) + 0.5) / Double(count) }
        }
        times = Array(times.prefix(16))
        let sheet = MCP.str(a["layout"]) == "sheet"
        let side = min(4096, max(64, MCP.int(a["max_size"]) ?? (sheet ? 1568 : 768)))
        let cols = sheet ? Int(Double(times.count).squareRoot().rounded(.up)) : 1
        let got = try await frames(u, at: times, maxSide: sheet ? max(64, side / cols) : side)
        guard !got.isEmpty else { throw MCP.failed("Couldn't decode frames from \(u.path)") }
        var d: JSON = ["path": u.path, "duration_seconds": round(vi.duration), "width": vi.w, "height": vi.h, "fps": round(vi.fps),
                       "times_seconds": got.map { round($0.0, 1000) }]
        var extra: [JSON] = []
        if sheet {
            d["layout"] = "sheet, left to right then top to bottom, \(cols) across"
            if let sh = contactSheet(got.map(\.1), cols: cols, cell: side / cols), let png = sh.pngData() { extra.append(MCP.image(png: png)) }
        } else {
            extra = got.compactMap { $0.1.pngData().map(MCP.image(png:)) }
        }
        return MCP.result(d, extra: extra)
    }

    private static func tagCapture(_ a: JSON) throws -> JSON {
        let u = URL(fileURLWithPath: MCP.str(a["path"]) ?? "")
        let lib = Library.shared
        guard lib.isInLibrary(u), FileManager.default.fileExists(atPath: u.path) else { throw MCP.failed("Not a capture in the gallery: \(u.path)") }
        if a["rating"] != nil, !MCP.isNumber(a["rating"]) { throw MCP.failed("rating must be a number from 0 to 5.") }
        if a["comment"] != nil, !(a["comment"] is String) { throw MCP.failed("comment must be text.") }
        let add = MCP.strings(a["add_tags"])
        if !add.isEmpty { lib.addTags(add, to: [u]) }
        for t in MCP.strings(a["remove_tags"]) { lib.removeTag(t, from: [u]) }
        if let r = MCP.int(a["rating"]) { lib.setRating(min(5, max(0, r)), [u]) }
        if let c = MCP.str(a["comment"]) { lib.setComment(c, u) }
        return MCP.result(captureJSON(u, lib.meta(u)))
    }

    // MARK: OCR

    @MainActor
    private static func readText(_ a: JSON) async throws -> JSON {
        var d: JSON = [:]
        let img: CGImage
        var origin = CGPoint.zero, scale: CGFloat = 1, screen = false
        if let path = MCP.str(a["path"]), !path.isEmpty {
            let u = try checkMedia(path)
            if MediaFiles.isVideo(u) {
                guard let f = try await frames(u, at: [max(0, MCP.num(a["time_seconds"]) ?? 0)], maxSide: 0).first else { throw MCP.failed("Couldn't read a frame.") }
                img = f.1
            } else {
                guard let i = CGImage.load(u) else { throw MCP.failed("Couldn't open \(u.path)") }
                img = i
            }
            d["path"] = u.path
        } else {
            let t = try resolve(a)
            img = try await grab(t, a)
            let r = t.window?.frame ?? t.rect
            origin = r.origin
            scale = CGFloat(img.width) / max(1, r.width)
            screen = true
            d["rect"] = rect(r)
        }
        let source = img
        let words = try await Task.detached { try OCR.words(source) }.value
        var text = "", line = -1
        for w in words {
            if !text.isEmpty { text += w.line != line ? "\n" : " " }
            text += w.text
            line = w.line
        }
        d["text"] = text
        if MCP.bool(a["with_boxes"]) == true {
            d["words"] = words.map { w -> JSON in
                let r = screen ? CGRect(x: origin.x + w.rect.minX / scale, y: origin.y + w.rect.minY / scale, width: w.rect.width / scale, height: w.rect.height / scale) : w.rect
                var j = rect(r)
                j["text"] = w.text
                j["line"] = w.line
                return j
            }
            d["coordinates"] = screen ? "screen points" : "image pixels"
        }
        return MCP.result(d)
    }

    // MARK: settings and status

    private static let settingKeys: [(String, String, ClosedRange<Int>?)] = [
        ("copy_to_clipboard", "CopyToClipboard", nil), ("save_to_file", "SaveToFile", nil), ("capture_cursor", "CaptureCursor", nil),
        ("auto_redact", "AutoRedact", nil), ("show_notifications", "ShowToast", nil), ("video_fps", "VideoFps", 5...120),
        ("gif_fps", "GifFps", 2...50), ("countdown_seconds", "CountdownSeconds", 0...10), ("record_cursor", "RecordCursor", nil),
        ("system_audio", "RecordSystemAudio", nil), ("microphone", "RecordMicrophone", nil), ("show_clicks", "ShowClicks", nil),
        ("show_keys", "ShowKeys", nil), ("show_gamepad", "ShowGamepad", nil), ("gamepad_opacity", "GamepadOpacity", 10...100),
    ]

    private static func settingsJSON() -> JSON {
        var j: JSON = [:]
        for (name, key, range) in settingKeys { j[name] = range == nil ? s.bool(key) as Any : s.int(key) as Any }
        j["gamepad_corner"] = s.string("GamepadCorner")
        j["after_capture"] = s.string("AfterCapture")
        return j
    }

    private static func configure(_ a: JSON) throws -> JSON {
        // Everything is checked before anything is written: a wrong value changes nothing.
        for (name, _, range) in settingKeys where a[name] != nil {
            if let range, !MCP.isNumber(a[name]) { throw MCP.failed("\(name) must be a number from \(range.lowerBound) to \(range.upperBound).") }
            if range == nil, !MCP.isBool(a[name]) { throw MCP.failed("\(name) must be true or false.") }
        }
        if a["gamepad_corner"] != nil, !["topleft", "topright", "bottomleft", "bottomright"].contains(MCP.str(a["gamepad_corner"]) ?? "") {
            throw MCP.failed("gamepad_corner is topleft, topright, bottomleft or bottomright.")
        }
        var changed: [String] = []
        for (name, key, range) in settingKeys where a[name] != nil {
            if let range { s.set(key, min(range.upperBound, max(range.lowerBound, MCP.int(a[name]) ?? range.lowerBound))) }
            else { s.set(key, MCP.bool(a[name]) ?? false) }
            changed.append(name)
        }
        if let c = MCP.str(a["gamepad_corner"]) {
            s.set("GamepadCorner", c)
            changed.append("gamepad_corner")
        }
        return MCP.result(["changed": changed, "settings": settingsJSON()])
    }

    private static func status() -> JSON {
        let active = Capture.activeWindow()
        var d: JSON = [
            "app": "Ather Screenshot", "version": build, "platform": "macos", "captures_folder": s.capturesFolder.path,
            "recording": recordingState(), "virtual_screen": rect(Geo.virtualRect), "settings": settingsJSON(),
            "screen_recording_permission": Capture.hasPermission(),
            "coordinates": "Points (CG global): the main display's top-left is 0,0. Pictures are in pixels (points × scale).",
            "displays": displays().enumerated().map { i, disp -> JSON in
                var j = rect(disp.rect)
                j["index"] = i
                j["primary"] = disp.primary
                j["scale"] = Double(disp.scale)
                return j
            },
        ]
        if let w = active { d["active_window"] = windowJSON(w, active: w.id) }
        return MCP.result(d)
    }

    private static func listWindows(_ a: JSON) -> JSON {
        let q = (MCP.str(a["query"]) ?? "").lowercased()
        let active = Capture.activeWindow()?.id
        let list = Capture.windows().filter { q.isEmpty || $0.title.lowercased().contains(q) || $0.app.lowercased().contains(q) }
        return MCP.result(["windows": list.map { windowJSON($0, active: active) }])
    }

    // MARK: the rest

    private static func upload(_ a: JSON, _ reply: @escaping MCP.Reply) {
        do {
            let u = try checkMedia(MCP.str(a["path"]))
            let cfg = s.uploadConfig
            if let p = Upload.problem(cfg) { return reply(MCP.error("Upload isn't set up: \(p)")) }
            Upload.upload(u, cfg) { r in
                switch r {
                case .success(let link): reply(MCP.result(["path": u.path, "url": link]))
                case .failure(let e): reply(MCP.error("Upload failed: \(e.localizedDescription)"))
                }
            }
        } catch { reply(MCP.error(error.localizedDescription)) }
    }

    static let commands: [CmdDef] = kCmds.filter { ![.quit, .toggleAgents, .copyAgentSetup].contains($0.cmd) }

    private static func runCommand(_ a: JSON) throws -> JSON {
        let want = (MCP.str(a["command"]) ?? "").lowercased()
        guard let c = commands.first(where: { $0.cmd.rawValue.lowercased() == want }) else { throw MCP.failed("Unknown command: \(want)") }
        AppDelegate.shared?.execute(c.cmd)
        return MCP.result(["ran": c.cmd.rawValue, "title": c.title])
    }

    // MARK: the tool list

    private static let targetProps: JSON = [
        "target": ["type": "string", "enum": ["screen", "display", "window", "region"],
                   "description": "screen = every display; display = one display (see display); window = one app window (see window_id / window_title, else the active window); region = x, y, width, height. Default: screen."],
        "display": ["type": "integer", "minimum": 0, "description": "Display index from get_status (0 = the main display)."],
        "window_id": ["type": "integer", "description": "Window id from list_windows."],
        "window_title": ["type": "string", "description": "Part of a window title or app name, e.g. the game's name."],
        "bring_to_front": ["type": "boolean", "description": "Activate the window's app first. Windows are captured even when covered."],
        "x": ["type": "integer"], "y": ["type": "integer"], "width": ["type": "integer", "minimum": 1], "height": ["type": "integer", "minimum": 1],
        "include_cursor": ["type": "boolean", "description": "Draw the mouse pointer. Default: the app's setting."],
    ]

    private static func schema(_ props: JSON, required: [String] = []) -> JSON {
        var j: JSON = ["type": "object", "properties": props]
        if !required.isEmpty { j["required"] = required }
        return j
    }

    private static func merged(_ a: JSON, _ b: JSON) -> JSON { a.merging(b) { x, _ in x } }

    private static let boolean: JSON = ["type": "boolean"]
    private static let stringList: JSON = ["type": "array", "items": ["type": "string"]]

    static func tools() -> [MCP.Tool] {
        func tool(_ name: String, _ title: String, _ description: String, _ schema: JSON, readOnly: Bool = false, mainThread: Bool = true,
                  timeout: TimeInterval = 60, _ run: @escaping (JSON, @escaping MCP.Reply) -> Void) -> MCP.Tool {
            MCP.Tool(name: name, title: title, description: description, schema: schema, readOnly: readOnly, mainThread: mainThread, timeout: timeout, run: run)
        }
        func sync(_ body: @escaping (JSON) throws -> JSON) -> (JSON, @escaping MCP.Reply) -> Void {
            { a, reply in
                do { reply(try body(a)) } catch { reply(MCP.error(error.localizedDescription)) }
            }
        }
        let commandList = commands.map { "\n- \($0.cmd.rawValue): \($0.title)" }.joined()
        return [
            tool("get_status", "Status",
                 "Ather Screenshot's state: version, captures folder, displays (index, position, size, scale), the active window, whether something is being recorded (and for how long, with its markers), and the capture and recording settings. Call it first to learn the screen layout.",
                 schema([:]), readOnly: true, sync { _ in status() }),
            tool("list_windows", "List windows",
                 "Open app windows, front to back, with ids, titles, app names and screen rectangles. Use the id or the title as a screenshot or recording target.",
                 schema(["query": ["type": "string", "description": "Only windows whose title or app name contains this."]]), readOnly: true, sync(listWindows)),
            tool("take_screenshot", "Take a screenshot",
                 "Captures the screen, a display, a window or a region without any UI, saves it as a PNG in the captures folder (it shows in the gallery), and returns its path and the picture. Good for playtesting: tag shots (e.g. bug, level-3) and add a comment, then find them with list_captures.",
                 schema(merged(targetProps, [
                     "delay_seconds": ["type": "number", "minimum": 0, "maximum": 60, "description": "Wait this long first."],
                     "redact": ["type": "boolean", "description": "Pixelate emails, keys, card and phone numbers found by OCR. Default: the app's setting."],
                     "save": ["type": "boolean", "description": "Save the PNG (default true). false only returns the picture."],
                     "copy_to_clipboard": ["type": "boolean", "description": "Also copy it to the clipboard (default false)."],
                     "tags": merged(stringList, ["description": "Gallery tags for the saved file."]),
                     "comment": ["type": "string", "description": "A gallery comment for the saved file."],
                     "image": ["type": "string", "enum": ["preview", "full", "none"], "description": "What picture to return: preview (default, at most 1568 px), full size, or none."],
                 ])), timeout: 120) { a, reply in onMain(reply) { try await takeScreenshot(a) } },
            tool("start_recording", "Start recording",
                 "Starts recording the screen, a display, a window (follows it, even when covered) or a region to MP4 (with system audio and microphone if on) or GIF. Returns right away; call stop_recording to finish and get the file. Records the game controller, clicks and keystrokes when those overlays are on. Use add_marker to note moments while it runs.",
                 schema(merged(targetProps, [
                     "format": ["type": "string", "enum": ["mp4", "gif"], "description": "Default mp4."],
                     "fps": ["type": "integer", "minimum": 1, "maximum": 120, "description": "Frames per second. Default: the app's setting (30 for MP4, 15 for GIF)."],
                     "follow_window": ["type": "boolean", "description": "For a window target: follow the window (default true) instead of recording its current area."],
                     "countdown_seconds": ["type": "integer", "minimum": 0, "maximum": 10, "description": "On-screen countdown before recording. Default 0."],
                     "max_seconds": ["type": "number", "minimum": 1, "description": "Stop by itself after this long."],
                     "system_audio": boolean, "microphone": boolean, "show_clicks": boolean,
                     "show_keys": ["type": "boolean", "description": "Careful: also shows typed passwords."],
                     "show_gamepad": ["type": "boolean", "description": "Draw the game controller in a corner."],
                     "tags": stringList, "comment": ["type": "string"],
                 ]))) { a, reply in onMain(reply) { try await startRecording(a) } },
            tool("stop_recording", "Stop recording",
                 "Stops the recording, waits until the file is saved and returns its path, length, size and markers. Also reports a recording that already ended (stopped by the user, or max_seconds).",
                 schema(["discard": ["type": "boolean", "description": "Throw the recording away instead of saving it."]]), timeout: 600, stopRecording),
            tool("pause_recording", "Pause or resume recording", "Pauses (paused: true, the default) or resumes the recording.",
                 schema(["paused": boolean]), sync(pauseRecording)),
            tool("add_marker", "Mark a moment",
                 "Marks the current moment of the running recording (e.g. \"boss fight starts\", \"clipping through the wall\"). Markers come back from stop_recording and are saved with an MP4 as review notes (<video>.notes.json).",
                 schema(["label": ["type": "string"],
                         "kind": ["type": "string", "enum": ["note", "issue", "question", "good"], "description": "Default note. issue = a bug."],
                         "seconds": ["type": "number", "minimum": 0, "description": "When, in recording time. Default: now."]], required: ["label"]),
                 sync(addMarker)),
            tool("list_captures", "Find captures",
                 "Searches the gallery (screenshots, GIFs and recordings in the captures folder), newest first. The query works like the gallery's search: words in file names, tags, app and window names and text inside pictures, related words (\"graph\" finds charts) and dates (\"today\", \"last week\").",
                 schema(["query": ["type": "string"], "type": ["type": "string", "enum": ["image", "gif", "video"]],
                         "tags": merged(stringList, ["description": "Only captures with all of these tags."]),
                         "min_rating": ["type": "integer", "minimum": 0, "maximum": 5],
                         "limit": ["type": "integer", "minimum": 1, "maximum": 200, "description": "Default 20."], "offset": ["type": "integer", "minimum": 0]]),
                 readOnly: true, listCaptures),
            tool("get_capture", "View a capture",
                 "Returns a picture (or one frame of a video, at time_seconds) and its size, so you can look at an earlier capture.",
                 schema(["path": ["type": "string"], "time_seconds": ["type": "number", "minimum": 0, "description": "For videos: which moment. Default 0."],
                         "max_size": ["type": "integer", "minimum": 0, "description": "Longest side in pixels (default 1568, 0 = full size)."]], required: ["path"]),
                 readOnly: true, mainThread: false) { a, reply in offMain(reply) { try await getCapture(a) } },
            tool("get_video_frames", "Look through a video",
                 "Frames from a video: at the given times, or `count` frames spread evenly between start_seconds and end_seconds. layout \"sheet\" puts them in one contact-sheet picture (cheaper to read). Use it to review a gameplay recording, or the moments around its markers.",
                 schema(["path": ["type": "string"], "times": ["type": "array", "items": ["type": "number"], "maxItems": 16],
                         "count": ["type": "integer", "minimum": 1, "maximum": 16, "description": "Default 6."],
                         "start_seconds": ["type": "number", "minimum": 0], "end_seconds": ["type": "number", "minimum": 0],
                         "layout": ["type": "string", "enum": ["frames", "sheet"], "description": "Default frames (one picture each)."],
                         "max_size": ["type": "integer", "minimum": 64, "maximum": 4096, "description": "Longest side of each frame (default 768), or of the sheet (default 1568)."]],
                        required: ["path"]),
                 readOnly: true, mainThread: false, timeout: 180) { a, reply in offMain(reply) { try await videoFrames(a) } },
            tool("read_text", "Read text (OCR)",
                 "Reads the text in a picture file, a video frame, or a fresh capture of the screen, a display, a window or a region (nothing is saved). with_boxes adds each word's rectangle, in screen points for captures, so you can find a button or a line of dialogue on screen.",
                 schema(merged(targetProps, ["path": ["type": "string", "description": "A picture or video file. Without it, the target is captured."],
                                             "time_seconds": ["type": "number", "minimum": 0, "description": "For videos."], "with_boxes": boolean])),
                 readOnly: true) { a, reply in onMain(reply) { try await readText(a) } },
            tool("tag_capture", "Tag a capture", "Adds or removes gallery tags, sets the rating (0-5) or the comment of a capture.",
                 schema(["path": ["type": "string"], "add_tags": stringList, "remove_tags": stringList,
                         "rating": ["type": "integer", "minimum": 0, "maximum": 5], "comment": ["type": "string"]], required: ["path"]),
                 sync(tagCapture)),
            tool("configure", "Change settings",
                 "Changes capture and recording settings (they stay changed, as if set in Settings). Only the ones given change; returns them all.",
                 schema(merged(Dictionary(uniqueKeysWithValues: settingKeys.map { name, _, range -> (String, Any) in
                     (name, range.map { ["type": "integer", "minimum": $0.lowerBound, "maximum": $0.upperBound] as JSON } ?? boolean)
                 }), ["gamepad_corner": ["type": "string", "enum": ["topleft", "topright", "bottomleft", "bottomright"]]])),
                 sync(configure)),
            tool("open_in_editor", "Open in an editor",
                 "Opens a picture in the annotation editor or a video in the video editor, for the person at the computer (trim, captions, markup).",
                 schema(["path": ["type": "string"]], required: ["path"]), sync { a in
                     let u = try checkMedia(MCP.str(a["path"]))
                     AppDelegate.shared?.openToEdit(u)
                     return MCP.result(["opened": u.path, "editor": MediaFiles.isVideo(u) ? "video" : "image"])
                 }),
            tool("upload_capture", "Upload a capture",
                 "Uploads a file with the uploader set up in Settings › Upload (Imgur, a custom endpoint or S3) and returns its link. Sends the file off this computer.",
                 schema(["path": ["type": "string"]], required: ["path"]), timeout: 300, upload),
            tool("run_command", "Run a palette command",
                 "Runs any command from the command palette, like pressing its shortcut. Commands that select a region wait for the person at the computer. Commands:" + commandList,
                 schema(["command": ["type": "string", "enum": commands.map(\.cmd.rawValue)]], required: ["command"]), sync(runCommand)),
        ]
    }

    static let instructions = "Ather Screenshot is a screenshot and screen-recording app running on this computer. Use it to see the screen and to record what happens, e.g. while playtesting a game: take_screenshot to look (and keep tagged evidence), start_recording / add_marker / stop_recording for gameplay sessions, get_video_frames to review a recording, read_text to read on-screen text, list_captures to find earlier captures. Coordinates are screen points; get_status lists the displays. Everything is saved in the user's captures folder and gallery."
}
