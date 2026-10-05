#pragma once
#include <optional>

#include "common.h"

namespace ather {

// Updates in place: a small manifest (downloads/latest.json on GitHub, written by package.bat) names the newest
// Windows version, its exe, SHA-256 and size. The app checks it, downloads the exe next to itself, verifies it
// (size, hash, and the version inside the exe), swaps it for the running one, and restarts. Installed and
// portable copies both work without admin rights, because the exe lives in a folder the user owns.
//
// Set ATHER_UPDATE_URL to try another manifest (http is allowed for localhost only, for testing).

struct UpdateInfo {
    std::wstring version, url, sha256, notes;
    uint64_t size = 0;
};

// -1, 0 or 1, comparing dotted numbers ("0.0.10" > "0.0.9", "1.0" == "1.0.0").
int CompareVersions(const std::wstring& a, const std::wstring& b);
// The Windows entry of a manifest, if it's well-formed and points at this repository over HTTPS.
std::optional<UpdateInfo> ParseUpdateManifest(const std::string& json, bool allowLocalhost = false);
// True when the file is exactly the update the manifest describes (its version compared as numbers, so "0.0.3"
// matches "0.0.3.0"). Otherwise *error says why.
bool VerifyUpdateFile(const std::wstring& path, const UpdateInfo& info, std::wstring* error);
// Puts `staged` in place of `target`, keeping the old one as target.old (rolled back on failure).
bool SwapExe(const std::wstring& target, const std::wstring& staged, std::wstring* error);

// Looks for a newer version on a worker thread. `done` runs on the UI thread: the newer version (or none when
// this one is current), or an error.
void CheckForUpdateAsync(std::function<void(std::optional<UpdateInfo>, std::wstring error)> done);
// Downloads and verifies the update next to the running exe. `progress` (0…1) and `done` run on the UI thread;
// `done` gets the staged file, or an error.
void DownloadUpdateAsync(const UpdateInfo& info, std::function<void(double)> progress,
                         std::function<void(std::wstring staged, std::wstring error)> done);
// Swaps the staged exe in and starts it (it waits for this process to exit). The caller then quits.
bool InstallStagedUpdate(const std::wstring& staged, std::wstring* error);

// At startup, before the single-instance check. `--after-update <pid>`: waits for the old copy to exit and
// returns true (show "Updated").
bool FinishUpdate(const std::wstring& cmdline);
// Removes leftovers of an earlier update (<exe>.old, <exe>.update). Only the running instance may call it: another
// one may be downloading into <exe>.update right now.
void CleanUpUpdateFiles();

}  // namespace ather
