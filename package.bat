@echo off
rem Builds a release and packages it for the team.
rem   package.bat            -> dist\ with the setup exe, a portable zip, checksums
rem   package.bat publish    -> same, then creates a GitHub Release with gh (needs a git repo + remote)
rem   package.bat downloads  -> same, then puts the Windows files and latest.json into downloads\ (replacing the
rem                             previous Windows version). Commit and push, and running copies offer the update.
rem Set UPDATE_NOTES to one line about what's new; the app shows it when it offers the update.
rem Optional code signing (removes the SmartScreen "unknown publisher" warning):
rem   set SIGN_PFX=path\to\cert.pfx & set SIGN_PASSWORD=...   before running.
setlocal
cd /d "%~dp0"

call "%~dp0build.bat" || exit /b 1

for /f "tokens=3" %%v in ('findstr /c:"#define ATHER_VERSION_STR" src\version.h') do set VER=%%~v
if not defined VER (
  echo Could not read the version from src\version.h
  exit /b 1
)

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

rem 4) What running copies read to find this version (src\updater.cpp). It points at the setup exe in downloads\.
powershell -NoProfile -Command "$f = Get-Item '%DIST%\AtherScreenshot-Setup-%VER%.exe'; $w = [ordered]@{ version = '%VER%'; url = 'https://github.com/AskTinNguyen/AtherScreenshot/raw/main/downloads/' + $f.Name; sha256 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash.ToLower(); size = $f.Length; notes = [string]$env:UPDATE_NOTES }; [IO.File]::WriteAllText((Join-Path (Resolve-Path '%DIST%') 'latest.json'), (ConvertTo-Json @{ windows = $w } -Depth 3))" || exit /b 1

echo.
echo Packaged Ather Screenshot %VER%:
dir /b %DIST%

if /i "%1"=="downloads" (
  if not exist downloads mkdir downloads
  del /q downloads\AtherScreenshot-Setup-*.exe downloads\AtherScreenshot-*-portable.zip 2>nul
  copy /y "%DIST%\AtherScreenshot-Setup-%VER%.exe" downloads\ >nul || exit /b 1
  copy /y "%DIST%\AtherScreenshot-%VER%-portable.zip" downloads\ >nul || exit /b 1
  copy /y "%DIST%\SHA256SUMS.txt" downloads\SHA256SUMS-Windows.txt >nul || exit /b 1
  copy /y "%DIST%\latest.json" downloads\latest.json >nul || exit /b 1
  echo Copied to downloads\. Commit and push to publish %VER%.
  exit /b 0
)

if /i not "%1"=="publish" exit /b 0
where gh >nul 2>nul || (echo gh CLI not found & exit /b 1)
gh release create v%VER% "%DIST%\AtherScreenshot-Setup-%VER%.exe" "%DIST%\AtherScreenshot-%VER%-portable.zip" "%DIST%\SHA256SUMS.txt" --title "Ather Screenshot %VER%" --notes-file INSTALL.md
