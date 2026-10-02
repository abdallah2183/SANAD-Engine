@echo off
set "PATH=C:\Windows\System32;C:\Windows"
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%~dp0"
cmake --build build/release
exit /b %ERRORLEVEL%
