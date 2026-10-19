@echo off
setlocal
REM Build using the same tool discovery as configure_release.bat.
set "REPO_ROOT=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if exist "%VCVARS%" call "%VCVARS%" >nul 2>&1
if not defined VCToolsInstallDir if exist "%VCVARS%" call "%VCVARS%" >nul 2>&1
where cmake >nul 2>&1
if errorlevel 1 if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
where cmake >nul 2>&1 || (echo CMake was not found on PATH.& exit /b 9009)
cd /d "%REPO_ROOT%"
cmake --build build/release
exit /b %ERRORLEVEL%
