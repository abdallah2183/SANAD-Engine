@echo off
REM Configure the Release preset. Kept as a script because every build here needs
REM vcvars64 first and getting that quoting right inline in PowerShell is a
REM reliable way to waste an afternoon.
set "PATH=C:\Windows\System32;C:\Windows"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cmake --preset release
exit /b %ERRORLEVEL%
