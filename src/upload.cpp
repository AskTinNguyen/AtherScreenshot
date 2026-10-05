#include "upload.h"

#include "json.h"
#include "net.h"

#include <cwctype>
#include <thread>

namespace ather {
namespace {

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

std::string Multipart(const std::string& boundary, const std::wstring& field, const std::wstring& path,
                      const std::string& bytes) {
    std::string b = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + ToUtf8(field) +
                    "\"; filename=\"" + ToUtf8(FileNameOf(path)) + "\"\r\nContent-Type: " + ToUtf8(ContentType(path)) +
                    "\r\n\r\n";
    b += bytes;
    b += "\r\n--" + boundary + "--\r\n";
    return b;
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
    const auto u = CrackUrl(endpoint);
    if (!u) {
        err = L"Invalid S3Endpoint";
        return;
    }
    std::wstring hostHeader = u->host;
    const bool defaultPort = (u->https && u->port == 443) || (!u->https && u->port == 80);
    if (!defaultPort) hostHeader += L":" + std::to_wstring(u->port);

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
    std::string k = HmacSha256("AWS4" + ToUtf8(cfg.s3SecretKey), dateStamp);
    k = HmacSha256(k, region);
    k = HmacSha256(k, "s3");
    k = HmacSha256(k, "aws4_request");
    const std::string auth = "AWS4-HMAC-SHA256 Credential=" + ToUtf8(cfg.s3AccessKey) + "/" + scope +
                             ", SignedHeaders=" + signedHeaders + ", Signature=" + Hex(HmacSha256(k, toSign));

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
