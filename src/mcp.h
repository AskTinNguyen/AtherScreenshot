#pragma once
#include "common.h"
#include "json.h"

namespace ather::mcp {

// Model Context Protocol (https://modelcontextprotocol.io) for AI agents such as Claude Code or Codex.
// The running app serves it over Streamable HTTP on 127.0.0.1 (plain JSON responses, no SSE, no sessions);
// `AtherScreenshot.exe --mcp` is a stdio server for agents that start one, relaying to the running app.

// A tool's result is an MCP CallToolResult: {"content": [...], "structuredContent"?: {...}, "isError"?: true}.
using Reply = std::function<void(Json result)>;

struct Tool {
    std::string name, title, description;
    Json inputSchema;       // a JSON Schema object
    bool readOnly = false;  // annotations.readOnlyHint
    bool uiThread = true;   // run on the UI thread; false: on the request's own thread (COM initialized, MTA)
    int timeoutMs = 60000;  // the call answers with an error if `run` hasn't replied by then
    // Replies exactly once, now or later, from any thread.
    std::function<void(const Json& args, Reply reply)> run;
};

// Result helpers.
Json TextContent(const std::string& text);
Json ImageContent(const std::vector<BYTE>& png);  // base64 PNG
Json Result(Json content, Json structured = Json());  // content: an array of content items
Json JsonResult(const Json& data, Json extra = Json::Array());  // the data as text and as structuredContent, then `extra`
Json ErrorResult(const std::string& message);
std::string Base64(const BYTE* data, size_t n);

struct ServerInfo {
    std::string name, title, version, instructions;
};

// The protocol, independent of the transport: one JSON-RPC message in, its response out.
class Server {
public:
    Server(ServerInfo info, std::vector<Tool> tools) : info_(std::move(info)), tools_(std::move(tools)) {}
    // Returns the response text, or "" for notifications and responses (HTTP 202). Blocks while a tool runs.
    // `onUi` runs work on the UI thread.
    std::string Handle(const std::string& body, const std::function<void(std::function<void()>)>& onUi) const;
    const std::vector<Tool>& Tools() const { return tools_; }
    std::function<void(const std::string& client)> onInitialize;  // a client connected (called on the request's thread)

private:
    Json Call(const Json& params, const std::function<void(std::function<void()>)>& onUi) const;
    ServerInfo info_;
    std::vector<Tool> tools_;
};

// ---- Streamable HTTP on 127.0.0.1:port/mcp, Authorization: Bearer <token> ----

// Requests are answered on worker threads; tool calls hop to the UI thread through RunOnUi.
bool StartHttp(std::shared_ptr<Server> server, int port, const std::string& token, std::wstring* error);
void StopHttp();
bool HttpRunning();
int HttpPort();

// What the HTTP layer decides before the body is looked at (exposed for tests).
struct HttpRequest {
    std::string method, path, body;
    std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
    std::string Header(const char* name) const;
};
// Parses the head and body of one request; false while incomplete. `bad` is set for malformed requests.
bool ParseHttpRequest(const std::string& raw, HttpRequest* out, bool* bad);
// 0 when the request may go on, else the HTTP status to refuse it with.
int CheckHttpRequest(const HttpRequest& r, int port, const std::string& token);

// ---- stdio ----

// Newline-delimited JSON-RPC on stdin/stdout, relayed to http://127.0.0.1:port/mcp. `ensureRunning` starts the app
// when it isn't, and returns false when it can't. Returns the process exit code.
int RunStdioBridge(int port, const std::string& token, const std::function<bool()>& ensureRunning);

// POSTs `body` to 127.0.0.1:port/mcp. Returns the HTTP status (0 when the connection failed) and the response body.
int HttpPost(int port, const std::string& token, const std::string& body, std::string* response, int timeoutMs);

}  // namespace ather::mcp
