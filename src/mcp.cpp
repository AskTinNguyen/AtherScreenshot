#include "mcp.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "selftest.h"

#pragma comment(lib, "ws2_32")

namespace ather::mcp {

// ---- results ----

std::string Base64(const BYTE* data, size_t n) {
    static const char kAbc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out += kAbc[v >> 18 & 63];
        out += kAbc[v >> 12 & 63];
        out += i + 1 < n ? kAbc[v >> 6 & 63] : '=';
        out += i + 2 < n ? kAbc[v & 63] : '=';
    }
    return out;
}

Json TextContent(const std::string& text) {
    Json j = Json::Object();
    j.Set("type", "text");
    j.Set("text", text);
    return j;
}

Json ImageContent(const std::vector<BYTE>& png) {
    Json j = Json::Object();
    j.Set("type", "image");
    j.Set("data", Base64(png.data(), png.size()));
    j.Set("mimeType", "image/png");
    return j;
}

Json Result(Json content, Json structured) {
    Json j = Json::Object();
    j.Set("content", std::move(content));
    if (structured.IsObject()) j.Set("structuredContent", std::move(structured));
    return j;
}

Json JsonResult(const Json& data, Json extra) {
    Json content = Json::Array();
    content.Push(TextContent(data.Dump()));
    for (const auto& e : extra.Items()) content.Push(e);
    return Result(std::move(content), data.IsObject() ? data : Json());
}

Json ErrorResult(const std::string& message) {
    Json content = Json::Array();
    content.Push(TextContent(message));
    Json j = Result(std::move(content));
    j.Set("isError", true);
    return j;
}

// ---- protocol ----

namespace {

const char* const kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};  // newest first

Json RpcResponse(const Json& id, Json result) {
    Json j = Json::Object();
    j.Set("jsonrpc", "2.0");
    j.Set("id", id);
    j.Set("result", std::move(result));
    return j;
}

Json RpcError(const Json& id, int code, const std::string& message) {
    Json e = Json::Object();
    e.Set("code", code);
    e.Set("message", message);
    Json j = Json::Object();
    j.Set("jsonrpc", "2.0");
    j.Set("id", id);
    j.Set("error", std::move(e));
    return j;
}

// What one call's reply lands in. Shared, because a tool may reply after the call has given up waiting.
struct Pending {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    Json result;
};

}  // namespace

Json Server::Call(const Json& params, const std::function<void(std::function<void()>)>& onUi) const {
    const std::string name = params["name"].Str();
    auto it = std::find_if(tools_.begin(), tools_.end(), [&](const Tool& t) { return t.name == name; });
    if (it == tools_.end()) return Json();  // the caller answers with "unknown tool"
    const Tool& tool = *it;
    const Json args = params["arguments"].IsObject() ? params["arguments"] : Json::Object();
    auto p = std::make_shared<Pending>();
    Reply reply = [p](Json r) {
        std::lock_guard<std::mutex> lock(p->m);
        if (p->done) return;
        p->done = true;
        p->result = std::move(r);
        p->cv.notify_all();
    };
    auto task = [run = tool.run, args, reply] {
        try {
            run(args, reply);
        } catch (...) {
            reply(ErrorResult("The tool failed unexpectedly."));
        }
    };
    if (tool.uiThread) onUi(task);
    else task();
    std::unique_lock<std::mutex> lock(p->m);
    if (!p->cv.wait_for(lock, std::chrono::milliseconds(tool.timeoutMs), [&] { return p->done; })) {
        p->done = true;  // a late reply is dropped
        return ErrorResult("Ather Screenshot didn't finish “" + name + "” in time.");
    }
    return p->result;
}

std::string Server::Handle(const std::string& body, const std::function<void(std::function<void()>)>& onUi) const {
    bool ok = false;
    const Json msg = Json::Parse(body, &ok);
    if (!ok) return RpcError(Json(), -32700, "Parse error").Dump();
    if (msg.IsArray()) {  // a batch (2025-03-26): answer each request in it
        Json out = Json::Array();
        for (const auto& m : msg.Items()) {
            std::string r = Handle(m.Dump(), onUi);
            if (!r.empty()) out.Push(Json::Parse(r));
        }
        return out.size() ? out.Dump() : std::string();
    }
    if (!msg.IsObject()) return RpcError(Json(), -32600, "Invalid request").Dump();
    if (!msg.Has("method")) return "";  // a response to us (we never ask, so nothing to do)
    const std::string method = msg["method"].Str();
    if (!msg.Has("id")) return "";  // notifications: initialized, cancelled… need no answer
    const Json& id = msg["id"];
    const Json& params = msg["params"];

    if (method == "initialize") {
        std::string version = kProtocolVersions[0];
        for (const char* v : kProtocolVersions)
            if (params["protocolVersion"].Str() == v) version = v;
        if (onInitialize) onInitialize(params["clientInfo"]["name"].Str());
        Json tools = Json::Object();
        tools.Set("listChanged", false);
        Json caps = Json::Object();
        caps.Set("tools", std::move(tools));
        Json server = Json::Object();
        server.Set("name", info_.name);
        server.Set("title", info_.title);
        server.Set("version", info_.version);
        Json r = Json::Object();
        r.Set("protocolVersion", version);
        r.Set("capabilities", std::move(caps));
        r.Set("serverInfo", std::move(server));
        if (!info_.instructions.empty()) r.Set("instructions", info_.instructions);
        return RpcResponse(id, std::move(r)).Dump();
    }
    if (method == "ping") return RpcResponse(id, Json::Object()).Dump();
    if (method == "tools/list") {
        Json list = Json::Array();
        for (const auto& t : tools_) {
            Json ann = Json::Object();
            ann.Set("title", t.title);
            ann.Set("readOnlyHint", t.readOnly);
            ann.Set("destructiveHint", false);
            ann.Set("openWorldHint", false);
            Json j = Json::Object();
            j.Set("name", t.name);
            j.Set("title", t.title);
            j.Set("description", t.description);
            j.Set("inputSchema", t.inputSchema);
            j.Set("annotations", std::move(ann));
            list.Push(std::move(j));
        }
        Json r = Json::Object();
        r.Set("tools", std::move(list));
        return RpcResponse(id, std::move(r)).Dump();
    }
    if (method == "tools/call") {
        Json r = Call(params, onUi);
        if (r.IsNull()) return RpcError(id, -32602, "Unknown tool: " + params["name"].Str()).Dump();
        return RpcResponse(id, std::move(r)).Dump();
    }
    return RpcError(id, -32601, "Method not found: " + method).Dump();
}

// ---- HTTP ----

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool SameSecret(const std::string& a, const std::string& b) {  // constant time for equal lengths
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

constexpr size_t kMaxRequest = 8 << 20;

}  // namespace

std::string HttpRequest::Header(const char* name) const {
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return {};
}

bool ParseHttpRequest(const std::string& raw, HttpRequest* out, bool* bad) {
    *bad = false;
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos) {
        *bad = raw.size() > 64 * 1024;  // a head this long isn't a real client
        return false;
    }
    HttpRequest r;
    size_t pos = raw.find("\r\n");
    const std::string line = raw.substr(0, pos);
    const size_t s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos || line.compare(s2 + 1, 5, "HTTP/") != 0) return *bad = true, false;
    r.method = line.substr(0, s1);
    r.path = line.substr(s1 + 1, s2 - s1 - 1);
    r.path = r.path.substr(0, r.path.find('?'));
    while (pos < end) {
        const size_t next = raw.find("\r\n", pos + 2);
        const std::string h = raw.substr(pos + 2, next - pos - 2);
        pos = next;
        const size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        r.headers.emplace_back(Lower(Trim(h.substr(0, colon))), Trim(h.substr(colon + 1)));
    }
    const std::string rest = raw.substr(end + 4);
    if (Lower(r.Header("transfer-encoding")).find("chunked") != std::string::npos) {
        size_t p = 0;
        for (;;) {
            const size_t eol = rest.find("\r\n", p);
            if (eol == std::string::npos) return false;
            const size_t n = strtoul(rest.substr(p, eol - p).c_str(), nullptr, 16);
            if (n > kMaxRequest || r.body.size() + n > kMaxRequest) return *bad = true, false;
            if (n == 0) break;
            if (rest.size() < eol + 2 + n + 2) return false;
            r.body.append(rest, eol + 2, n);
            p = eol + 2 + n + 2;
        }
    } else {
        const std::string cl = r.Header("content-length");
        const size_t n = cl.empty() ? 0 : strtoull(cl.c_str(), nullptr, 10);
        if (n > kMaxRequest) return *bad = true, false;
        if (rest.size() < n) return false;
        r.body = rest.substr(0, n);
    }
    *out = std::move(r);
    return true;
}

int CheckHttpRequest(const HttpRequest& r, int port, const std::string& token) {
    // Only this machine's own clients: a web page could reach 127.0.0.1 through DNS rebinding (wrong Host) or
    // a plain cross-site request (its Origin), so both have to name this server.
    const std::string p = std::to_string(port);
    const std::string host = Lower(r.Header("host"));
    if (host != "127.0.0.1:" + p && host != "localhost:" + p && host != "127.0.0.1" && host != "localhost") return 403;
    const std::string origin = Lower(r.Header("origin"));
    if (!origin.empty() && origin != "http://127.0.0.1:" + p && origin != "http://localhost:" + p) return 403;
    if (r.path != "/mcp" && r.path != "/mcp/") return 404;
    const std::string auth = r.Header("authorization");
    if (auth.size() < 7 || Lower(auth.substr(0, 7)) != "bearer " || !SameSecret(Trim(auth.substr(7)), token)) return 401;
    return 0;
}

namespace {

struct HttpState {
    std::mutex m;
    SOCKET listener = INVALID_SOCKET;
    std::thread acceptor;
    int port = 0;
    std::atomic<int> connections{0};
    ~HttpState() {  // quitting without StopHttp: don't take the process down with a joinable thread
        if (acceptor.joinable()) acceptor.detach();
    }
};
HttpState g_http;

bool EnsureWinsock() {
    static const bool ok = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

const char* StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
    }
    return "Error";
}

void SendAll(SOCKET s, const std::string& data) {
    for (size_t sent = 0; sent < data.size();) {
        const int n = send(s, data.data() + sent, (int)std::min<size_t>(data.size() - sent, 1 << 20), 0);
        if (n <= 0) return;
        sent += n;
    }
}

void Respond(SOCKET s, int status, const std::string& body = {}, const char* extraHeaders = "") {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
    if (!body.empty()) head += "Content-Type: application/json\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n";
    head += extraHeaders;
    head += "\r\n";
    SendAll(s, head + body);
}

void ServeConnection(SOCKET s, std::shared_ptr<Server> server, int port, std::string token) {
    DWORD timeout = 30000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    std::string raw;
    HttpRequest req;
    bool bad = false;
    char buf[16384];
    while (!ParseHttpRequest(raw, &req, &bad) && !bad) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        raw.append(buf, n);
        if (raw.size() > kMaxRequest + 64 * 1024) bad = true;
    }
    if (bad) return Respond(s, 400);
    if (const int refuse = CheckHttpRequest(req, port, token))
        return Respond(s, refuse, {}, refuse == 401 ? "WWW-Authenticate: Bearer\r\n" : "");
    if (req.method != "POST") return Respond(s, 405, {}, "Allow: POST\r\n");  // no server-to-client stream, no sessions
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::string out = server->Handle(req.body, [](std::function<void()> fn) { RunOnUi(std::move(fn)); });
    if (SUCCEEDED(co)) CoUninitialize();
    if (out.empty()) Respond(s, 202);
    else Respond(s, 200, out);
}

}  // namespace

bool StartHttp(std::shared_ptr<Server> server, int port, const std::string& token, std::wstring* error) {
    StopHttp();
    if (!EnsureWinsock()) return *error = L"Windows networking isn't available.", false;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) return *error = L"Couldn't open a network socket.", false;
    BOOL exclusive = TRUE;
    setsockopt(l, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never reachable from other machines
    if (bind(l, (sockaddr*)&a, sizeof(a)) != 0 || listen(l, 16) != 0) {
        closesocket(l);
        return *error = L"Port " + std::to_wstring(port) + L" is in use by another app. Pick another in Settings › AI agents.", false;
    }
    int len = sizeof(a);
    getsockname(l, (sockaddr*)&a, &len);
    std::lock_guard<std::mutex> lock(g_http.m);
    g_http.listener = l;
    g_http.port = ntohs(a.sin_port);
    g_http.acceptor = std::thread([l, server, token, p = g_http.port] {
        for (;;) {
            SOCKET c = accept(l, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;  // closed by StopHttp
            if (g_http.connections >= 16) {
                closesocket(c);
                continue;
            }
            ++g_http.connections;
            std::thread([c, server, token, p] {
                ServeConnection(c, server, p, token);
                shutdown(c, SD_SEND);
                closesocket(c);
                --g_http.connections;
            }).detach();
        }
    });
    return true;
}

void StopHttp() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_http.m);
        if (g_http.listener == INVALID_SOCKET) return;
        closesocket(g_http.listener);
        g_http.listener = INVALID_SOCKET;
        g_http.port = 0;
        t = std::move(g_http.acceptor);
    }
    if (t.joinable()) t.join();
}

bool HttpRunning() {
    std::lock_guard<std::mutex> lock(g_http.m);
    return g_http.listener != INVALID_SOCKET;
}

int HttpPort() {
    std::lock_guard<std::mutex> lock(g_http.m);
    return g_http.port;
}

int HttpPost(int port, const std::string& token, const std::string& body, std::string* response, int timeoutMs) {
    response->clear();
    if (!EnsureWinsock()) return 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return 0;
    }
    DWORD timeout = (DWORD)timeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    const std::string p = std::to_string(port);
    SendAll(s, "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + p + "\r\nAuthorization: Bearer " + token +
                   "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    std::string raw;
    char buf[16384];
    for (int n; (n = recv(s, buf, sizeof(buf), 0)) > 0;) raw.append(buf, n);
    closesocket(s);
    // "HTTP/1.1 200 OK\r\n…\r\n\r\n<body>": the server always closes after one response.
    if (raw.compare(0, 5, "HTTP/") != 0) return 0;
    const size_t sp = raw.find(' '), end = raw.find("\r\n\r\n");
    if (sp == std::string::npos || end == std::string::npos) return 0;
    *response = raw.substr(end + 4);
    return atoi(raw.c_str() + sp + 1);
}

// ---- stdio ----

namespace {

void WriteLine(HANDLE out, std::mutex& m, const std::string& text) {
    std::lock_guard<std::mutex> lock(m);
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    line += '\n';
    DWORD wr = 0;
    WriteFile(out, line.data(), (DWORD)line.size(), &wr, nullptr);
    FlushFileBuffers(out);
}

}  // namespace

int RunStdioBridge(int port, const std::string& token, const std::function<bool()>& startApp) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE) return 2;
    std::mutex outLock;
    std::mutex startLock;  // the first request that finds the app missing starts it; the others wait for that
    std::vector<std::thread> calls;
    auto relay = [&](std::string line) {
        std::string resp;
        int status = HttpPost(port, token, line, &resp, 15 * 60 * 1000);
        if (status == 0) {
            std::lock_guard<std::mutex> lock(startLock);
            status = HttpPost(port, token, line, &resp, 15 * 60 * 1000);
            if (status == 0 && startApp()) {
                for (int i = 0; i < 80 && status == 0; ++i) {  // up to 20 s for it to start listening
                    Sleep(250);
                    status = HttpPost(port, token, line, &resp, 15 * 60 * 1000);
                }
            }
        }
        if (status == 200 && !resp.empty()) return WriteLine(out, outLock, resp);
        if (status == 202) return;
        bool ok = false;
        const Json msg = Json::Parse(line, &ok);
        if (!ok || !msg.IsObject() || !msg.Has("id") || !msg.Has("method")) return;  // nothing to answer
        std::string why;
        if (status == 0)
            why = "Ather Screenshot isn't accepting AI agents. Turn on \"Allow AI agents (MCP)\" in Ather Screenshot's "
                  "Settings (AI agents), or run \"Allow AI agents (MCP)\" from its command palette (Ctrl+Alt+K).";
        else if (status == 401 || status == 403)
            why = "Ather Screenshot refused the connection (the agent token didn't match). Restart Ather Screenshot and try again.";
        else
            why = "Ather Screenshot answered with HTTP " + std::to_string(status) + ".";
        Json e = Json::Object();
        e.Set("code", -32000);
        e.Set("message", why);
        Json r = Json::Object();
        r.Set("jsonrpc", "2.0");
        r.Set("id", msg["id"]);
        r.Set("error", std::move(e));
        WriteLine(out, outLock, r.Dump());
    };
    std::string pending;
    char buf[65536];
    for (DWORD n = 0; ReadFile(in, buf, sizeof(buf), &n, nullptr) && n > 0;) {
        pending.append(buf, n);
        for (size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            calls.emplace_back(relay, std::move(line));  // concurrently: a long call doesn't hold up a ping
        }
    }
    for (auto& t : calls) t.join();
    return 0;
}

// ---- tests ----

namespace {

std::shared_ptr<Server> TestServer() {
    Tool echo;
    echo.name = "echo";
    echo.title = "Echo";
    echo.description = "Says it back.";
    echo.inputSchema = Json::Object();
    echo.inputSchema.Set("type", "object");
    echo.readOnly = true;
    echo.uiThread = false;
    echo.run = [](const Json& args, Reply reply) {
        Json d = Json::Object();
        d.Set("said", args["text"].Str());
        reply(JsonResult(d));
    };
    Tool late = echo;
    late.name = "late";
    late.timeoutMs = 50;
    late.run = [](const Json&, Reply reply) {
        std::thread([reply] {
            Sleep(300);
            reply(ErrorResult("too late"));  // after the call gave up: dropped
        }).detach();
    };
    Tool boom = echo;
    boom.name = "boom";
    boom.run = [](const Json&, Reply) { throw 1; };
    return std::make_shared<Server>(ServerInfo{"ather", "Ather", "1.0", "Hi."}, std::vector<Tool>{echo, late, boom});
}

const auto kInline = [](std::function<void()> fn) { fn(); };

}  // namespace

ATHER_TEST(mcp_protocol) {
    auto s = TestServer();
    Json init = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})", kInline));
    CHECK_EQ(init["id"].Int(), 1);
    CHECK_EQ(init["result"]["protocolVersion"].Str(), std::string("2025-03-26"));
    CHECK(init["result"]["capabilities"]["tools"].IsObject());
    CHECK_EQ(init["result"]["serverInfo"]["name"].Str(), std::string("ather"));
    CHECK_EQ(init["result"]["instructions"].Str(), std::string("Hi."));
    // An unknown version gets ours.
    Json v = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"1999-01-01"}})", kInline));
    CHECK_EQ(v["id"].Str(), std::string("a"));
    CHECK_EQ(v["result"]["protocolVersion"].Str(), std::string(kProtocolVersions[0]));
    // Notifications and responses: nothing to send back.
    CHECK(s->Handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", kInline).empty());
    CHECK(s->Handle(R"({"jsonrpc":"2.0","id":5,"result":{}})", kInline).empty());
    Json list = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", kInline));
    CHECK_EQ(list["result"]["tools"].size(), 3u);
    CHECK_EQ(list["result"]["tools"][0]["name"].Str(), std::string("echo"));
    CHECK(list["result"]["tools"][0]["annotations"]["readOnlyHint"].Bool());
    Json call = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hé"}}})", kInline));
    CHECK_EQ(call["result"]["structuredContent"]["said"].Str(), std::string("h\xc3\xa9"));
    CHECK_EQ(call["result"]["content"][0]["type"].Str(), std::string("text"));
    CHECK(!call["result"].Has("isError"));
    Json late = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"late"}})", kInline));
    CHECK(late["result"]["isError"].Bool());
    Json boom = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"boom"}})", kInline));
    CHECK(boom["result"]["isError"].Bool());
    CHECK_EQ(Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nope"}})", kInline))["error"]["code"].Int(), -32602);
    CHECK_EQ(Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":8,"method":"resources/list"})", kInline))["error"]["code"].Int(), -32601);
    CHECK_EQ(Json::Parse(s->Handle("{nope", kInline))["error"]["code"].Int(), -32700);
    Json ping = Json::Parse(s->Handle(R"({"jsonrpc":"2.0","id":9,"method":"ping"})", kInline));
    CHECK(ping["result"].IsObject());
    Json batch = Json::Parse(s->Handle(R"([{"jsonrpc":"2.0","id":10,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"}])", kInline));
    CHECK(batch.IsArray() && batch.size() == 1u && batch[0]["id"].Int() == 10);
    Sleep(400);  // let `late` reply into its abandoned slot
}

ATHER_TEST(mcp_base64) {
    auto b = [](const char* s) { return Base64((const BYTE*)s, strlen(s)); };
    CHECK_EQ(b(""), std::string(""));
    CHECK_EQ(b("f"), std::string("Zg=="));
    CHECK_EQ(b("fo"), std::string("Zm8="));
    CHECK_EQ(b("foo"), std::string("Zm9v"));
    CHECK_EQ(b("foobar"), std::string("Zm9vYmFy"));
}

ATHER_TEST(mcp_http_requests_are_checked) {
    HttpRequest r;
    bool bad = false;
    const std::string head = "POST /mcp?x=1 HTTP/1.1\r\nHost: 127.0.0.1:4000\r\nAuthorization: Bearer secret\r\nContent-Length: 2\r\n\r\n";
    CHECK(!ParseHttpRequest(head, &r, &bad) && !bad);  // the body hasn't arrived
    CHECK(ParseHttpRequest(head + "{}", &r, &bad));
    CHECK_EQ(r.method, std::string("POST"));
    CHECK_EQ(r.path, std::string("/mcp"));
    CHECK_EQ(r.body, std::string("{}"));
    CHECK_EQ(CheckHttpRequest(r, 4000, "secret"), 0);
    CHECK_EQ(CheckHttpRequest(r, 4000, "other"), 401);
    CHECK_EQ(CheckHttpRequest(r, 4001, "secret"), 403);  // DNS rebinding names another host
    HttpRequest web = r;
    web.headers.emplace_back("origin", "https://evil.example");
    CHECK_EQ(CheckHttpRequest(web, 4000, "secret"), 403);
    HttpRequest local = r;
    local.headers.emplace_back("origin", "http://localhost:4000");
    CHECK_EQ(CheckHttpRequest(local, 4000, "secret"), 0);
    HttpRequest elsewhere = r;
    elsewhere.path = "/.well-known/oauth-authorization-server";
    CHECK_EQ(CheckHttpRequest(elsewhere, 4000, "secret"), 404);
    CHECK_EQ(CheckHttpRequest(r, 4000, ""), 401);  // no token configured: nobody gets in
    CHECK(ParseHttpRequest("POST /mcp HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n", &r, &bad));
    CHECK_EQ(r.body, std::string("{}"));
    CHECK(!ParseHttpRequest("garbage\r\n\r\n", &r, &bad) && bad);
}

ATHER_TEST(mcp_http_round_trip) {
    std::wstring err;
    CHECK(StartHttp(TestServer(), 0, "tok", &err));
    const int port = HttpPort();
    CHECK(port > 0 && HttpRunning());
    std::string resp;
    CHECK_EQ(HttpPost(port, "tok", R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"echo","arguments":{"text":"hi"}}})", &resp, 5000), 200);
    CHECK_EQ(Json::Parse(resp)["result"]["structuredContent"]["said"].Str(), std::string("hi"));
    CHECK_EQ(HttpPost(port, "tok", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", &resp, 5000), 202);
    CHECK_EQ(HttpPost(port, "wrong", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", &resp, 5000), 401);
    StopHttp();
    CHECK(!HttpRunning());
    CHECK_EQ(HttpPost(port, "tok", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", &resp, 1000), 0);
}

}  // namespace ather::mcp
