@echo off
rem Builds build\winhttp.dll (x64, static CRT) with the Visual Studio 2022 Build Tools.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1

if not exist src\winhttp_proxy\forwards.h powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_forwards.ps1 || exit /b 1
if not exist build mkdir build

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE ^
   /Fobuild\ src\winhttp_proxy\proxy.cpp src\winhttp_proxy\lan.cpp src\winhttp_proxy\crash.cpp src\winhttp_proxy\pause.cpp ^
   /LD /link ws2_32.lib /OUT:build\winhttp.dll /IMPLIB:build\winhttp_proxy.lib || exit /b 1

echo.
echo OK: build\winhttp.dll
