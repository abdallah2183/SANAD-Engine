# Contributing

[Documentation](Docs/README.md) · [Build and test](Docs/Build_and_Test.md) · [Code of conduct](CODE_OF_CONDUCT.md)

Contributions to C++ systems, tools, samples, documentation and the Arabic editor are welcome. Keep changes focused so another contributor can reproduce and review them.

## Development workflow

1. Fork and clone the repository, then create a branch for one change.
2. Read the repository's [agent/project contract](AGENTS.md) and relevant module instructions.
3. Build with `build_nf.bat Release` or the appropriate Debug configuration.
4. Run the suites and checks relevant to the change; preserve failures and skips in your report.
5. Open a pull request explaining the problem, resulting behavior and verification.

```bat
build_nf.bat Release
build\Release\bin\EditorTests.exe
ctest --test-dir build\Release --output-on-failure
```

Full prerequisites and commands are in [Build and test](Docs/Build_and_Test.md).

## Engineering rules

- Use the native project/runtime workflow. A SANAD game is a `.nfproj` project, not a web application or another engine's project.
- Keep module boundaries intact: Assets must not depend on Rendering/RHI, and Rendering must not depend on ECS/Scene.
- Keep compiler warnings actionable; the build uses warnings as errors.
- For rendering changes, report Vulkan validation results and resource cleanup.
- Test changed behavior with a reproduction that would catch a regression.
- Keep authored source separate from `Cache/`, `dist/` and other generated output.

## Documentation and media

- Put user workflows in `Docs/`, release records in `Docs/Releases/` and historical plans/reviews in `Docs/Archive/`.
- Keep `README.md` and `README.ar.md` aligned in scope and navigation. Link to detailed references instead of copying them into the landing page.
- Use current, native screenshots, with version and capture context. Follow the [media policy](Docs/media/README.md); do not add development crops or repeated website copies.
- Run `python Scripts/check_docs.py` to validate local links and required assets.
- Do not rewrite historical release results as current verification.

## Pull request information

Describe the concrete trigger or problem, what changes for the user, and how you verified it. Include limitations, hardware-dependent skips and screenshots only when relevant. Do not claim that a file compiles or a test passes unless you observed it.

For a security issue, use [private reporting](SECURITY.md) rather than a public bug report.

## بالعربية

اختر تغييرًا محددًا، ابنِ المحرك، ثم اختبر السلوك الذي تغير. اشرح المشكلة والنتيجة والتحقق في طلب الدمج. لتحسين التوثيق، حافظ على صفحة رئيسية مختصرة ومراجع واضحة وصور حديثة دون تكرار. قواعد [السلوك](CODE_OF_CONDUCT.md) تنطبق على جميع المساهمات.
