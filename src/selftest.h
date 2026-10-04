#pragma once
#include <cmath>
#include <string>

// `AtherScreenshot.exe --selftest [filter]` runs every ATHER_TEST whose name contains `filter`, prints the
// results and exits with the number of failures. ATHER_SUPPORT_DIR points at a fresh temp folder for the run,
// so tests never touch the real settings or library.

namespace ather::test {

void Register(const char* name, void (*fn)());
void Check(bool ok, const char* expr, const char* file, int line);
void Note(const std::string& text);  // extra context printed with the next failure
int Run(const std::wstring& filter);
std::wstring TempDir();  // a fresh folder inside the run's temp folder

}  // namespace ather::test

#define ATHER_TEST(name)                                                                  \
    static void name();                                                                   \
    static const bool name##_registered = (::ather::test::Register(#name, name), true);   \
    static void name()

#define CHECK(e) ::ather::test::Check(!!(e), #e, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::ather::test::Check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, eps) ::ather::test::Check(std::fabs((double)(a) - (double)(b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
