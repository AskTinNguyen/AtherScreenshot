@echo off
rem Builds a release and packages it for the team.
rem   package.bat            -> dist\ with the setup exe, a portable zip, checksums and latest.json
rem   package.bat publish    -> same, then puts them on the GitHub Release v<version> (created, or updated for a
rem                             new build of the same version) and writes downloads\latest.json. Commit and push
rem                             that file too: copies before 0.0.2.1 look for updates there.
rem Running copies find the update within a day (or at once with "Check for updates...").
rem Set UPDATE_NOTES to one line about what's new; the app shows it when it offers the update.
rem A re-release of the same version: raise ATHER_VERSION_BUILD and ATHER_BUILD_STR in src\version.h.
rem Optional code signing (removes the SmartScreen "unknown publisher" warning):
rem   set SIGN_PFX=path\to\cert.pfx & set SIGN_PASSWORD=...   before running.
setlocal
cd /d "%~dp0"

call "%~dp0build.bat" || exit /b 1

for /f "tokens=3" %%v in ('findstr /c:"#define ATHER_VERSION_STR" src\version.h') do set VER=%%~v
for /f "tokens=3" %%v in ('findstr /c:"#define ATHER_BUILD_STR" src\version.h') do set BUILD=%%~v
if not defined VER (
  echo Could not read the version from src\version.h
  exit /b 1
)
if not defined BUILD set BUILD=%VER%
set REPO=AskTinNguyen/AtherScreenshot

if defined SIGN_PFX (
  signtool sign /f "%SIGN_PFX%" /p "%SIGN_PASSWORD%" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 build\AtherScreenshot.exe || exit /b 1
)

set DIST=dist
set STAGE=%DIST%\AtherScreenshot-%VER%
if exist %DIST% rmdir /s /q %DIST%
mkdir %STAGE% || exit /b 1

rem 1) The one file teammates need: double-click to install / update.
copy /y build\AtherScreenshot.exe "%DIST%\AtherScreenshot-Setup-%VER%.exe" >nul || exit /b 1

rem 2) Portable zip: exe + docs (running it still offers to install; "Run without installing" stays portable).
copy /y build\AtherScreenshot.exe %STAGE%\AtherScreenshot.exe >nul
copy /y README.md %STAGE%\README.md >nul
copy /y INSTALL.md %STAGE%\INSTALL.md >nul
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%DIST%\AtherScreenshot-%VER%-portable.zip' -Force" || exit /b 1
copy /y INSTALL.md %DIST%\INSTALL.md >nul
rmdir /s /q %STAGE%

rem 3) Checksums so people can verify what they downloaded.
powershell -NoProfile -Command "Get-ChildItem '%DIST%\*' -Include *.exe,*.zip | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() + '  ' + $_.Name } | Set-Content -Encoding ascii '%DIST%\SHA256SUMS.txt'" || exit /b 1

rem 4) What running copies read to find this build (src\updater.cpp): the build number, and the setup exe on the
rem    GitHub Release. Other platforms' entries already in downloads\latest.json (e.g. macos) are kept.
powershell -NoProfile -Command "$f = Get-Item '%DIST%\AtherScreenshot-Setup-%VER%.exe'; $w = [ordered]@{ version = '%BUILD%'; url = 'https://github.com/%REPO%/releases/download/v%VER%/' + $f.Name; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower(); size = $f.Length; notes = [string]$env:UPDATE_NOTES }; $m = [ordered]@{}; if (Test-Path 'downloads\latest.json') { foreach ($q in (Get-Content 'downloads\latest.json' -Raw | ConvertFrom-Json).PSObject.Properties) { $m[$q.Name] = $q.Value } }; $m['windows'] = $w; [IO.File]::WriteAllText((Join-Path (Resolve-Path '%DIST%') 'latest.json'), (ConvertTo-Json $m -Depth 4))" || exit /b 1

echo.
echo Packaged Ather Screenshot %VER% (build %BUILD%):
dir /b %DIST%

if /i not "%1"=="publish" exit /b 0
where gh >nul 2>nul || (echo gh CLI not found & exit /b 1)
rem The release is tagged at the commit that was built, so it has to be on GitHub already.
for /f %%c in ('git rev-parse HEAD') do set COMMIT=%%c
git fetch -q origin
git branch -r --contains %COMMIT% | findstr /c:"origin/" >nul || (echo Push %COMMIT% first: the release is tagged there. & exit /b 1)
set ASSETS="%DIST%\AtherScreenshot-Setup-%VER%.exe" "%DIST%\AtherScreenshot-%VER%-portable.zip" "%DIST%\SHA256SUMS.txt" "%DIST%\latest.json"
gh release view v%VER% --repo %REPO% >nul 2>nul
if errorlevel 1 (
  gh release create v%VER% %ASSETS% --repo %REPO% --target %COMMIT% --title "Ather Screenshot %VER%" --notes-file INSTALL.md --latest || exit /b 1
) else (
  gh release upload v%VER% %ASSETS% --repo %REPO% --clobber || exit /b 1
)
if not exist downloads mkdir downloads
copy /y "%DIST%\latest.json" downloads\latest.json >nul || exit /b 1
echo Published v%VER% (build %BUILD%). Commit and push downloads\latest.json for copies that look there.
