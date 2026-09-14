@echo on
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
echo VCVARS_RC=%ERRORLEVEL%
where cl
