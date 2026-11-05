# Getting started

[Documentation](README.md) · [العربية](Getting_Started_Ar.md)

This guide uses the **0.8.0 Windows portable release**. Source-build instructions are in [Build and test](Build_and_Test.md).

## Requirements

- Windows 10 or 11, x64.
- A graphics driver that provides Vulkan support.
- A writable folder for your projects.

The portable package needs no separate MSVC runtime installation. Building the engine yourself also requires the development tools listed in the build guide.

## 1. Download and open

Download the ZIP and `SHA256SUMS.txt` from [the 0.8.0 release](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.8.0). Extract the ZIP, then run `SANADEditor.exe`.

The executable uses a self-signed development certificate. See [Support](../SUPPORT.md#windows-blocks-a-downloaded-executable) if Windows reports a publisher or policy warning.

## 2. Create a project

Choose **Create New Project**, select **ThirdPerson**, choose a name and writable location, then create it. The editor opens the new scene. Press **Play** to run it; close the game window to return to editing.

The starter is an example to edit, not a finished game. Other CLI templates include `Default`, `FPSStarter` and `Platformer2D`.

## 3. Make a change

Select an object in the scene hierarchy. Change its position in the Inspector, or use the transform tool in the viewport. Save with `Ctrl+S`. Use `Ctrl+Z` and `Ctrl+Y` to undo and redo.

See the [Editor guide](Editor_Guide.md) for camera navigation, input bindings and the console.

## 4. Package a standalone game

From the extracted engine folder:

```bat
nf.exe new MyGame --name MyGame --template ThirdPerson
nf.exe build --project MyGame\MyGame.nfproj
MyGame\dist\NFPlayer.exe --project MyGame\dist\MyGame.nfproj
```

For a project you already created, run only the `build` command with its `.nfproj` path. Share the complete generated `dist` folder, not just its executable.

## 5. Check it without a window

```bat
nf.exe run --project MyGame\MyGame.nfproj --frames 120 --headless --validation
```

A successful check exits with code `0` and reports `Validation errors: 0`.

## Project files

```text
MyGame/
  MyGame.nfproj              Project name, mounts and startup scene
  Content/Scenes/           Editable .nfscene files
  Content/Scripts/          Lua gameplay
  Content/Materials/        Material definitions
  Cache/                    Generated cooked assets
  dist/                     Generated standalone package
```

Edit the source project under `Content/`; rebuild to update `dist/`. See [asset import](Asset_Import.md), [Lua scripting](Lua_Scripting_Ar.md) and the [native AI-agent workflow](AI_Game_Guide.md) for the next steps.
