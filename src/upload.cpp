#include "upload.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <cwctype>
#include <thread>

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")

namespace ather {
namespace {

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    while (!s.empty() && iswspace(s.front())) s.erase(0, 1);
    return s;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size();
    CloseHandle(f);
    return ok;
}

std::wstring ContentType(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext == L"png") return L"image/png";
    if (ext == L"jpg" || ext == L"jpeg") return L"image/jpeg";
    if (ext == L"gif") return L"image/gif";
    if (ext == L"mp4") return L"video/mp4";
    return L"application/octet-stream";
}

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::wstring error;
};

HttpResult Http(const std::wstring& method, const std::wstring& url,
                const std::vector<std::pair<std::wstring, std::wstring>>& headers, const std::string& body) {
    HttpResult res;
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    uc.dwExtraInfoLength = 1;  // keep the query string attached to the path
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        res.error = L"Invalid URL: " + url;
        return res;
    }
    std::wstring fullPath = path;
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) fullPath.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"AtherScreenshot/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, method.c_str(), fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = r != nullptr;
    if (ok) WinHttpSetTimeouts(r, 10000, 15000, 120000, 120000);
    for (const auto& [k, v] : headers) {
        std::wstring line = k + L": " + v;
        if (ok) ok = WinHttpAddRequestHeaders(r, line.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (ok)
        ok = WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body.data(), (DWORD)body.size(),
                                (DWORD)body.size(), 0) &&
             WinHttpReceiveResponse(r, nullptr);
    if (ok) {
        DWORD size = sizeof(res.status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &res.status, &size, WINHTTP_NO_HEADER_INDEX);
        for (DWORD avail = 0; WinHttpQueryDataAvailable(r, &avail) && avail;) {
            size_t old = res.body.size();
            res.body.resize(old + avail);
            DWORD got = 0;
            WinHttpReadData(r, res.body.data() + old, avail, &got);
            res.body.resize(old + got);
            if (res.body.size() > 4 * 1024 * 1024) break;
        }
    } else {
        wchar_t msg[96];
        swprintf_s(msg, L"Network error %lu", GetLastError());
        res.error = msg;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return res;
}

std::string Multipart(const std::string& boundary, const std::wstring& field, const std::wstring& path,
                      const std::string& bytes) {
    std::string b = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + ToUtf8(field) +
                    "\"; filename=\"" + ToUtf8(FileNameOf(path)) + "\"\r\nContent-Type: " + ToUtf8(ContentType(path)) +
                    "\r\n\r\n";
    b += bytes;
    b += "\r\n--" + boundary + "--\r\n";
    return b;
}

std::wstring HttpError(const HttpResult& r) {
    if (!r.error.empty()) return r.error;
    std::wstring body = FromUtf8(r.body.substr(0, 300));
    return L"HTTP " + std::to_wstring(r.status) + (body.empty() ? L"" : L": " + body);
}

// ---- AWS Signature V4 (S3-compatible PUT) ----

std::string Hmac(const std::string& key, const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out(32, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        BCryptHash(alg, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)data.data(), (ULONG)data.size(),
                   (PUCHAR)out.data(), 32);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

std::string Sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    std::string out(32, '\0');
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), (PUCHAR)out.data(), 32);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return out;
}

std::string Hex(const std::string& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

std::string UriEncode(const std::string& s, bool keepSlash) {
    static const char* d = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) out += (char)c;
        else {
            out += '%';
            out += d[c >> 4];
            out += d[c & 15];
        }
    }
    return out;
}

void UploadS3(const std::wstring& path, const UploadConfig& cfg, std::wstring& url, std::wstring& err) {
    std::string bytes;
    if (!ReadFileBytes(path, bytes)) {
        err = L"Could not read " + FileNameOf(path);
        return;
    }
    std::wstring endpoint = cfg.s3Endpoint;
    while (!endpoint.empty() && endpoint.back() == L'/') endpoint.pop_back();
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    if (!WinHttpCrackUrl(endpoint.c_str(), 0, 0, &uc)) {
        err = L"Invalid S3Endpoint";
        return;
    }
    std::wstring hostHeader = host;
    const bool defaultPort = (uc.nScheme == INTERNET_SCHEME_HTTPS && uc.nPort == 443) ||
                             (uc.nScheme == INTERNET_SCHEME_HTTP && uc.nPort == 80);
    if (!defaultPort) hostHeader += L":" + std::to_wstring(uc.nPort);

    SYSTEMTIME t;
    GetSystemTime(&t);
    char amzDate[32], dateStamp[16];
    sprintf_s(amzDate, "%04u%02u%02uT%02u%02u%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    sprintf_s(dateStamp, "%04u%02u%02u", t.wYear, t.wMonth, t.wDay);
    char month[16];
    sprintf_s(month, "%04u-%02u", t.wYear, t.wMonth);

    const std::string key = std::string(month) + "/" + ToUtf8(FileNameOf(path));
    const std::string ct = ToUtf8(ContentType(path)), region = ToUtf8(cfg.s3Region.empty() ? L"auto" : cfg.s3Region);
    const std::string canonicalUri = "/" + UriEncode(ToUtf8(cfg.s3Bucket), false) + "/" + UriEncode(key, true);
    const std::string signedHeaders = "content-type;host;x-amz-content-sha256;x-amz-date";
    const std::string canonicalRequest = "PUT\n" + canonicalUri + "\n\ncontent-type:" + ct + "\nhost:" +
                                         ToUtf8(hostHeader) + "\nx-amz-content-sha256:UNSIGNED-PAYLOAD\nx-amz-date:" +
                                         amzDate + "\n\n" + signedHeaders + "\nUNSIGNED-PAYLOAD";
    const std::string scope = std::string(dateStamp) + "/" + region + "/s3/aws4_request";
    const std::string toSign = std::string("AWS4-HMAC-SHA256\n") + amzDate + "\n" + scope + "\n" + Hex(Sha256(canonicalRequest));
    std::string k = Hmac("AWS4" + ToUtf8(cfg.s3SecretKey), dateStamp);
    k = Hmac(k, region);
    k = Hmac(k, "s3");
    k = Hmac(k, "aws4_request");
    const std::string auth = "AWS4-HMAC-SHA256 Credential=" + ToUtf8(cfg.s3AccessKey) + "/" + scope +
                             ", SignedHeaders=" + signedHeaders + ", Signature=" + Hex(Hmac(k, toSign));

    HttpResult r = Http(L"PUT", endpoint + FromUtf8(canonicalUri),
                        {{L"Content-Type", FromUtf8(ct)},
                         {L"x-amz-content-sha256", L"UNSIGNED-PAYLOAD"},
                         {L"x-amz-date", FromUtf8(amzDate)},
                         {L"Authorization", FromUtf8(auth)}},
                        bytes);
    if (r.status < 200 || r.status >= 300) {
        err = HttpError(r);
        return;
    }
    std::wstring base = cfg.s3PublicUrl.empty() ? endpoint + L"/" + cfg.s3Bucket : cfg.s3PublicUrl;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    url = base + L"/" + FromUtf8(UriEncode(key, true));
}

void UploadMultipart(const std::wstring& path, const std::wstring& endpoint, const std::wstring& field,
                     std::vector<std::pair<std::wstring, std::wstring>> headers, const std::wstring& responsePath,
                     std::wstring& url, std::wstring& err) {
    std::string bytes;
    if (!ReadFileBytes(path, bytes)) {
        err = L"Could not read " + FileNameOf(path);
        return;
    }
    char boundary[48];
    sprintf_s(boundary, "----AtherBoundary%08lX%08llX", GetCurrentThreadId(), GetTickCount64());
    headers.push_back({L"Content-Type", L"multipart/form-data; boundary=" + FromUtf8(boundary)});
    HttpResult r = Http(L"POST", endpoint, headers, Multipart(boundary, field, path, bytes));
    if (r.status < 200 || r.status >= 300) {
        err = HttpError(r);
        return;
    }
    url = responsePath.empty() ? Trim(FromUtf8(r.body)) : JsonPath(r.body, responsePath);
    if (url.empty()) err = L"Upload succeeded but no link was found in the response.";
}

}  // namespace

// ---- tiny JSON path reader ----

namespace {
struct JsonCursor {
    const std::string& s;
    size_t i = 0;
    void Ws() {
        while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    }
    std::string String() {  // at '"'
        std::string out;
        ++i;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char e = s[++i];
                if (e == 'n') out += '\n';
                else if (e == 't') out += '\t';
                else if (e == 'u' && i + 4 < s.size()) {
                    unsigned cp = std::stoul(s.substr(i + 1, 4), nullptr, 16);
                    out += ToUtf8(std::wstring(1, (wchar_t)cp));
                    i += 4;
                } else out += e;  // \" \\ \/
                ++i;
            } else {
                out += s[i++];
            }
        }
        ++i;
        return out;
    }
    void Skip() {  // any value
        Ws();
        if (i >= s.size()) return;
        if (s[i] == '"') {
            String();
        } else if (s[i] == '{' || s[i] == '[') {
            int depth = 0;
            do {
                if (s[i] == '"') {
                    String();
                    continue;
                }
                if (s[i] == '{' || s[i] == '[') ++depth;
                if (s[i] == '}' || s[i] == ']') --depth;
                ++i;
            } while (i < s.size() && depth > 0);
        } else {
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        }
    }
    // Moves to the value of `key` in the object at the cursor; false if absent.
    bool Member(const std::string& key) {
        Ws();
        if (i >= s.size() || s[i] != '{') return false;
        ++i;
        for (;;) {
            Ws();
            if (i >= s.size() || s[i] == '}') return false;
            std::string k = String();
            Ws();
            ++i;  // ':'
            Ws();
            if (k == key) return true;
            Skip();
            Ws();
            if (i < s.size() && s[i] == ',') ++i;
        }
    }
    bool Index(size_t n) {
        Ws();
        if (i >= s.size() || s[i] != '[') return false;
        ++i;
        for (size_t k = 0;; ++k) {
            Ws();
            if (i >= s.size() || s[i] == ']') return false;
            if (k == n) return true;
            Skip();
            Ws();
            if (i < s.size() && s[i] == ',') ++i;
        }
    }
};
}  // namespace

std::wstring JsonPath(const std::string& json, const std::wstring& path) try {
    JsonCursor c{json};
    std::string p = ToUtf8(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t dot = p.find('.', start);
        std::string part = p.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        const bool numeric = !part.empty() && part.find_first_not_of("0123456789") == std::string::npos;
        if (!(numeric ? c.Index(std::stoul(part)) : c.Member(part))) return L"";
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    c.Ws();
    if (c.i < json.size() && json[c.i] == '"') return FromUtf8(c.String());
    size_t b = c.i;
    c.Skip();
    return Trim(FromUtf8(json.substr(b, c.i - b)));
} catch (...) {
    return L"";  // malformed JSON
}

std::wstring UploadConfigProblem(const UploadConfig& cfg) {
    if (cfg.uploader == L"imgur") return cfg.imgurClientId.empty() ? L"Set ImgurClientId in settings.ini [Upload]." : L"";
    if (cfg.uploader == L"custom") return cfg.customUrl.empty() ? L"Set CustomUrl in settings.ini [Upload]." : L"";
    if (cfg.uploader == L"s3")
        return cfg.s3Endpoint.empty() || cfg.s3Bucket.empty() || cfg.s3AccessKey.empty() || cfg.s3SecretKey.empty()
                   ? L"Set S3Endpoint, S3Bucket, S3AccessKey and S3SecretKey in settings.ini [Upload]."
                   : L"";
    return L"No uploader configured. Set Uploader=imgur, custom or s3 in settings.ini [Upload].";
}

void UploadFileAsync(const std::wstring& path, const UploadConfig& cfg,
                     std::function<void(std::wstring, std::wstring)> done) {
    std::thread([path, cfg, done = std::move(done)] {
        std::wstring url, err = UploadConfigProblem(cfg);
        if (err.empty()) {
            if (cfg.uploader == L"imgur") {
                UploadMultipart(path, L"https://api.imgur.com/3/image", L"image",
                                {{L"Authorization", L"Client-ID " + cfg.imgurClientId}}, L"data.link", url, err);
            } else if (cfg.uploader == L"custom") {
                std::vector<std::pair<std::wstring, std::wstring>> headers;
                for (size_t start = 0; start < cfg.customHeaders.size();) {  // "Name: value; Name2: value2"
                    size_t semi = cfg.customHeaders.find(L';', start);
                    std::wstring h = cfg.customHeaders.substr(start, semi == std::wstring::npos ? std::wstring::npos : semi - start);
                    size_t colon = h.find(L':');
                    if (colon != std::wstring::npos) headers.push_back({Trim(h.substr(0, colon)), Trim(h.substr(colon + 1))});
                    if (semi == std::wstring::npos) break;
                    start = semi + 1;
                }
                UploadMultipart(path, cfg.customUrl, cfg.customFileField.empty() ? L"file" : cfg.customFileField, headers,
                                cfg.customResponseUrl, url, err);
            } else {
                UploadS3(path, cfg, url, err);
            }
        }
        RunOnUi([done, url, err] { done(url, err); });
    }).detach();
}

}  // namespace ather
