# Documentation

**SANAD Engine 0.8.0** · [Repository](../README.md) · [العربية](../README.ar.md) · [Release notes](Releases/0.8.0.md)

Choose the guide for your task. Current user documentation, technical references and historical records are kept in separate sections.

## Start and use the editor

| Guide | What you will do | Language |
|:---|:---|:---|
| [Getting started](Getting_Started.md) | Download, create, build and run your first project | English |
| [البدء السريع](Getting_Started_Ar.md) | الانتقال من تنزيل المحرك إلى مشروع يعمل | العربية |
| [الدرس العملي](Tutorial_Ar.md) | إنشاء مشروع وتعديله وتشغيله وتغليفه | العربية |
| [Editor guide](Editor_Guide.md) | Navigate the panels, camera, input and console workflows | English / العربية |
| [Screenshots](Screenshots.md) | View current captures and their source | English / العربية |
| [Release package](Demo_Release.md) | Understand the portable download, contents and requirements | English / العربية |

## Build games and import assets

| Guide | Focus |
|:---|:---|
| [Android preview (development)](Android_Preview_Ar.md) | Build and test an experimental APK on Android |
| [Runtime UI (development)](Runtime_UI.md) | Lua panels, labels, buttons, progress bars and sliders in the standalone player |
| [Lua scripting](Lua_Scripting_Ar.md) | Script lifecycle and the engine's gameplay API |
| [AI agent workflow](AI_Game_Guide.md) | Executable native-project workflow for coding agents |
| [Asset import](Asset_Import.md) | Mesh formats, textures, materials and import tools |
| [Blender pipeline](Blender_Pipeline.md) | Exporting models, skeletons and clips |
| [Gamepad checklist](Gamepad_Checklist.md) | Input behavior and hardware checks |
| [TreeWorld](../Games/TreeWorld/README.md) | Sample project source and run instructions |

## Technical references

| Reference | Focus |
|:---|:---|
| [Architecture](Architecture.md) | Modules, runtime binding and dependency rules |
| [Scene systems](Reference/Scene_Systems.md) · [العربية](Reference/Scene_Systems.ar.md) | Authored scene components, AI and combat behavior |
| [GPU profiling](GPU_Profiling.md) | Vulkan timestamps and per-pass measurements |
| [Frame pacing](Frame_Pacing.md) | Average FPS, low FPS, jitter and CPU/GPU attribution |
| [Ballistics](Combat_Ballistics.md) | Projectiles, factions, targeting and impact events |
| [Damage](Combat_Damage.md) | Armor, hit zones, status effects and suppression |
| [Complete design](SANAD_Engine_Complete_Design.md) | Long-form architecture and stable design section numbers |

## Maintain the project

[Build and test](Build_and_Test.md) · [Contributing](../CONTRIBUTING.md) · [Support](../SUPPORT.md) · [Security](../SECURITY.md)

## Releases and history

- [Changelog](../CHANGELOG.md): released versions and links.
- [Roadmap](../ROADMAP.md): current baseline and areas for proposals.
- [Archive](Archive/README.md): milestone plans, dated reviews and development history. Archived statements are historical, not current support commitments.

## Documentation conventions

- Keep one canonical page for each subject; link to it instead of copying its instructions.
- Use paths relative to the page, and keep commands runnable from their stated directory.
- Record the version and context of screenshots. Store current captures once in `Docs/images/`.
- Keep old plans and reviews in the archive; do not present them as current user guides.
- Run `python Scripts/check_docs.py` before submitting documentation changes.
