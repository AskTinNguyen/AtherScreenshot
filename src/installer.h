#pragma once
#include "common.h"

namespace ather {

// Per-user install location: %LOCALAPPDATA%\Programs\AtherScreenshot\AtherScreenshot.exe
std::wstring InstalledExePath();

// Runs before the app starts. Handles `--uninstall`, and when the exe is launched from anywhere other than
// the install location (e.g. Downloads), shows the branded install/update window.
// Returns true if the process should exit with *exitCode; false to continue starting the app.
bool RunInstallFlow(const std::wstring& cmdline, HICON icon, int* exitCode);
// After an in-app update of the installed copy: the version in Apps & features and the Explorer menus.
void RefreshInstallRecord();

}  // namespace ather
