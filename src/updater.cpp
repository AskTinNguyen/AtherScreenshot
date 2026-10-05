#include "updater.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <thread>

#include "json.h"
#include "selftest.h"
#include "version.h"

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "version")

namespace ather {
namespace {

constexpr wchar_t kManifestUrl[] = L"https://raw.githubusercontent.com/AskTinNguyen/AtherScreenshot/main/downloads/latest.json";
constexpr wchar_t kRepoPath[] = L"/AskTinNguyen/AtherScreenshot/";
constexpr uint64_t kMaxExe = 64ull << 20, kMaxManifest = 64 << 10;

std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return n && n < std::size(buf) ? std::wstring(buf, n) : L"";
}

std::wstring SelfExe() {
    wchar_t p[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, p, (DWORD)std::size(p));
    return p;
}

struct Url {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool https = false;
};

std::optional<Url> Crack(const std::wstring& url) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    uc.dwExtraInfoLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return std::nullopt;
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) return std::nullopt;
    Url u{host, path, uc.nPort, uc.nScheme == INTERNET_SCHEME_HTTPS};
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) u.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    return u;
}

bool IsLocalhost(const std::wstring& host) { return _wcsicmp(host.c_str(), L"localhost") == 0 || host == L"127.0.0.1"; }

// Updates come only from this repository on GitHub, over HTTPS (redirects to GitHub's file hosts are followed,
// HTTPS only). With a test manifest, this PC is allowed too.
bool AllowedUrl(const std::wstring& url, bool allowLocalhost) {
    const auto u = Crack(url);
    if (!u) return false;
    if (allowLocalhost && IsLocalhost(u->host)) return true;
    if (!u->https) return false;
    const bool github = _wcsicmp(u->host.c_str(), L"github.com") == 0 || _wcsicmp(u->host.c_str(), L"raw.githubusercontent.com") == 0;
    return github && _wcsnicmp(u->path.c_str(), kRepoPath, wcslen(kRepoPath)) == 0;
}

// GET, handing the body to `sink` as it arrives. `progress(received, total)`; total is 0 when unknown.
bool HttpGet(const std::wstring& url, uint64_t maxBytes, const std::function<bool(const char*, size_t)>& sink,
             const std::function<void(uint64_t, uint64_t)>& progress, std::wstring* error) {
    const auto u = Crack(url);
    if (!u) {
        *error = L"Bad address: " + url;
        return false;
    }
    const std::wstring agent = std::wstring(L"AtherScreenshot/") + ATHER_VERSION_WSTR;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, u->host.c_str(), u->port, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", u->path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         (u->https ? WINHTTP_FLAG_SECURE : 0) | WINHTTP_FLAG_REFRESH)
                    : nullptr;
    bool ok = r != nullptr;
    if (ok) {
        WinHttpSetTimeouts(r, 10000, 15000, 30000, 30000);
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        ok = WinHttpSendRequest(r, L"Cache-Control: no-cache\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr);
        if (!ok) *error = L"Can't reach GitHub (network error " + std::to_wstring(GetLastError()) + L").";
    }
    DWORD status = 0;
    if (ok) {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            ok = false;
            *error = L"The update server answered " + std::to_wstring(status) + L".";
        }
    }
    if (ok) {
        uint64_t total = 0, got = 0;
        wchar_t len[32] = {};
        DWORD size = sizeof(len);
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(len, nullptr, 10);
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD n = 0;
            if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) {
                ok = false;
                *error = L"The download was interrupted.";
                break;
            }
            if (n == 0) break;
            got += n;
            if (got > maxBytes) {
                ok = false;
                *error = L"The download is larger than expected.";
                break;
            }
            if (!sink(buf.data(), n)) {
                ok = false;
                *error = L"Couldn't save the download.";
                break;
            }
            if (progress) progress(got, total);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!ok && error->empty()) *error = L"Can't reach GitHub.";
    return ok;
}

std::wstring Sha256File(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring hex;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        std::vector<char> buf(64 * 1024);
        bool ok = true;
        while (ok && in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            if (in.gcount() > 0) ok = BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)buf.data(), (ULONG)in.gcount(), 0));
        }
        UCHAR digest[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(h, digest, sizeof(digest), 0))) {
            wchar_t b[3];
            for (UCHAR x : digest) {
                swprintf_s(b, L"%02x", x);
                hex += b;
            }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// The version written in an exe's VERSIONINFO (ProductVersion), e.g. "0.0.3".
std::wstring ExeVersion(const std::wstring& path) {
    DWORD ignore = 0;
    const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &ignore);
    if (!n) return L"";
    std::vector<BYTE> data(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, data.data())) return L"";
    struct Lang {
        WORD lang, cp;
    }* langs = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (void**)&langs, &len) || len < sizeof(Lang)) return L"";
    wchar_t key[64];
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\ProductVersion", langs[0].lang, langs[0].cp);
    wchar_t* v = nullptr;
    if (!VerQueryValueW(data.data(), key, (void**)&v, &len) || !v) return L"";
    return v;
}

}  // namespace

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        uint64_t x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        if (i < a.size()) ++i;  // the dot
        if (j < b.size()) ++j;
    }
    return 0;
}

std::optional<UpdateInfo> ParseUpdateManifest(const std::string& json, bool allowLocalhost) {
    std::string text = json;
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // a BOM from a Windows editor
    bool ok = false;
    const Json root = Json::Parse(text, &ok);
    const Json& w = root["windows"];
    if (!ok || !w.IsObject()) return std::nullopt;
    UpdateInfo u;
    u.version = w["version"].WStr();
    u.url = w["url"].WStr();
    u.sha256 = w["sha256"].WStr();
    u.notes = w["notes"].WStr();
    u.size = w["size"].UInt();
    for (auto& c : u.sha256) c = (wchar_t)towlower(c);
    const bool versionOk = !u.version.empty() && u.version.size() <= 20 &&
                           std::all_of(u.version.begin(), u.version.end(), [](wchar_t c) { return iswdigit(c) || c == L'.'; }) && iswdigit(u.version[0]);
    const bool hashOk = u.sha256.size() == 64 && std::all_of(u.sha256.begin(), u.sha256.end(), [](wchar_t c) { return iswxdigit(c); });
    if (!versionOk || !hashOk || u.size == 0 || u.size > kMaxExe || !AllowedUrl(u.url, allowLocalhost)) return std::nullopt;
    if (u.notes.size() > 300) u.notes.resize(300);
    return u;
}

bool VerifyUpdateFile(const std::wstring& path, const UpdateInfo& info, std::wstring* error) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        *error = L"The download is missing.";
        return false;
    }
    if ((((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) != info.size) {
        *error = L"The download is incomplete.";
        return false;
    }
    if (Sha256File(path) != info.sha256) {
        *error = L"The download doesn't match its checksum, so it wasn't installed.";
        return false;
    }
    if (CompareVersions(ExeVersion(path), info.version) != 0 || ExeVersion(path).empty()) {
        *error = L"The download isn't Ather Screenshot " + info.version + L", so it wasn't installed.";
        return false;
    }
    return true;
}

bool SwapExe(const std::wstring& target, const std::wstring& staged, std::wstring* error) {
    const std::wstring old = target + L".old";
    DeleteFileW(old.c_str());
    // A running exe can be renamed, just not overwritten.
    if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Can't replace " + FileNameOf(target) + L" (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    if (!MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Can't put the new version in place (error " + std::to_wstring(GetLastError()) + L").";
        MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    return true;
}

void CheckForUpdateAsync(std::function<void(std::optional<UpdateInfo>, std::wstring)> done) {
    std::thread([done] {
        const std::wstring custom = EnvVar(L"ATHER_UPDATE_URL");
        const std::wstring url = custom.empty() ? kManifestUrl : custom;
        std::string body;
        std::wstring err;
        std::optional<UpdateInfo> found;
        if (HttpGet(url, kMaxManifest, [&](const char* p, size_t n) { return body.append(p, n), true; }, nullptr, &err)) {
            if (auto info = ParseUpdateManifest(body, !custom.empty())) {
                if (CompareVersions(info->version, ATHER_VERSION_WSTR) > 0) found = info;
            } else {
                err = L"The update information on GitHub couldn't be read.";
            }
        }
        RunOnUi([done, found, err] { done(found, err); });
    }).detach();
}

void DownloadUpdateAsync(const UpdateInfo& info, std::function<void(double)> progress, std::function<void(std::wstring, std::wstring)> done) {
    std::thread([info, progress, done] {
        const std::wstring self = SelfExe();
        const std::wstring staged = self + L".update";
        std::wstring err;
        HANDLE f = CreateFileW(staged.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            err = L"Can't write to " + self.substr(0, self.find_last_of(L'\\')) + L".";
        } else {
            int lastPct = -1;
            HttpGet(info.url, kMaxExe, [&](const char* p, size_t n) {
                DWORD w = 0;
                return WriteFile(f, p, (DWORD)n, &w, nullptr) && w == n;
            }, [&](uint64_t got, uint64_t total) {
                const int pct = (int)(100 * got / std::max<uint64_t>(1, total ? total : info.size));
                if (pct != lastPct && progress) {
                    lastPct = pct;
                    RunOnUi([progress, pct] { progress(std::min(1.0, pct / 100.0)); });
                }
            }, &err);
            CloseHandle(f);
            if (err.empty()) VerifyUpdateFile(staged, info, &err);
            if (!err.empty()) DeleteFileW(staged.c_str());
        }
        RunOnUi([done, staged, err] { done(err.empty() ? staged : L"", err); });
    }).detach();
}

bool InstallStagedUpdate(const std::wstring& staged, std::wstring* error) {
    const std::wstring target = SelfExe();
    if (!SwapExe(target, staged, error)) return false;
    std::wstring cmd = L"\"" + target + L"\" --after-update " + std::to_wstring(GetCurrentProcessId());
    const std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(target.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        *error = L"Couldn't start the new version (error " + std::to_wstring(GetLastError()) + L").";
        MoveFileExW(target.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING);  // back as it was
        MoveFileExW((target + L".old").c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(staged.c_str());
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool FinishUpdate(const std::wstring& cmdline) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    const bool after = argv && argc >= 3 && _wcsicmp(argv[1], L"--after-update") == 0;
    const DWORD pid = after ? (DWORD)_wtoi(argv[2]) : 0;
    LocalFree(argv);
    if (after && pid) {  // let the old copy finish saving and quit, so this one becomes the running instance
        if (HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(p, 5 * 60 * 1000);  // it may be finishing a save
            CloseHandle(p);
        }
    }
    return after;
}

void CleanUpUpdateFiles() {
    const std::wstring self = SelfExe();
    for (const wchar_t* ext : {L".old", L".update"}) {
        const std::wstring f = self + ext;
        // The old exe can still be mapped for a moment after its process exits.
        for (int i = 0; i < 10 && GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(f.c_str()); ++i) Sleep(200);
    }
}

// ---- tests ----

ATHER_TEST(updater_compares_versions) {
    CHECK(CompareVersions(L"0.0.10", L"0.0.9") > 0);
    CHECK(CompareVersions(L"0.0.2", L"0.1") < 0);
    CHECK(CompareVersions(L"1.0", L"1.0.0") == 0);
    CHECK(CompareVersions(L"0.0.2", L"0.0.2") == 0);
}

ATHER_TEST(updater_manifest_only_from_this_repo_over_https) {
    const std::string sha(64, 'a');
    auto m = [&](const std::string& url, const std::string& hash = std::string(64, 'a'), const std::string& ver = "0.0.3") {
        return "{\"windows\":{\"version\":\"" + ver + "\",\"url\":\"" + url + "\",\"sha256\":\"" + hash + "\",\"size\":2795520,\"notes\":\"New things\"}}";
    };
    const std::string good = "https://github.com/AskTinNguyen/AtherScreenshot/raw/main/downloads/AtherScreenshot-Setup-0.0.3.exe";
    auto u = ParseUpdateManifest(m(good));
    CHECK(u && u->version == L"0.0.3" && u->size == 2795520 && u->notes == L"New things");
    CHECK(ParseUpdateManifest("\xEF\xBB\xBF" + m(good)).has_value());  // BOM
    CHECK(ParseUpdateManifest(m("https://raw.githubusercontent.com/AskTinNguyen/AtherScreenshot/main/downloads/x.exe")).has_value());
    CHECK(!ParseUpdateManifest(m("http://github.com/AskTinNguyen/AtherScreenshot/raw/main/x.exe")));      // not HTTPS
    CHECK(!ParseUpdateManifest(m("https://github.com/SomeoneElse/AtherScreenshot/raw/main/x.exe")));      // another repo
    CHECK(!ParseUpdateManifest(m("https://evil.example/AskTinNguyen/AtherScreenshot/x.exe")));            // another host
    CHECK(!ParseUpdateManifest(m(good, "1234")));                                                        // bad hash
    CHECK(!ParseUpdateManifest(m(good, sha, "0.0.3; rm")));                                              // bad version
    CHECK(!ParseUpdateManifest("{\"macos\":{}}"));
    CHECK(!ParseUpdateManifest(m("http://localhost:8000/x.exe")));
    CHECK(ParseUpdateManifest(m("http://localhost:8000/x.exe"), true).has_value());  // test manifests only
}

ATHER_TEST(updater_verifies_and_swaps_the_exe) {
    const std::wstring dir = test::TempDir();
    const std::wstring staged = dir + L"\\new.exe", target = dir + L"\\app.exe";
    CHECK(CopyFileW(SelfExe().c_str(), staged.c_str(), FALSE));  // a real exe of this version
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(staged.c_str(), GetFileExInfoStandard, &fa);
    UpdateInfo info;
    info.version = ATHER_VERSION_WSTR;
    info.size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    info.sha256 = Sha256File(staged);
    std::wstring err;
    CHECK(info.sha256.size() == 64);
    CHECK(VerifyUpdateFile(staged, info, &err));
    UpdateInfo padded = info;
    padded.version += L".0";  // "0.0.2.0" is the same version as the "0.0.2" inside the exe
    CHECK(VerifyUpdateFile(staged, padded, &err));
    UpdateInfo wrong = info;
    wrong.sha256[0] = wrong.sha256[0] == L'0' ? L'1' : L'0';
    CHECK(!VerifyUpdateFile(staged, wrong, &err) && err.find(L"checksum") != std::wstring::npos);
    wrong = info;
    wrong.version = L"9.9.9";  // right file, but not the version the manifest promised
    CHECK(!VerifyUpdateFile(staged, wrong, &err));
    wrong = info;
    wrong.size += 1;
    CHECK(!VerifyUpdateFile(staged, wrong, &err));
    {
        std::ofstream(target, std::ios::binary) << "old";
    }
    CHECK(SwapExe(target, staged, &err));
    CHECK(GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(Sha256File(target) == info.sha256);
    std::ifstream old(target + L".old", std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(old)), {});
    CHECK(s == "old");
    // A missing staged file leaves the original where it was.
    old.close();
    CHECK(!SwapExe(target, dir + L"\\nothing.exe", &err));
    CHECK(Sha256File(target) == info.sha256);
}

// End to end over the network, opt-in: set ATHER_UPDATE_URL to a test server's manifest for a newer build.
ATHER_TEST(updater_finds_downloads_and_verifies_from_a_test_server) {
    if (EnvVar(L"ATHER_UPDATE_URL").empty()) return;
    HWND dispatcher = StartUiDispatcher();
    auto pump = [](const bool& until) {
        for (int i = 0; i < 600 && !until; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            Sleep(50);
        }
    };
    bool checked = false;
    std::optional<UpdateInfo> found;
    std::wstring err;
    CheckForUpdateAsync([&](std::optional<UpdateInfo> info, std::wstring e) {
        found = info;
        err = e;
        checked = true;
    });
    pump(checked);
    test::Note("check error: " + ToUtf8(err));
    CHECK(checked && err.empty() && found.has_value());
    if (found) {
        bool downloaded = false;
        double last = 0;
        std::wstring staged;
        DownloadUpdateAsync(*found, [&](double p) { last = p; }, [&](std::wstring s2, std::wstring e) {
            staged = s2;
            err = e;
            downloaded = true;
        });
        pump(downloaded);
        test::Note("download error: " + ToUtf8(err));
        CHECK(downloaded && err.empty() && !staged.empty());
        CHECK(last > 0.99);
        CHECK(staged.empty() || ExeVersion(staged) == found->version);
        if (!staged.empty()) DeleteFileW(staged.c_str());
        // A tampered manifest (wrong checksum) never leaves a file behind.
        UpdateInfo bad = *found;
        bad.sha256[0] = bad.sha256[0] == L'0' ? L'1' : L'0';
        downloaded = false;
        DownloadUpdateAsync(bad, nullptr, [&](std::wstring s2, std::wstring e) {
            staged = s2;
            err = e;
            downloaded = true;
        });
        pump(downloaded);
        CHECK(staged.empty() && err.find(L"checksum") != std::wstring::npos);
        CHECK(GetFileAttributesW((SelfExe() + L".update").c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    StopUiDispatcher(dispatcher);
}

}  // namespace ather
