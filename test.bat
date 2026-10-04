@echo off
rem Builds (unless --nobuild) and runs the unit tests: test.bat [--nobuild] [name filter]
setlocal
cd /d "%~dp0"
if /i "%1"=="--nobuild" (
  shift
) else (
  call "%~dp0build.bat" || exit /b 1
)
rem The exe is a GUI app: run it to completion with its output in a file. Tests use a temp support folder
rem (ATHER_SUPPORT_DIR), never %APPDATA%\AtherScreenshot.
start "" /wait /b "%~dp0build\AtherScreenshot.exe" --selftest %1 > "%~dp0build\selftest.log" 2>&1
set RESULT=%errorlevel%
type "%~dp0build\selftest.log"
exit /b %RESULT%
