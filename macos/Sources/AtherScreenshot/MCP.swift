import Foundation
#if canImport(Darwin)
import Darwin
private let kSockStream = SOCK_STREAM
#else
import Glibc
private let kSockStream = Int32(SOCK_STREAM.rawValue)
#endif

// Model Context Protocol (https://modelcontextprotocol.io) for AI agents such as Claude Code or Codex.
// The running app serves it over Streamable HTTP on 127.0.0.1 (plain JSON responses, no SSE, no sessions);
// `AtherScreenshot --mcp` is a stdio server for agents that start one, relaying to the running app.
// Foundation only (no AppKit), the same protocol and checks as the Windows app's mcp.cpp.
enum MCP {
    typealias JSON = [String: Any]
    // A CallToolResult: ["content": [...], "structuredContent"?: [...], "isError"?: true]. Exactly once, any thread.
    typealias Reply = (JSON) -> Void

    struct Tool {
        var name: String
        var title: String
        var description: String
        var schema: JSON           // a JSON Schema object
        var readOnly = false       // annotations.readOnlyHint
        var mainThread = true      // run on the main queue; false: on the request's own thread
        var timeout: TimeInterval = 60
        var run: (JSON, @escaping Reply) -> Void
    }

    struct ServerInfo {
        var name: String, title: String, version: String, instructions: String
    }

    static let protocolVersions = ["2025-06-18", "2025-03-26", "2024-11-05"]  // newest first

    // MARK: results

    static func text(_ s: String) -> JSON { ["type": "text", "text": s] }
    static func image(png: Data) -> JSON { ["type": "image", "data": png.base64EncodedString(), "mimeType": "image/png"] }

    static func encode(_ v: Any) -> String {
        guard JSONSerialization.isValidJSONObject(v) || v is NSNull,
              let d = try? JSONSerialization.data(withJSONObject: v, options: [.sortedKeys, .withoutEscapingSlashes, .fragmentsAllowed])
        else { return "null" }
        return String(decoding: d, as: UTF8.self)
    }

    // The data as text and as structuredContent, then `extra` (pictures).
    static func result(_ data: JSON, extra: [JSON] = []) -> JSON {
        ["content": [text(encode(data))] + extra, "structuredContent": data]
    }

    static func error(_ message: String) -> JSON { ["content": [text(message)], "isError": true] }

    // MARK: argument helpers (JSONSerialization numbers are NSNumber, booleans too)

    static func num(_ v: Any?) -> Double? { (v as? NSNumber)?.doubleValue }
    // Clamped, so a huge number can't trap the conversion.
    static func int(_ v: Any?) -> Int? {
        guard let d = num(v), d.isFinite else { return nil }
        return Int(min(max(d, -1e15), 1e15).rounded())
    }
    static func bool(_ v: Any?) -> Bool? { (v as? NSNumber)?.boolValue }
    // A JSON true/false, not a number that happens to be 0 or 1.
    static func isBool(_ v: Any?) -> Bool {
        #if canImport(Darwin)
        guard let n = v as? NSNumber else { return false }
        return CFGetTypeID(n) == CFBooleanGetTypeID()
        #else
        guard let n = v as? NSNumber else { return false }
        return v is Bool && String(cString: n.objCType) == "c"
        #endif
    }
    static func isNumber(_ v: Any?) -> Bool { v is NSNumber && !isBool(v) }
    static func str(_ v: Any?) -> String? { v as? String }
    static func strings(_ v: Any?) -> [String] {
        if let s = v as? String, !s.isEmpty { return [s] }
        return (v as? [Any] ?? []).compactMap { $0 as? String }.filter { !$0.isEmpty }
    }

    // MARK: protocol

    final class Server {
        let info: ServerInfo
        let tools: [Tool]
        var onInitialize: ((String) -> Void)?  // a client connected (its name), on the request's thread
        // How tool work reaches the main thread; tests run it inline.
        var onMain: (@escaping () -> Void) -> Void = { DispatchQueue.main.async(execute: $0) }

        init(info: ServerInfo, tools: [Tool]) {
            self.info = info
            self.tools = tools
        }

        private func response(_ id: Any, _ result: Any) -> JSON { ["jsonrpc": "2.0", "id": id, "result": result] }
        private func failure(_ id: Any, _ code: Int, _ message: String) -> JSON {
            ["jsonrpc": "2.0", "id": id, "error": ["code": code, "message": message]]
        }

        // One JSON-RPC message in, the response text out ("" for notifications and responses: HTTP 202).
        // Blocks while a tool runs, so never call it on the main thread.
        func handle(_ body: Data) -> String {
            guard let msg = try? JSONSerialization.jsonObject(with: body, options: [.fragmentsAllowed]) else {
                return MCP.encode(failure(NSNull(), -32700, "Parse error"))
            }
            if let batch = msg as? [Any] {  // 2025-03-26 batches: answer each request in it
                let out = batch.compactMap { m -> Any? in
                    guard let d = try? JSONSerialization.data(withJSONObject: m, options: [.fragmentsAllowed]) else { return nil }
                    let r = handle(d)
                    return r.isEmpty ? nil : try? JSONSerialization.jsonObject(with: Data(r.utf8))
                }
                return out.isEmpty ? "" : MCP.encode(out)
            }
            guard let m = msg as? JSON else { return MCP.encode(failure(NSNull(), -32600, "Invalid request")) }
            guard let method = m["method"] as? String else { return "" }  // a response to us: nothing to do
            guard let id = m["id"] else { return "" }  // notifications need no answer
            let params = m["params"] as? JSON ?? [:]
            switch method {
            case "initialize":
                let asked = params["protocolVersion"] as? String ?? ""
                let version = MCP.protocolVersions.contains(asked) ? asked : MCP.protocolVersions[0]
                onInitialize?((params["clientInfo"] as? JSON)?["name"] as? String ?? "")
                var r: JSON = [
                    "protocolVersion": version,
                    "capabilities": ["tools": ["listChanged": false]],
                    "serverInfo": ["name": info.name, "title": info.title, "version": info.version],
                ]
                if !info.instructions.isEmpty { r["instructions"] = info.instructions }
                return MCP.encode(response(id, r))
            case "ping":
                return MCP.encode(response(id, JSON()))
            case "tools/list":
                let list: [JSON] = tools.map {
                    ["name": $0.name, "title": $0.title, "description": $0.description, "inputSchema": $0.schema,
                     "annotations": ["title": $0.title, "readOnlyHint": $0.readOnly, "destructiveHint": false, "openWorldHint": false]]
                }
                return MCP.encode(response(id, ["tools": list]))
            case "tools/call":
                let name = params["name"] as? String ?? ""
                guard let tool = tools.first(where: { $0.name == name }) else { return MCP.encode(failure(id, -32602, "Unknown tool: \(name)")) }
                return MCP.encode(response(id, call(tool, params["arguments"] as? JSON ?? [:])))
            default:
                return MCP.encode(failure(id, -32601, "Method not found: \(method)"))
            }
        }

        private func call(_ tool: Tool, _ args: JSON) -> JSON {
            let lock = NSLock()
            let sem = DispatchSemaphore(value: 0)
            var result: JSON?
            var open = true
            let reply: Reply = { r in
                lock.lock()
                defer { lock.unlock() }
                guard open, result == nil else { return }  // a late reply, after the call gave up, is dropped
                result = r
                sem.signal()
            }
            if tool.mainThread { onMain { tool.run(args, reply) } } else { tool.run(args, reply) }
            let inTime = sem.wait(timeout: .now() + tool.timeout) == .success
            lock.lock()
            defer { lock.unlock() }
            open = false
            if !inTime || result == nil { return MCP.error("Ather Screenshot didn't finish “\(tool.name)” in time.") }
            return result!
        }
    }

    // MARK: HTTP

    struct HTTPRequest {
        var method = "", path = "", body = Data()
        var headers: [(String, String)] = []  // names lower-cased
        func header(_ name: String) -> String { headers.first { $0.0 == name }?.1 ?? "" }
    }

    static let maxRequest = 8 << 20

    enum Parse { case incomplete, bad, done(HTTPRequest) }

    static func parseRequest(_ raw: Data) -> Parse {
        let sep = Data("\r\n\r\n".utf8)
        guard let end = raw.range(of: sep) else { return raw.count > 64 * 1024 ? .bad : .incomplete }
        let head = String(decoding: raw[raw.startIndex..<end.lowerBound], as: UTF8.self)
        var lines = head.components(separatedBy: "\r\n")
        let first = lines.removeFirst().split(separator: " ", omittingEmptySubsequences: false)
        guard first.count >= 3, first[2].hasPrefix("HTTP/") else { return .bad }
        var r = HTTPRequest()
        r.method = String(first[0])
        r.path = String(first[1].split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
        for l in lines {
            guard let c = l.firstIndex(of: ":") else { continue }
            r.headers.append((l[..<c].trimmingCharacters(in: .whitespaces).lowercased(), l[l.index(after: c)...].trimmingCharacters(in: .whitespaces)))
        }
        let rest = raw[end.upperBound...]
        if r.header("transfer-encoding").lowercased().contains("chunked") {
            var p = rest.startIndex
            var body = Data()
            while true {
                guard let eol = rest[p...].range(of: Data("\r\n".utf8)) else { return .incomplete }
                let n = Int(String(decoding: rest[p..<eol.lowerBound], as: UTF8.self).trimmingCharacters(in: .whitespaces), radix: 16) ?? -1
                if n < 0 || n > maxRequest - body.count { return .bad }
                if n == 0 { break }
                let start = eol.upperBound
                guard rest.distance(from: start, to: rest.endIndex) >= n + 2 else { return .incomplete }
                let stop = rest.index(start, offsetBy: n)
                body.append(rest[start..<stop])
                p = rest.index(stop, offsetBy: 2)
            }
            r.body = body
        } else {
            let n = Int(r.header("content-length")) ?? 0
            if n < 0 || n > maxRequest { return .bad }
            if rest.count < n { return .incomplete }
            r.body = Data(rest.prefix(n))
        }
        return .done(r)
    }

    // 0 when the request may go on, else the HTTP status to refuse it with. Only this computer's own clients:
    // a web page could reach 127.0.0.1 through DNS rebinding (wrong Host) or a cross-site request (its Origin).
    static func check(_ r: HTTPRequest, port: Int, token: String) -> Int {
        let host = r.header("host").lowercased()
        guard ["127.0.0.1:\(port)", "localhost:\(port)", "127.0.0.1", "localhost"].contains(host) else { return 403 }
        let origin = r.header("origin").lowercased()
        if !origin.isEmpty && origin != "http://127.0.0.1:\(port)" && origin != "http://localhost:\(port)" { return 403 }
        guard r.path == "/mcp" || r.path == "/mcp/" else { return 404 }
        let auth = r.header("authorization")
        guard auth.lowercased().hasPrefix("bearer "), sameSecret(String(auth.dropFirst(7)).trimmingCharacters(in: .whitespaces), token) else { return 401 }
        return 0
    }

    static func sameSecret(_ a: String, _ b: String) -> Bool {  // constant time for equal lengths
        let x = Array(a.utf8), y = Array(b.utf8)
        guard x.count == y.count, !x.isEmpty else { return false }
        return zip(x, y).reduce(UInt8(0)) { $0 | ($1.0 ^ $1.1) } == 0
    }

    private static func statusText(_ s: Int) -> String {
        [200: "OK", 202: "Accepted", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed", 503: "Service Unavailable"][s] ?? "Error"
    }

    private static func sendAll(_ fd: Int32, _ data: Data) {
        data.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            var sent = 0
            while sent < p.count {
                #if canImport(Darwin)
                let n = send(fd, p.baseAddress! + sent, p.count - sent, 0)
                #else
                let n = send(fd, p.baseAddress! + sent, p.count - sent, Int32(MSG_NOSIGNAL))
                #endif
                if n <= 0 { return }
                sent += n
            }
        }
    }

    private static func respond(_ fd: Int32, _ status: Int, _ body: String = "", extra: String = "") {
        let b = Data(body.utf8)
        var head = "HTTP/1.1 \(status) \(statusText(status))\r\n"
        if !b.isEmpty { head += "Content-Type: application/json\r\n" }
        head += "Content-Length: \(b.count)\r\nCache-Control: no-store\r\nConnection: close\r\n" + extra + "\r\n"
        sendAll(fd, Data(head.utf8) + b)
    }

    private static func setTimeout(_ fd: Int32, seconds: Int) {
        var tv = timeval(tv_sec: seconds, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        #if canImport(Darwin)
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        #endif
    }

    private static func loopback(_ port: Int) -> sockaddr_in {
        var a = sockaddr_in()
        #if canImport(Darwin)
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        #endif
        a.sin_family = sa_family_t(AF_INET)
        a.sin_port = in_port_t(UInt16(clamping: port)).bigEndian
        a.sin_addr.s_addr = in_addr_t(0x7F00_0001).bigEndian  // 127.0.0.1: never reachable from other machines
        return a
    }

    final class HTTPServer {
        private let server: Server
        private let token: String
        private var fd: Int32 = -1
        private(set) var port = 0
        private let lock = NSLock()
        private var connections = 0
        private var stopping = false
        private var acceptorDone: DispatchSemaphore?

        init(server: Server, token: String) {
            self.server = server
            self.token = token
        }

        // Port 0 picks a free one (tests).
        func start(port wanted: Int) throws {
            stop()
            guard (0...65535).contains(wanted) else { throw MCP.failed("Port \(wanted) isn't a port (use 1024 to 65535).") }
            let s = socket(AF_INET, kSockStream, 0)
            guard s >= 0 else { throw MCP.failed("Couldn't open a network socket.") }
            // Connections just closed linger in TIME_WAIT; without this, turning agents off and on can't bind for ~30 s.
            var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
            var addr = MCP.loopback(wanted)
            let bound = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
            guard bound == 0, listen(s, 16) == 0 else {
                close(s)
                throw MCP.failed("Port \(wanted) is in use by another app. Pick another in Settings › AI agents.")
            }
            // Darwin's shutdown() doesn't reliably wake accept() on a listening socket. Poll a nonblocking
            // listener so stop() can join the accept thread before the descriptor or port is reused.
            guard fcntl(s, F_SETFL, O_NONBLOCK) == 0 else {
                close(s)
                throw MCP.failed("Couldn't configure the network socket.")
            }
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            _ = withUnsafeMutablePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(s, $0, &len) } }
            fd = s
            port = Int(UInt16(bigEndian: addr.sin_port))
            stopping = false
            let done = DispatchSemaphore(value: 0)
            acceptorDone = done
            let t = Thread { [self] in
                acceptLoop(s)
                close(s)  // only once the loop is over, so its number can't be reused under it
                done.signal()
            }
            t.name = "ather.mcp.accept"
            t.start()
        }

        var running: Bool { fd >= 0 }

        func stop() {
            guard fd >= 0 else { return }
            lock.lock()
            stopping = true
            lock.unlock()
            acceptorDone?.wait()
            fd = -1
            port = 0
        }

        private func acceptLoop(_ listener: Int32) {
            while true {
                lock.lock()
                let shouldStop = stopping
                lock.unlock()
                if shouldStop { return }
                var ready = pollfd(fd: listener, events: Int16(POLLIN), revents: 0)
                guard poll(&ready, 1, 100) > 0 else { continue }
                let c = accept(listener, nil, nil)
                lock.lock()
                let stopped = stopping
                lock.unlock()
                if stopped {
                    if c >= 0 { close(c) }
                    return
                }
                if c < 0 {
                    usleep(50_000)  // a client that gave up, or out of descriptors for a moment: keep serving
                    continue
                }
                lock.lock()
                let busy = connections >= 16
                if !busy { connections += 1 }
                lock.unlock()
                if busy {
                    MCP.setTimeout(c, seconds: 1)
                    MCP.respond(c, 503, extra: "Retry-After: 1\r\n")
                    close(c)
                    continue
                }
                let p = port
                Thread { [self] in
                    serve(c, port: p)
                    shutdown(c, Int32(SHUT_WR))
                    close(c)
                    lock.lock()
                    connections -= 1
                    lock.unlock()
                }.start()
            }
        }

        private func serve(_ c: Int32, port: Int) {
            MCP.setTimeout(c, seconds: 30)
            var raw = Data()
            var buf = [UInt8](repeating: 0, count: 16384)
            var req: HTTPRequest?
            while req == nil {
                switch MCP.parseRequest(raw) {
                case .done(let r): req = r
                case .bad: return MCP.respond(c, 400)
                case .incomplete:
                    let n = recv(c, &buf, buf.count, 0)
                    if n <= 0 { return }
                    raw.append(buf, count: n)
                    if raw.count > MCP.maxRequest + 64 * 1024 { return MCP.respond(c, 400) }
                }
            }
            let r = req!
            let refuse = MCP.check(r, port: port, token: token)
            if refuse != 0 { return MCP.respond(c, refuse, extra: refuse == 401 ? "WWW-Authenticate: Bearer\r\n" : "") }
            guard r.method == "POST" else { return MCP.respond(c, 405, extra: "Allow: POST\r\n") }  // no stream, no sessions
            let out = server.handle(r.body)
            if out.isEmpty { MCP.respond(c, 202) } else { MCP.respond(c, 200, out) }
        }
    }

    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }
    static func failed(_ s: String) -> Failure { Failure(message: s) }

    // POSTs `body` to 127.0.0.1:port/mcp: the HTTP status and the response body. notListening when nobody accepted
    // the connection, 0 when it was accepted but no answer came (dropped, or timed out).
    static let notListening = -1
    static func post(port: Int, token: String, body: Data, timeout: Int) -> (Int, Data) {
        guard (1...65535).contains(port) else { return (notListening, Data()) }
        let s = socket(AF_INET, kSockStream, 0)
        guard s >= 0 else { return (notListening, Data()) }
        defer { close(s) }
        var addr = loopback(port)
        let ok = withUnsafePointer(to: &addr) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        guard ok == 0 else { return (notListening, Data()) }
        setTimeout(s, seconds: timeout)
        let head = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:\(port)\r\nAuthorization: Bearer \(token)\r\nContent-Type: application/json\r\n"
            + "Accept: application/json, text/event-stream\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n"
        sendAll(s, Data(head.utf8) + body)
        var raw = Data()
        var buf = [UInt8](repeating: 0, count: 16384)
        while true {
            let n = recv(s, &buf, buf.count, 0)
            if n <= 0 { break }
            raw.append(buf, count: n)
        }
        // "HTTP/1.1 200 OK\r\n…\r\n\r\n<body>": the server always closes after one response.
        guard raw.starts(with: Data("HTTP/".utf8)), let end = raw.range(of: Data("\r\n\r\n".utf8)) else { return (0, Data()) }
        let status = String(decoding: raw[..<end.lowerBound], as: UTF8.self).split(separator: " ").dropFirst().first.flatMap { Int($0) } ?? 0
        return (status, Data(raw[end.upperBound...]))
    }

    // MARK: stdio

    // Newline-delimited JSON-RPC on stdin/stdout, relayed to the running app. `startApp` starts the app when it
    // isn't running and says whether it did. Returns the exit code.
    static func runStdioBridge(port: Int, token: String, startApp: @escaping () -> Bool) -> Int32 {
        signal(SIGPIPE, SIG_IGN)
        let out = NSLock(), starting = NSLock()
        let group = DispatchGroup()
        func emit(_ s: String) {
            out.lock()
            defer { out.unlock() }
            var line = s
            while line.hasSuffix("\n") || line.hasSuffix("\r") { line.removeLast() }
            let d = Data((line + "\n").utf8)
            d.withUnsafeBytes { p in
                var done = 0
                while done < p.count {
                    let n = write(1, p.baseAddress! + done, p.count - done)
                    if n <= 0 { return }
                    done += n
                }
            }
        }
        func relay(_ line: Data) {
            // Only a connection nobody accepted is retried (and may start the app). A call that was accepted and then
            // dropped (the app quit or crashed mid-call) may have done its work already, so it's never sent twice.
            var (status, resp) = post(port: port, token: token, body: line, timeout: 15 * 60)
            if status == notListening {
                starting.lock()  // the first request that finds the app missing starts it; the others wait for that
                (status, resp) = post(port: port, token: token, body: line, timeout: 15 * 60)
                if status == notListening, startApp() {
                    let until = Date().addingTimeInterval(20)  // for it to start listening
                    while status == notListening, Date() < until {
                        usleep(250_000)
                        (status, resp) = post(port: port, token: token, body: line, timeout: 15 * 60)
                    }
                }
                starting.unlock()
            }
            if status == 200, !resp.isEmpty { return emit(String(decoding: resp, as: UTF8.self)) }
            if status == 202 { return }
            guard let msg = (try? JSONSerialization.jsonObject(with: line)) as? JSON, let id = msg["id"], msg["method"] != nil else { return }
            let why: String
            switch status {
            case notListening: why = "Ather Screenshot isn't accepting AI agents. Turn on \"Allow AI agents (MCP)\" in Ather Screenshot's Settings (AI agents), or run \"Allow AI agents (MCP)\" from its command palette (⌃⌥K)."
            case 0: why = "Ather Screenshot closed the connection before answering (it quit, or the call took over 15 minutes)."
            case 503: why = "Ather Screenshot is busy with other agent calls. Try again in a moment."
            case 401, 403: why = "Ather Screenshot refused the connection (the agent token didn't match). Restart Ather Screenshot and try again."
            default: why = "Ather Screenshot answered with HTTP \(status)."
            }
            emit(encode(["jsonrpc": "2.0", "id": id, "error": ["code": -32000, "message": why]]))
        }
        var pending = Data()
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = read(0, &buf, buf.count)
            if n <= 0 { break }
            pending.append(buf, count: n)
            while let nl = pending.firstIndex(of: 10) {
                var line = Data(pending[pending.startIndex..<nl])
                pending = Data(pending[pending.index(after: nl)...])
                if line.last == 13 { line.removeLast() }
                if line.allSatisfy({ $0 == 32 || $0 == 9 }) { continue }
                let request = line
                group.enter()
                Thread {  // concurrently: a long call doesn't hold up a ping
                    relay(request)
                    group.leave()
                }.start()
            }
        }
        group.wait()
        return 0
    }
}
