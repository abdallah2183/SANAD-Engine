@echo off
REM Compatibility shim kept for old bookmarks. Use Scripts\build.bat directly.
setlocal
call "%~dp0build.bat" Debug %*
exit /b %ERRORLEVEL%
