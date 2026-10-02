@echo off
rem NOVAForge Editor launcher — double-click to open the editor.
rem Starts in the engine tree so content:// mounts resolve.
cd /d "%~dp0"
start "NOVAForge Editor" "build\DebugNinja\bin\NOVAForgeEditor.exe" --scene content://Scenes/Example.nfscene
