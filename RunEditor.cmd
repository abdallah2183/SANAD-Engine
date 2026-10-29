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
rem A browser-downloaded ZIP gets a Low integrity label, extraction inherits it,
rem and Windows then starts the editor at Low IL: it cannot write to Desktop,
rem Documents, or OneDrive, so "Create Project" fails with Access denied (5).
rem Raising the label needs a normal (Medium) process, which this script is when
rem double-clicked. Harmless when the files are already Medium. Relabel the
rem whole bin folder, not just the editor: it also holds nf.exe and the player.
for %%f in ("%SANAD_EDITOR%") do (
    icacls "%%~dpf*" /setintegritylevel Medium /C /Q >nul 2>&1
)
start "SANAD Editor" "%SANAD_EDITOR%" %*
