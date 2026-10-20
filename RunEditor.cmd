@echo off
rem SANAD Editor launcher — double-click to open the editor.
rem
rem No --scene / --project on purpose: a cold start opens the project shell
rem (New game / Open project / Learn). Passing a scene here skipped that shell
rem and dropped a first-time user straight into an internal Example scene with
rem no obvious way to create their own project.
rem
rem Pick the newest build tree that exists, release first.
cd /d "%~dp0"
set "SANAD_EDITOR="
if exist "build\release\bin\SANADEditor.exe" set "SANAD_EDITOR=build\release\bin\SANADEditor.exe"
if not defined SANAD_EDITOR if exist "build\video\bin\SANADEditor.exe" set "SANAD_EDITOR=build\video\bin\SANADEditor.exe"
rem The staged package directory is named after the version it was built from
rem (dist\stage\SANAD-<version>), so a path with the version written into it
rem breaks the moment the version is bumped. Glob the stage directory and take
rem the newest one that actually holds the editor.
if not defined SANAD_EDITOR (
    for /f "usebackq delims=" %%d in (`dir /b /ad /o-d "dist\stage" 2^>nul`) do (
        if not defined SANAD_EDITOR if exist "dist\stage\%%d\SANADEditor.exe" set "SANAD_EDITOR=dist\stage\%%d\SANADEditor.exe"
    )
)
if not defined SANAD_EDITOR (
    echo SANADEditor.exe was not found.
    echo Build it first:  build_release.bat   or   cmake --build build\release
    pause
    exit /b 1
)
start "SANAD Editor" "%SANAD_EDITOR%"
