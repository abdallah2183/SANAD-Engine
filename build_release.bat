@echo off
REM Compatibility wrapper. Prefer: build_nf.bat Release
setlocal
call "%~dp0build_nf.bat" Release %*
exit /b %ERRORLEVEL%
