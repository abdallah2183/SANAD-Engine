# build_installer.ps1 - stage the release, zip a portable, compile the Setup EXE.
#
# Windows-only by design: the installer targets Windows 10+ x64 (see
# packaging/windows/SANAD.iss: ArchitecturesAllowed=x64compatible,
# MinVersion=10.0). Run from the repo root:
#   powershell -ExecutionPolicy Bypass -File packaging/windows/build_installer.ps1
# Switches:
#   -SkipBuild        do not (re)build; fail if build/release/bin exes are missing
#   -SkipInstaller    stage + zip only, do not invoke Inno Setup
#   -SkipPortable     stage + installer only, no portable zip
#   -StageOnly        stage only (used to inspect the folder without artifacts)

param(
    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$SkipPortable,
    [switch]$StageOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)  # packaging/windows -> repo root
if (-not (Test-Path (Join-Path $repo 'CMakeLists.txt'))) {
    # Fallback when invoked from the repo root with a relative path.
    $repo = (Get-Location).Path
}
Write-Host "repo: $repo"

# --- Version: single source of truth is CMakeLists.txt (project VERSION + NF_VERSION_SUFFIX).
$cmake = Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw
$m = [regex]::Match($cmake, 'project\s*\(\s*SANAD\s+VERSION\s+(\d+)\.(\d+)\.(\d+)')
if (-not $m.Success) { throw 'Could not parse project(VERSION) from CMakeLists.txt' }
$verNum = "$($m.Groups[1].Value).$($m.Groups[2].Value).$($m.Groups[3].Value)"
$s = [regex]::Match($cmake, 'set\s*\(\s*NF_VERSION_SUFFIX\s+"([^"]*)"')
$suffix = if ($s.Success) { $s.Groups[1].Value } else { '' }
$version = if ($suffix -eq '') { $verNum } else { "$verNum-$suffix" }
$versionInfo = "$($m.Groups[1].Value).$($m.Groups[2].Value).$($m.Groups[3].Value).0"
Write-Host "version: $version (VersionInfo $versionInfo)"

$build = Join-Path $repo 'build\release'
$dist = Join-Path $repo 'dist'
$stage = Join-Path $dist "stage\SANAD-$version"
$setupName = "SANAD-$version-Windows-x64-Setup.exe"
$zipName = "SANAD-$version-Windows-x64-Portable.zip"

# --- Build (unless skipped) ---------------------------------------------------
$needBins = @('SANADEditor.exe', 'NFGamePlayer.exe')
$missing = @($needBins | Where-Object { -not (Test-Path (Join-Path $build "bin\$_")) })
if ($missing.Count -gt 0 -and -not $SkipBuild) {
    Write-Host "Release binaries missing ($($missing -join ', ')) - building Release now (this takes a while)..."
    $cfg = Join-Path $repo 'configure_release.bat'
    $bld = Join-Path $repo 'build_release_all.bat'
    if (-not (Test-Path $build)) {
        Write-Host '> configure_release.bat'
        cmd /c "`"$cfg`"" | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "configure_release.bat failed ($LASTEXITCODE)" }
    }
    Write-Host '> build_release_all.bat'
    cmd /c "`"$bld`"" | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "build_release_all.bat failed ($LASTEXITCODE)" }
}
$missing = @($needBins | Where-Object { -not (Test-Path (Join-Path $build "bin\$_")) })
if ($missing.Count -gt 0) { throw "Missing binaries after build: $($missing -join ', ') (expected in $build\bin)" }

# --- Fresh stage (never merged into: stale exe/shader next to the new one
# --- reads as a broken build for reasons that have nothing to do with code).
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($d in @('Shaders', 'Templates', 'Docs', 'Resources\fonts')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $d) | Out-Null
}
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'Docs\images') | Out-Null

# --- Binaries ----------------------------------------------------------------
foreach ($b in $needBins) {
    $p = Join-Path $build "bin\$b"
    Copy-Item $p $stage
    Write-Host ("  {0,-22} {1,8:N1} MB" -f $b, ((Get-Item $p).Length / 1MB))
}
# The CLI + import/cook tools ship too: "make a game" needs `nf build`, and a
# demo without the asset cooker cannot bring a model into the project.
foreach ($b in @('nf.exe', 'NFAssetCooker.exe', 'NFModelImporter.exe')) {
    $p = Join-Path $build "bin\$b"
    if (Test-Path $p) {
        Copy-Item $p $stage
        Write-Host ("  {0,-22} {1,8:N1} MB" -f $b, ((Get-Item $p).Length / 1MB))
    }
}

# --- Shaders: ship EditorImGui + Basic3D next to the exe ----------------------
# The editor bakes an ABSOLUTE build-tree shader path; the portable folder works
# because resolve_imgui_shader_dir() and the Basic3D loader fall back to
# "Shaders/" relative to the module.
$shaderCount = 0
foreach ($d in @('EditorImGui', 'Basic3D')) {
    $src = Join-Path $build "Shaders\$d"
    if (-not (Test-Path $src)) { continue }
    $dst = Join-Path $stage "Shaders\$d"
    New-Item -ItemType Directory -Force -Path $dst | Out-Null
    Get-ChildItem $src -Filter *.spv | ForEach-Object { Copy-Item $_.FullName $dst; $script:shaderCount++ }
}
Write-Host ("  Shaders/                {0,8} .spv  (EditorImGui + Basic3D)" -f $shaderCount)
if ($shaderCount -eq 0) { throw 'No shaders staged - build the release first.' }

# --- Project templates --------------------------------------------------------
# EVERY template folder, not the contents of Default: the launcher resolves
# <exe>/Templates/<Name> and the gallery offers ThirdPerson, FPSStarter and
# Platformer2D. The old layout put Default's files directly under Templates/,
# so on a machine without the developer's build tree every card failed with
# "template not available" — the packaged New Project could not create anything.
$tplRoot = Join-Path $repo 'Templates'
if (-not (Test-Path (Join-Path $tplRoot 'Default'))) {
    throw 'Templates/Default is missing - "New project" would have nothing to copy.'
}
Get-ChildItem $tplRoot -Directory | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $stage 'Templates') -Recurse -Force
}
$n = (Get-ChildItem (Join-Path $stage 'Templates') -Recurse -File).Count
$tplNames = (Get-ChildItem (Join-Path $stage 'Templates') -Directory | Select-Object -ExpandProperty Name) -join ', '
Write-Host ("  Templates/              {0,8} files  ({1})" -f $n, $tplNames)

# --- Docs: the .md the launcher OPENS + the images it LOADS as card thumbnails
$docsDst = Join-Path $stage 'Docs'
foreach ($src in @('Docs\Getting_Started_Ar.md', 'Docs\Tutorial_Ar.md', 'Docs\Lua_Scripting_Ar.md',
                   'Docs\Asset_Import.md', 'Docs\Blender_Pipeline.md')) {
    $p = Join-Path $repo $src
    if (Test-Path $p) { Copy-Item $p $docsDst -Force }
}
$p = Join-Path $repo 'ROADMAP.md'
if (Test-Path $p) { Copy-Item $p (Join-Path $docsDst 'ROADMAP.md') -Force }
$p = Join-Path $repo 'README.md'
if (Test-Path $p) { Copy-Item $p (Join-Path $docsDst 'README-repo.md') -Force }
$needThumbs = @('shot_game.png', 'shot_vehicle.png', 'shot_editor_en.png', 'shot_editor_ar.png')
$missing = @()
foreach ($t in $needThumbs) {
    $p = Join-Path $repo "Docs\images\$t"
    if (Test-Path $p) { Copy-Item $p (Join-Path $stage 'Docs\images') } else { $missing += $t }
}
if ($missing.Count -gt 0) { throw "Docs/images is missing: $($missing -join ', '). The launcher would draw grey placeholder cards." }
Write-Host ("  Docs/                   images/ ({0} thumbnails, REQUIRED)" -f $needThumbs.Count)

# --- Arabic font (REQUIRED: without it every Arabic label renders as tofu) ----
$fontSrc = Join-Path $repo 'Resources\fonts\Amiri-Regular.ttf'
if (-not (Test-Path $fontSrc)) { throw 'Resources/fonts/Amiri-Regular.ttf is missing. The Arabic UI cannot render without it.' }
Copy-Item $fontSrc (Join-Path $stage 'Resources\fonts') -Force
Write-Host '  Resources/fonts/    Amiri-Regular.ttf (REQUIRED)'

# --- Icon + README ------------------------------------------------------------
Copy-Item (Join-Path $repo 'Editor\resources\SANAD.png') $stage -Force
$readme = @"
SANAD Engine $version (Windows x64) - demo build
================================================
WHAT THIS IS
    The SANAD editor and tools, a Release build (optimised, no debug asserts).
    Arabic-first UI (full RTL), English available from Settings.

RUNNING IT (portable zip)
    Extract the zip anywhere and double-click SANADEditor.exe.

YOUR FIRST GAME IN FIVE MINUTES
    1. Create New Project -> pick "Third-Person Starter" -> name it -> Create Project.
       (The project opens in the editor with a ground, a character and crates.)
    2. The "Start Here" card in the viewport lists the same path:
       1) New scene   2) Add ground   3) Play.  Close it when you know the way.
    3. Shape the scene: drag the red/green/blue arrows (W/E/R move/rotate/scale),
       add cubes/ground from the toolbar, or drag a mesh from the Assets browser.
    4. Press the arrow at the far right of the toolbar (Play). The game opens in
       its own window: WASD or arrow keys to move, Space to jump. Close that
       window to stop. The editor keeps running.
    5. Ctrl+S saves. Closing a scene with unsaved changes asks
       Save / Discard / Cancel, and always leaves cache://EditorRecovery.nfscene.

MAKE A STANDALONE BUILD
    nf new MyGame --template ThirdPerson
    nf build --project MyGame\MyGame.nfproj
    MyGame\dist\NFPlayer.exe
    (nf.exe, NFAssetCooker.exe and NFModelImporter.exe ship beside the editor.)

TEMPLATES
    Default, ThirdPerson, FPSStarter, Platformer2D. They live in Templates\
    next to the editor and are copied into each new project.

WHAT IT NEEDS FROM YOUR MACHINE
    * Windows 10 or newer, 64-bit. This build is Windows-only.
    * A Vulkan-capable GPU driver. The loader (vulkan-1.dll) ships with the
      driver, NOT with this package, and is deliberately not bundled. The
      editor tells you plainly at startup if it is missing.
    * Nothing else. The MSVC runtime is linked statically.

DOCS
    Docs\Getting_Started_Ar.md   - من وين أبدأ (Arabic)
    Docs\Tutorial_Ar.md          - 20-minute tutorial (Arabic)
    Docs\Lua_Scripting_Ar.md     - scripting reference (Arabic)
    Docs\Asset_Import.md         - models, materials, textures
    README-repo.md, ROADMAP.md   - the repository docs

WHERE THE VERSION NUMBER COMES FROM
    One place: project(VERSION) and NF_VERSION_SUFFIX in CMakeLists.txt.

REPORTING A PROBLEM
    Include the console output (the editor logs every Vulkan/asset error) and
    the exact step that failed.
"@
$readme | Set-Content -Path (Join-Path $stage 'README.txt') -Encoding UTF8
Write-Host '  README.txt + SANAD.png'

$total = (Get-ChildItem $stage -Recurse -File | Measure-Object -Property Length -Sum)
Write-Host ("staged {0} files, {1:N1} MB -> {2}" -f $total.Count, ($total.Sum / 1MB), $stage)

if ($StageOnly) { return }

# --- Portable zip -------------------------------------------------------------
if (-not $SkipPortable) {
    $zipPath = Join-Path $dist $zipName
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zipPath -CompressionLevel Optimal
    Write-Host ("portable: {0} ({1:N1} MB)" -f $zipPath, ((Get-Item $zipPath).Length / 1MB))
}

# --- Setup EXE via Inno Setup --------------------------------------------------
if (-not $SkipInstaller) {
    $iscc = $null
    $candidates = @(
        'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        'C:\Program Files\Inno Setup 6\ISCC.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
    )
    foreach ($p in $candidates) { if ($p -and (Test-Path $p)) { $iscc = $p; break } }
    if (-not $iscc) {
        $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Source }
    }
    if (-not $iscc) {
        Write-Host 'Inno Setup not found - installing via winget (JRSoftware.InnoSetup)...'
        winget install --id JRSoftware.InnoSetup -e --silent --accept-package-agreements --accept-source-agreements
        foreach ($p in $candidates) { if ($p -and (Test-Path $p)) { $iscc = $p; break } }
    }
    if (-not $iscc) { throw 'ISCC.exe still not found after winget install. Install Inno Setup 6 manually and re-run.' }
    Write-Host "Inno: $iscc"
    $iss = Join-Path $PSScriptRoot 'SANAD.iss'
    & $iscc $iss "/DAppVersion=$version" "/DVersionInfo=$versionInfo" "/DStageDir=$stage" "/DOutputDir=$dist" "/DRepoDir=$repo"
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
    $setupPath = Join-Path $dist $setupName
    if (-not (Test-Path $setupPath)) { throw "Installer was not produced: $setupPath" }
    Write-Host ("installer: {0} ({1:N1} MB)" -f $setupPath, ((Get-Item $setupPath).Length / 1MB))
}

Write-Host ''
Write-Host 'DONE. Upload these two files to the GitHub release:'
Write-Host ("  - $dist\$setupName  (installer, Windows 10+ x64)")
Write-Host ("  - $dist\$zipName  (portable, unzip and run)")
