import XCTest
@testable import AtherScreenshot

// The MCP server AI agents use (MCP.swift) and the tools it offers (Agent.swift).
final class AgentTests: XCTestCase {
    private func object(_ s: String) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: Data(s.utf8))) as? [String: Any] ?? [:]
    }

    private func testServer() -> MCP.Server {
        let echo = MCP.Tool(name: "echo", title: "Echo", description: "Says it back.", schema: ["type": "object"], readOnly: true, mainThread: false) { a, reply in
            reply(MCP.result(["said": MCP.str(a["text"]) ?? "", "n": MCP.int(a["n"]) ?? -1]))
        }
        var late = echo
        late.name = "late"
        late.timeout = 0.05
        late.run = { _, reply in DispatchQueue.global().asyncAfter(deadline: .now() + 0.3) { reply(MCP.error("too late")) } }
        let s = MCP.Server(info: .init(name: "ather", title: "Ather", version: "1.0", instructions: "Hi."), tools: [echo, late])
        s.onMain = { DispatchQueue.global().async(execute: $0) }
        return s
    }

    func testProtocol() {
        let s = testServer()
        func handle(_ text: String) -> [String: Any] { object(s.handle(Data(text.utf8))) }
        let initialize = handle(#"{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","clientInfo":{"name":"t"}}}"#)
        let result = initialize["result"] as? [String: Any]
        XCTAssertEqual(result?["protocolVersion"] as? String, "2025-03-26")
        XCTAssertEqual(result?["instructions"] as? String, "Hi.")
        XCTAssertNotNil((result?["capabilities"] as? [String: Any])?["tools"])
        let unknown = handle(#"{"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"1999-01-01"}}"#)
        XCTAssertEqual((unknown["result"] as? [String: Any])?["protocolVersion"] as? String, MCP.protocolVersions[0])
        XCTAssertEqual(unknown["id"] as? String, "a")
        XCTAssertTrue(s.handle(Data(#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#.utf8)).isEmpty)
        let call = handle(#"{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hé","n":2.6}}}"#)
        let structured = (call["result"] as? [String: Any])?["structuredContent"] as? [String: Any]
        XCTAssertEqual(structured?["said"] as? String, "hé")
        XCTAssertEqual(structured?["n"] as? Int, 3)
        let late = handle(#"{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"late"}}"#)
        XCTAssertEqual((late["result"] as? [String: Any])?["isError"] as? Bool, true)
        XCTAssertEqual((handle(#"{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"nope"}}"#)["error"] as? [String: Any])?["code"] as? Int, -32602)
        XCTAssertEqual((handle(#"{"jsonrpc":"2.0","id":6,"method":"resources/list"}"#)["error"] as? [String: Any])?["code"] as? Int, -32601)
        XCTAssertEqual((handle("{nope")["error"] as? [String: Any])?["code"] as? Int, -32700)
    }

    func testHttpRequestsAreChecked() {
        let head = "POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1:4000\r\nAuthorization: Bearer secret\r\nContent-Length: 2\r\n\r\n"
        guard case .incomplete = MCP.parseRequest(Data(head.utf8)) else { return XCTFail("the body hasn't arrived") }
        guard case .done(let r) = MCP.parseRequest(Data((head + "{}").utf8)) else { return XCTFail("parse") }
        XCTAssertEqual(r.path, "/mcp")
        XCTAssertEqual(r.body, Data("{}".utf8))
        XCTAssertEqual(MCP.check(r, port: 4000, token: "secret"), 0)
        XCTAssertEqual(MCP.check(r, port: 4000, token: "other"), 401)
        XCTAssertEqual(MCP.check(r, port: 4001, token: "secret"), 403)  // DNS rebinding names another host
        var web = r
        web.headers.append(("origin", "https://evil.example"))
        XCTAssertEqual(MCP.check(web, port: 4000, token: "secret"), 403)
        var elsewhere = r
        elsewhere.path = "/.well-known/oauth-authorization-server"
        XCTAssertEqual(MCP.check(elsewhere, port: 4000, token: "secret"), 404)
        XCTAssertEqual(MCP.check(r, port: 4000, token: ""), 401)
        guard case .done(let chunked) = MCP.parseRequest(Data("POST /mcp HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n".utf8)) else {
            return XCTFail("chunked")
        }
        XCTAssertEqual(chunked.body, Data("{}".utf8))
    }

    // Numbers and requests from a client can't crash the app.
    func testHostileInputIsSafe() {
        XCTAssertEqual(MCP.int(1e19), 1_000_000_000_000_000)  // clamped instead of trapping
        XCTAssertEqual(MCP.int(-1e300), -1_000_000_000_000_000)
        XCTAssertNil(MCP.int(Double.infinity))
        XCTAssertNil(MCP.int("12"))
        XCTAssertTrue(MCP.isBool(true))
        XCTAssertFalse(MCP.isBool(1))
        XCTAssertTrue(MCP.isNumber(1))
        XCTAssertFalse(MCP.isNumber(true))
        let huge = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:4000\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nX\r\n7fffffffffffffff\r\n"
        guard case .bad = MCP.parseRequest(Data(huge.utf8)) else { return XCTFail("a giant chunk must be refused") }
        let parsed = try? JSONSerialization.jsonObject(with: Data(#"{"a": true, "b": 1}"#.utf8)) as? [String: Any]
        XCTAssertTrue(MCP.isBool(parsed?["a"]))
        XCTAssertFalse(MCP.isBool(parsed?["b"]))
    }

    func testSignedRequests() {
        let t: UInt64 = 5_000_000
        let n = MCP.issueNonce(now: t)
        XCTAssertFalse(MCP.useNonce("5000000.0123456789abcdef0123456789abcdef.00", now: t))  // not one of ours
        XCTAssertFalse(MCP.useNonce(n + "0", now: t))    // altered
        XCTAssertFalse(MCP.useNonce(n, now: t + 61_000))  // too old
        XCTAssertTrue(MCP.useNonce(n, now: t + 1000))
        XCTAssertFalse(MCP.useNonce(n, now: t + 2000))   // only once
        XCTAssertNotEqual(MCP.sign("tok", "request", n, Data("{}".utf8)), MCP.sign("tok", "reply", n, Data("{}".utf8)))
        let fresh = MCP.issueNonce()
        var r = MCP.HTTPRequest(method: "POST", path: "/mcp", body: Data("{}".utf8))
        r.headers = [("host", "127.0.0.1:4000"), ("x-ather-nonce", fresh), ("x-ather-signature", MCP.sign("tok", "request", fresh, Data("{}".utf8)))]
        var tampered = r
        tampered.body = Data("[]".utf8)
        XCTAssertEqual(MCP.check(tampered, port: 4000, token: "tok"), 401)
        XCTAssertEqual(MCP.check(r, port: 4000, token: "other"), 401)
        XCTAssertEqual(MCP.check(r, port: 4000, token: "tok"), 0)
        XCTAssertEqual(MCP.check(r, port: 4000, token: "tok"), 401)  // replayed
        var ask = MCP.HTTPRequest(method: "GET", path: "/mcp/nonce")
        ask.headers = [("host", "127.0.0.1:4000")]
        XCTAssertEqual(MCP.check(ask, port: 4000, token: "tok"), 0)
        ask.headers.append(("origin", "https://evil.example"))
        XCTAssertEqual(MCP.check(ask, port: 4000, token: "tok"), 403)  // web pages can't even ask
    }

    // Another program on the port (on a shared Mac, another user's) answers like the app. It must never see the
    // token, and its answers must never reach the agent.
    func testBridgeNeverGivesAnImpostorTheToken() throws {
        let l = socket(AF_INET, SOCK_STREAM, 0)
        defer { close(l) }
        var a = sockaddr_in()
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        a.sin_family = sa_family_t(AF_INET)
        a.sin_addr.s_addr = in_addr_t(0x7F00_0001).bigEndian
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let bound = withUnsafeMutablePointer(to: &a) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(l, $0, len) == 0 && listen(l, 4) == 0 && getsockname(l, $0, &len) == 0 }
        }
        XCTAssertTrue(bound)
        let body = #"{"jsonrpc":"2.0","id":1,"method":"ping"}"#
        var heard = ""
        let done = DispatchSemaphore(value: 0)
        Thread {
            for answer in [#"{"nonce":"0123456789abcdef"}"#, #"{"jsonrpc":"2.0","id":1,"result":{}}"#] {
                var ready = pollfd(fd: l, events: Int16(POLLIN), revents: 0)
                guard poll(&ready, 1, 5000) > 0 else { break }  // the bridge stopped early: don't hang the test
                let c = accept(l, nil, nil)
                guard c >= 0 else { break }
                var got = ""
                var buf = [UInt8](repeating: 0, count: 4096)
                while !got.contains("\r\n\r\n") || (got.hasPrefix("POST") && !got.contains(body)) {
                    let n = recv(c, &buf, buf.count, 0)
                    if n <= 0 { break }
                    got += String(decoding: buf[0..<n], as: UTF8.self)
                }
                heard += got
                let reply = "HTTP/1.1 200 OK\r\nContent-Length: \(answer.utf8.count)\r\nConnection: close\r\n\r\n" + answer
                _ = reply.withCString { send(c, $0, strlen($0), 0) }
                shutdown(c, SHUT_WR)
                close(c)
            }
            done.signal()
        }.start()
        let (status, resp) = MCP.post(port: Int(UInt16(bigEndian: a.sin_port)), token: "s3cret-agent-token", body: Data(body.utf8), timeout: 5)
        XCTAssertEqual(done.wait(timeout: .now() + 10), .success)
        XCTAssertEqual(status, MCP.impostor)
        XCTAssertTrue(resp.isEmpty)
        XCTAssertTrue(heard.contains("POST /mcp"))  // it was asked…
        XCTAssertFalse(heard.contains("s3cret-agent-token"))  // …without the token
    }

    func testHttpRoundTrip() throws {
        let http = MCP.HTTPServer(server: testServer(), token: "tok")
        try http.start(port: 0)
        let port = http.port
        XCTAssertGreaterThan(port, 0)
        let (status, body) = MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}}"#.utf8), timeout: 5)
        XCTAssertEqual(status, 200)
        XCTAssertEqual(((object(String(decoding: body, as: UTF8.self))["result"] as? [String: Any])?["structuredContent"] as? [String: Any])?["said"] as? String, "hi")
        XCTAssertEqual(MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","method":"notifications/initialized"}"#.utf8), timeout: 5).0, 202)
        XCTAssertEqual(MCP.post(port: port, token: "bad", body: Data(#"{"jsonrpc":"2.0","id":1,"method":"ping"}"#.utf8), timeout: 5).0, 401)
        http.stop()
        XCTAssertEqual(MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","id":1,"method":"ping"}"#.utf8), timeout: 1).0, MCP.notListening)
        for _ in 0..<5 {
            try http.start(port: port)  // straight back on the same port (TIME_WAIT connections don't block it)
            XCTAssertEqual(MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","id":2,"method":"ping"}"#.utf8), timeout: 5).0, 200)
            http.stop()
            XCTAssertFalse(http.running)
            XCTAssertEqual(MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","id":3,"method":"ping"}"#.utf8), timeout: 1).0, MCP.notListening)
        }
    }

    // The same tools as the Windows app, then the Mac-only meeting tools, each with a valid object schema.
    func testToolsMatchWindows() {
        let tools = Agent.tools()
        let meetings = ["start_meeting_recording", "stop_meeting_recording", "add_meeting_marker", "list_meetings", "get_meeting",
                        "edit_meeting", "export_meeting", "transcribe_meeting", "import_audio"]
        XCTAssertEqual(Array(tools.map(\.name).suffix(meetings.count)), meetings)
        XCTAssertEqual(Array(tools.map(\.name).dropLast(meetings.count)), ["get_status", "list_windows", "take_screenshot", "start_recording", "stop_recording", "pause_recording",
                                           "add_marker", "list_captures", "get_capture", "get_video_frames", "read_text", "tag_capture",
                                           "configure", "open_in_editor", "upload_capture", "list_quick_prompts", "run_command"])
        for t in tools {
            XCTAssertTrue(JSONSerialization.isValidJSONObject(t.schema), t.name)
            XCTAssertEqual(t.schema["type"] as? String, "object", t.name)
            XCTAssertNotNil(t.schema["properties"] as? [String: Any], t.name)
        }
        let run = tools.first { $0.name == "run_command" }!
        let names = ((run.schema["properties"] as? [String: Any])?["command"] as? [String: Any])?["enum"] as? [String] ?? []
        XCTAssertTrue(names.contains("CaptureRegion"))
        XCTAssertFalse(names.contains("Quit"))
        XCTAssertFalse(names.contains("ToggleAiAgents"))
    }

    func testTargets() throws {
        XCTAssertEqual(try Agent.resolve([:]).kind, "screen")
        XCTAssertThrowsError(try Agent.resolve(["target": "display", "display": 99]))
        XCTAssertThrowsError(try Agent.resolve(["target": "region", "x": 0]))
        XCTAssertThrowsError(try Agent.resolve(["target": "moon"]))
        XCTAssertThrowsError(try Agent.resolve(["target": "window", "window_title": "no window is called this 1f3c"]))
    }
}
