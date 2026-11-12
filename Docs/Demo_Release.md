# Portable release / النسخة المحمولة

[Documentation](README.md) · [Changelog](../CHANGELOG.md) · [0.9.0 release notes](Releases/0.9.0.md)

The current public release is **SANAD 0.9.0**, for Windows 10/11 x64. Download the portable ZIP and checksum from [GitHub Releases](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.9.0).

## Package contents

| Component | Purpose |
|:---|:---|
| `SANADEditor.exe` | Project launcher and scene editor |
| `nf.exe` | Project creation, build, verify and run workflow |
| `NFPlayer.exe` / `NFGamePlayer.exe` | Standalone and editor-launched gameplay |
| `NFAssetCooker.exe` / `NFModelImporter.exe` | Asset processing |
| `Shaders/` | Runtime and editor shader binaries |
| `Templates/` | Starter projects and Blender integration |
| `Resources/fonts/` | Bundled Arabic font |
| `Games/TreeWorld/` | Sample project source |
| `Docs/` | User guides |

## Run and verify

Extract the complete archive and run `SANADEditor.exe`. Follow [Getting started](Getting_Started.md) or [البدء السريع](Getting_Started_Ar.md) to create a project. Keep shaders, templates and resources beside the executable.

The ZIP is portable; 0.9.0 does not include an installer. An experimental Android preview APK is described in [Android preview](Android_Preview_Ar.md). A Vulkan-capable driver is required. There is no Linux/macOS binary in this release.

## Signing and checksums

The six executables use the self-signed **SANAD Dev Build** certificate. This is a development signature, not a publicly trusted certificate. Compare the archive with `SHA256SUMS.txt` from the release before using it. See [Support](../SUPPORT.md) for policy or launch errors.

## بالعربية

فك الضغط كاملًا وشغّل المحرر. لا تنقل الملف التنفيذي وحده؛ الشيدرز والقوالب والخط والأدلة جزء من الحزمة. أنشئ مشروعًا في مجلد قابل للكتابة، ثم اتبع [الدرس العملي](Tutorial_Ar.md). للمشاركة، ابنِ مشروعك باستخدام `nf build` وشارك مجلد `dist` الناتج كاملًا.

The previous 0.2.0 demo description is historical; released-version records are indexed in [the changelog](../CHANGELOG.md).
