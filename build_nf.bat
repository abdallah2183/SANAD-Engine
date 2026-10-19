@echo off
REM ---------------------------------------------------------------------------
REM SANAD Engine - portable Windows build entry point.
REM
REM Usage (from anywhere):
REM   build_nf.bat                 -> Debug build in build\Debug
REM   build_nf.bat Release         -> Release build in build\Release
REM
REM The real work lives in Scripts\build.bat: it discovers Visual Studio with
REM vswhere, sets up MSVC + the Windows SDK + CMake + Ninja, configures the
REM build directory, and builds every target. No absolute paths are baked in.
REM ---------------------------------------------------------------------------
setlocal
call "%~dp0Scripts\build.bat" %*
exit /b %ERRORLEVEL%
