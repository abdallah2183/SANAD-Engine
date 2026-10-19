@echo off
rem SANAD Editor launcher — double-click to open the editor.
rem Starts in the engine tree so content:// mounts resolve.
rem The working build tree is build\video (RelWithDebInfo + Ninja).
cd /d "%~dp0"
start "SANAD Editor" "build\video\bin\SANADEditor.exe" --scene content://Scenes/Example.nfscene
