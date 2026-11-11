@echo off
rem Build Vaultik with MSVC + Ninja + vcpkg.
rem Usage: build.cmd [debug|release] [test]
setlocal

set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSDIR="
for /f "usebackq delims=" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio with the C++ tools was not found.
    exit /b 1
)
if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSDIR%\VC\vcpkg"
rem The developer environment sets VCPKG_ROOT to Visual Studio's own vcpkg; keep the one chosen.
set "VAULTIK_VCPKG_ROOT=%VCPKG_ROOT%"

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "VCPKG_ROOT=%VAULTIK_VCPKG_ROOT%"

set "PRESET=%~1"
if not defined PRESET set "PRESET=debug"

cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i "%~2"=="test" (
    ctest --preset %PRESET% || exit /b 1
)
