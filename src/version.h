#pragma once
// Single source of truth for the version: used by the code, the VERSIONINFO resource and package.bat.
#define ATHER_VERSION_MAJOR 0
#define ATHER_VERSION_MINOR 0
#define ATHER_VERSION_PATCH 2
#define ATHER_VERSION_STR "0.0.2"
#define ATHER_VERSION_WSTR L"0.0.2"
// A re-release of the same version gets the next build number. People see the version above; the exe's file
// version and the updater use the build, so copies of an earlier build of 0.0.2 still get the update.
#define ATHER_VERSION_BUILD 2
#define ATHER_BUILD_STR "0.0.2.2"
#define ATHER_BUILD_WSTR L"0.0.2.2"
