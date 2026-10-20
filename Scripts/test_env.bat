@echo on
rem ---------------------------------------------------------------------------
rem Scripts/test_env.bat — print the MSVC environment this machine would build
rem with, for diagnosing a toolchain problem by hand.
rem
rem The Visual Studio install used to be a hardcoded path
rem ("C:\Program Files\Microsoft Visual Studio\18\Community\...\vcvars64.bat"),
rem which exists on exactly one machine. It is discovered with vswhere now —
rem the same way Scripts/build.bat does it.
rem ---------------------------------------------------------------------------

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%i"

if not defined VS_INSTALL (
    echo No Visual Studio installation with the C++ toolset was found.
    exit /b 1
)

set "VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
    echo vcvars64.bat not found at "%VCVARS%".
    exit /b 1
)

echo VS install: %VS_INSTALL%
call "%VCVARS%"
echo VCVARS_RC=%ERRORLEVEL%
where cl
