#pragma once
#include <optional>

#include "common.h"

namespace ather {

// HTTP(S) through WinHTTP with the system proxy, shared by uploads and updates, plus the hashes they need.

using HttpHeaders = std::vector<std::pair<std::wstring, std::wstring>>;

// Streams a response body instead of keeping it in HttpResult::body.
struct HttpStream {
    uint64_t maxBytes = 4 << 20;                         // a longer body is an error
    std::function<bool(const char*, size_t)> sink;       // takes the body as it arrives; false stops the download
    std::function<void(uint64_t got, uint64_t total)> progress;  // total is 0 when the server doesn't say
};

struct HttpResult {
    DWORD status = 0;
    std::string body;    // when not streamed (at most 4 MB)
    std::wstring error;  // network trouble; any HTTP status still counts as an answer
};

HttpResult Http(const std::wstring& method, const std::wstring& url, const HttpHeaders& headers = {}, const std::string& body = {},
                const HttpStream* stream = nullptr);
// The network error, or "HTTP 404: <start of the body>".
std::wstring HttpError(const HttpResult& r);

struct UrlParts {
    std::wstring host, path;  // path includes the query
    unsigned short port = 0;
    bool https = false;
};
std::optional<UrlParts> CrackUrl(const std::wstring& url);  // http and https only

std::string Sha256(const std::string& data);  // the raw 32 bytes
std::string HmacSha256(const std::string& key, const std::string& data);
std::wstring Sha256File(const std::wstring& path);  // lowercase hex, or empty when unreadable
std::string Hex(const std::string& bytes);

}  // namespace ather
