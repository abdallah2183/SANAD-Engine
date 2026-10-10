@echo off
REM Build the Release preset. Same reason as configure_release.bat: vcvars64 first.
set "PATH=C:\Windows\System32;C:\Windows"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cmake --build build/release --target SANADEditor %*
exit /b %ERRORLEVEL%
