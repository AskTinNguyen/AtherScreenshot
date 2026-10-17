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
        Thread.sleep(forTimeInterval: 0.2)
        XCTAssertEqual(MCP.post(port: port, token: "tok", body: Data(#"{"jsonrpc":"2.0","id":1,"method":"ping"}"#.utf8), timeout: 1).0, 0)
    }

    // The same tools as the Windows app, each with a valid object schema.
    func testToolsMatchWindows() {
        let tools = Agent.tools()
        XCTAssertEqual(tools.map(\.name), ["get_status", "list_windows", "take_screenshot", "start_recording", "stop_recording", "pause_recording",
                                           "add_marker", "list_captures", "get_capture", "get_video_frames", "read_text", "tag_capture",
                                           "configure", "open_in_editor", "upload_capture", "run_command"])
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
