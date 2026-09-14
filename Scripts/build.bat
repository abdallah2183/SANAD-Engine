@echo off
REM ---------------------------------------------------------------------------
REM NOVAForge Engine — Windows build helper (CMake + Ninja + MSVC)
REM
REM Usage:
REM   Scripts\build.bat [Debug|Release|RelWithDebInfo]
REM
REM Sets up the MSVC command-line environment directly instead of calling
REM vcvarsall.bat: vcvarsall shells out to reg.exe, which some sandboxed/CI
REM environments block, and it also misparses PATH entries that contain
REM parentheses. Everything it would have configured is set explicitly below.
REM ---------------------------------------------------------------------------

setlocal EnableDelayedExpansion

set BUILD_TYPE=%1
if "%BUILD_TYPE%"=="" set BUILD_TYPE=Debug

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo [NF] ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS_INSTALL=%%i

if "%VS_INSTALL%"=="" (
    echo [NF] ERROR: No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

REM --- Pick the newest MSVC toolset available -------------------------------
set MSVC_ROOT=
for /f "delims=" %%d in ('dir /b /o-n "%VS_INSTALL%\VC\Tools\MSVC" 2^>nul') do (
    if "!MSVC_ROOT!"=="" (
        if exist "%VS_INSTALL%\VC\Tools\MSVC\%%d\bin\Hostx64\x64\cl.exe" set "MSVC_ROOT=%VS_INSTALL%\VC\Tools\MSVC\%%d"
    )
)

REM --- Pick the newest Windows SDK -----------------------------------------
set SDK_ROOT=C:\Program Files (x86)\Windows Kits\10
set SDK_VERSION=
for /f "delims=" %%d in ('dir /b /o-n "%SDK_ROOT%\Include" 2^>nul') do (
    if "!SDK_VERSION!"=="" (
        if exist "%SDK_ROOT%\Include\%%d\um\windows.h" set "SDK_VERSION=%%d"
    )
)

if "%MSVC_ROOT%"=="" (
    echo [NF] ERROR: Could not locate an MSVC toolset under %VS_INSTALL%\VC\Tools\MSVC
    exit /b 1
)
if "%SDK_VERSION%"=="" (
    echo [NF] ERROR: Could not locate a Windows SDK under %SDK_ROOT%\Include
    exit /b 1
)

echo [NF] Visual Studio : %VS_INSTALL%
echo [NF] MSVC toolset  : %MSVC_ROOT%
echo [NF] Windows SDK   : %SDK_VERSION%
echo [NF] Build type    : %BUILD_TYPE%

REM --- Toolchain paths ------------------------------------------------------
set "CMAKE_BIN=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA_BIN=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;%SDK_ROOT%\bin\%SDK_VERSION%\x64;%CMAKE_BIN%;%NINJA_BIN%;%PATH%"
set "INCLUDE=%MSVC_ROOT%\include;%SDK_ROOT%\Include\%SDK_VERSION%\ucrt;%SDK_ROOT%\Include\%SDK_VERSION%\shared;%SDK_ROOT%\Include\%SDK_VERSION%\um;%SDK_ROOT%\Include\%SDK_VERSION%\winrt;%SDK_ROOT%\Include\%SDK_VERSION%\cppwinrt;"
set "LIB=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\Lib\%SDK_VERSION%\ucrt\x64;%SDK_ROOT%\Lib\%SDK_VERSION%\um\x64;"
set "LIBPATH=%MSVC_ROOT%\lib\x64;%SDK_ROOT%\UnionMetadata\%SDK_VERSION%;%SDK_ROOT%\References\%SDK_VERSION%;"

echo [NF] cmake: & where cmake
echo [NF] ninja: & where ninja

set BUILD_DIR=%~dp0..\build\%BUILD_TYPE%

echo [NF] Configuring...
cmake -S "%~dp0.." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 (
    echo [NF] ERROR: CMake configure failed.
    exit /b 1
)

echo [NF] Building...
cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 (
    echo [NF] ERROR: Build failed.
    exit /b 1
)

echo [NF] BUILD_SUCCEEDED: %BUILD_DIR%
endlocal
