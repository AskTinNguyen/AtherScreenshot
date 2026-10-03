@echo off
rem Builds build\AtherScreenshot.exe with MSVC. Usage: build.bat [debug]
setlocal
cd /d "%~dp0"

if defined VCINSTALLDIR goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
goto :build
:novs
echo Visual Studio with the C++ workload was not found.
exit /b 1

:build
if not exist build mkdir build

set CFLAGS=/nologo /std:c++20 /permissive- /utf-8 /EHsc /W4 /wd4100 /MP /DUNICODE /D_UNICODE
set LFLAGS=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\app.manifest
if /i "%1"=="debug" (
  set CFLAGS=%CFLAGS% /Od /Zi /MTd
  set LFLAGS=%LFLAGS% /DEBUG
) else (
  set CFLAGS=%CFLAGS% /O2 /GL /Gy /Gw /MT /DNDEBUG
  set LFLAGS=%LFLAGS% /LTCG /OPT:REF /OPT:ICF
)
set LIBS=user32.lib gdi32.lib shell32.lib ole32.lib windowscodecs.lib dwmapi.lib shcore.lib advapi32.lib comdlg32.lib windowsapp.lib gdiplus.lib mfplat.lib mfreadwrite.lib mfuuid.lib

rem res\app.ico is generated from the vector logo in src\logo.cpp. If it's missing, bootstrap it:
rem build once without resources, ask the exe to write the icon, then do the real build below.
if not exist res\app.ico (
  cl %CFLAGS% /Fo:build\ /Fd:build\ /Fe:build\AtherScreenshot.exe src\*.cpp /link %LFLAGS% %LIBS% || exit /b 1
  build\AtherScreenshot.exe --write-icon res\app.ico || exit /b 1
)
rc /nologo /fo build\app.res res\app.rc || exit /b 1

cl %CFLAGS% /Fo:build\ /Fd:build\ /Fe:build\AtherScreenshot.exe src\*.cpp build\app.res /link %LFLAGS% %LIBS%
if errorlevel 1 exit /b 1
rem Dev builds run in place: the marker makes the exe skip its install prompt.
if not exist build\AtherScreenshot.portable type nul > build\AtherScreenshot.portable
echo.
echo Built build\AtherScreenshot.exe
