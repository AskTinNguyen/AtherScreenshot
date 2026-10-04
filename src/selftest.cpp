#include "selftest.h"

#include "common.h"

#include <shellapi.h>

#include <vector>

namespace ather::test {
namespace {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& Cases() {
    static std::vector<Case> c;
    return c;
}

int g_failures = 0, g_checks = 0;
const char* g_current = "";
std::string g_note, g_out;
std::wstring g_root;
int g_tempSeq = 0;

void Print(const std::string& s) { g_out += s; }

void Flush() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    bool console = false;
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            console = true;
        }
    }
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(out, g_out.data(), (DWORD)g_out.size(), &wr, nullptr);
        if (console) CloseHandle(out);
    }
}

std::string Narrow(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}

void RemoveTree(const std::wstring& dir) {
    std::wstring from = dir + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

}  // namespace

void Register(const char* name, void (*fn)()) { Cases().push_back({name, fn}); }

void Out(const std::string& text) { Print(text); }
void FlushOut() {
    Flush();
    g_out.clear();
}

void Note(const std::string& text) { g_note = text; }

void Check(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    const char* base = strrchr(file, '\\');
    Print(std::string("  FAIL ") + g_current + ": " + expr + "  (" + (base ? base + 1 : file) + ":" + std::to_string(line) + ")\n");
    if (!g_note.empty()) Print("       " + g_note + "\n");
}

std::wstring TempDir() {
    std::wstring d = g_root + L"\\t" + std::to_wstring(++g_tempSeq);
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

int Run(const std::wstring& filter) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"AtherSelftest-" + std::to_wstring(GetCurrentProcessId());
    RemoveTree(g_root);
    CreateDirectoryW(g_root.c_str(), nullptr);
    const std::wstring support = g_root + L"\\support";
    CreateDirectoryW(support.c_str(), nullptr);
    SetEnvironmentVariableW(L"ATHER_SUPPORT_DIR", support.c_str());

    const std::string f = Narrow(filter);
    int ran = 0, failedCases = 0;
    for (const auto& c : Cases()) {
        if (!f.empty() && std::string(c.name).find(f) == std::string::npos) continue;
        g_current = c.name;
        g_note.clear();
        const int before = g_failures;
        const ULONGLONG t0 = GetTickCount64();
        c.fn();
        ++ran;
        const bool ok = g_failures == before;
        if (!ok) ++failedCases;
        Print(std::string(ok ? "ok   " : "FAIL ") + c.name + "  (" + std::to_string(GetTickCount64() - t0) + " ms)\n");
    }
    Print("\n" + std::to_string(ran) + " tests, " + std::to_string(g_checks) + " checks, " + std::to_string(failedCases) +
          " failed\nResult: " + (g_failures ? "FAIL" : "PASS") + "\n");
    Flush();
    RemoveTree(g_root);
    return g_failures;
}

}  // namespace ather::test
