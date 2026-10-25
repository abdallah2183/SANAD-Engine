@echo off
setlocal
rem SANAD Engine - repair the Low integrity label left by a downloaded ZIP.
rem
rem Windows marks files extracted from a browser-downloaded archive with a Low
rem mandatory label. Anything started from such a file runs at Low integrity,
rem and Windows then refuses writes to Desktop, Documents, and OneDrive. The
rem editor looks broken: Create Project fails with "Access is denied (code 5)".
rem
rem This raises the whole engine folder back to Medium. No administrator rights
rem are required, but it must run from a normal (Medium) process, so
rem double-click this file in Explorer. A process that is itself Low cannot
rem raise its own label.

cd /d "%~dp0"

rem A process that is itself Low cannot raise its own label; the calls below
rem would report success and change nothing. The label SID is locale-independent
rem (S-1-16-4096 = Low, S-1-16-8192 = Medium).
whoami /groups | findstr /c:"S-1-16-4096" >nul 2>&1
if not errorlevel 1 (
    echo.
    echo This window is itself running at Low integrity, so it cannot repair
    echo anything. Close it and double-click FixPermissions.cmd in Explorer.
    echo.
    pause
    exit /b 1
)

echo.
echo Repairing integrity labels in:
echo   %CD%
echo.
icacls "%CD%" /setintegritylevel "(OI)(CI)Medium" /T /C /Q
if errorlevel 1 (
    echo.
    echo icacls reported errors. Re-run this file by double-clicking it in
    echo Explorer. If Create Project still fails after that, the project folder
    echo itself - not the engine - is the one that is read-only.
    echo.
    pause
    exit /b 1
)

echo.
echo Done. Run RunEditor.cmd, or SANADEditor.exe, and Create Project will work.
echo.
pause
