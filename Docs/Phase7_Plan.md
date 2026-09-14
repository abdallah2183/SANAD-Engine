# Phase 7 — Project, Build, Standalone Run

**Status:** planned, not started
**Written:** 2026-09-13
**Predecessor:** Phase 6 (`a0668ea` — descriptor caching, GPU picking, headless acceptance), complete and verified
**Chosen direction:** close the shipping loop (Abdal's decision, 2026-09-13)

---

## 1. Why this phase

The design document's own phase-1 success criteria (§262) define a ten-step chain:

```text
Create project → Open editor → Create scene → Add entity → Add mesh → Add material
→ Play → Save → Build → Run outside editor
```

**Seven of those ten work today.** The last three — `Create project`, `Build`, `Run outside editor` —
do not exist in any form. Verified by search:

| Missing capability | Evidence |
|---|---|
| Project-file concept | No `.nfproj` / `ProjectFile` / `project.json` anywhere in `Engine/`, `Editor/`, `Tools/`, `Samples/` |
| Packaging / standalone build | No packaging code in `Tools/`, `Editor/src/`, `Engine/Runtime/src/` |
| Build tooling | `Tools/BuildTool/` contains only a `.gitkeep` placeholder |

**Why this is the right next phase, and not just a checkbox:**

1. **It is the gate Abdal wrote himself.** The design doc names these steps as the criteria for
   declaring the first phase done. They are the only ones outstanding.
2. **It needs no new subsystem.** Every piece already exists — the VFS, the registry, the cooker,
   `Runtime`, `Application`. This is plumbing, not research. Lowest-risk phase available.
3. **It changes how everything after it is verified.** Today every system is exercised only inside
   the editor or a test binary. Once a standalone build exists, Physics / Audio / Animation /
   Scripting each get validated in a real shipping context instead of in a vacuum.
4. **It forces the project-root abstraction that streaming will need.** World streaming (the
   design's deferred "from day one" principle) has to hang off a notion of "the content root and
   what is resident from it". That abstraction does not exist yet, and building it here is cheap.

**What this phase is not:** it does not add gameplay. A packaged game will still have no player
controller, no physics, no audio. Phase 7 ships the *pipe*; gameplay subsystems come after.

---

## 2. Current state — what the code actually does

This is the part the plan must be built against, so it is stated precisely.

### 2.1 The runtime discovers its project by walking up the filesystem

`Engine/Runtime/src/Application.cpp:37-67` builds the VFS itself:

```cpp
assets::VirtualFileSystem vfs;
std::filesystem::path project_root = std::filesystem::current_path();
for (...) {
    if (std::filesystem::exists(project_root / "Engine") &&
        std::filesystem::exists(project_root / "Content")) break;
    project_root = project_root.parent_path();
}
...
setup_mount("engine://",  project_root / "Engine");
setup_mount("project://", project_root);
setup_mount("content://", project_root / "Content");
setup_mount("cache://",   project_root / "Cache");
```

The walk-up looks for **`Engine/` + `Content/`** — the *engine source tree* layout. A shipped game
project has neither, so this cannot work outside the repo. `ApplicationConfig`
(`Application.hpp:9-18`) has no field for a project path or mounts, so a caller cannot override it.

### 2.2 The asset cooker has the same assumption

`Tools/AssetCooker/main.cpp:66-111` (`setup_vfs`) walks up looking for `CMakeLists.txt` + `Engine/`.
It also cooks **one asset per invocation** (`--input` / `--output`); there is no cook-everything
command.

### 2.3 Shader lookup is a hardcoded search of build trees

`Engine/Runtime/src/Runtime.cpp:93-128` (`resolve_shader_dir`) tries a compile-time define, then six
literal paths including `build/DebugNinja/Shaders/Basic3D` and `build/debug/Shaders/Basic3D`, then
`content://Shaders/Basic3D`, then falls back to a build path that may not exist.

A packaged game cannot rely on any of that. **This is the single most likely thing to break the
first standalone build**, and it is why shaders get an explicit mount in §3.1.

### 2.4 What is already good and should be reused

- `VirtualFileSystem` (`VirtualFileSystem.hpp:43-77`) — explicit `mount(logical, physical)` with
  path-traversal rejection. Exactly the right seam.
- `AssetRegistry` (`AssetRegistry.hpp`) — UUID keys, `save_to_physical` / `load_from_physical` for
  tools that have no VFS mounted, atomic save.
- `AssetCooker` — validate → copy → register, with fingerprint-based skip. The cook logic is correct;
  it only lacks a "cook all" driver and a project-aware root.
- `Application` — a clean runnable entry point; it just needs to accept a description instead of
  guessing one.
- The editor's `NFEditorCore` split (logic in a static lib, thin `main.cpp`) — the pattern the new
  CLI should copy so its logic is unit-testable.

---

## 3. Design

### 3.1 Project descriptor — `<Name>.nfproj`

**Format: line-based tolerant text**, matching `.nfreg`, `.nfmat` and `.nfscene`. The project has no
JSON parser and hand-rolls its own formats; adding a JSON dependency for this would be inconsistent
and unnecessary.

```text
# NOVAForge Project
version: 1
name: Demo
engine_version: 0.1
startup_scene: content://Scenes/Main.nfscene
title: Demo Game
window_width: 1280
window_height: 720

# Mounts. Paths are relative to this file's directory unless absolute.
mount: engine://  -> <engine-root>
mount: project:// -> .
mount: content:// -> Content
mount: cache://   -> Cache
mount: shaders:// -> Shaders
```

Rules:
- Unknown keys are ignored (forward compatibility), but a malformed `mount:` line is an error.
- `version` is required. A future `version` is rejected with a clear message rather than
  half-parsed — the same discipline `Scene` should have had.
- Four mounts have defaults relative to the project root: `project://` → `.`, `content://` →
  `Content`, `cache://` → `Cache`, `shaders://` → `Shaders`. An explicit declaration overrides the
  default. `engine://` has **no** default — it only means anything inside the engine source tree, so
  a project that needs it declares it.
- A relative mount path may not escape the project directory (`..` traversal is rejected). Absolute
  paths are allowed — `engine://` points at an install — but are never *derived* from a traversal.

**New `shaders://` mount.** This is what removes the `build/DebugNinja` hardcoding. In-repo, it
points at the compiled shader output; in a package, at the shipped `Shaders/` directory.

### 3.2 Runtime consumes the descriptor

`ApplicationConfig` gains an optional project path. Behaviour:

- **Project given** → mounts, startup scene, window size and title come from the descriptor.
- **No project** → today's walk-up behaviour, unchanged. This keeps every existing sample, test and
  the CI acceptance working exactly as they do now.

`Runtime::resolve_shader_dir()` gains the project's `shaders://` mount as its **first** candidate,
falling back to the current list so nothing regresses.

### 3.3 Cook-all

Extend `NFAssetCooker` with:

```
NFAssetCooker --all --registry <logical> [--content <logical>] [--project <file>]
```

Walks the content mount, cooks every supported extension (`.nfmesh`, `.spv`, `.nfmat`, `.nfscene`,
`.png`/`.jpg`/`.bmp`/`.tga`), updates the registry, and reports
`cooked / skipped / failed / total`. The existing single-asset mode stays — CI depends on it.

### 3.4 CLI — `nf` (replaces the empty `Tools/BuildTool`)

```
nf new <dir> --name <Name>     scaffold a project: .nfproj, Content/Scenes/Main.nfscene, .gitignore
nf cook  [--project <file>]    cook-all
nf build [--project <file>]    cook + package into dist/
nf run   [--project <file>]    build if stale, then launch the player
nf verify [--project <file>]   registry integrity report
```

**Logic lives in a static library, `main.cpp` stays thin** — the `NFEditorCore` pattern. Otherwise
none of this is unit-testable, and the project's whole culture is that behaviour is asserted.

### 3.5 Package layout — plain directories, no archive

```text
dist/
  Demo.nfproj
  NFPlayer.exe
  Content/            cooked, mirrored
  Cache/
  Shaders/Basic3D/    *.spv
  manifest.txt        relative path + fingerprint, for integrity checks
```

Deliberately **not** an archive format. A `.nfpkg` container is a later optimisation; plain files
are debuggable, diffable, and testable today. Shipping an archive now would add a format to
maintain for no user-visible benefit.

### 3.6 Standalone player — `NFPlayer`

A thin executable that reads a `.nfproj` (argument, or `*.nfproj` beside the exe), builds an
`ApplicationConfig` from it, and runs. **This is the "Run outside editor" step.**

---

## 4. Work breakdown

Ordered; each item is independently verifiable.

| # | Work | Primary files | Acceptance |
|---|---|---|---|
| **W1** | Project descriptor type + loader + `apply_mounts` | new `Engine/Assets/.../ProjectDescriptor.{hpp,cpp}` | `AssetTests`: valid parse, malformed mount rejected, missing required key rejected, future `version` rejected, traversal rejected |
| **W2** | Runtime consumes descriptor; `shaders://` first for shader lookup | `Runtime/Application.{hpp,cpp}`, `Runtime.cpp:93-128` | `RuntimeTests`: headless run from a temp project; existing suites unchanged and still green |
| **W3** | Cook-all mode | `Tools/AssetCooker/main.cpp` | `AssetTests`: cook-all over a temp tree; second run reports all skipped; a corrupt asset reports failed and non-zero exit |
| **W4** | `nf` CLI logic + thin `main.cpp` | new `Tools/BuildTool/` (+ lib), `Tools/CMakeLists.txt` | new `ToolTests`: `nf new` scaffolds a loadable project; `nf cook`/`verify` on it succeed |
| **W5** | Packaging + manifest | CLI lib | `nf build` produces `dist/`; manifest fingerprints match the cooked files |
| **W6** | `NFPlayer` standalone exe | new `Tools/Player/` or `Samples/` | `NFPlayer --project dist/Demo.nfproj --frames 60 --validation` → exit 0, 0 validation errors, geometry drawn, 0 leaks |
| **W7** | Editor integration | `Editor/src/` panels + `EditorApp` | acceptance adds: new project, open project, build, project name in title; still 0 validation errors / 0 leaks |
| **W8** | CI: end-to-end fresh-project job | `.github/workflows/ci.yml` | CI runs `nf new` → `nf build` → `NFPlayer` on a clean runner and fails on any error |

**Ordering rationale:** W1 before W2 (descriptor must exist before anything consumes it); W3 before
W5 (cannot package what cannot be cooked in bulk); W6 before W7 (prove the player works headlessly
before wiring a UI button to it).

### Progress

All eight work items are complete and verified. The phase acceptance in §5 runs green.

| # | Status | Notes |
|---|---|---|
| **W1** | ✅ **done** | `ProjectDescriptor.{hpp,cpp}` in `Engine/Assets`. 14 tests in `Tests/AssetTests/test_project_descriptor.cpp`. |
| **W2** | ✅ **done** | `RunConfigResolver.{hpp,cpp}` holds the precedence rules so they are assertable rather than buried in `Application::run()`. `resolve_shader_dir()` consults `shaders://` first. 6 tests. |
| **W3** | ✅ **done** | Cook logic moved into `Tools/ProjectTool` (`ProjectCooker`); `--all` added to `NFAssetCooker`. 18 tests. Also fixed: cook-all prunes entries whose source asset was deleted, and format strings now match the editor's import queue (`nfmesh-v1` / `img-v1`). |
| **W4** | ✅ **done** | `Tools/BuildTool` is the `nf` CLI (new/cook/build/run/verify); logic in the library, `main.cpp` thin. 6 tests. |
| **W5** | ✅ **done** | `ProjectPackager` writes a plain-directory package with a fingerprinted manifest. 8 tests. |
| **W6** | ✅ **done** | `Tools/Player` builds `NFPlayer`. **Verified with the engine's build shaders renamed away and the package relocated**: exit 0, `meshes=1`, 0 validation errors, 0 leaks. |
| **W7** | ✅ **done** | Editor takes `--project`, shows the project name, has a Build action, and its headless acceptance gained scaffold/build/package steps. |
| **W8** | ✅ **done** | CI job runs `nf new` → `nf build` → `nf verify` → the **packaged** player, with the build tree's shaders hidden. Run locally before committing. |

**Final state:** **263 passed / 0 failed / 1 skipped / 264** across eight suites (AssetTests 13 → 45,
RuntimeTests 13 → 20, new ToolTests 14). Every sample and the packaged player report 0 validation
errors and 0 leaked RHI objects.

**Two things this phase found that were not in the plan:**
- `Content/Meshes/corrupt.nfmesh` (21 bytes, from the initial commit, referenced by nothing) and
  `Content/Shaders/dummy.frag` (a text file containing the literal characters `\x03\x02...`, with a
  registry entry whose cooked path nothing produces) were both sitting in the shippable content
  tree. Moved to `Tests/fixtures/`; the stale shader registry entry was removed, so the engine's own
  registry now verifies clean. Reversible with `git mv` if that was intentional.
- `Application::run()` had no way to express "no scene": `scene_path` carried a non-empty default, so
  a project could not supply its own startup scene. The default is now empty, meaning "unspecified",
  with the old fallback applied explicitly. Same fix in the editor.


---

## 5. End-to-end acceptance

The phase is done when this runs green, from a clean checkout, on a machine with no GPU:

```bash
nf new /tmp/Demo --name Demo
nf build --project /tmp/Demo/Demo.nfproj
./dist/NFPlayer.exe --project dist/Demo.nfproj --frames 60 --validation
# expect: exit 0, 0 validation errors, 0 leaked RHI objects,
#         "Scene geometry drawn" (the Phase 6 assertion), non-zero meshes
```

Plus: the existing 212-test suite stays green, and the existing samples still exit 0 with 0
validation errors.

**This is the strongest proof the project has ever had** — not "the tests pass", but "a game was
created, built, packaged and run as a standalone program, from scratch, with nothing but the
engine's own tools."

---

## 6. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Refactoring `Application::run()` breaks existing samples / CI | Medium | Descriptor is **optional**; the walk-up path is preserved verbatim as the fallback. W2's acceptance explicitly requires the existing suites to stay green. |
| Shader resolution breaks in a package | **High** | Known and designed for: `shaders://` is added as the first candidate in W2, and the package ships shaders at that path. Test W6 with the build tree renamed away to prove the hardcoded paths are no longer load-bearing. |
| Scope creep into gameplay | Medium | §1's non-goals are binding. A packaged game has no player controller by design. |
| Cooking a whole project is slow / non-idempotent | Low | Fingerprint skip already exists; W3's acceptance requires a second run to report all-skipped. |
| `dist/` accidentally committed | Low | Add `dist/` to `.gitignore` in W5. |
| Descriptor format becomes a compatibility burden | Low | `version:` is mandatory and a future version is rejected loudly, not half-parsed. |

---

## 7. Explicit non-goals

Written down so they cannot quietly become scope.

- No archive / packed container format — plain directories on disk.
- No incremental dependency-graph cooking — fingerprint skip only.
- No cross-platform packaging — Windows only, matching the current RHI.
- No installer, code signing, or auto-update.
- No scripting or gameplay systems — the package has no gameplay to run.
- No editor polish (thumbnails, per-property override badges, VFS dialogs) — still deferred from
  Phase 5.

---

## 8. What this unblocks

- **Physics (Jolt)** becomes verifiable in a shipping context rather than only in the editor.
- **Audio, Animation, Scripting** likewise — each can be dropped into a real build and heard/seen.
- **World streaming** gets the content-root abstraction it needs to hang off, built here for a
  cheap reason instead of retrofitted later for an expensive one.
- **The first vertical slice** (§260) becomes possible, because there is finally somewhere to put it.

---

## 9. Carried-over debt (not this phase, but do not lose it)

Recorded so it is not forgotten. From `Docs/Full_Project_Review_2026-09-13.md` §4.4–§4.7:

1. **Layering inversions** — Assets→Rendering and Rendering→ECS/Scene. Currently two; every new
   subsystem risks adding more. Worth fixing before a third appears.
2. **The resource-identity rule is still unenforced.** Six handle types in `RHI.hpp:401-406` are
   dead code and the device returns raw pointers. The historical SIGSEGV was a direct consequence.
   Streaming and virtual texturing will make this worse.
3. **Custom containers are unused** — `nf::HashMap` has zero references. Either adopt or delete.
4. **Benchmark thresholds are unasserted** — no baseline has been measured.
