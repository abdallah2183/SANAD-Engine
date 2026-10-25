@echo off
REM ---------------------------------------------------------------------------
REM SANAD Engine — Windows build helper (CMake + Ninja + MSVC)
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

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
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
REM Compared as VERSIONS, not as text and not by date. "dir /o-n" orders by
REM modified date, and a lexicographic sort ranks "14.9.35207" above
REM "14.44.35207" (true as a string, false as a version) — so both of those
REM could hand the build an old toolset. CMake/Toolchain-MSVC.cmake applies
REM the same numeric rule; this is that rule, in batch.
set MSVC_ROOT=
set MSVC_VER=
for /f "delims=" %%d in ('dir /b "%VS_INSTALL%\VC\Tools\MSVC" 2^>nul') do (
    echo %%d| findstr /r /c:"^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$" >nul
    if not errorlevel 1 (
        if exist "%VS_INSTALL%\VC\Tools\MSVC\%%d\bin\Hostx64\x64\cl.exe" (
            call :version_newer "%%d" "!MSVC_VER!"
            if "!VERSION_NEWER!"=="1" (
                set "MSVC_VER=%%d"
                set "MSVC_ROOT=%VS_INSTALL%\VC\Tools\MSVC\%%d"
            )
        )
    )
)

REM --- Pick the newest Windows SDK -----------------------------------------
set "SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
set SDK_VERSION=
for /f "delims=" %%d in ('dir /b "%SDK_ROOT%\Include" 2^>nul') do (
    echo %%d| findstr /r /c:"^10\.[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*$" >nul
    if not errorlevel 1 (
        if exist "%SDK_ROOT%\Include\%%d\um\windows.h" (
            call :version_newer "%%d" "!SDK_VERSION!"
            if "!VERSION_NEWER!"=="1" set "SDK_VERSION=%%d"
        )
    )
)

if "%MSVC_ROOT%"=="" (
    echo [NF] ERROR: Could not locate an MSVC C++ toolset in the Visual Studio installation.
    exit /b 1
)
if "%SDK_VERSION%"=="" (
    echo [NF] ERROR: Could not locate a Windows 10/11 SDK installation.
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

set "BUILD_DIR=%~dp0..\build\%BUILD_TYPE%"

echo [NF] Configuring...
REM NF_BUILD_TOOLS is ON on purpose: this script is the documented build
REM entry point, and it promises the CLI (nf), the standalone player, the
REM asset cooker and the model importer. The CMake default is OFF -- a bare
REM "cmake -S . -B build" is a library-only build -- so a clean tree built
REM through this script produced an engine with NO CLI: the release package
REM then shipped without the tool that creates a game, and the AGENTS.md
REM instruction "build_nf.bat Release" did not build what it says it builds.
cmake -S "%~dp0.." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DNF_BUILD_TOOLS=ON
if errorlevel 1 (
    echo [NF] ERROR: CMake configure failed.
    exit /b 1
)

echo [NF] Building...
REM Capture the log and inspect it as well as ERRORLEVEL: some cmake/ninja
REM combinations have reported 0 after a failed link, which made this script
REM print BUILD_SUCCEEDED over a stale executable. A "FAILED:" line or
REM "ninja: build stopped" is authoritative.
set "BUILD_LOG=%BUILD_DIR%\build.log"
cmake --build "%BUILD_DIR%" --parallel > "%BUILD_LOG%" 2>&1
set "BUILD_RC=%ERRORLEVEL%"
type "%BUILD_LOG%"
if not "%BUILD_RC%"=="0" (
    echo [NF] ERROR: Build failed.
    exit /b 1
)
findstr /C:"FAILED:" /C:"ninja: build stopped" "%BUILD_LOG%" >nul
if not errorlevel 1 (
    echo [NF] ERROR: Build failed.
    exit /b 1
)

echo [NF] BUILD_SUCCEEDED: %BUILD_DIR%
endlocal
exit /b 0

REM ---------------------------------------------------------------------------
REM :version_newer <candidate> <current>
REM   Sets VERSION_NEWER to 1 when <candidate> is numerically newer than
REM   <current> (or <current> is empty), 0 otherwise. Dotted numeric parts are
REM   compared one at a time, so 14.44.35207 beats 14.9.35207.
REM ---------------------------------------------------------------------------
:version_newer
set "VERSION_NEWER=0"
if "%~2"=="" (
    set "VERSION_NEWER=1"
) else (
    call :version_split "%~1"
    set "VN_A=!VS_A!" & set "VN_B=!VS_B!" & set "VN_C=!VS_C!" & set "VN_D=!VS_D!"
    call :version_split "%~2"
    set "NEWER=0"
    if !VN_A! GTR !VS_A! set "NEWER=1"
    if !VN_A! EQU !VS_A! (
        if !VN_B! GTR !VS_B! set "NEWER=1"
        if !VN_B! EQU !VS_B! (
            if !VN_C! GTR !VS_C! set "NEWER=1"
            if !VN_C! EQU !VS_C! (
                if !VN_D! GTR !VS_D! set "NEWER=1"
            )
        )
    )
    set "VERSION_NEWER=!NEWER!"
)
goto :eof

REM ---------------------------------------------------------------------------
REM :version_split <version>
REM   Splits <version> on "." into VS_A..VS_D, defaulting missing parts to 0.
REM ---------------------------------------------------------------------------
:version_split
set "VS_A=0" & set "VS_B=0" & set "VS_C=0" & set "VS_D=0"
for /f "tokens=1-4 delims=." %%a in ("%~1") do (
    set "VS_A=%%a"
    set "VS_B=%%b"
    if not defined VS_B set "VS_B=0"
    set "VS_C=%%c"
    if not defined VS_C set "VS_C=0"
    set "VS_D=%%d"
    if not defined VS_D set "VS_D=0"
)
goto :eof
