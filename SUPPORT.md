# Support / الدعم

[Documentation](Docs/README.md) · [Getting started](Docs/Getting_Started.md) · [Issue tracker](https://github.com/abdallah2183/SANAD-Engine/issues)

## Before opening an issue

Try the current [0.8.0 portable release](https://github.com/abdallah2183/SANAD-Engine/releases/tag/v0.8.0) or record the exact source commit you built. Describe the smallest project or sequence that reproduces the problem.

Include:

- Engine version or commit, and whether you used a download or a source build.
- Windows version, GPU model and graphics driver.
- The command or editor action, expected result and actual result.
- Relevant console output, including validation errors and exit code.
- A screenshot only when it clarifies the problem; remove personal paths or private content first.

## Common cases

| Symptom | First check |
|:---|:---|
| Project creation fails | Choose a writable folder and copy the reported path/error code |
| Models are missing | Check the logical mount, asset registry and cooker/import log |
| Edit controls are disabled | Stop play/simulation before structural edits |
| A packaged game cannot find files | Run the complete generated `dist` folder and use its packaged `.nfproj` |
| Build cannot find MSVC or shaders | Follow [Build and test](Docs/Build_and_Test.md) and verify the C++ workload/Vulkan SDK |
| A test reports skipped | Read its reason; some cases require optional hardware or an opt-in benchmark |

## Windows blocks a downloaded executable

The portable executables have a self-signed **SANAD Dev Build** certificate. It is not a publicly trusted certificate. Check the download's SHA-256 against `SHA256SUMS.txt` on the release page and inspect the signing/policy message.

Do not disable Windows security protections. If your device policy does not permit the download, use a permitted source-build/signing workflow or ask the device administrator. Report a launch-policy denial separately from a test that started and failed.

## Security reports

Report exploitable vulnerabilities privately using [SECURITY.md](SECURITY.md). Do not attach a working exploit to a public support issue.

## بالعربية

عند الإبلاغ، اذكر الإصدار ونسخة Windows ونوع البطاقة والتعريف، وخطوات تكرار المشكلة ورسالة الطرفية. استخدم مشروعًا صغيرًا لإعادة المشكلة إن أمكن. لا ترفع مفاتيح أو بيانات شخصية أو ملفات خاصة مع السجل. لمشكلات التوقيع، تحقق من البصمة ورسالة Windows دون تعطيل الحماية.
