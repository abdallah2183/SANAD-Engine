# package_beta.ps1 — assemble a self-contained beta folder on the Desktop.
#
# "Self-contained" is a claim, so here is exactly what it means and what it does
# NOT mean, because getting this wrong is how a "just unzip and run" build turns
# into a bug report about a black screen:
#
#   INCLUDED: the editor, the game player Play launches, the compiled SPIR-V
#             shaders, the project template, and the docs the launcher links to.
#   NOT INCLUDED: the Vulkan loader. It ships with the GPU driver, not with an
#             application; bundling a copy is how a project ends up loading a
#             loader older than the driver it is supposed to use. The launcher
#             says so plainly if it is missing, which is the honest behaviour.
#   NOT INCLUDED: a runtimes folder. The MSVC runtime is linked statically
#             (MSVC_RUNTIME_LIBRARY MultiThreaded), which is why the exe is
#             ~5 MB instead of ~20 MB and why "copy the exe" is close to enough.
#
# Run from the repo root:  powershell -File package_beta.ps1
$ErrorActionPreference = 'Stop'

$repo    = $PSScriptRoot
$version = '0.2.0-beta.1'
$build   = Join-Path $repo 'build\release'

# The REAL Desktop, not $env:USERPROFILE\Desktop. OneDrive redirects it, and on
# this machine it IS redirected - so the naive path writes to a folder the user
# never opens. This is the same trap documented at real_documents_dir() in
# Editor/src/ProjectLauncher.cpp ("NOT USERPROFILE\Documents: OneDrive redirects
# it, and on this machine it is redirected"), and the first run of this script
# fell into it and staged the whole beta somewhere invisible.
$desktop = [Environment]::GetFolderPath('Desktop')
$stage   = Join-Path $desktop "NOVAForge-$version"

if (-not (Test-Path $build)) {
    throw "No Release build at $build. Run: cmd /c configure_release.bat && cmd /c build_release.bat"
}

Write-Host "NOVAForge $version -> $stage`n"

# --- Fresh stage -------------------------------------------------------------
# Removed and recreated, never merged into: a stale exe or shader from a previous
# beta sitting next to the new one is the kind of thing that makes a build look
# broken for reasons that have nothing to do with the code.
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Shaders') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Templates') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Docs') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Resources\fonts') | Out-Null

# --- Binaries ----------------------------------------------------------------
$bins = @(
    @{ src = 'NOVAForgeEditor.exe'; why = 'the editor' },
    @{ src = 'NFGamePlayer.exe';  why = 'what the editor Play button launches' }
)
foreach ($b in $bins) {
    $p = Join-Path $build "bin\$($b.src)"
    if (-not (Test-Path $p)) { throw "Missing $($b.src) - build the release first." }
    Copy-Item $p $stage
    Write-Host ("  {0,-22} {1,8:N1} MB  ({2})" -f $b.src,
                ((Get-Item $p).Length / 1MB), $b.why)
}

# --- Shaders -----------------------------------------------------------------
# The editor bakes an ABSOLUTE path to the build tree's shader dir, which exists
# on this machine but will not on anyone else's. Shipping the .spv next to the
# exe is what makes the folder portable; resolve_imgui_shader_dir() and the
# Basic3D loader both walk "Shaders/" relative to the module as a fallback.
$shaderCount = 0
foreach ($d in @('EditorImGui', 'Basic3D')) {
    $src = Join-Path $build "Shaders\$d"
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $stage "Shaders\$d"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Get-ChildItem $src -Filter *.spv | ForEach-Object {
        Copy-Item $_.FullName $dst
        $script:shaderCount++
    }
}
Write-Host ("  Shaders/                {0,8} .spv  (EditorImGui + Basic3D)" -f $shaderCount)

# --- Project template --------------------------------------------------------
# "New project" scaffolds from this. Without it the button creates a folder with
# no .nfproj and the launcher then refuses to open it, which reads as a broken
# editor rather than a missing file.
$tpl = Join-Path $repo 'Templates\Default'
if (Test-Path $tpl) {
    Copy-Item $tpl\* (Join-Path $stage 'Templates') -Recurse -Force
    $n = (Get-ChildItem (Join-Path $stage 'Templates') -Recurse -File).Count
    Write-Host ("  Templates/              {0,8} files (New project scaffolds from here)" -f $n)
} else {
    Write-Warning "No Templates/Default - the New project button will have nothing to copy."
}

# --- Docs the launcher links to, AND the card thumbnails ---------------------
# Two different needs in one folder, which is why the first beta shipped the .md
# files and still showed grey cards:
#
#   Docs/*.md      the launcher OPENS these by name (Learn/Help pages). A missing
#                  one produces a status-line message.
#   Docs/images/   the launcher LOADS these as the project-card thumbnails at
#                  startup (shot_game.png, shot_vehicle.png, shot_editor_en.png,
#                  shot_editor_ar.png), walking up from the executable looking for
#                  Docs/images/. Ship the folder without it and every card falls
#                  back to a grey gradient - the launcher looks broken in a way
#                  that has nothing to do with the code, because the code is fine
#                  and the file simply is not there.
$docNames = @('ROADMAP.md', 'Blender_Pipeline.md', 'Tutorial_Ar.md')
foreach ($d in $docNames) {
    $p = Join-Path $repo "Docs\$d"
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'Docs') }
}
# The four the launcher actually asks for. Asserted, not assumed: a launcher that
# cannot find a thumbnail draws a grey box, and that reads as "the beta is broken"
# rather than "a file was left out of the package".
$needThumbs = @('shot_game.png', 'shot_vehicle.png', 'shot_editor_en.png', 'shot_editor_ar.png')
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Docs\images') | Out-Null
$missing = @()
foreach ($t in $needThumbs) {
    $p = Join-Path $repo "Docs\images\$t"
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'Docs\images') } else { $missing += $t }
}
if ($missing.Count -gt 0) {
    throw "Docs/images is missing: $($missing -join ', '). The launcher would draw grey placeholder cards."
}
$n = (Get-ChildItem (Join-Path $stage 'Docs') -File -ErrorAction SilentlyContinue).Count
$t = (Get-ChildItem (Join-Path $stage 'Docs\images') -File).Count
Write-Host ("  Docs/                   {0,8} files + images/ ({1} thumbnails, REQUIRED)" -f $n, $t)

# --- Resources/fonts ---------------------------------------------------------
# THE ONE THAT BIT. Resources/fonts/Amiri-Regular.ttf is not a project asset and
# not a shader: it is the editor's Arabic companion font, merged into the ImGui
# atlas at startup. The base font (Segoe UI) is loaded with NO glyph ranges, so
# ImGui rasterises Basic Latin only - meaning this file is the ONLY source of
# Arabic codepoints in the editor. It was missing from the first staged beta, and
# the editor rendered every Arabic label as tofu diamonds with no error, because
# find_bundled_font() walks up from the executable looking for a
# Resources/fonts/ that the package had never contained.
#
# So this is a HARD requirement, not a nice-to-have: if the copy fails, the
# package is not shippable and the script says so rather than writing a folder
# that looks fine and opens to diamonds.
$fonts = @('Amiri-Regular.ttf')
$fontOk = $true
foreach ($f in $fonts) {
    $src = Join-Path $repo "Resources\fonts\$f"
    if (-not (Test-Path $src)) { $fontOk = $false; continue }
    Copy-Item $src (Join-Path $stage 'Resources\fonts') -Force
}
if (-not $fontOk) {
    throw "Resources/fonts is missing Amiri-Regular.ttf. The editor's Arabic UI cannot render without it."
}
$fcount = (Get-ChildItem (Join-Path $stage 'Resources\fonts') -File).Count
Write-Host ("  Resources/fonts/        {0,8} files (REQUIRED - Arabic UI renders as tofu without it)" -f $fcount)

# --- The logo ----------------------------------------------------------------
# A copy outside the repo, because "here is the icon, here is the exe that has
# it" is the whole point of shipping one.
Copy-Item (Join-Path $repo 'Editor\resources\NOVAForge.png') $stage -Force
Write-Host "  NOVAForge.png                  the engine icon"

# --- README ------------------------------------------------------------------
$readme = @"
NOVAForge Engine $version
=========================

WHAT THIS IS
    The editor, a Release build (optimised, no debug asserts). The build that
    produced it passed its own test suite in Debug; see the note at the bottom
    about Release.

RUNNING IT
    Double-click NOVAForgeEditor.exe. That opens the project launcher, not the
    editor: pick or create a project and the editor opens on it.

    A project needs a .nfproj file. "New project" creates one for you from
    Templates/.

WHAT IS IN HERE
    NOVAForgeEditor.exe    the editor
    NFGamePlayer.exe       what the editor's Play button launches
    Shaders/               compiled SPIR-V, loaded at startup
    Templates/             what "New project" copies
    Docs/                  what the launcher's Learn and Help pages open
    NOVAForge.png          the engine icon

WHAT IT NEEDS FROM YOUR MACHINE
    * A Vulkan-capable GPU driver. The loader (vulkan-1.dll) ships with the
      driver, NOT with this folder, and is deliberately not bundled: shipping a
      loader older than the driver is a well-known way to get a black screen.
      The launcher tells you plainly if it cannot find one.
    * Nothing else. The MSVC runtime is linked statically, which is why the exe
      is ~5 MB rather than ~20 MB and why there is no redistributable folder.

WHERE THE VERSION NUMBER COMES FROM
    One place: project(VERSION) and NF_VERSION_SUFFIX in the root CMakeLists.txt.
    It reaches this README's version, the exe's Windows properties
    (right-click > Properties > Details) and the launcher's sidebar from the
    same two variables. Nothing keeps a second copy.

NEW IN THIS BETA
    * Full transform hierarchy. Rotation and scale now propagate through
      parents and reach the renderer. Previously a rotated object drew
      UNROTATED and a scaled object drew at 1x1 - the Inspector would show a
      scale of 40 while the viewport showed a one-metre cube, and no amount of
      dragging the gizmo changed the picture. Also: a child's offset is now
      expressed in its parent's space, so turning a parent turns its children
      with it.
    * Asset delete and rename from the FileSystem dock, in both list and grid
      views, with a red confirm for anything destructive and a refusal to
      delete a mount root.
    * Frame Selection / Frame All. The viewport orbits a pivot, not the world
      origin, so content away from the origin is reachable at all. Keys: Home
      and Shift+Home.
    * Godot-style FileSystem dock: FileSystem/History tabs, back/forward/up, a
      res:// breadcrumb, favourites, and list and grid views.
    * A drawn application icon, replacing the default blank glyph.
"@
$readme | Set-Content -Path (Join-Path $stage 'README.txt') -Encoding UTF8
Write-Host "  README.txt"

# --- Report ------------------------------------------------------------------
$total = (Get-ChildItem $stage -Recurse -File | Measure-Object -Property Length -Sum)
Write-Host ""
Write-Host ("staged {0} files, {1:N1} MB total" -f $total.Count, ($total.Sum / 1MB))
Write-Host ""
Write-Host $stage
