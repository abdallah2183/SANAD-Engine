@echo off
REM Compatibility wrapper. build_nf.bat configures CMake with the same Release preset.
setlocal
call "%~dp0build_nf.bat" Release %*
exit /b %ERRORLEVEL%
