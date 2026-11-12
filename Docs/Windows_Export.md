# Export a Windows game

Save the project, then choose **Game → Export Windows EXE**. The editor opens
`dist-windows/` with the project's named executable. In PowerShell, run the
same export from the folder containing the engine's `nf.exe`.

```powershell
.\nf.exe export --project MyGame\MyGame.nfproj
```

Share `MyGame\dist-windows\MyGame.exe`. It includes the project, assets and
shaders, uses the SANAD icon, and shows the engine logo at startup. The player
needs Windows 10/11 x64 and a Vulkan-capable driver. The export can embed up to
1 GiB of project data. Keep distributable assets in the project's mounted
folders.

The game prepares its data under `%LOCALAPPDATA%\SANAD\Games` on first launch.
Each game has a folder keyed by its project name and descriptor filename.
`Builds` contains the data for each export. `Saves`, `Crashes` and
`player.log` sit beside it. An export keeps the same save folder across
builds with the same project name and descriptor filename.

## Sign before publishing

Install a publicly trusted code-signing certificate and its key provider in
`Cert:\CurrentUser\My`, and install SignTool from the Windows SDK. Use the
certificate thumbprint and the RFC 3161 timestamp URL provided by your signing
provider.

```powershell
$game = 'MyGame\dist-windows\MyGame.exe'
.\Scripts\sign_release.ps1 -Path $game -CertThumbprint $thumbprint -TimestampUrl $timestampUrl
```

The script signs the completed EXE and verifies its signature and timestamp.
Sign every export you distribute using the same publisher identity. For an
engine release, `packaging\windows\build_installer.ps1` takes
`-ReleaseCertThumbprint` and `-TimestampUrl` to sign the staged engine binaries
and the finished installer.

A trusted signature identifies the publisher. SmartScreen can still warn
about a new download while its reputation builds. Microsoft Store distribution
avoids the SmartScreen download warning.
[Microsoft SmartScreen guidance](https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation) ·
[SignTool reference](https://learn.microsoft.com/windows/win32/seccrypto/signtool)

## التصدير والنشر

احفظ المشروع، ثم اختر **اللعبة ← تصدير اللعبة لويندوز**. شارك ملف EXE باسم
لعبتك من مجلد `dist-windows`. يشغّل اللاعب الملف مباشرة ويظهر شعار SANAD.
تشغّل المحرر نفسه من `SANADEditor.exe`.

للنشر العام، وقّع ملف EXE النهائي بشهادة توقيع كود عامة باسم الناشر باستخدام
الأمر أعلاه. يمكن أن يظهر تنبيه SmartScreen عند تنزيل إصدار جديد إلى أن
تتراكم سمعة الملف والناشر.
[شرح Microsoft](https://learn.microsoft.com/windows/apps/package-and-deploy/smartscreen-reputation)
