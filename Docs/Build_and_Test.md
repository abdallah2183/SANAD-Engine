# Build and test

[Documentation](README.md) · [Contributing](../CONTRIBUTING.md) · [0.8.0 verification](Releases/0.8.0.md#verification--التحقق)

All commands below run from the repository root on **Windows x64** unless stated otherwise.

## Prerequisites

| Tool | Requirement |
|:---|:---|
| Visual Studio | MSVC C++ toolchain with the Desktop development with C++ workload and a Windows SDK |
| CMake | 3.24 or newer |
| Ninja | Available to the build helper |
| Vulkan SDK | Headers, validation layers and `glslc` for shader compilation |
| Graphics driver | Vulkan support for native GPU checks |

The engine uses C++23. The build helper discovers Visual Studio using `vswhere` and configures the compiler environment. A downloaded portable release does not require this toolchain.

## Build

```bat
build_nf.bat Release
```

Expected binaries are in `build\Release\bin\`, including `SANADEditor.exe`, `nf.exe`, `NFPlayer.exe`, import/cook tools, samples and test suites. Debug builds use `build_nf.bat Debug` and `build\Debug\bin\`.

Manual CMake, from a configured MSVC developer terminal:

```bat
cmake --preset release
cmake --build --preset release --parallel
```

## Run tests

```bat
ctest --test-dir build\Release --output-on-failure
```

Run a focused suite, or pass a substring of a test name:

```bat
build\Release\bin\EditorTests.exe
build\Release\bin\EditorTests.exe editor_console
build\Release\bin\UITests.exe
```

The test filter is positional, not `--filter`. GPU checks need a Vulkan device. Hardware-dependent and opt-in cases report skips explicitly; preserve those details when reporting results.

## Static checks

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File Tests\check_ui_literals.ps1
python Scripts\check_docs.py
```

From Git Bash:

```bash
bash Scripts/check_layering.sh
```

## Native acceptance

```bat
build\Release\bin\SANADEditor.exe --frames 180 --headless --validation
```

Success means exit `0`, `automation=OK`, `Validation errors: 0` and `Alive RHI objects before shutdown: 0`. A bounded automation run exercises scene editing, undo, save/load, play and packaging without overwriting the opened scene.

For an unchanged visual capture:

```bat
build\Release\bin\SANADEditor.exe --frames 90 --no-automation --no-start-here --arabic --validation --screenshot editor.bmp
```

Use the [capture policy](media/README.md) before adding a screenshot to the repository.

## Package a release

After a successful Release build:

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File packaging\windows\build_installer.ps1 -SkipBuild -SkipInstaller
```

The version comes from `project(VERSION)` in `CMakeLists.txt`. The portable ZIP is written to `dist/`; build outputs and archives do not belong in Git. `-Sign` uses an existing local signing certificate; a self-signed certificate is a development signature. Installer builds require Inno Setup and omit `-SkipInstaller`.

Before publishing, extract the exact ZIP outside the checkout and use its CLI to create, build and run a project. Verify its editor too, record validation/leak results, and publish the SHA-256 of the uploaded archive. See [release records](../CHANGELOG.md).

## Restricted environments

Use a writable test scratch directory. If a sandbox blocks localhost sockets, rerun the network suite with the required local-network permissions; do not count an environment denial as a code pass. If Windows blocks an executable, follow [Support](../SUPPORT.md#windows-blocks-a-downloaded-executable) and record the launch failure separately from test failures.
