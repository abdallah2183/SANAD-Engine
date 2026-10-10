@echo off
rem SANAD Editor launcher — double-click to open the editor.
rem Starts in the engine tree so content:// mounts resolve.
cd /d "%~dp0"
start "SANAD Editor" "build\DebugNinja\bin\SANADEditor.exe" --scene content://Scenes/Example.nfscene
