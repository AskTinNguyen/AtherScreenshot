#include "net.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <fstream>

#include "json.h"
#include "version.h"

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")

namespace ather {

std::optional<UrlParts> CrackUrl(const std::wstring& url) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    uc.dwExtraInfoLength = 1;  // keep the query string attached to the path
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return std::nullopt;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return std::nullopt;
    UrlParts u{host, path, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS};
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) u.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    return u;
}

HttpResult Http(const std::wstring& method, const std::wstring& url, const HttpHeaders& headers, const std::string& body, const HttpStream* stream) {
    HttpResult res;
    const auto u = CrackUrl(url);
    if (!u) {
        res.error = L"Invalid URL: " + url;
        return res;
    }
    const std::wstring agent = std::wstring(L"AtherScreenshot/") + ATHER_VERSION_WSTR;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, u->host.c_str(), u->port, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, method.c_str(), u->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u->https ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = r != nullptr;
    if (ok) {
        WinHttpSetTimeouts(r, 10000, 15000, 120000, 120000);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;  // redirects stay on HTTPS
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    }
    for (const auto& [k, v] : headers) {
        const std::wstring line = k + L": " + v;
        if (ok) ok = WinHttpAddRequestHeaders(r, line.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (ok)
        ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
             WinHttpReceiveResponse(r, nullptr);
    if (!ok) {
        res.error = L"Network error " + std::to_wstring(GetLastError());
    } else {
        DWORD size = sizeof(res.status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &res.status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        uint64_t total = 0, got = 0;
        wchar_t len[32] = {};
        size = sizeof(len);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(len, nullptr, 10);
        const uint64_t maxBytes = stream ? stream->maxBytes : 4 << 20;
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) {
                res.error = L"The download was interrupted.";
                break;
            }
            if (n == 0) break;
            if ((got += n) > maxBytes) {
                res.error = L"The answer is larger than expected.";
                break;
            }
            if (stream && stream->sink) {
                if (!stream->sink(buf.data(), n)) {
                    res.error = L"Couldn't save the download.";
                    break;
                }
            } else {
                res.body.append(buf.data(), n);
            }
            if (stream && stream->progress) stream->progress(got, total);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return res;
}

std::wstring HttpError(const HttpResult& r) {
    if (!r.error.empty()) return r.error;
    const std::wstring body = FromUtf8(r.body.substr(0, 300));
    return L"HTTP " + std::to_wstring(r.status) + (body.empty() ? L"" : L": " + body);
}

namespace {

std::string Digest(const std::string& key, const std::string& data, bool hmac) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out(32, '\0');
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0))) {
        BCryptHash(alg, hmac ? (PUCHAR)key.data() : nullptr, hmac ? (ULONG)key.size() : 0, (PUCHAR)data.data(), (ULONG)data.size(),
                   (PUCHAR)out.data(), 32);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

}  // namespace

std::string Sha256(const std::string& data) { return Digest({}, data, false); }
std::string HmacSha256(const std::string& key, const std::string& data) { return Digest(key, data, true); }

std::string Hex(const std::string& bytes) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : bytes) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

std::wstring Sha256File(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string digest(32, '\0');
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
    std::vector<char> buf(64 * 1024);
    while (ok && in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        if (in.gcount() > 0) ok = BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)in.gcount(), 0));
    }
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, (PUCHAR)digest.data(), 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? FromUtf8(Hex(digest)) : L"";
}

}  // namespace ather
